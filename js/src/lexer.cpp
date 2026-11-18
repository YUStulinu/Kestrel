// The kjs lexer: the whole source is tokenized up front. Whether "/" starts a
// regular expression is decided from the previous token, and a brace stack
// tells template substitutions apart from blocks.
#include <cstring>

#include "ast.h"
#include "lexer.h"

namespace kjs {

bool is_line_term(char32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }
bool is_js_space(char32_t c) {
    return c == '\t' || c == 0x0B || c == 0x0C || c == ' ' || c == 0xA0 || c == 0xFEFF || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}
// approximations of ID_Start / ID_Continue outside ASCII
bool is_id_start(char32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
    if (is_js_space(c) || is_line_term(c)) return false;
    if (c == 0x2E2F) return false;  // VERTICAL TILDE is Pattern_Syntax
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F && c != 0x3005 && c != 0x3006 && c != 0x3007) ||
        (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xD800 && c <= 0xDFFF) || c == 0xD7 || c == 0xF7 || (c >= 0x300 && c <= 0x36F))
        return false;
    if (c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) return false;
    return true;
}
bool is_id_part(char32_t c) {
    if (c < 128) return is_id_start(c) || (c >= '0' && c <= '9');
    if (c == 0x200C || c == 0x200D) return true;
    if (c >= 0x300 && c <= 0x36F) return true;  // combining marks
    if (c == 0xB7 || c == 0x387 || (c >= 0x1369 && c <= 0x1371) || c == 0x19DA) return true;
    return is_id_start(c);
}

static const char* KEYWORDS[] = {"await", "break", "case", "catch", "class", "const", "continue", "debugger", "default",
                                 "delete", "do", "else", "enum", "export", "extends", "false", "finally", "for",
                                 "function", "if", "import", "in", "instanceof", "new", "null", "return", "super",
                                 "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with",
                                 "yield", "let", "static", "implements", "interface", "package", "private",
                                 "protected", "public", "async", "of", "get", "set", "target", "meta", "from", "as",
                                 "using", nullptr};

Lexer::Lexer(const u16& s) : src(s) {}

char32_t Lexer::cp_at(size_t i, int* len) const {
    if (i >= src.size()) {
        *len = 0;
        return 0;
    }
    char16_t c = src[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < src.size() && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
        *len = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00);
    }
    *len = 1;
    return c;
}

[[noreturn]] void Lexer::fail(const std::string& m) { throw SyntaxError{m, line}; }

static void push_cp(u16& s, char32_t c) {
    if (c >= 0x10000) {
        c -= 0x10000;
        s.push_back(char16_t(0xD800 + (c >> 10)));
        s.push_back(char16_t(0xDC00 + (c & 0x3FF)));
    } else {
        s.push_back(char16_t(c));
    }
}

