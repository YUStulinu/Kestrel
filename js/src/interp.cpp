// The bytecode interpreter: one C++ call per JavaScript call, frames on the
// heap so generators and async functions can suspend and resume.
#include "interp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kjs {

void Frame::trace(Runtime& rt) {
    rt.mark(code);
    rt.mark(callee);
    rt.mark(env);
    for (auto& v : stack) rt.mark(v);
    for (auto& v : args) rt.mark(v);
    for (auto& h : handlers) rt.mark(h.env);
    rt.mark(completion);
    rt.mark(this_value);
    rt.mark(new_target);
    rt.mark(generator);
}
void IterRecord::trace(Runtime& rt) {
    Obj::trace(rt);
    rt.mark(iterator);
    rt.mark(next);
}
void Generator::trace(Runtime& rt) {
    Obj::trace(rt);
    rt.mark(frame);
    for (auto& r : queue) {
        rt.mark(r.value);
        rt.mark(r.promise);
        rt.mark(r.resolve);
        rt.mark(r.reject);
    }
}
void ClassData::trace(Runtime& rt) {
    for (auto* l : {&fields, &static_fields})
        for (auto& f : *l) {
            rt.mark(f.key);
            rt.mark(f.priv);
            rt.mark(f.init);
        }
    for (auto* p : methods) rt.mark(p);
}

Env* new_env(Runtime& rt, Env* parent) {
    Env* e = rt.alloc<Env>();
    e->parent = parent;
    return e;
}

Binding* find_binding(Env* e, Str* name) {
    auto it = e->vars.find(name);
    return it == e->vars.end() ? nullptr : &it->second;
}

ClassData* class_data(Runtime& rt, Function* ctor, bool create) {
    for (auto& v : ctor->slots)
        if (v.tag == Tag::Object && v.o == nullptr) {}
    if (!ctor->slots.empty() && ctor->slots[0].tag == Tag::Empty && ctor->slots[0].c) return (ClassData*)ctor->slots[0].c;
    if (!create) return nullptr;
    ClassData* d = rt.alloc<ClassData>();
    Value v = Value::empty();
    v.c = d;
    ctor->slots.insert(ctor->slots.begin(), v);
    return d;
}

// ------------------------------------------------------------------ names
static Env* var_env(Env* e) {
    while (e && !e->var_scope) e = e->parent;
    return e;
}
static Env* function_env(Env* e) {
    while (e && !(e->has_this)) e = e->parent;
    return e;
}

struct Ref {
    Env* env = nullptr;
    Binding* b = nullptr;
    Obj* obj = nullptr;  // object environment holding the name
    bool with = false;
};

static bool unscopable(Runtime& rt, Obj* o, Key k) {
    Value u = rt.get(Value(o), rt.key(rt.sym_unscopables));
    if (!u.is_obj()) return false;
    return rt.to_boolean(rt.get(u, k));
}

static bool resolve(Runtime& rt, Env* e, Str* name, Ref& r) {
    Key k{name, false};
    for (; e; e = e->parent) {
        if (Binding* b = find_binding(e, name)) {
            r.env = e;
            r.b = b;
            return true;
        }
        if (e->object) {
            if (rt.has_property(e->object, k)) {
                if (e->with_env && unscopable(rt, e->object, k)) continue;
                r.env = e;
                r.obj = e->object;
                r.with = e->with_env;
                return true;
            }
        }
    }
    return false;
}

[[noreturn]] static void tdz(Runtime& rt, Str* name) {
    rt.throw_error(E_REFERENCE, "Cannot access '" + to_utf8(name->v) + "' before initialization");
}

static Value get_var(Runtime& rt, Env* env, Str* name, bool typeof_) {
    Ref r;
    if (!resolve(rt, env, name, r)) {
        if (typeof_) return Value();
        rt.throw_error(E_REFERENCE, to_utf8(name->v) + " is not defined");
    }
    if (r.b) {
        if (!r.b->initialized) tdz(rt, name);
        return r.b->v;
    }
    Key k{name, false};
    // the binding may have been deleted meanwhile (strict mode reference error)
    if (!r.with && !rt.has_property(r.obj, k)) {
        if (typeof_) return Value();
        rt.throw_error(E_REFERENCE, to_utf8(name->v) + " is not defined");
    }
    return rt.get(r.obj, k, Value(r.obj));
}

static void set_var(Runtime& rt, Env* env, Str* name, Value v, bool strict) {
    Ref r;
    Key k{name, false};
    if (!resolve(rt, env, name, r)) {
        if (strict) rt.throw_error(E_REFERENCE, to_utf8(name->v) + " is not defined");
        rt.set(rt.realm.global, k, v, Value(rt.realm.global));
        return;
    }
    if (r.b) {
        if (!r.b->initialized) tdz(rt, name);
        if (!r.b->mutable_) {
            if (strict || r.b->strict_const) rt.throw_error(E_TYPE, "Assignment to constant variable '" + to_utf8(name->v) + "'");
            return;
        }
        r.b->v = v;
        return;
    }
    // object environment: the property may have disappeared
    if (strict && !r.with && !rt.has_property(r.obj, k)) rt.throw_error(E_REFERENCE, to_utf8(name->v) + " is not defined");
    if (!rt.set(r.obj, k, v, Value(r.obj)) && strict) rt.throw_error(E_TYPE, "Cannot assign to read only property '" + to_utf8(name->v) + "'");
}

// ------------------------------------------------------------------ closures
Function* make_closure(Runtime& rt, Code* c, Env* env) {
    Function* fn = rt.alloc<Function>();
    fn->kind = Kind::Function;
    fn->code = c;
    Realm& R = rt.realm;
    switch (c->kind) {
        case FnKind::Generator: fn->proto = R.generator_function_proto; break;
        case FnKind::Async: case FnKind::AsyncArrow: fn->proto = R.async_function_proto; break;
        case FnKind::AsyncGenerator: fn->proto = R.async_generator_function_proto; break;
        default: fn->proto = R.function_proto;
    }
    if (c->has_name_binding) {
        // a named function expression sees its own name in an intermediate scope
        Env* e = new_env(rt, env);
        Binding b;
        b.v = Value(fn);
        b.mutable_ = false;
        b.strict_const = false;
        e->vars[rt.intern(c->name)] = b;
        env = e;
    }
    fn->env = env;
    rt.define_data(fn, "length", Value(double(c->nparams)), CONFIGURABLE);
    rt.define_data(fn, "name", Value(rt.intern(c->name)), CONFIGURABLE);
    if (c->kind == FnKind::Generator || c->kind == FnKind::AsyncGenerator) {
        Obj* proto = rt.new_object(c->kind == FnKind::Generator ? R.generator_proto : R.async_generator_proto);
        rt.define_data(fn, "prototype", Value(proto), WRITABLE);
    } else if (c->kind == FnKind::Normal) {
        make_constructor(rt, fn);
    }
    return fn;
}

void make_constructor(Runtime& rt, Function* fn) {
    Obj* proto = rt.new_object(rt.realm.object_proto);
    rt.define_data(proto, "constructor", Value(fn), WRITABLE | CONFIGURABLE);
    rt.define_data(fn, "prototype", Value(proto), WRITABLE);
}

void set_function_name(Runtime& rt, Obj* fn, Value key, const char* prefix) {
    u16 name;
    if (key.is_sym()) {
        if (key.y->description.is_undef()) name = u"";
        else name = u"[" + key.y->description.s->v + u"]";
    } else if (key.tag == Tag::Empty && key.c) {
        // a private name
        name = u"#" + ((PrivateName*)key.c)->description.s->v;
    } else {
        name = rt.str_of(key);
    }
    if (prefix) name = to_u16(prefix) + u" " + name;
    Desc d;
    d.has_value = true;
    d.value = Value(rt.str(name));
    d.has_writable = d.has_enumerable = d.has_configurable = true;
    d.configurable = true;
    rt.define(fn, rt.key("name"), d);
}

// ------------------------------------------------------------------ private names
static int find_private(Obj* o, PrivateName* pn) {
    for (size_t i = 0; i < o->privates.size(); i++)
        if (o->privates[i].first == pn) return int(i);
    return -1;
}
void private_add(Runtime& rt, Obj* o, PrivateName* pn, Value v) {
    if (find_private(o, pn) >= 0) rt.throw_error(E_TYPE, "Cannot initialize private field twice on the same object");
    Prop p;
    p.value = v;
    p.flags = WRITABLE;
    o->privates.push_back({pn, p});
}
Value private_get(Runtime& rt, Value obj, PrivateName* pn) {
    if (!obj.is_obj()) rt.throw_error(E_TYPE, "Cannot read private member from a non-object");
    int i = find_private(obj.o, pn);
    if (i < 0) rt.throw_error(E_TYPE, "Cannot read private member #" + to_utf8(pn->description.s->v) + " from an object whose class did not declare it");
    if (pn->is_method) return pn->method;
    if (pn->is_accessor) {
        if (pn->getter.is_undef()) rt.throw_error(E_TYPE, "'#" + to_utf8(pn->description.s->v) + "' was defined without a getter");
        return rt.call(pn->getter, obj, nullptr, 0);
    }
    return obj.o->privates[i].second.value;
}
void private_set(Runtime& rt, Value obj, PrivateName* pn, Value v) {
    if (!obj.is_obj()) rt.throw_error(E_TYPE, "Cannot write private member to a non-object");
    int i = find_private(obj.o, pn);
    if (i < 0) rt.throw_error(E_TYPE, "Cannot write private member #" + to_utf8(pn->description.s->v) + " to an object whose class did not declare it");
    if (pn->is_method) rt.throw_error(E_TYPE, "Private method is not writable");
    if (pn->is_accessor) {
        if (pn->setter.is_undef()) rt.throw_error(E_TYPE, "'#" + to_utf8(pn->description.s->v) + "' was defined without a setter");
        Value a[1] = {v};
        rt.call(pn->setter, obj, a, 1);
        return;
    }
    obj.o->privates[i].second.value = v;
}

