// String and its iterator; JSON.
#include <algorithm>
#include <cstring>

#include "builtins.h"

namespace kjs {

static Str* this_str(Runtime& rt, Value v, const char* m) {
    if (v.is_nullish()) rt.throw_error(E_TYPE, std::string("String.prototype.") + m + " called on null or undefined");
    return rt.to_string(v);
}
static bool is_regexp(Runtime& rt, Value v) {
    if (!v.is_obj()) return false;
    Value m = rt.get(v, rt.key(rt.sym_match));
    if (!m.is_undef()) return rt.to_boolean(m);
    return v.o->kind == Kind::RegExp;
}

NATIVE(string_ctor) {
    UNUSED_NATIVE_ARGS;
    Str* s;
    if (argc == 0) s = rt.intern("");
    else if (nt.is_undef() && args[0].is_sym()) {
        Sym* y = args[0].y;
        u16 d = y->description.is_undef() ? u"" : y->description.s->v;
        return Value(rt.str(u"Symbol(" + d + u")"));
    } else s = rt.to_string(args[0]);
    if (nt.is_undef()) return Value(s);
    Obj* o = rt.to_object(Value(s));
    o->proto = rt.get_prototype_from_ctor(nt, rt.realm.string_proto);
    return Value(o);
}
NATIVE(string_from_char_code) {
    UNUSED_NATIVE_ARGS;
    u16 s;
    for (int i = 0; i < argc; i++) s.push_back(char16_t(rt.to_uint32(args[i]) & 0xFFFF));
    return Value(rt.str(s));
}
NATIVE(string_from_code_point) {
    UNUSED_NATIVE_ARGS;
    u16 s;
    for (int i = 0; i < argc; i++) {
        double d = rt.to_number(args[i]);
        if (d != std::trunc(d) || d < 0 || d > 0x10FFFF || std::isnan(d)) rt.throw_error(E_RANGE, "Invalid code point");
        uint32_t c = uint32_t(d);
        if (c >= 0x10000) {
            c -= 0x10000;
            s.push_back(char16_t(0xD800 + (c >> 10)));
            s.push_back(char16_t(0xDC00 + (c & 0x3FF)));
        } else {
            s.push_back(char16_t(c));
        }
    }
    return Value(rt.str(s));
}
NATIVE(string_raw) {
    UNUSED_NATIVE_ARGS;
    Obj* cooked = rt.to_object(ARG(0));
    Obj* raw = rt.to_object(rt.get(Value(cooked), "raw"));
    double len = double(rt.length_of(Value(raw)));
    u16 out;
    for (double i = 0; i < len; i++) {
        out += rt.to_string(rt.get(Value(raw), idx_key(rt, i)))->v;
        if (i + 1 < len && i + 1 < argc) out += rt.to_string(args[int(i) + 1])->v;
    }
    return Value(rt.str(out));
}
static Value this_string_value(Runtime& rt, Value v) {
    if (v.is_str()) return v;
    if (v.is_obj() && v.o->kind == Kind::StringBox) return v.o->prim;
    rt.throw_error(E_TYPE, "String.prototype.valueOf requires that 'this' be a String");
}
NATIVE(string_value_of) {
    UNUSED_NATIVE_ARGS;
    return this_string_value(rt, thisv);
}
NATIVE(string_char_at) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;  // 0 charAt 1 charCodeAt 2 codePointAt 3 at
    Str* s = this_str(rt, thisv, "charAt");
    double p = rt.to_integer(ARG(0));
    double n = double(s->v.size());
    if (m == 3 && p < 0) p += n;
    if (p < 0 || p >= n) return m == 0 ? Value(rt.intern("")) : m == 1 ? Value(NAN) : Value();
    char16_t c = s->v[size_t(p)];
    if (m == 0 || m == 3) return Value(rt.str(u16(1, c)));
    if (m == 1) return Value(double(c));
    if (c >= 0xD800 && c <= 0xDBFF && p + 1 < n) {
        char16_t d = s->v[size_t(p) + 1];
        if (d >= 0xDC00 && d <= 0xDFFF) return Value(double(0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00)));
    }
    return Value(double(c));
}
NATIVE(string_concat) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "concat")->v;
    for (int i = 0; i < argc; i++) s += rt.to_string(args[i])->v;
    return Value(rt.str(s));
}
// magic 0 includes 1 startsWith 2 endsWith
NATIVE(string_includes) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    u16 s = this_str(rt, thisv, "includes")->v;
    if (is_regexp(rt, ARG(0))) rt.throw_error(E_TYPE, "First argument must not be a regular expression");
    u16 t = rt.to_string(ARG(0))->v;
    double len = double(s.size());
    if (m == 2) {
        double end = ARG(1).is_undef() ? len : std::min(std::max(rt.to_integer(ARG(1)), 0.0), len);
        double start = end - double(t.size());
        if (start < 0) return Value(false);
        return Value(s.compare(size_t(start), t.size(), t) == 0);
    }
    double pos = std::min(std::max(rt.to_integer(ARG(1)), 0.0), len);
    if (m == 1) return Value(pos + t.size() <= len && s.compare(size_t(pos), t.size(), t) == 0);
    return Value(s.find(t, size_t(pos)) != u16::npos);
}
NATIVE(string_index_of) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "indexOf")->v;
    u16 t = rt.to_string(ARG(0))->v;
    double pos = std::min(std::max(rt.to_integer(ARG(1)), 0.0), double(s.size()));
    size_t r = s.find(t, size_t(pos));
    return Value(r == u16::npos ? -1.0 : double(r));
}
NATIVE(string_last_index_of) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "lastIndexOf")->v;
    u16 t = rt.to_string(ARG(0))->v;
    double n = rt.to_number(ARG(1));
    double pos = std::isnan(n) ? INFINITY : std::trunc(n);
    pos = std::min(std::max(pos, 0.0), double(s.size()));
    size_t r = s.rfind(t, size_t(pos));
    return Value(r == u16::npos ? -1.0 : double(r));
}
NATIVE(string_slice) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "slice")->v;
    double len = double(s.size());
    double a = rt.to_integer(ARG(0));
    a = a < 0 ? std::max(len + a, 0.0) : std::min(a, len);
    double b = ARG(1).is_undef() ? len : rt.to_integer(ARG(1));
    b = b < 0 ? std::max(len + b, 0.0) : std::min(b, len);
    if (a >= b) return Value(rt.intern(""));
    return Value(rt.str(s.substr(size_t(a), size_t(b - a))));
}
NATIVE(string_substring) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "substring")->v;
    double len = double(s.size());
    double a = std::min(std::max(rt.to_integer(ARG(0)), 0.0), len);
    double b = ARG(1).is_undef() ? len : std::min(std::max(rt.to_integer(ARG(1)), 0.0), len);
    if (a > b) std::swap(a, b);
    return Value(rt.str(s.substr(size_t(a), size_t(b - a))));
}
NATIVE(string_substr) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "substr")->v;
    double len = double(s.size());
    double a = rt.to_integer(ARG(0));
    if (a < 0) a = std::max(len + a, 0.0);
    a = std::min(a, len);
    double n = ARG(1).is_undef() ? len - a : std::min(std::max(rt.to_integer(ARG(1)), 0.0), len - a);
    if (n <= 0) return Value(rt.intern(""));
    return Value(rt.str(s.substr(size_t(a), size_t(n))));
}
static char16_t lower(char16_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x100 && c <= 0x17F && !(c & 1) && c != 0x130 && c != 0x138) return c + 1;
    return c;
}
static char16_t upper(char16_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0x100 && c <= 0x17F && (c & 1) && c != 0x131 && c != 0x149 && c != 0x17F) return c - 1;
    if (c == 0xB5) return 0x39C;
    return c;
}
// magic 0 lower 1 upper
NATIVE(string_case) {
    UNUSED_NATIVE_ARGS;
    bool up = ((Function*)callee)->native_magic & 1;
    u16 s = this_str(rt, thisv, "toLowerCase")->v;
    u16 o;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (up && c == 0xDF) {
            o += u"SS";
            continue;
        }
        if (!up && c == 0x130) {
            o += u"i̇";
            continue;
        }
        if (!up && c == 0x3A3) {
            // final sigma: preceded by a letter and not followed by one
            bool before = i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z') || s[i - 1] >= 0x370);
            bool after = i + 1 < s.size() && ((s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z') || s[i + 1] >= 0x370);
            o.push_back(before && !after ? 0x3C2 : 0x3C3);
            continue;
        }
        o.push_back(up ? upper(c) : lower(c));
    }
    return Value(rt.str(o));
}
static bool trim_ch(char16_t c) { return is_js_space(c) || is_line_term(c); }
// magic 0 trim 1 trimStart 2 trimEnd
NATIVE(string_trim) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    u16 s = this_str(rt, thisv, "trim")->v;
    size_t a = 0, b = s.size();
    if (m != 2)
        while (a < b && trim_ch(s[a])) a++;
    if (m != 1)
        while (b > a && trim_ch(s[b - 1])) b--;
    return Value(rt.str(s.substr(a, b - a)));
}
// magic 0 padStart 1 padEnd
NATIVE(string_pad) {
    UNUSED_NATIVE_ARGS;
    bool end = ((Function*)callee)->native_magic == 1;
    u16 s = this_str(rt, thisv, "padStart")->v;
    double max = double(rt.to_length(ARG(0)));
    if (max <= s.size()) return Value(rt.str(s));
    u16 fill = ARG(1).is_undef() ? u" " : rt.to_string(ARG(1))->v;
    if (fill.empty()) return Value(rt.str(s));
    size_t need = size_t(max) - s.size();
    if (need > (1u << 30)) rt.throw_error(E_RANGE, "Invalid string length");
    u16 pad;
    while (pad.size() < need) pad += fill;
    pad.resize(need);
    return Value(rt.str(end ? s + pad : pad + s));
}
NATIVE(string_repeat) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "repeat")->v;
    double n = rt.to_integer(ARG(0));
    if (n < 0 || std::isinf(n)) rt.throw_error(E_RANGE, "Invalid count value");
    if (s.empty() || n == 0) return Value(rt.intern(""));
    if (n * s.size() > (1u << 30)) rt.throw_error(E_RANGE, "Invalid string length");
    u16 o;
    for (double i = 0; i < n; i++) o += s;
    return Value(rt.str(o));
}
NATIVE(string_locale_compare) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "localeCompare")->v;
    u16 t = rt.to_string(ARG(0))->v;
    return Value(s < t ? -1.0 : s > t ? 1.0 : 0.0);
}
NATIVE(string_normalize) {
    UNUSED_NATIVE_ARGS;
    u16 s = this_str(rt, thisv, "normalize")->v;
    if (!ARG(0).is_undef()) {
        u16 f = rt.to_string(ARG(0))->v;
        if (f != u"NFC" && f != u"NFD" && f != u"NFKC" && f != u"NFKD") rt.throw_error(E_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
    }
    return Value(rt.str(s));  // shortcut: strings are returned unnormalized
}
NATIVE(string_is_well_formed) {
    UNUSED_NATIVE_ARGS;
    bool fix = ((Function*)callee)->native_magic == 1;
    u16 s = this_str(rt, thisv, "isWellFormed")->v;
    bool ok = true;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            i++;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            ok = false;
            if (fix) s[i] = 0xFFFD;
        }
    }
    return fix ? Value(rt.str(s)) : Value(ok);
}
// split/replace/match/search: delegate to a method on the argument when present
static Value delegate(Runtime& rt, Value thisv, Value arg, Sym* sym, const std::vector<Value>& rest, bool* done) {
    *done = false;
    if (!arg.is_nullish()) {
        Value m = rt.get_method(arg, rt.key(sym));
        if (!m.is_undef()) {
            *done = true;
            std::vector<Value> a{thisv};
            a.insert(a.end(), rest.begin(), rest.end());
            return rt.call(m, arg, a);
        }
    }
    return Value();
}
static u16 get_substitution(Runtime& rt, const u16& matched, const u16& str, size_t pos, const std::vector<Value>& caps,
                            Value named, const u16& repl) {
    u16 out;
    size_t m = caps.size();
    for (size_t i = 0; i < repl.size(); i++) {
        char16_t c = repl[i];
        if (c != '$' || i + 1 >= repl.size()) {
            out.push_back(c);
            continue;
        }
        char16_t n = repl[i + 1];
        if (n == '$') {
            out.push_back('$');
            i++;
        } else if (n == '&') {
            out += matched;
            i++;
        } else if (n == '`') {
            out += str.substr(0, pos);
            i++;
        } else if (n == '\'') {
            size_t tail = std::min(str.size(), pos + matched.size());
            out += str.substr(tail);
            i++;
        } else if (n >= '0' && n <= '9') {
            int d = n - '0';
            bool two = i + 2 < repl.size() && repl[i + 2] >= '0' && repl[i + 2] <= '9';
            int dd = two ? d * 10 + (repl[i + 2] - '0') : -1;
            if (two && dd >= 1 && size_t(dd) <= m) {
                Value v = caps[dd - 1];
                if (!v.is_undef()) out += rt.to_string(v)->v;
                i += 2;
            } else if (d >= 1 && size_t(d) <= m) {
                Value v = caps[d - 1];
                if (!v.is_undef()) out += rt.to_string(v)->v;
                i += 1;
            } else {
                out.push_back('$');
            }
        } else if (n == '<') {
            if (named.is_undef()) {
                out.push_back('$');
                continue;
            }
            size_t close = repl.find('>', i + 2);
            if (close == u16::npos) {
                out.push_back('$');
                continue;
            }
            u16 name = repl.substr(i + 2, close - i - 2);
            Value v = rt.get(named, rt.key(rt.intern(name)));
            if (!v.is_undef()) out += rt.to_string(v)->v;
            i = close;
        } else {
            out.push_back('$');
        }
    }
    return out;
}
Value string_get_substitution(Runtime& rt, const u16& matched, const u16& str, size_t pos, const std::vector<Value>& caps, Value named,
                              const u16& repl) {
    return Value(rt.str(get_substitution(rt, matched, str, pos, caps, named, repl)));
}
// magic 0 replace 1 replaceAll
NATIVE(string_replace) {
    UNUSED_NATIVE_ARGS;
    bool all = ((Function*)callee)->native_magic == 1;
    if (thisv.is_nullish()) rt.throw_error(E_TYPE, "String.prototype.replace called on null or undefined");
    Value search = ARG(0), repl = ARG(1);
    if (all && is_regexp(rt, search)) {
        Value flags = rt.get(search, "flags");
        if (flags.is_nullish()) rt.throw_error(E_TYPE, "flags is null or undefined");
        if (rt.to_string(flags)->v.find('g') == u16::npos) rt.throw_error(E_TYPE, "replaceAll must be called with a global RegExp");
    }
    bool done;
    Value r = delegate(rt, thisv, search, rt.sym_replace, {repl}, &done);
    if (done) return r;
    u16 s = rt.to_string(thisv)->v;
    u16 t = rt.to_string(search)->v;
    bool fn = rt.is_callable(repl);
    u16 rs = fn ? u"" : rt.to_string(repl)->v;
    std::vector<size_t> positions;
    size_t adv = std::max<size_t>(1, t.size());
    size_t p = s.find(t);
    while (p != u16::npos) {
        positions.push_back(p);
        if (!all) break;
        p = s.find(t, p + adv);
        if (t.empty() && p > s.size()) break;
    }
    if (t.empty() && all) {
        positions.clear();
        for (size_t i = 0; i <= s.size(); i++) positions.push_back(i);
    }
    u16 out;
    size_t end = 0;
    for (size_t pos : positions) {
        out += s.substr(end, pos - end);
        if (fn) {
            Value a[3] = {Value(rt.str(t)), Value(double(pos)), Value(rt.str(s))};
            out += rt.to_string(rt.call(repl, Value(), a, 3))->v;
        } else {
            out += get_substitution(rt, t, s, pos, {}, Value(), rs);
        }
        end = pos + t.size();
    }
    out += s.substr(std::min(end, s.size()));
    return Value(rt.str(out));
}
NATIVE(string_split) {
    UNUSED_NATIVE_ARGS;
    if (thisv.is_nullish()) rt.throw_error(E_TYPE, "String.prototype.split called on null or undefined");
    bool done;
    Value r = delegate(rt, thisv, ARG(0), rt.sym_split, {ARG(1)}, &done);
    if (done) return r;
    u16 s = rt.to_string(thisv)->v;
    uint32_t lim = ARG(1).is_undef() ? 0xFFFFFFFFu : rt.to_uint32(ARG(1));
    u16 sep = rt.to_string(ARG(0))->v;
    std::vector<Value> out;
    if (lim == 0) return Value(rt.array_from_list(out));
    if (ARG(0).is_undef()) return Value(rt.array_from_list({Value(rt.str(s))}));
    if (sep.empty()) {
        for (size_t i = 0; i < s.size() && out.size() < lim; i++) out.push_back(Value(rt.str(u16(1, s[i]))));
        return Value(rt.array_from_list(out));
    }
    size_t start = 0, p;
    while ((p = s.find(sep, start)) != u16::npos) {
        out.push_back(Value(rt.str(s.substr(start, p - start))));
        if (out.size() >= lim) return Value(rt.array_from_list(out));
        start = p + sep.size();
    }
    out.push_back(Value(rt.str(s.substr(start))));
    return Value(rt.array_from_list(out));
}
// magic 0 match 1 matchAll 2 search
NATIVE(string_match) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    if (thisv.is_nullish()) rt.throw_error(E_TYPE, "String.prototype.match called on null or undefined");
    Value re = ARG(0);
    if (m == 1 && is_regexp(rt, re)) {
        Value flags = rt.get(re, "flags");
        if (flags.is_nullish()) rt.throw_error(E_TYPE, "flags is null or undefined");
        if (rt.to_string(flags)->v.find('g') == u16::npos) rt.throw_error(E_TYPE, "matchAll must be called with a global RegExp");
    }
    Sym* sym = m == 0 ? rt.sym_match : m == 1 ? rt.sym_match_all : rt.sym_search;
    bool done;
    Value r = delegate(rt, thisv, re, sym, {}, &done);
    if (done) return r;
    Str* s = rt.to_string(thisv);
    extern Value regexp_create(Runtime & rt, Value pattern, Value flags);
    Value rx = regexp_create(rt, re, m == 1 ? Value(rt.intern("g")) : Value());
    return rt.invoke(rx, rt.key(sym), {Value(s)});
}
NATIVE(string_iterator) {
    UNUSED_NATIVE_ARGS;
    Str* s = this_str(rt, thisv, "[Symbol.iterator]");
    Obj* it = rt.new_object(rt.realm.string_iterator_proto);
    it->elems.push_back(Value(s));
    it->prim = Value(0.0);
    it->kind = Kind::ArrayIterator;
    it->length = 99;  // marks a string iterator
    return Value(it);
}
NATIVE(string_iterator_next) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj() || thisv.o->kind != Kind::ArrayIterator || thisv.o->length != 99) rt.throw_error(E_TYPE, "next called on incompatible receiver");
    Obj* it = thisv.o;
    if (it->elems[0].is_undef()) return Value(rt.iter_result(Value(), true));
    const u16& s = it->elems[0].s->v;
    size_t i = size_t(it->prim.n);
    if (i >= s.size()) {
        it->elems[0] = Value();
        return Value(rt.iter_result(Value(), true));
    }
    size_t n = 1;
    if (s[i] >= 0xD800 && s[i] <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) n = 2;
    it->prim = Value(double(i + n));
    return Value(rt.iter_result(Value(rt.str(s.substr(i, n))), false));
}
// annex B html methods: magic picks the tag
static const char* HTML_TAGS[][2] = {{"anchor", "a name"}, {"big", "big"}, {"blink", "blink"}, {"bold", "b"}, {"fixed", "tt"},
                                     {"fontcolor", "font color"}, {"fontsize", "font size"}, {"italics", "i"}, {"link", "a href"},
                                     {"small", "small"}, {"strike", "strike"}, {"sub", "sub"}, {"sup", "sup"}};
