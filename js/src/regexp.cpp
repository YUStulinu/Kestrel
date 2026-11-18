// Regular expressions: a parser to a node tree and a backtracking matcher
// with continuations, plus RegExp and its Symbol methods.
#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <memory>

#include "builtins.h"

namespace kjs {

namespace re {

enum NT { Char, Any, Class, Seq, Alt, Group, Star, Backref, Bol, Eol, WordB, NotWordB, Look, Empty };

struct Range {
    char32_t lo, hi;
};

struct RNode {
    NT t;
    char32_t ch = 0;
    std::vector<Range> ranges;
    bool negate = false;
    std::vector<std::unique_ptr<RNode>> kids;
    int group = -1;             // capture index (Group) or backref target
    u16 backref_name;           // \k<name> resolved after parsing
    int min = 0, max = -1;      // quantifier
    bool greedy = true;
    bool ahead = true, neg = false;  // lookaround
    int first_group = 0, last_group = 0;  // groups inside a quantified atom (reset each iteration)
};

struct Program {
    std::unique_ptr<RNode> root;
    int ngroups = 1;
    std::vector<u16> names;  // per group (empty when unnamed)
    bool icase = false, multiline = false, dotall = false, unicode = false, sticky = false, global = false, indices = false;
};

struct Parser {
    const u16& s;
    size_t i = 0;
    bool u;
    bool named_groups;  // a named group appears: \k is a reference
    Program& prog;
    std::vector<RNode*> backrefs_by_name;
    std::string err;

    [[noreturn]] void fail(const std::string& m) { throw m; }
    bool eof() const { return i >= s.size(); }
    char32_t peek() const { return eof() ? 0 : s[i]; }
    char32_t next_cp() {
        char32_t c = s[i++];
        if (u && c >= 0xD800 && c <= 0xDBFF && i < s.size() && s[i] >= 0xDC00 && s[i] <= 0xDFFF) c = 0x10000 + ((c - 0xD800) << 10) + (s[i++] - 0xDC00);
        return c;
    }

