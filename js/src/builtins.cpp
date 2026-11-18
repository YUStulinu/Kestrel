// The core builtins: the global object, Object, Function, Error, Boolean,
// Symbol, Number, Math, Reflect, eval and URI functions, generator objects.
#include "builtins.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace kjs {

Value desc_to_object(Runtime& rt, const Desc& d) {
    Obj* o = rt.new_object();
    if (d.is_accessor()) {
        create_data(rt, o, rt.key("get"), d.get);
        create_data(rt, o, rt.key("set"), d.set);
    } else {
        create_data(rt, o, rt.key("value"), d.value);
        create_data(rt, o, rt.key("writable"), Value(d.writable));
    }
    create_data(rt, o, rt.key("enumerable"), Value(d.enumerable));
    create_data(rt, o, rt.key("configurable"), Value(d.configurable));
    return Value(o);
}

Desc to_desc(Runtime& rt, Value v) {
    if (!v.is_obj()) rt.throw_error(E_TYPE, "Property description must be an object");
    Desc d;
    Obj* o = v.o;
    auto has = [&](const char* k) { return rt.has_property(o, rt.key(k)); };
    if (has("enumerable")) d.has_enumerable = true, d.enumerable = rt.to_boolean(rt.get(v, "enumerable"));
    if (has("configurable")) d.has_configurable = true, d.configurable = rt.to_boolean(rt.get(v, "configurable"));
    if (has("value")) d.has_value = true, d.value = rt.get(v, "value");
    if (has("writable")) d.has_writable = true, d.writable = rt.to_boolean(rt.get(v, "writable"));
    if (has("get")) {
        d.has_get = true;
        d.get = rt.get(v, "get");
        if (!d.get.is_undef() && !rt.is_callable(d.get)) rt.throw_error(E_TYPE, "Getter must be a function");
    }
    if (has("set")) {
        d.has_set = true;
        d.set = rt.get(v, "set");
        if (!d.set.is_undef() && !rt.is_callable(d.set)) rt.throw_error(E_TYPE, "Setter must be a function");
    }
    if ((d.has_get || d.has_set) && (d.has_value || d.has_writable)) rt.throw_error(E_TYPE, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
    return d;
}

// ------------------------------------------------------------------ Object
NATIVE(object_ctor) {
    UNUSED_NATIVE_ARGS;
    if (!nt.is_undef() && nt.o != callee) return Value(rt.new_object(rt.get_prototype_from_ctor(nt, rt.realm.object_proto)));
    Value v = ARG(0);
    if (v.is_nullish()) return Value(rt.new_object());
    return Value(rt.to_object(v));
}
NATIVE(object_get_proto) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(ARG(0));
    Obj* p = rt.get_proto(o);
    return p ? Value(p) : Value::null();
}
NATIVE(object_set_proto) {
    UNUSED_NATIVE_ARGS;
    Value o = ARG(0), p = ARG(1);
    if (o.is_nullish()) rt.throw_error(E_TYPE, "Object.setPrototypeOf called on null or undefined");
    if (!p.is_obj() && !p.is_null()) rt.throw_error(E_TYPE, "Object prototype may only be an Object or null");
    if (!o.is_obj()) return o;
    if (!rt.set_proto(o.o, p.is_null() ? nullptr : p.o)) rt.throw_error(E_TYPE, "Cannot set prototype");
    return o;
}
static void define_properties(Runtime& rt, Obj* o, Value props) {
    Obj* p = rt.to_object(props);
    std::vector<std::pair<Key, Desc>> ds;
    for (Key k : rt.own_keys(p)) {
        Desc pd;
        if (rt.get_own(p, k, pd) && pd.enumerable) ds.push_back({k, to_desc(rt, rt.get(Value(p), k))});
    }
    for (auto& kd : ds) rt.define(o, kd.first, kd.second, true);
}
NATIVE(object_create) {
    UNUSED_NATIVE_ARGS;
    Value p = ARG(0);
    if (!p.is_obj() && !p.is_null()) rt.throw_error(E_TYPE, "Object prototype may only be an Object or null");
    Obj* o = rt.new_object();
    o->proto = p.is_null() ? nullptr : p.o;
    if (!ARG(1).is_undef()) define_properties(rt, o, ARG(1));
    return Value(o);
}
NATIVE(object_define_property) {
    UNUSED_NATIVE_ARGS;
    if (!ARG(0).is_obj()) rt.throw_error(E_TYPE, "Object.defineProperty called on non-object");
    Key k = rt.to_key(ARG(1));
    Desc d = to_desc(rt, ARG(2));
    rt.define(ARG(0).o, k, d, true);
    return ARG(0);
}
NATIVE(object_define_properties) {
    UNUSED_NATIVE_ARGS;
    if (!ARG(0).is_obj()) rt.throw_error(E_TYPE, "Object.defineProperties called on non-object");
    define_properties(rt, ARG(0).o, ARG(1));
    return ARG(0);
}
NATIVE(object_gopd) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(ARG(0));
    Key k = rt.to_key(ARG(1));
    Desc d;
    if (!rt.get_own(o, k, d)) return Value();
    return desc_to_object(rt, d);
}
NATIVE(object_gopds) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(ARG(0));
    Obj* r = rt.new_object();
    for (Key k : rt.own_keys(o)) {
        Desc d;
        if (rt.get_own(o, k, d)) create_data(rt, r, k, desc_to_object(rt, d));
    }
    return Value(r);
}
NATIVE(object_gopn) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(ARG(0));
    std::vector<Value> out;
    for (Key k : rt.own_keys(o))
        if (!k.is_sym) out.push_back(Value(k.str()));
    return Value(rt.array_from_list(out));
}
NATIVE(object_gops) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(ARG(0));
    std::vector<Value> out;
    for (Key k : rt.own_keys(o))
        if (k.is_sym) out.push_back(Value(k.sym()));
    return Value(rt.array_from_list(out));
}
// magic: 0 keys, 1 values, 2 entries
NATIVE(object_keys) {
    UNUSED_NATIVE_ARGS;
    int kind = ((Function*)callee)->native_magic;
    Obj* o = rt.to_object(ARG(0));
    std::vector<Value> out;
    for (Key k : rt.own_keys(o)) {
        if (k.is_sym) continue;
        Desc d;
        if (!rt.get_own(o, k, d) || !d.enumerable) continue;
        if (kind == 0) out.push_back(Value(k.str()));
        else {
            Value v = rt.get(Value(o), k);
            if (kind == 1) out.push_back(v);
            else out.push_back(Value(rt.array_from_list({Value(k.str()), v})));
        }
    }
    return Value(rt.array_from_list(out));
}
NATIVE(object_assign) {
    UNUSED_NATIVE_ARGS;
    Obj* to = rt.to_object(ARG(0));
    for (int i = 1; i < argc; i++) {
        if (args[i].is_nullish()) continue;
        Obj* from = rt.to_object(args[i]);
        for (Key k : rt.own_keys(from)) {
            Desc d;
            if (rt.get_own(from, k, d) && d.enumerable) {
                if (!rt.set(to, k, rt.get(Value(from), k), Value(to))) rt.throw_error(E_TYPE, "Cannot assign to read only property");
            }
        }
    }
    return Value(to);
}
NATIVE(object_is) {
    UNUSED_NATIVE_ARGS;
    return Value(rt.same_value(ARG(0), ARG(1)));
}
// magic: 0 freeze, 1 seal, 2 preventExtensions
NATIVE(object_freeze) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Value v = ARG(0);
    if (!v.is_obj()) return v;
    Obj* o = v.o;
    rt.prevent_extensions(o);
    if (m == 2) return v;
    for (Key k : rt.own_keys(o)) {
        Desc cur;
        if (!rt.get_own(o, k, cur)) continue;
        Desc d;
        d.has_configurable = true;
        d.configurable = false;
        if (m == 0 && cur.is_data()) {
            d.has_writable = true;
            d.writable = false;
        }
        rt.define(o, k, d, true);
    }
    return v;
}
// magic: 0 isFrozen, 1 isSealed
NATIVE(object_is_frozen) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Value v = ARG(0);
    if (!v.is_obj()) return Value(true);
    if (rt.is_extensible(v.o)) return Value(false);
    for (Key k : rt.own_keys(v.o)) {
        Desc d;
        if (!rt.get_own(v.o, k, d)) continue;
        if (d.configurable) return Value(false);
        if (m == 0 && d.is_data() && d.writable) return Value(false);
    }
    return Value(true);
}
NATIVE(object_is_extensible) {
    UNUSED_NATIVE_ARGS;
    return Value(ARG(0).is_obj() && rt.is_extensible(ARG(0).o));
}
NATIVE(object_from_entries) {
    UNUSED_NATIVE_ARGS;
    if (ARG(0).is_nullish()) rt.throw_error(E_TYPE, "Object.fromEntries requires an iterable");
    Obj* o = rt.new_object();
    Runtime::Root r(rt, Value(o));
    Runtime::Iter it = rt.get_iterator(ARG(0));
    Value e;
    while (rt.iter_step(it, e)) {
        try {
            if (!e.is_obj()) rt.throw_error(E_TYPE, "Iterator value is not an entry object");
            Value k = rt.get(e, rt.key("0"));
            Value v = rt.get(e, rt.key("1"));
            create_data(rt, o, rt.to_key(k), v);
        } catch (Throw&) {
            rt.iter_close(it, true);
            throw;
        }
    }
    return Value(o);
}
NATIVE(object_has_own) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(ARG(0));
    return Value(rt.has_own(o, rt.to_key(ARG(1))));
}
NATIVE(object_group_by) {
    UNUSED_NATIVE_ARGS;
    Value items = ARG(0), cb = ARG(1);
    if (items.is_nullish()) rt.throw_error(E_TYPE, "groupBy requires an iterable");
    if (!rt.is_callable(cb)) rt.throw_error(E_TYPE, "callback is not a function");
    Obj* o = rt.new_object();
    o->proto = nullptr;
    Runtime::Root r(rt, Value(o));
    Runtime::Iter it = rt.get_iterator(items);
    Value v;
    double k = 0;
    while (rt.iter_step(it, v)) {
        Key key;
        try {
            Value a[2] = {v, Value(k++)};
            key = rt.to_key(rt.call(cb, Value(), a, 2));
        } catch (Throw&) {
            rt.iter_close(it, true);
            throw;
        }
        Value g = rt.get(Value(o), key);
        if (g.is_undef()) {
            g = Value(rt.new_array(0));
            create_data(rt, o, key, g);
        }
        g.o->elems.push_back(v);
        g.o->length = uint32_t(g.o->elems.size());
    }
    return Value(o);
}

