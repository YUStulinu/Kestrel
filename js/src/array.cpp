// Array, its iterators, and the iterator prototypes; for-in enumeration.
#include <algorithm>

#include "builtins.h"

namespace kjs {

static double rel_index(Runtime& rt, Value v, double len, double def) {
    if (v.is_undef()) return def;
    double r = rt.to_integer(v);
    if (r < 0) return std::max(len + r, 0.0);
    return std::min(r, len);
}

static Obj* array_species_create(Runtime& rt, Obj* original, double len) {
    if (!rt.is_array(Value(original))) {
        if (len > 4294967295.0) rt.throw_error(E_RANGE, "Invalid array length");
        return rt.new_array(uint32_t(len));
    }
    Value c = rt.get(Value(original), "constructor");
    if (c.is_obj() && rt.is_constructor(c) && c.o == rt.realm.array_ctor) {
    } else if (c.is_obj()) {
        c = rt.get(c, rt.key(rt.sym_species));
        if (c.is_null()) c = Value();
    }
    if (c.is_undef() || (c.is_obj() && c.o == rt.realm.array_ctor)) {
        if (len > 4294967295.0) rt.throw_error(E_RANGE, "Invalid array length");
        return rt.new_array(uint32_t(len));
    }
    if (!rt.is_constructor(c)) rt.throw_error(E_TYPE, "species is not a constructor");
    Value r = rt.construct(c, {Value(len)});
    return r.o;
}

static void set_length(Runtime& rt, Obj* o, double len) {
    if (!rt.set(o, rt.key("length"), Value(len), Value(o))) rt.throw_error(E_TYPE, "Cannot assign to read only property 'length'");
}
static void set_or_throw(Runtime& rt, Obj* o, Key k, Value v) {
    if (!rt.set(o, k, v, Value(o))) rt.throw_error(E_TYPE, "Cannot assign to read only property");
}
static void delete_or_throw(Runtime& rt, Obj* o, Key k) {
    if (!rt.delete_property(o, k)) rt.throw_error(E_TYPE, "Cannot delete property");
}

NATIVE(array_ctor) {
    UNUSED_NATIVE_ARGS;
    Obj* proto = nt.is_undef() ? rt.realm.array_proto : rt.get_prototype_from_ctor(nt, rt.realm.array_proto);
    Obj* a;
    if (argc == 1 && args[0].is_num()) {
        uint32_t n = rt.to_uint32(args[0]);
        if (double(n) != args[0].n) rt.throw_error(E_RANGE, "Invalid array length");
        a = rt.new_array(0);
        a->length = n;
    } else {
        a = rt.array_from_list(std::vector<Value>(args, args + argc));
    }
    a->proto = proto;
    return Value(a);
}
NATIVE(array_is_array) {
    UNUSED_NATIVE_ARGS;
    return Value(rt.is_array(ARG(0)));
}
NATIVE(array_of) {
    UNUSED_NATIVE_ARGS;
    Obj* a;
    if (rt.is_constructor(thisv)) a = rt.construct(thisv, {Value(double(argc))}).o;
    else a = rt.new_array(uint32_t(argc));
    Runtime::Root r(rt, Value(a));
    for (int i = 0; i < argc; i++) create_data_or_throw(rt, a, idx_key(rt, i), args[i]);
    set_length(rt, a, argc);
    return Value(a);
}
NATIVE(array_from) {
    UNUSED_NATIVE_ARGS;
    Value items = ARG(0), mapfn = ARG(1), mthis = ARG(2);
    bool mapping = !mapfn.is_undef();
    if (mapping && !rt.is_callable(mapfn)) rt.throw_error(E_TYPE, "Array.from: mapper is not a function");
    Value using_it = rt.get_method(items.is_nullish() ? Value(rt.new_object()) : items, rt.key(rt.sym_iterator));
    if (items.is_nullish()) rt.throw_error(E_TYPE, "Array.from requires an array-like object");
    if (!using_it.is_undef()) {
        Obj* a = rt.is_constructor(thisv) ? rt.construct(thisv, {}).o : rt.new_array(0);
        Runtime::Root r(rt, Value(a));
        Runtime::Iter it;
        it.iterator = rt.call(using_it, items, nullptr, 0);
        if (!it.iterator.is_obj()) rt.throw_error(E_TYPE, "iterator is not an object");
        it.next = rt.get(it.iterator, "next");
        double k = 0;
        Value v;
        while (rt.iter_step(it, v)) {
            try {
                if (mapping) {
                    Value av[2] = {v, Value(k)};
                    v = rt.call(mapfn, mthis, av, 2);
                }
                create_data_or_throw(rt, a, idx_key(rt, k), v);
            } catch (Throw&) {
                rt.iter_close(it, true);
                throw;
            }
            k++;
        }
        set_length(rt, a, k);
        return Value(a);
    }
    Obj* src = rt.to_object(items);
    double len = double(rt.length_of(Value(src)));
    Obj* a = rt.is_constructor(thisv) ? rt.construct(thisv, {Value(len)}).o : rt.new_array(uint32_t(len));
    Runtime::Root r(rt, Value(a));
    for (double k = 0; k < len; k++) {
        Value v = rt.get(Value(src), idx_key(rt, k));
        if (mapping) {
            Value av[2] = {v, Value(k)};
            v = rt.call(mapfn, mthis, av, 2);
        }
        create_data_or_throw(rt, a, idx_key(rt, k), v);
    }
    set_length(rt, a, len);
    return Value(a);
}
NATIVE(array_species) {
    UNUSED_NATIVE_ARGS;
    return thisv;
}

NATIVE(array_push) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    // fast path: a plain extensible array with writable length
    if (o->kind == Kind::Array && o->extensible && o->length_writable && o->length == o->elems.size() && o->proto == rt.realm.array_proto) {
        for (int i = 0; i < argc; i++) o->elems.push_back(args[i]);
        o->length = uint32_t(o->elems.size());
        return Value(double(o->length));
    }
    double len = double(rt.length_of(Value(o)));
    if (len + argc > 9007199254740991.0) rt.throw_error(E_TYPE, "Pushing would exceed the maximum array length");
    for (int i = 0; i < argc; i++) set_or_throw(rt, o, idx_key(rt, len + i), args[i]);
    set_length(rt, o, len + argc);
    return Value(len + argc);
}
NATIVE(array_pop) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (len == 0) {
        set_length(rt, o, 0);
        return Value();
    }
    Key k = idx_key(rt, len - 1);
    Value v = rt.get(Value(o), k);
    delete_or_throw(rt, o, k);
    set_length(rt, o, len - 1);
    return v;
}
NATIVE(array_shift) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (len == 0) {
        set_length(rt, o, 0);
        return Value();
    }
    Value first = rt.get(Value(o), idx_key(rt, 0));
    for (double k = 1; k < len; k++) {
        Key from = idx_key(rt, k), to = idx_key(rt, k - 1);
        if (rt.has_property(o, from)) set_or_throw(rt, o, to, rt.get(Value(o), from));
        else delete_or_throw(rt, o, to);
    }
    delete_or_throw(rt, o, idx_key(rt, len - 1));
    set_length(rt, o, len - 1);
    return first;
}
NATIVE(array_unshift) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (argc > 0) {
        if (len + argc > 9007199254740991.0) rt.throw_error(E_TYPE, "Unshift would exceed the maximum array length");
        for (double k = len; k > 0; k--) {
            Key from = idx_key(rt, k - 1), to = idx_key(rt, k + argc - 1);
            if (rt.has_property(o, from)) set_or_throw(rt, o, to, rt.get(Value(o), from));
            else delete_or_throw(rt, o, to);
        }
        for (int j = 0; j < argc; j++) set_or_throw(rt, o, idx_key(rt, j), args[j]);
    }
    set_length(rt, o, len + argc);
    return Value(len + argc);
}
NATIVE(array_slice) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double k = rel_index(rt, ARG(0), len, 0);
    double fin = rel_index(rt, ARG(1), len, len);
    double count = std::max(fin - k, 0.0);
    Obj* a = array_species_create(rt, o, count);
    Runtime::Root r(rt, Value(a));
    double n = 0;
    for (; k < fin; k++, n++) {
        Key pk = idx_key(rt, k);
        if (rt.has_property(o, pk)) create_data_or_throw(rt, a, idx_key(rt, n), rt.get(Value(o), pk));
    }
    set_length(rt, a, n);
    return Value(a);
}
NATIVE(array_splice) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double start = rel_index(rt, ARG(0), len, 0);
    double del;
    if (argc == 0) del = 0;
    else if (argc == 1) del = len - start;
    else del = std::min(std::max(rt.to_integer(ARG(1)), 0.0), len - start);
    int items = std::max(argc - 2, 0);
    if (len + items - del > 9007199254740991.0) rt.throw_error(E_TYPE, "Splice would exceed the maximum array length");
    Obj* a = array_species_create(rt, o, del);
    Runtime::Root r(rt, Value(a));
    for (double k = 0; k < del; k++) {
        Key from = idx_key(rt, start + k);
        if (rt.has_property(o, from)) create_data_or_throw(rt, a, idx_key(rt, k), rt.get(Value(o), from));
    }
    set_length(rt, a, del);
    if (items < del) {
        for (double k = start; k < len - del; k++) {
            Key from = idx_key(rt, k + del), to = idx_key(rt, k + items);
            if (rt.has_property(o, from)) set_or_throw(rt, o, to, rt.get(Value(o), from));
            else delete_or_throw(rt, o, to);
        }
        for (double k = len; k > len - del + items; k--) delete_or_throw(rt, o, idx_key(rt, k - 1));
    } else if (items > del) {
        for (double k = len - del; k > start; k--) {
            Key from = idx_key(rt, k + del - 1), to = idx_key(rt, k + items - 1);
            if (rt.has_property(o, from)) set_or_throw(rt, o, to, rt.get(Value(o), from));
            else delete_or_throw(rt, o, to);
        }
    }
    for (int j = 0; j < items; j++) set_or_throw(rt, o, idx_key(rt, start + j), args[j + 2]);
    set_length(rt, o, len - del + items);
    return Value(a);
}
static bool is_concat_spreadable(Runtime& rt, Value v) {
    if (!v.is_obj()) return false;
    Value s = rt.get(v, rt.key(rt.sym_is_concat_spreadable));
    if (!s.is_undef()) return rt.to_boolean(s);
    return rt.is_array(v);
}
NATIVE(array_concat) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    Obj* a = array_species_create(rt, o, 0);
    Runtime::Root r(rt, Value(a));
    double n = 0;
    std::vector<Value> items{Value(o)};
    items.insert(items.end(), args, args + argc);
    for (Value e : items) {
        if (is_concat_spreadable(rt, e)) {
            double len = double(rt.length_of(e));
            if (n + len > 9007199254740991.0) rt.throw_error(E_TYPE, "Concat would exceed the maximum array length");
            for (double k = 0; k < len; k++, n++) {
                Key pk = idx_key(rt, k);
                if (rt.has_property(e.o, pk)) create_data_or_throw(rt, a, idx_key(rt, n), rt.get(e, pk));
            }
        } else {
            if (n >= 9007199254740991.0) rt.throw_error(E_TYPE, "Concat would exceed the maximum array length");
            create_data_or_throw(rt, a, idx_key(rt, n), e);
            n++;
        }
    }
    set_length(rt, a, n);
    return Value(a);
}
NATIVE(array_join) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    u16 sep = ARG(0).is_undef() ? u"," : rt.to_string(ARG(0))->v;
    // cycle detection
    static thread_local std::vector<Obj*> seen;
    if (std::find(seen.begin(), seen.end(), o) != seen.end()) return Value(rt.intern(""));
    seen.push_back(o);
    struct Pop {
        ~Pop() { seen.pop_back(); }
    } p;
    u16 out;
    for (double k = 0; k < len; k++) {
        if (k > 0) out += sep;
        Value v = rt.get(Value(o), idx_key(rt, k));
        if (!v.is_nullish()) out += rt.to_string(v)->v;
    }
    return Value(rt.str(out));
}
NATIVE(array_to_string) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    Value j = rt.get(Value(o), "join");
    if (rt.is_callable(j)) return rt.call(j, Value(o), nullptr, 0);
    extern Value objproto_to_string_fn(Runtime & rt, Value thisv);
    return rt.call(rt.get(Value(rt.realm.object_proto), "toString"), Value(o), nullptr, 0);
}
NATIVE(array_to_locale_string) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    u16 out;
    for (double k = 0; k < len; k++) {
        if (k > 0) out += u",";
        Value v = rt.get(Value(o), idx_key(rt, k));
        if (!v.is_nullish()) out += rt.to_string(rt.invoke(v, rt.key("toLocaleString"), {}))->v;
    }
    return Value(rt.str(out));
}
NATIVE(array_reverse) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    for (double lo = 0, hi = len - 1; lo < hi; lo++, hi--) {
        Key lk = idx_key(rt, lo), hk = idx_key(rt, hi);
        bool le = rt.has_property(o, lk);
        Value lv = le ? rt.get(Value(o), lk) : Value();
        bool he = rt.has_property(o, hk);
        Value hv = he ? rt.get(Value(o), hk) : Value();
        if (he) set_or_throw(rt, o, lk, hv);
        else if (le) delete_or_throw(rt, o, lk);
        if (le) set_or_throw(rt, o, hk, lv);
        else if (he) delete_or_throw(rt, o, hk);
    }
    return Value(o);
}
NATIVE(array_index_of) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;  // 0 indexOf, 1 lastIndexOf, 2 includes
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (len == 0) return m == 2 ? Value(false) : Value(-1.0);
    if (m == 1) {
        double n = argc > 1 ? rt.to_integer(ARG(1)) : len - 1;
        double k = n >= 0 ? std::min(n, len - 1) : len + n;
        for (; k >= 0; k--) {
            Key pk = idx_key(rt, k);
            if (rt.has_property(o, pk) && rt.strict_equals(rt.get(Value(o), pk), ARG(0))) return Value(k);
        }
        return Value(-1.0);
    }
    double n = rt.to_integer(ARG(1));
    if (n >= len) return m == 2 ? Value(false) : Value(-1.0);
    double k = n >= 0 ? n : std::max(len + n, 0.0);
    for (; k < len; k++) {
        Key pk = idx_key(rt, k);
        if (m == 2) {
            if (rt.same_value_zero(rt.get(Value(o), pk), ARG(0))) return Value(true);
        } else if (rt.has_property(o, pk) && rt.strict_equals(rt.get(Value(o), pk), ARG(0))) {
            return Value(k);
        }
    }
    return m == 2 ? Value(false) : Value(-1.0);
}
// iteration methods: magic 0 forEach 1 map 2 filter 3 some 4 every 5 find 6 findIndex 7 findLast 8 findLastIndex
NATIVE(array_iterate) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    Value f = ARG(0), t = ARG(1);
    if (!rt.is_callable(f)) rt.throw_error(E_TYPE, "callback is not a function");
    Obj* a = nullptr;
    if (m == 1) a = array_species_create(rt, o, len);
    if (m == 2) a = array_species_create(rt, o, 0);
    Runtime::Root r(rt, a ? Value(a) : Value());
    double to = 0;
    bool reverse = m == 7 || m == 8;
    for (double i = 0; i < len; i++) {
        double k = reverse ? len - 1 - i : i;
        Key pk = idx_key(rt, k);
        bool find = m >= 5;
        if (!find && !rt.has_property(o, pk)) continue;
        Value v = rt.get(Value(o), pk);
        Value cargs[3] = {v, Value(k), Value(o)};
        Value res = rt.call(f, t, cargs, 3);
        switch (m) {
            case 1: create_data_or_throw(rt, a, pk, res); break;
            case 2:
                if (rt.to_boolean(res)) create_data_or_throw(rt, a, idx_key(rt, to++), v);
                break;
            case 3:
                if (rt.to_boolean(res)) return Value(true);
                break;
            case 4:
                if (!rt.to_boolean(res)) return Value(false);
                break;
            case 5: case 7:
                if (rt.to_boolean(res)) return v;
                break;
            case 6: case 8:
                if (rt.to_boolean(res)) return Value(k);
                break;
        }
    }
    switch (m) {
        case 1: case 2: return Value(a);
        case 3: return Value(false);
        case 4: return Value(true);
        case 6: case 8: return Value(-1.0);
        default: return Value();
    }
}
NATIVE(array_reduce) {
    UNUSED_NATIVE_ARGS;
    bool right = ((Function*)callee)->native_magic == 1;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    Value f = ARG(0);
    if (!rt.is_callable(f)) rt.throw_error(E_TYPE, "callback is not a function");
    double k = right ? len - 1 : 0;
    double step = right ? -1 : 1;
    Value acc;
    if (argc >= 2) {
        acc = args[1];
    } else {
        bool found = false;
        for (; right ? k >= 0 : k < len; k += step) {
            Key pk = idx_key(rt, k);
            if (rt.has_property(o, pk)) {
                acc = rt.get(Value(o), pk);
                found = true;
                k += step;
                break;
            }
        }
        if (!found) rt.throw_error(E_TYPE, "Reduce of empty array with no initial value");
    }
    for (; right ? k >= 0 : k < len; k += step) {
        Key pk = idx_key(rt, k);
        if (!rt.has_property(o, pk)) continue;
        Value cargs[4] = {acc, rt.get(Value(o), pk), Value(k), Value(o)};
        acc = rt.call(f, Value(), cargs, 4);
    }
    return acc;
}
NATIVE(array_fill) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double k = rel_index(rt, ARG(1), len, 0);
    double fin = rel_index(rt, ARG(2), len, len);
    for (; k < fin; k++) set_or_throw(rt, o, idx_key(rt, k), ARG(0));
    return Value(o);
}
NATIVE(array_copy_within) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double to = rel_index(rt, ARG(0), len, 0);
    double from = rel_index(rt, ARG(1), len, 0);
    double fin = rel_index(rt, ARG(2), len, len);
    double count = std::min(fin - from, len - to);
    double dir = 1;
    if (from < to && to < from + count) {
        dir = -1;
        from += count - 1;
        to += count - 1;
    }
    for (; count > 0; count--, from += dir, to += dir) {
        Key fk = idx_key(rt, from), tk = idx_key(rt, to);
        if (rt.has_property(o, fk)) set_or_throw(rt, o, tk, rt.get(Value(o), fk));
        else delete_or_throw(rt, o, tk);
    }
    return Value(o);
}
NATIVE(array_at) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double k = rt.to_integer(ARG(0));
    if (k < 0) k += len;
    if (k < 0 || k >= len) return Value();
    return rt.get(Value(o), idx_key(rt, k));
}
static double flatten(Runtime& rt, Obj* target, Value src, double srclen, double start, double depth, Value mapper, Value thisarg) {
    double t = start;
    for (double k = 0; k < srclen; k++) {
        Key pk = idx_key(rt, k);
        if (!rt.has_property(src.o, pk)) continue;
        Value e = rt.get(src, pk);
        if (!mapper.is_undef()) {
            Value a[3] = {e, Value(k), src};
            e = rt.call(mapper, thisarg, a, 3);
        }
        if (depth > 0 && rt.is_array(e)) {
            t = flatten(rt, target, e, double(rt.length_of(e)), t, depth - 1, Value(), Value());
        } else {
            if (t >= 9007199254740991.0) rt.throw_error(E_TYPE, "too long");
            create_data_or_throw(rt, target, idx_key(rt, t), e);
            t++;
        }
    }
    return t;
}
NATIVE(array_flat) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double depth = ARG(0).is_undef() ? 1 : rt.to_integer(ARG(0));
    if (depth < 0) depth = 0;
    Obj* a = array_species_create(rt, o, 0);
    Runtime::Root r(rt, Value(a));
    flatten(rt, a, Value(o), len, 0, depth, Value(), Value());
    return Value(a);
}
NATIVE(array_flat_map) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (!rt.is_callable(ARG(0))) rt.throw_error(E_TYPE, "flatMap mapper is not callable");
    Obj* a = array_species_create(rt, o, 0);
    Runtime::Root r(rt, Value(a));
    flatten(rt, a, Value(o), len, 0, 1, ARG(0), ARG(1));
    return Value(a);
}

