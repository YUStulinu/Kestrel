// The runtime: the collector, strings, the object model (ECMA-262 chapter 10
// essential internal methods, with Array, String and Arguments exotics),
// conversions, operators, iterators, and running scripts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "interp.h"

namespace kjs {

// ------------------------------------------------------------------ text
u16 to_u16(const std::string& s) {
    u16 o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6 && i + 1 < s.size()) cp = ((c & 31) << 6) | (s[i + 1] & 63), n = 2;
        else if ((c >> 4) == 14 && i + 2 < s.size()) cp = ((c & 15) << 12) | ((s[i + 1] & 63) << 6) | (s[i + 2] & 63), n = 3;
        else if (i + 3 < s.size()) cp = ((c & 7) << 18) | ((s[i + 1] & 63) << 12) | ((s[i + 2] & 63) << 6) | (s[i + 3] & 63), n = 4;
        else cp = 0xFFFD, n = 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            o.push_back(char16_t(0xD800 + (cp >> 10)));
            o.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            o.push_back(char16_t(cp));
        }
        i += n;
    }
    return o;
}

std::string to_utf8(const u16& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) o.push_back(char(c));
        else if (c < 0x800) o.push_back(char(0xC0 | (c >> 6))), o.push_back(char(0x80 | (c & 63)));
        else if (c < 0x10000) o.push_back(char(0xE0 | (c >> 12))), o.push_back(char(0x80 | ((c >> 6) & 63))), o.push_back(char(0x80 | (c & 63)));
        else
            o.push_back(char(0xF0 | (c >> 18))), o.push_back(char(0x80 | ((c >> 12) & 63))), o.push_back(char(0x80 | ((c >> 6) & 63))),
                o.push_back(char(0x80 | (c & 63)));
    }
    return o;
}

u16 number_to_u16(double d, int radix) {
    if (std::isnan(d)) return u"NaN";
    if (d == 0) return u"0";
    if (std::isinf(d)) return d < 0 ? u"-Infinity" : u"Infinity";
    if (radix != 10) {
        bool neg = d < 0;
        d = std::fabs(d);
        double ip = std::floor(d), fp = d - ip;
        std::string s;
        if (ip == 0) s = "0";
        while (ip >= 1) {
            double q = std::floor(ip / radix);
            int digit = int(ip - q * radix);
            s.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[digit]);
            ip = q;
        }
        std::reverse(s.begin(), s.end());
        if (fp > 0) {
            s.push_back('.');
            // enough digits to round-trip a double
            double delta = std::max(std::nextafter(d, INFINITY) - d, 1e-300) / 2;
            int n = 0;
            do {
                fp *= radix;
                delta *= radix;
                int digit = int(fp);
                fp -= digit;
                if (fp > 0.5 || (fp == 0.5 && (digit & 1))) {
                    if (fp + delta > 1) {
                        // round up and stop
                        digit++;
                        s.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[std::min(digit, radix - 1)]);
                        break;
                    }
                }
                s.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[digit]);
                n++;
            } while (fp >= delta && n < 1100);
        }
        return to_u16((neg ? "-" : "") + s);
    }
    // the shortest digits that round-trip
    char buf[64];
    int k = 0, e = 0;
    std::string digits;
    for (int p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, d);
        if (strtod(buf, nullptr) == d) break;
    }
    // buf: [-]d.ddde[+-]xx
    const char* q = buf;
    bool neg = false;
    if (*q == '-') neg = true, q++;
    for (; *q && *q != 'e'; q++)
        if (*q != '.') digits.push_back(*q);
    e = atoi(q + 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    k = int(digits.size());
    int n = e + 1;
    std::string out;
    if (k <= n && n <= 21) {
        out = digits + std::string(n - k, '0');
    } else if (0 < n && n <= 21) {
        out = digits.substr(0, n) + "." + digits.substr(n);
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string(-n, '0') + digits;
    } else {
        out = digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        out += "e";
        out += (n - 1 >= 0 ? "+" : "-");
        out += std::to_string(std::abs(n - 1));
    }
    return to_u16((neg ? "-" : "") + out);
}

static bool is_ws_or_lt(char16_t c) { return is_js_space(c) || is_line_term(c); }

double string_to_number(const u16& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws_or_lt(s[a])) a++;
    while (b > a && is_ws_or_lt(s[b - 1])) b--;
    if (a == b) return 0;
    std::string t;
    for (size_t i = a; i < b; i++) {
        if (s[i] > 127) return NAN;
        t.push_back(char(s[i]));
    }
    if (t.size() > 2 && t[0] == '0' && strchr("xXoObB", t[1])) {
        int base = (t[1] | 32) == 'x' ? 16 : (t[1] | 32) == 'o' ? 8 : 2;
        double v = 0;
        for (size_t i = 2; i < t.size(); i++) {
            int d = isdigit((unsigned char)t[i]) ? t[i] - '0' : isalpha((unsigned char)t[i]) ? (t[i] | 32) - 'a' + 10 : 99;
            if (d >= base) return NAN;
            v = v * base + d;
        }
        return v;
    }
    size_t i = 0;
    bool neg = false;
    if (t[0] == '+' || t[0] == '-') {
        neg = t[0] == '-';
        i = 1;
    }
    if (t.compare(i, std::string::npos, "Infinity") == 0) return neg ? -INFINITY : INFINITY;
    // StrUnsignedDecimalLiteral
    size_t j = i;
    bool digits = false;
    while (j < t.size() && isdigit((unsigned char)t[j])) j++, digits = true;
    if (j < t.size() && t[j] == '.') {
        j++;
        while (j < t.size() && isdigit((unsigned char)t[j])) j++, digits = true;
    }
    if (!digits) return NAN;
    if (j < t.size() && (t[j] == 'e' || t[j] == 'E')) {
        j++;
        if (j < t.size() && (t[j] == '+' || t[j] == '-')) j++;
        size_t k = j;
        while (j < t.size() && isdigit((unsigned char)t[j])) j++;
        if (j == k) return NAN;
    }
    if (j != t.size()) return NAN;
    return strtod(t.c_str(), nullptr);
}