NATIVE(objproto_has_own) {
    UNUSED_NATIVE_ARGS;
    Key k = rt.to_key(ARG(0));
    Obj* o = this_obj(rt, thisv);
    return Value(rt.has_own(o, k));
}
NATIVE(objproto_is_prototype_of) {
    UNUSED_NATIVE_ARGS;
    if (!ARG(0).is_obj()) return Value(false);
    Obj* o = this_obj(rt, thisv);
    for (Obj* p = rt.get_proto(ARG(0).o); p; p = rt.get_proto(p))
        if (p == o) return Value(true);
    return Value(false);
}
NATIVE(objproto_property_is_enumerable) {
    UNUSED_NATIVE_ARGS;
    Key k = rt.to_key(ARG(0));
    Obj* o = this_obj(rt, thisv);
    Desc d;
    return Value(rt.get_own(o, k, d) && d.enumerable);
}
NATIVE(objproto_to_string) {
    UNUSED_NATIVE_ARGS;
    if (thisv.is_undef()) return Value(rt.intern("[object Undefined]"));
    if (thisv.is_null()) return Value(rt.intern("[object Null]"));
    Obj* o = rt.to_object(thisv);
    const char* builtin = "Object";
    switch (o->kind) {
        case Kind::Array: builtin = "Array"; break;
        case Kind::Arguments: builtin = "Arguments"; break;
        case Kind::Function: case Kind::Native: case Kind::Bound: builtin = "Function"; break;
        case Kind::Error: builtin = "Error"; break;
        case Kind::BooleanBox: builtin = "Boolean"; break;
        case Kind::NumberBox: builtin = "Number"; break;
        case Kind::StringBox: builtin = "String"; break;
        case Kind::Date: builtin = "Date"; break;
        case Kind::RegExp: builtin = "RegExp"; break;
        default: break;
    }
    Value tag = rt.get(Value(o), rt.key(rt.sym_to_string_tag));
    u16 t = tag.is_str() ? tag.s->v : to_u16(builtin);
    return Value(rt.str(u"[object " + t + u"]"));
}
NATIVE(objproto_to_locale_string) {
    UNUSED_NATIVE_ARGS;
    return rt.invoke(thisv, rt.key("toString"), {});
}
NATIVE(objproto_value_of) {
    UNUSED_NATIVE_ARGS;
    return Value(this_obj(rt, thisv));
}
NATIVE(objproto_proto_get) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    Obj* p = rt.get_proto(o);
    return p ? Value(p) : Value::null();
}
NATIVE(objproto_proto_set) {
    UNUSED_NATIVE_ARGS;
    if (thisv.is_nullish()) rt.throw_error(E_TYPE, "called on null or undefined");
    Value p = ARG(0);
    if (!p.is_obj() && !p.is_null()) return Value();
    if (!thisv.is_obj()) return Value();
    if (!rt.set_proto(thisv.o, p.is_null() ? nullptr : p.o)) rt.throw_error(E_TYPE, "Cannot set prototype");
    return Value();
}
// __defineGetter__ (0), __defineSetter__ (1)
NATIVE(objproto_define_accessor) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    if (!rt.is_callable(ARG(1))) rt.throw_error(E_TYPE, "accessor is not a function");
    Desc d;
    if (((Function*)callee)->native_magic == 0) d.has_get = true, d.get = ARG(1);
    else d.has_set = true, d.set = ARG(1);
    d.has_enumerable = d.has_configurable = true;
    d.enumerable = d.configurable = true;
    rt.define(o, rt.to_key(ARG(0)), d, true);
    return Value();
}
// __lookupGetter__ (0), __lookupSetter__ (1)
NATIVE(objproto_lookup_accessor) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    Key k = rt.to_key(ARG(0));
    for (; o; o = rt.get_proto(o)) {
        Desc d;
        if (rt.get_own(o, k, d)) {
            if (!d.is_accessor()) return Value();
            return ((Function*)callee)->native_magic == 0 ? d.get : d.set;
        }
    }
    return Value();
}

// ------------------------------------------------------------------ Function
NATIVE(function_proto_call) {
    UNUSED_NATIVE_ARGS;
    return Value();
}
NATIVE(fn_call) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_callable(thisv)) rt.throw_error(E_TYPE, "Function.prototype.call called on a non-function");
    return rt.call(thisv, ARG(0), argc > 1 ? args + 1 : nullptr, argc > 1 ? argc - 1 : 0);
}
NATIVE(fn_apply) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_callable(thisv)) rt.throw_error(E_TYPE, "Function.prototype.apply called on a non-function");
    if (ARG(1).is_nullish()) return rt.call(thisv, ARG(0), nullptr, 0);
    std::vector<Value> a = rt.list_from_array_like(ARG(1));
    return rt.call(thisv, ARG(0), a);
}
NATIVE(fn_bind) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_callable(thisv)) rt.throw_error(E_TYPE, "Bind must be called on a function");
    Function* b = rt.alloc<Function>();
    b->kind = Kind::Bound;
    b->proto = rt.get_proto(thisv.o);
    b->bound_target = thisv;
    b->bound_this = ARG(0);
    for (int i = 1; i < argc; i++) b->bound_args.push_back(args[i]);
    Runtime::Root r(rt, Value(b));
    double len = 0;
    if (rt.has_own(thisv.o, rt.key("length"))) {
        Value l = rt.get(thisv, "length");
        if (l.is_num()) {
            if (std::isinf(l.n)) len = l.n > 0 ? INFINITY : 0;
            else len = std::max(0.0, rt.to_integer(l) - std::max(0, argc - 1));
        }
    }
    rt.define_data(b, "length", Value(len), CONFIGURABLE);
    Value name = rt.get(thisv, "name");
    if (!name.is_str()) name = Value(rt.intern(""));
    rt.define_data(b, "name", Value(rt.str(u"bound " + name.s->v)), CONFIGURABLE);
    return Value(b);
}
NATIVE(fn_to_string) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_callable(thisv)) rt.throw_error(E_TYPE, "Function.prototype.toString requires that 'this' be a Function");
    if (thisv.o->kind == Kind::Function) {
        Code* c = ((Function*)thisv.o)->code;
        if (!c->source.empty()) return Value(rt.str(c->source));
    }
    Value name = rt.get(thisv, "name");
    u16 n = name.is_str() ? name.s->v : u"";
    return Value(rt.str(u"function " + n + u"() { [native code] }"));
}
NATIVE(fn_has_instance) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_callable(thisv)) return Value(false);
    if (thisv.o->kind == Kind::Bound) return Value(rt.instance_of(ARG(0), ((Function*)thisv.o)->bound_target));
    Value v = ARG(0);
    if (!v.is_obj()) return Value(false);
    Value p = rt.get(thisv, "prototype");
    if (!p.is_obj()) rt.throw_error(E_TYPE, "Function has non-object prototype in instanceof check");
    for (Obj* o = rt.get_proto(v.o); o; o = rt.get_proto(o))
        if (o == p.o) return Value(true);
    return Value(false);
}