    std::unique_ptr<RNode> disjunction() {
        auto a = alternative();
        if (peek() != '|') return a;
        auto alt = std::make_unique<RNode>();
        alt->t = Alt;
        alt->kids.push_back(std::move(a));
        while (!eof() && peek() == '|') {
            i++;
            alt->kids.push_back(alternative());
        }
        return alt;
    }
    std::unique_ptr<RNode> alternative() {
        auto seq = std::make_unique<RNode>();
        seq->t = Seq;
        while (!eof() && peek() != '|' && peek() != ')') seq->kids.push_back(term());
        return seq;
    }
    static int hexv(char32_t c) {
        if (c >= '0' && c <= '9') return int(c - '0');
        if ((c | 32) >= 'a' && (c | 32) <= 'f') return int((c | 32) - 'a' + 10);
        return -1;
    }
    bool read_quant(int& mn, int& mx) {
        size_t save = i;
        if (peek() != '{') return false;
        i++;
        auto num = [&](int& out) {
            size_t st = i;
            long v = 0;
            while (!eof() && peek() >= '0' && peek() <= '9') {
                v = std::min<long>(v * 10 + (peek() - '0'), 1L << 30);
                i++;
            }
            out = int(v);
            return i > st;
        };
        if (!num(mn)) {
            i = save;
            return false;
        }
        mx = mn;
        if (peek() == ',') {
            i++;
            if (!num(mx)) mx = -1;
        }
        if (peek() != '}') {
            i = save;
            return false;
        }
        i++;
        if (mx >= 0 && mx < mn) fail("numbers out of order in {} quantifier");
        return true;
    }
    std::unique_ptr<RNode> term() {
        char32_t c = peek();
        // assertions
        if (c == '^') {
            i++;
            auto n = std::make_unique<RNode>();
            n->t = Bol;
            return n;
        }
        if (c == '$') {
            i++;
            auto n = std::make_unique<RNode>();
            n->t = Eol;
            return n;
        }
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == 'b' || s[i + 1] == 'B')) {
            i += 2;
            auto n = std::make_unique<RNode>();
            n->t = s[i - 1] == 'b' ? WordB : NotWordB;
            return n;
        }
        if (c == '(' && s.compare(i, 3, u"(?=") == 0 || s.compare(i, 3, u"(?!") == 0 || s.compare(i, 4, u"(?<=") == 0 ||
            s.compare(i, 4, u"(?<!") == 0) {
            bool behind = s[i + 2] == '<';
            auto n = std::make_unique<RNode>();
            n->t = Look;
            n->ahead = !behind;
            n->neg = s[i + (behind ? 3 : 2)] == '!';
            i += behind ? 4 : 3;
            n->kids.push_back(disjunction());
            if (peek() != ')') fail("unterminated group");
            i++;
            // annex B: lookaheads may be quantified in non-unicode mode
            if (!u && !behind) return quantifier(std::move(n), 0, 0);
            if (!eof() && (peek() == '*' || peek() == '+' || peek() == '?')) fail("nothing to repeat");
            if (peek() == '{') {
                int a, b;
                if (read_quant(a, b) || u) fail("nothing to repeat");
            }
            return n;
        }
        int g0 = prog.ngroups;
        auto a = atom();
        return quantifier(std::move(a), g0, prog.ngroups);
    }
    std::unique_ptr<RNode> quantifier(std::unique_ptr<RNode> a, int g0, int g1) {
        int mn, mx;
        char32_t c = peek();
        if (c == '*') mn = 0, mx = -1, i++;
        else if (c == '+') mn = 1, mx = -1, i++;
        else if (c == '?') mn = 0, mx = 1, i++;
        else if (c == '{') {
            if (!read_quant(mn, mx)) {
                if (u) fail("incomplete quantifier");
                return a;
            }
        } else return a;
        auto q = std::make_unique<RNode>();
        q->t = Star;
        q->min = mn;
        q->max = mx;
        if (peek() == '?') {
            q->greedy = false;
            i++;
        }
        q->first_group = g0;
        q->last_group = g1;
        q->kids.push_back(std::move(a));
        return q;
    }
    void add_class_escape(char32_t e, std::vector<Range>& r) {
        auto digits = [&] { r.push_back({'0', '9'}); };
        auto words = [&] {
            r.push_back({'0', '9'});
            r.push_back({'A', 'Z'});
            r.push_back({'_', '_'});
            r.push_back({'a', 'z'});
            if (u && prog.icase) {
                r.push_back({0x17F, 0x17F});
                r.push_back({0x212A, 0x212A});
            }
        };
        auto spaces = [&] {
            for (char32_t c : {9u, 10u, 11u, 12u, 13u, 32u, 0xA0u, 0x1680u, 0x2028u, 0x2029u, 0x202Fu, 0x205Fu, 0x3000u, 0xFEFFu}) r.push_back({c, c});
            r.push_back({0x2000, 0x200A});
        };
        std::vector<Range> tmp;
        bool neg = e == 'D' || e == 'W' || e == 'S';
        std::vector<Range>& dst = neg ? tmp : r;
        std::swap(dst, neg ? tmp : r);
        if (e == 'd' || e == 'D') {
            if (neg) tmp.push_back({'0', '9'});
            else digits();
        } else if (e == 'w' || e == 'W') {
            if (neg) {
                tmp = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
                if (u && prog.icase) {
                    tmp.push_back({0x17F, 0x17F});
                    tmp.push_back({0x212A, 0x212A});
                }
            } else words();
        } else {
            if (neg) {
                std::vector<Range> save;
                std::swap(save, r);
                spaces();
                tmp = r;
                r = save;
            } else spaces();
        }
        if (neg) {
            // complement of tmp within [0, 0x10FFFF]
            std::sort(tmp.begin(), tmp.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
            char32_t lo = 0;
            for (auto& x : tmp) {
                if (x.lo > lo) r.push_back({lo, x.lo - 1});
                lo = std::max<char32_t>(lo, x.hi + 1);
            }
            if (lo <= 0x10FFFF) r.push_back({lo, 0x10FFFF});
        }
    }
    bool unicode_property(std::vector<Range>& r, bool negate) {
        // \p{...}: a few general categories and binary properties
        if (peek() != '{') fail("invalid property name");
        size_t close = s.find('}', i);
        if (close == u16::npos) fail("invalid property name");
        std::string name = to_utf8(s.substr(i + 1, close - i - 1));
        i = close + 1;
        std::vector<Range> v;
        if (name == "ASCII") v = {{0, 0x7F}};
        else if (name == "ASCII_Hex_Digit") v = {{'0', '9'}, {'A', 'F'}, {'a', 'f'}};
        else if (name == "Any") v = {{0, 0x10FFFF}};
        else if (name == "L" || name == "Letter" || name == "Alphabetic" || name == "General_Category=Letter" || name == "gc=L")
            v = {{'A', 'Z'}, {'a', 'z'}, {0xAA, 0xAA}, {0xB5, 0xB5}, {0xBA, 0xBA}, {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x2C1}, {0x370, 0x3FF}, {0x400, 0x52F},
                 {0x4E00, 0x9FFF}};
        else if (name == "Lu" || name == "Uppercase_Letter" || name == "Uppercase") v = {{'A', 'Z'}, {0xC0, 0xD6}, {0xD8, 0xDE}, {0x391, 0x3A9}, {0x410, 0x42F}};
        else if (name == "Ll" || name == "Lowercase_Letter" || name == "Lowercase") v = {{'a', 'z'}, {0xDF, 0xF6}, {0xF8, 0xFF}, {0x3B1, 0x3C9}, {0x430, 0x44F}};
        else if (name == "N" || name == "Number" || name == "Nd" || name == "Decimal_Number" || name == "digit") v = {{'0', '9'}, {0x660, 0x669}, {0xFF10, 0xFF19}};
        else if (name == "White_Space" || name == "space") add_class_escape('s', v);
        else if (name == "P" || name == "Punctuation") v = {{'!', '#'}, {'%', '*'}, {',', '/'}, {':', ';'}, {'?', '@'}, {'[', ']'}, {'_', '_'}, {'{', '{'}, {'}', '}'}};
        else if (name.rfind("Script=", 0) == 0 || name.rfind("sc=", 0) == 0 || name.rfind("Script_Extensions=", 0) == 0 || name.rfind("scx=", 0) == 0) {
            std::string sc = name.substr(name.find('=') + 1);
            if (sc == "Latin" || sc == "Latn") v = {{'A', 'Z'}, {'a', 'z'}, {0xC0, 0x24F}};
            else if (sc == "Greek" || sc == "Grek") v = {{0x370, 0x3FF}, {0x1F00, 0x1FFF}};
            else if (sc == "Cyrillic" || sc == "Cyrl") v = {{0x400, 0x52F}};
            else if (sc == "Han" || sc == "Hani") v = {{0x4E00, 0x9FFF}, {0x3400, 0x4DBF}};
            else fail("invalid property name");
        } else fail("invalid property name");
        if (negate) {
            std::sort(v.begin(), v.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
            char32_t lo = 0;
            for (auto& x : v) {
                if (x.lo > lo) r.push_back({lo, x.lo - 1});
                lo = std::max<char32_t>(lo, x.hi + 1);
            }
            if (lo <= 0x10FFFF) r.push_back({lo, 0x10FFFF});
        } else {
            r.insert(r.end(), v.begin(), v.end());
        }
        return true;
    }
    // a character escape after '\'; returns the code point, or -1 for class escapes added to `cls`
    long escape(std::vector<Range>* cls, bool in_class) {
        if (eof()) fail("\\ at end of pattern");
        char32_t c = next_cp();
        switch (c) {
            case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
                std::vector<Range> r;
                add_class_escape(c, r);
                if (cls) cls->insert(cls->end(), r.begin(), r.end());
                return -1 - long(c);
            }
            case 'p': case 'P':
                if (u) {
                    std::vector<Range> r;
                    unicode_property(r, c == 'P');
                    if (cls) cls->insert(cls->end(), r.begin(), r.end());
                    return -1 - long(c);
                }
                return c;
            case 't': return '\t';
            case 'n': return '\n';
            case 'v': return '\v';
            case 'f': return '\f';
            case 'r': return '\r';
            case 'b':
                if (in_class) return '\b';
                return 'b';
            case '-':
                if (in_class) return '-';
                if (u) fail("invalid escape");
                return '-';
            case 'c': {
                char32_t l = peek();
                if ((l | 32) >= 'a' && (l | 32) <= 'z') {
                    i++;
                    return l % 32;
                }
                if (!u && in_class && ((l >= '0' && l <= '9') || l == '_')) {
                    i++;
                    return l % 32;
                }
                if (u) fail("invalid unicode escape");
                i--;
                return '\\';
            }
            case '0':
                if (!(peek() >= '0' && peek() <= '9')) return 0;
                if (u) fail("invalid decimal escape");
                [[fallthrough]];
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                if (!in_class) {
                    // a back reference (decided by the caller); here only class/legacy octal
                }
                if (u) fail("invalid escape");
                long v = c - '0';
                if (v < 8) {
                    int n = 1;
                    while (n < 3 && peek() >= '0' && peek() <= '7' && v * 8 + (peek() - '0') <= 0377) {
                        v = v * 8 + (peek() - '0');
                        i++;
                        n++;
                    }
                }
                return v;
            }
            case '8': case '9':
                if (u) fail("invalid escape");
                return c;
            case 'x': {
                if (i + 1 < s.size() + 0 && i + 2 <= s.size() && hexv(s[i]) >= 0 && i + 1 < s.size() && hexv(s[i + 1]) >= 0) {
                    long v = hexv(s[i]) * 16 + hexv(s[i + 1]);
                    i += 2;
                    return v;
                }
                if (u) fail("invalid escape");
                return 'x';
            }
            case 'u': {
                if (u && peek() == '{') {
                    size_t j = i + 1;
                    long v = 0;
                    while (j < s.size() && hexv(s[j]) >= 0) {
                        v = v * 16 + hexv(s[j]);
                        if (v > 0x10FFFF) fail("invalid unicode escape");
                        j++;
                    }
                    if (j == i + 1 || j >= s.size() || s[j] != '}') fail("invalid unicode escape");
                    i = j + 1;
                    return v;
                }
                if (i + 4 <= s.size() && hexv(s[i]) >= 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0 && hexv(s[i + 3]) >= 0) {
                    long v = hexv(s[i]) << 12 | hexv(s[i + 1]) << 8 | hexv(s[i + 2]) << 4 | hexv(s[i + 3]);
                    i += 4;
                    if (u && v >= 0xD800 && v <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        long w = 0;
                        bool ok = true;
                        for (int k = 2; k < 6; k++) {
                            int h = hexv(s[i + k]);
                            if (h < 0) ok = false;
                            w = w * 16 + std::max(h, 0);
                        }
                        if (ok && w >= 0xDC00 && w <= 0xDFFF) {
                            i += 6;
                            return 0x10000 + ((v - 0xD800) << 10) + (w - 0xDC00);
                        }
                    }
                    return v;
                }
                if (u) fail("invalid unicode escape");
                return 'u';
            }
            case 'k':
                if (u || named_groups) fail("invalid named reference");
                return 'k';
            default:
                if (u) {
                    // only syntax characters and / may be escaped
                    if (strchr("^$\\.*+?()[]{}|/", int(c)) && c < 128) return c;
                    fail("invalid escape");
                }
                if (c == 'k' && named_groups) fail("invalid named reference");
                return c;
        }
    }
    std::unique_ptr<RNode> char_node(char32_t c) {
        auto n = std::make_unique<RNode>();
        n->t = Char;
        n->ch = c;
        return n;
    }
    std::unique_ptr<RNode> atom() {
        char32_t c = peek();
        if (c == '.') {
            i++;
            auto n = std::make_unique<RNode>();
            n->t = Any;
            return n;
        }
        if (c == '(') {
            i++;
            auto n = std::make_unique<RNode>();
            n->t = Group;
            if (s.compare(i, 2, u"?:") == 0) {
                i += 2;
                n->group = -1;
            } else if (s.compare(i, 2, u"?<") == 0) {
                i += 2;
                size_t close = s.find('>', i);
                if (close == u16::npos) fail("invalid capture group name");
                u16 name = s.substr(i, close - i);
                if (name.empty() || !is_id_start(name[0])) fail("invalid capture group name");
                for (char16_t ch : name)
                    if (!is_id_part(ch) && !(ch >= 0xD800 && ch <= 0xDFFF)) fail("invalid capture group name");
                if (std::find(prog.names.begin(), prog.names.end(), name) != prog.names.end()) fail("duplicate capture group name");
                i = close + 1;
                n->group = prog.ngroups++;
                prog.names.resize(prog.ngroups);
                prog.names[n->group] = name;
            } else if (peek() == '?') {
                fail("invalid group");
            } else {
                n->group = prog.ngroups++;
                prog.names.resize(prog.ngroups);
            }
            n->kids.push_back(disjunction());
            if (peek() != ')') fail("unterminated group");
            i++;
            return n;
        }
        if (c == ')') fail("unmatched ')'");
        if (c == '*' || c == '+' || c == '?') fail("nothing to repeat");
        if (c == '{') {
            if (u) fail("lone quantifier brackets");
            int a, b;
            size_t save = i;
            if (read_quant(a, b)) fail("nothing to repeat");
            i = save + 1;
            return char_node('{');
        }
        if (c == '}' || c == ']') {
            if (u) fail("lone quantifier brackets");
            i++;
            return char_node(c);
        }
        if (c == '[') return char_class();
        if (c == '\\') {
            i++;
            // back references
            if (!eof() && peek() >= '1' && peek() <= '9') {
                size_t save = i;
                long v = 0;
                while (!eof() && peek() >= '0' && peek() <= '9') {
                    v = std::min<long>(v * 10 + (peek() - '0'), 100000);
                    i++;
                }
                if (v < total_groups) {
                    auto n = std::make_unique<RNode>();
                    n->t = Backref;
                    n->group = int(v);
                    return n;
                }
                if (u) fail("invalid escape");
                i = save;
            }
            if (!eof() && peek() == 'k' && (u || named_groups)) {
                i++;
                if (peek() != '<') fail("invalid named reference");
                size_t close = s.find('>', i);
                if (close == u16::npos) fail("invalid named reference");
                auto n = std::make_unique<RNode>();
                n->t = Backref;
                n->backref_name = s.substr(i + 1, close - i - 1);
                if (n->backref_name.empty()) fail("invalid named reference");
                i = close + 1;
                backrefs_by_name.push_back(n.get());
                return n;
            }
            std::vector<Range> cls;
            long e = escape(&cls, false);
            if (e < 0) {
                auto n = std::make_unique<RNode>();
                n->t = Class;
                n->ranges = cls;
                return n;
            }
            return char_node(char32_t(e));
        }
        return char_node(next_cp());
    }
    long total_groups = 0;
    std::unique_ptr<RNode> char_class() {
        i++;  // [
        auto n = std::make_unique<RNode>();
        n->t = Class;
        if (peek() == '^') {
            n->negate = true;
            i++;
        }
        while (true) {
            if (eof()) fail("unterminated character class");
            if (peek() == ']') {
                i++;
                break;
            }
            long a = class_atom(n->ranges);
            if (peek() == '-' && i + 1 < s.size() && s[i + 1] != ']') {
                i++;
                long b = class_atom(n->ranges);
                if (a < 0 || b < 0) {
                    if (u) fail("invalid character class");
                    if (a >= 0) n->ranges.push_back({char32_t(a), char32_t(a)});
                    n->ranges.push_back({'-', '-'});
                    if (b >= 0) n->ranges.push_back({char32_t(b), char32_t(b)});
                    continue;
                }
                if (a > b) fail("range out of order in character class");
                n->ranges.push_back({char32_t(a), char32_t(b)});
            } else if (a >= 0) {
                n->ranges.push_back({char32_t(a), char32_t(a)});
            }
        }
        return n;
    }
    long class_atom(std::vector<Range>& r) {
        char32_t c = peek();
        if (c == '\\') {
            i++;
            return escape(&r, true);
        }
        return long(next_cp());
    }
};