NATIVE(string_html) {
    UNUSED_NATIVE_ARGS;
    int m = ((Function*)callee)->native_magic;
    u16 s = this_str(rt, thisv, HTML_TAGS[m][0])->v;
    std::string spec = HTML_TAGS[m][1];
    size_t sp = spec.find(' ');
    u16 tag = to_u16(spec.substr(0, sp));
    u16 open = u"<" + tag;
    if (sp != std::string::npos) {
        u16 v = rt.to_string(ARG(0))->v;
        u16 esc;
        for (char16_t c : v) {
            if (c == '"') esc += u"&quot;";
            else esc.push_back(c);
        }
        open += u" " + to_u16(spec.substr(sp + 1)) + u"=\"" + esc + u"\"";
    }
    return Value(rt.str(open + u">" + s + u"</" + tag + u">"));
}

void init_string(Runtime& rt) {
    Realm& R = rt.realm;
    R.string_proto = rt.new_object();
    R.string_proto->kind = Kind::StringBox;
    R.string_proto->prim = Value(rt.intern(""));
    rt.define_data(R.string_proto, "length", Value(0.0), 0);
    Function* sc = constructor(rt, "String", string_ctor, 1, R.string_proto);
    method(rt, sc, "fromCharCode", string_from_char_code, 1);
    method(rt, sc, "fromCodePoint", string_from_code_point, 1);
    method(rt, sc, "raw", string_raw, 1);
    Obj* P = R.string_proto;
    method(rt, P, "at", string_char_at, 1, 3);
    method(rt, P, "charAt", string_char_at, 1, 0);
    method(rt, P, "charCodeAt", string_char_at, 1, 1);
    method(rt, P, "codePointAt", string_char_at, 1, 2);
    method(rt, P, "concat", string_concat, 1);
    method(rt, P, "endsWith", string_includes, 1, 2);
    method(rt, P, "includes", string_includes, 1, 0);
    method(rt, P, "indexOf", string_index_of, 1);
    method(rt, P, "isWellFormed", string_is_well_formed, 0, 0);
    method(rt, P, "lastIndexOf", string_last_index_of, 1);
    method(rt, P, "localeCompare", string_locale_compare, 1);
    method(rt, P, "match", string_match, 1, 0);
    method(rt, P, "matchAll", string_match, 1, 1);
    method(rt, P, "normalize", string_normalize, 0);
    method(rt, P, "padEnd", string_pad, 1, 1);
    method(rt, P, "padStart", string_pad, 1, 0);
    method(rt, P, "repeat", string_repeat, 1);
    method(rt, P, "replace", string_replace, 2, 0);
    method(rt, P, "replaceAll", string_replace, 2, 1);
    method(rt, P, "search", string_match, 1, 2);
    method(rt, P, "slice", string_slice, 2);
    method(rt, P, "split", string_split, 2);
    method(rt, P, "startsWith", string_includes, 1, 1);
    method(rt, P, "substr", string_substr, 2);
    method(rt, P, "substring", string_substring, 2);
    method(rt, P, "toLocaleLowerCase", string_case, 0, 0);
    method(rt, P, "toLocaleUpperCase", string_case, 0, 1);
    method(rt, P, "toLowerCase", string_case, 0, 0);
    method(rt, P, "toString", string_value_of, 0);
    method(rt, P, "toUpperCase", string_case, 0, 1);
    method(rt, P, "toWellFormed", string_is_well_formed, 0, 1);
    Function* tr = method(rt, P, "trim", string_trim, 0, 0);
    Function* ts = method(rt, P, "trimStart", string_trim, 0, 1);
    Function* te = method(rt, P, "trimEnd", string_trim, 0, 2);
    (void)tr;
    rt.define_data(P, "trimLeft", Value(ts), WRITABLE | CONFIGURABLE);
    rt.define_data(P, "trimRight", Value(te), WRITABLE | CONFIGURABLE);
    method(rt, P, "valueOf", string_value_of, 0);
    method(rt, P, rt.sym_iterator, "[Symbol.iterator]", string_iterator, 0);
    for (int i = 0; i < 13; i++) {
        bool has_arg = strchr(HTML_TAGS[i][1], ' ') != nullptr;
        method(rt, P, HTML_TAGS[i][0], string_html, has_arg ? 1 : 0, i);
    }
    R.string_iterator_proto = rt.new_object(R.iterator_proto);
    method(rt, R.string_iterator_proto, "next", string_iterator_next, 0);
    rt.define_data(R.string_iterator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("String Iterator")), CONFIGURABLE);
}

