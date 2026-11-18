// Promise and the job queue, async functions, async generators, the
// async-from-sync iterator, default class constructors, Map/Set, Date.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "builtins.h"

namespace kjs {

struct PromiseObj : Obj {
    int state = 0;  // 0 pending, 1 fulfilled, 2 rejected
    Value result;
    struct Reaction {
        Value promise, resolve, reject;  // the capability (promise may be undefined)
        Value handler;
        bool reject_kind;
    };
    std::vector<Reaction> fulfill, rejectr;
    bool handled = false;
    void trace(Runtime& rt) override {
        Obj::trace(rt);
        rt.mark(result);
        for (auto* l : {&fulfill, &rejectr})
            for (auto& r : *l) {
                rt.mark(r.promise);
                rt.mark(r.resolve);
                rt.mark(r.reject);
                rt.mark(r.handler);
            }
    }
};

static Function* closure(Runtime& rt, const char* name, NativeFn fn, int len, std::initializer_list<Value> slots) {
    Function* f = rt.new_native(name, fn, len);
    f->slots.assign(slots.begin(), slots.end());
    return f;
}

static void enqueue(Runtime& rt, Value fn, std::vector<Value> args) { rt.jobs.push_back(Job{fn, std::move(args)}); }

// a reaction job: slots = promise, resolve, reject, handler, reject_kind
NATIVE(reaction_job) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Value promise = f->slots[0], resolve = f->slots[1], reject = f->slots[2], handler = f->slots[3];
    bool rej = f->slots[4].b;
    Value arg = ARG(0);
    Value result;
    bool threw = false;
    if (handler.is_undef()) {
        result = arg;
        threw = rej;
    } else {
        try {
            result = rt.call(handler, Value(), &arg, 1);
        } catch (Throw& t) {
            result = t.v;
            threw = true;
        }
    }
    if (promise.is_undef() && resolve.is_undef()) return Value();
    rt.call(threw ? reject : resolve, Value(), &result, 1);
    return Value();
}

static void trigger(Runtime& rt, std::vector<PromiseObj::Reaction>& rs, Value arg) {
    for (auto& r : rs) {
        Function* job = closure(rt, "", reaction_job, 1, {r.promise, r.resolve, r.reject, r.handler, Value(r.reject_kind)});
        enqueue(rt, Value(job), {arg});
    }
    rs.clear();
}
static void settle(Runtime& rt, PromiseObj* p, int state, Value v) {
    if (p->state != 0) return;
    auto f = std::move(p->fulfill);
    auto r = std::move(p->rejectr);
    p->state = state;
    p->result = v;
    trigger(rt, state == 1 ? f : r, v);
}

NATIVE(resolve_thenable_job) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Value then = f->slots[2], thenable = f->slots[1];
    Value resolve = f->slots[3], reject = f->slots[4];
    Value a[2] = {resolve, reject};
    try {
        rt.call(then, thenable, a, 2);
    } catch (Throw& t) {
        rt.call(reject, Value(), &t.v, 1);
    }
    return Value();
}

static void make_resolving(Runtime& rt, PromiseObj* p, Value& resolve, Value& reject);

// resolve/reject functions: slots = promise, the shared "already resolved" record
NATIVE(promise_resolve_fn) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Obj* rec = f->slots[1].o;
    if (rec->prim.b) return Value();
    rec->prim = Value(true);
    PromiseObj* p = (PromiseObj*)f->slots[0].o;
    Value v = ARG(0);
    if (v.is_obj() && v.o == p) {
        settle(rt, p, 2, Value(rt.make_error(E_TYPE, u"Chaining cycle detected for promise")));
        return Value();
    }
    if (!v.is_obj()) {
        settle(rt, p, 1, v);
        return Value();
    }
    Value then;
    try {
        then = rt.get(v, "then");
    } catch (Throw& t) {
        settle(rt, p, 2, t.v);
        return Value();
    }
    if (!rt.is_callable(then)) {
        settle(rt, p, 1, v);
        return Value();
    }
    Value res, rej;
    make_resolving(rt, p, res, rej);
    Function* job = closure(rt, "", resolve_thenable_job, 0, {Value(p), v, then, res, rej});
    enqueue(rt, Value(job), {});
    return Value();
}
NATIVE(promise_reject_fn) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Obj* rec = f->slots[1].o;
    if (rec->prim.b) return Value();
    rec->prim = Value(true);
    settle(rt, (PromiseObj*)f->slots[0].o, 2, ARG(0));
    return Value();
}
static void make_resolving(Runtime& rt, PromiseObj* p, Value& resolve, Value& reject) {
    Obj* rec = rt.new_object();
    rec->prim = Value(false);
    resolve = Value(closure(rt, "", promise_resolve_fn, 1, {Value(p), Value(rec)}));
    reject = Value(closure(rt, "", promise_reject_fn, 1, {Value(p), Value(rec)}));
}

Obj* Runtime::new_promise(Value& resolve, Value& reject) {
    PromiseObj* p = alloc<PromiseObj>();
    p->kind = Kind::Promise;
    p->proto = realm.promise_proto;
    make_resolving(*this, p, resolve, reject);
    return p;
}

// NewPromiseCapability(C): slots of the executor: [resolve, reject]
NATIVE(capability_executor) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    if (!f->slots[0].is_undef() || !f->slots[1].is_undef()) rt.throw_error(E_TYPE, "Promise executor has already been invoked");
    f->slots[0] = ARG(0);
    f->slots[1] = ARG(1);
    return Value();
}
struct Capability {
    Value promise, resolve, reject;
};
static Capability new_capability(Runtime& rt, Value c) {
    if (!rt.is_constructor(c)) rt.throw_error(E_TYPE, "Promise capability constructor is not a constructor");
    if (c.is_obj() && c.o == rt.realm.promise_ctor) {
        Capability cap;
        cap.promise = Value(rt.new_promise(cap.resolve, cap.reject));
        return cap;
    }
    Function* ex = closure(rt, "", capability_executor, 2, {Value(), Value()});
    Runtime::Root r(rt, Value(ex));
    Value p = rt.construct(c, {Value(ex)});
    if (!rt.is_callable(ex->slots[0]) || !rt.is_callable(ex->slots[1])) rt.throw_error(E_TYPE, "Promise resolve or reject function is not callable");
    return Capability{p, ex->slots[0], ex->slots[1]};
}

static bool is_promise(Value v) { return v.is_obj() && v.o->kind == Kind::Promise; }

static void perform_then(Runtime& rt, PromiseObj* p, Value on_ok, Value on_err, const Capability* cap) {
    PromiseObj::Reaction f{cap ? cap->promise : Value(), cap ? cap->resolve : Value(), cap ? cap->reject : Value(),
                           rt.is_callable(on_ok) ? on_ok : Value(), false};
    PromiseObj::Reaction r{cap ? cap->promise : Value(), cap ? cap->resolve : Value(), cap ? cap->reject : Value(),
                           rt.is_callable(on_err) ? on_err : Value(), true};
    if (p->state == 0) {
        p->fulfill.push_back(f);
        p->rejectr.push_back(r);
    } else {
        std::vector<PromiseObj::Reaction> one{p->state == 1 ? f : r};
        trigger(rt, one, p->result);
    }
    p->handled = true;
}

void Runtime::promise_then(Value p, Value on_ok, Value on_err) { perform_then(*this, (PromiseObj*)p.o, on_ok, on_err, nullptr); }