// count capture groups up front (forward references)
static long count_groups(const u16& s, bool* has_named) {
    long n = 1;
    bool in_class = false;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (in_class) {
            if (c == ']') in_class = false;
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == '(') {
            if (i + 1 < s.size() && s[i + 1] == '?') {
                if (i + 2 < s.size() && s[i + 2] == '<' && i + 3 < s.size() && s[i + 3] != '=' && s[i + 3] != '!') {
                    n++;
                    *has_named = true;
                }
            } else {
                n++;
            }
        }
    }
    return n;
}

static bool compile(const u16& pattern, const u16& flags, Program& p, std::string* err) {
    for (char16_t f : flags) {
        switch (f) {
            case 'g': p.global = true; break;
            case 'i': p.icase = true; break;
            case 'm': p.multiline = true; break;
            case 's': p.dotall = true; break;
            case 'u': case 'v': p.unicode = true; break;
            case 'y': p.sticky = true; break;
            case 'd': p.indices = true; break;
            default:
                if (err) *err = "invalid flags";
                return false;
        }
    }
    std::set<char16_t> seen(flags.begin(), flags.end());
    if (seen.size() != flags.size() || (seen.count('u') && seen.count('v'))) {
        if (err) *err = "invalid flags";
        return false;
    }
    bool has_named = false;
    long groups = count_groups(pattern, &has_named);
    Parser ps{pattern, 0, p.unicode, has_named, p, {}, ""};
    ps.total_groups = groups;
    p.names.resize(1);
    try {
        p.root = ps.disjunction();
        if (!ps.eof()) ps.fail(ps.peek() == ')' ? "unmatched ')'" : "unexpected character");
        for (RNode* b : ps.backrefs_by_name) {
            auto it = std::find(p.names.begin(), p.names.end(), b->backref_name);
            if (it == p.names.end()) ps.fail("invalid named capture referenced");
            b->group = int(it - p.names.begin());
        }
    } catch (std::string& e) {
        if (err) *err = e;
        return false;
    }
    return true;
}