static int hexval(char32_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// \uXXXX or \u{X...}; pos is at 'u'. Returns -1 on error
long Lexer::read_unicode_escape() {
    pos++;  // u
    if (pos < src.size() && src[pos] == '{') {
        pos++;
        long v = 0;
        int n = 0;
        while (pos < src.size() && src[pos] != '}') {
            int h = hexval(src[pos]);
            if (h < 0) return -1;
            v = v * 16 + h;
            if (v > 0x10FFFF) return -1;
            pos++;
            n++;
        }
        if (pos >= src.size() || n == 0) return -1;
        pos++;
        return v;
    }
    long v = 0;
    for (int i = 0; i < 4; i++) {
        if (pos >= src.size()) return -1;
        int h = hexval(src[pos]);
        if (h < 0) return -1;
        v = v * 16 + h;
        pos++;
    }
    return v;
}

bool Lexer::regex_allowed() const {
    if (toks.empty()) return true;
    const Token& t = toks.back();
    switch (t.type) {
        case T::Num: case T::BigInt: case T::String: case T::Regex: case T::Private: return false;
        case T::Template: return !t.tmpl_tail;
        case T::Ident: return false;
        case T::Keyword: {
            static const char* vals[] = {"this", "super", "null", "true", "false", "of", "get", "set", "static",
                                         "async", "let", "target", "meta", "from", "as", "using", nullptr};
            for (int i = 0; vals[i]; i++)
                if (t.value == to_u16(vals[i])) return false;
            return true;
        }
        case T::Punct:
            if (t.value == u")" || t.value == u"]") return false;
            if (t.value == u"}") return true;
            if (t.value == u"++" || t.value == u"--") return false;
            return true;
        default: return true;
    }
}

void Lexer::skip_space(bool& nl) {
    while (pos < src.size()) {
        int l;
        char32_t c = cp_at(pos, &l);
        if (is_line_term(c)) {
            nl = true;
            if (c == '\r' && pos + 1 < src.size() && src[pos + 1] == '\n') pos++;
            pos++;
            line++;
            // HTML-like close comment at line start: --> ...
            size_t p = pos;
            while (p < src.size() && is_js_space(src[p])) p++;
            if (!module && p + 2 < src.size() + 0 && src.compare(p, 3, u"-->") == 0) {
                pos = p;
                while (pos < src.size() && !is_line_term(src[pos])) pos++;
            }
        } else if (is_js_space(c)) {
            pos += l;
        } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
            while (pos < src.size() && !is_line_term(src[pos])) pos++;
        } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*') {
            pos += 2;
            bool closed = false;
            while (pos < src.size()) {
                if (src[pos] == '*' && pos + 1 < src.size() && src[pos + 1] == '/') {
                    pos += 2;
                    closed = true;
                    break;
                }
                if (is_line_term(src[pos])) {
                    nl = true;
                    line++;
                }
                pos++;
            }
            if (!closed) fail("unterminated comment");
            // a multi-line comment followed by --> on the same line
            if (nl) {
                size_t p = pos;
                while (p < src.size() && is_js_space(src[p])) p++;
                if (!module && src.compare(p, 3, u"-->") == 0) {
                    pos = p;
                    while (pos < src.size() && !is_line_term(src[pos])) pos++;
                }
            }
        } else if (!module && c == '<' && src.compare(pos, 4, u"<!--") == 0) {
            while (pos < src.size() && !is_line_term(src[pos])) pos++;
        } else if (!module && c == '-' && (nl || toks.empty()) && src.compare(pos, 3, u"-->") == 0) {
            while (pos < src.size() && !is_line_term(src[pos])) pos++;
        } else {
            break;
        }
    }
}

void Lexer::read_ident(Token& t) {
    bool escaped = false;
    bool first = true;
    while (pos < src.size()) {
        int l;
        char32_t c = cp_at(pos, &l);
        if (c == '\\') {
            if (pos + 1 >= src.size() || src[pos + 1] != 'u') fail("bad escape in identifier");
            pos++;  // the backslash; read_unicode_escape starts at the u
            long v = read_unicode_escape();
            if (v < 0) fail("bad unicode escape");
            if (first ? !is_id_start(char32_t(v)) : !is_id_part(char32_t(v))) fail("bad identifier escape");
            push_cp(t.value, char32_t(v));
            escaped = true;
        } else if (first ? is_id_start(c) : is_id_part(c)) {
            push_cp(t.value, c);
            pos += l;
        } else {
            break;
        }
        first = false;
    }
    t.escaped = escaped;
}