void initialize_instance(Runtime& rt, Obj* obj, Function* ctor) {
    ClassData* cd = class_data(rt, ctor, false);
    if (!cd) return;
    for (PrivateName* pn : cd->methods) private_add(rt, obj, pn, Value());
    for (auto& fr : cd->fields) {
        Value v;
        if (!fr.init.is_undef()) v = rt.call(fr.init, Value(obj), nullptr, 0);
        if (fr.priv) {
            private_add(rt, obj, fr.priv, v);
        } else {
            Desc d;
            d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
            d.value = v;
            d.writable = d.enumerable = d.configurable = true;
            rt.define(obj, rt.to_key(fr.key), d, true);
        }
    }
}

// ------------------------------------------------------------------ template objects
Obj* template_object(Runtime& rt, Code* c, int idx) {
    if (c->templates[idx]) return c->templates[idx];
    std::vector<Value> cooked, raw;
    for (auto& q : c->template_strs[idx]) {
        cooked.push_back(q.first);
        raw.push_back(q.second);
    }
    Obj* a = rt.array_from_list(cooked);
    Obj* r = rt.array_from_list(raw);
    // frozen
    for (Obj* o : {r, a}) {
        for (uint32_t k = 0; k < o->length; k++) {
            Desc d;
            d.has_writable = d.has_configurable = true;
            rt.define(o, rt.to_key(Value(double(k))), d);
        }
        Desc d;
        d.has_writable = true;
        rt.define(o, rt.key("length"), d);
        o->extensible = false;
    }
    rt.define_data(a, "raw", Value(r), 0);
    c->templates[idx] = a;
    return a;
}

// ------------------------------------------------------------------ arguments
Obj* make_arguments(Runtime& rt, Frame* f, bool mapped) {
    Obj* a = rt.alloc<Obj>();
    a->kind = Kind::Arguments;
    a->proto = rt.realm.object_proto;
    for (size_t i = 0; i < f->args.size(); i++) rt.define_data(a, rt.to_key(Value(double(i))), f->args[i], WRITABLE | ENUMERABLE | CONFIGURABLE);
    rt.define_data(a, "length", Value(double(f->args.size())), WRITABLE | CONFIGURABLE);
    rt.define_data(a, rt.key(rt.sym_iterator), rt.get(Value(rt.realm.array_proto), "values"), WRITABLE | CONFIGURABLE);
    if (mapped) {
        rt.define_data(a, "callee", Value(f->callee), WRITABLE | CONFIGURABLE);
        // map indices to parameter bindings: elems[i] holds the parameter name (as a string) or Empty
        Node* fnode = nullptr;
        (void)fnode;
        a->prim = Value(f->env ? (Obj*)nullptr : nullptr);
        // the parameter names come from the code's first DECL_VAR ops
        std::vector<Str*> params;
        Code* c = f->code;
        for (size_t k = 0; k < c->ops.size() && params.size() < size_t(1000); k++) {
            if (c->ops[k] == OP_DECL_VAR) params.push_back(c->names[c->args[k]]);
            else break;
        }
        int nformal = 0;
        // the formal parameter count is the number of declared parameter names (simple lists only)
        nformal = int(params.size());
        a->elems.assign(std::min<size_t>(f->args.size(), nformal), Value::empty());
        for (int i = 0; i < int(a->elems.size()); i++) {
            // a later parameter with the same name wins
            bool shadowed = false;
            for (int j = i + 1; j < nformal; j++)
                if (params[j] == params[i]) shadowed = true;
            if (!shadowed) a->elems[i] = Value(params[i]);
        }
        Value envv;
        envv.tag = Tag::Empty;
        envv.c = f->env;
        a->prim = envv;  // the function environment
    } else {
        Desc d;
        d.has_get = d.has_set = true;
        d.get = d.set = Value(rt.realm.throw_type_error);
        d.has_enumerable = d.has_configurable = true;
        rt.define(a, rt.key("callee"), d);
    }
    return a;
}

// ------------------------------------------------------------------ calls
static Value bind_this(Runtime& rt, Code* c, Value thisv) {
    if (c->strict) return thisv;
    if (thisv.is_nullish()) return Value(rt.realm.global);
    if (!thisv.is_obj()) return Value(rt.to_object(thisv));
    return thisv;
}

static Frame* new_frame(Runtime& rt, Function* fn, Value thisv, const Value* args, int argc, Value new_target) {
    Code* c = fn->code;
    Frame* f = rt.alloc<Frame>();
    f->code = c;
    f->callee = fn;
    f->args.assign(args, args + argc);
    Env* e = new_env(rt, fn->env);
    e->var_scope = true;
    e->function = fn;
    if (!c->is_arrow) {
        e->has_this = true;
        e->home_object = fn->home_object;
        e->new_target = new_target;
        if (c->kind == FnKind::DerivedConstructor) {
            e->this_initialized = false;
        } else {
            e->this_value = bind_this(rt, c, thisv);
        }
    }
    f->env = e;
    f->new_target = new_target;
    return f;
}

struct FrameGuard {
    Runtime& rt;
    FrameGuard(Runtime& r, Frame* f) : rt(r) {
        rt.frames.push_back(f);
        if (++rt.depth > 3000) {
            rt.depth--;
            rt.frames.pop_back();
            rt.throw_error(E_RANGE, "Maximum call stack size exceeded");
        }
    }
    ~FrameGuard() {
        rt.depth--;
        rt.frames.pop_back();
    }
};

Value call_function(Runtime& rt, Function* fn, Value thisv, const Value* args, int argc, Value new_target) {
    Code* c = fn->code;
    if (fn->is_class_constructor && new_target.is_undef())
        rt.throw_error(E_TYPE, "Class constructor " + to_utf8(c->name) + " cannot be invoked without 'new'");
    Frame* f = new_frame(rt, fn, thisv, args, argc, new_target);
    switch (c->kind) {
        case FnKind::Generator: case FnKind::AsyncGenerator: {
            Generator* g = rt.alloc<Generator>();
            g->kind = c->kind == FnKind::Generator ? Kind::Generator : Kind::AsyncGenerator;
            Value p = rt.get(Value(fn), "prototype");
            g->proto = p.is_obj() ? p.o : (c->kind == FnKind::Generator ? rt.realm.generator_proto : rt.realm.async_generator_proto);
            Runtime::Root root(rt, Value(g));
            g->frame = f;
            f->generator = g;
            // run the prologue (parameter binding) up to the initial yield
            {
                FrameGuard fg(rt, f);
                execute(rt, f, Resume{});
            }
            g->state = Generator::SuspendedStart;
            return Value(g);
        }
        case FnKind::Async: case FnKind::AsyncArrow: return start_async_function(rt, f);
        default: break;
    }
    Value result;
    {
        FrameGuard fg(rt, f);
        if (c->kind == FnKind::ClassConstructor && !new_target.is_undef()) {
            // base class: fields are initialized before the body runs
            initialize_instance(rt, f->env->this_value.o, fn);
        }
        Result r = execute(rt, f, Resume{});
        result = r.v;
    }
    if (c->kind == FnKind::DerivedConstructor) {
        if (result.is_obj()) return result;
        if (!result.is_undef()) rt.throw_error(E_TYPE, "Derived constructors may only return object or undefined");
        if (!f->env->this_initialized) rt.throw_error(E_REFERENCE, "Must call super constructor before returning from derived constructor");
        return f->env->this_value;
    }
    return result;
}

// ------------------------------------------------------------------ the loop
static Value pop(Frame* f) {
    Value v = f->stack.back();
    f->stack.pop_back();
    return v;
}
#define PUSH(x) f->stack.push_back(x)
#define TOP() f->stack.back()
#define AT(n) f->stack[f->stack.size() - 1 - (n)]

static Value this_of(Runtime& rt, Env* env) {
    Env* e = function_env(env);
    if (!e) return Value(rt.realm.global);
    if (!e->this_initialized) rt.throw_error(E_REFERENCE, "Must call super constructor before accessing 'this'");
    return e->this_value;
}

static Obj* home_of(Env* env) {
    Env* e = function_env(env);
    return e ? e->home_object : nullptr;
}

static Value super_base(Runtime& rt, Env* env) {
    Obj* home = home_of(env);
    if (!home) rt.throw_error(E_SYNTAX, "'super' keyword unexpected here");
    Obj* p = rt.get_proto(home);
    return p ? Value(p) : Value::null();
}