// ------------------------------------------------------------------ JSON
struct JsonParser {
    Runtime& rt;
    const u16& s;
    size_t i = 0;
    [[noreturn]] void fail() { rt.throw_error(E_SYNTAX, "Unexpected token in JSON at position " + std::to_string(i)); }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    Value value() {
        ws();
        if (i >= s.size()) fail();
        char16_t c = s[i];
        if (c == '{') {
            i++;
            Obj* o = rt.new_object();
            Runtime::Root r(rt, Value(o));
            ws();
            if (i < s.size() && s[i] == '}') {
                i++;
                return Value(o);
            }
            while (true) {
                ws();
                if (i >= s.size() || s[i] != '"') fail();
                Str* k = string();
                ws();
                if (i >= s.size() || s[i] != ':') fail();
                i++;
                Value v = value();
                create_data(rt, o, rt.key(k), v);
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    i++;
                    return Value(o);
                }
                fail();
            }
        }
        if (c == '[') {
            i++;
            Obj* a = rt.new_array(0);
            Runtime::Root r(rt, Value(a));
            ws();
            if (i < s.size() && s[i] == ']') {
                i++;
                return Value(a);
            }
            while (true) {
                Value v = value();
                a->elems.push_back(v);
                a->length = uint32_t(a->elems.size());
                ws();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    i++;
                    return Value(a);
                }
                fail();
            }
        }
        if (c == '"') return Value(string());
        if (s.compare(i, 4, u"true") == 0) return i += 4, Value(true);
        if (s.compare(i, 5, u"false") == 0) return i += 5, Value(false);
        if (s.compare(i, 4, u"null") == 0) return i += 4, Value::null();
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = i;
            if (s[i] == '-') i++;
            if (i >= s.size()) fail();
            if (s[i] == '0') i++;
            else if (s[i] >= '1' && s[i] <= '9')
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            else fail();
            if (i < s.size() && s[i] == '.') {
                i++;
                size_t d = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
                if (i == d) fail();
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                i++;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
                size_t d = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
                if (i == d) fail();
            }
            return Value(strtod(to_utf8(s.substr(st, i - st)).c_str(), nullptr));
        }
        fail();
    }
    Str* string() {
        i++;
        u16 o;
        while (true) {
            if (i >= s.size()) fail();
            char16_t c = s[i++];
            if (c == '"') break;
            if (c < 0x20) fail();
            if (c != '\\') {
                o.push_back(c);
                continue;
            }
            if (i >= s.size()) fail();
            char16_t e = s[i++];
            switch (e) {
                case '"': o.push_back('"'); break;
                case '\\': o.push_back('\\'); break;
                case '/': o.push_back('/'); break;
                case 'b': o.push_back('\b'); break;
                case 'f': o.push_back('\f'); break;
                case 'n': o.push_back('\n'); break;
                case 'r': o.push_back('\r'); break;
                case 't': o.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) fail();
                    int v = 0;
                    for (int k = 0; k < 4; k++) {
                        char16_t h = s[i++];
                        int d = h >= '0' && h <= '9' ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : -1;
                        if (d < 0) fail();
                        v = v * 16 + d;
                    }
                    o.push_back(char16_t(v));
                    break;
                }
                default: fail();
            }
        }
        return rt.str(o);
    }
};