// ---- matching
static char32_t canon(char32_t c, bool unicode) {
    if (unicode) {
        // simple case folding toward lower case
        if (c >= 'A' && c <= 'Z') return c + 32;
        if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
        if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
        if (c >= 0x410 && c <= 0x42F) return c + 32;
        if (c == 0x17F) return 's';
        if (c == 0x212A) return 'k';
        if (c == 0x2126) return 0x3C9;
        return c;
    }
    // non-unicode: toUpperCase, unless that maps an non-ASCII char to ASCII
    char32_t u = c;
    if (c >= 'a' && c <= 'z') u = c - 32;
    else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) u = c - 32;
    else if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) u = c - 32;
    else if (c >= 0x430 && c <= 0x44F) u = c - 32;
    else if (c == 0xB5) u = 0x39C;
    else if (c == 0xFF) u = 0x178;
    if (c >= 128 && u < 128) return c;
    return u;
}

struct Matcher {
    const Program& p;
    const u16& in;
    std::vector<int> caps;  // 2 per group
    long steps = 0;

    char32_t at(int i, int* len) const {
        char32_t c = in[i];
        *len = 1;
        if (p.unicode && c >= 0xD800 && c <= 0xDBFF && i + 1 < int(in.size()) && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            *len = 2;
            return 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
        }
        return c;
    }
    char32_t before(int i, int* len) const {
        char32_t c = in[i - 1];
        *len = 1;
        if (p.unicode && c >= 0xDC00 && c <= 0xDFFF && i >= 2 && in[i - 2] >= 0xD800 && in[i - 2] <= 0xDBFF) {
            *len = 2;
            return 0x10000 + ((in[i - 2] - 0xD800) << 10) + (c - 0xDC00);
        }
        return c;
    }
    bool is_word(int i) const {
        if (i < 0 || i >= int(in.size())) return false;
        char32_t c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return true;
        if (p.unicode && p.icase && (c == 0x17F || c == 0x212A)) return true;
        return false;
    }
    bool in_class(const RNode* n, char32_t c) const {
        bool hit = false;
        for (auto& r : n->ranges) {
            if (c >= r.lo && c <= r.hi) {
                hit = true;
                break;
            }
        }
        if (!hit && p.icase) {
            char32_t cc = canon(c, p.unicode);
            for (auto& r : n->ranges) {
                // compare canonical forms; scan small ranges, test endpoints for large ones
                if (r.hi - r.lo < 600) {
                    for (char32_t x = r.lo; x <= r.hi; x++)
                        if (canon(x, p.unicode) == cc) {
                            hit = true;
                            break;
                        }
                } else if (cc >= r.lo && cc <= r.hi) {
                    hit = true;
                }
                if (hit) break;
            }
        }
        return hit != n->negate;
    }
    using K = std::function<bool(int)>;
    // backward: matching inside a lookbehind (right to left)
    bool m(const RNode* n, int i, bool back, const K& k) {
        if (++steps > 50000000) throw std::string("regular expression too complex");
        switch (n->t) {
            case Empty: return k(i);
            case Char: case Any: case Class: {
                int len;
                char32_t c;
                if (back) {
                    if (i <= 0) return false;
                    c = before(i, &len);
                } else {
                    if (i >= int(in.size())) return false;
                    c = at(i, &len);
                }
                bool ok;
                if (n->t == Char) ok = p.icase ? canon(c, p.unicode) == canon(n->ch, p.unicode) : c == n->ch;
                else if (n->t == Any) ok = p.dotall || !(c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029);
                else ok = in_class(n, c);
                if (!ok) return false;
                return k(back ? i - len : i + len);
            }
            case Seq: return seq(n, back ? int(n->kids.size()) - 1 : 0, i, back, k);
            case Alt:
                for (auto& a : n->kids)
                    if (m(a.get(), i, back, k)) return true;
                return false;
            case Group: {
                if (n->group < 0) return m(n->kids[0].get(), i, back, k);
                int g = n->group;
                int s0 = caps[2 * g], e0 = caps[2 * g + 1];
                bool r = m(n->kids[0].get(), i, back, [&, g, i](int j) {
                    int ps = caps[2 * g], pe = caps[2 * g + 1];
                    if (back) {
                        caps[2 * g] = j;
                        caps[2 * g + 1] = i;
                    } else {
                        caps[2 * g] = i;
                        caps[2 * g + 1] = j;
                    }
                    if (k(j)) return true;
                    caps[2 * g] = ps;
                    caps[2 * g + 1] = pe;
                    return false;
                });
                if (!r) {
                    caps[2 * g] = s0;
                    caps[2 * g + 1] = e0;
                }
                return r;
            }
            case Backref: {
                int g = n->group;
                int s = caps[2 * g], e = caps[2 * g + 1];
                if (s < 0 || e < 0) return k(i);
                int len = e - s;
                if (back) {
                    if (i - len < 0) return false;
                    for (int x = 0; x < len; x++) {
                        char32_t a = in[s + x], b = in[i - len + x];
                        if (p.icase ? canon(a, p.unicode) != canon(b, p.unicode) : a != b) return false;
                    }
                    return k(i - len);
                }
                if (i + len > int(in.size())) return false;
                for (int x = 0; x < len; x++) {
                    char32_t a = in[s + x], b = in[i + x];
                    if (p.icase ? canon(a, p.unicode) != canon(b, p.unicode) : a != b) return false;
                }
                return k(i + len);
            }
            case Bol:
                if (i == 0 || (p.multiline && (in[i - 1] == '\n' || in[i - 1] == '\r' || in[i - 1] == 0x2028 || in[i - 1] == 0x2029))) return k(i);
                return false;
            case Eol:
                if (i == int(in.size()) || (p.multiline && (in[i] == '\n' || in[i] == '\r' || in[i] == 0x2028 || in[i] == 0x2029))) return k(i);
                return false;
            case WordB: case NotWordB: {
                bool a = is_word(i - 1), b = is_word(i);
                if ((a != b) == (n->t == WordB)) return k(i);
                return false;
            }
            case Look: {
                std::vector<int> saved = caps;
                bool r = m(n->kids[0].get(), i, !n->ahead, [](int) { return true; });
                if (n->neg) {
                    caps = saved;
                    if (r) return false;
                    return k(i);
                }
                if (!r) {
                    caps = saved;
                    return false;
                }
                if (k(i)) return true;
                caps = saved;
                return false;
            }
            case Star: return star(n, i, back, 0, -1, k);
        }
        return false;
    }
    bool seq(const RNode* n, int idx, int i, bool back, const K& k) {
        if (back ? idx < 0 : idx >= int(n->kids.size())) return k(i);
        return m(n->kids[idx].get(), i, back, [&, idx](int j) { return seq(n, back ? idx - 1 : idx + 1, j, back, k); });
    }
    bool star(const RNode* n, int i, bool back, int count, int last, const K& k) {
        // an iteration must consume input once min is reached (empty-check)
        if (n->max >= 0 && count >= n->max) return k(i);
        auto iterate = [&]() {
            std::vector<int> saved(caps.begin() + 2 * n->first_group, caps.begin() + 2 * n->last_group);
            for (int g = n->first_group; g < n->last_group; g++) caps[2 * g] = caps[2 * g + 1] = -1;
            bool r = m(n->kids[0].get(), i, back, [&](int j) {
                if (j == i && count >= n->min) return false;
                return star(n, j, back, count + 1, i, k);
            });
            if (!r) std::copy(saved.begin(), saved.end(), caps.begin() + 2 * n->first_group);
            return r;
        };
        (void)last;
        if (count < n->min) return iterate();
        if (n->greedy) {
            if (iterate()) return true;
            return k(i);
        }
        if (k(i)) return true;
        return iterate();
    }
};

}  // namespace re