void Lexer::read_number(Token& t) {
    size_t s = pos;
    auto digits = [&](int base, bool allow_sep) {
        std::string out;
        bool last_sep = true;  // no leading separator
        while (pos < src.size()) {
            char16_t c = src[pos];
            if (c == '_' && allow_sep) {
                if (last_sep) fail("bad numeric separator");
                last_sep = true;
                pos++;
                continue;
            }
            int h = hexval(c);
            if (h < 0 || h >= base) break;
            out.push_back(char(c));
            last_sep = false;
            pos++;
        }
        if (!out.empty() && last_sep) fail("trailing numeric separator");
        return out;
    };
    char16_t c0 = src[pos];
    if (c0 == '0' && pos + 1 < src.size() && (src[pos + 1] | 32) != 0 &&
        strchr("xob", char(src[pos + 1] | 32)) && src[pos + 1] != 0) {
        int base = (src[pos + 1] | 32) == 'x' ? 16 : (src[pos + 1] | 32) == 'o' ? 8 : 2;
        pos += 2;
        std::string d = digits(base, true);
        if (d.empty()) fail("missing digits");
        if (pos < src.size() && src[pos] == 'n') {
            pos++;
            t.type = T::BigInt;
            t.value = to_u16(std::to_string(base) + ":" + d);
        } else {
            double v = 0;
            for (char ch : d) v = v * base + hexval(ch);
            t.type = T::Num;
            t.num = v;
        }
    } else if (c0 == '0' && pos + 1 < src.size() && src[pos + 1] >= '0' && src[pos + 1] <= '9') {
        // legacy octal (or a decimal with a leading zero when 8 or 9 appears)
        pos++;
        std::string d;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') d.push_back(char(src[pos++]));
        t.legacy_octal = true;
        bool octal = d.find_first_of("89") == std::string::npos;
        if (octal) {
            double v = 0;
            for (char ch : d) v = v * 8 + (ch - '0');
            t.num = v;
        } else {
            // NonOctalDecimalIntegerLiteral, may have a fraction and exponent
            std::string num = "0" + d;
            if (pos < src.size() && src[pos] == '.') {
                num.push_back('.');
                pos++;
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') num.push_back(char(src[pos++]));
            }
            if (pos < src.size() && (src[pos] | 32) == 'e') {
                num.push_back('e');
                pos++;
                if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) num.push_back(char(src[pos++]));
                while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') num.push_back(char(src[pos++]));
            }
            t.num = strtod(num.c_str(), nullptr);
        }
        t.type = T::Num;
    } else {
        std::string num = digits(10, true);
        bool is_int = true;
        if (pos < src.size() && src[pos] == '.') {
            is_int = false;
            pos++;
            if (pos < src.size() && src[pos] == '_') fail("bad numeric separator");
            num += "." + digits(10, true);
        }
        if (pos < src.size() && (src[pos] | 32) == 'e') {
            is_int = false;
            pos++;
            num.push_back('e');
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) num.push_back(char(src[pos++]));
            std::string e = digits(10, true);
            if (e.empty()) fail("missing exponent");
            num += e;
        }
        if (pos < src.size() && src[pos] == 'n') {
            if (!is_int || (num.size() > 1 && num[0] == '0')) fail("bad BigInt literal");
            pos++;
            t.type = T::BigInt;
            t.value = to_u16("10:" + num);
        } else {
            t.type = T::Num;
            t.num = strtod(num.c_str(), nullptr);
        }
    }
    (void)s;
    if (pos < src.size()) {
        int l;
        char32_t c = cp_at(pos, &l);
        if (is_id_start(c) || (c >= '0' && c <= '9') || c == '\\') fail("identifier directly after number");
    }
}

// one escape sequence after '\'; returns false when the escape is invalid
// (templates keep going with cooked=undefined)
bool Lexer::read_escape(u16& out, bool in_template, bool& octal) {
    char16_t c = src[pos];
    switch (c) {
        case 'n': out.push_back('\n'); pos++; return true;
        case 't': out.push_back('\t'); pos++; return true;
        case 'r': out.push_back('\r'); pos++; return true;
        case 'b': out.push_back('\b'); pos++; return true;
        case 'f': out.push_back('\f'); pos++; return true;
        case 'v': out.push_back('\v'); pos++; return true;
        case '\r':
            pos++;
            if (pos < src.size() && src[pos] == '\n') pos++;
            line++;
            return true;
        case '\n': case 0x2028: case 0x2029: pos++; line++; return true;
        case 'x': {
            if (pos + 2 >= src.size() + 0 || hexval(src[pos + 1]) < 0 || pos + 2 >= src.size() || hexval(src[pos + 2]) < 0) {
                pos++;
                return false;
            }
            out.push_back(char16_t(hexval(src[pos + 1]) * 16 + hexval(src[pos + 2])));
            pos += 3;
            return true;
        }
        case 'u': {
            size_t save = pos;
            long v = read_unicode_escape();
            if (v < 0) {
                pos = save + 1;
                return false;
            }
            push_cp(out, char32_t(v));
            return true;
        }
        default:
            if (c >= '0' && c <= '7') {
                if (c == '0' && !(pos + 1 < src.size() && src[pos + 1] >= '0' && src[pos + 1] <= '9')) {
                    out.push_back(0);
                    pos++;
                    return true;
                }
                if (in_template) {
                    pos++;
                    return false;
                }
                octal = true;
                int v = 0, n = 0;
                int maxn = c <= '3' ? 3 : 2;
                while (n < maxn && pos < src.size() && src[pos] >= '0' && src[pos] <= '7') {
                    v = v * 8 + (src[pos] - '0');
                    pos++;
                    n++;
                }
                out.push_back(char16_t(v));
                return true;
            }
            if (c == '8' || c == '9') {
                if (in_template) {
                    pos++;
                    return false;
                }
                octal = true;
                out.push_back(c);
                pos++;
                return true;
            }
            {
                int l;
                char32_t cp = cp_at(pos, &l);
                push_cp(out, cp);
                pos += l;
            }
            return true;
    }
}