static double num(Runtime& rt, Value v) { return v.is_num() ? v.n : rt.to_number(v); }

static Value binary_num(Runtime& rt, int op, Value a, Value b) {
    Value na = rt.to_numeric(a), nb = rt.to_numeric(b);
    double x = na.n, y = nb.n;
    switch (op) {
        case OP_SUB: return Value(x - y);
        case OP_MUL: return Value(x * y);
        case OP_DIV: return Value(x / y);
        case OP_MOD: {
            if (std::isinf(y) && std::isfinite(x)) return Value(x);
            return Value(std::fmod(x, y));
        }
        case OP_EXP: {
            if (std::isnan(y)) return Value(NAN);
            if ((x == 1 || x == -1) && std::isinf(y)) return Value(NAN);
            return Value(std::pow(x, y));
        }
        case OP_SHL: return Value(double(int32_t(uint32_t(rt.to_int32(na)) << (rt.to_uint32(nb) & 31))));
        case OP_SHR: return Value(double(rt.to_int32(na) >> (rt.to_uint32(nb) & 31)));
        case OP_USHR: return Value(double(rt.to_uint32(na) >> (rt.to_uint32(nb) & 31)));
        case OP_BAND: return Value(double(rt.to_int32(na) & rt.to_int32(nb)));
        case OP_BOR: return Value(double(rt.to_int32(na) | rt.to_int32(nb)));
        case OP_BXOR: return Value(double(rt.to_int32(na) ^ rt.to_int32(nb)));
    }
    return Value();
}

// abstract relational comparison: returns 0 false, 1 true, 2 undefined (NaN)
static int less_than(Runtime& rt, Value a, Value b, bool left_first) {
    Value pa, pb;
    if (left_first) {
        pa = rt.to_primitive(a, 1);
        pb = rt.to_primitive(b, 1);
    } else {
        pb = rt.to_primitive(b, 1);
        pa = rt.to_primitive(a, 1);
    }
    if (pa.is_str() && pb.is_str()) return pa.s->v < pb.s->v ? 1 : 0;
    double x = rt.to_number(pa), y = rt.to_number(pb);
    if (std::isnan(x) || std::isnan(y)) return 2;
    return x < y ? 1 : 0;
}

static Value do_call(Runtime& rt, Value fn, Value thisv, const Value* args, int argc) {
    if (fn.is_obj() && fn.o->kind == Kind::Function) return call_function(rt, (Function*)fn.o, thisv, args, argc, Value());
    // natives: their C++ locals are not GC roots
    rt.unsafe_depth++;
    struct D {
        Runtime& r;
        ~D() { r.unsafe_depth--; }
    } d{rt};
    return rt.call(fn, thisv, args, argc);
}

static Value do_construct(Runtime& rt, Value fn, const Value* args, int argc, Value nt) {
    std::vector<Value> v(args, args + argc);
    rt.unsafe_depth++;
    struct D {
        Runtime& r;
        ~D() { r.unsafe_depth--; }
    } d{rt};
    return rt.construct(fn, v, nt);
}

Value direct_eval_call(Runtime& rt, Frame* f, const Value* args, int argc, bool strict);