bool regexp_syntax_ok(const u16& pattern, const u16& flags, std::string* err) {
    re::Program p;
    return re::compile(pattern, flags, p, err);
}

struct RegExpObj : Obj {
    std::shared_ptr<re::Program> prog;
    Value source, flags;
    void trace(Runtime& rt) override {
        Obj::trace(rt);
        rt.mark(source);
        rt.mark(flags);
    }
};

static RegExpObj* this_regexp(Runtime& rt, Value v) {
    if (!v.is_obj() || v.o->kind != Kind::RegExp) rt.throw_error(E_TYPE, "receiver is not a RegExp");
    return (RegExpObj*)v.o;
}

static void regexp_init(Runtime& rt, RegExpObj* r, Value pattern, Value flags) {
    u16 p = pattern.is_undef() ? u"" : rt.to_string(pattern)->v;
    u16 f = flags.is_undef() ? u"" : rt.to_string(flags)->v;
    auto prog = std::make_shared<re::Program>();
    std::string err;
    if (!re::compile(p, f, *prog, &err)) rt.throw_error(E_SYNTAX, "Invalid regular expression: /" + to_utf8(p) + "/: " + err);
    r->prog = prog;
    r->source = Value(rt.str(p));
    r->flags = Value(rt.str(f));
    if (!rt.set(Value((Obj*)r), rt.key("lastIndex"), Value(0.0), true)) rt.throw_error(E_TYPE, "Cannot assign to read only property 'lastIndex'");
}

static RegExpObj* regexp_alloc(Runtime& rt, Obj* proto) {
    RegExpObj* r = rt.alloc<RegExpObj>();
    r->kind = Kind::RegExp;
    r->proto = proto;
    rt.define_data(r, "lastIndex", Value(0.0), WRITABLE);
    return r;
}

Value regexp_create(Runtime& rt, Value pattern, Value flags) {
    RegExpObj* r = regexp_alloc(rt, rt.realm.regexp_proto);
    Runtime::Root root(rt, Value((Obj*)r));
    regexp_init(rt, r, pattern, flags);
    return Value((Obj*)r);
}

static bool is_regexp(Runtime& rt, Value v) {
    if (!v.is_obj()) return false;
    Value m = rt.get(v, rt.key(rt.sym_match));
    if (!m.is_undef()) return rt.to_boolean(m);
    return v.o->kind == Kind::RegExp;
}

NATIVE(regexp_ctor) {
    UNUSED_NATIVE_ARGS;
    Value pattern = ARG(0), flags = ARG(1);
    bool pis = is_regexp(rt, pattern);
    Value newt = nt;
    if (nt.is_undef()) {
        newt = Value(callee);
        if (pis && flags.is_undef()) {
            Value c = rt.get(pattern, "constructor");
            if (c.is_obj() && c.o == callee) return pattern;
        }
    }
    Value p, f;
    if (pattern.is_obj() && pattern.o->kind == Kind::RegExp) {
        RegExpObj* src = (RegExpObj*)pattern.o;
        p = src->source;
        f = flags.is_undef() ? src->flags : flags;
    } else if (pis) {
        p = rt.get(pattern, "source");
        f = flags.is_undef() ? rt.get(pattern, "flags") : flags;
    } else {
        p = pattern;
        f = flags;
    }
    RegExpObj* r = regexp_alloc(rt, rt.get_prototype_from_ctor(newt, rt.realm.regexp_proto));
    Runtime::Root root(rt, Value((Obj*)r));
    regexp_init(rt, r, p, f);
    return Value((Obj*)r);
}