bool is_array_index(const u16& s, uint32_t* out) {
    if (s.empty() || s.size() > 10) return false;
    if (s[0] == '0' && s.size() > 1) return false;
    uint64_t v = 0;
    for (char16_t c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v >= 4294967295ull) return false;
    *out = uint32_t(v);
    return true;
}

static bool key_index(Key k, uint32_t* out) {
    if (k.is_sym) return false;
    Str* s = k.str();
    if (s->idx == -2) {
        uint32_t v;
        s->idx = is_array_index(s->v, &v) ? int64_t(v) : -1;
    }
    if (s->idx < 0) return false;
    *out = uint32_t(s->idx);
    return true;
}

// ------------------------------------------------------------------ cells
void Sym::trace(Runtime& rt) { rt.mark(description); }
void PrivateName::trace(Runtime& rt) {
    rt.mark(description);
    rt.mark(method);
    rt.mark(getter);
    rt.mark(setter);
}
void Obj::trace(Runtime& rt) {
    rt.mark(proto);
    for (auto& p : props) {
        rt.mark(p.first.p);
        rt.mark(p.second.value);
        rt.mark(p.second.setter);
    }
    for (auto& p : privates) {
        rt.mark(p.first);
        rt.mark(p.second.value);
    }
    for (auto& v : elems) rt.mark(v);
    rt.mark(prim);
}
void Function::trace(Runtime& rt) {
    Obj::trace(rt);
    rt.mark(code);
    rt.mark(env);
    rt.mark(home_object);
    rt.mark(this_value);
    rt.mark(fields_init);
    rt.mark(bound_target);
    rt.mark(bound_this);
    for (auto& v : bound_args) rt.mark(v);
    for (auto& v : slots) rt.mark(v);
}
void Env::trace(Runtime& rt) {
    rt.mark(parent);
    rt.mark(object);
    for (auto& kv : vars) {
        rt.mark(kv.first);
        rt.mark(kv.second.v);
    }
    rt.mark(this_value);
    rt.mark(function);
    rt.mark(new_target);
    rt.mark(home_object);
}
void Code::trace(Runtime& rt) {
    for (auto& v : consts) rt.mark(v);
    for (auto* n : names) rt.mark(n);
    for (auto* f : functions) rt.mark(f);
    for (auto* t : templates) rt.mark(t);
    for (auto& l : template_strs)
        for (auto& p : l) {
            rt.mark(p.first);
            rt.mark(p.second);
        }
    for (auto& s : scopes)
        for (auto& n : s.names) rt.mark(n.first);
    for (auto& d : decl_lists)
        for (auto& n : d) rt.mark(n.first);
}

int Obj::find(Key k) const {
    if (!index.empty()) {
        auto it = index.find(k);
        return it == index.end() ? -1 : int(it->second);
    }
    for (size_t i = 0; i < props.size(); i++)
        if (props[i].first == k) return int(i);
    return -1;
}

static void obj_add(Obj* o, Key k, const Prop& p) {
    o->props.push_back({k, p});
    if (!o->index.empty()) o->index[k] = uint32_t(o->props.size() - 1);
    else if (o->props.size() > 12) {
        for (size_t i = 0; i < o->props.size(); i++) o->index[o->props[i].first] = uint32_t(i);
    }
}
static void obj_remove(Obj* o, int i) {
    o->props.erase(o->props.begin() + i);
    if (!o->index.empty()) {
        o->index.clear();
        for (size_t j = 0; j < o->props.size(); j++) o->index[o->props[j].first] = uint32_t(j);
    }
}

// ------------------------------------------------------------------ runtime
Runtime::Runtime() {
    auto mk = [&](const char* d) {
        Sym* s = alloc<Sym>();
        s->description = Value(intern(d));
        return s;
    };
    sym_iterator = mk("Symbol.iterator");
    sym_async_iterator = mk("Symbol.asyncIterator");
    sym_has_instance = mk("Symbol.hasInstance");
    sym_to_primitive = mk("Symbol.toPrimitive");
    sym_to_string_tag = mk("Symbol.toStringTag");
    sym_species = mk("Symbol.species");
    sym_is_concat_spreadable = mk("Symbol.isConcatSpreadable");
    sym_unscopables = mk("Symbol.unscopables");
    sym_match = mk("Symbol.match");
    sym_match_all = mk("Symbol.matchAll");
    sym_replace = mk("Symbol.replace");
    sym_search = mk("Symbol.search");
    sym_split = mk("Symbol.split");
    init_builtins(*this);
}

Runtime::~Runtime() {
    while (heap) {
        Cell* n = heap->next;
        delete heap;
        heap = n;
    }
}

void Runtime::mark(Cell* c) {
    if (!c || c->marked) return;
    c->marked = true;
    gray.push_back(c);
}

void Runtime::maybe_gc() {
    if (allocated > threshold) gc();
}

void Runtime::gc() {
    for (auto& kv : interned) mark(kv.second);
    for (auto& kv : symbol_registry) mark(kv.second);
    for (Sym* s : {sym_iterator, sym_async_iterator, sym_has_instance, sym_to_primitive, sym_to_string_tag, sym_species,
                   sym_is_concat_spreadable, sym_unscopables, sym_match, sym_match_all, sym_replace, sym_search, sym_split})
        mark(s);
    Obj** r = &realm.global;
    for (size_t i = 0; i < sizeof(Realm) / sizeof(void*); i++) {
        Cell* c = (Cell*)r[i];
        if (c) mark(c);
    }
    mark(eval_fn);
    for (auto& v : roots) mark(v);
    for (auto* f : frames) mark(f);
    for (auto& j : jobs) {
        mark(j.fn);
        for (auto& a : j.args) mark(a);
    }
    while (!gray.empty()) {
        Cell* c = gray.back();
        gray.pop_back();
        c->trace(*this);
    }
    Cell** p = &heap;
    size_t live = 0;
    while (*p) {
        Cell* c = *p;
        if (!c->marked) {
            *p = c->next;
            delete c;
        } else {
            c->marked = false;
            p = &c->next;
            live++;
        }
    }
    allocated = live * 96;
    threshold = std::max<size_t>(allocated * 2, 16 << 20);
}

