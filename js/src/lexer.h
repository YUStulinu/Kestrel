#pragma once
#include "ast.h"

namespace kjs {

struct Lexer {
    const u16& src;
    size_t pos = 0;
    int line = 1;
    bool module = false;
    std::vector<Token> toks;
    std::vector<bool> braces;  // true: a template substitution's ${

    explicit Lexer(const u16& s);
    std::vector<Token> run();

   private:
    char32_t cp_at(size_t i, int* len) const;
    [[noreturn]] void fail(const std::string& m);
    long read_unicode_escape();
    bool regex_allowed() const;
    void skip_space(bool& nl);
    void read_ident(Token& t);
    void read_number(Token& t);
    bool read_escape(u16& out, bool in_template, bool& octal);
    void read_string(Token& t, char16_t q);
    void read_template(Token& t);
    void read_regex(Token& t);
};

}  // namespace kjs