Value Runtime::promise_resolve(Value c, Value v) {
    if (is_promise(v)) {
        Value ctor = get(v, "constructor");
        if (same_value(ctor, c)) return v;
    }
    Capability cap = new_capability(*this, c);
    call(cap.resolve, Value(), &v, 1);
    return cap.promise;
}

void Runtime::run_jobs() {
    size_t i = 0;
    while (i < jobs.size()) {
        Job j = jobs[i++];
        call(j.fn, Value(), j.args);
        if (i > 4096) {
            jobs.erase(jobs.begin(), jobs.begin() + i);
            i = 0;
        }
    }
    jobs.clear();
}

NATIVE(promise_ctor) {
    UNUSED_NATIVE_ARGS;
    if (nt.is_undef()) rt.throw_error(E_TYPE, "Promise constructor cannot be invoked without 'new'");
    Value ex = ARG(0);
    if (!rt.is_callable(ex)) rt.throw_error(E_TYPE, "Promise resolver is not a function");
    Obj* proto = rt.get_prototype_from_ctor(nt, rt.realm.promise_proto);
    Value res, rej;
    PromiseObj* p = (PromiseObj*)rt.new_promise(res, rej);
    p->proto = proto;
    Runtime::Root r(rt, Value(p));
    Value a[2] = {res, rej};
    try {
        rt.call(ex, Value(), a, 2);
    } catch (Throw& t) {
        rt.call(rej, Value(), &t.v, 1);
    }
    return Value(p);
}
NATIVE(promise_then_fn) {
    UNUSED_NATIVE_ARGS;
    if (!is_promise(thisv)) rt.throw_error(E_TYPE, "Promise.prototype.then called on a non-promise");
    Obj* c = rt.species_constructor(thisv.o, rt.realm.promise_ctor);
    Capability cap = new_capability(rt, Value(c));
    perform_then(rt, (PromiseObj*)thisv.o, ARG(0), ARG(1), &cap);
    return cap.promise;
}
NATIVE(promise_catch) {
    UNUSED_NATIVE_ARGS;
    return rt.invoke(thisv, rt.key("then"), {Value(), ARG(0)});
}
// finally helpers: slots = onFinally, C ; magic 0 then-finally, 1 catch-finally
NATIVE(finally_value_thunk) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    if (f->native_magic == 1) throw Throw{f->slots[0]};
    return f->slots[0];
}
NATIVE(finally_reaction) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Value r = rt.call(f->slots[0], Value(), nullptr, 0);
    Value p = rt.promise_resolve(f->slots[1], r);
    Function* thunk = closure(rt, "", finally_value_thunk, 0, {ARG(0)});
    thunk->native_magic = f->native_magic;
    return rt.invoke(p, rt.key("then"), {Value(thunk)});
}
NATIVE(promise_finally) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "Promise.prototype.finally called on a non-object");
    Obj* c = rt.species_constructor(thisv.o, rt.realm.promise_ctor);
    Value on = ARG(0);
    if (!rt.is_callable(on)) return rt.invoke(thisv, rt.key("then"), {on, on});
    Function* a = closure(rt, "", finally_reaction, 1, {on, Value(c)});
    Function* b = closure(rt, "", finally_reaction, 1, {on, Value(c)});
    b->native_magic = 1;
    return rt.invoke(thisv, rt.key("then"), {Value(a), Value(b)});
}
NATIVE(promise_resolve_static) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "Promise.resolve called on a non-object");
    return rt.promise_resolve(thisv, ARG(0));
}
NATIVE(promise_reject_static) {
    UNUSED_NATIVE_ARGS;
    Capability cap = new_capability(rt, thisv);
    Value v = ARG(0);
    rt.call(cap.reject, Value(), &v, 1);
    return cap.promise;
}
NATIVE(promise_with_resolvers) {
    UNUSED_NATIVE_ARGS;
    Capability cap = new_capability(rt, thisv);
    Obj* o = rt.new_object();
    create_data(rt, o, rt.key("promise"), cap.promise);
    create_data(rt, o, rt.key("resolve"), cap.resolve);
    create_data(rt, o, rt.key("reject"), cap.reject);
    return Value(o);
}
// Promise.all family. element functions: slots = values array, index, counter record, resolve, kind, called flag record
NATIVE(promise_all_element) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Obj* called = f->slots[5].o;
    if (called->prim.b) return Value();
    called->prim = Value(true);
    int kind = int(f->slots[4].n);  // 0 all, 1 allSettled fulfilled, 2 allSettled rejected, 3 any rejected
    Obj* values = f->slots[0].o;
    double idx = f->slots[1].n;
    Obj* counter = f->slots[2].o;
    Value x = ARG(0);
    if (kind == 1 || kind == 2) {
        Obj* o = rt.new_object();
        create_data(rt, o, rt.key("status"), Value(rt.intern(kind == 1 ? "fulfilled" : "rejected")));
        create_data(rt, o, rt.key(kind == 1 ? "value" : "reason"), x);
        x = Value(o);
    }
    create_data(rt, values, idx_key(rt, idx), x);
    counter->prim = Value(counter->prim.n - 1);
    if (counter->prim.n == 0) {
        if (kind == 3) {
            Obj* err = rt.make_error(E_AGGREGATE, u"All promises were rejected");
            rt.define_data(err, "errors", Value(rt.array_from_list(values->elems)), WRITABLE | CONFIGURABLE);
            Value e(err);
            rt.call(f->slots[3], Value(), &e, 1);
        } else {
            Value v(values);
            rt.call(f->slots[3], Value(), &v, 1);
        }
    }
    return Value();
}
// magic 0 all, 1 allSettled, 2 any, 3 race
NATIVE(promise_combinator) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Value C = thisv;
    Capability cap = new_capability(rt, C);
    Runtime::Root r1(rt, cap.promise);
    Runtime::Root r2(rt, cap.resolve);
    Runtime::Root r3(rt, cap.reject);
    Value resolve_fn;
    Runtime::Iter it;
    bool have_iter = false;
    try {
        resolve_fn = rt.get(C, "resolve");
        if (!rt.is_callable(resolve_fn)) rt.throw_error(E_TYPE, "resolve is not callable");
        it = rt.get_iterator(ARG(0));
        have_iter = true;
        Obj* values = rt.new_array(0);
        Runtime::Root r4(rt, Value(values));
        Obj* counter = rt.new_object();
        counter->prim = Value(1.0);
        double index = 0;
        while (true) {
            Value next;
            bool more;
            try {
                more = rt.iter_step(it, next);
            } catch (Throw&) {
                it.done = true;
                throw;
            }
            if (!more) break;
            if (m != 3) {
                values->elems.push_back(Value());
                values->length = uint32_t(values->elems.size());
            }
            Value p = rt.call(resolve_fn, C, &next, 1);
            Value on_ok, on_err;
            if (m == 3) {
                on_ok = cap.resolve;
                on_err = cap.reject;
            } else {
                auto mk = [&](int kind, Value target) {
                    Obj* called = rt.new_object();
                    called->prim = Value(false);
                    return Value(closure(rt, "", promise_all_element, 1, {Value(values), Value(index), Value(counter), target, Value(double(kind)), Value(called)}));
                };
                if (m == 0) {
                    on_ok = mk(0, cap.resolve);
                    on_err = cap.reject;
                } else if (m == 1) {
                    Obj* called = rt.new_object();
                    called->prim = Value(false);
                    on_ok = Value(closure(rt, "", promise_all_element, 1, {Value(values), Value(index), Value(counter), cap.resolve, Value(1.0), Value(called)}));
                    on_err = Value(closure(rt, "", promise_all_element, 1, {Value(values), Value(index), Value(counter), cap.resolve, Value(2.0), Value(called)}));
                } else {
                    on_ok = cap.resolve;
                    on_err = mk(3, cap.reject);
                }
                counter->prim = Value(counter->prim.n + 1);
            }
            rt.invoke(p, rt.key("then"), {on_ok, on_err});
            index++;
        }
        if (m != 3) {
            counter->prim = Value(counter->prim.n - 1);
            if (counter->prim.n == 0) {
                if (m == 2) {
                    Obj* err = rt.make_error(E_AGGREGATE, u"All promises were rejected");
                    rt.define_data(err, "errors", Value(rt.array_from_list(values->elems)), WRITABLE | CONFIGURABLE);
                    Value e(err);
                    rt.call(cap.reject, Value(), &e, 1);
                } else {
                    Value v(values);
                    rt.call(cap.resolve, Value(), &v, 1);
                }
            }
        }
    } catch (Throw& t) {
        if (have_iter && !it.done) rt.iter_close(it, true);
        Value e = t.v;
        rt.call(cap.reject, Value(), &e, 1);
    }
    return cap.promise;
}
NATIVE(get_species) {
    UNUSED_NATIVE_ARGS;
    return thisv;
}