Str* Runtime::str(const u16& s) { return alloc<Str>(s); }
Str* Runtime::str(const char* s) { return alloc<Str>(to_u16(s)); }
Str* Runtime::intern(const u16& s) {
    auto it = interned.find(s);
    if (it != interned.end()) return it->second;
    Str* x = alloc<Str>(s);
    x->interned = true;
    interned[s] = x;
    return x;
}
Str* Runtime::intern(const char* s) { return intern(to_u16(s)); }

Obj* Runtime::new_object(Obj* proto) {
    Obj* o = alloc<Obj>();
    o->proto = proto ? proto : realm.object_proto;
    return o;
}
Obj* Runtime::new_array(uint32_t n) {
    Obj* a = alloc<Obj>();
    a->kind = Kind::Array;
    a->proto = realm.array_proto;
    a->elems.assign(n, Value::empty());
    a->length = n;
    return a;
}
Obj* Runtime::array_from_list(const std::vector<Value>& v) {
    Obj* a = new_array(0);
    a->elems = v;
    a->length = uint32_t(v.size());
    return a;
}
Function* Runtime::new_native(const char* name, NativeFn fn, int length, int magic) {
    Function* f = alloc<Function>();
    f->kind = Kind::Native;
    f->proto = realm.function_proto;
    f->native = fn;
    f->native_magic = magic;
    define_data(f, "length", Value(double(length)), CONFIGURABLE);
    define_data(f, "name", Value(intern(name)), CONFIGURABLE);
    return f;
}

Obj* Runtime::make_error(ErrType t, const u16& msg) {
    Obj* e = alloc<Obj>();
    e->kind = Kind::Error;
    e->proto = realm.errors[t] ? get(Value(realm.errors[t]), "prototype").o : realm.object_proto;
    define_data(e, "message", Value(str(msg)), WRITABLE | CONFIGURABLE);
    // the stack property is a simple trace of the current frames
    return e;
}
void Runtime::throw_error(ErrType t, const std::string& msg) { throw Throw{Value(make_error(t, kjs::to_u16(msg)))}; }

// ------------------------------------------------------------------ own properties
static bool string_box_own(Runtime& rt, Obj* o, Key k, Desc& d) {
    uint32_t i;
    if (o->kind != Kind::StringBox || !key_index(k, &i)) return false;
    Str* s = o->prim.s;
    if (i >= s->v.size()) return false;
    d = Desc{};
    d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
    d.value = Value(rt.str(u16(1, s->v[i])));
    d.enumerable = true;
    return true;
}

// a mapped arguments object: the parameter binding behind index k, if any
static Binding* arg_binding(Obj* o, Key k) {
    uint32_t i;
    if (o->kind != Kind::Arguments || o->elems.empty() || !key_index(k, &i) || i >= o->elems.size()) return nullptr;
    Value n = o->elems[i];
    if (n.is_empty()) return nullptr;
    Env* env = (Env*)o->prim.c;
    return find_binding(env, n.s);
}
static void arg_unmap(Obj* o, Key k) {
    uint32_t i;
    if (o->kind == Kind::Arguments && key_index(k, &i) && i < o->elems.size()) o->elems[i] = Value::empty();
}

bool Runtime::get_own(Obj* o, Key k, Desc& d) {
    uint32_t i;
    if (o->kind == Kind::Array && key_index(k, &i) && i < o->elems.size() && !o->elems[i].is_empty()) {
        d = Desc{};
        d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
        d.value = o->elems[i];
        d.writable = d.enumerable = d.configurable = true;
        return true;
    }
    if (o->kind == Kind::Array && !k.is_sym && k.str()->v == u"length") {
        d = Desc{};
        d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
        d.value = Value(double(o->length));
        d.writable = o->length_writable;
        return true;
    }
    if (string_box_own(*this, o, k, d)) return true;
    int at = o->find(k);
    if (at < 0) return false;
    const Prop& p = o->props[at].second;
    d = Desc{};
    if (p.accessor()) {
        d.has_get = d.has_set = true;
        d.get = p.value;
        d.set = p.setter;
    } else {
        d.has_value = d.has_writable = true;
        d.value = p.value;
        d.writable = p.flags & WRITABLE;
        if (Binding* b = arg_binding(o, k)) d.value = b->v;
    }
    d.has_enumerable = d.has_configurable = true;
    d.enumerable = p.flags & ENUMERABLE;
    d.configurable = p.flags & CONFIGURABLE;
    return true;
}

bool Runtime::has_own(Obj* o, Key k) {
    Desc d;
    return get_own(o, k, d);
}

static void store(Runtime& rt, Obj* o, Key k, const Desc& d) {
    // d is complete
    uint32_t i;
    bool default_data = d.is_data() && d.writable && d.enumerable && d.configurable;
    if (o->kind == Kind::Array && key_index(k, &i)) {
        if (default_data && i < o->elems.size() + 1000000) {
            int at = o->find(k);
            if (at >= 0) obj_remove(o, at);
            if (i >= o->elems.size()) o->elems.resize(i + 1, Value::empty());
            o->elems[i] = d.value;
            if (i >= o->length) o->length = i + 1;
            return;
        }
        if (i < o->elems.size()) o->elems[i] = Value::empty();
        if (i >= o->length) o->length = i + 1;
    }
    Prop p;
    p.flags = (d.enumerable ? ENUMERABLE : 0) | (d.configurable ? CONFIGURABLE : 0);
    if (d.is_accessor()) {
        p.flags |= ACCESSOR;
        p.value = d.get;
        p.setter = d.set;
    } else {
        p.flags |= d.writable ? WRITABLE : 0;
        p.value = d.value;
    }
    int at = o->find(k);
    if (at >= 0) o->props[at].second = p;
    else obj_add(o, k, p);
    (void)rt;
}