void Lexer::read_string(Token& t, char16_t q) {
    pos++;
    t.type = T::String;
    while (true) {
        if (pos >= src.size()) fail("unterminated string");
        char16_t c = src[pos];
        if (c == q) {
            pos++;
            break;
        }
        if (c == '\n' || c == '\r') fail("unterminated string");
        if (c == '\\') {
            pos++;
            if (pos >= src.size()) fail("unterminated string");
            bool octal = false;
            if (!read_escape(t.value, false, octal)) fail("bad escape sequence");
            if (octal) t.legacy_octal = true;
        } else {
            t.value.push_back(c);
            pos++;
        }
    }
}

// reads a template part starting right after ` or }
void Lexer::read_template(Token& t) {
    t.type = T::Template;
    bool invalid = false;
    size_t raw_start = pos;
    while (true) {
        if (pos >= src.size()) fail("unterminated template");
        char16_t c = src[pos];
        if (c == '`') {
            t.raw = src.substr(raw_start, pos - raw_start);
            pos++;
            t.tmpl_tail = true;
            break;
        }
        if (c == '$' && pos + 1 < src.size() && src[pos + 1] == '{') {
            t.raw = src.substr(raw_start, pos - raw_start);
            pos += 2;
            braces.push_back(true);
            break;
        }
        if (c == '\\') {
            pos++;
            if (pos >= src.size()) fail("unterminated template");
            bool octal = false;
            if (!read_escape(t.value, true, octal)) invalid = true;
        } else if (c == '\r') {
            // CR and CRLF become LF in both cooked and raw values
            t.value.push_back('\n');
            pos++;
            if (pos < src.size() && src[pos] == '\n') pos++;
            line++;
        } else {
            if (c == '\n') line++;
            t.value.push_back(c);
            pos++;
        }
    }
    // normalize CR in raw
    u16 raw;
    for (size_t i = 0; i < t.raw.size(); i++) {
        if (t.raw[i] == '\r') {
            raw.push_back('\n');
            if (i + 1 < t.raw.size() && t.raw[i + 1] == '\n') i++;
        } else {
            raw.push_back(t.raw[i]);
        }
    }
    t.raw = raw;
    t.cooked_invalid = invalid;
}

void Lexer::read_regex(Token& t) {
    pos++;  // '/'
    bool in_class = false;
    while (true) {
        if (pos >= src.size() || is_line_term(src[pos])) fail("unterminated regular expression");
        char16_t c = src[pos];
        if (c == '\\') {
            t.value.push_back(c);
            pos++;
            if (pos >= src.size() || is_line_term(src[pos])) fail("unterminated regular expression");
            t.value.push_back(src[pos++]);
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) {
            pos++;
            break;
        }
        t.value.push_back(c);
        pos++;
    }
    while (pos < src.size()) {
        int l;
        char32_t c = cp_at(pos, &l);
        if (c == '\\') fail("escape in regex flags");
        if (!is_id_part(c)) break;
        push_cp(t.raw, c);
        pos += l;
    }
    t.type = T::Regex;
}

static const char* PUNCTS[] = {">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "??=", "=>", "==",
                               "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=",
                               "|=", "^=", "<<", ">>", "**", "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+",
                               "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", ":", "=", ".", "@", nullptr};