// sorting: a merge sort (stable); comparator errors propagate
static bool sort_less(Runtime& rt, Value cmp, Value a, Value b) {
    // undefined sorts last (handled by the caller); holes are removed
    if (!cmp.is_undef()) {
        Value args[2] = {a, b};
        double r = rt.to_number(rt.call(cmp, Value(), args, 2));
        return r < 0;
    }
    return rt.to_string(a)->v < rt.to_string(b)->v;
}
static void merge_sort(Runtime& rt, Value cmp, std::vector<Value>& v) {
    if (v.size() < 2) return;
    std::vector<Value> tmp(v.size());
    for (size_t w = 1; w < v.size(); w *= 2) {
        for (size_t lo = 0; lo < v.size(); lo += 2 * w) {
            size_t mid = std::min(lo + w, v.size()), hi = std::min(lo + 2 * w, v.size());
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = sort_less(rt, cmp, v[j], v[i]) ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
        }
        v.swap(tmp);
    }
}
static std::vector<Value> sorted_values(Runtime& rt, Obj* o, double len, Value cmp, bool skip_holes, size_t* holes) {
    std::vector<Value> vals;
    size_t undefs = 0;
    *holes = 0;
    for (double k = 0; k < len; k++) {
        Key pk = idx_key(rt, k);
        if (skip_holes && !rt.has_property(o, pk)) {
            (*holes)++;
            continue;
        }
        Value v = rt.get(Value(o), pk);
        if (v.is_undef()) undefs++;
        else vals.push_back(v);
    }
    // the values stay rooted in a temporary array while comparing
    Obj* keep = rt.array_from_list(vals);
    Runtime::Root r(rt, Value(keep));
    merge_sort(rt, cmp, vals);
    for (size_t i = 0; i < undefs; i++) vals.push_back(Value());
    return vals;
}
NATIVE(array_sort) {
    UNUSED_NATIVE_ARGS;
    Value cmp = ARG(0);
    if (!cmp.is_undef() && !rt.is_callable(cmp)) rt.throw_error(E_TYPE, "The comparison function must be either a function or undefined");
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    size_t holes;
    std::vector<Value> vals = sorted_values(rt, o, len, cmp, true, &holes);
    double k = 0;
    for (; k < vals.size(); k++) set_or_throw(rt, o, idx_key(rt, k), vals[size_t(k)]);
    for (; k < len; k++) delete_or_throw(rt, o, idx_key(rt, k));
    return Value(o);
}
NATIVE(array_to_sorted) {
    UNUSED_NATIVE_ARGS;
    Value cmp = ARG(0);
    if (!cmp.is_undef() && !rt.is_callable(cmp)) rt.throw_error(E_TYPE, "The comparison function must be either a function or undefined");
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (len > 4294967295.0) rt.throw_error(E_RANGE, "Invalid array length");
    size_t holes;
    std::vector<Value> vals = sorted_values(rt, o, len, cmp, false, &holes);
    return Value(rt.array_from_list(vals));
}
NATIVE(array_to_reversed) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    if (len > 4294967295.0) rt.throw_error(E_RANGE, "Invalid array length");
    std::vector<Value> v;
    for (double k = len - 1; k >= 0; k--) v.push_back(rt.get(Value(o), idx_key(rt, k)));
    return Value(rt.array_from_list(v));
}
NATIVE(array_with) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double rel = rt.to_integer(ARG(0));
    double at = rel >= 0 ? rel : len + rel;
    if (at >= len || at < 0) rt.throw_error(E_RANGE, "Invalid index");
    if (len > 4294967295.0) rt.throw_error(E_RANGE, "Invalid array length");
    std::vector<Value> v;
    for (double k = 0; k < len; k++) v.push_back(k == at ? ARG(1) : rt.get(Value(o), idx_key(rt, k)));
    return Value(rt.array_from_list(v));
}
NATIVE(array_to_spliced) {
    UNUSED_NATIVE_ARGS;
    Obj* o = this_obj(rt, thisv);
    double len = double(rt.length_of(Value(o)));
    double start = rel_index(rt, ARG(0), len, 0);
    double skip;
    if (argc == 0) skip = 0;
    else if (argc == 1) skip = len - start;
    else skip = std::min(std::max(rt.to_integer(ARG(1)), 0.0), len - start);
    int items = std::max(argc - 2, 0);
    double newlen = len + items - skip;
    if (newlen > 9007199254740991.0) rt.throw_error(E_TYPE, "too long");
    if (newlen > 4294967295.0) rt.throw_error(E_RANGE, "Invalid array length");
    std::vector<Value> v;
    for (double k = 0; k < start; k++) v.push_back(rt.get(Value(o), idx_key(rt, k)));
    for (int j = 0; j < items; j++) v.push_back(args[j + 2]);
    for (double k = start + skip; k < len; k++) v.push_back(rt.get(Value(o), idx_key(rt, k)));
    return Value(rt.array_from_list(v));
}