static bool array_set_length(Runtime& rt, Obj* a, const Desc& d, bool throw_on_fail) {
    auto fail = [&]() {
        if (throw_on_fail) rt.throw_error(E_TYPE, "Cannot redefine array length");
        return false;
    };
    if (!d.has_value) {
        if (d.has_configurable && d.configurable) return fail();
        if (d.has_enumerable && d.enumerable) return fail();
        if (d.is_accessor()) return fail();
        if (d.has_writable && d.writable && !a->length_writable) return fail();
        if (d.has_writable && !d.writable) a->length_writable = false;
        return true;
    }
    double nv = rt.to_number(d.value);
    uint32_t newlen = rt.to_uint32(d.value);
    if (double(newlen) != nv) rt.throw_error(E_RANGE, "Invalid array length");
    if (d.has_configurable && d.configurable) return fail();
    if (d.has_enumerable && d.enumerable) return fail();
    if (d.is_accessor()) return fail();
    if (newlen >= a->length) {
        if (!a->length_writable && newlen != a->length) return fail();
        if (d.has_writable && d.writable && !a->length_writable) return fail();
        a->length = newlen;
        if (d.has_writable && !d.writable) a->length_writable = false;
        return true;
    }
    if (!a->length_writable) return fail();
    // delete from the end; stop at a non-configurable element
    uint32_t target = newlen;
    // non-default elements live in props
    std::vector<uint32_t> idx;
    for (auto& p : a->props) {
        uint32_t i;
        if (key_index(p.first, &i) && i >= newlen) idx.push_back(i);
    }
    std::sort(idx.rbegin(), idx.rend());
    for (uint32_t i : idx) {
        Key k = rt.key(rt.intern(number_to_u16(i)));
        int at = a->find(k);
        if (at >= 0 && !(a->props[at].second.flags & CONFIGURABLE)) {
            target = i + 1;
            break;
        }
        if (at >= 0) obj_remove(a, at);
    }
    if (a->elems.size() > target) a->elems.resize(target);
    a->length = std::max(target, newlen);
    if (target > newlen) {
        a->length = target;
        if (d.has_writable && !d.writable) a->length_writable = false;
        return fail();
    }
    if (d.has_writable && !d.writable) a->length_writable = false;
    return true;
}

bool Runtime::define(Obj* o, Key k, const Desc& d, bool throw_on_fail) {
    auto fail = [&]() {
        if (throw_on_fail) throw_error(E_TYPE, "Cannot redefine property: " + (k.is_sym ? std::string("symbol") : to_utf8(k.str()->v)));
        return false;
    };
    if (o->kind == Kind::Array && !k.is_sym && k.str()->v == u"length") return array_set_length(*this, o, d, throw_on_fail);
    uint32_t idx;
    if (o->kind == Kind::Array && key_index(k, &idx) && idx >= o->length && !o->length_writable) return fail();
    Desc cur;
    bool exists = get_own(o, k, cur);
    if (o->kind == Kind::StringBox && exists && string_box_own(*this, o, k, cur)) {
        // string indices are read-only; compatible redefinitions succeed
        if ((d.has_configurable && d.configurable) || (d.has_enumerable && !d.enumerable) || d.is_accessor() ||
            (d.has_writable && d.writable) || (d.has_value && !same_value(d.value, cur.value)))
            return fail();
        return true;
    }
    if (!exists) {
        if (!o->extensible) return fail();
        Desc n;
        if (d.is_accessor()) {
            n.has_get = n.has_set = true;
            n.get = d.has_get ? d.get : Value();
            n.set = d.has_set ? d.set : Value();
        } else {
            n.has_value = n.has_writable = true;
            n.value = d.has_value ? d.value : Value();
            n.writable = d.has_writable && d.writable;
        }
        n.has_enumerable = n.has_configurable = true;
        n.enumerable = d.has_enumerable && d.enumerable;
        n.configurable = d.has_configurable && d.configurable;
        store(*this, o, k, n);
        return true;
    }
    // ValidateAndApplyPropertyDescriptor
    if (!cur.configurable) {
        if (d.has_configurable && d.configurable) return fail();
        if (d.has_enumerable && d.enumerable != cur.enumerable) return fail();
        if (!d.is_accessor() && !d.is_data()) {
        } else if (d.is_accessor() != cur.is_accessor()) {
            return fail();
        } else if (cur.is_accessor()) {
            if (d.has_get && !same_value(d.get, cur.get)) return fail();
            if (d.has_set && !same_value(d.set, cur.set)) return fail();
        } else if (!cur.writable) {
            if (d.has_writable && d.writable) return fail();
            if (d.has_value && !same_value(d.value, cur.value)) return fail();
        }
    }
    Desc n = cur;
    if (d.is_accessor() && !cur.is_accessor()) {
        n = Desc{};
        n.has_get = n.has_set = true;
        n.has_enumerable = n.has_configurable = true;
        n.enumerable = cur.enumerable;
        n.configurable = cur.configurable;
    } else if (d.is_data() && cur.is_accessor()) {
        n = Desc{};
        n.has_value = n.has_writable = true;
        n.has_enumerable = n.has_configurable = true;
        n.enumerable = cur.enumerable;
        n.configurable = cur.configurable;
    }
    if (d.has_value) n.value = d.value;
    if (d.has_writable) n.writable = d.writable;
    if (d.has_get) n.get = d.get;
    if (d.has_set) n.set = d.set;
    if (d.has_enumerable) n.enumerable = d.enumerable;
    if (d.has_configurable) n.configurable = d.configurable;
    // a mapped argument
    if (Binding* b = arg_binding(o, k)) {
        if (d.is_accessor()) {
            arg_unmap(o, k);
        } else {
            if (d.has_value) b->v = d.value;
            else n.value = b->v;
            if (d.has_writable && !d.writable) arg_unmap(o, k);
        }
    }
    store(*this, o, k, n);
    return true;
}