// ------------------------------------------------------------------ async functions
static void async_step(Runtime& rt, Frame* f, Resume r);
// slots: frame (as Empty-tagged cell); magic 0 fulfilled, 1 rejected
NATIVE(async_resume) {
    UNUSED_NATIVE_ARGS;
    Function* fn = (Function*)callee;
    Frame* f = (Frame*)fn->slots[0].c;
    Resume r;
    r.k = fn->native_magic == 0 ? Resume::Next : Resume::Throw;
    r.v = ARG(0);
    async_step(rt, f, r);
    return Value();
}
static Value frame_value(Frame* f) {
    Value v = Value::empty();
    v.c = f;
    return v;
}
// awaits v, resuming the frame through `cont` (a function that receives the frame back)
static void await_value(Runtime& rt, Frame* f, Value v, NativeFn resume) {
    Value p;
    try {
        p = rt.promise_resolve(Value(rt.realm.promise_ctor), v);
    } catch (Throw& t) {
        Function* rej = closure(rt, "", resume, 1, {frame_value(f)});
        rej->native_magic = 1;
        enqueue(rt, Value(rej), {t.v});
        return;
    }
    Function* ok = closure(rt, "", resume, 1, {frame_value(f)});
    Function* err = closure(rt, "", resume, 1, {frame_value(f)});
    err->native_magic = 1;
    rt.promise_then(p, Value(ok), Value(err));
}
static void async_step(Runtime& rt, Frame* f, Resume r) {
    Value resolve = f->stack.empty() ? Value() : Value();
    (void)resolve;
    Result res;
    // the capability lives in the frame's completion slot: [promise, resolve, reject] array
    Obj* cap = f->completion.o;
    try {
        rt.frames.push_back(f);
        struct Pop {
            Runtime& rt;
            ~Pop() { rt.frames.pop_back(); }
        } p{rt};
        res = execute(rt, f, r);
    } catch (Throw& t) {
        rt.call(cap->elems[2], Value(), &t.v, 1);
        return;
    }
    if (res.k == Result::Return) {
        rt.call(cap->elems[1], Value(), &res.v, 1);
        return;
    }
    await_value(rt, f, res.v, async_resume);
}
Value start_async_function(Runtime& rt, Frame* f) {
    Value res, rej;
    Obj* p = rt.new_promise(res, rej);
    Obj* cap = rt.array_from_list({Value(p), res, rej});
    // async function frames do not use the completion slot otherwise
    f->completion = Value(cap);
    Runtime::Root root(rt, Value(cap));
    async_step(rt, f, Resume{});
    return Value(p);
}

// ------------------------------------------------------------------ async generators
static void agen_resume_next(Runtime& rt, Generator* g);
static void agen_complete(Runtime& rt, Generator* g, bool threw, Value v, bool done) {
    Generator::Req q = g->queue.front();
    g->queue.erase(g->queue.begin());
    if (threw) rt.call(q.reject, Value(), &v, 1);
    else {
        Value r(rt.iter_result(v, done));
        rt.call(q.resolve, Value(), &r, 1);
    }
}
static void agen_run(Runtime& rt, Generator* g, Resume r);
NATIVE(agen_await_resume) {
    UNUSED_NATIVE_ARGS;
    Function* fn = (Function*)callee;
    Frame* f = (Frame*)fn->slots[0].c;
    Resume r;
    r.k = fn->native_magic == 0 ? Resume::Next : Resume::Throw;
    r.v = ARG(0);
    agen_run(rt, (Generator*)f->generator, r);
    return Value();
}
static void agen_run(Runtime& rt, Generator* g, Resume r) {
    Frame* f = g->frame;
    Result res;
    try {
        rt.frames.push_back(f);
        struct Pop {
            Runtime& rt;
            ~Pop() { rt.frames.pop_back(); }
        } p{rt};
        if (f->suspended == Suspend::Initial) {
            f->suspended = Suspend::None;
            r = Resume{};
        }
        res = execute(rt, f, r);
    } catch (Throw& t) {
        g->state = Generator::Completed;
        agen_complete(rt, g, true, t.v, true);
        agen_resume_next(rt, g);
        return;
    }
    if (res.k == Result::Await) {
        await_value(rt, f, res.v, agen_await_resume);
        return;
    }
    if (res.k == Result::Return) {
        g->state = Generator::Completed;
        agen_complete(rt, g, false, res.v, true);
    } else {
        g->state = Generator::SuspendedYield;
        agen_complete(rt, g, false, res.v, false);
    }
    agen_resume_next(rt, g);
}
// return(v) on a completed generator: slots = generator; magic 0 fulfilled 1 rejected
NATIVE(agen_return_settled) {
    UNUSED_NATIVE_ARGS;
    Function* fn = (Function*)callee;
    Generator* g = (Generator*)fn->slots[0].o;
    g->awaiting_return = false;
    agen_complete(rt, g, fn->native_magic == 1, ARG(0), true);
    agen_resume_next(rt, g);
    return Value();
}
static void agen_resume_next(Runtime& rt, Generator* g) {
    while (true) {
        if (g->state == Generator::Executing || g->awaiting_return) return;
        if (g->queue.empty()) return;
        Generator::Req& q = g->queue.front();
        if (q.mode != Resume::Next) {
            if (g->state == Generator::SuspendedStart) g->state = Generator::Completed;
            if (g->state == Generator::Completed) {
                if (q.mode == Resume::Return) {
                    g->awaiting_return = true;
                    Value p;
                    try {
                        p = rt.promise_resolve(Value(rt.realm.promise_ctor), q.value);
                    } catch (Throw& t) {
                        g->awaiting_return = false;
                        agen_complete(rt, g, true, t.v, true);
                        continue;
                    }
                    Function* ok = closure(rt, "", agen_return_settled, 1, {Value((Obj*)g)});
                    Function* err = closure(rt, "", agen_return_settled, 1, {Value((Obj*)g)});
                    err->native_magic = 1;
                    rt.promise_then(p, Value(ok), Value(err));
                    return;
                }
                agen_complete(rt, g, true, q.value, true);
                continue;
            }
        } else if (g->state == Generator::Completed) {
            agen_complete(rt, g, false, Value(), true);
            continue;
        }
        g->state = Generator::Executing;
        Resume r;
        r.k = q.mode;
        r.v = q.value;
        agen_run(rt, g, r);
        return;
    }
}
// magic 0 next 1 throw 2 return
NATIVE(agen_method) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Value res, rej;
    Obj* p = rt.new_promise(res, rej);
    if (!thisv.is_obj() || thisv.o->kind != Kind::AsyncGenerator) {
        Value e(rt.make_error(E_TYPE, u"AsyncGenerator method called on incompatible receiver"));
        rt.call(rej, Value(), &e, 1);
        return Value(p);
    }
    Generator* g = (Generator*)thisv.o;
    Generator::Req q;
    q.mode = m == 0 ? Resume::Next : m == 1 ? Resume::Throw : Resume::Return;
    q.value = ARG(0);
    q.promise = Value(p);
    q.resolve = res;
    q.reject = rej;
    g->queue.push_back(q);
    if (g->state != Generator::Executing) agen_resume_next(rt, g);
    return Value(p);
}