Result execute(Runtime& rt, Frame* f, Resume resume) {
    Code* c = f->code;
    bool strict = c->strict;
    bool pending_throw = false;
    Value thrown;
    switch (resume.k) {
        case Resume::None: break;
        case Resume::Throw:
            if (f->suspended == Suspend::Yield || f->suspended == Suspend::YieldStar) {
                PUSH(resume.v);
                PUSH(Value(1.0));
            } else {
                pending_throw = true;
                thrown = resume.v;
            }
            break;
        case Resume::Next:
            PUSH(resume.v);
            if (f->suspended == Suspend::Yield || f->suspended == Suspend::YieldStar) PUSH(Value(0.0));
            break;
        case Resume::Return:
            PUSH(resume.v);
            PUSH(Value(2.0));
            break;
    }
    f->suspended = Suspend::None;
    while (true) {
        try {
            if (pending_throw) {
                pending_throw = false;
                throw Throw{thrown};
            }
            while (true) {
                uint8_t op = c->ops[f->pc];
                int32_t arg = c->args[f->pc];
                f->pc++;
                switch (op) {
                    case OP_UNDEF: PUSH(Value()); break;
                    case OP_NULL: PUSH(Value::null()); break;
                    case OP_TRUE: PUSH(Value(true)); break;
                    case OP_FALSE: PUSH(Value(false)); break;
                    case OP_NUM: case OP_STR: PUSH(c->consts[arg]); break;
                    case OP_EMPTY: PUSH(Value::empty()); break;
                    case OP_BIGINT: rt.throw_error(E_SYNTAX, "BigInt is not supported");
                    case OP_POP: f->stack.pop_back(); break;
                    case OP_DUP: PUSH(TOP()); break;
                    case OP_DUP2: {
                        Value a = AT(1), b = AT(0);
                        PUSH(a);
                        PUSH(b);
                        break;
                    }
                    case OP_SWAP: std::swap(AT(0), AT(1)); break;
                    case OP_ROT3: {  // a b c -> c a b
                        Value cc = AT(0), b = AT(1), a = AT(2);
                        AT(2) = cc;
                        AT(1) = a;
                        AT(0) = b;
                        break;
                    }
                    case OP_ROTL3: {  // a b c -> b c a
                        Value cc = AT(0), b = AT(1), a = AT(2);
                        AT(2) = b;
                        AT(1) = cc;
                        AT(0) = a;
                        break;
                    }
                    case OP_ROT4: {  // a b c d -> d a b c
                        Value d = AT(0), cc = AT(1), b = AT(2), a = AT(3);
                        AT(3) = d;
                        AT(2) = a;
                        AT(1) = b;
                        AT(0) = cc;
                        break;
                    }
                    case OP_PICK: PUSH(AT(arg)); break;
                    case OP_TRUNCATE: f->stack.resize(size_t(arg)); break;

                    case OP_GET_VAR: PUSH(get_var(rt, f->env, c->names[arg], false)); break;
                    case OP_GET_VAR_TYPEOF: PUSH(get_var(rt, f->env, c->names[arg], true)); break;
                    case OP_GET_VAR_CALL: {
                        Ref r;
                        Str* name = c->names[arg];
                        if (!resolve(rt, f->env, name, r)) rt.throw_error(E_REFERENCE, to_utf8(name->v) + " is not defined");
                        if (r.b) {
                            if (!r.b->initialized) tdz(rt, name);
                            PUSH(r.b->v);
                            PUSH(Value());
                        } else {
                            Key k{name, false};
                            if (!r.with && !rt.has_property(r.obj, k)) rt.throw_error(E_REFERENCE, to_utf8(name->v) + " is not defined");
                            PUSH(rt.get(r.obj, k, Value(r.obj)));
                            PUSH(r.with ? Value(r.obj) : Value());
                        }
                        break;
                    }
                    case OP_SET_VAR: set_var(rt, f->env, c->names[arg], TOP(), strict); break;
                    case OP_INIT_VAR: {
                        Value v = pop(f);
                        Str* name = c->names[arg];
                        Env* e = f->env;
                        Binding* b = nullptr;
                        for (; e; e = e->parent)
                            if ((b = find_binding(e, name))) break;
                        if (!b) rt.throw_error(E_REFERENCE, "internal: no binding for " + to_utf8(name->v));
                        b->v = v;
                        b->initialized = true;
                        break;
                    }
                    case OP_INIT_VAR_FN: {
                        Value v = pop(f);
                        Str* name = c->names[arg];
                        Env* e = var_env(f->env);
                        if (e->object && !find_binding(e, name)) {
                            rt.set(e->object, Key{name, false}, v, Value(e->object));
                        } else {
                            Binding& b = e->vars[name];
                            b.v = v;
                            b.initialized = true;
                        }
                        break;
                    }
                    case OP_DELETE_VAR: {
                        Ref r;
                        Str* name = c->names[arg];
                        if (!resolve(rt, f->env, name, r)) PUSH(Value(true));
                        else if (r.b) {
                            if (r.b->deletable) {
                                r.env->vars.erase(name);
                                PUSH(Value(true));
                            } else {
                                PUSH(Value(false));
                            }
                        } else {
                            PUSH(Value(rt.delete_property(r.obj, Key{name, false})));
                        }
                        break;
                    }
                    case OP_DECL_VAR: {
                        Str* name = c->names[arg];
                        Env* e = var_env(f->env);
                        if (e->object) {
                            // global or eval-in-global var
                            Key k{name, false};
                            if (!rt.has_own(e->object, k)) {
                                Desc d;
                                d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
                                d.writable = d.enumerable = true;
                                d.configurable = c->is_eval;
                                rt.define(e->object, k, d, true);
                            }
                        } else if (!find_binding(e, name)) {
                            Binding b;
                            b.deletable = c->is_eval;
                            e->vars[name] = b;
                        }
                        break;
                    }
                    case OP_DECL_LEX: case OP_DECL_CONST: {
                        Binding b;
                        b.initialized = false;
                        b.mutable_ = op == OP_DECL_LEX;
                        f->env->vars[c->names[arg]] = b;
                        break;
                    }
                    case OP_DECL_FN_VAR: {
                        // a top-level function of a script or eval
                        Value v = pop(f);
                        Str* name = c->names[arg];
                        Env* e = var_env(f->env);
                        Key k{name, false};
                        if (e->object) {
                            Desc existing;
                            bool has = rt.get_own(e->object, k, existing);
                            Desc d;
                            d.has_value = true;
                            d.value = v;
                            if (!has || existing.configurable) {
                                d.has_writable = d.has_enumerable = d.has_configurable = true;
                                d.writable = d.enumerable = true;
                                d.configurable = c->is_eval;
                            }
                            rt.define(e->object, k, d, true);
                            if (has && !existing.configurable) rt.set(e->object, k, v, Value(e->object));
                        } else {
                            Binding& b = e->vars[name];
                            b.v = v;
                            b.initialized = true;
                            if (c->is_eval) b.deletable = true;
                        }
                        break;
                    }
                    case OP_PUSH_SCOPE: {
                        Env* e = new_env(rt, f->env);
                        int idx = arg;
                        if (idx < 0) {
                            e->var_scope = true;
                            idx = -idx - 1;
                        }
                        for (auto& n : c->scopes[idx].names) {
                            Binding b;
                            b.initialized = false;
                            b.mutable_ = n.second != 1;
                            e->vars[n.first] = b;
                        }
                        f->env = e;
                        break;
                    }
                    case OP_POP_SCOPE: f->env = f->env->parent; break;
                    case OP_COPY_SCOPE: {
                        Env* e = new_env(rt, f->env->parent);
                        e->vars = f->env->vars;
                        f->env = e;
                        break;
                    }
                    case OP_PUSH_WITH: {
                        Value v = pop(f);
                        Obj* o = rt.to_object(v);
                        Env* e = new_env(rt, f->env);
                        e->object = o;
                        e->with_env = true;
                        f->env = e;
                        break;
                    }
                    case OP_GLOBAL_DECLS: case OP_EVAL_DECLS: {
                        auto& decls = c->decl_lists[arg];
                        Env* ve = var_env(f->env);
                        if (op == OP_GLOBAL_DECLS) {
                            Env* g = rt.realm.global_env;
                            Obj* go = rt.realm.global;
                            // checks first
                            for (auto& d : decls) {
                                Key k{d.first, false};
                                if (d.second >= 2 && d.second <= 4) {
                                    if (find_binding(g, d.first)) rt.throw_error(E_SYNTAX, "Identifier '" + to_utf8(d.first->v) + "' has already been declared");
                                    Desc ex;
                                    if (rt.get_own(go, k, ex) && !ex.configurable) rt.throw_error(E_SYNTAX, "Identifier '" + to_utf8(d.first->v) + "' has already been declared");
                                } else {
                                    if (find_binding(g, d.first)) rt.throw_error(E_SYNTAX, "Identifier '" + to_utf8(d.first->v) + "' has already been declared");
                                }
                            }
                            for (auto& d : decls) {
                                Key k{d.first, false};
                                if (d.second == 1) {
                                    Desc ex;
                                    bool has = rt.get_own(go, k, ex);
                                    if (has && !ex.configurable && !(ex.is_data() && ex.writable && ex.enumerable))
                                        rt.throw_error(E_TYPE, "Cannot redefine global function '" + to_utf8(d.first->v) + "'");
                                    if (!has && !rt.is_extensible(go)) rt.throw_error(E_TYPE, "Cannot define global function '" + to_utf8(d.first->v) + "'");
                                } else if (d.second == 0 || d.second == 5) {
                                    if (!rt.has_own(go, k) && !rt.is_extensible(go)) rt.throw_error(E_TYPE, "Cannot define global variable '" + to_utf8(d.first->v) + "'");
                                }
                            }
                            for (auto& d : decls) {
                                Key k{d.first, false};
                                if (d.second >= 2 && d.second <= 4) {
                                    Binding b;
                                    b.initialized = false;
                                    b.mutable_ = d.second != 3;
                                    g->vars[d.first] = b;
                                } else if (d.second == 0 || d.second == 5) {
                                    if (!rt.has_own(go, k)) {
                                        Desc dd;
                                        dd.has_value = dd.has_writable = dd.has_enumerable = dd.has_configurable = true;
                                        dd.writable = dd.enumerable = true;
                                        rt.define(go, k, dd, true);
                                    }
                                }
                            }
                        } else {
                            // eval: vars may not collide with lexical bindings between here and the var scope
                            if (!c->strict) {
                                for (auto& d : decls) {
                                    if (d.second >= 2 && d.second <= 4) continue;
                                    for (Env* e = f->env->parent; e && e != ve; e = e->parent) {
                                        if (!e->with_env && find_binding(e, d.first))
                                            rt.throw_error(E_SYNTAX, "Identifier '" + to_utf8(d.first->v) + "' has already been declared");
                                    }
                                    if (ve == rt.realm.global_env && find_binding(ve, d.first))
                                        rt.throw_error(E_SYNTAX, "Identifier '" + to_utf8(d.first->v) + "' has already been declared");
                                    if (ve->object && ve == rt.realm.global_env && d.second == 1) {
                                        Desc ex;
                                        Key k{d.first, false};
                                        bool has = rt.get_own(ve->object, k, ex);
                                        if (has && !ex.configurable && !(ex.is_data() && ex.writable && ex.enumerable))
                                            rt.throw_error(E_TYPE, "Cannot redefine global function '" + to_utf8(d.first->v) + "'");
                                        if (!has && !rt.is_extensible(ve->object)) rt.throw_error(E_TYPE, "Cannot define global function");
                                    }
                                }
                            }
                            for (auto& d : decls) {
                                if (d.second >= 2 && d.second <= 4) {
                                    Binding b;
                                    b.initialized = false;
                                    b.mutable_ = d.second != 3;
                                    f->env->vars[d.first] = b;
                                } else if (d.second == 0 || d.second == 5) {
                                    if (ve->object) {
                                        Key k{d.first, false};
                                        if (!rt.has_own(ve->object, k)) {
                                            if (!rt.is_extensible(ve->object)) rt.throw_error(E_TYPE, "Cannot define global variable");
                                            Desc dd;
                                            dd.has_value = dd.has_writable = dd.has_enumerable = dd.has_configurable = true;
                                            dd.writable = dd.enumerable = dd.configurable = true;
                                            rt.define(ve->object, k, dd, true);
                                        }
                                    } else if (!find_binding(ve, d.first)) {
                                        Binding b;
                                        b.deletable = true;
                                        ve->vars[d.first] = b;
                                    }
                                }
                            }
                        }
                        break;
                    }

                    case OP_GET_PROP: {
                        Value o = TOP();
                        if (o.is_nullish())
                            rt.throw_error(E_TYPE, "Cannot read properties of " + std::string(o.is_null() ? "null" : "undefined") +
                                                       " (reading '" + to_utf8(c->names[arg]->v) + "')");
                        TOP() = rt.get(o, Key{c->names[arg], false});
                        break;
                    }
                    case OP_GET_ELEM: {
                        Value k = AT(0), o = AT(1);
                        if (o.is_nullish()) rt.throw_error(E_TYPE, "Cannot read properties of " + std::string(o.is_null() ? "null" : "undefined"));
                        Key key = rt.to_key(k);
                        f->stack.pop_back();
                        TOP() = rt.get(o, key);
                        break;
                    }
                    case OP_SET_PROP: {
                        Value v = AT(0), o = AT(1);
                        if (o.is_nullish())
                            rt.throw_error(E_TYPE, "Cannot set properties of " + std::string(o.is_null() ? "null" : "undefined") +
                                                       " (setting '" + to_utf8(c->names[arg]->v) + "')");
                        if (!rt.set(o, Key{c->names[arg], false}, v, strict) && strict)
                            rt.throw_error(E_TYPE, "Cannot assign to read only property '" + to_utf8(c->names[arg]->v) + "'");
                        f->stack.pop_back();
                        TOP() = v;
                        break;
                    }
                    case OP_SET_ELEM: {
                        Value v = AT(0), k = AT(1), o = AT(2);
                        if (o.is_nullish()) rt.throw_error(E_TYPE, "Cannot set properties of " + std::string(o.is_null() ? "null" : "undefined"));
                        Key key = rt.to_key(k);
                        if (!rt.set(o, key, v, strict) && strict) rt.throw_error(E_TYPE, "Cannot assign to read only property '" + rt.str_of(rt.key_to_value(key)).size() * 0 + std::string("") + "'");
                        f->stack.resize(f->stack.size() - 2);
                        TOP() = v;
                        break;
                    }
                    case OP_DELETE_PROP: case OP_DELETE_ELEM: {
                        Key key;
                        bool st;
                        if (op == OP_DELETE_PROP) {
                            key = Key{c->names[arg / 2], false};
                            st = arg & 1;
                        } else {
                            Value kv = pop(f);
                            if (TOP().is_nullish()) rt.throw_error(E_TYPE, "Cannot convert undefined or null to object");
                            key = rt.to_key(kv);
                            st = arg & 1;
                        }
                        Obj* o = rt.to_object(TOP());
                        bool ok = rt.delete_property(o, key);
                        if (!ok && st) rt.throw_error(E_TYPE, "Cannot delete property");
                        TOP() = Value(ok);
                        break;
                    }
                    case OP_GET_SUPER: {
                        Value k = pop(f);
                        Value thisv = pop(f);
                        Value base = super_base(rt, f->env);
                        Key key = rt.to_key(k);
                        if (base.is_null()) rt.throw_error(E_TYPE, "Cannot read properties of null");
                        PUSH(rt.get(rt.to_object(base), key, thisv));
                        break;
                    }
                    case OP_SET_SUPER: {
                        Value v, k, thisv;
                        if (arg == 1) {
                            k = pop(f);
                            thisv = pop(f);
                            v = pop(f);
                        } else {
                            v = pop(f);
                            k = pop(f);
                            thisv = pop(f);
                        }
                        Value base = super_base(rt, f->env);
                        Key key = rt.to_key(k);
                        if (base.is_null()) rt.throw_error(E_TYPE, "Cannot set properties of null");
                        if (!rt.set(rt.to_object(base), key, v, thisv) && strict) rt.throw_error(E_TYPE, "Cannot assign to read only property");
                        PUSH(v);
                        break;
                    }
                    case OP_GET_PRIVATE: {
                        Value pn = get_var(rt, f->env, c->names[arg], false);
                        TOP() = private_get(rt, TOP(), (PrivateName*)pn.c);
                        break;
                    }
                    case OP_SET_PRIVATE: {
                        Value v = pop(f);
                        Value pn = get_var(rt, f->env, c->names[arg], false);
                        private_set(rt, TOP(), (PrivateName*)pn.c, v);
                        TOP() = v;
                        break;
                    }
                    case OP_PRIVATE_IN: {
                        Value o = TOP();
                        if (!o.is_obj()) rt.throw_error(E_TYPE, "Cannot use 'in' operator to search for a private field in a non-object");
                        Value pn = get_var(rt, f->env, c->names[arg], false);
                        TOP() = Value(find_private(o.o, (PrivateName*)pn.c) >= 0);
                        break;
                    }
                    case OP_TO_KEY: TOP() = rt.key_to_value(rt.to_key(TOP())); break;
                    case OP_TO_OBJECT_CHECK:
                        if (TOP().is_nullish()) rt.throw_error(E_TYPE, "Cannot destructure '" + std::string(TOP().is_null() ? "null" : "undefined") + "'");
                        break;

                    case OP_NEW_OBJECT: PUSH(Value(rt.new_object())); break;
                    case OP_NEW_ARRAY: PUSH(Value(rt.new_array(0))); break;
                    case OP_ARRAY_PUSH: {
                        Value v = pop(f);
                        Obj* a = TOP().o;
                        a->elems.push_back(v);
                        a->length = uint32_t(a->elems.size());
                        break;
                    }
                    case OP_ARRAY_HOLE: {
                        Obj* a = TOP().o;
                        a->elems.push_back(Value::empty());
                        a->length = uint32_t(a->elems.size());
                        break;
                    }
                    case OP_ARRAY_SPREAD: {
                        Value src = pop(f);
                        Obj* a = TOP().o;
                        Runtime::Iter it = rt.get_iterator(src);
                        Value v;
                        while (rt.iter_step(it, v)) {
                            a->elems.push_back(v);
                            a->length = uint32_t(a->elems.size());
                        }
                        break;
                    }
                    case OP_DEFINE_INIT: {
                        Value v = pop(f);
                        Value k = pop(f);
                        Desc d;
                        d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
                        d.value = v;
                        d.writable = d.enumerable = d.configurable = true;
                        rt.define(TOP().o, rt.to_key(k), d, true);
                        break;
                    }
                    case OP_DEFINE_METHOD: case OP_DEFINE_GETTER: case OP_DEFINE_SETTER: {
                        Value fn = pop(f);
                        Value k = pop(f);
                        Obj* o = TOP().o;
                        ((Function*)fn.o)->home_object = o;
                        Desc d;
                        if (op == OP_DEFINE_METHOD) {
                            set_function_name(rt, fn.o, k);
                            d.has_value = d.has_writable = true;
                            d.value = fn;
                            d.writable = true;
                        } else if (op == OP_DEFINE_GETTER) {
                            set_function_name(rt, fn.o, k, "get");
                            d.has_get = true;
                            d.get = fn;
                        } else {
                            set_function_name(rt, fn.o, k, "set");
                            d.has_set = true;
                            d.set = fn;
                        }
                        d.has_enumerable = d.has_configurable = true;
                        d.enumerable = d.configurable = true;
                        rt.define(o, rt.to_key(k), d, true);
                        break;
                    }
                    case OP_COPY_DATA: {
                        Value src = pop(f);
                        Obj* target = TOP().o;
                        if (!src.is_nullish()) {
                            Obj* from = rt.to_object(src);
                            for (Key k : rt.own_keys(from)) {
                                Desc d;
                                if (rt.get_own(from, k, d) && d.enumerable) {
                                    Value v = rt.get(Value(from), k);
                                    Desc nd;
                                    nd.has_value = nd.has_writable = nd.has_enumerable = nd.has_configurable = true;
                                    nd.value = v;
                                    nd.writable = nd.enumerable = nd.configurable = true;
                                    rt.define(target, k, nd, true);
                                }
                            }
                        }
                        break;
                    }
                    case OP_COPY_DATA_EXCEPT: {
                        Value keys = pop(f);
                        Value src = pop(f);
                        Obj* target = rt.new_object();
                        PUSH(Value(target));
                        Obj* from = rt.to_object(src);
                        std::vector<Key> excluded;
                        for (auto& kv : keys.o->elems) excluded.push_back(rt.to_key(kv));
                        for (Key k : rt.own_keys(from)) {
                            if (std::find(excluded.begin(), excluded.end(), k) != excluded.end()) continue;
                            Desc d;
                            if (rt.get_own(from, k, d) && d.enumerable) {
                                Desc nd;
                                nd.has_value = nd.has_writable = nd.has_enumerable = nd.has_configurable = true;
                                nd.value = rt.get(Value(from), k);
                                nd.writable = nd.enumerable = nd.configurable = true;
                                rt.define(target, k, nd, true);
                            }
                        }
                        break;
                    }
                    case OP_SET_PROTO_LIT: {
                        Value v = pop(f);
                        if (v.is_obj()) TOP().o->proto = v.o;
                        else if (v.is_null()) TOP().o->proto = nullptr;
                        break;
                    }
                    case OP_TEMPLATE:
                        if (arg == -1) {
                            Value v = pop(f);
                            if (v.is_sym()) rt.throw_error(E_TYPE, "Cannot convert a Symbol value to a string");
                            Str* s = rt.to_string(v);
                            TOP() = Value(rt.str(TOP().s->v + s->v));
                        } else {
                            PUSH(Value(template_object(rt, c, arg)));
                        }
                        break;
                    case OP_REGEX: {
                        Value flags = pop(f);
                        Value pat = pop(f);
                        Value ctor = rt.get(Value(rt.realm.global), "RegExp");
                        extern Value regexp_create(Runtime & rt, Value pattern, Value flags);
                        PUSH(regexp_create(rt, pat, flags));
                        (void)ctor;
                        break;
                    }
                    case OP_SET_FN_NAME: {
                        Value fn = TOP();
                        Value name = arg == -1 ? AT(1) : c->consts[arg];
                        // only anonymous functions get the inferred name
                        Desc d;
                        bool has_name = rt.get_own(fn.o, rt.key("name"), d);
                        bool empty = !has_name || (d.is_data() && d.value.is_str() && d.value.s->v.empty());
                        if (empty) set_function_name(rt, fn.o, name);
                        break;
                    }

                    case OP_ADD: {
                        Value b = AT(0), a = AT(1);
                        Value r;
                        if (a.is_num() && b.is_num()) r = Value(a.n + b.n);
                        else r = rt.add(a, b);
                        f->stack.pop_back();
                        TOP() = r;
                        break;
                    }
                    case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_EXP: case OP_SHL: case OP_SHR: case OP_USHR:
                    case OP_BAND: case OP_BOR: case OP_BXOR: {
                        Value b = AT(0), a = AT(1);
                        Value r = binary_num(rt, op, a, b);
                        f->stack.pop_back();
                        TOP() = r;
                        break;
                    }
                    case OP_EQ: case OP_NE: {
                        Value b = AT(0), a = AT(1);
                        bool r = rt.loose_equals(a, b);
                        f->stack.pop_back();
                        TOP() = Value(op == OP_EQ ? r : !r);
                        break;
                    }
                    case OP_SEQ: case OP_SNE: {
                        Value b = pop(f);
                        bool r = rt.strict_equals(TOP(), b);
                        TOP() = Value(op == OP_SEQ ? r : !r);
                        break;
                    }
                    case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                        Value b = AT(0), a = AT(1);
                        bool r;
                        if (a.is_num() && b.is_num()) {
                            r = op == OP_LT ? a.n < b.n : op == OP_GT ? a.n > b.n : op == OP_LE ? a.n <= b.n : a.n >= b.n;
                        } else if (op == OP_LT) {
                            r = less_than(rt, a, b, true) == 1;
                        } else if (op == OP_GT) {
                            r = less_than(rt, b, a, false) == 1;
                        } else if (op == OP_LE) {
                            r = less_than(rt, b, a, false) == 0;
                        } else {
                            r = less_than(rt, a, b, true) == 0;
                        }
                        f->stack.pop_back();
                        TOP() = Value(r);
                        break;
                    }
                    case OP_INSTANCEOF: {
                        Value b = AT(0), a = AT(1);
                        bool r = rt.instance_of(a, b);
                        f->stack.pop_back();
                        TOP() = Value(r);
                        break;
                    }
                    case OP_IN: {
                        Value b = AT(0), a = AT(1);
                        if (!b.is_obj()) rt.throw_error(E_TYPE, "Cannot use 'in' operator to search for a key in a non-object");
                        bool r = rt.has_property(b.o, rt.to_key(a));
                        f->stack.pop_back();
                        TOP() = Value(r);
                        break;
                    }
                    case OP_NOT: TOP() = Value(!rt.to_boolean(TOP())); break;
                    case OP_NEG: TOP() = Value(-rt.to_numeric(TOP()).n); break;
                    case OP_PLUS: TOP() = Value(rt.to_number(TOP())); break;
                    case OP_BNOT: TOP() = Value(double(~rt.to_int32(rt.to_numeric(TOP())))); break;
                    case OP_TYPEOF: TOP() = rt.typeof_(TOP()); break;
                    case OP_INC: TOP() = Value(TOP().n + 1); break;
                    case OP_DEC: TOP() = Value(TOP().n - 1); break;
                    case OP_TO_NUMERIC: TOP() = rt.to_numeric(TOP()); break;

                    case OP_JMP:
                        if (arg < f->pc) {
                            if (rt.interrupt) rt.throw_error(E_ERROR, "interrupted (time limit)");
                            if (rt.unsafe_depth == 0) rt.maybe_gc();
                        }
                        f->pc = arg;
                        break;
                    case OP_JMP_FALSE:
                        if (!rt.to_boolean(pop(f))) f->pc = arg;
                        break;
                    case OP_JMP_TRUE:
                        if (rt.to_boolean(pop(f))) {
                            if (rt.interrupt) rt.throw_error(E_ERROR, "interrupted (time limit)");
                            if (arg < f->pc && rt.unsafe_depth == 0) rt.maybe_gc();
                            f->pc = arg;
                        }
                        break;
                    case OP_JMP_FALSE_KEEP:
                        if (!rt.to_boolean(TOP())) f->pc = arg;
                        break;
                    case OP_JMP_TRUE_KEEP:
                        if (rt.to_boolean(TOP())) f->pc = arg;
                        break;
                    case OP_JMP_NULLISH_KEEP:
                        if (TOP().is_nullish()) f->pc = arg;
                        break;
                    case OP_JMP_NOT_NULLISH_KEEP:
                        if (!TOP().is_nullish()) f->pc = arg;
                        break;
                    case OP_JMP_UNDEF_KEEP:
                        if (TOP().is_undef()) f->pc = arg;
                        break;
                    case OP_JMP_NOT_UNDEF:
                        if (!TOP().is_undef()) f->pc = arg;
                        break;
                    case OP_JMP_NULLISH:
                        if (TOP().is_nullish()) {
                            f->stack.pop_back();
                            TOP() = Value();
                            f->pc = arg;
                        }
                        break;

                    case OP_CALL: {
                        size_t base = f->stack.size() - arg - 2;
                        Value fn = f->stack[base], thisv = f->stack[base + 1];
                        if (!rt.is_callable(fn)) rt.throw_error(E_TYPE, "not a function");
                        if (rt.interrupt) rt.throw_error(E_ERROR, "interrupted (time limit)");
                        if (rt.unsafe_depth == 0) rt.maybe_gc();
                        Value r = do_call(rt, fn, thisv, f->stack.data() + base + 2, arg);
                        f->stack.resize(base);
                        PUSH(r);
                        break;
                    }
                    case OP_CALL_SPREAD: case OP_CALL_EVAL_SPREAD: {
                        Value arr = pop(f);
                        size_t base = f->stack.size() - 2;
                        Value fn = f->stack[base], thisv = f->stack[base + 1];
                        std::vector<Value> args(arr.o->elems.begin(), arr.o->elems.end());
                        // keep the arguments rooted on the stack while calling
                        for (auto& a : args) PUSH(a);
                        if (!rt.is_callable(fn)) rt.throw_error(E_TYPE, "not a function");
                        Value r;
                        if (op == OP_CALL_EVAL_SPREAD && fn.is_obj() && fn.o == rt.eval_fn)
                            r = direct_eval_call(rt, f, f->stack.data() + base + 2, int(args.size()), arg & 1);
                        else
                            r = do_call(rt, fn, thisv, f->stack.data() + base + 2, int(args.size()));
                        f->stack.resize(base);
                        PUSH(r);
                        break;
                    }
                    case OP_CALL_EVAL: {
                        int argc = arg / 2;
                        size_t base = f->stack.size() - argc - 2;
                        Value fn = f->stack[base], thisv = f->stack[base + 1];
                        if (!rt.is_callable(fn)) rt.throw_error(E_TYPE, "not a function");
                        Value r;
                        if (fn.is_obj() && fn.o == rt.eval_fn) r = direct_eval_call(rt, f, f->stack.data() + base + 2, argc, arg & 1);
                        else r = do_call(rt, fn, thisv, f->stack.data() + base + 2, argc);
                        f->stack.resize(base);
                        PUSH(r);
                        break;
                    }
                    case OP_NEW: {
                        size_t base = f->stack.size() - arg - 1;
                        Value fn = f->stack[base];
                        if (!rt.is_constructor(fn)) rt.throw_error(E_TYPE, "not a constructor");
                        Value r = do_construct(rt, fn, f->stack.data() + base + 1, arg, fn);
                        f->stack.resize(base);
                        PUSH(r);
                        break;
                    }
                    case OP_NEW_SPREAD: {
                        Value arr = pop(f);
                        Value fn = TOP();
                        std::vector<Value> args(arr.o->elems.begin(), arr.o->elems.end());
                        PUSH(arr);
                        if (!rt.is_constructor(fn)) rt.throw_error(E_TYPE, "not a constructor");
                        Value r = do_construct(rt, fn, args.data(), int(args.size()), fn);
                        f->stack.pop_back();
                        TOP() = r;
                        break;
                    }
                    case OP_SUPER_CALL: case OP_SUPER_CALL_SPREAD: {
                        std::vector<Value> args;
                        size_t base;
                        if (op == OP_SUPER_CALL) {
                            base = f->stack.size() - arg;
                            args.assign(f->stack.begin() + base, f->stack.end());
                        } else {
                            base = f->stack.size() - 1;
                            Value arr = TOP();
                            args.assign(arr.o->elems.begin(), arr.o->elems.end());
                        }
                        Env* fe = function_env(f->env);
                        Function* active = (Function*)fe->function;
                        Obj* super_ctor = rt.get_proto(active);
                        if (!super_ctor || !rt.is_constructor(Value(super_ctor))) rt.throw_error(E_TYPE, "Super constructor is not a constructor");
                        Value r = do_construct(rt, Value(super_ctor), args.data(), int(args.size()), fe->new_target);
                        if (fe->this_initialized) rt.throw_error(E_REFERENCE, "Super constructor may only be called once");
                        fe->this_value = r;
                        fe->this_initialized = true;
                        f->stack.resize(base);
                        PUSH(r);
                        initialize_instance(rt, r.o, active);
                        break;
                    }
                    case OP_RETURN: {
                        Value v = pop(f);
                        f->stack.clear();
                        return Result{Result::Return, v};
                    }
                    case OP_THROW: throw Throw{pop(f)};
                    case OP_THROW_ERROR: {
                        u16 m = c->consts[arg].s->v;
                        if (!m.empty() && m[0] == 1) m = m.substr(1);
                        rt.throw_error(E_REFERENCE, to_utf8(m));
                    }
                    case OP_TRY_PUSH: f->handlers.push_back(Handler{arg, f->stack.size(), f->env}); break;
                    case OP_TRY_POP: f->handlers.pop_back(); break;
                    case OP_SET_COMPLETION: f->completion = pop(f); break;
                    case OP_GET_COMPLETION: PUSH(f->completion); break;

                    case OP_CLOSURE: PUSH(Value(make_closure(rt, c->functions[arg], f->env))); break;
                    case OP_THIS: PUSH(this_of(rt, f->env)); break;
                    case OP_NEW_TARGET: {
                        Env* e = function_env(f->env);
                        PUSH(e && e != rt.realm.global_env ? e->new_target : Value());
                        break;
                    }
                    case OP_ARGUMENTS: PUSH(Value(make_arguments(rt, f, arg == 1))); break;
                    case OP_REST_ARGS: {
                        std::vector<Value> rest;
                        for (size_t k = size_t(arg); k < f->args.size(); k++) rest.push_back(f->args[k]);
                        PUSH(Value(rt.array_from_list(rest)));
                        break;
                    }
                    case OP_GET_ARG: PUSH(size_t(arg) < f->args.size() ? f->args[arg] : Value()); break;
                    case OP_CLASS: {
                        int ctor_idx = arg / 4 - 1;
                        bool has_super = arg & 1;
                        bool name_from_stack = arg & 2;
                        Value namev = c->consts[int(pop(f).n)];
                        Value superclass = pop(f);
                        Value keyname;
                        if (name_from_stack) keyname = pop(f);
                        Obj* proto_parent = rt.realm.object_proto;
                        Obj* ctor_parent = rt.realm.function_proto;
                        if (has_super) {
                            if (superclass.is_null()) {
                                proto_parent = nullptr;
                            } else {
                                if (!rt.is_constructor(superclass)) rt.throw_error(E_TYPE, "Class extends value is not a constructor or null");
                                Value pp = rt.get(superclass, "prototype");
                                if (!pp.is_obj() && !pp.is_null()) rt.throw_error(E_TYPE, "Class extends value does not have valid prototype property");
                                proto_parent = pp.is_null() ? nullptr : pp.o;
                                ctor_parent = superclass.o;
                            }
                        }
                        Obj* proto = rt.new_object(proto_parent);
                        Function* F;
                        if (ctor_idx >= 0) {
                            F = make_closure(rt, c->functions[ctor_idx], f->env);
                        } else {
                            // the default constructor
                            extern Function* default_constructor(Runtime & rt, bool derived, Env* env);
                            F = default_constructor(rt, has_super, f->env);
                        }
                        F->proto = ctor_parent;
                        F->is_class_constructor = true;
                        F->home_object = proto;
                        Desc d;
                        d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
                        d.value = Value(proto);
                        rt.define(F, rt.key("prototype"), d);
                        rt.define_data(proto, "constructor", Value(F), WRITABLE | CONFIGURABLE);
                        proto->prim = Value(F);  // back link for fields
                        // the name
                        Value nm = name_from_stack ? keyname : namev;
                        Desc nd;
                        bool has = rt.get_own(F, rt.key("name"), nd);
                        (void)has;
                        set_function_name(rt, F, nm.is_undef() ? Value(rt.intern("")) : nm);
                        PUSH(Value(F));
                        PUSH(Value(proto));
                        class_data(rt, F, true);
                        break;
                    }
                    case OP_CLASS_PRIVATE: {
                        Value desc = pop(f);
                        PrivateName* pn = rt.alloc<PrivateName>();
                        pn->description = desc;
                        Binding& b = f->env->vars[c->names[arg]];
                        Value v = Value::empty();
                        v.c = pn;
                        b.v = v;
                        b.initialized = true;
                        b.mutable_ = false;
                        break;
                    }
                    case OP_CLASS_METHOD: {
                        Value fn = pop(f);
                        Value k = pop(f);
                        Obj* home = TOP().o;
                        bool is_static = arg & 1;
                        int kind = arg / 2;
                        ((Function*)fn.o)->home_object = home;
                        if (k.tag == Tag::Empty && k.c) {
                            PrivateName* pn = (PrivateName*)k.c;
                            set_function_name(rt, fn.o, k, kind == 1 ? "get" : kind == 2 ? "set" : nullptr);
                            if (kind == 0) {
                                pn->is_method = true;
                                pn->method = fn;
                            } else {
                                pn->is_accessor = true;
                                if (kind == 1) pn->getter = fn;
                                else pn->setter = fn;
                            }
                            Function* ctor = is_static ? (Function*)home : (Function*)home->prim.o;
                            if (is_static) {
                                if (find_private(ctor, pn) < 0) private_add(rt, ctor, pn, Value());
                            } else {
                                ClassData* cd = class_data(rt, ctor, true);
                                if (std::find(cd->methods.begin(), cd->methods.end(), pn) == cd->methods.end()) cd->methods.push_back(pn);
                            }
                            break;
                        }
                        Key key = rt.to_key(k);
                        Desc d;
                        if (kind == 0) {
                            set_function_name(rt, fn.o, k);
                            d.has_value = d.has_writable = true;
                            d.value = fn;
                            d.writable = true;
                        } else if (kind == 1) {
                            set_function_name(rt, fn.o, k, "get");
                            d.has_get = true;
                            d.get = fn;
                        } else {
                            set_function_name(rt, fn.o, k, "set");
                            d.has_set = true;
                            d.set = fn;
                        }
                        d.has_enumerable = d.has_configurable = true;
                        d.configurable = true;
                        rt.define(home, key, d, true);
                        break;
                    }
                    case OP_CLASS_FIELD: {
                        Value init = pop(f);
                        Value k = pop(f);
                        Obj* home = pop(f).o;
                        bool is_static = arg & 1;
                        if (init.is_obj()) ((Function*)init.o)->home_object = home;
                        FieldRecord fr;
                        if (k.tag == Tag::Empty && k.c) fr.priv = (PrivateName*)k.c;
                        else fr.key = k;
                        fr.init = init;
                        if (init.is_obj()) {
                            // anonymous functions in initializers are named after the field
                            ((Function*)init.o)->slots.push_back(fr.priv ? k : k);
                        }
                        Function* ctor = is_static ? (Function*)home : (Function*)home->prim.o;
                        ClassData* cd = class_data(rt, ctor, true);
                        (is_static ? cd->static_fields : cd->fields).push_back(fr);
                        break;
                    }
                    case OP_INIT_FIELDS: {
                        Value ctor = pop(f);
                        ClassData* cd = class_data(rt, (Function*)ctor.o, true);
                        FieldRecord fr = cd->static_fields[cd->static_next++];
                        Value v;
                        if (!fr.init.is_undef()) v = rt.call(fr.init, ctor, nullptr, 0);
                        if (fr.priv) private_add(rt, ctor.o, fr.priv, v);
                        else {
                            Desc d;
                            d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
                            d.value = v;
                            d.writable = d.enumerable = d.configurable = true;
                            rt.define(ctor.o, rt.to_key(fr.key), d, true);
                        }
                        break;
                    }
                    case OP_CLASS_SET_HOME: {
                        Value fn = TOP();
                        ((Function*)fn.o)->home_object = AT(1).o;
                        break;
                    }
                    case OP_IMPORT_CALL: {
                        Value opts = arg ? pop(f) : Value();
                        Value spec = pop(f);
                        Value res, rej;
                        Obj* p = rt.new_promise(res, rej);
                        try {
                            rt.to_string(spec);
                            if (!opts.is_undef() && !opts.is_obj()) rt.throw_error(E_TYPE, "import() options must be an object");
                            rt.throw_error(E_TYPE, "modules are not supported");
                        } catch (Throw& t) {
                            Value a[1] = {t.v};
                            rt.call(rej, Value(), a, 1);
                        }
                        PUSH(Value(p));
                        break;
                    }

                    case OP_GET_ITER: case OP_GET_ASYNC_ITER: {
                        Value v = pop(f);
                        Runtime::Iter it = rt.get_iterator(v, op == OP_GET_ASYNC_ITER);
                        IterRecord* r = rt.alloc<IterRecord>();
                        r->iterator = it.iterator;
                        r->next = it.next;
                        PUSH(Value((Obj*)r));
                        break;
                    }
                    case OP_ITER_NEXT: {
                        IterRecord* r = (IterRecord*)TOP().o;
                        Value res = rt.call(r->next, r->iterator, nullptr, 0);
                        if (arg == 0 && !res.is_obj()) rt.throw_error(E_TYPE, "Iterator result is not an object");
                        PUSH(res);
                        break;
                    }
                    case OP_ASYNC_ITER_RESULT:
                        if (!TOP().is_obj()) rt.throw_error(E_TYPE, "Iterator result is not an object");
                        if (arg == 1) f->stack.pop_back();
                        break;
                    case OP_ITER_DONE: {
                        Value res = TOP();
                        IterRecord* r = (IterRecord*)AT(1).o;
                        if (rt.to_boolean(rt.get(res, "done"))) {
                            r->done = true;
                            f->stack.pop_back();
                            f->pc = arg;
                        } else {
                            TOP() = rt.get(res, "value");
                        }
                        break;
                    }
                    case OP_ITER_VALUE: {
                        IterRecord* r = (IterRecord*)TOP().o;
                        if (r->done) {
                            PUSH(Value());
                            break;
                        }
                        Value v;
                        try {
                            Value res = rt.call(r->next, r->iterator, nullptr, 0);
                            if (!res.is_obj()) rt.throw_error(E_TYPE, "Iterator result is not an object");
                            if (rt.to_boolean(rt.get(res, "done"))) {
                                r->done = true;
                            } else {
                                v = rt.get(res, "value");
                            }
                        } catch (Throw&) {
                            r->done = true;
                            throw;
                        }
                        PUSH(v);
                        break;
                    }
                    case OP_ITER_REST: {
                        IterRecord* r = (IterRecord*)TOP().o;
                        Obj* a = rt.new_array(0);
                        PUSH(Value(a));
                        while (!r->done) {
                            try {
                                Value res = rt.call(r->next, r->iterator, nullptr, 0);
                                if (!res.is_obj()) rt.throw_error(E_TYPE, "Iterator result is not an object");
                                if (rt.to_boolean(rt.get(res, "done"))) {
                                    r->done = true;
                                    break;
                                }
                                Value v = rt.get(res, "value");
                                a->elems.push_back(v);
                                a->length = uint32_t(a->elems.size());
                            } catch (Throw&) {
                                r->done = true;
                                throw;
                            }
                        }
                        break;
                    }
                    case OP_ITER_CLOSE: {
                        IterRecord* r = (IterRecord*)pop(f).o;
                        Runtime::Iter it{r->iterator, r->next, r->done};
                        if (arg == 1) {
                            // async: push the result of return() for the AWAIT that follows
                            Value ret = rt.get_method(r->iterator, rt.key("return"));
                            if (ret.is_undef()) {
                                PUSH(Value(rt.new_object()));  // nothing to await; an object passes the check
                            } else {
                                PUSH(rt.call(ret, r->iterator, nullptr, 0));
                            }
                        } else {
                            rt.iter_close(it, false);
                        }
                        break;
                    }
                    case OP_ITER_CLOSE_UNDER: {
                        Value exc = pop(f);
                        IterRecord* r = (IterRecord*)pop(f).o;
                        if (arg == 2) {
                            // async iterator, throw completion: errors from return() are ignored
                            PUSH(exc);
                            Value res;
                            try {
                                Value ret = rt.get_method(r->iterator, rt.key("return"));
                                if (!ret.is_undef()) res = rt.call(ret, r->iterator, nullptr, 0);
                            } catch (Throw&) {
                            }
                            PUSH(res);
                            break;
                        }
                        if (!r->done) {
                            Runtime::Iter it{r->iterator, r->next, false};
                            try {
                                rt.iter_close(it, true);
                            } catch (Throw&) {
                            }
                        }
                        PUSH(exc);
                        break;
                    }
                    case OP_ITER_CLOSE_IF_NOT_DONE: {
                        IterRecord* r = (IterRecord*)pop(f).o;
                        if (!r->done) {
                            Runtime::Iter it{r->iterator, r->next, false};
                            rt.iter_close(it, false);
                        }
                        break;
                    }
                    case OP_FOR_IN: {
                        Value v = pop(f);
                        extern Obj* for_in_enumerator(Runtime & rt, Value v);
                        PUSH(Value(for_in_enumerator(rt, v)));
                        break;
                    }
                    case OP_FOR_IN_NEXT: {
                        extern bool for_in_next(Runtime & rt, Obj * e, Value & out);
                        Value k;
                        if (!for_in_next(rt, TOP().o, k)) f->pc = arg;
                        else PUSH(k);
                        break;
                    }

                    case OP_INITIAL_YIELD:
                        f->suspended = Suspend::Initial;
                        return Result{Result::Yield, Value()};
                    case OP_YIELD: {
                        Value v = pop(f);
                        f->suspended = Suspend::Yield;
                        return Result{Result::Yield, Value(rt.iter_result(v, false))};
                    }
                    case OP_ASYNC_GEN_YIELD: {
                        Value v = pop(f);
                        f->suspended = Suspend::Yield;
                        return Result{Result::Yield, v};
                    }
                    case OP_AWAIT: {
                        Value v = pop(f);
                        f->suspended = Suspend::Await;
                        return Result{Result::Await, v};
                    }
                    case OP_RESUME_MODE: {
                        double mode = pop(f).n;
                        if (mode == 1) throw Throw{pop(f)};
                        if (mode == 2) f->pc = arg;
                        break;
                    }
                    case OP_YIELD_STAR_INIT: {
                        Value v = pop(f);
                        Runtime::Iter it = rt.get_iterator(v, arg == 1);
                        IterRecord* r = rt.alloc<IterRecord>();
                        r->iterator = it.iterator;
                        r->next = it.next;
                        PUSH(Value((Obj*)r));
                        PUSH(Value());
                        PUSH(Value(0.0));
                        break;
                    }
                    case OP_YIELD_STAR: {
                        bool is_async = arg & 1;
                        int done_target = arg / 2;
                        Value inner_result;
                        IterRecord* r;
                        double mode = 0;
                        if (is_async && f->stack.size() >= 2 && AT(1).is_obj() && ((IterRecord*)AT(1).o)->phase == 1 &&
                            AT(1).o->kind == Kind::Ordinary && false) {
                        }
                        // phase 1 (async): the awaited inner result is on top
                        Value top = TOP();
                        Value below = AT(1);
                        if (is_async && below.is_obj() && dynamic_cast<IterRecord*>(below.o) && ((IterRecord*)below.o)->phase != 0) {
                            r = (IterRecord*)below.o;
                            inner_result = pop(f);
                            mode = r->phase - 1;  // phase stores mode+1
                            r->phase = 0;
                        } else {
                            mode = pop(f).n;
                            Value received = pop(f);
                            r = (IterRecord*)TOP().o;
                            (void)top;
                            Value method;
                            if (mode == 0) {
                                inner_result = rt.call(r->next, r->iterator, &received, 1);
                            } else if (mode == 1) {
                                method = rt.get_method(r->iterator, rt.key("throw"));
                                if (method.is_undef()) {
                                    // close the iterator, then a TypeError
                                    Runtime::Iter it{r->iterator, r->next, false};
                                    if (is_async) {
                                        Value ret = rt.get_method(r->iterator, rt.key("return"));
                                        if (!ret.is_undef()) rt.call(ret, r->iterator, nullptr, 0);
                                    } else {
                                        rt.iter_close(it, false);
                                    }
                                    rt.throw_error(E_TYPE, "The iterator does not provide a 'throw' method");
                                }
                                inner_result = rt.call(method, r->iterator, &received, 1);
                            } else {
                                method = rt.get_method(r->iterator, rt.key("return"));
                                if (method.is_undef()) {
                                    // return the received value from this generator
                                    f->stack.pop_back();
                                    PUSH(received);
                                    break;  // falls through to the return path (pc+1)
                                }
                                inner_result = rt.call(method, r->iterator, &received, 1);
                            }
                            if (is_async) {
                                // await the inner result, then come back here (phase = mode+1)
                                r->phase = int(mode) + 1;
                                f->pc--;
                                f->suspended = Suspend::Await;
                                return Result{Result::Await, inner_result};
                            }
                        }
                        if (!inner_result.is_obj()) rt.throw_error(E_TYPE, "Iterator result is not an object");
                        if (rt.to_boolean(rt.get(inner_result, "done"))) {
                            Value value = rt.get(inner_result, "value");
                            f->stack.pop_back();  // the record
                            PUSH(value);
                            if (mode == 2) break;  // the return path at pc+1
                            f->pc = done_target;
                            break;
                        }
                        // yield the inner result and re-run this op on resumption
                        f->pc--;
                        f->suspended = Suspend::YieldStar;
                        if (is_async) return Result{Result::Yield, rt.get(inner_result, "value")};
                        return Result{Result::Yield, inner_result};
                    }
                    case OP_DEBUGGER: case OP_NOP: break;
                    default: rt.throw_error(E_ERROR, "internal: bad opcode");
                }
            }
        } catch (Throw& t) {
            if (f->handlers.empty()) {
                f->stack.clear();
                throw;
            }
            Handler h = f->handlers.back();
            f->handlers.pop_back();
            f->stack.resize(h.sp);
            f->env = h.env;
            f->stack.push_back(t.v);
            f->pc = h.pc;
        }
    }
}