// ------------------------------------------------------------------ iterators
// array iterator: elems[0] = target, prim = next index (number), length field reused as kind
NATIVE(array_values) {
    UNUSED_NATIVE_ARGS;
    int kind = ((Function*)callee)->native_magic;  // 0 keys, 1 values, 2 entries
    Obj* o = this_obj(rt, thisv);
    Obj* it = rt.new_object(rt.realm.array_iterator_proto);
    it->kind = Kind::ArrayIterator;
    it->elems.push_back(Value(o));
    it->prim = Value(0.0);
    it->length = uint32_t(kind);
    return Value(it);
}
NATIVE(array_iterator_next) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj() || thisv.o->kind != Kind::ArrayIterator) rt.throw_error(E_TYPE, "next called on incompatible receiver");
    Obj* it = thisv.o;
    if (it->elems[0].is_undef()) return Value(rt.iter_result(Value(), true));
    Value target = it->elems[0];
    double i = it->prim.n;
    double len = double(rt.length_of(target));
    if (i >= len) {
        it->elems[0] = Value();
        return Value(rt.iter_result(Value(), true));
    }
    it->prim = Value(i + 1);
    switch (it->length) {
        case 0: return Value(rt.iter_result(Value(i), false));
        case 1: return Value(rt.iter_result(rt.get(target, idx_key(rt, i)), false));
        default: {
            Value v = rt.get(target, idx_key(rt, i));
            return Value(rt.iter_result(Value(rt.array_from_list({Value(i), v})), false));
        }
    }
}