// ------------------------------------------------------------------ async-from-sync iterator
// the wrapper keeps the sync iterator and next in elems[0..1]
NATIVE(afs_unwrap) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    return Value(rt.iter_result(ARG(0), f->slots[0].b));
}
NATIVE(afs_close_then_throw) {
    UNUSED_NATIVE_ARGS;
    Function* f = (Function*)callee;
    Runtime::Iter it{f->slots[0], Value(), false};
    rt.iter_close(it, true);
    throw Throw{ARG(0)};
}
static Value afs_continue(Runtime& rt, Value result, Capability& cap, Value sync_iter, bool close_on_reject) {
    bool done = rt.to_boolean(rt.get(result, "done"));
    Value value = rt.get(result, "value");
    Value wrapper;
    try {
        wrapper = rt.promise_resolve(Value(rt.realm.promise_ctor), value);
    } catch (Throw&) {
        if (!done && close_on_reject) {
            Runtime::Iter it{sync_iter, Value(), false};
            rt.iter_close(it, true);
        }
        throw;
    }
    Function* unwrap = closure(rt, "", afs_unwrap, 1, {Value(done)});
    Value on_err;
    if (!done && close_on_reject) on_err = Value(closure(rt, "", afs_close_then_throw, 1, {sync_iter}));
    perform_then(rt, (PromiseObj*)wrapper.o, Value(unwrap), on_err, &cap);
    return cap.promise;
}
// magic 0 next 1 return 2 throw
NATIVE(afs_method) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Obj* w = thisv.o;
    Value iter = w->elems[0], next = w->elems[1];
    Capability cap;
    cap.promise = Value(rt.new_promise(cap.resolve, cap.reject));
    try {
        Value result;
        if (m == 0) {
            result = argc ? rt.call(next, iter, args, 1) : rt.call(next, iter, nullptr, 0);
        } else {
            Value method = rt.get_method(iter, rt.key(m == 1 ? "return" : "throw"));
            if (method.is_undef()) {
                if (m == 1) {
                    Value r(rt.iter_result(ARG(0), true));
                    rt.call(cap.resolve, Value(), &r, 1);
                    return cap.promise;
                }
                // throw without a throw method: close the iterator, then reject with a TypeError
                Runtime::Iter it{iter, next, false};
                rt.iter_close(it, false);
                rt.throw_error(E_TYPE, "The iterator does not provide a 'throw' method");
            }
            result = argc ? rt.call(method, iter, args, 1) : rt.call(method, iter, nullptr, 0);
        }
        if (!result.is_obj()) rt.throw_error(E_TYPE, "Iterator result is not an object");
        return afs_continue(rt, result, cap, iter, m != 1);
    } catch (Throw& t) {
        rt.call(cap.reject, Value(), &t.v, 1);
        return cap.promise;
    }
}
Value make_async_from_sync(Runtime& rt, Value iterator, Value next) {
    Obj* w = rt.new_object(rt.realm.async_from_sync_iter_proto);
    w->elems = {iterator, next};
    return Value(w);
}

// ------------------------------------------------------------------ default constructors
NATIVE(default_ctor) {
    UNUSED_NATIVE_ARGS;
    Function* F = (Function*)callee;
    if (nt.is_undef()) rt.throw_error(E_TYPE, "Class constructor cannot be invoked without 'new'");
    if (F->native_magic == 1001) {
        Obj* parent = rt.get_proto(F);
        if (!parent || !rt.is_constructor(Value(parent))) rt.throw_error(E_TYPE, "Super constructor is not a constructor");
        Value r = rt.construct(Value(parent), std::vector<Value>(args, args + argc), nt);
        initialize_instance(rt, r.o, F);
        return r;
    }
    Obj* o = rt.new_object(rt.get_prototype_from_ctor(nt, rt.realm.object_proto));
    Runtime::Root r(rt, Value(o));
    initialize_instance(rt, o, F);
    return Value(o);
}
Function* default_constructor(Runtime& rt, bool derived, Env* env) {
    (void)env;
    Function* f = rt.new_native("", default_ctor, 0, derived ? 1001 : 1000);
    f->is_class_constructor = true;
    return f;
}