static Value internalize(Runtime& rt, Value holder, Key name, Value reviver) {
    Value val = rt.get(holder, name);
    if (val.is_obj()) {
        if (rt.is_array(val)) {
            double len = double(rt.length_of(val));
            for (double i = 0; i < len; i++) {
                Key k = idx_key(rt, i);
                Value nv = internalize(rt, val, k, reviver);
                if (nv.is_undef()) rt.delete_property(val.o, k);
                else create_data(rt, val.o, k, nv);
            }
        } else {
            for (Key k : rt.own_keys(val.o)) {
                if (k.is_sym) continue;
                Desc d;
                if (!rt.get_own(val.o, k, d) || !d.enumerable) continue;
                Value nv = internalize(rt, val, k, reviver);
                if (nv.is_undef()) rt.delete_property(val.o, k);
                else create_data(rt, val.o, k, nv);
            }
        }
    }
    Value a[2] = {rt.key_to_value(name), val};
    return rt.call(reviver, holder, a, 2);
}

NATIVE(json_parse) {
    UNUSED_NATIVE_ARGS;
    Str* text = rt.to_string(ARG(0));
    JsonParser p{rt, text->v};
    Value v = p.value();
    p.ws();
    if (p.i != text->v.size()) p.fail();
    if (rt.is_callable(ARG(1))) {
        Obj* root = rt.new_object();
        Runtime::Root r(rt, Value(root));
        create_data(rt, root, rt.key(""), v);
        return internalize(rt, Value(root), rt.key(""), ARG(1));
    }
    return v;
}