// new Function / GeneratorFunction / AsyncFunction (magic 1000 + kind)
NATIVE(function_ctor) {
    UNUSED_NATIVE_ARGS;
    int kind = ((Function*)callee)->native_magic - 1000;  // 0 normal, 1 generator, 2 async, 3 async generator
    u16 params, body;
    for (int i = 0; i + 1 < argc; i++) {
        if (i) params += u",";
        params += rt.to_string(args[i])->v;
    }
    if (argc > 0) body = rt.to_string(args[argc - 1])->v;
    Ast ast;
    Code* code;
    try {
        parse_function_ctor(ast, params, body, kind & 1, kind & 2);
        Node* fn = ast.root->kids[0]->a;
        fn->str = u"anonymous";
        code = compile_function_node(rt, ast, fn, "Function");
        code->name = u"anonymous";
        code->has_name_binding = false;
    } catch (SyntaxError& e) {
        rt.throw_error(E_SYNTAX, e.msg);
    }
    Function* f = make_closure(rt, code, rt.realm.global_env);
    Obj* def = kind == 0 ? rt.realm.function_proto : kind == 1 ? rt.realm.generator_function_proto
               : kind == 2 ? rt.realm.async_function_proto : rt.realm.async_generator_function_proto;
    if (!nt.is_undef()) f->proto = rt.get_prototype_from_ctor(nt, def);
    return Value(f);
}

// ------------------------------------------------------------------ Error
static Obj* error_proto(Runtime& rt, int t) { return rt.get(Value(rt.realm.errors[t]), "prototype").o; }
static void install_error_cause(Runtime& rt, Obj* e, Value options) {
    if (options.is_obj() && rt.has_property(options.o, rt.key("cause")))
        rt.define_data(e, "cause", rt.get(options, "cause"), WRITABLE | CONFIGURABLE);
}
NATIVE(error_ctor) {
    UNUSED_NATIVE_ARGS;
    int t = ((Function*)callee)->native_magic - 1000;
    Value target = nt.is_undef() ? Value(callee) : nt;
    Obj* e = rt.new_object(rt.get_prototype_from_ctor(target, error_proto(rt, t)));
    e->kind = Kind::Error;
    Runtime::Root r(rt, Value(e));
    if (t == E_AGGREGATE) {
        if (!ARG(1).is_undef()) rt.define_data(e, "message", Value(rt.to_string(ARG(1))), WRITABLE | CONFIGURABLE);
        install_error_cause(rt, e, ARG(2));
        std::vector<Value> errs;
        Runtime::Iter it = rt.get_iterator(ARG(0));
        Value v;
        while (rt.iter_step(it, v)) errs.push_back(v);
        rt.define_data(e, "errors", Value(rt.array_from_list(errs)), WRITABLE | CONFIGURABLE);
        return Value(e);
    }
    if (!ARG(0).is_undef()) rt.define_data(e, "message", Value(rt.to_string(ARG(0))), WRITABLE | CONFIGURABLE);
    install_error_cause(rt, e, ARG(1));
    return Value(e);
}
NATIVE(error_to_string) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "Error.prototype.toString called on a non-object");
    Value n = rt.get(thisv, "name");
    u16 name = n.is_undef() ? u"Error" : rt.to_string(n)->v;
    Value m = rt.get(thisv, "message");
    u16 msg = m.is_undef() ? u"" : rt.to_string(m)->v;
    if (name.empty()) return Value(rt.str(msg));
    if (msg.empty()) return Value(rt.str(name));
    return Value(rt.str(name + u": " + msg));
}
NATIVE(throw_type_error) {
    UNUSED_NATIVE_ARGS;
    rt.throw_error(E_TYPE, "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions");
}

// ------------------------------------------------------------------ Boolean
NATIVE(boolean_ctor) {
    UNUSED_NATIVE_ARGS;
    bool b = rt.to_boolean(ARG(0));
    if (nt.is_undef()) return Value(b);
    Obj* o = rt.new_object(rt.get_prototype_from_ctor(nt, rt.realm.boolean_proto));
    o->kind = Kind::BooleanBox;
    o->prim = Value(b);
    return Value(o);
}
static bool this_boolean(Runtime& rt, Value v) {
    if (v.is_bool()) return v.b;
    if (v.is_obj() && v.o->kind == Kind::BooleanBox) return v.o->prim.b;
    rt.throw_error(E_TYPE, "not a Boolean");
}
NATIVE(boolean_to_string) {
    UNUSED_NATIVE_ARGS;
    return Value(rt.intern(this_boolean(rt, thisv) ? "true" : "false"));
}
NATIVE(boolean_value_of) {
    UNUSED_NATIVE_ARGS;
    return Value(this_boolean(rt, thisv));
}

// ------------------------------------------------------------------ Symbol
NATIVE(symbol_ctor) {
    UNUSED_NATIVE_ARGS;
    if (!nt.is_undef()) rt.throw_error(E_TYPE, "Symbol is not a constructor");
    Sym* s = rt.alloc<Sym>();
    if (!ARG(0).is_undef()) s->description = Value(rt.to_string(ARG(0)));
    return Value(s);
}
NATIVE(symbol_for) {
    UNUSED_NATIVE_ARGS;
    u16 k = rt.to_string(ARG(0))->v;
    auto it = rt.symbol_registry.find(k);
    if (it != rt.symbol_registry.end()) return Value(it->second);
    Sym* s = rt.alloc<Sym>();
    s->description = Value(rt.intern(k));
    s->registered = true;
    rt.symbol_registry[k] = s;
    return Value(s);
}
NATIVE(symbol_key_for) {
    UNUSED_NATIVE_ARGS;
    if (!ARG(0).is_sym()) rt.throw_error(E_TYPE, "not a symbol");
    if (!ARG(0).y->registered) return Value();
    return ARG(0).y->description;
}
static Sym* this_symbol(Runtime& rt, Value v) {
    if (v.is_sym()) return v.y;
    if (v.is_obj() && v.o->kind == Kind::SymbolBox) return v.o->prim.y;
    rt.throw_error(E_TYPE, "not a Symbol");
}
NATIVE(symbol_to_string) {
    UNUSED_NATIVE_ARGS;
    Sym* s = this_symbol(rt, thisv);
    u16 d = s->description.is_undef() ? u"" : s->description.s->v;
    return Value(rt.str(u"Symbol(" + d + u")"));
}
NATIVE(symbol_value_of) {
    UNUSED_NATIVE_ARGS;
    return Value(this_symbol(rt, thisv));
}
NATIVE(symbol_description) {
    UNUSED_NATIVE_ARGS;
    return this_symbol(rt, thisv)->description;
}