// ------------------------------------------------------------------ Map, Set, WeakMap, WeakSet
struct ValueHash {
    size_t operator()(const Value& v) const {
        switch (v.tag) {
            case Tag::Number: {
                double d = v.n == 0 ? 0 : v.n;
                if (std::isnan(d)) return 0x7ff8;
                uint64_t b;
                memcpy(&b, &d, 8);
                return std::hash<uint64_t>()(b);
            }
            case Tag::String: return std::hash<u16>()(v.s->v);
            case Tag::Bool: return v.b;
            case Tag::Undefined: return 1;
            case Tag::Null: return 2;
            default: return std::hash<void*>()(v.c);
        }
    }
};
struct ValueEq {
    bool operator()(const Value& a, const Value& b) const {
        if (a.is_num() && b.is_num()) return (std::isnan(a.n) && std::isnan(b.n)) || a.n == b.n;
        if (a.tag != b.tag) return false;
        if (a.is_str()) return a.s->v == b.s->v;
        if (a.is_bool()) return a.b == b.b;
        if (a.is_nullish()) return true;
        return a.c == b.c;
    }
};
struct MapObj : Obj {
    std::vector<std::pair<Value, Value>> entries;  // deleted: key Empty
    std::unordered_map<Value, size_t, ValueHash, ValueEq> index;
    size_t live = 0;
    void trace(Runtime& rt) override {
        Obj::trace(rt);
        for (auto& e : entries) {
            rt.mark(e.first);
            rt.mark(e.second);
        }
    }
};
static MapObj* this_map(Runtime& rt, Value v, Kind k, const char* m) {
    if (!v.is_obj() || v.o->kind != k) rt.throw_error(E_TYPE, std::string(m) + " called on incompatible receiver");
    return (MapObj*)v.o;
}
static Value norm(Value k) {
    if (k.is_num() && k.n == 0) return Value(0.0);
    return k;
}
static bool can_be_held_weakly(Value v) { return v.is_obj() || (v.is_sym() && !v.y->registered); }
// collection constructors: magic 1000 Map, 1001 Set, 1002 WeakMap, 1003 WeakSet
NATIVE(collection_ctor) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic - 1000;
    static const char* names[] = {"Map", "Set", "WeakMap", "WeakSet"};
    if (nt.is_undef()) rt.throw_error(E_TYPE, std::string("Constructor ") + names[m] + " requires 'new'");
    Obj* protos[] = {rt.realm.map_proto, rt.realm.set_proto, rt.realm.weakmap_proto, rt.realm.weakset_proto};
    Kind kinds[] = {Kind::Map, Kind::Set, Kind::WeakMap, Kind::WeakSet};
    MapObj* o = rt.alloc<MapObj>();
    o->kind = kinds[m];
    o->proto = rt.get_prototype_from_ctor(nt, protos[m]);
    Runtime::Root r(rt, Value((Obj*)o));
    Value it = ARG(0);
    if (it.is_nullish()) return Value((Obj*)o);
    bool is_map = m == 0 || m == 2;
    Value adder = rt.get(Value((Obj*)o), is_map ? "set" : "add");
    if (!rt.is_callable(adder)) rt.throw_error(E_TYPE, "adder is not callable");
    Runtime::Iter iter = rt.get_iterator(it);
    Value v;
    while (rt.iter_step(iter, v)) {
        try {
            if (is_map) {
                if (!v.is_obj()) rt.throw_error(E_TYPE, "Iterator value is not an entry object");
                Value k = rt.get(v, rt.key("0"));
                Value val = rt.get(v, rt.key("1"));
                Value a[2] = {k, val};
                rt.call(adder, Value((Obj*)o), a, 2);
            } else {
                rt.call(adder, Value((Obj*)o), &v, 1);
            }
        } catch (Throw&) {
            rt.iter_close(iter, true);
            throw;
        }
    }
    return Value((Obj*)o);
}
// map methods: magic encodes the kind: 0 Map 1 Set 2 WeakMap 3 WeakSet
static Kind KINDS[] = {Kind::Map, Kind::Set, Kind::WeakMap, Kind::WeakSet};
NATIVE(map_get) {
    UNUSED_NATIVE_ARGS;
    MapObj* m = this_map(rt, thisv, KINDS[((Function*)callee)->native_magic], "get");
    auto it = m->index.find(norm(ARG(0)));
    return it == m->index.end() ? Value() : m->entries[it->second].second;
}
NATIVE(map_has) {
    UNUSED_NATIVE_ARGS;
    MapObj* m = this_map(rt, thisv, KINDS[((Function*)callee)->native_magic], "has");
    return Value(m->index.count(norm(ARG(0))) > 0);
}
NATIVE(map_set) {
    UNUSED_NATIVE_ARGS;
    int kind = ((Function*)callee)->native_magic;
    MapObj* m = this_map(rt, thisv, KINDS[kind], "set");
    Value k = norm(ARG(0));
    if ((kind == 2 || kind == 3) && !can_be_held_weakly(k)) rt.throw_error(E_TYPE, "Invalid value used as weak map key");
    Value v = (kind == 1 || kind == 3) ? k : ARG(1);
    auto it = m->index.find(k);
    if (it != m->index.end()) m->entries[it->second].second = v;
    else {
        m->index[k] = m->entries.size();
        m->entries.push_back({k, v});
        m->live++;
    }
    return thisv;
}
NATIVE(map_delete) {
    UNUSED_NATIVE_ARGS;
    MapObj* m = this_map(rt, thisv, KINDS[((Function*)callee)->native_magic], "delete");
    auto it = m->index.find(norm(ARG(0)));
    if (it == m->index.end()) return Value(false);
    m->entries[it->second] = {Value::empty(), Value()};
    m->index.erase(it);
    m->live--;
    return Value(true);
}
NATIVE(map_clear) {
    UNUSED_NATIVE_ARGS;
    MapObj* m = this_map(rt, thisv, KINDS[((Function*)callee)->native_magic], "clear");
    for (auto& e : m->entries) e = {Value::empty(), Value()};
    m->index.clear();
    m->live = 0;
    return Value();
}
NATIVE(map_size) {
    UNUSED_NATIVE_ARGS;
    MapObj* m = this_map(rt, thisv, KINDS[((Function*)callee)->native_magic], "size");
    return Value(double(m->live));
}
NATIVE(map_for_each) {
    UNUSED_NATIVE_ARGS;
    int kind = ((Function*)callee)->native_magic;
    MapObj* m = this_map(rt, thisv, KINDS[kind], "forEach");
    if (!rt.is_callable(ARG(0))) rt.throw_error(E_TYPE, "callback is not a function");
    for (size_t i = 0; i < m->entries.size(); i++) {
        auto e = m->entries[i];
        if (e.first.is_empty()) continue;
        Value a[3] = {e.second, e.first, thisv};
        rt.call(ARG(0), ARG(1), a, 3);
    }
    return Value();
}
// iterators: elems[0] = map, prim = position, length = kind (0 keys 1 values 2 entries) + 10*is_set
NATIVE(map_iter) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    bool is_set = m >= 10;
    MapObj* mo = this_map(rt, thisv, is_set ? Kind::Set : Kind::Map, "iterator");
    Obj* it = rt.new_object(is_set ? rt.realm.set_iterator_proto : rt.realm.map_iterator_proto);
    it->kind = Kind::ArrayIterator;
    it->elems.push_back(Value((Obj*)mo));
    it->prim = Value(0.0);
    it->length = uint32_t(100 + m);
    return Value(it);
}
NATIVE(map_iter_next) {
    UNUSED_NATIVE_ARGS;
    bool want_set = ((Function*)callee)->native_magic == 1;
    if (!thisv.is_obj() || thisv.o->kind != Kind::ArrayIterator || thisv.o->length < 100 || ((thisv.o->length >= 110) != want_set))
        rt.throw_error(E_TYPE, "next called on incompatible receiver");
    Obj* it = thisv.o;
    if (it->elems[0].is_undef()) return Value(rt.iter_result(Value(), true));
    MapObj* m = (MapObj*)it->elems[0].o;
    int kind = int(it->length - 100) % 10;
    size_t i = size_t(it->prim.n);
    while (i < m->entries.size() && m->entries[i].first.is_empty()) i++;
    if (i >= m->entries.size()) {
        it->elems[0] = Value();
        return Value(rt.iter_result(Value(), true));
    }
    it->prim = Value(double(i + 1));
    auto& e = m->entries[i];
    if (kind == 0) return Value(rt.iter_result(e.first, false));
    if (kind == 1) return Value(rt.iter_result(e.second, false));
    return Value(rt.iter_result(Value(rt.array_from_list({e.first, e.second})), false));
}
NATIVE(map_group_by) {
    UNUSED_NATIVE_ARGS;
    Value items = ARG(0), cb = ARG(1);
    if (items.is_nullish()) rt.throw_error(E_TYPE, "groupBy requires an iterable");
    if (!rt.is_callable(cb)) rt.throw_error(E_TYPE, "callback is not a function");
    MapObj* o = rt.alloc<MapObj>();
    o->kind = Kind::Map;
    o->proto = rt.realm.map_proto;
    Runtime::Root r(rt, Value((Obj*)o));
    Runtime::Iter it = rt.get_iterator(items);
    Value v;
    double k = 0;
    while (rt.iter_step(it, v)) {
        Value key;
        try {
            Value a[2] = {v, Value(k++)};
            key = norm(rt.call(cb, Value(), a, 2));
        } catch (Throw&) {
            rt.iter_close(it, true);
            throw;
        }
        auto f = o->index.find(key);
        Obj* g;
        if (f == o->index.end()) {
            g = rt.new_array(0);
            o->index[key] = o->entries.size();
            o->entries.push_back({key, Value(g)});
            o->live++;
        } else {
            g = o->entries[f->second].second.o;
        }
        g->elems.push_back(v);
        g->length = uint32_t(g->elems.size());
    }
    return Value((Obj*)o);
}