void Runtime::define_data(Obj* o, Key k, Value v, uint8_t flags) {
    Desc d;
    d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
    d.value = v;
    d.writable = flags & WRITABLE;
    d.enumerable = flags & ENUMERABLE;
    d.configurable = flags & CONFIGURABLE;
    define(o, k, d);
}

bool Runtime::delete_property(Obj* o, Key k) {
    uint32_t i;
    if (o->kind == Kind::Array && key_index(k, &i) && i < o->elems.size() && !o->elems[i].is_empty()) {
        o->elems[i] = Value::empty();
        return true;
    }
    if (o->kind == Kind::Array && !k.is_sym && k.str()->v == u"length") return false;
    Desc d;
    if (string_box_own(*this, o, k, d)) return false;
    int at = o->find(k);
    if (at < 0) return true;
    if (!(o->props[at].second.flags & CONFIGURABLE)) return false;
    arg_unmap(o, k);
    obj_remove(o, at);
    return true;
}

std::vector<Key> Runtime::own_keys(Obj* o) {
    std::vector<std::pair<uint32_t, Key>> ints;
    std::vector<Key> strs, syms;
    if (o->kind == Kind::Array) {
        for (uint32_t i = 0; i < o->elems.size(); i++)
            if (!o->elems[i].is_empty()) ints.push_back({i, key(intern(number_to_u16(i)))});
    }
    if (o->kind == Kind::StringBox) {
        for (uint32_t i = 0; i < o->prim.s->v.size(); i++) ints.push_back({i, key(intern(number_to_u16(i)))});
    }
    for (auto& p : o->props) {
        uint32_t i;
        if (p.first.is_sym) syms.push_back(p.first);
        else if (key_index(p.first, &i)) ints.push_back({i, p.first});
        else strs.push_back(p.first);
    }
    std::sort(ints.begin(), ints.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<Key> out;
    for (auto& p : ints) out.push_back(p.second);
    if (o->kind == Kind::Array) out.push_back(key("length"));
    for (auto& k : strs) out.push_back(k);
    for (auto& k : syms) out.push_back(k);
    return out;
}

Obj* Runtime::get_proto(Obj* o) { return o->proto; }
bool Runtime::set_proto(Obj* o, Obj* proto) {
    if (o->proto == proto) return true;
    if (!o->extensible) return false;
    // immutable prototype exotic: Object.prototype
    if (o == realm.object_proto) return false;
    for (Obj* p = proto; p; p = p->proto)
        if (p == o) return false;
    o->proto = proto;
    return true;
}
bool Runtime::prevent_extensions(Obj* o) {
    o->extensible = false;
    return true;
}
bool Runtime::is_extensible(Obj* o) { return o->extensible; }

bool Runtime::has_property(Obj* o, Key k) {
    for (; o; o = o->proto)
        if (has_own(o, k)) return true;
    return false;
}

Value Runtime::get(Obj* o, Key k, Value receiver) {
    for (Obj* cur = o; cur; cur = cur->proto) {
        uint32_t i;
        // fast paths
        if (cur->kind == Kind::Array && key_index(k, &i) && i < cur->elems.size() && !cur->elems[i].is_empty()) return cur->elems[i];
        Desc d;
        if (!get_own(cur, k, d)) continue;
        if (d.is_accessor()) {
            if (d.get.is_undef()) return Value();
            return call(d.get, receiver, nullptr, 0);
        }
        return d.value;
    }
    return Value();
}

Value Runtime::get(Value base, Key k) {
    if (base.is_obj()) return get(base.o, k, base);
    if (base.is_str()) {
        uint32_t i;
        if (!k.is_sym && k.str()->v == u"length") return Value(double(base.s->v.size()));
        if (key_index(k, &i) && i < base.s->v.size()) return Value(str(u16(1, base.s->v[i])));
        return get(realm.string_proto, k, base);
    }
    if (base.is_num()) return get(realm.number_proto, k, base);
    if (base.is_bool()) return get(realm.boolean_proto, k, base);
    if (base.is_sym()) return get(realm.symbol_proto, k, base);
    throw_error(E_TYPE, "Cannot read properties of " + std::string(base.is_null() ? "null" : "undefined"));
}

// OrdinarySet
bool Runtime::set(Obj* o, Key k, Value v, Value receiver) {
    Desc own;
    Obj* cur = o;
    bool found = false;
    for (; cur; cur = cur->proto) {
        if (get_own(cur, k, own)) {
            found = true;
            break;
        }
    }
    if (!found) {
        own = Desc{};
        own.has_value = own.has_writable = true;
        own.writable = true;
        own.has_enumerable = own.has_configurable = true;
        own.enumerable = own.configurable = true;
        own.value = Value();
    }
    if (own.is_data()) {
        if (!own.writable) return false;
        if (!receiver.is_obj()) return false;
        Obj* r = receiver.o;
        // fast path: an existing writable own data property
        if (r == cur && found) {
            uint32_t i;
            if (r->kind == Kind::Array && key_index(k, &i) && i < r->elems.size() && !r->elems[i].is_empty()) {
                r->elems[i] = v;
                return true;
            }
            Desc d;
            d.has_value = true;
            d.value = v;
            return define(r, k, d);
        }
        Desc ex;
        if (get_own(r, k, ex)) {
            if (ex.is_accessor() || !ex.writable) return false;
            Desc d;
            d.has_value = true;
            d.value = v;
            return define(r, k, d);
        }
        Desc d;
        d.has_value = d.has_writable = d.has_enumerable = d.has_configurable = true;
        d.value = v;
        d.writable = d.enumerable = d.configurable = true;
        return define(r, k, d);
    }
    if (own.set.is_undef()) return false;
    Value a[1] = {v};
    call(own.set, receiver, a, 1);
    return true;
}

bool Runtime::set(Value base, Key k, Value v, bool strict) {
    if (base.is_obj()) return set(base.o, k, v, base);
    if (base.is_nullish()) throw_error(E_TYPE, "Cannot set properties of " + std::string(base.is_null() ? "null" : "undefined"));
    // primitives: setters on the prototype chain still run
    Obj* proto = base.is_str() ? realm.string_proto : base.is_num() ? realm.number_proto : base.is_bool() ? realm.boolean_proto : realm.symbol_proto;
    if (base.is_str()) {
        uint32_t i;
        if ((!k.is_sym && k.str()->v == u"length") || (key_index(k, &i) && i < base.s->v.size())) return false;
    }
    (void)strict;
    return set(proto, k, v, base);
}

// ------------------------------------------------------------------ calls
bool Runtime::is_callable(Value v) {
    return v.is_obj() && (v.o->kind == Kind::Function || v.o->kind == Kind::Native || v.o->kind == Kind::Bound);
}
bool Runtime::is_constructor(Value v) {
    if (!v.is_obj()) return false;
    Obj* o = v.o;
    if (o->kind == Kind::Function) {
        Code* c = ((Function*)o)->code;
        return c->kind == FnKind::Normal || c->kind == FnKind::ClassConstructor || c->kind == FnKind::DerivedConstructor ||
               o->is_class_constructor;
    }
    if (o->kind == Kind::Native) return ((Function*)o)->native_magic >= 1000 || o->is_class_constructor;
    if (o->kind == Kind::Bound) return is_constructor(((Function*)o)->bound_target);
    return false;
}

Value Runtime::call(Value f, Value thisv, const Value* args, int argc) {
    if (!is_callable(f)) throw_error(E_TYPE, "not a function");
    Function* fn = (Function*)f.o;
    switch (fn->kind) {
        case Kind::Function: {
            unsafe_depth++;
            struct D {
                Runtime& r;
                ~D() { r.unsafe_depth--; }
            } d{*this};
            return call_function(*this, fn, thisv, args, argc, Value());
        }
        case Kind::Native: {
            if (++depth > 3000) {
                depth--;
                throw_error(E_RANGE, "Maximum call stack size exceeded");
            }
            struct D {
                Runtime& r;
                ~D() { r.depth--; }
            } d{*this};
            if (fn->native_magic >= 1000 && false) {}
            return fn->native(*this, thisv, args, argc, fn, Value());
        }
        case Kind::Bound: {
            std::vector<Value> all = fn->bound_args;
            all.insert(all.end(), args, args + argc);
            return call(fn->bound_target, fn->bound_this, all.data(), int(all.size()));
        }
        default: throw_error(E_TYPE, "not a function");
    }
}
Value Runtime::call(Value f, Value thisv, const std::vector<Value>& args) { return call(f, thisv, args.data(), int(args.size())); }

Obj* Runtime::get_prototype_from_ctor(Value ctor, Obj* def) {
    Value p = get(ctor, "prototype");
    return p.is_obj() ? p.o : def;
}

Value Runtime::construct(Value f, const std::vector<Value>& args, Value new_target) {
    if (new_target.is_undef()) new_target = f;
    if (!is_constructor(f)) throw_error(E_TYPE, "not a constructor");
    Function* fn = (Function*)f.o;
    switch (fn->kind) {
        case Kind::Function: {
            Code* c = fn->code;
            unsafe_depth++;
            struct D {
                Runtime& r;
                ~D() { r.unsafe_depth--; }
            } d{*this};
            if (c->kind == FnKind::DerivedConstructor) return call_function(*this, fn, Value(), args.data(), int(args.size()), new_target);
            Obj* proto = get_prototype_from_ctor(new_target, realm.object_proto);
            Obj* thisobj = new_object(proto);
            Root r(*this, Value(thisobj));
            Value res = call_function(*this, fn, Value(thisobj), args.data(), int(args.size()), new_target);
            return res.is_obj() ? res : Value(thisobj);
        }
        case Kind::Native: {
            if (++depth > 3000) {
                depth--;
                throw_error(E_RANGE, "Maximum call stack size exceeded");
            }
            struct D {
                Runtime& r;
                ~D() { r.depth--; }
            } d{*this};
            return fn->native(*this, Value(), args.data(), int(args.size()), fn, new_target);
        }
        case Kind::Bound: {
            std::vector<Value> all = fn->bound_args;
            all.insert(all.end(), args.begin(), args.end());
            if (new_target.is_obj() && new_target.o == fn) new_target = fn->bound_target;
            return construct(fn->bound_target, all, new_target);
        }
        default: throw_error(E_TYPE, "not a constructor");
    }
}

bool Runtime::is_array(Value v) { return v.is_obj() && v.o->kind == Kind::Array; }

Value Runtime::get_method(Value v, Key k) {
    Value f = get(v, k);
    if (f.is_nullish()) return Value();
    if (!is_callable(f)) throw_error(E_TYPE, "is not a function");
    return f;
}

Value Runtime::invoke(Value v, Key k, const std::vector<Value>& args) {
    Value f = get(v, k);
    return call(f, v, args);
}

uint64_t Runtime::length_of(Value o) { return to_length(get(o, "length")); }

std::vector<Value> Runtime::list_from_array_like(Value v) {
    if (!v.is_obj()) throw_error(E_TYPE, "CreateListFromArrayLike called on non-object");
    uint64_t n = length_of(v);
    if (n > 10000000) throw_error(E_RANGE, "Too many arguments");
    std::vector<Value> out;
    for (uint64_t i = 0; i < n; i++) out.push_back(get(v, to_key(Value(double(i)))));
    return out;
}

Obj* Runtime::species_constructor(Obj* o, Obj* def) {
    Value c = get(Value(o), "constructor");
    if (c.is_undef()) return def;
    if (!c.is_obj()) throw_error(E_TYPE, "constructor is not an object");
    Value s = get(c, key(sym_species));
    if (s.is_nullish()) return def;
    if (is_constructor(s)) return s.o;
    throw_error(E_TYPE, "species is not a constructor");
}

// ------------------------------------------------------------------ conversions
Value Runtime::to_primitive(Value v, int hint) {
    if (!v.is_obj()) return v;
    Value exotic = get_method(v, key(sym_to_primitive));
    if (!exotic.is_undef()) {
        Value h = Value(intern(hint == 0 ? "default" : hint == 1 ? "number" : "string"));
        Value r = call(exotic, v, &h, 1);
        if (r.is_obj()) throw_error(E_TYPE, "Cannot convert object to primitive value");
        return r;
    }
    const char* order[2] = {"valueOf", "toString"};
    if (hint == 2) std::swap(order[0], order[1]);
    for (const char* m : order) {
        Value f = get(v, m);
        if (is_callable(f)) {
            Value r = call(f, v, nullptr, 0);
            if (!r.is_obj()) return r;
        }
    }
    throw_error(E_TYPE, "Cannot convert object to primitive value");
}

bool Runtime::to_boolean(Value v) {
    switch (v.tag) {
        case Tag::Undefined: case Tag::Null: case Tag::Empty: return false;
        case Tag::Bool: return v.b;
        case Tag::Number: return !(v.n == 0 || std::isnan(v.n));
        case Tag::String: return !v.s->v.empty();
        default: return true;
    }
}

double Runtime::to_number(Value v) {
    switch (v.tag) {
        case Tag::Undefined: return NAN;
        case Tag::Null: return 0;
        case Tag::Bool: return v.b ? 1 : 0;
        case Tag::Number: return v.n;
        case Tag::String: return string_to_number(v.s->v);
        case Tag::Symbol: throw_error(E_TYPE, "Cannot convert a Symbol value to a number");
        case Tag::Object: return to_number(to_primitive(v, 1));
        default: return NAN;
    }
}
Value Runtime::to_numeric(Value v) { return Value(to_number(v)); }

double Runtime::to_integer(Value v) {
    double d = to_number(v);
    if (std::isnan(d) || d == 0) return 0;
    if (std::isinf(d)) return d;
    return std::trunc(d);
}
int32_t Runtime::to_int32(Value v) { return int32_t(to_uint32(v)); }
uint32_t Runtime::to_uint32(Value v) {
    double d = v.is_num() ? v.n : to_number(v);
    if (std::isnan(d) || std::isinf(d)) return 0;
    d = std::trunc(d);
    d = std::fmod(d, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return uint32_t(d);
}
uint64_t Runtime::to_length(Value v) {
    double d = to_integer(v);
    if (d <= 0) return 0;
    return uint64_t(std::min(d, 9007199254740991.0));
}
uint64_t Runtime::to_index(Value v) {
    if (v.is_undef()) return 0;
    double d = to_integer(v);
    if (d < 0 || d > 9007199254740991.0) throw_error(E_RANGE, "Invalid index");
    return uint64_t(d);
}

Str* Runtime::to_string(Value v) {
    switch (v.tag) {
        case Tag::Undefined: return intern("undefined");
        case Tag::Null: return intern("null");
        case Tag::Bool: return intern(v.b ? "true" : "false");
        case Tag::Number: return str(number_to_u16(v.n));
        case Tag::String: return v.s;
        case Tag::Symbol: throw_error(E_TYPE, "Cannot convert a Symbol value to a string");
        case Tag::Object: return to_string(to_primitive(v, 2));
        default: return intern("");
    }
}

Obj* Runtime::to_object(Value v) {
    Obj* o;
    switch (v.tag) {
        case Tag::Object: return v.o;
        case Tag::Bool: o = new_object(realm.boolean_proto); o->kind = Kind::BooleanBox; break;
        case Tag::Number: o = new_object(realm.number_proto); o->kind = Kind::NumberBox; break;
        case Tag::Symbol: o = new_object(realm.symbol_proto); o->kind = Kind::SymbolBox; break;
        case Tag::String:
            o = new_object(realm.string_proto);
            o->kind = Kind::StringBox;
            o->prim = v;
            define_data(o, "length", Value(double(v.s->v.size())), 0);
            return o;
        default: throw_error(E_TYPE, "Cannot convert undefined or null to object");
    }
    o->prim = v;
    return o;
}

Key Runtime::to_key(Value v) {
    if (v.is_sym()) return Key{v.y, true};
    if (v.is_str()) return key(v.s);
    if (v.is_num()) {
        double d = v.n;
        if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) {
            // small integers are common: format directly
            char buf[16];
            snprintf(buf, sizeof buf, "%u", uint32_t(d));
            return key(buf);
        }
    }
    Value p = to_primitive(v, 2);
    if (p.is_sym()) return Key{p.y, true};
    return key(to_string(p));
}
Value Runtime::key_to_value(Key k) { return k.is_sym ? Value(k.sym()) : Value(k.str()); }

// ------------------------------------------------------------------ operators
bool Runtime::strict_equals(Value a, Value b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case Tag::Undefined: case Tag::Null: return true;
        case Tag::Bool: return a.b == b.b;
        case Tag::Number: return a.n == b.n;
        case Tag::String: return a.s == b.s || a.s->v == b.s->v;
        default: return a.c == b.c;
    }
}
bool Runtime::same_value_zero(Value a, Value b) {
    if (a.is_num() && b.is_num()) return (std::isnan(a.n) && std::isnan(b.n)) || a.n == b.n;
    return strict_equals(a, b);
}
bool Runtime::same_value(Value a, Value b) {
    if (a.is_num() && b.is_num()) {
        if (std::isnan(a.n) && std::isnan(b.n)) return true;
        if (a.n == 0 && b.n == 0) return std::signbit(a.n) == std::signbit(b.n);
        return a.n == b.n;
    }
    return strict_equals(a, b);
}
bool Runtime::loose_equals(Value a, Value b) {
    if (a.tag == b.tag) return strict_equals(a, b);
    if (a.is_nullish() && b.is_nullish()) return true;
    if (a.is_num() && b.is_str()) return a.n == to_number(b);
    if (a.is_str() && b.is_num()) return to_number(a) == b.n;
    if (a.is_bool()) return loose_equals(Value(a.b ? 1.0 : 0.0), b);
    if (b.is_bool()) return loose_equals(a, Value(b.b ? 1.0 : 0.0));
    if ((a.is_num() || a.is_str() || a.is_sym()) && b.is_obj()) return loose_equals(a, to_primitive(b, 0));
    if (a.is_obj() && (b.is_num() || b.is_str() || b.is_sym())) return loose_equals(to_primitive(a, 0), b);
    return false;
}
Value Runtime::typeof_(Value v) {
    switch (v.tag) {
        case Tag::Undefined: return Value(intern("undefined"));
        case Tag::Null: return Value(intern("object"));
        case Tag::Bool: return Value(intern("boolean"));
        case Tag::Number: return Value(intern("number"));
        case Tag::String: return Value(intern("string"));
        case Tag::Symbol: return Value(intern("symbol"));
        case Tag::Object: return Value(intern(is_callable(v) ? "function" : "object"));
        default: return Value(intern("undefined"));
    }
}
bool Runtime::instance_of(Value v, Value target) {
    if (!target.is_obj()) throw_error(E_TYPE, "Right-hand side of 'instanceof' is not an object");
    Value h = get_method(target, key(sym_has_instance));
    if (!h.is_undef()) return to_boolean(call(h, target, &v, 1));
    if (!is_callable(target)) throw_error(E_TYPE, "Right-hand side of 'instanceof' is not callable");
    // OrdinaryHasInstance
    if (target.o->kind == Kind::Bound) return instance_of(v, ((Function*)target.o)->bound_target);
    if (!v.is_obj()) return false;
    Value p = get(target, "prototype");
    if (!p.is_obj()) throw_error(E_TYPE, "Function has non-object prototype in instanceof check");
    for (Obj* o = get_proto(v.o); o; o = get_proto(o))
        if (o == p.o) return true;
    return false;
}
Value Runtime::add(Value a, Value b) {
    Value pa = to_primitive(a, 0), pb = to_primitive(b, 0);
    if (pa.is_str() || pb.is_str()) {
        Str* sa = to_string(pa);
        Str* sb = to_string(pb);
        return Value(str(sa->v + sb->v));
    }
    return Value(to_number(pa) + to_number(pb));
}