// ------------------------------------------------------------------ Number
NATIVE(number_ctor) {
    UNUSED_NATIVE_ARGS;
    double n = argc ? rt.to_number(ARG(0)) : 0;
    if (nt.is_undef()) return Value(n);
    Obj* o = rt.new_object(rt.get_prototype_from_ctor(nt, rt.realm.number_proto));
    o->kind = Kind::NumberBox;
    o->prim = Value(n);
    return Value(o);
}
static double this_number(Runtime& rt, Value v) {
    if (v.is_num()) return v.n;
    if (v.is_obj() && v.o->kind == Kind::NumberBox) return v.o->prim.n;
    rt.throw_error(E_TYPE, "not a Number");
}
NATIVE(number_to_string) {
    UNUSED_NATIVE_ARGS;
    double x = this_number(rt, thisv);
    int radix = 10;
    if (!ARG(0).is_undef()) {
        double r = rt.to_integer(ARG(0));
        if (r < 2 || r > 36) rt.throw_error(E_RANGE, "toString() radix must be between 2 and 36");
        radix = int(r);
    }
    return Value(rt.str(number_to_u16(x, radix)));
}
NATIVE(number_value_of) {
    UNUSED_NATIVE_ARGS;
    return Value(this_number(rt, thisv));
}
// exact decimal digits of a double for toFixed / toPrecision / toExponential
static std::string big_digits(double x, int frac) {
    char buf[1200];
    snprintf(buf, sizeof buf, "%.*f", frac, x);
    return buf;
}
NATIVE(number_to_fixed) {
    UNUSED_NATIVE_ARGS;
    double x = this_number(rt, thisv);
    double f = rt.to_integer(ARG(0));
    if (std::isinf(f) || f < 0 || f > 100) rt.throw_error(E_RANGE, "toFixed() digits argument must be between 0 and 100");
    if (std::isnan(x)) return Value(rt.intern("NaN"));
    if (std::fabs(x) >= 1e21) return Value(rt.to_string(Value(x)));
    // round half up on the exact decimal value
    std::string s = big_digits(x, int(f));
    if (s == "-0" || (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos)) s = s.substr(1);
    return Value(rt.str(to_u16(s)));
}
static std::string exp_digits(double x, int p, int* e) {
    // p significant digits, rounding half away from zero on the exact value
    char buf[64];
    snprintf(buf, sizeof buf, "%.*e", p - 1, x);
    std::string d;
    const char* q = buf;
    for (; *q && *q != 'e'; q++)
        if (*q != '.' && *q != '-') d.push_back(*q);
    *e = atoi(q + 1);
    return d;
}
NATIVE(number_to_exponential) {
    UNUSED_NATIVE_ARGS;
    double x = this_number(rt, thisv);
    double f = rt.to_integer(ARG(0));
    if (std::isnan(x)) return Value(rt.intern("NaN"));
    if (std::isinf(x)) return Value(rt.intern(x < 0 ? "-Infinity" : "Infinity"));
    if (std::isinf(f) || f < 0 || f > 100) rt.throw_error(E_RANGE, "toExponential() argument must be between 0 and 100");
    std::string sign = x < 0 ? "-" : "";
    x = std::fabs(x);
    std::string d;
    int e;
    if (ARG(0).is_undef()) {
        u16 s = number_to_u16(x);
        // the shortest representation's digits
        char buf[64];
        int p;
        for (p = 1; p <= 17; p++) {
            snprintf(buf, sizeof buf, "%.*e", p - 1, x);
            if (strtod(buf, nullptr) == x) break;
        }
        d = exp_digits(x, p, &e);
    } else if (x == 0) {
        d = std::string(int(f) + 1, '0');
        e = 0;
    } else {
        d = exp_digits(x, int(f) + 1, &e);
    }
    std::string out = d.substr(0, 1);
    if (d.size() > 1) out += "." + d.substr(1);
    out += "e" + std::string(e >= 0 ? "+" : "-") + std::to_string(std::abs(e));
    return Value(rt.str(to_u16(sign + out)));
}
NATIVE(number_to_precision) {
    UNUSED_NATIVE_ARGS;
    double x = this_number(rt, thisv);
    if (ARG(0).is_undef()) return Value(rt.to_string(Value(x)));
    double p = rt.to_integer(ARG(0));
    if (std::isnan(x)) return Value(rt.intern("NaN"));
    if (std::isinf(x)) return Value(rt.intern(x < 0 ? "-Infinity" : "Infinity"));
    if (p < 1 || p > 100) rt.throw_error(E_RANGE, "toPrecision() argument must be between 1 and 100");
    std::string sign = x < 0 ? "-" : "";
    x = std::fabs(x);
    int ip = int(p);
    std::string d;
    int e;
    if (x == 0) {
        d = std::string(ip, '0');
        e = 0;
    } else {
        d = exp_digits(x, ip, &e);
    }
    std::string out;
    if (e < -6 || e >= ip) {
        out = d.substr(0, 1);
        if (ip > 1) out += "." + d.substr(1);
        out += "e" + std::string(e >= 0 ? "+" : "-") + std::to_string(std::abs(e));
    } else if (e == ip - 1) {
        out = d;
    } else if (e >= 0) {
        out = d.substr(0, e + 1) + "." + d.substr(e + 1);
    } else {
        out = "0." + std::string(-(e + 1), '0') + d;
    }
    return Value(rt.str(to_u16(sign + out)));
}
NATIVE(number_is_finite) {
    UNUSED_NATIVE_ARGS;
    return Value(ARG(0).is_num() && std::isfinite(ARG(0).n));
}
NATIVE(number_is_integer) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Value v = ARG(0);
    if (!v.is_num() || !std::isfinite(v.n) || std::trunc(v.n) != v.n) return Value(false);
    if (m == 1) return Value(std::fabs(v.n) <= 9007199254740991.0);
    return Value(true);
}
NATIVE(number_is_nan) {
    UNUSED_NATIVE_ARGS;
    return Value(ARG(0).is_num() && std::isnan(ARG(0).n));
}
NATIVE(global_is_nan) {
    UNUSED_NATIVE_ARGS;
    return Value(std::isnan(rt.to_number(ARG(0))));
}
NATIVE(global_is_finite) {
    UNUSED_NATIVE_ARGS;
    return Value(std::isfinite(rt.to_number(ARG(0))));
}
NATIVE(global_parse_float) {
    UNUSED_NATIVE_ARGS;
    u16 s = rt.to_string(ARG(0))->v;
    size_t i = 0;
    while (i < s.size() && (is_js_space(s[i]) || is_line_term(s[i]))) i++;
    std::string t;
    for (; i < s.size() && s[i] < 128; i++) t.push_back(char(s[i]));
    // the longest prefix that is a StrDecimalLiteral
    size_t j = 0;
    if (j < t.size() && (t[j] == '+' || t[j] == '-')) j++;
    if (t.compare(j, 8, "Infinity") == 0) return Value(t[0] == '-' ? -INFINITY : INFINITY);
    size_t start = j;
    bool digits = false;
    while (j < t.size() && isdigit((unsigned char)t[j])) j++, digits = true;
    if (j < t.size() && t[j] == '.') {
        size_t k = j + 1;
        while (k < t.size() && isdigit((unsigned char)t[k])) k++, digits = true;
        j = k;
    }
    if (!digits) return Value(NAN);
    if (j < t.size() && (t[j] == 'e' || t[j] == 'E')) {
        size_t k = j + 1;
        if (k < t.size() && (t[k] == '+' || t[k] == '-')) k++;
        size_t k2 = k;
        while (k2 < t.size() && isdigit((unsigned char)t[k2])) k2++;
        if (k2 > k) j = k2;
    }
    (void)start;
    return Value(strtod(t.substr(0, j).c_str(), nullptr));
}
NATIVE(global_parse_int) {
    UNUSED_NATIVE_ARGS;
    u16 s = rt.to_string(ARG(0))->v;
    int radix = rt.to_int32(ARG(1));
    size_t i = 0;
    while (i < s.size() && (is_js_space(s[i]) || is_line_term(s[i]))) i++;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    bool strip = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return Value(NAN);
        if (radix != 16) strip = false;
    } else {
        radix = 10;
    }
    if (strip && i + 1 < s.size() && s[i] == '0' && (s[i + 1] | 32) == 'x') {
        i += 2;
        radix = 16;
    }
    double v = 0;
    size_t start = i;
    std::string dig;
    for (; i < s.size(); i++) {
        char16_t c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (d >= radix) break;
        dig.push_back(char(c));
        v = v * radix + d;
    }
    if (i == start) return Value(NAN);
    if (radix == 10 && dig.size() > 15) v = strtod(dig.c_str(), nullptr);
    return Value(neg ? -v : v);
}