void init_collections(Runtime& rt) {
    Realm& R = rt.realm;
    const char* names[] = {"Map", "Set", "WeakMap", "WeakSet"};
    Obj** protos[] = {&R.map_proto, &R.set_proto, &R.weakmap_proto, &R.weakset_proto};
    for (int m = 0; m < 4; m++) {
        Obj* P = rt.new_object();
        *protos[m] = P;
        Function* c = constructor(rt, names[m], collection_ctor, 0, P);
        c->native_magic = 1000 + m;
        if (m < 2) getter(rt, c, rt.key(rt.sym_species), "get [Symbol.species]", get_species);
        bool is_map = m == 0 || m == 2;
        if (is_map) {
            method(rt, P, "get", map_get, 1, m);
            method(rt, P, "set", map_set, 2, m);
        } else {
            method(rt, P, "add", map_set, 1, m);
        }
        method(rt, P, "has", map_has, 1, m);
        method(rt, P, "delete", map_delete, 1, m);
        if (m < 2) {
            method(rt, P, "clear", map_clear, 0, m);
            method(rt, P, "forEach", map_for_each, 1, m);
            Function* sz = rt.new_native("get size", map_size, 0, m);
            Desc d;
            d.has_get = d.has_set = d.has_enumerable = d.has_configurable = true;
            d.get = Value(sz);
            d.configurable = true;
            rt.define(P, rt.key("size"), d);
            int base = m == 1 ? 10 : 0;
            Function* entries = method(rt, P, "entries", map_iter, 0, base + 2);
            Function* values = method(rt, P, "values", map_iter, 0, base + 1);
            if (m == 0) {
                method(rt, P, "keys", map_iter, 0, base + 0);
                rt.define_data(P, rt.key(rt.sym_iterator), Value(entries), WRITABLE | CONFIGURABLE);
            } else {
                rt.define_data(P, "keys", Value(values), WRITABLE | CONFIGURABLE);
                rt.define_data(P, rt.key(rt.sym_iterator), Value(values), WRITABLE | CONFIGURABLE);
            }
            if (m == 0) method(rt, c, "groupBy", map_group_by, 2);
        }
        rt.define_data(P, rt.key(rt.sym_to_string_tag), Value(rt.intern(names[m])), CONFIGURABLE);
    }
    R.map_iterator_proto = rt.new_object(R.iterator_proto);
    method(rt, R.map_iterator_proto, "next", map_iter_next, 0, 0);
    rt.define_data(R.map_iterator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("Map Iterator")), CONFIGURABLE);
    R.set_iterator_proto = rt.new_object(R.iterator_proto);
    method(rt, R.set_iterator_proto, "next", map_iter_next, 0, 1);
    rt.define_data(R.set_iterator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("Set Iterator")), CONFIGURABLE);
}

void init_promise(Runtime& rt) {
    Realm& R = rt.realm;
    R.promise_proto = rt.new_object();
    Function* pc = constructor(rt, "Promise", promise_ctor, 1, R.promise_proto);
    R.promise_ctor = pc;
    method(rt, pc, "all", promise_combinator, 1, 0);
    method(rt, pc, "allSettled", promise_combinator, 1, 1);
    method(rt, pc, "any", promise_combinator, 1, 2);
    method(rt, pc, "race", promise_combinator, 1, 3);
    method(rt, pc, "reject", promise_reject_static, 1);
    method(rt, pc, "resolve", promise_resolve_static, 1);
    method(rt, pc, "withResolvers", promise_with_resolvers, 0);
    getter(rt, pc, rt.key(rt.sym_species), "get [Symbol.species]", get_species);
    method(rt, R.promise_proto, "catch", promise_catch, 1);
    method(rt, R.promise_proto, "finally", promise_finally, 1);
    method(rt, R.promise_proto, "then", promise_then_fn, 2);
    rt.define_data(R.promise_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("Promise")), CONFIGURABLE);

    // async generators
    Obj* async_iter_proto = rt.new_object();
    method(rt, async_iter_proto, rt.sym_async_iterator, "[Symbol.asyncIterator]",
           [](Runtime&, Value thisv, const Value*, int, Obj*, Value) { return thisv; }, 0);
    R.async_generator_proto = rt.new_object(async_iter_proto);
    rt.define_data(R.async_generator_function_proto, "prototype", Value(R.async_generator_proto), CONFIGURABLE);
    rt.define_data(R.async_generator_proto, "constructor", Value(R.async_generator_function_proto), CONFIGURABLE);
    rt.define_data(R.async_generator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("AsyncGenerator")), CONFIGURABLE);
    method(rt, R.async_generator_proto, "next", agen_method, 1, 0);
    method(rt, R.async_generator_proto, "throw", agen_method, 1, 1);
    method(rt, R.async_generator_proto, "return", agen_method, 1, 2);
    R.async_from_sync_iter_proto = rt.new_object(async_iter_proto);
    method(rt, R.async_from_sync_iter_proto, "next", afs_method, 1, 0);
    method(rt, R.async_from_sync_iter_proto, "return", afs_method, 1, 1);
    method(rt, R.async_from_sync_iter_proto, "throw", afs_method, 1, 2);
}