std::vector<Token> Lexer::run() {
    // a hashbang comment at the very start
    if (src.compare(0, 2, u"#!") == 0) {
        while (pos < src.size() && !is_line_term(src[pos])) pos++;
    }
    while (true) {
        bool nl = false;
        skip_space(nl);
        Token t;
        t.nl_before = nl;
        t.start = uint32_t(pos);
        t.line = line;
        if (pos >= src.size()) {
            t.type = T::Eof;
            toks.push_back(t);
            break;
        }
        int l;
        char32_t c = cp_at(pos, &l);
        if (is_id_start(c) || c == '\\') {
            read_ident(t);
            t.type = T::Ident;
            if (!t.escaped) {
                for (int i = 0; KEYWORDS[i]; i++)
                    if (t.value == to_u16(KEYWORDS[i])) t.type = T::Keyword;
            } else {
                for (int i = 0; KEYWORDS[i]; i++)
                    if (t.value == to_u16(KEYWORDS[i])) t.type = T::Keyword;
            }
        } else if (c == '#') {
            pos++;
            int l2;
            char32_t c2 = cp_at(pos, &l2);
            if (!(is_id_start(c2) || c2 == '\\')) fail("bad private name");
            read_ident(t);
            t.type = T::Private;
        } else if ((c >= '0' && c <= '9') || (c == '.' && pos + 1 < src.size() && src[pos + 1] >= '0' && src[pos + 1] <= '9')) {
            if (c == '.') {
                // .5
                size_t s = pos;
                pos++;
                std::string num = "0.";
                bool last_sep = false;
                while (pos < src.size() && ((src[pos] >= '0' && src[pos] <= '9') || src[pos] == '_')) {
                    if (src[pos] == '_') {
                        if (last_sep) fail("bad numeric separator");
                        last_sep = true;
                        pos++;
                        continue;
                    }
                    last_sep = false;
                    num.push_back(char(src[pos++]));
                }
                if (last_sep) fail("bad numeric separator");
                if (pos < src.size() && (src[pos] | 32) == 'e') {
                    num.push_back('e');
                    pos++;
                    if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) num.push_back(char(src[pos++]));
                    size_t d0 = num.size();
                    while (pos < src.size() && ((src[pos] >= '0' && src[pos] <= '9') || src[pos] == '_')) {
                        if (src[pos] != '_') num.push_back(char(src[pos]));
                        pos++;
                    }
                    if (num.size() == d0) fail("missing exponent");
                }
                t.type = T::Num;
                t.num = strtod(num.c_str(), nullptr);
                (void)s;
                int l3;
                char32_t c3 = cp_at(pos, &l3);
                if (is_id_start(c3) || (c3 >= '0' && c3 <= '9')) fail("identifier directly after number");
            } else {
                read_number(t);
            }
        } else if (c == '"' || c == '\'') {
            read_string(t, char16_t(c));
        } else if (c == '`') {
            pos++;
            t.tmpl_head = true;
            read_template(t);
        } else if (c == '}' && !braces.empty() && braces.back()) {
            braces.pop_back();
            pos++;
            read_template(t);
        } else if (c == '/' && regex_allowed()) {
            read_regex(t);
        } else {
            t.type = T::Punct;
            bool found = false;
            for (int i = 0; PUNCTS[i]; i++) {
                size_t n = strlen(PUNCTS[i]);
                bool m = pos + n <= src.size();
                for (size_t j = 0; m && j < n; j++)
                    if (src[pos + j] != char16_t(PUNCTS[i][j])) m = false;
                if (!m) continue;
                // ?. followed by a digit is ? then .5
                if (n == 2 && PUNCTS[i][0] == '?' && PUNCTS[i][1] == '.' && pos + 2 < src.size() && src[pos + 2] >= '0' &&
                    src[pos + 2] <= '9')
                    continue;
                t.value = to_u16(PUNCTS[i]);
                pos += n;
                found = true;
                break;
            }
            if (!found) fail("unexpected character");
            if (t.value == u"{") braces.push_back(false);
            else if (t.value == u"}" && !braces.empty()) braces.pop_back();
        }
        t.end = uint32_t(pos);
        toks.push_back(std::move(t));
    }
    return std::move(toks);
}

}  // namespace kjs