// RegExpBuiltinExec
static Value builtin_exec(Runtime& rt, RegExpObj* r, Str* s) {
    double li = double(rt.to_length(rt.get(Value((Obj*)r), "lastIndex")));
    const re::Program& p = *r->prog;
    bool global = p.global, sticky = p.sticky;
    if (!global && !sticky) li = 0;
    const u16& in = s->v;
    if (li > in.size()) {
        if (global || sticky)
            if (!rt.set(Value((Obj*)r), rt.key("lastIndex"), Value(0.0), true)) rt.throw_error(E_TYPE, "Cannot assign to read only property 'lastIndex'");
        return Value::null();
    }
    re::Matcher mt{p, in, {}, 0};
    int start = int(li), end = -1;
    try {
        while (start <= int(in.size())) {
            mt.caps.assign(2 * p.ngroups, -1);
            if (mt.m(p.root.get(), start, false, [&](int j) {
                    end = j;
                    return true;
                }))
                break;
            if (sticky) {
                start = int(in.size()) + 1;
                break;
            }
            // advance (by code point in unicode mode)
            if (p.unicode && start < int(in.size()) && in[start] >= 0xD800 && in[start] <= 0xDBFF && start + 1 < int(in.size()) &&
                in[start + 1] >= 0xDC00 && in[start + 1] <= 0xDFFF)
                start += 2;
            else start++;
        }
    } catch (std::string& e) {
        rt.throw_error(E_RANGE, e);
    }
    if (end < 0) {
        if (global || sticky)
            if (!rt.set(Value((Obj*)r), rt.key("lastIndex"), Value(0.0), true)) rt.throw_error(E_TYPE, "Cannot assign to read only property 'lastIndex'");
        return Value::null();
    }
    if (global || sticky)
        if (!rt.set(Value((Obj*)r), rt.key("lastIndex"), Value(double(end)), true)) rt.throw_error(E_TYPE, "Cannot assign to read only property 'lastIndex'");
    mt.caps[0] = start;
    mt.caps[1] = end;
    Obj* a = rt.new_array(0);
    Runtime::Root root(rt, Value(a));
    create_data(rt, a, rt.key("index"), Value(double(start)));
    create_data(rt, a, rt.key("input"), Value(s));
    for (int g = 0; g < p.ngroups; g++) {
        int cs = mt.caps[2 * g], ce = mt.caps[2 * g + 1];
        a->elems.push_back(cs < 0 ? Value() : Value(rt.str(in.substr(cs, ce - cs))));
    }
    a->length = uint32_t(a->elems.size());
    bool has_names = false;
    for (auto& n : p.names)
        if (!n.empty()) has_names = true;
    Value groups;
    if (has_names) {
        Obj* g = rt.new_object();
        g->proto = nullptr;
        for (int i = 1; i < p.ngroups; i++)
            if (!p.names[i].empty()) create_data(rt, g, rt.key(rt.intern(p.names[i])), a->elems[i]);
        groups = Value(g);
    }
    create_data(rt, a, rt.key("groups"), groups);
    if (p.indices) {
        Obj* ind = rt.new_array(0);
        for (int g = 0; g < p.ngroups; g++) {
            int cs = mt.caps[2 * g], ce = mt.caps[2 * g + 1];
            ind->elems.push_back(cs < 0 ? Value() : Value(rt.array_from_list({Value(double(cs)), Value(double(ce))})));
        }
        ind->length = uint32_t(ind->elems.size());
        Value igroups;
        if (has_names) {
            Obj* g = rt.new_object();
            g->proto = nullptr;
            for (int i = 1; i < p.ngroups; i++)
                if (!p.names[i].empty()) create_data(rt, g, rt.key(rt.intern(p.names[i])), ind->elems[i]);
            igroups = Value(g);
        }
        create_data(rt, ind, rt.key("groups"), igroups);
        create_data(rt, a, rt.key("indices"), Value(ind));
    }
    return Value(a);
}

// RegExpExec: a user exec method wins
static Value regexp_exec(Runtime& rt, Value r, Str* s) {
    Value exec = rt.get(r, "exec");
    if (rt.is_callable(exec)) {
        Value sv(s);
        Value res = rt.call(exec, r, &sv, 1);
        if (!res.is_obj() && !res.is_null()) rt.throw_error(E_TYPE, "exec result must be an object or null");
        return res;
    }
    return builtin_exec(rt, this_regexp(rt, r), s);
}