// ------------------------------------------------------------------ Date
static double day(double t) { return std::floor(t / 86400000.0); }
static double days_in_year(double y) {
    if (std::fmod(y, 4) != 0) return 365;
    if (std::fmod(y, 100) != 0) return 366;
    if (std::fmod(y, 400) != 0) return 365;
    return 366;
}
static double day_from_year(double y) {
    return 365 * (y - 1970) + std::floor((y - 1969) / 4) - std::floor((y - 1901) / 100) + std::floor((y - 1601) / 400);
}
static double year_from_time(double t) {
    double y = std::floor(t / (86400000.0 * 365.2425)) + 1970;
    while (day_from_year(y) * 86400000.0 > t) y--;
    while (day_from_year(y + 1) * 86400000.0 <= t) y++;
    return y;
}
static int month_from_time(double t, int* date) {
    double y = year_from_time(t);
    int d = int(day(t) - day_from_year(y));
    bool leap = days_in_year(y) == 366;
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
    for (int m = 0; m < 12; m++) {
        int start = cum[m] + (m >= 2 && leap ? 1 : 0);
        int end = cum[m + 1] + (m + 1 >= 2 && leap ? 1 : 0);
        if (d < end) {
            *date = d - start + 1;
            return m;
        }
    }
    *date = 1;
    return 11;
}
static double make_day(double y, double m, double d) {
    if (!std::isfinite(y) || !std::isfinite(m) || !std::isfinite(d)) return NAN;
    y = std::trunc(y);
    m = std::trunc(m);
    d = std::trunc(d);
    double ym = y + std::floor(m / 12);
    double mn = std::fmod(m, 12);
    if (mn < 0) mn += 12;
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (std::fabs(ym) > 400000) return NAN;
    double dd = day_from_year(ym) + cum[int(mn)] + (mn >= 2 && days_in_year(ym) == 366 ? 1 : 0);
    return dd + d - 1;
}
static double make_time(double h, double m, double s, double ms) {
    if (!std::isfinite(h) || !std::isfinite(m) || !std::isfinite(s) || !std::isfinite(ms)) return NAN;
    return std::trunc(h) * 3600000.0 + std::trunc(m) * 60000.0 + std::trunc(s) * 1000.0 + std::trunc(ms);
}
static double make_date(double d, double t) {
    if (!std::isfinite(d) || !std::isfinite(t)) return NAN;
    return d * 86400000.0 + t;
}
static double time_clip(double t) {
    if (!std::isfinite(t) || std::fabs(t) > 8.64e15) return NAN;
    return std::trunc(t) + 0.0;
}
static double now_ms() {
    return double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
static double parse_date(const u16& s16) {
    std::string s = to_utf8(s16);
    int y = 0, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0;
    char sign = 0;
    size_t i = 0;
    auto num = [&](int n, int& out) {
        out = 0;
        for (int k = 0; k < n; k++) {
            if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
            out = out * 10 + (s[i++] - '0');
        }
        return true;
    };
    // the date time string format: YYYY-MM-DDTHH:mm:ss.sssZ (and the expanded years)
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        sign = s[i++];
        if (!num(6, y)) return NAN;
        if (sign == '-' && y == 0) return NAN;
        if (sign == '-') y = -y;
    } else if (!num(4, y)) {
        goto other;
    }
    {
        bool date_only = true;
        if (i < s.size() && s[i] == '-') {
            i++;
            if (!num(2, mo)) return NAN;
            if (i < s.size() && s[i] == '-') {
                i++;
                if (!num(2, d)) return NAN;
            }
        }
        double tz = 0;
        bool has_tz = false;
        if (i < s.size() && s[i] == 'T') {
            date_only = false;
            i++;
            if (!num(2, h) || i >= s.size() || s[i++] != ':' || !num(2, mi)) return NAN;
            if (i < s.size() && s[i] == ':') {
                i++;
                if (!num(2, sec)) return NAN;
                if (i < s.size() && s[i] == '.') {
                    i++;
                    int digits = 0;
                    ms = 0;
                    while (i < s.size() && isdigit((unsigned char)s[i])) {
                        if (digits < 3) ms = ms * 10 + (s[i] - '0');
                        digits++;
                        i++;
                    }
                    if (!digits) return NAN;
                    while (digits < 3) ms *= 10, digits++;
                }
            }
            if (i < s.size() && s[i] == 'Z') {
                i++;
                has_tz = true;
            } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                char sg = s[i++];
                int th, tm;
                if (!num(2, th) || i >= s.size() || s[i++] != ':' || !num(2, tm)) return NAN;
                tz = (th * 60 + tm) * 60000.0 * (sg == '-' ? -1 : 1);
                has_tz = true;
            }
        }
        (void)date_only;
        (void)has_tz;
        if (i != s.size()) return NAN;
        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || sec > 59) return NAN;
        if (h == 24 && (mi || sec || ms)) return NAN;
        double t = make_date(make_day(y, mo - 1, d), make_time(h, mi, sec, ms)) - tz;
        return time_clip(t);
    }
