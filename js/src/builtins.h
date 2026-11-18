// Helpers shared by the builtin library files.
#pragma once
#include <cmath>

#include "interp.h"

namespace kjs {

#define NATIVE(name) static Value name(Runtime& rt, Value thisv, const Value* args, int argc, Obj* callee, Value nt)
#define ARG(i) ((i) < argc ? args[(i)] : Value())
#define UNUSED_NATIVE_ARGS (void)thisv, (void)args, (void)argc, (void)callee, (void)nt

inline Function* method(Runtime& rt, Obj* o, const char* name, NativeFn fn, int length, int magic = 0) {
    Function* f = rt.new_native(name, fn, length, magic);
    rt.define_data(o, name, Value(f), WRITABLE | CONFIGURABLE);
    return f;
}
inline Function* method(Runtime& rt, Obj* o, Sym* key, const char* name, NativeFn fn, int length, uint8_t flags = WRITABLE | CONFIGURABLE) {
    Function* f = rt.new_native(name, fn, length);
    rt.define_data(o, rt.key(key), Value(f), flags);
    return f;
}
inline void getter(Runtime& rt, Obj* o, Key k, const char* name, NativeFn fn) {
    Function* f = rt.new_native(name, fn, 0);
    Desc d;
    d.has_get = d.has_set = d.has_enumerable = d.has_configurable = true;
    d.get = Value(f);
    d.configurable = true;
    rt.define(o, k, d);
}
// a constructor: native_magic >= 1000 marks it constructible
inline Function* constructor(Runtime& rt, const char* name, NativeFn fn, int length, Obj* proto) {
    Function* f = rt.new_native(name, fn, length, 1000);
    if (proto) {
        rt.define_data(f, "prototype", Value(proto), 0);
        rt.define_data(proto, "constructor", Value(f), WRITABLE | CONFIGURABLE);
    }
    rt.define_data(rt.realm.global, name, Value(f), WRITABLE | CONFIGURABLE);
    return f;
}
inline Obj* this_obj(Runtime& rt, Value thisv) {
    if (thisv.is_nullish()) rt.throw_error(E_TYPE, "called on null or undefined");
    return rt.to_object(thisv);
}
inline Desc data_desc(Value v) {
    Desc d;
    d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
    d.value = v;
    d.writable = d.enumerable = d.configurable = true;
    return d;
}
inline bool create_data(Runtime& rt, Obj* o, Key k, Value v) { return rt.define(o, k, data_desc(v)); }
inline void create_data_or_throw(Runtime& rt, Obj* o, Key k, Value v) { rt.define(o, k, data_desc(v), true); }
inline Key idx_key(Runtime& rt, double i) { return rt.to_key(Value(i)); }
Value desc_to_object(Runtime& rt, const Desc& d);
Desc to_desc(Runtime& rt, Value v);

void init_array(Runtime& rt);
void init_string(Runtime& rt);
void init_iterators(Runtime& rt);

}  // namespace kjs