struct JsonWriter {
    Runtime& rt;
    Value replacer;
    std::vector<Key> allow;
    bool has_allow = false;
    u16 gap, indent;
    std::vector<Obj*> stack;

    static u16 quote(const u16& s) {
        u16 o = u"\"";
        for (size_t i = 0; i < s.size(); i++) {
            char16_t c = s[i];
            switch (c) {
                case '"': o += u"\\\""; break;
                case '\\': o += u"\\\\"; break;
                case '\b': o += u"\\b"; break;
                case '\f': o += u"\\f"; break;
                case '\n': o += u"\\n"; break;
                case '\r': o += u"\\r"; break;
                case '\t': o += u"\\t"; break;
                default: {
                    bool lone = false;
                    if (c >= 0xD800 && c <= 0xDBFF) {
                        if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                            o.push_back(c);
                            o.push_back(s[++i]);
                            continue;
                        }
                        lone = true;
                    } else if (c >= 0xDC00 && c <= 0xDFFF) {
                        lone = true;
                    }
                    if (c < 0x20 || lone) {
                        char b[8];
                        snprintf(b, sizeof b, "\\u%04x", c);
                        o += to_u16(b);
                    } else {
                        o.push_back(c);
                    }
                }
            }
        }
        return o + u"\"";
    }

    // returns false for undefined (the property is skipped)
    bool str(Value holder, Key key, u16& out) {
        Value v = rt.get(holder, key);
        if (v.is_obj()) {
            Value tj = rt.get(v, "toJSON");
            if (rt.is_callable(tj)) {
                Value k = rt.key_to_value(key);
                v = rt.call(tj, v, &k, 1);
            }
        }
        if (!replacer.is_undef()) {
            Value a[2] = {rt.key_to_value(key), v};
            v = rt.call(replacer, holder, a, 2);
        }
        if (v.is_obj()) {
            switch (v.o->kind) {
                case Kind::NumberBox: v = Value(rt.to_number(v)); break;
                case Kind::StringBox: v = Value(rt.to_string(v)); break;
                case Kind::BooleanBox: v = v.o->prim; break;
                default: break;
            }
        }
        if (v.is_null()) return out += u"null", true;
        if (v.is_bool()) return out += v.b ? u"true" : u"false", true;
        if (v.is_str()) return out += quote(v.s->v), true;
        if (v.is_num()) return out += std::isfinite(v.n) ? number_to_u16(v.n) : u"null", true;
        if (v.is_obj() && !rt.is_callable(v)) {
            if (std::find(stack.begin(), stack.end(), v.o) != stack.end()) rt.throw_error(E_TYPE, "Converting circular structure to JSON");
            stack.push_back(v.o);
            u16 step = indent;
            indent += gap;
            if (rt.is_array(v)) {
                double len = double(rt.length_of(v));
                std::vector<u16> parts;
                for (double i = 0; i < len; i++) {
                    u16 p;
                    if (!str(v, idx_key(rt, i), p)) p = u"null";
                    parts.push_back(p);
                }
                if (parts.empty()) out += u"[]";
                else if (gap.empty()) {
                    out += u"[";
                    for (size_t i = 0; i < parts.size(); i++) out += (i ? u"," : u"") + parts[i];
                    out += u"]";
                } else {
                    out += u"[\n" + indent;
                    for (size_t i = 0; i < parts.size(); i++) out += (i ? u",\n" + indent : u"") + parts[i];
                    out += u"\n" + step + u"]";
                }
            } else {
                std::vector<Key> keys;
                if (has_allow) keys = allow;
                else
                    for (Key k : rt.own_keys(v.o)) {
                        if (k.is_sym) continue;
                        Desc d;
                        if (rt.get_own(v.o, k, d) && d.enumerable) keys.push_back(k);
                    }
                std::vector<u16> parts;
                for (Key k : keys) {
                    u16 p;
                    if (!str(v, k, p)) continue;
                    parts.push_back(quote(k.str()->v) + (gap.empty() ? u":" : u": ") + p);
                }
                if (parts.empty()) out += u"{}";
                else if (gap.empty()) {
                    out += u"{";
                    for (size_t i = 0; i < parts.size(); i++) out += (i ? u"," : u"") + parts[i];
                    out += u"}";
                } else {
                    out += u"{\n" + indent;
                    for (size_t i = 0; i < parts.size(); i++) out += (i ? u",\n" + indent : u"") + parts[i];
                    out += u"\n" + step + u"}";
                }
            }
            stack.pop_back();
            indent = step;
            return true;
        }
        return false;
    }
};