other:
    // the toString / toUTCString formats: "Tue Feb 01 2022 00:00:00 GMT+0000" / "Tue, 01 Feb 2022 00:00:00 GMT"
    {
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char wd[8] = {}, mon[8] = {};
        int dd, yy, hh = 0, mm = 0, ss = 0;
        if (sscanf(s.c_str(), "%3s %3s %d %d %d:%d:%d", wd, mon, &dd, &yy, &hh, &mm, &ss) >= 4 ||
            sscanf(s.c_str(), "%3s, %d %3s %d %d:%d:%d", wd, &dd, mon, &yy, &hh, &mm, &ss) >= 4) {
            for (int k = 0; k < 12; k++)
                if (strcmp(mon, months[k]) == 0) return time_clip(make_date(make_day(yy, k, dd), make_time(hh, mm, ss, 0)));
        }
    }
    return NAN;
}
static double this_time(Runtime& rt, Value v) {
    if (!v.is_obj() || v.o->kind != Kind::Date) rt.throw_error(E_TYPE, "this is not a Date object.");
    return v.o->prim.n;
}
NATIVE(date_ctor) {
    UNUSED_NATIVE_ARGS;
    if (nt.is_undef()) {
        double t = now_ms();
        Obj tmp;
        (void)tmp;
        // Date() returns a string
        Obj* d = rt.new_object(rt.realm.date_proto);
        d->kind = Kind::Date;
        d->prim = Value(t);
        return rt.invoke(Value(d), rt.key("toString"), {});
    }
    double tv;
    if (argc == 0) tv = now_ms();
    else if (argc == 1) {
        Value v = args[0];
        if (v.is_obj() && v.o->kind == Kind::Date) tv = v.o->prim.n;
        else {
            Value p = rt.to_primitive(v, 0);
            if (p.is_str()) tv = parse_date(p.s->v);
            else tv = time_clip(rt.to_number(p));
        }
    } else {
        double f[7] = {0, 0, 1, 0, 0, 0, 0};
        for (int i = 0; i < 7 && i < argc; i++) f[i] = rt.to_number(args[i]);
        double y = f[0];
        if (!std::isnan(y)) {
            double yi = std::trunc(y);
            if (yi >= 0 && yi <= 99) y = 1900 + yi;
        }
        tv = time_clip(make_date(make_day(y, f[1], f[2]), make_time(f[3], f[4], f[5], f[6])));
    }
    Obj* d = rt.new_object(rt.get_prototype_from_ctor(nt, rt.realm.date_proto));
    d->kind = Kind::Date;
    d->prim = Value(tv);
    return Value(d);
}
NATIVE(date_now) {
    UNUSED_NATIVE_ARGS;
    return Value(now_ms());
}
NATIVE(date_parse) {
    UNUSED_NATIVE_ARGS;
    return Value(parse_date(rt.to_string(ARG(0))->v));
}
NATIVE(date_utc) {
    UNUSED_NATIVE_ARGS;
    double f[7] = {NAN, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < 7 && i < argc; i++) f[i] = rt.to_number(args[i]);
    double y = f[0];
    if (!std::isnan(y)) {
        double yi = std::trunc(y);
        if (yi >= 0 && yi <= 99) y = 1900 + yi;
    }
    return Value(time_clip(make_date(make_day(y, f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
}
// getters: magic 0 FullYear 1 Month 2 Date 3 Day 4 Hours 5 Minutes 6 Seconds 7 Milliseconds 8 Time 9 TimezoneOffset
NATIVE(date_get) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic % 20;
    double t = this_time(rt, thisv);
    if (std::isnan(t)) return Value(NAN);
    int date;
    switch (m) {
        case 0: return Value(year_from_time(t));
        case 1: return Value(double(month_from_time(t, &date)));
        case 2: month_from_time(t, &date); return Value(double(date));
        case 3: {
            double wd = std::fmod(day(t) + 4, 7);
            return Value(wd < 0 ? wd + 7 : wd);
        }
        case 4: { double h = std::fmod(std::floor(t / 3600000.0), 24); return Value(h < 0 ? h + 24 : h); }
        case 5: { double v = std::fmod(std::floor(t / 60000.0), 60); return Value(v < 0 ? v + 60 : v); }
        case 6: { double v = std::fmod(std::floor(t / 1000.0), 60); return Value(v < 0 ? v + 60 : v); }
        case 7: { double v = std::fmod(t, 1000); return Value(v < 0 ? v + 1000 : v); }
        case 8: return Value(t);
        case 9: return Value(0.0);
    }
    return Value(NAN);
}
// setters: magic 0 Milliseconds 1 Seconds 2 Minutes 3 Hours 4 Date 5 Month 6 FullYear 7 Time
NATIVE(date_set) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic % 20;
    double t = this_time(rt, thisv);
    if (m == 7) {
        double v = time_clip(rt.to_number(ARG(0)));
        thisv.o->prim = Value(v);
        return Value(v);
    }
    std::vector<double> a;
    for (int i = 0; i < argc; i++) a.push_back(rt.to_number(args[i]));
    if (m == 6 && std::isnan(t)) t = 0;
    if (std::isnan(t)) return Value(NAN);
    int date;
    double y = year_from_time(t), mo = month_from_time(t, &date), d = date;
    double h = std::fmod(std::floor(t / 3600000.0), 24), mi = std::fmod(std::floor(t / 60000.0), 60),
           s = std::fmod(std::floor(t / 1000.0), 60), ms = std::fmod(t, 1000);
    if (h < 0) h += 24;
    if (mi < 0) mi += 60;
    if (s < 0) s += 60;
    if (ms < 0) ms += 1000;
    auto A = [&](size_t i, double def) { return i < a.size() ? a[i] : def; };
    switch (m) {
        case 0: ms = A(0, NAN); break;
        case 1: s = A(0, NAN); ms = A(1, ms); break;
        case 2: mi = A(0, NAN); s = A(1, s); ms = A(2, ms); break;
        case 3: h = A(0, NAN); mi = A(1, mi); s = A(2, s); ms = A(3, ms); break;
        case 4: d = A(0, NAN); break;
        case 5: mo = A(0, NAN); d = A(1, d); break;
        case 6: y = A(0, NAN); mo = A(1, mo); d = A(2, d); break;
    }
    double v = time_clip(make_date(make_day(y, mo, d), make_time(h, mi, s, ms)));
    thisv.o->prim = Value(v);
    return Value(v);
}
static std::string two(int v) {
    char b[8];
    snprintf(b, sizeof b, "%02d", v);
    return b;
}
// magic 0 toString 1 toUTCString 2 toISOString 3 toDateString 4 toTimeString
NATIVE(date_to_string) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    double t = this_time(rt, thisv);
    if (std::isnan(t)) {
        if (m == 2) rt.throw_error(E_RANGE, "Invalid time value");
        return Value(rt.intern("Invalid Date"));
    }
    static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int date;
    int y = int(year_from_time(t)), mo = month_from_time(t, &date);
    double w = std::fmod(day(t) + 4, 7);
    if (w < 0) w += 7;
    int h = int(std::fmod(std::floor(t / 3600000.0), 24) + 24) % 24;
    int mi = int(std::fmod(std::floor(t / 60000.0), 60) + 60) % 60;
    int s = int(std::fmod(std::floor(t / 1000.0), 60) + 60) % 60;
    int ms = int(std::fmod(t, 1000) + 1000) % 1000;
    char yb[16];
    if (y >= 0) snprintf(yb, sizeof yb, "%04d", y);
    else snprintf(yb, sizeof yb, "-%06d", -y);
    std::string date_part = std::string(wd[int(w)]) + " " + mn[mo] + " " + two(date) + " " + yb;
    std::string time_part = two(h) + ":" + two(mi) + ":" + two(s) + " GMT+0000 (Coordinated Universal Time)";
    std::string out;
    switch (m) {
        case 0: out = date_part + " " + time_part; break;
        case 1: out = std::string(wd[int(w)]) + ", " + two(date) + " " + mn[mo] + " " + yb + " " + two(h) + ":" + two(mi) + ":" + two(s) + " GMT"; break;
        case 2: {
            char b[64];
            if (y >= 0 && y <= 9999) snprintf(b, sizeof b, "%04d-%s-%sT%s:%s:%s.%03dZ", y, two(mo + 1).c_str(), two(date).c_str(), two(h).c_str(), two(mi).c_str(), two(s).c_str(), ms);
            else snprintf(b, sizeof b, "%c%06d-%s-%sT%s:%s:%s.%03dZ", y < 0 ? '-' : '+', std::abs(y), two(mo + 1).c_str(), two(date).c_str(), two(h).c_str(), two(mi).c_str(), two(s).c_str(), ms);
            out = b;
            break;
        }
        case 3: out = date_part; break;
        case 4: out = time_part; break;
    }
    return Value(rt.str(to_u16(out)));
}
NATIVE(date_to_json) {
    UNUSED_NATIVE_ARGS;
    Obj* o = rt.to_object(thisv);
    Value tv = rt.to_primitive(Value(o), 1);
    if (tv.is_num() && !std::isfinite(tv.n)) return Value::null();
    return rt.invoke(Value(o), rt.key("toISOString"), {});
}
NATIVE(date_to_primitive) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "Date.prototype[Symbol.toPrimitive] called on non-object");
    Value h = ARG(0);
    if (!h.is_str()) rt.throw_error(E_TYPE, "Invalid hint");
    u16 hint = h.s->v;
    bool string_first;
    if (hint == u"string" || hint == u"default") string_first = true;
    else if (hint == u"number") string_first = false;
    else rt.throw_error(E_TYPE, "Invalid hint");
    const char* order[2] = {string_first ? "toString" : "valueOf", string_first ? "valueOf" : "toString"};
    for (const char* m : order) {
        Value f = rt.get(thisv, m);
        if (rt.is_callable(f)) {
            Value r = rt.call(f, thisv, nullptr, 0);
            if (!r.is_obj()) return r;
        }
    }
    rt.throw_error(E_TYPE, "Cannot convert object to primitive value");
}

void init_date(Runtime& rt) {
    Realm& R = rt.realm;
    R.date_proto = rt.new_object();
    Function* dc = constructor(rt, "Date", date_ctor, 7, R.date_proto);
    method(rt, dc, "now", date_now, 0);
    method(rt, dc, "parse", date_parse, 1);
    method(rt, dc, "UTC", date_utc, 7);
    Obj* P = R.date_proto;
    const char* gets[] = {"FullYear", "Month", "Date", "Day", "Hours", "Minutes", "Seconds", "Milliseconds"};
    for (int i = 0; i < 8; i++) {
        method(rt, P, ("get" + std::string(gets[i])).c_str(), date_get, 0, i);
        method(rt, P, ("getUTC" + std::string(gets[i])).c_str(), date_get, 0, i + 20);
    }
    method(rt, P, "getTime", date_get, 0, 8);
    method(rt, P, "valueOf", date_get, 0, 8);
    method(rt, P, "getTimezoneOffset", date_get, 0, 9);
    const char* sets[] = {"Milliseconds", "Seconds", "Minutes", "Hours", "Date", "Month", "FullYear"};
    int lens[] = {1, 2, 3, 4, 1, 2, 3};
    for (int i = 0; i < 7; i++) {
        method(rt, P, ("set" + std::string(sets[i])).c_str(), date_set, lens[i], i);
        method(rt, P, ("setUTC" + std::string(sets[i])).c_str(), date_set, lens[i], i + 20);
    }
    method(rt, P, "setTime", date_set, 1, 7);
    method(rt, P, "toString", date_to_string, 0, 0);
    method(rt, P, "toUTCString", date_to_string, 0, 1);
    method(rt, P, "toISOString", date_to_string, 0, 2);
    method(rt, P, "toDateString", date_to_string, 0, 3);
    method(rt, P, "toTimeString", date_to_string, 0, 4);
    method(rt, P, "toLocaleString", date_to_string, 0, 0);
    method(rt, P, "toLocaleDateString", date_to_string, 0, 3);
    method(rt, P, "toLocaleTimeString", date_to_string, 0, 4);
    method(rt, P, "toJSON", date_to_json, 1);
    method(rt, P, rt.sym_to_primitive, "[Symbol.toPrimitive]", date_to_primitive, 1, CONFIGURABLE);
}

}  // namespace kjs