// ------------------------------------------------------------------ Math
NATIVE(math_fn) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    double x = rt.to_number(ARG(0));
    switch (m) {
        case 0: return Value(std::fabs(x));
        case 1: return Value(std::acos(x));
        case 2: return Value(std::acosh(x));
        case 3: return Value(std::asin(x));
        case 4: return Value(std::asinh(x));
        case 5: return Value(std::atan(x));
        case 6: return Value(std::atanh(x));
        case 7: return Value(std::cbrt(x));
        case 8: return Value(std::ceil(x));
        case 9: {
            uint32_t n = rt.to_uint32(Value(x));
            if (n == 0) return Value(32.0);
            int c = 0;
            while (!(n & 0x80000000u)) n <<= 1, c++;
            return Value(double(c));
        }
        case 10: return Value(std::cos(x));
        case 11: return Value(std::cosh(x));
        case 12: return Value(std::exp(x));
        case 13: return Value(std::expm1(x));
        case 14: return Value(std::floor(x));
        case 15: return Value(double(float(x)));
        case 16: return Value(std::log(x));
        case 17: return Value(std::log1p(x));
        case 18: return Value(std::log10(x));
        case 19: return Value(std::log2(x));
        case 20: {
            if (std::isnan(x) || std::isinf(x)) return Value(x);
            if (x == 0) return Value(x);
            if (x > 0 && x < 0.5) return Value(0.0);
            if (x < 0 && x >= -0.5) return Value(-0.0);
            return Value(std::floor(x + 0.5));
        }
        case 21: return Value(std::isnan(x) ? NAN : x > 0 ? 1.0 : x < 0 ? -1.0 : x);
        case 22: return Value(std::sin(x));
        case 23: return Value(std::sinh(x));
        case 24: return Value(std::sqrt(x));
        case 25: return Value(std::tan(x));
        case 26: return Value(std::tanh(x));
        case 27: return Value(std::trunc(x));
    }
    return Value(NAN);
}
NATIVE(math_atan2) {
    UNUSED_NATIVE_ARGS;
    double y = rt.to_number(ARG(0)), x = rt.to_number(ARG(1));
    return Value(std::atan2(y, x));
}
NATIVE(math_pow) {
    UNUSED_NATIVE_ARGS;
    double x = rt.to_number(ARG(0)), y = rt.to_number(ARG(1));
    if (std::isnan(y)) return Value(NAN);
    if ((x == 1 || x == -1) && std::isinf(y)) return Value(NAN);
    return Value(std::pow(x, y));
}
NATIVE(math_imul) {
    UNUSED_NATIVE_ARGS;
    int32_t a = rt.to_int32(ARG(0)), b = rt.to_int32(ARG(1));
    return Value(double(int32_t(uint32_t(a) * uint32_t(b))));
}
// magic 0 max, 1 min
NATIVE(math_max) {
    UNUSED_NATIVE_ARGS;
    bool mx = ((Function*)callee)->native_magic == 0;
    std::vector<double> v;
    for (int i = 0; i < argc; i++) v.push_back(rt.to_number(args[i]));
    double r = mx ? -INFINITY : INFINITY;
    for (double x : v) {
        if (std::isnan(x)) return Value(NAN);
        if (mx ? (x > r || (x == 0 && r == 0 && !std::signbit(x))) : (x < r || (x == 0 && r == 0 && std::signbit(x)))) r = x;
    }
    return Value(r);
}
NATIVE(math_hypot) {
    UNUSED_NATIVE_ARGS;
    std::vector<double> v;
    for (int i = 0; i < argc; i++) v.push_back(rt.to_number(args[i]));
    bool nan = false;
    for (double x : v) {
        if (std::isinf(x)) return Value(INFINITY);
        if (std::isnan(x)) nan = true;
    }
    if (nan) return Value(NAN);
    double m = 0;
    for (double x : v) m = std::max(m, std::fabs(x));
    if (m == 0) return Value(0.0);
    double s = 0;
    for (double x : v) s += (x / m) * (x / m);
    return Value(m * std::sqrt(s));
}
static thread_local uint64_t rng_state = 0x9E3779B97F4A7C15ull;
NATIVE(math_random) {
    UNUSED_NATIVE_ARGS;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return Value(double(rng_state >> 11) / 9007199254740992.0);
}

// ------------------------------------------------------------------ Reflect
static Obj* need_obj(Runtime& rt, Value v) {
    if (!v.is_obj()) rt.throw_error(E_TYPE, "Reflect method called on a non-object");
    return v.o;
}
NATIVE(reflect_apply) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_callable(ARG(0))) rt.throw_error(E_TYPE, "Reflect.apply target is not callable");
    std::vector<Value> a = rt.list_from_array_like(ARG(2));
    return rt.call(ARG(0), ARG(1), a);
}
NATIVE(reflect_construct) {
    UNUSED_NATIVE_ARGS;
    if (!rt.is_constructor(ARG(0))) rt.throw_error(E_TYPE, "Reflect.construct target is not a constructor");
    Value newt = argc > 2 ? ARG(2) : ARG(0);
    if (!rt.is_constructor(newt)) rt.throw_error(E_TYPE, "Reflect.construct newTarget is not a constructor");
    std::vector<Value> a = rt.list_from_array_like(ARG(1));
    return rt.construct(ARG(0), a, newt);
}
NATIVE(reflect_define_property) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    Key k = rt.to_key(ARG(1));
    return Value(rt.define(o, k, to_desc(rt, ARG(2))));
}
NATIVE(reflect_delete_property) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    return Value(rt.delete_property(o, rt.to_key(ARG(1))));
}
NATIVE(reflect_get) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    Key k = rt.to_key(ARG(1));
    return rt.get(o, k, argc > 2 ? ARG(2) : ARG(0));
}
NATIVE(reflect_gopd) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    Desc d;
    if (!rt.get_own(o, rt.to_key(ARG(1)), d)) return Value();
    return desc_to_object(rt, d);
}
NATIVE(reflect_get_proto) {
    UNUSED_NATIVE_ARGS;
    Obj* p = rt.get_proto(need_obj(rt, ARG(0)));
    return p ? Value(p) : Value::null();
}
NATIVE(reflect_has) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    return Value(rt.has_property(o, rt.to_key(ARG(1))));
}
NATIVE(reflect_is_extensible) {
    UNUSED_NATIVE_ARGS;
    return Value(rt.is_extensible(need_obj(rt, ARG(0))));
}
NATIVE(reflect_own_keys) {
    UNUSED_NATIVE_ARGS;
    std::vector<Value> out;
    for (Key k : rt.own_keys(need_obj(rt, ARG(0)))) out.push_back(rt.key_to_value(k));
    return Value(rt.array_from_list(out));
}
NATIVE(reflect_prevent_extensions) {
    UNUSED_NATIVE_ARGS;
    return Value(rt.prevent_extensions(need_obj(rt, ARG(0))));
}
NATIVE(reflect_set) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    Key k = rt.to_key(ARG(1));
    return Value(rt.set(o, k, ARG(2), argc > 3 ? ARG(3) : ARG(0)));
}
NATIVE(reflect_set_proto) {
    UNUSED_NATIVE_ARGS;
    Obj* o = need_obj(rt, ARG(0));
    if (!ARG(1).is_obj() && !ARG(1).is_null()) rt.throw_error(E_TYPE, "Object prototype may only be an Object or null");
    return Value(rt.set_proto(o, ARG(1).is_null() ? nullptr : ARG(1).o));
}

// ------------------------------------------------------------------ globals
NATIVE(global_eval) {
    UNUSED_NATIVE_ARGS;
    // indirect eval: global scope, sloppy unless the code says otherwise
    if (!ARG(0).is_str()) return ARG(0);
    Ast ast;
    ast.source = ARG(0).s->v;
    ParseOptions o;
    o.is_eval = true;
    Code* code;
    try {
        parse(ast, o);
        code = compile_program(rt, ast, "eval", true, false);
    } catch (SyntaxError& e) {
        rt.throw_error(E_SYNTAX, e.msg);
    }
    Env* env = new_env(rt, rt.realm.global_env);
    if (code->strict) env->var_scope = true;
    Frame* f = rt.alloc<Frame>();
    f->code = code;
    f->env = env;
    rt.frames.push_back(f);
    struct Pop {
        Runtime& r;
        ~Pop() { r.frames.pop_back(); }
    } p{rt};
    return execute(rt, f, Resume{}).v;
}