// ------------------------------------------------------------------ iterators
Obj* Runtime::iter_result(Value v, bool done) {
    Obj* o = new_object();
    define_data(o, "value", v, WRITABLE | ENUMERABLE | CONFIGURABLE);
    define_data(o, "done", Value(done), WRITABLE | ENUMERABLE | CONFIGURABLE);
    return o;
}

Value make_async_from_sync(Runtime& rt, Value iterator, Value next);

Runtime::Iter Runtime::get_iterator(Value v, bool async) {
    Iter it;
    if (async) {
        Value m = get_method(v, key(sym_async_iterator));
        if (m.is_undef()) {
            Value sm = get_method(v, key(sym_iterator));
            if (sm.is_undef()) throw_error(E_TYPE, "object is not async iterable");
            Value iter = call(sm, v, nullptr, 0);
            if (!iter.is_obj()) throw_error(E_TYPE, "Result of the Symbol.iterator method is not an object");
            Value next = get(iter, "next");
            Value w = make_async_from_sync(*this, iter, next);
            it.iterator = w;
            it.next = get(w, "next");
            return it;
        }
        it.iterator = call(m, v, nullptr, 0);
    } else {
        Value m = get_method(v, key(sym_iterator));
        if (m.is_undef()) throw_error(E_TYPE, "object is not iterable");
        it.iterator = call(m, v, nullptr, 0);
    }
    if (!it.iterator.is_obj()) throw_error(E_TYPE, "Result of the Symbol.iterator method is not an object");
    it.next = get(it.iterator, "next");
    return it;
}
bool Runtime::iter_step(Iter& it, Value& out) {
    Value r = call(it.next, it.iterator, nullptr, 0);
    if (!r.is_obj()) throw_error(E_TYPE, "Iterator result is not an object");
    if (to_boolean(get(r, "done"))) {
        it.done = true;
        return false;
    }
    out = get(r, "value");
    return true;
}
void Runtime::iter_close(Iter& it, bool on_throw) {
    Value ret;
    try {
        ret = get_method(it.iterator, key("return"));
    } catch (Throw&) {
        if (on_throw) return;
        throw;
    }
    if (ret.is_undef()) return;
    Value r;
    try {
        r = call(ret, it.iterator, nullptr, 0);
    } catch (Throw&) {
        if (on_throw) return;
        throw;
    }
    if (on_throw) return;
    if (!r.is_obj()) throw_error(E_TYPE, "Iterator result is not an object");
}

// ------------------------------------------------------------------ scripts
Value Runtime::eval_script(const u16& src, const std::string& name, bool strict) {
    Ast ast;
    ast.source = src;
    ParseOptions o;
    o.strict = strict;
    Code* code;
    try {
        parse(ast, o);
        code = compile_program(*this, ast, name, false, strict);
    } catch (SyntaxError& e) {
        throw_error(E_SYNTAX, e.msg + " (line " + std::to_string(e.line) + ")");
    }
    Root r(*this, Value());
    Frame* f = alloc<Frame>();
    f->code = code;
    f->env = realm.global_env;
    frames.push_back(f);
    struct Pop {
        Runtime& rt;
        ~Pop() { rt.frames.pop_back(); }
    } p{*this};
    Result res = execute(*this, f, Resume{});
    return res.v;
}

}  // namespace kjs