// for-in: an enumerator object holding the keys left to visit
struct ForIn : Obj {
    Value object;
    std::vector<Value> keys;
    size_t pos = 0;
    std::vector<Key> visited;
    Obj* current = nullptr;
    void trace(Runtime& rt) override {
        Obj::trace(rt);
        rt.mark(object);
        for (auto& k : keys) rt.mark(k);
        for (auto& k : visited) rt.mark(k.p);
        rt.mark(current);
    }
};
Obj* for_in_enumerator(Runtime& rt, Value v) {
    ForIn* e = rt.alloc<ForIn>();
    if (!v.is_nullish()) {
        Obj* o = rt.to_object(v);
        e->current = o;
        for (Key k : rt.own_keys(o))
            if (!k.is_sym) e->keys.push_back(Value(k.str()));
    }
    return e;
}
bool for_in_next(Runtime& rt, Obj* eo, Value& out) {
    ForIn* e = (ForIn*)eo;
    while (e->current) {
        while (e->pos < e->keys.size()) {
            Value k = e->keys[e->pos++];
            Key key = rt.key(k.s);
            if (std::find(e->visited.begin(), e->visited.end(), key) != e->visited.end()) continue;
            Desc d;
            if (!rt.get_own(e->current, key, d)) continue;  // deleted meanwhile
            e->visited.push_back(key);
            if (!d.enumerable) continue;
            out = k;
            return true;
        }
        e->current = rt.get_proto(e->current);
        e->keys.clear();
        e->pos = 0;
        if (e->current)
            for (Key k : rt.own_keys(e->current))
                if (!k.is_sym) e->keys.push_back(Value(k.str()));
    }
    return false;
}