// ------------------------------------------------------------------ eval
Value direct_eval_call(Runtime& rt, Frame* f, const Value* args, int argc, bool strict) {
    if (argc == 0) return Value();
    if (!args[0].is_str()) return args[0];
    // the context: inside a function (new.target, super) or a class field
    Env* fe = function_env(f->env);
    bool in_function = fe && fe != rt.realm.global_env && fe->function;
    Code* fc = in_function ? ((Function*)fe->function)->code : nullptr;
    Ast ast;
    ast.source = args[0].s->v;
    ParseOptions o;
    o.strict = strict;
    o.is_eval = true;
    o.in_function = false;
    o.allow_new_target = in_function && fc && !fc->is_arrow;
    o.allow_super_property = in_function && fe->home_object;
    o.allow_super_call = in_function && fc && fc->kind == FnKind::DerivedConstructor;
    o.in_class_field = in_function && fc && fc->is_class_field;
    // private names visible from here
    for (Env* e = f->env; e; e = e->parent)
        for (auto& kv : e->vars)
            if (!kv.first->v.empty() && kv.first->v[0] == '#') o.private_names.push_back(kv.first->v.substr(1));
    // arguments is not allowed in class field initializers
    Code* code;
    try {
        parse(ast, o);
        if (o.in_class_field) {
            // a reference to `arguments` anywhere in eval code inside a field is a SyntaxError
            for (auto& n : ast.pool)
                if (n->type == N::Ident && n->str == u"arguments") throw SyntaxError{"arguments in class field", n->line};
        }
        code = compile_program(rt, ast, "eval", true, strict);
    } catch (SyntaxError& e) {
        rt.throw_error(E_SYNTAX, e.msg);
    }
    Env* env = new_env(rt, f->env);
    if (code->strict) env->var_scope = true;
    Frame* ef = rt.alloc<Frame>();
    ef->code = code;
    ef->env = env;
    FrameGuard fg(rt, ef);
    Result r = execute(rt, ef, Resume{});
    return r.v;
}

}  // namespace kjs