NATIVE(regexp_exec_fn) {
    UNUSED_NATIVE_ARGS;
    RegExpObj* r = this_regexp(rt, thisv);
    return builtin_exec(rt, r, rt.to_string(ARG(0)));
}
NATIVE(regexp_test) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype.test called on a non-object");
    return Value(!regexp_exec(rt, thisv, rt.to_string(ARG(0))).is_null());
}
NATIVE(regexp_to_string) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype.toString called on a non-object");
    u16 p = rt.to_string(rt.get(thisv, "source"))->v;
    u16 f = rt.to_string(rt.get(thisv, "flags"))->v;
    return Value(rt.str(u"/" + p + u"/" + f));
}
NATIVE(regexp_flags) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype.flags getter called on a non-object");
    u16 out;
    const char* names[][2] = {{"hasIndices", "d"}, {"global", "g"}, {"ignoreCase", "i"}, {"multiline", "m"}, {"dotAll", "s"},
                              {"unicode", "u"}, {"unicodeSets", "v"}, {"sticky", "y"}};
    for (auto& n : names)
        if (rt.to_boolean(rt.get(thisv, n[0]))) out += to_u16(n[1]);
    return Value(rt.str(out));
}
// flag getters: magic is the flag character
NATIVE(regexp_flag) {
    UNUSED_NATIVE_ARGS;
    char16_t f = char16_t(((Function*)callee)->native_magic);
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp flag getter called on a non-object");
    if (thisv.o->kind != Kind::RegExp) {
        if (thisv.o == rt.realm.regexp_proto) return Value();
        rt.throw_error(E_TYPE, "RegExp flag getter called on an incompatible receiver");
    }
    return Value(((RegExpObj*)thisv.o)->flags.s->v.find(f) != u16::npos);
}
NATIVE(regexp_source) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype.source getter called on a non-object");
    if (thisv.o->kind != Kind::RegExp) {
        if (thisv.o == rt.realm.regexp_proto) return Value(rt.intern("(?:)"));
        rt.throw_error(E_TYPE, "RegExp.prototype.source getter called on an incompatible receiver");
    }
    u16 s = ((RegExpObj*)thisv.o)->source.s->v;
    if (s.empty()) return Value(rt.intern("(?:)"));
    u16 o;
    bool in_class = false;
    for (size_t i = 0; i < s.size(); i++) {
        char16_t c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            o.push_back(c);
            o.push_back(s[++i]);
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        if (c == '/' && !in_class) o += u"\\/";
        else if (c == '\n') o += u"\\n";
        else if (c == '\r') o += u"\\r";
        else if (c == 0x2028) o += u"\\u2028";
        else if (c == 0x2029) o += u"\\u2029";
        else o.push_back(c);
    }
    return Value(rt.str(o));
}
NATIVE(regexp_compile) {
    UNUSED_NATIVE_ARGS;
    RegExpObj* r = this_regexp(rt, thisv);
    Value p = ARG(0), f = ARG(1);
    if (p.is_obj() && p.o->kind == Kind::RegExp) {
        if (!f.is_undef()) rt.throw_error(E_TYPE, "flags must be undefined when the pattern is a RegExp");
        f = ((RegExpObj*)p.o)->flags;
        p = ((RegExpObj*)p.o)->source;
    }
    regexp_init(rt, r, p, f);
    return thisv;
}
static double advance(const u16& s, double i, bool unicode) {
    if (!unicode || i + 1 >= s.size()) return i + 1;
    char16_t c = s[size_t(i)];
    if (c >= 0xD800 && c <= 0xDBFF && s[size_t(i) + 1] >= 0xDC00 && s[size_t(i) + 1] <= 0xDFFF) return i + 2;
    return i + 1;
}
static void set_last_index(Runtime& rt, Value r, double v) {
    if (!rt.set(r, rt.key("lastIndex"), Value(v), true)) rt.throw_error(E_TYPE, "Cannot assign to read only property 'lastIndex'");
}
NATIVE(regexp_match) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype[Symbol.match] called on a non-object");
    Str* s = rt.to_string(ARG(0));
    u16 flags = rt.to_string(rt.get(thisv, "flags"))->v;
    if (flags.find('g') == u16::npos) return regexp_exec(rt, thisv, s);
    bool unicode = flags.find('u') != u16::npos || flags.find('v') != u16::npos;
    set_last_index(rt, thisv, 0);
    Obj* a = rt.new_array(0);
    Runtime::Root root(rt, Value(a));
    while (true) {
        Value r = regexp_exec(rt, thisv, s);
        if (r.is_null()) return a->elems.empty() ? Value::null() : Value(a);
        Str* m = rt.to_string(rt.get(r, rt.key("0")));
        a->elems.push_back(Value(m));
        a->length = uint32_t(a->elems.size());
        if (m->v.empty()) {
            double li = double(rt.to_length(rt.get(thisv, "lastIndex")));
            set_last_index(rt, thisv, advance(s->v, li, unicode));
        }
    }
}
NATIVE(regexp_search) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype[Symbol.search] called on a non-object");
    Str* s = rt.to_string(ARG(0));
    Value prev = rt.get(thisv, "lastIndex");
    if (!rt.same_value(prev, Value(0.0))) set_last_index(rt, thisv, 0);
    Value r = regexp_exec(rt, thisv, s);
    Value cur = rt.get(thisv, "lastIndex");
    if (!rt.same_value(cur, prev)) set_last_index(rt, thisv, 0), rt.set(thisv, rt.key("lastIndex"), prev, true);
    if (r.is_null()) return Value(-1.0);
    return rt.get(r, "index");
}
Value string_get_substitution(Runtime& rt, const u16& matched, const u16& str, size_t pos, const std::vector<Value>& caps, Value named,
                              const u16& repl);