void init_iterators(Runtime& rt) {
    rt.realm.array_iterator_proto = rt.new_object(rt.realm.iterator_proto);
    method(rt, rt.realm.array_iterator_proto, "next", array_iterator_next, 0);
    rt.define_data(rt.realm.array_iterator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("Array Iterator")), CONFIGURABLE);
}

void init_array(Runtime& rt) {
    Realm& R = rt.realm;
    R.array_proto = rt.new_array(0);
    R.array_proto->proto = R.object_proto;
    Function* ac = constructor(rt, "Array", array_ctor, 1, R.array_proto);
    R.array_ctor = ac;
    method(rt, ac, "from", array_from, 1);
    method(rt, ac, "isArray", array_is_array, 1);
    method(rt, ac, "of", array_of, 0);
    getter(rt, ac, rt.key(rt.sym_species), "get [Symbol.species]", array_species);
    Obj* P = R.array_proto;
    method(rt, P, "at", array_at, 1);
    method(rt, P, "concat", array_concat, 1);
    method(rt, P, "copyWithin", array_copy_within, 2);
    method(rt, P, "entries", array_values, 0, 2);
    method(rt, P, "every", array_iterate, 1, 4);
    method(rt, P, "fill", array_fill, 1);
    method(rt, P, "filter", array_iterate, 1, 2);
    method(rt, P, "find", array_iterate, 1, 5);
    method(rt, P, "findIndex", array_iterate, 1, 6);
    method(rt, P, "findLast", array_iterate, 1, 7);
    method(rt, P, "findLastIndex", array_iterate, 1, 8);
    method(rt, P, "flat", array_flat, 0);
    method(rt, P, "flatMap", array_flat_map, 1);
    method(rt, P, "forEach", array_iterate, 1, 0);
    method(rt, P, "includes", array_index_of, 1, 2);
    method(rt, P, "indexOf", array_index_of, 1, 0);
    method(rt, P, "join", array_join, 1);
    method(rt, P, "keys", array_values, 0, 0);
    method(rt, P, "lastIndexOf", array_index_of, 1, 1);
    method(rt, P, "map", array_iterate, 1, 1);
    method(rt, P, "pop", array_pop, 0);
    method(rt, P, "push", array_push, 1);
    method(rt, P, "reduce", array_reduce, 1, 0);
    method(rt, P, "reduceRight", array_reduce, 1, 1);
    method(rt, P, "reverse", array_reverse, 0);
    method(rt, P, "shift", array_shift, 0);
    method(rt, P, "slice", array_slice, 2);
    method(rt, P, "some", array_iterate, 1, 3);
    method(rt, P, "sort", array_sort, 1);
    method(rt, P, "splice", array_splice, 2);
    method(rt, P, "toLocaleString", array_to_locale_string, 0);
    method(rt, P, "toReversed", array_to_reversed, 0);
    method(rt, P, "toSorted", array_to_sorted, 1);
    method(rt, P, "toSpliced", array_to_spliced, 2);
    method(rt, P, "toString", array_to_string, 0);
    method(rt, P, "unshift", array_unshift, 1);
    Function* values = method(rt, P, "values", array_values, 0, 1);
    method(rt, P, "with", array_with, 2);
    rt.define_data(P, rt.key(rt.sym_iterator), Value(values), WRITABLE | CONFIGURABLE);
    Obj* uns = rt.new_object();
    uns->proto = nullptr;
    for (const char* n : {"at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast", "findLastIndex", "flat", "flatMap",
                          "includes", "keys", "toReversed", "toSorted", "toSpliced", "values"})
        rt.define_data(uns, n, Value(true), WRITABLE | ENUMERABLE | CONFIGURABLE);
    rt.define_data(P, rt.key(rt.sym_unscopables), Value(uns), CONFIGURABLE);
}

}  // namespace kjs