NATIVE(json_stringify) {
    UNUSED_NATIVE_ARGS;
    JsonWriter w{rt, Value(), {}, false, u"", u"", {}};
    Value rep = ARG(1);
    if (rt.is_callable(rep)) w.replacer = rep;
    else if (rt.is_array(rep)) {
        w.has_allow = true;
        double len = double(rt.length_of(rep));
        for (double i = 0; i < len; i++) {
            Value v = rt.get(rep, idx_key(rt, i));
            Value item;
            if (v.is_str()) item = v;
            else if (v.is_num()) item = Value(rt.to_string(v));
            else if (v.is_obj() && (v.o->kind == Kind::StringBox || v.o->kind == Kind::NumberBox)) item = Value(rt.to_string(v));
            if (item.is_undef()) continue;
            Key k = rt.key(item.s);
            if (std::find(w.allow.begin(), w.allow.end(), k) == w.allow.end()) w.allow.push_back(k);
        }
    }
    Value space = ARG(2);
    if (space.is_obj()) {
        if (space.o->kind == Kind::NumberBox) space = Value(rt.to_number(space));
        else if (space.o->kind == Kind::StringBox) space = Value(rt.to_string(space));
    }
    if (space.is_num()) {
        int n = int(std::min(10.0, std::max(0.0, rt.to_integer(space))));
        w.gap = u16(n, ' ');
    } else if (space.is_str()) {
        w.gap = space.s->v.substr(0, 10);
    }
    Obj* wrapper = rt.new_object();
    Runtime::Root r(rt, Value(wrapper));
    create_data(rt, wrapper, rt.key(""), ARG(0));
    u16 out;
    if (!w.str(Value(wrapper), rt.key(""), out)) return Value();
    return Value(rt.str(out));
}

void init_json(Runtime& rt) {
    Obj* J = rt.new_object();
    rt.define_data(rt.realm.global, "JSON", Value(J), WRITABLE | CONFIGURABLE);
    method(rt, J, "parse", json_parse, 2);
    method(rt, J, "stringify", json_stringify, 3);
    rt.define_data(J, rt.key(rt.sym_to_string_tag), Value(rt.intern("JSON")), CONFIGURABLE);
}

}  // namespace kjs