static const char* URI_RESERVED = ";/?:@&=+$,";
static const char* URI_UNESCAPED_EXTRA = "-_.!~*'()";
static bool uri_unescaped(char16_t c) {
    return (c < 128 && (isalnum(c) || strchr(URI_UNESCAPED_EXTRA, char(c)))) && c != 0;
}
// magic: 0 encodeURI, 1 encodeURIComponent
NATIVE(global_encode) {
    UNUSED_NATIVE_ARGS;
    bool component = ((Function*)callee)->native_magic == 1;
    u16 s = rt.to_string(ARG(0))->v;
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (uri_unescaped(c) || (!component && c < 128 && (strchr(URI_RESERVED, char(c)) || c == '#') && c != 0)) {
            out.push_back(char(c));
            continue;
        }
        char32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) rt.throw_error(E_URI, "URI malformed");
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) rt.throw_error(E_URI, "URI malformed");
            cp = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        std::string u = to_utf8(u16(1, char16_t(0)));
        u.clear();
        if (cp < 0x80) u.push_back(char(cp));
        else if (cp < 0x800) u.push_back(char(0xC0 | (cp >> 6))), u.push_back(char(0x80 | (cp & 63)));
        else if (cp < 0x10000) u.push_back(char(0xE0 | (cp >> 12))), u.push_back(char(0x80 | ((cp >> 6) & 63))), u.push_back(char(0x80 | (cp & 63)));
        else
            u.push_back(char(0xF0 | (cp >> 18))), u.push_back(char(0x80 | ((cp >> 12) & 63))), u.push_back(char(0x80 | ((cp >> 6) & 63))),
                u.push_back(char(0x80 | (cp & 63)));
        for (unsigned char b : u) {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", b);
            out += buf;
        }
    }
    return Value(rt.str(to_u16(out)));
}
// magic: 0 decodeURI, 1 decodeURIComponent
NATIVE(global_decode) {
    UNUSED_NATIVE_ARGS;
    bool component = ((Function*)callee)->native_magic == 1;
    u16 s = rt.to_string(ARG(0))->v;
    u16 out;
    auto hex = [&](size_t i) -> int {
        if (i + 2 >= s.size() + 0 && i + 2 > s.size() - 0) {}
        if (i + 2 >= s.size() + 1) rt.throw_error(E_URI, "URI malformed");
        auto h = [&](char16_t c) {
            if (c >= '0' && c <= '9') return c - '0';
            if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
            rt.throw_error(E_URI, "URI malformed");
        };
        if (s[i] != '%') rt.throw_error(E_URI, "URI malformed");
        return h(s[i + 1]) * 16 + h(s[i + 2]);
    };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        size_t start = i;
        if (i + 2 >= s.size()) rt.throw_error(E_URI, "URI malformed");
        int b = hex(i);
        i += 2;
        if (b < 0x80) {
            if (!component && (strchr(URI_RESERVED, b) || b == '#') && b != 0) out += s.substr(start, 3);
            else out.push_back(char16_t(b));
            continue;
        }
        int n = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) rt.throw_error(E_URI, "URI malformed");
        char32_t cp = b & (0xFF >> (n + 1));
        for (int k = 1; k < n; k++) {
            if (i + 3 >= s.size() + 1 || i + 1 >= s.size()) rt.throw_error(E_URI, "URI malformed");
            int bb = hex(i + 1);
            if ((bb & 0xC0) != 0x80) rt.throw_error(E_URI, "URI malformed");
            cp = (cp << 6) | (bb & 63);
            i += 3;
        }
        static const char32_t min[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < min[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) rt.throw_error(E_URI, "URI malformed");
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(char16_t(cp));
        }
    }
    return Value(rt.str(out));
}
NATIVE(global_escape) {
    UNUSED_NATIVE_ARGS;
    u16 s = rt.to_string(ARG(0))->v;
    std::string out;
    for (char16_t c : s) {
        if (c < 128 && (isalnum(c) || strchr("@*_+-./", char(c))) && c) out.push_back(char(c));
        else if (c < 256) {
            char b[8];
            snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        } else {
            char b[8];
            snprintf(b, sizeof b, "%%u%04X", c);
            out += b;
        }
    }
    return Value(rt.str(to_u16(out)));
}
NATIVE(global_unescape) {
    UNUSED_NATIVE_ARGS;
    u16 s = rt.to_string(ARG(0))->v;
    u16 out;
    auto hx = [](char16_t c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 5 < s.size() + 0 + 1 && i + 5 <= s.size() - 1 + 1 && s.size() > i + 5 && s[i + 1] == 'u' &&
            hx(s[i + 2]) >= 0 && hx(s[i + 3]) >= 0 && hx(s[i + 4]) >= 0 && hx(s[i + 5]) >= 0) {
            out.push_back(char16_t(hx(s[i + 2]) << 12 | hx(s[i + 3]) << 8 | hx(s[i + 4]) << 4 | hx(s[i + 5])));
            i += 5;
        } else if (s[i] == '%' && i + 2 < s.size() && hx(s[i + 1]) >= 0 && hx(s[i + 2]) >= 0) {
            out.push_back(char16_t(hx(s[i + 1]) << 4 | hx(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return Value(rt.str(out));
}
NATIVE(global_print) {
    UNUSED_NATIVE_ARGS;
    u16 s;
    for (int i = 0; i < argc; i++) {
        if (i) s += u" ";
        s += rt.to_string(args[i])->v;
    }
    if (rt.print) rt.print(s);
    return Value();
}

// ------------------------------------------------------------------ generators
NATIVE(generator_next) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;  // 0 next, 1 throw, 2 return
    if (!thisv.is_obj() || thisv.o->kind != Kind::Generator) rt.throw_error(E_TYPE, "next method called on incompatible receiver");
    Resume::K k = m == 0 ? Resume::Next : m == 1 ? Resume::Throw : Resume::Return;
    return generator_resume(rt, (Generator*)thisv.o, k, ARG(0));
}

Value generator_resume(Runtime& rt, Generator* g, Resume::K mode, Value v) {
    if (g->state == Generator::Executing) rt.throw_error(E_TYPE, "Generator is already running");
    if (g->state == Generator::SuspendedStart && mode != Resume::Next) g->state = Generator::Completed;
    if (g->state == Generator::Completed) {
        if (mode == Resume::Throw) throw Throw{v};
        if (mode == Resume::Return) return Value(rt.iter_result(v, true));
        return Value(rt.iter_result(Value(), true));
    }
    g->state = Generator::Executing;
    Frame* f = g->frame;
    Result r;
    try {
        rt.frames.push_back(f);
        struct Pop {
            Runtime& rt;
            ~Pop() { rt.frames.pop_back(); }
        } p{rt};
        Resume res;
        res.k = mode;
        res.v = v;
        if (f->suspended == Suspend::Initial) {
            f->suspended = Suspend::None;
            res.k = Resume::None;
        }
        r = execute(rt, f, res);
    } catch (Throw&) {
        g->state = Generator::Completed;
        throw;
    }
    if (r.k == Result::Return) {
        g->state = Generator::Completed;
        return Value(rt.iter_result(r.v, true));
    }
    g->state = Generator::SuspendedYield;
    return r.v;  // an iterator result object (made by YIELD), or a re-yielded inner result
}

NATIVE(iterator_proto_iterator) {
    UNUSED_NATIVE_ARGS;
    return thisv;
}

// ------------------------------------------------------------------ $262 host hooks
NATIVE(host_eval_script) {
    UNUSED_NATIVE_ARGS;
    return rt.eval_script(rt.to_string(ARG(0))->v, "evalScript");
}
NATIVE(host_gc) {
    UNUSED_NATIVE_ARGS;
    return Value();
}
NATIVE(host_create_realm) {
    UNUSED_NATIVE_ARGS;
    rt.throw_error(E_TYPE, "createRealm is not supported");
}

Function* default_constructor(Runtime& rt, bool derived, Env* env);

void init_builtins(Runtime& rt) {
    Realm& R = rt.realm;
    R.object_proto = rt.alloc<Obj>();
    R.object_proto->proto = nullptr;
    Function* fp = rt.alloc<Function>();
    fp->kind = Kind::Native;
    fp->native = function_proto_call;
    fp->proto = R.object_proto;
    R.function_proto = fp;
    R.global = rt.alloc<Obj>();
    R.global->proto = R.object_proto;
    R.global_env = new_env(rt, nullptr);
    R.global_env->object = R.global;
    R.global_env->var_scope = true;
    R.global_env->has_this = true;
    R.global_env->this_value = Value(R.global);
    rt.define_data(fp, "length", Value(0.0), CONFIGURABLE);
    rt.define_data(fp, "name", Value(rt.intern("")), CONFIGURABLE);

    Obj* G = R.global;
    rt.define_data(G, "globalThis", Value(G), WRITABLE | CONFIGURABLE);
    rt.define_data(G, "NaN", Value(NAN), 0);
    rt.define_data(G, "Infinity", Value(INFINITY), 0);
    rt.define_data(G, "undefined", Value(), 0);

    // Object
    Function* oc = constructor(rt, "Object", object_ctor, 1, R.object_proto);
    R.object_ctor = oc;
    method(rt, oc, "assign", object_assign, 2);
    method(rt, oc, "create", object_create, 2);
    method(rt, oc, "defineProperty", object_define_property, 3);
    method(rt, oc, "defineProperties", object_define_properties, 2);
    method(rt, oc, "entries", object_keys, 1, 2);
    method(rt, oc, "freeze", object_freeze, 1, 0);
    method(rt, oc, "fromEntries", object_from_entries, 1);
    method(rt, oc, "getOwnPropertyDescriptor", object_gopd, 2);
    method(rt, oc, "getOwnPropertyDescriptors", object_gopds, 1);
    method(rt, oc, "getOwnPropertyNames", object_gopn, 1);
    method(rt, oc, "getOwnPropertySymbols", object_gops, 1);
    method(rt, oc, "getPrototypeOf", object_get_proto, 1);
    method(rt, oc, "groupBy", object_group_by, 2);
    method(rt, oc, "hasOwn", object_has_own, 2);
    method(rt, oc, "is", object_is, 2);
    method(rt, oc, "isExtensible", object_is_extensible, 1);
    method(rt, oc, "isFrozen", object_is_frozen, 1, 0);
    method(rt, oc, "isSealed", object_is_frozen, 1, 1);
    method(rt, oc, "keys", object_keys, 1, 0);
    method(rt, oc, "preventExtensions", object_freeze, 1, 2);
    method(rt, oc, "seal", object_freeze, 1, 1);
    method(rt, oc, "setPrototypeOf", object_set_proto, 2);
    method(rt, oc, "values", object_keys, 1, 1);
    Obj* OP = R.object_proto;
    method(rt, OP, "hasOwnProperty", objproto_has_own, 1);
    method(rt, OP, "isPrototypeOf", objproto_is_prototype_of, 1);
    method(rt, OP, "propertyIsEnumerable", objproto_property_is_enumerable, 1);
    method(rt, OP, "toLocaleString", objproto_to_locale_string, 0);
    method(rt, OP, "toString", objproto_to_string, 0);
    method(rt, OP, "valueOf", objproto_value_of, 0);
    method(rt, OP, "__defineGetter__", objproto_define_accessor, 2, 0);
    method(rt, OP, "__defineSetter__", objproto_define_accessor, 2, 1);
    method(rt, OP, "__lookupGetter__", objproto_lookup_accessor, 1, 0);
    method(rt, OP, "__lookupSetter__", objproto_lookup_accessor, 1, 1);
    {
        Desc d;
        d.has_get = d.has_set = d.has_enumerable = d.has_configurable = true;
        d.get = Value(rt.new_native("get __proto__", objproto_proto_get, 0));
        d.set = Value(rt.new_native("set __proto__", objproto_proto_set, 1));
        d.configurable = true;
        rt.define(OP, rt.key("__proto__"), d);
    }

    // Function
    Function* fc = constructor(rt, "Function", function_ctor, 1, fp);
    fc->native_magic = 1000;
    R.function_ctor = fc;
    method(rt, fp, "apply", fn_apply, 2);
    method(rt, fp, "bind", fn_bind, 1);
    method(rt, fp, "call", fn_call, 1);
    method(rt, fp, "toString", fn_to_string, 0);
    method(rt, fp, rt.sym_has_instance, "[Symbol.hasInstance]", fn_has_instance, 1, 0);
    R.throw_type_error = rt.new_native("", throw_type_error, 0);
    R.throw_type_error->extensible = false;
    {
        Desc d;
        d.has_configurable = true;
        rt.define(R.throw_type_error, rt.key("length"), d);
        rt.define(R.throw_type_error, rt.key("name"), d);
        d.has_get = d.has_set = d.has_enumerable = true;
        d.get = d.set = Value(R.throw_type_error);
        d.configurable = true;
        rt.define(fp, rt.key("caller"), d);
        rt.define(fp, rt.key("arguments"), d);
        Desc nd;
        nd.has_writable = nd.has_configurable = true;
        rt.define(R.throw_type_error, rt.key("length"), nd);
        rt.define(R.throw_type_error, rt.key("name"), nd);
    }

    // Error types
    const char* names[] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError", "AggregateError"};
    for (int t = 0; t < 8; t++) {
        Obj* proto = rt.new_object(t == 0 ? R.object_proto : R.error_proto);
        Function* c = constructor(rt, names[t], error_ctor, t == E_AGGREGATE ? 2 : 1, proto);
        c->native_magic = 1000 + t;
        if (t > 0) c->proto = R.errors[0];
        rt.define_data(proto, "name", Value(rt.intern(names[t])), WRITABLE | CONFIGURABLE);
        rt.define_data(proto, "message", Value(rt.intern("")), WRITABLE | CONFIGURABLE);
        if (t == 0) {
            R.error_proto = proto;
            method(rt, proto, "toString", error_to_string, 0);
        }
        R.errors[t] = c;
    }

    // Boolean
    R.boolean_proto = rt.new_object();
    R.boolean_proto->kind = Kind::BooleanBox;
    R.boolean_proto->prim = Value(false);
    constructor(rt, "Boolean", boolean_ctor, 1, R.boolean_proto);
    method(rt, R.boolean_proto, "toString", boolean_to_string, 0);
    method(rt, R.boolean_proto, "valueOf", boolean_value_of, 0);

    // Symbol
    R.symbol_proto = rt.new_object();
    Function* sc = constructor(rt, "Symbol", symbol_ctor, 0, R.symbol_proto);
    method(rt, sc, "for", symbol_for, 1);
    method(rt, sc, "keyFor", symbol_key_for, 1);
    std::pair<const char*, Sym*> wk[] = {{"iterator", rt.sym_iterator}, {"asyncIterator", rt.sym_async_iterator},
                                          {"hasInstance", rt.sym_has_instance}, {"toPrimitive", rt.sym_to_primitive},
                                          {"toStringTag", rt.sym_to_string_tag}, {"species", rt.sym_species},
                                          {"isConcatSpreadable", rt.sym_is_concat_spreadable}, {"unscopables", rt.sym_unscopables},
                                          {"match", rt.sym_match}, {"matchAll", rt.sym_match_all}, {"replace", rt.sym_replace},
                                          {"search", rt.sym_search}, {"split", rt.sym_split}};
    for (auto& w : wk) rt.define_data(sc, w.first, Value(w.second), 0);
    method(rt, R.symbol_proto, "toString", symbol_to_string, 0);
    method(rt, R.symbol_proto, "valueOf", symbol_value_of, 0);
    getter(rt, R.symbol_proto, rt.key("description"), "get description", symbol_description);
    method(rt, R.symbol_proto, rt.sym_to_primitive, "[Symbol.toPrimitive]", symbol_value_of, 1, CONFIGURABLE);
    rt.define_data(R.symbol_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("Symbol")), CONFIGURABLE);

    // Number
    R.number_proto = rt.new_object();
    R.number_proto->kind = Kind::NumberBox;
    R.number_proto->prim = Value(0.0);
    Function* nc = constructor(rt, "Number", number_ctor, 1, R.number_proto);
    rt.define_data(nc, "EPSILON", Value(2.220446049250313e-16), 0);
    rt.define_data(nc, "MAX_SAFE_INTEGER", Value(9007199254740991.0), 0);
    rt.define_data(nc, "MIN_SAFE_INTEGER", Value(-9007199254740991.0), 0);
    rt.define_data(nc, "MAX_VALUE", Value(1.7976931348623157e308), 0);
    rt.define_data(nc, "MIN_VALUE", Value(5e-324), 0);
    rt.define_data(nc, "NaN", Value(NAN), 0);
    rt.define_data(nc, "NEGATIVE_INFINITY", Value(-INFINITY), 0);
    rt.define_data(nc, "POSITIVE_INFINITY", Value(INFINITY), 0);
    method(rt, nc, "isFinite", number_is_finite, 1);
    method(rt, nc, "isInteger", number_is_integer, 1, 0);
    method(rt, nc, "isNaN", number_is_nan, 1);
    method(rt, nc, "isSafeInteger", number_is_integer, 1, 1);
    method(rt, R.number_proto, "toExponential", number_to_exponential, 1);
    method(rt, R.number_proto, "toFixed", number_to_fixed, 1);
    method(rt, R.number_proto, "toLocaleString", number_to_string, 0);
    method(rt, R.number_proto, "toPrecision", number_to_precision, 1);
    method(rt, R.number_proto, "toString", number_to_string, 1);
    method(rt, R.number_proto, "valueOf", number_value_of, 0);
    Function* pf = method(rt, G, "parseFloat", global_parse_float, 1);
    Function* pi = method(rt, G, "parseInt", global_parse_int, 2);
    rt.define_data(nc, "parseFloat", Value(pf), WRITABLE | CONFIGURABLE);
    rt.define_data(nc, "parseInt", Value(pi), WRITABLE | CONFIGURABLE);
    method(rt, G, "isNaN", global_is_nan, 1);
    method(rt, G, "isFinite", global_is_finite, 1);

    // Math
    Obj* M = rt.new_object();
    rt.define_data(G, "Math", Value(M), WRITABLE | CONFIGURABLE);
    rt.define_data(M, rt.key(rt.sym_to_string_tag), Value(rt.intern("Math")), CONFIGURABLE);
    rt.define_data(M, "E", Value(2.718281828459045), 0);
    rt.define_data(M, "LN10", Value(2.302585092994046), 0);
    rt.define_data(M, "LN2", Value(0.6931471805599453), 0);
    rt.define_data(M, "LOG10E", Value(0.4342944819032518), 0);
    rt.define_data(M, "LOG2E", Value(1.4426950408889634), 0);
    rt.define_data(M, "PI", Value(3.141592653589793), 0);
    rt.define_data(M, "SQRT1_2", Value(0.7071067811865476), 0);
    rt.define_data(M, "SQRT2", Value(1.4142135623730951), 0);
    const char* mf[] = {"abs", "acos", "acosh", "asin", "asinh", "atan", "atanh", "cbrt", "ceil", "clz32", "cos", "cosh", "exp", "expm1",
                        "floor", "fround", "log", "log1p", "log10", "log2", "round", "sign", "sin", "sinh", "sqrt", "tan", "tanh", "trunc"};
    for (int i = 0; i < 28; i++) method(rt, M, mf[i], math_fn, 1, i);
    method(rt, M, "atan2", math_atan2, 2);
    method(rt, M, "hypot", math_hypot, 2);
    method(rt, M, "imul", math_imul, 2);
    method(rt, M, "max", math_max, 2, 0);
    method(rt, M, "min", math_max, 2, 1);
    method(rt, M, "pow", math_pow, 2);
    method(rt, M, "random", math_random, 0);

    // Reflect
    Obj* Rf = rt.new_object();
    rt.define_data(G, "Reflect", Value(Rf), WRITABLE | CONFIGURABLE);
    rt.define_data(Rf, rt.key(rt.sym_to_string_tag), Value(rt.intern("Reflect")), CONFIGURABLE);
    method(rt, Rf, "apply", reflect_apply, 3);
    method(rt, Rf, "construct", reflect_construct, 2);
    method(rt, Rf, "defineProperty", reflect_define_property, 3);
    method(rt, Rf, "deleteProperty", reflect_delete_property, 2);
    method(rt, Rf, "get", reflect_get, 2);
    method(rt, Rf, "getOwnPropertyDescriptor", reflect_gopd, 2);
    method(rt, Rf, "getPrototypeOf", reflect_get_proto, 1);
    method(rt, Rf, "has", reflect_has, 2);
    method(rt, Rf, "isExtensible", reflect_is_extensible, 1);
    method(rt, Rf, "ownKeys", reflect_own_keys, 1);
    method(rt, Rf, "preventExtensions", reflect_prevent_extensions, 1);
    method(rt, Rf, "set", reflect_set, 3);
    method(rt, Rf, "setPrototypeOf", reflect_set_proto, 2);

    // globals
    rt.eval_fn = method(rt, G, "eval", global_eval, 1);
    method(rt, G, "encodeURI", global_encode, 1, 0);
    method(rt, G, "encodeURIComponent", global_encode, 1, 1);
    method(rt, G, "decodeURI", global_decode, 1, 0);
    method(rt, G, "decodeURIComponent", global_decode, 1, 1);
    method(rt, G, "escape", global_escape, 1);
    method(rt, G, "unescape", global_unescape, 1);
    method(rt, G, "print", global_print, 1);

    // iterators and generators
    R.iterator_proto = rt.new_object();
    method(rt, R.iterator_proto, rt.sym_iterator, "[Symbol.iterator]", iterator_proto_iterator, 0);
    Obj* gfp = rt.new_object(fp);  // %GeneratorFunction.prototype%
    R.generator_function_proto = gfp;
    R.generator_proto = rt.new_object(R.iterator_proto);
    rt.define_data(gfp, "prototype", Value(R.generator_proto), CONFIGURABLE);
    rt.define_data(R.generator_proto, "constructor", Value(gfp), CONFIGURABLE);
    rt.define_data(R.generator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("Generator")), CONFIGURABLE);
    rt.define_data(gfp, rt.key(rt.sym_to_string_tag), Value(rt.intern("GeneratorFunction")), CONFIGURABLE);
    method(rt, R.generator_proto, "next", generator_next, 1, 0);
    method(rt, R.generator_proto, "throw", generator_next, 1, 1);
    method(rt, R.generator_proto, "return", generator_next, 1, 2);
    Function* gfc = rt.new_native("GeneratorFunction", function_ctor, 1, 1001);
    gfc->proto = fc;
    rt.define_data(gfc, "prototype", Value(gfp), 0);
    rt.define_data(gfp, "constructor", Value(gfc), CONFIGURABLE);

    R.async_function_proto = rt.new_object(fp);
    rt.define_data(R.async_function_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("AsyncFunction")), CONFIGURABLE);
    Function* afc = rt.new_native("AsyncFunction", function_ctor, 1, 1002);
    afc->proto = fc;
    rt.define_data(afc, "prototype", Value(R.async_function_proto), 0);
    rt.define_data(R.async_function_proto, "constructor", Value(afc), CONFIGURABLE);

    R.async_generator_function_proto = rt.new_object(fp);
    rt.define_data(R.async_generator_function_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("AsyncGeneratorFunction")), CONFIGURABLE);
    Function* agfc = rt.new_native("AsyncGeneratorFunction", function_ctor, 1, 1003);
    agfc->proto = fc;
    rt.define_data(agfc, "prototype", Value(R.async_generator_function_proto), 0);
    rt.define_data(R.async_generator_function_proto, "constructor", Value(agfc), CONFIGURABLE);

    init_iterators(rt);
    init_array(rt);
    init_string(rt);
    init_json(rt);
    init_promise(rt);
    init_collections(rt);
    init_regexp(rt);
    init_date(rt);

    // $262 for test262
    Obj* h = rt.new_object();
    rt.define_data(G, "$262", Value(h), WRITABLE | CONFIGURABLE);
    rt.define_data(h, "global", Value(G), WRITABLE | CONFIGURABLE);
    method(rt, h, "evalScript", host_eval_script, 1);
    method(rt, h, "gc", host_gc, 0);
    method(rt, h, "createRealm", host_create_realm, 0);
    Obj* agent = rt.new_object();
    rt.define_data(h, "agent", Value(agent), WRITABLE | CONFIGURABLE);
}

}  // namespace kjs
