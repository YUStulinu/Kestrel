// The C interface the browser uses: create a runtime with a host callback,
// evaluate scripts, run the job queue. The DOM itself is a JavaScript
// prelude (engine/src/dom.js) built on one native: __host(op, ...args).
#include <cstring>

#include "interp.h"

using namespace kjs;

extern "C" {

struct kjs_val {
    int tag;  // 0 undefined, 1 null, 2 bool, 3 number, 4 string
    double num;
    const char16_t* str;
    size_t len;
};
typedef void (*kjs_host_fn)(void* ctx, int op, const kjs_val* args, int argc, kjs_val* ret);

struct KjsHandle {
    Runtime rt;
    kjs_host_fn fn = nullptr;
    void* ctx = nullptr;
};

static thread_local KjsHandle* g_current = nullptr; // one engine per thread may run at a time

static Value host_native(Runtime& rt, Value thisv, const Value* args, int argc, Obj* callee, Value nt) {
    (void)thisv;
    (void)callee;
    (void)nt;
    KjsHandle* h = g_current;
    if (!h || !h->fn || argc < 1) return Value();
    int op = int(rt.to_number(args[0]));
    std::vector<kjs_val> v;
    std::vector<u16> keep;  // string storage for this call
    keep.reserve(argc);
    for (int i = 1; i < argc; i++) {
        kjs_val x{0, 0, nullptr, 0};
        Value a = args[i];
        switch (a.tag) {
            case Tag::Undefined: x.tag = 0; break;
            case Tag::Null: x.tag = 1; break;
            case Tag::Bool: x.tag = 2; x.num = a.b; break;
            case Tag::Number: x.tag = 3; x.num = a.n; break;
            default: {
                keep.push_back(rt.to_string(a)->v);
                x.tag = 4;
                x.str = keep.back().data();
                x.len = keep.back().size();
            }
        }
        v.push_back(x);
    }
    kjs_val ret{0, 0, nullptr, 0};
    h->fn(h->ctx, op, v.data(), int(v.size()), &ret);
    switch (ret.tag) {
        case 1: return Value::null();
        case 2: return Value(ret.num != 0);
        case 3: return Value(ret.num);
        case 4: return Value(rt.str(u16(ret.str, ret.len)));
        default: return Value();
    }
}

void* kjs_new(kjs_host_fn fn, void* ctx) {
    KjsHandle* h = new KjsHandle();
    h->fn = fn;
    h->ctx = ctx;
    Function* f = h->rt.new_native("__host", host_native, 1);
    h->rt.define_data(h->rt.realm.global, "__host", Value(f), WRITABLE | CONFIGURABLE);
    return h;
}

void kjs_free(void* p) { delete (KjsHandle*)p; }

static void copy_err(const std::string& s, char* err, size_t errlen) {
    if (!err || !errlen) return;
    size_t n = std::min(s.size(), errlen - 1);
    memcpy(err, s.data(), n);
    err[n] = 0;
}

int kjs_eval(void* p, const char16_t* src, size_t len, const char* name, char* err, size_t errlen) {
    KjsHandle* h = (KjsHandle*)p;
    KjsHandle* prev = g_current;
    g_current = h;
    int rc = 0;
    try {
        h->rt.eval_script(u16(src, len), name ? name : "script");
    } catch (Throw& t) {
        std::string m;
        try {
            Value v = t.v;
            if (v.is_obj()) {
                Value n = h->rt.get(v, "name"), msg = h->rt.get(v, "message");
                m = (n.is_str() ? to_utf8(n.s->v) : std::string("Error")) + ": " + (msg.is_undef() ? "" : to_utf8(h->rt.to_string(msg)->v));
            } else {
                m = to_utf8(h->rt.to_string(v)->v);
            }
        } catch (Throw&) {
            m = "uncaught exception";
        }
        copy_err(m, err, errlen);
        rc = 1;
    } catch (std::exception& e) {
        copy_err(e.what(), err, errlen);
        rc = 2;
    }
    g_current = prev;
    return rc;
}

int kjs_run_jobs(void* p, char* err, size_t errlen) {
    KjsHandle* h = (KjsHandle*)p;
    KjsHandle* prev = g_current;
    g_current = h;
    int rc = 0;
    try {
        h->rt.run_jobs();
    } catch (Throw& t) {
        try {
            copy_err(to_utf8(h->rt.to_string(t.v)->v), err, errlen);
        } catch (Throw&) {
        }
        rc = 1;
    }
    g_current = prev;
    return rc;
}

}  // extern "C"