NATIVE(regexp_replace) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype[Symbol.replace] called on a non-object");
    Str* s = rt.to_string(ARG(0));
    Value repl = ARG(1);
    bool fn = rt.is_callable(repl);
    u16 rs = fn ? u"" : rt.to_string(repl)->v;
    u16 flags = rt.to_string(rt.get(thisv, "flags"))->v;
    bool global = flags.find('g') != u16::npos;
    bool unicode = flags.find('u') != u16::npos || flags.find('v') != u16::npos;
    if (global) set_last_index(rt, thisv, 0);
    std::vector<Value> results;
    Obj* keep = rt.new_array(0);
    Runtime::Root root(rt, Value(keep));
    while (true) {
        Value r = regexp_exec(rt, thisv, s);
        if (r.is_null()) break;
        keep->elems.push_back(r);
        results.push_back(r);
        if (!global) break;
        Str* m = rt.to_string(rt.get(r, rt.key("0")));
        if (m->v.empty()) {
            double li = double(rt.to_length(rt.get(thisv, "lastIndex")));
            set_last_index(rt, thisv, advance(s->v, li, unicode));
        }
    }
    u16 out;
    size_t next = 0;
    for (Value r : results) {
        double ncaps = std::max(double(rt.length_of(r)) - 1, 0.0);
        Str* matched = rt.to_string(rt.get(r, rt.key("0")));
        double pos = std::max(std::min(rt.to_integer(rt.get(r, "index")), double(s->v.size())), 0.0);
        std::vector<Value> caps;
        for (double n = 1; n <= ncaps; n++) {
            Value c = rt.get(r, idx_key(rt, n));
            if (!c.is_undef()) c = Value(rt.to_string(c));
            caps.push_back(c);
        }
        Value named = rt.get(r, "groups");
        u16 replacement;
        if (fn) {
            std::vector<Value> a{Value(matched)};
            a.insert(a.end(), caps.begin(), caps.end());
            a.push_back(Value(pos));
            a.push_back(Value(s));
            if (!named.is_undef()) a.push_back(named);
            replacement = rt.to_string(rt.call(repl, Value(), a))->v;
        } else {
            if (!named.is_undef()) named = Value(rt.to_object(named));
            replacement = string_get_substitution(rt, matched->v, s->v, size_t(pos), caps, named, rs).s->v;
        }
        if (pos >= next) {
            out += s->v.substr(next, size_t(pos) - next) + replacement;
            next = size_t(pos) + matched->v.size();
        }
    }
    if (next < s->v.size()) out += s->v.substr(next);
    return Value(rt.str(out));
}
NATIVE(regexp_split) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype[Symbol.split] called on a non-object");
    Str* s = rt.to_string(ARG(0));
    Obj* C = rt.species_constructor(thisv.o, rt.get(Value(rt.realm.global), "RegExp").o);
    u16 flags = rt.to_string(rt.get(thisv, "flags"))->v;
    bool unicode = flags.find('u') != u16::npos || flags.find('v') != u16::npos;
    u16 nf = flags.find('y') == u16::npos ? flags + u"y" : flags;
    Value splitter = rt.construct(Value(C), {thisv, Value(rt.str(nf))});
    Runtime::Root r0(rt, splitter);
    Obj* a = rt.new_array(0);
    Runtime::Root r1(rt, Value(a));
    uint32_t lim = ARG(1).is_undef() ? 0xFFFFFFFFu : rt.to_uint32(ARG(1));
    if (lim == 0) return Value(a);
    const u16& in = s->v;
    auto push = [&](Value v) {
        a->elems.push_back(v);
        a->length = uint32_t(a->elems.size());
    };
    if (in.empty()) {
        Value z = regexp_exec(rt, splitter, s);
        if (z.is_null()) push(Value(s));
        return Value(a);
    }
    size_t p = 0, q = 0;
    while (q < in.size()) {
        set_last_index(rt, splitter, double(q));
        Value z = regexp_exec(rt, splitter, s);
        if (z.is_null()) {
            q = size_t(advance(in, double(q), unicode));
            continue;
        }
        size_t e = size_t(std::min(double(rt.to_length(rt.get(splitter, "lastIndex"))), double(in.size())));
        if (e == p) {
            q = size_t(advance(in, double(q), unicode));
            continue;
        }
        push(Value(rt.str(in.substr(p, q - p))));
        if (a->length == lim) return Value(a);
        p = e;
        double ncaps = std::max(double(rt.length_of(z)) - 1, 0.0);
        for (double i = 1; i <= ncaps; i++) {
            push(rt.get(z, idx_key(rt, i)));
            if (a->length == lim) return Value(a);
        }
        q = p;
    }
    push(Value(rt.str(in.substr(p))));
    return Value(a);
}
// matchAll: the iterator keeps the regexp and string in elems; flags in length bits
NATIVE(regexp_match_all) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj()) rt.throw_error(E_TYPE, "RegExp.prototype[Symbol.matchAll] called on a non-object");
    Str* s = rt.to_string(ARG(0));
    Obj* C = rt.species_constructor(thisv.o, rt.get(Value(rt.realm.global), "RegExp").o);
    Str* flags = rt.to_string(rt.get(thisv, "flags"));
    Value matcher = rt.construct(Value(C), {thisv, Value(flags)});
    Runtime::Root r0(rt, matcher);
    double li = double(rt.to_length(rt.get(thisv, "lastIndex")));
    set_last_index(rt, matcher, li);
    Obj* it = rt.new_object(rt.realm.regexp_string_iterator_proto);
    it->elems = {matcher, Value(s)};
    it->length = (flags->v.find('g') != u16::npos ? 1 : 0) | (flags->v.find('u') != u16::npos || flags->v.find('v') != u16::npos ? 2 : 0);
    it->prim = Value(false);  // done
    return Value(it);
}
NATIVE(regexp_string_iterator_next) {
    UNUSED_NATIVE_ARGS;
    if (!thisv.is_obj() || thisv.o->proto != rt.realm.regexp_string_iterator_proto || thisv.o->elems.size() != 2)
        rt.throw_error(E_TYPE, "next called on incompatible receiver");
    Obj* it = thisv.o;
    if (it->prim.b) return Value(rt.iter_result(Value(), true));
    Value r = it->elems[0];
    Str* s = it->elems[1].s;
    Value m = regexp_exec(rt, r, s);
    if (m.is_null()) {
        it->prim = Value(true);
        return Value(rt.iter_result(Value(), true));
    }
    if (!(it->length & 1)) {
        it->prim = Value(true);
        return Value(rt.iter_result(m, false));
    }
    Str* ms = rt.to_string(rt.get(m, rt.key("0")));
    if (ms->v.empty()) {
        double li = double(rt.to_length(rt.get(r, "lastIndex")));
        set_last_index(rt, r, advance(s->v, li, it->length & 2));
    }
    return Value(rt.iter_result(m, false));
}
NATIVE(regexp_species) {
    UNUSED_NATIVE_ARGS;
    return thisv;
}

void init_regexp(Runtime& rt) {
    Realm& R = rt.realm;
    R.regexp_proto = rt.new_object();
    Function* c = constructor(rt, "RegExp", regexp_ctor, 2, R.regexp_proto);
    getter(rt, c, rt.key(rt.sym_species), "get [Symbol.species]", regexp_species);
    Obj* P = R.regexp_proto;
    method(rt, P, "compile", regexp_compile, 2);
    method(rt, P, "exec", regexp_exec_fn, 1);
    method(rt, P, "test", regexp_test, 1);
    method(rt, P, "toString", regexp_to_string, 0);
    getter(rt, P, rt.key("flags"), "get flags", regexp_flags);
    getter(rt, P, rt.key("source"), "get source", regexp_source);
    const char* fl[][2] = {{"global", "g"}, {"ignoreCase", "i"}, {"multiline", "m"}, {"dotAll", "s"}, {"unicode", "u"},
                           {"unicodeSets", "v"}, {"sticky", "y"}, {"hasIndices", "d"}};
    for (auto& f : fl) {
        Function* g = rt.new_native(("get " + std::string(f[0])).c_str(), regexp_flag, 0, f[1][0]);
        Desc d;
        d.has_get = d.has_set = d.has_enumerable = d.has_configurable = true;
        d.get = Value(g);
        d.configurable = true;
        rt.define(P, rt.key(f[0]), d);
    }
    method(rt, P, rt.sym_match, "[Symbol.match]", regexp_match, 1);
    method(rt, P, rt.sym_match_all, "[Symbol.matchAll]", regexp_match_all, 1);
    method(rt, P, rt.sym_replace, "[Symbol.replace]", regexp_replace, 2);
    method(rt, P, rt.sym_search, "[Symbol.search]", regexp_search, 1);
    method(rt, P, rt.sym_split, "[Symbol.split]", regexp_split, 2);
    R.regexp_string_iterator_proto = rt.new_object(R.iterator_proto);
    method(rt, R.regexp_string_iterator_proto, "next", regexp_string_iterator_next, 0);
    rt.define_data(R.regexp_string_iterator_proto, rt.key(rt.sym_to_string_tag), Value(rt.intern("RegExp String Iterator")), CONFIGURABLE);
}

}  // namespace kjs
