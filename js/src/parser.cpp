// The kjs parser: recursive descent over the token vector, with a cover
// grammar for arrow parameters and destructuring assignment. Scope-dependent
// early errors (redeclarations) are checked by the compiler.
#include <algorithm>
#include <map>
#include <set>

#include "ast.h"
#include "lexer.h"

namespace kjs {

namespace {

const char* ALWAYS_RESERVED[] = {"break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete",
                                 "do", "else", "enum", "export", "extends", "false", "finally", "for", "function", "if",
                                 "import", "in", "instanceof", "new", "null", "return", "super", "switch", "this",
                                 "throw", "true", "try", "typeof", "var", "void", "while", "with", nullptr};
const char* STRICT_RESERVED[] = {"implements", "interface", "let", "package", "private", "protected", "public", "static",
                                 "yield", nullptr};

bool in_list(const u16& s, const char** list) {
    for (int i = 0; list[i]; i++)
        if (s == to_u16(list[i])) return true;
    return false;
}

struct FnCtx {
    bool is_async = false, is_gen = false, is_arrow = false;
    bool allow_super_prop = false, allow_super_call = false, allow_new_target = false;
    bool in_class_field = false;  // arguments forbidden
    bool in_static_block = false;
    bool in_params = false;
    bool is_function = false;     // false for the script / eval top level
    int loop = 0, sw = 0;
    std::vector<std::pair<u16, bool>> labels;  // name, labels a loop
    Node* node = nullptr;
};

struct ClassCtx {
    std::set<u16> declared;
    std::vector<std::pair<u16, int>> used;  // names referenced inside, with a line
    std::map<u16, int> kinds;               // 1 getter, 2 setter, 3 other (for duplicate checks)
};

struct Parser {
    Ast& ast;
    std::vector<Token> toks;
    size_t i = 0;
    bool strict = false;
    std::vector<FnCtx> fns;
    std::vector<ClassCtx> classes;
    std::vector<u16> outer_private;  // eval inside a class body

    Parser(Ast& a) : ast(a) {}

    // ---------------------------------------------------------- tokens
    const Token& peek(size_t k = 0) const { return toks[std::min(i + k, toks.size() - 1)]; }
    const Token& next() { return toks[std::min(i++, toks.size() - 1)]; }
    [[noreturn]] void fail(const std::string& m) { throw SyntaxError{m, peek().line}; }
    [[noreturn]] void fail_at(const Token& t, const std::string& m) { throw SyntaxError{m, t.line}; }
    bool is_p(const char* s, size_t k = 0) const { return peek(k).type == T::Punct && peek(k).value == to_u16(s); }
    bool is_kw(const char* s, size_t k = 0) const {
        const Token& t = peek(k);
        return t.type == T::Keyword && !t.escaped && t.value == to_u16(s);
    }
    // a contextual word (escapes allowed only where noted)
    bool is_word(const char* s, size_t k = 0) const {
        const Token& t = peek(k);
        return (t.type == T::Keyword || t.type == T::Ident) && t.value == to_u16(s);
    }
    bool eat_p(const char* s) {
        if (is_p(s)) {
            i++;
            return true;
        }
        return false;
    }
    bool eat_kw(const char* s) {
        if (is_kw(s)) {
            i++;
            return true;
        }
        return false;
    }
    void expect_p(const char* s) {
        if (!eat_p(s)) fail(std::string("expected '") + s + "'");
    }
    void expect_kw(const char* s) {
        if (!eat_kw(s)) fail(std::string("expected '") + s + "'");
    }
    void semicolon() {
        if (eat_p(";")) return;
        if (is_p("}") || peek().type == T::Eof || peek().nl_before) return;
        fail("missing semicolon");
    }
    Node* make(N t, const Token& at) {
        Node* n = ast.make(t);
        n->start = at.start;
        n->line = at.line;
        return n;
    }
    Node* finish(Node* n) {
        n->end = i > 0 ? toks[i - 1].end : 0;
        return n;
    }
    FnCtx& fn() { return fns.back(); }
    bool in_gen() const { return !fns.empty() && fns.back().is_gen; }
    bool in_async() const { return !fns.empty() && fns.back().is_async; }

    // ---------------------------------------------------------- identifiers
    bool await_reserved() const { return in_async() || (!fns.empty() && fns.back().in_static_block); }
    bool ident_ok(const Token& t) const {
        if (t.type == T::Ident) return true;
        if (t.type != T::Keyword) return false;
        const u16& v = t.value;
        if (in_list(v, ALWAYS_RESERVED)) return false;
        if (strict && in_list(v, STRICT_RESERVED)) return false;
        if (v == u"yield" && (in_gen() || strict)) return false;
        if (v == u"await" && await_reserved()) return false;
        return true;
    }
    bool at_ident() const { return ident_ok(peek()); }
    // an IdentifierReference / BindingIdentifier
    u16 ident(bool binding = false) {
        const Token& t = peek();
        if (!ident_ok(t)) {
            if (t.type == T::Keyword && t.escaped) fail("escaped reserved word");
            fail("expected identifier");
        }
        if (t.type == T::Keyword && t.value == u"yield" && fns.size() && fn().in_params && in_gen()) fail("yield in parameters");
        if (t.type == T::Keyword && t.value == u"await" && fns.size() && fn().in_params && in_async()) fail("await in parameters");
        if (binding && strict && (t.value == u"eval" || t.value == u"arguments")) fail("eval or arguments as binding name");
        if (t.value == u"arguments" && !fns.empty() && in_class_field_scope()) fail("arguments in class field");
        i++;
        return t.value;
    }
    bool in_class_field_scope() const {
        for (auto it = fns.rbegin(); it != fns.rend(); ++it) {
            if (it->in_class_field) return true;
            if (!it->is_arrow) return false;
        }
        return false;
    }
    // IdentifierName (property names): any identifier or keyword
    bool at_ident_name() const { return peek().type == T::Ident || peek().type == T::Keyword; }

    // ---------------------------------------------------------- program
    void program(const ParseOptions& opt) {
        Lexer lx(ast.source);
        toks = lx.run();
        strict = opt.strict;
        outer_private = opt.private_names;
        FnCtx top;
        top.is_function = opt.in_function;
        top.allow_new_target = opt.allow_new_target;
        top.allow_super_prop = opt.allow_super_property;
        top.allow_super_call = opt.allow_super_call;
        top.in_class_field = opt.in_class_field;
        fns.push_back(top);
        Node* p = make(N::Program, peek());
        if (directives(p->kids)) strict = true;
        while (peek().type != T::Eof) p->kids.push_back(statement_list_item());
        if (strict) p->flags |= F_STRICT;
        ast.root = finish(p);
        if (!classes.empty()) fail("unbalanced class scope");
    }

    // parses the directive prologue; returns true if it contains "use strict"
    bool directives(std::vector<Node*>& out) {
        bool use_strict = false;
        bool saw_octal = false;
        while (peek().type == T::String) {
            const Token& t = peek();
            // a directive is a string literal expression statement on its own
            const Token& after = peek(1);
            bool ends = (after.type == T::Punct && (after.value == u";" || after.value == u"}")) || after.type == T::Eof || after.nl_before;
            if (!ends) break;
            if (after.nl_before && after.type == T::Punct && after.value != u";" && after.value != u"}" ) {
                // e.g. "a"\n + 1 : an expression, not a directive
                static const char* cont[] = {"+", "-", "*", "/", "%", ".", "[", "(", "?", "=", ",", "==", "===", "!=", "!==",
                                             "<", ">", "&&", "||", "??", "&", "|", "^", "**", "<<", ">>", ">>>", "in", nullptr};
                bool c = false;
                for (int k = 0; cont[k]; k++)
                    if (after.value == to_u16(cont[k])) c = true;
                if (c) break;
            }
            u16 raw = ast.source.substr(t.start + 1, t.end - t.start - 2);
            if (raw == u"use strict") use_strict = true;
            if (t.legacy_octal) saw_octal = true;
            if (use_strict && saw_octal) fail("octal escape in strict code");
            out.push_back(statement());
        }
        return use_strict;
    }

    // ---------------------------------------------------------- statements
    Node* statement_list_item() {
        const Token& t = peek();
        if (is_kw("function")) return function_decl(false, t);
        if (is_word("async") && !peek().escaped && is_kw("function", 1) && !peek(1).nl_before) return function_decl(true, t);
        if (is_kw("class")) return class_decl();
        if (is_kw("const")) return var_decl(F_CONST, true);
        if (is_let_decl()) return var_decl(F_LET, true);
        if (is_kw("import") && !is_p("(", 1) && !is_p(".", 1)) fail("import declaration outside a module");
        if (is_kw("export")) fail("export outside a module");
        return statement();
    }
    bool is_let_decl() const {
        if (!is_word("let")) return false;
        const Token& n = peek(1);
        if (n.type == T::Punct && (n.value == u"[" || n.value == u"{")) return true;
        if (n.type == T::Ident || (n.type == T::Keyword && !in_list(n.value, ALWAYS_RESERVED)) ||
            (n.type == T::Keyword && (n.value == u"yield" || n.value == u"await" || n.value == u"let"))) {
            if (peek().escaped) return !strict ? false : true;
            if (n.nl_before && !strict) {
                // `let` newline identifier is still a declaration
                return true;
            }
            return true;
        }
        return false;
    }

    Node* statement() {
        const Token& t = peek();
        if (t.type == T::Punct) {
            if (t.value == u"{") return block();
            if (t.value == u";") {
                i++;
                return finish(make(N::Empty, t));
            }
        }
        if (t.type == T::Keyword && !t.escaped) {
            const u16& v = t.value;
            if (v == u"var") return var_decl(0, true);
            if (v == u"if") return if_stmt();
            if (v == u"for") return for_stmt();
            if (v == u"while") return while_stmt();
            if (v == u"do") return do_stmt();
            if (v == u"return") return return_stmt();
            if (v == u"break" || v == u"continue") return jump_stmt(v == u"break");
            if (v == u"throw") return throw_stmt();
            if (v == u"try") return try_stmt();
            if (v == u"switch") return switch_stmt();
            if (v == u"with") return with_stmt();
            if (v == u"debugger") {
                i++;
                semicolon();
                return finish(make(N::Debugger, t));
            }
            if (v == u"function") fail("function declaration in statement position");
            if (v == u"class") fail("class declaration in statement position");
            if (v == u"const") fail("lexical declaration in statement position");
            if (v == u"let" && is_p("[", 1)) fail("let [ in statement position");
            if (v == u"async" && is_kw("function", 1) && !peek(1).nl_before) fail("async function in statement position");
        }
        if (t.type == T::Keyword && t.escaped && in_list(t.value, ALWAYS_RESERVED)) fail("escaped keyword");
        // labelled statement
        if (at_ident() && is_p(":", 1)) {
            u16 name = ident();
            if (name == u"await" && await_reserved()) fail("await label");
            i++;  // :
            for (auto& l : fn().labels)
                if (l.first == name) fail("duplicate label");
            Node* n = make(N::Labeled, t);
            n->str = name;
            bool loop = is_kw("for") || is_kw("while") || is_kw("do");
            fn().labels.push_back({name, loop});
            if (is_kw("function")) {
                if (strict) fail("labelled function in strict code");
                n->a = function_decl(false, peek());
                if (n->a->flags & (F_ASYNC | F_GENERATOR)) fail("labelled generator");
            } else {
                n->a = statement();
            }
            fn().labels.pop_back();
            return finish(n);
        }
        Node* n = make(N::ExprStmt, t);
        n->a = expression(false);
        semicolon();
        return finish(n);
    }

    Node* block() {
        Node* n = make(N::Block, peek());
        expect_p("{");
        while (!is_p("}")) {
            if (peek().type == T::Eof) fail("unterminated block");
            n->kids.push_back(statement_list_item());
        }
        i++;
        return finish(n);
    }

    // var / let / const; kids = declarators (a = target, b = init)
    Node* var_decl(int kind, bool need_semi, bool in_for = false) {
        Node* n = make(N::VarDecl, peek());
        n->flags = kind;
        i++;  // var / let / const
        do {
            Node* d = make(N::Declarator, peek());
            d->a = binding_target();
            if (kind && d->a->type == N::Ident && d->a->str == u"let") fail("let as lexical name");
            if (eat_p("=")) {
                d->b = assignment(in_for);
            } else if (!in_for) {
                if (kind & F_CONST) fail("missing const initializer");
                if (d->a->type != N::Ident) fail("missing destructuring initializer");
            }
            n->kids.push_back(finish(d));
        } while (eat_p(","));
        if (need_semi) semicolon();
        return finish(n);
    }

    Node* binding_target() {
        const Token& t = peek();
        if (is_p("[")) return array_binding();
        if (is_p("{")) return object_binding();
        Node* n = make(N::Ident, t);
        n->str = ident(true);
        return finish(n);
    }
    Node* binding_element() {
        Node* target = binding_target();
        if (is_p("=")) {
            Node* a = make(N::AssignPattern, peek());
            i++;
            a->a = target;
            a->b = assignment(false);
            return finish(a);
        }
        return target;
    }
    Node* array_binding() {
        Node* n = make(N::ArrayPattern, peek());
        expect_p("[");
        while (!is_p("]")) {
            if (is_p(",")) {
                i++;
                n->kids.push_back(make(N::Hole, peek()));
                continue;
            }
            if (is_p("...")) {
                Node* r = make(N::Rest, peek());
                i++;
                r->a = binding_target();
                n->kids.push_back(finish(r));
                if (!is_p("]")) fail("rest element must be last");
                break;
            }
            n->kids.push_back(binding_element());
            if (!is_p("]")) expect_p(",");
        }
        expect_p("]");
        return finish(n);
    }
    Node* object_binding() {
        Node* n = make(N::ObjectPattern, peek());
        expect_p("{");
        while (!is_p("}")) {
            if (is_p("...")) {
                Node* r = make(N::Rest, peek());
                i++;
                Node* id = make(N::Ident, peek());
                id->str = ident(true);
                r->a = finish(id);
                n->kids.push_back(finish(r));
                if (!is_p("}")) fail("rest element must be last");
                break;
            }
            Node* p = make(N::Property, peek());
            const Token& kt = peek();
            p->a = property_key(p);
            if (eat_p(":")) {
                p->b = binding_element();
            } else {
                if (p->flags & F_COMPUTED || (kt.type != T::Ident && kt.type != T::Keyword)) fail("bad shorthand binding");
                i--;
                Node* id = make(N::Ident, peek());
                id->str = ident(true);
                finish(id);
                p->flags |= F_SHORTHAND;
                if (is_p("=")) {
                    Node* a = make(N::AssignPattern, peek());
                    i++;
                    a->a = id;
                    a->b = assignment(false);
                    p->b = finish(a);
                } else {
                    p->b = id;
                }
            }
            n->kids.push_back(finish(p));
            if (!is_p("}")) expect_p(",");
        }
        expect_p("}");
        return finish(n);
    }

    // a statement in a position where annex B allows a function declaration (if bodies)
    Node* sub_statement(bool if_body) {
        if (if_body && !strict && is_kw("function")) {
            Node* f = function_decl(false, peek());
            if (f->flags & (F_ASYNC | F_GENERATOR)) fail("generator in if body");
            Node* b = make(N::Block, toks[i - 1]);
            b->kids.push_back(f);
            return finish(b);
        }
        return statement();
    }

    Node* if_stmt() {
        Node* n = make(N::If, next());
        expect_p("(");
        n->a = expression(false);
        expect_p(")");
        n->b = sub_statement(true);
        if (eat_kw("else")) n->c = sub_statement(true);
        return finish(n);
    }

    Node* loop_body() {
        fn().loop++;
        Node* b = statement();
        fn().loop--;
        return b;
    }

    Node* while_stmt() {
        Node* n = make(N::While, next());
        expect_p("(");
        n->a = expression(false);
        expect_p(")");
        n->b = loop_body();
        return finish(n);
    }
    Node* do_stmt() {
        Node* n = make(N::DoWhile, next());
        n->b = loop_body();
        expect_kw("while");
        expect_p("(");
        n->a = expression(false);
        expect_p(")");
        eat_p(";");  // ASI always applies after do-while
        return finish(n);
    }

    Node* for_stmt() {
        const Token& ft = next();
        bool is_await = false;
        if (is_kw("await")) {
            if (!in_async() && !(fns.size() == 1 && false)) fail("for await outside async function");
            i++;
            is_await = true;
        }
        expect_p("(");
        Node* init = nullptr;
        size_t init_tok = i;
        bool lexical = false;
        if (is_p(";")) {
            // no init
        } else if (is_kw("var")) {
            init = var_decl(0, false, true);
        } else if (is_kw("const") || (is_word("let") && !peek().escaped && (is_let_decl() || (is_p("[", 1))))) {
            init = var_decl(is_kw("const") ? F_CONST : F_LET, false, true);
            lexical = true;
        } else {
            bool starts_let = is_word("let");
            bool starts_async_of = is_word("async") && !peek().escaped && is_word("of", 1) && !is_p("=>", 2);
            init = expression(true);
            if (is_word("of") && starts_let && !is_await) fail("for (let of");
            if (is_word("of") && starts_async_of && !is_await) fail("for (async of");
        }
        (void)init_tok;
        if (init && (is_word("of") || is_kw("in"))) {
            bool of = is_word("of");
            if (of && peek().escaped) fail("escaped of");
            if (is_await && !of) fail("for await in");
            i++;
            Node* n = make(of ? N::ForOf : N::ForIn, ft);
            if (is_await) n->flags |= F_AWAIT;
            if (init->type == N::VarDecl) {
                if (init->kids.size() != 1) fail("multiple bindings in for-in/of");
                Node* d = init->kids[0];
                if (d->b) {
                    // annex B: for (var x = 1 in o) in sloppy mode with a simple binding
                    if (of || strict || lexical || d->a->type != N::Ident) fail("initializer in for-in/of");
                }
                if (lexical && d->a->type == N::Ident && d->a->str == u"let") fail("let binding in for");
            } else {
                init = to_pattern(init, false);
            }
            n->a = init;
            n->b = of ? assignment(false) : expression(false);
            expect_p(")");
            n->c = loop_body();
            return finish(n);
        }
        if (is_await) fail("for await needs of");
        if (init && init->type == N::VarDecl) {
            for (Node* d : init->kids) {
                if (!d->b && ((init->flags & F_CONST) || d->a->type != N::Ident)) fail("missing initializer");
            }
        }
        Node* n = make(N::For, ft);
        n->kids.push_back(init);
        expect_p(";");
        n->kids.push_back(is_p(";") ? nullptr : expression(false));
        expect_p(";");
        n->kids.push_back(is_p(")") ? nullptr : expression(false));
        expect_p(")");
        n->a = loop_body();
        return finish(n);
    }

    Node* return_stmt() {
        const Token& t = next();
        if (!fn().is_function) fail("return outside function");
        Node* n = make(N::Return, t);
        if (!is_p(";") && !is_p("}") && peek().type != T::Eof && !peek().nl_before) n->a = expression(false);
        semicolon();
        return finish(n);
    }

    Node* jump_stmt(bool brk) {
        const Token& t = next();
        Node* n = make(brk ? N::Break : N::Continue, t);
        if (!peek().nl_before && at_ident()) {
            n->str = ident();
            bool found = false;
            for (auto& l : fn().labels)
                if (l.first == n->str) {
                    found = true;
                    if (!brk && !l.second) fail("continue to a non-loop label");
                }
            if (!found) fail("undefined label");
        } else {
            if (brk && !fn().loop && !fn().sw) fail("break outside loop or switch");
            if (!brk && !fn().loop) fail("continue outside loop");
        }
        semicolon();
        return finish(n);
    }

    Node* throw_stmt() {
        Node* n = make(N::Throw, next());
        if (peek().nl_before) fail("newline after throw");
        n->a = expression(false);
        semicolon();
        return finish(n);
    }

    Node* try_stmt() {
        Node* n = make(N::Try, next());
        n->a = block();
        if (eat_kw("catch")) {
            Node* h = make(N::Block, toks[i - 1]);
            Node* param = nullptr;
            if (eat_p("(")) {
                param = binding_target();
                expect_p(")");
            }
            Node* body = block();
            h->a = param;
            h->b = body;
            n->b = finish(h);
        }
        if (eat_kw("finally")) n->c = block();
        if (!n->b && !n->c) fail("try without catch or finally");
        return finish(n);
    }

    Node* switch_stmt() {
        Node* n = make(N::Switch, next());
        expect_p("(");
        n->a = expression(false);
        expect_p(")");
        expect_p("{");
        bool def = false;
        fn().sw++;
        while (!eat_p("}")) {
            Node* c = make(N::Case, peek());
            if (eat_kw("default")) {
                if (def) fail("duplicate default");
                def = true;
            } else {
                expect_kw("case");
                c->a = expression(false);
            }
            expect_p(":");
            while (!is_p("}") && !is_kw("case") && !is_kw("default")) {
                if (peek().type == T::Eof) fail("unterminated switch");
                c->kids.push_back(statement_list_item());
            }
            n->kids.push_back(finish(c));
        }
        fn().sw--;
        return finish(n);
    }

    Node* with_stmt() {
        if (strict) fail("with in strict code");
        Node* n = make(N::With, next());
        expect_p("(");
        n->a = expression(false);
        expect_p(")");
        n->b = statement();
        return finish(n);
    }

    // ---------------------------------------------------------- functions
    Node* function_decl(bool async, const Token& t) {
        if (async) i++;
        i++;  // function
        bool gen = eat_p("*");
        Node* name = nullptr;
        if (at_ident() || (peek().type == T::Keyword && (peek().value == u"yield" || peek().value == u"await"))) {
            // the name of a declaration is bound in the enclosing scope
            const Token& nt = peek();
            if (!ident_ok(nt)) fail("bad function name");
            name = make(N::Ident, nt);
            name->str = nt.value;
            i++;
            finish(name);
        } else {
            fail("function declaration needs a name");
        }
        Node* f = function_rest(t, async, gen, false, name);
        f->flags |= F_DECL;
        return f;
    }

    // parses (params) { body } ; name may be null
    Node* function_rest(const Token& start, bool async, bool gen, bool method, Node* name, int extra_flags = 0, bool is_expr = false) {
        Node* f = make(N::Function, start);
        f->a = name;
        if (name) f->str = name->str;
        f->flags |= extra_flags;
        if (async) f->flags |= F_ASYNC;
        if (gen) f->flags |= F_GENERATOR;
        if (method) f->flags |= F_METHOD;
        FnCtx c;
        c.is_async = async;
        c.is_gen = gen;
        c.is_function = true;
        c.allow_new_target = true;
        c.allow_super_prop = method || (extra_flags & (F_CLASS_CTOR | F_GETTER | F_SETTER));
        c.allow_super_call = (extra_flags & F_DERIVED) != 0;
        c.node = f;
        // a function expression's own name: yield/await rules follow the function itself
        if (is_expr && name) {
            if ((gen && name->str == u"yield") || (async && name->str == u"await")) fail("bad function expression name");
        }
        bool outer_strict = strict;
        fns.push_back(c);
        params_and_body(f);
        fns.pop_back();
        strict = outer_strict;
        if ((f->flags & F_STRICT) && name && (name->str == u"eval" || name->str == u"arguments")) fail("eval/arguments as function name");
        if ((f->flags & F_STRICT) && name && !outer_strict && in_list(name->str, STRICT_RESERVED)) fail("reserved word as function name");
        return finish(f);
    }

    void params_and_body(Node* f) {
        expect_p("(");
        fn().in_params = true;
        bool simple = true;
        while (!is_p(")")) {
            if (is_p("...")) {
                Node* r = make(N::Rest, peek());
                i++;
                r->a = binding_target();
                f->kids.push_back(finish(r));
                simple = false;
                if (!is_p(")")) fail("rest parameter must be last");
                break;
            }
            Node* p = binding_element();
            if (p->type != N::Ident) simple = false;
            f->kids.push_back(p);
            if (!is_p(")")) expect_p(",");
        }
        expect_p(")");
        fn().in_params = false;
        if (simple) f->flags |= F_SIMPLE_PARAMS;
        if (f->flags & F_GETTER && !f->kids.empty()) fail("getter with parameters");
        if (f->flags & F_SETTER && (f->kids.size() != 1 || f->kids[0]->type == N::Rest)) fail("setter needs one parameter");
        f->b = function_body(f);
        check_params(f);
    }

    Node* function_body(Node* f) {
        Node* body = make(N::Block, peek());
        expect_p("{");
        bool outer_strict = strict;
        if (directives(body->kids)) {
            if (!(f->flags & F_SIMPLE_PARAMS)) fail("use strict with non-simple parameters");
            strict = true;
        }
        if (strict) f->flags |= F_STRICT;
        auto saved_labels = std::move(fn().labels);
        fn().labels.clear();
        while (!is_p("}")) {
            if (peek().type == T::Eof) fail("unterminated function body");
            body->kids.push_back(statement_list_item());
        }
        i++;
        fn().labels = std::move(saved_labels);
        (void)outer_strict;
        return finish(body);
    }

    void collect_bound(Node* p, std::vector<u16>& out) {
        if (!p) return;
        switch (p->type) {
            case N::Ident: out.push_back(p->str); break;
            case N::AssignPattern: collect_bound(p->a, out); break;
            case N::Rest: collect_bound(p->a, out); break;
            case N::ArrayPattern: for (Node* k : p->kids) collect_bound(k, out); break;
            case N::ObjectPattern:
                for (Node* k : p->kids) collect_bound(k->type == N::Rest ? k->a : k->b, out);
                break;
            default: break;
        }
    }

    void check_params(Node* f) {
        std::vector<u16> names;
        for (Node* p : f->kids) collect_bound(p, names);
        bool strictf = f->flags & F_STRICT;
        bool unique_required = strictf || (f->flags & (F_ARROW | F_METHOD | F_GETTER | F_SETTER | F_CLASS_CTOR)) ||
                               !(f->flags & F_SIMPLE_PARAMS);
        std::set<u16> seen;
        for (auto& n : names) {
            if (strictf && (n == u"eval" || n == u"arguments")) fail("eval/arguments as parameter");
            if (strictf && in_list(n, STRICT_RESERVED)) fail("reserved word as parameter");
            if (seen.count(n) && unique_required) fail("duplicate parameter");
            seen.insert(n);
        }
        // parameters may not be redeclared by lexical declarations in the body
        if (f->b && f->b->type == N::Block) {
            for (Node* s : f->b->kids) {
                if (s->type == N::VarDecl && (s->flags & (F_LET | F_CONST))) {
                    std::vector<u16> lex;
                    for (Node* d : s->kids) collect_bound(d->a, lex);
                    for (auto& l : lex)
                        if (seen.count(l)) fail("parameter redeclared");
                } else if (s->type == N::Class && s->a && seen.count(s->a->str)) {
                    fail("parameter redeclared by class");
                }
            }
        }
    }

    // ---------------------------------------------------------- classes
    Node* class_decl() {
        const Token& t = next();
        Node* name = nullptr;
        bool outer = strict;
        strict = true;
        if (at_ident()) {
            name = make(N::Ident, peek());
            name->str = ident(true);
            finish(name);
        } else {
            fail("class declaration needs a name");
        }
        Node* c = class_rest(t, name);
        c->flags |= F_DECL;
        strict = outer;
        return c;
    }

    Node* class_rest(const Token& t, Node* name) {
        bool outer = strict;
        strict = true;
        Node* c = make(N::Class, t);
        c->a = name;
        if (name) c->str = name->str;
        if (eat_kw("extends")) {
            c->b = lhs_expression();
            c->flags |= F_DERIVED;
        }
        expect_p("{");
        classes.push_back({});
        bool has_ctor = false;
        while (!eat_p("}")) {
            if (eat_p(";")) continue;
            Node* m = class_member(c, has_ctor);
            if (m) c->kids.push_back(m);
        }
        // every #name used inside must be declared here or in an enclosing class
        ClassCtx cc = std::move(classes.back());
        classes.pop_back();
        for (auto& u : cc.used) {
            if (cc.declared.count(u.first)) continue;
            if (!classes.empty()) {
                classes.back().used.push_back(u);
            } else if (std::find(outer_private.begin(), outer_private.end(), u.first) == outer_private.end()) {
                throw SyntaxError{"undeclared private name", u.second};
            }
        }
        strict = outer;
        return finish(c);
    }

    Node* class_member(Node* cls, bool& has_ctor) {
        const Token& t = peek();
        Node* m = make(N::ClassMember, t);
        bool is_static = false;
        if (is_word("static") && !peek().escaped && !is_p("(", 1) && !is_p("=", 1) && !is_p(";", 1) && !is_p("}", 1)) {
            i++;
            is_static = true;
            m->flags |= F_STATIC;
            if (is_p("{")) {
                // static initialization block
                m->type = N::StaticBlock;
                Node* f = make(N::Function, peek());
                f->flags |= F_METHOD | F_STRICT;
                FnCtx c;
                c.is_function = true;
                c.allow_new_target = true;
                c.allow_super_prop = true;
                c.in_class_field = true;
                c.in_static_block = true;
                fns.push_back(c);
                Node* body = make(N::Block, peek());
                expect_p("{");
                while (!is_p("}")) body->kids.push_back(statement_list_item());
                i++;
                fns.pop_back();
                f->b = finish(body);
                f->flags |= F_SIMPLE_PARAMS;
                m->b = finish(f);
                return finish(m);
            }
        }
        bool async = false, gen = false;
        int kind = 0;  // 0 method, 1 get, 2 set
        if (is_word("async") && !peek().escaped && !peek(1).nl_before && !is_p("(", 1) && !is_p("=", 1) && !is_p(";", 1) &&
            !is_p("}", 1)) {
            i++;
            async = true;
        }
        if (eat_p("*")) gen = true;
        if (!async && !gen && (is_word("get") || is_word("set")) && !peek().escaped && !is_p("(", 1) && !is_p("=", 1) &&
            !is_p(";", 1) && !is_p("}", 1) && !peek(1).nl_before) {
            kind = is_word("get") ? 1 : 2;
            i++;
        } else if (!async && !gen && (is_word("get") || is_word("set")) && !peek().escaped && peek(1).nl_before &&
                   !is_p("(", 1) && !is_p("=", 1) && !is_p(";", 1) && !is_p("}", 1)) {
            kind = is_word("get") ? 1 : 2;
            i++;
        }
        const Token& kt = peek();
        u16 private_name;
        bool is_private = false;
        if (kt.type == T::Private) {
            private_name = kt.value;
            is_private = true;
            if (private_name == u"constructor") fail("#constructor");
            i++;
            Node* k = make(N::PrivateName, kt);
            k->str = private_name;
            m->a = finish(k);
            auto& cc = classes.back();
            int bit = kind == 1 ? 1 : kind == 2 ? 2 : 3;
            int prev = cc.kinds.count(private_name) ? cc.kinds[private_name] : 0;
            int pstatic = cc.kinds.count(u"\x01" + private_name) ? cc.kinds[u"\x01" + private_name] : -1;
            if (prev) {
                // only a getter/setter pair (same staticness) may share a name
                if (!((prev == 1 && bit == 2) || (prev == 2 && bit == 1)) || pstatic != int(is_static)) fail("duplicate private name");
                cc.kinds[private_name] = 3;
            } else {
                cc.kinds[private_name] = bit;
                cc.kinds[u"\x01" + private_name] = is_static;
            }
            cc.declared.insert(private_name);
        } else {
            m->a = property_key(m);
        }
        bool is_ctor_name = !(m->flags & F_COMPUTED) && !is_private && m->a->type == N::Str && m->a->str == u"constructor";
        bool is_proto_name = !(m->flags & F_COMPUTED) && !is_private && m->a->type == N::Str && m->a->str == u"prototype";
        if (is_p("(")) {
            if (is_ctor_name && !is_static) {
                if (kind || async || gen) fail("special constructor");
                if (has_ctor) fail("duplicate constructor");
                has_ctor = true;
                int fl = F_CLASS_CTOR | ((cls->flags & F_DERIVED) ? F_DERIVED : 0);
                m->b = function_rest(t, false, false, true, nullptr, fl);
                m->flags |= F_CLASS_CTOR;
                return finish(m);
            }
            if (is_static && is_proto_name) fail("static prototype method");
            int fl = kind == 1 ? F_GETTER : kind == 2 ? F_SETTER : 0;
            m->b = function_rest(t, async, gen, true, nullptr, fl);
            if (kind == 1) m->flags |= F_GETTER;
            if (kind == 2) m->flags |= F_SETTER;
            return finish(m);
        }
        // a field
        if (kind || async || gen) fail("bad class field");
        if (is_ctor_name || (is_static && is_proto_name)) fail("bad field name");
        if (!is_private && !(m->flags & F_COMPUTED) && m->a->type == N::Str && m->a->str == u"constructor") fail("field named constructor");
        m->flags |= F_FIELD;
        if (eat_p("=")) {
            Node* f = make(N::Function, peek());
            f->flags |= F_METHOD | F_STRICT | F_SIMPLE_PARAMS | F_EXPR_BODY;
            FnCtx c;
            c.is_function = true;
            c.allow_new_target = true;
            c.allow_super_prop = true;
            c.in_class_field = true;
            fns.push_back(c);
            f->b = assignment(false);
            fns.pop_back();
            m->b = finish(f);
        }
        semicolon_field();
        return finish(m);
    }
    void semicolon_field() {
        if (eat_p(";")) return;
        if (is_p("}") || peek().nl_before) return;
        fail("missing semicolon after field");
    }

    // a property name; sets F_COMPUTED; non-computed keys become Str nodes
    Node* property_key(Node* owner) {
        const Token& t = peek();
        if (eat_p("[")) {
            owner->flags |= F_COMPUTED;
            Node* e = assignment(false);
            expect_p("]");
            return e;
        }
        Node* k = make(N::Str, t);
        if (t.type == T::Ident || t.type == T::Keyword) {
            k->str = t.value;
        } else if (t.type == T::String) {
            if (strict && t.legacy_octal) fail("octal escape in strict code");
            k->str = t.value;
        } else if (t.type == T::Num) {
            if (strict && t.legacy_octal) fail("octal literal in strict code");
            k->str = number_to_u16(t.num);
        } else if (t.type == T::BigInt) {
            k->str = bigint_key(t.value);
        } else {
            fail("bad property name");
        }
        i++;
        return finish(k);
    }
    static u16 bigint_key(const u16& v) {
        // "base:digits" to decimal (small values only)
        size_t c = v.find(':');
        int base = std::stoi(to_utf8(v.substr(0, c)));
        unsigned long long x = 0;
        for (size_t k = c + 1; k < v.size(); k++) {
            int d = v[k] <= '9' ? v[k] - '0' : (v[k] | 32) - 'a' + 10;
            x = x * base + d;
        }
        return to_u16(std::to_string(x));
    }

    // ---------------------------------------------------------- expressions
    Node* expression(bool no_in) {
        Node* e = assignment(no_in);
        if (is_p(",")) {
            Node* s = make(N::Seq, toks[e->start == 0 ? i : i]);
            s->start = e->start;
            s->line = e->line;
            s->kids.push_back(e);
            while (eat_p(",")) s->kids.push_back(assignment(no_in));
            return finish(s);
        }
        return e;
    }

    bool is_assign_op() const {
        if (peek().type != T::Punct) return false;
        static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=",
                                    "&&=", "||=", "??=", nullptr};
        for (int k = 0; ops[k]; k++)
            if (peek().value == to_u16(ops[k])) return true;
        return false;
    }

    // finds whether the parenthesized group starting at token index j ends with ") =>"
    bool arrow_ahead(size_t j) const {
        if (!(toks[j].type == T::Punct && toks[j].value == u"(")) return false;
        int depth = 0;
        for (size_t k = j; k < toks.size(); k++) {
            const Token& t = toks[k];
            if (t.type == T::Eof) return false;
            if (t.type == T::Punct) {
                if (t.value == u"(" || t.value == u"[" || t.value == u"{") depth++;
                else if (t.value == u")" || t.value == u"]" || t.value == u"}") {
                    depth--;
                    if (depth == 0) {
                        return k + 1 < toks.size() && toks[k + 1].type == T::Punct && toks[k + 1].value == u"=>" &&
                               !toks[k + 1].nl_before;
                    }
                }
            } else if (t.type == T::Template && !t.tmpl_tail) {
                depth++;  // ${ ... } counts as a bracket
            } else if (t.type == T::Template && !t.tmpl_head && t.tmpl_tail) {
                depth--;
            }
        }
        return false;
    }

    Node* assignment(bool no_in) {
        const Token& t = peek();
        // arrow functions
        if (at_ident_or_yield_await() && is_p("=>", 1) && !peek(1).nl_before) {
            if (is_word("async") && !peek().escaped && false) {}
            return arrow(false, no_in);
        }
        if (is_word("async") && !peek().escaped && !peek(1).nl_before) {
            if (peek(1).type != T::Punct && is_p("=>", 2) && !peek(2).nl_before && peek(1).type != T::Eof) {
                if (peek(1).nl_before) fail("newline after async");
                return arrow(true, no_in);
            }
            if (is_p("(", 1) && arrow_ahead(i + 1)) return arrow(true, no_in);
        }
        if (is_p("(") && arrow_ahead(i)) return arrow(false, no_in);
        if (is_kw("yield") && in_gen()) {
            if (fn().in_params) fail("yield in parameters");
            return yield_expr(no_in);
        }
        Node* lhs = conditional(no_in);
        if (is_assign_op()) {
            const Token& op = next();
            Node* n = make(N::Assign, t);
            n->start = lhs->start;
            n->str = op.value;
            if (op.value == u"=") {
                n->a = to_pattern(lhs, false);
            } else {
                if (!simple_target(lhs)) fail("invalid assignment target");
                n->a = lhs;
            }
            n->b = assignment(no_in);
            // anonymous function names
            return finish(n);
        }
        return lhs;
    }
    bool at_ident_or_yield_await() const {
        const Token& t = peek();
        if (t.type == T::Ident) return true;
        if (t.type == T::Keyword && (t.value == u"yield" || t.value == u"await")) return true;
        return ident_ok(t);
    }

    Node* yield_expr(bool no_in) {
        Node* n = make(N::Yield, next());
        if (eat_p("*")) {
            n->flags |= F_DELEGATE;
            n->a = assignment(no_in);
        } else if (!peek().nl_before && !is_p(")") && !is_p("]") && !is_p("}") && !is_p(",") && !is_p(";") && !is_p(":") &&
                   peek().type != T::Eof && !(peek().type == T::Template && !peek().tmpl_head) && !is_kw("in") && !is_word("of")) {
            n->a = assignment(no_in);
        }
        return finish(n);
    }

    Node* arrow(bool async, bool no_in) {
        const Token& t = peek();
        if (async) i++;
        Node* f = make(N::Function, t);
        f->flags |= F_ARROW;
        if (async) f->flags |= F_ASYNC;
        FnCtx c;
        c.is_async = async;
        c.is_arrow = true;
        c.is_function = true;
        // arrows inherit new.target / super from the enclosing function
        c.allow_new_target = fn().allow_new_target || fn().is_function;
        c.allow_super_prop = fn().allow_super_prop;
        c.allow_super_call = fn().allow_super_call;
        c.in_static_block = false;
        bool outer_gen = in_gen();
        bool outer_static = fn().in_static_block;
        c.node = f;
        fns.push_back(c);
        if (is_p("(")) {
            fn().in_params = true;
            // parameters may not contain yield expressions or await
            expect_p("(");
            bool simple = true;
            while (!is_p(")")) {
                if (is_p("...")) {
                    Node* r = make(N::Rest, peek());
                    i++;
                    r->a = binding_target();
                    f->kids.push_back(finish(r));
                    simple = false;
                    if (!is_p(")")) fail("rest parameter must be last");
                    break;
                }
                if (outer_gen && is_kw("yield")) fail("yield in arrow parameters");
                if (outer_static && is_word("await")) fail("await in arrow parameters");
                Node* p = binding_element();
                if (p->type != N::Ident) simple = false;
                f->kids.push_back(p);
                if (!is_p(")")) expect_p(",");
            }
            expect_p(")");
            fn().in_params = false;
            if (simple) f->flags |= F_SIMPLE_PARAMS;
        } else {
            Node* id = make(N::Ident, peek());
            const Token& it = peek();
            if (async && it.value == u"await") fail("await as async arrow parameter");
            if (outer_static && it.value == u"await") fail("await in static block arrow");
            if (it.type == T::Keyword && it.value == u"yield" && (strict || outer_gen)) fail("yield as parameter");
            id->str = ident(true);
            f->kids.push_back(finish(id));
            f->flags |= F_SIMPLE_PARAMS;
        }
        if (peek().nl_before) fail("newline before =>");
        expect_p("=>");
        if (is_p("{")) {
            f->b = function_body(f);
        } else {
            f->flags |= F_EXPR_BODY;
            if (strict) f->flags |= F_STRICT;
            f->b = assignment(no_in);
        }
        fns.pop_back();
        check_params(f);
        return finish(f);
    }

    // turns an expression into an assignment pattern (destructuring assignment)
    Node* to_pattern(Node* e, bool binding) {
        switch (e->type) {
            case N::Ident:
                if (strict && (e->str == u"eval" || e->str == u"arguments")) fail("assignment to eval/arguments");
                return e;
            case N::Member:
                if (binding) fail("member expression in binding pattern");
                if (e->flags & F_OPTIONAL) fail("optional chain as assignment target");
                return e;
            case N::Optional: fail("optional chain as assignment target");
            case N::Array: {
                if (e->flags & F_PARENS) fail("parenthesized pattern");
                Node* p = ast.make(N::ArrayPattern);
                p->start = e->start;
                p->end = e->end;
                p->line = e->line;
                for (size_t k = 0; k < e->kids.size(); k++) {
                    Node* el = e->kids[k];
                    if (el->type == N::Spread) {
                        if (k + 1 != e->kids.size() || (e->flags & F_COMPUTED /* trailing comma after spread */))
                            fail("rest element must be last");
                        Node* r = ast.make(N::Rest);
                        r->a = to_pattern(el->a, binding);
                        if (r->a->type == N::AssignPattern) fail("rest with initializer");
                        p->kids.push_back(r);
                    } else if (el->type == N::Hole) {
                        p->kids.push_back(el);
                    } else {
                        p->kids.push_back(to_pattern(el, binding));
                    }
                }
                return p;
            }
            case N::Object: {
                if (e->flags & F_PARENS) fail("parenthesized pattern");
                Node* p = ast.make(N::ObjectPattern);
                p->start = e->start;
                p->end = e->end;
                p->line = e->line;
                for (size_t k = 0; k < e->kids.size(); k++) {
                    Node* pr = e->kids[k];
                    if (pr->type == N::Spread) {
                        if (k + 1 != e->kids.size()) fail("rest element must be last");
                        Node* r = ast.make(N::Rest);
                        r->a = to_pattern(pr->a, binding);
                        if (r->a->type != N::Ident && r->a->type != N::Member) fail("bad object rest target");
                        p->kids.push_back(r);
                        continue;
                    }
                    if (pr->flags & (F_METHOD | F_GETTER | F_SETTER)) fail("method in pattern");
                    Node* np = ast.make(N::Property);
                    *np = *pr;
                    np->b = to_pattern(pr->b, binding);
                    np->flags &= ~F_COVER_INIT;
                    p->kids.push_back(np);
                }
                return p;
            }
            case N::Assign:
                if (e->str != u"=") fail("invalid destructuring default");
                if (e->flags & F_PARENS) fail("parenthesized default");
                {
                    Node* a = ast.make(N::AssignPattern);
                    a->start = e->start;
                    a->a = to_pattern(e->a, binding);
                    a->b = e->b;
                    return a;
                }
            case N::AssignPattern: return e;
            case N::Call:
                // annex B allows f() = 1 in sloppy code (a runtime ReferenceError)
                if (!strict && !binding && !(e->flags & F_PARENS && false)) {
                    e->flags |= F_COVER_INIT * 0;
                    Node* w = ast.make(N::Call);
                    *w = *e;
                    w->flags |= F_CHAIN_END;  // marks "call as assignment target"
                    return w;
                }
                fail("invalid assignment target");
            default:
                fail("invalid assignment target");
        }
    }
    bool simple_target(Node* e) {
        if (e->type == N::Ident) {
            if (strict && (e->str == u"eval" || e->str == u"arguments")) fail("assignment to eval/arguments");
            return true;
        }
        if (e->type == N::Member) return !(e->flags & F_OPTIONAL);
        if (e->type == N::Call && !strict) {
            e->flags |= F_CHAIN_END;
            return true;
        }
        return false;
    }

    Node* conditional(bool no_in) {
        Node* c = binary(0, no_in);
        if (is_p("?")) {
            Node* n = make(N::Cond, next());
            n->start = c->start;
            n->a = c;
            n->b = assignment(false);
            expect_p(":");
            n->c = assignment(no_in);
            return finish(n);
        }
        return c;
    }

    int prec(const Token& t, bool no_in) const {
        if (t.type == T::Keyword && !t.escaped) {
            if (t.value == u"instanceof") return 7;
            if (t.value == u"in" && !no_in) return 7;
            return -1;
        }
        if (t.type != T::Punct) return -1;
        const u16& v = t.value;
        if (v == u"??") return 1;
        if (v == u"||") return 1;
        if (v == u"&&") return 2;
        if (v == u"|") return 3;
        if (v == u"^") return 4;
        if (v == u"&") return 5;
        if (v == u"==" || v == u"!=" || v == u"===" || v == u"!==") return 6;
        if (v == u"<" || v == u">" || v == u"<=" || v == u">=") return 7;
        if (v == u"<<" || v == u">>" || v == u">>>") return 8;
        if (v == u"+" || v == u"-") return 9;
        if (v == u"*" || v == u"/" || v == u"%") return 10;
        if (v == u"**") return 11;
        return -1;
    }

    Node* binary(int min, bool no_in) {
        Node* left;
        // #x in obj
        if (peek().type == T::Private && is_kw("in", 1)) {
            const Token& pt = next();
            Node* pn = make(N::PrivateName, pt);
            pn->str = pt.value;
            use_private(pt);
            if (no_in || 7 < min) fail("bad private in");
            i++;  // in
            Node* n = make(N::PrivateIn, pt);
            n->a = finish(pn);
            n->b = binary(8, no_in);
            left = finish(n);
        } else {
            left = unary();
        }
        bool saw_nullish = false, saw_logical = false;
        while (true) {
            int p = prec(peek(), no_in);
            if (p < 0 || p < min) break;
            const Token& op = next();
            if (op.value == u"**") {
                if (left->type == N::Unary || (left->type == N::Await && !(left->flags & F_PARENS))) fail("unary before **");
                Node* n = make(N::Binary, op);
                n->start = left->start;
                n->str = op.value;
                n->a = left;
                n->b = binary(11, no_in);  // right associative
                left = finish(n);
                continue;
            }
            if (op.value == u"??") saw_nullish = true;
            if (op.value == u"||" || op.value == u"&&") saw_logical = true;
            if (saw_nullish && saw_logical) fail("?? mixed with || or &&");
            Node* right = binary(p + 1, no_in);
            if (op.value == u"??" && (right->type == N::Logical && !(right->flags & F_PARENS) && right->str != u"??")) fail("?? mixed");
            if ((op.value == u"||" || op.value == u"&&") && right->type == N::Logical && right->str == u"??" && !(right->flags & F_PARENS)) fail("?? mixed");
            if ((op.value == u"||" || op.value == u"&&") && left->type == N::Logical && left->str == u"??" && !(left->flags & F_PARENS)) fail("?? mixed");
            bool logical = op.value == u"||" || op.value == u"&&" || op.value == u"??";
            Node* n = make(logical ? N::Logical : N::Binary, op);
            n->start = left->start;
            n->str = op.value;
            n->a = left;
            n->b = right;
            left = finish(n);
        }
        return left;
    }

    Node* unary() {
        const Token& t = peek();
        if (t.type == T::Punct && (t.value == u"!" || t.value == u"~" || t.value == u"+" || t.value == u"-")) {
            i++;
            Node* n = make(N::Unary, t);
            n->str = t.value;
            n->a = unary();
            return finish(n);
        }
        if (t.type == T::Punct && (t.value == u"++" || t.value == u"--")) {
            i++;
            Node* n = make(N::Update, t);
            n->str = t.value;
            n->flags |= F_PREFIX;
            n->a = unary();
            if (!simple_target(n->a) || n->a->type == N::Call) {
                if (n->a->type == N::Call && !strict) return finish(n);
                fail("invalid update target");
            }
            return finish(n);
        }
        if (t.type == T::Keyword && !t.escaped && (t.value == u"delete" || t.value == u"void" || t.value == u"typeof")) {
            i++;
            Node* n = make(N::Unary, t);
            n->str = t.value;
            n->a = unary();
            if (t.value == u"delete" && strict && n->a->type == N::Ident) fail("delete of an identifier in strict code");
            if (t.value == u"delete" && n->a->type == N::Member && n->a->b && n->a->b->type == N::PrivateName) fail("delete of a private field");
            if (t.value == u"delete" && n->a->type == N::Optional) {
                Node* last = n->a->a;
                if (last->type == N::Member && last->b && last->b->type == N::PrivateName) fail("delete of a private field");
            }
            return finish(n);
        }
        if (is_kw("await") && (await_reserved() || false)) {
            if (fn().in_params) fail("await in parameters");
            if (!in_async() && fn().in_static_block) fail("await in static block");
            Node* n = make(N::Await, next());
            n->a = unary();
            return finish(n);
        }
        Node* e = postfix();
        return e;
    }

    Node* postfix() {
        Node* e = lhs_expression();
        if (peek().type == T::Punct && (peek().value == u"++" || peek().value == u"--") && !peek().nl_before) {
            const Token& op = next();
            if (!simple_target(e)) fail("invalid update target");
            Node* n = make(N::Update, op);
            n->start = e->start;
            n->str = op.value;
            n->a = e;
            return finish(n);
        }
        return e;
    }

    std::vector<Node*> arguments() {
        std::vector<Node*> args;
        expect_p("(");
        while (!is_p(")")) {
            if (is_p("...")) {
                Node* s = make(N::Spread, next());
                s->a = assignment(false);
                args.push_back(finish(s));
            } else {
                args.push_back(assignment(false));
            }
            if (!is_p(")")) expect_p(",");
        }
        expect_p(")");
        return args;
    }

    void use_private(const Token& t) {
        if (classes.empty()) {
            if (std::find(outer_private.begin(), outer_private.end(), t.value) == outer_private.end())
                fail_at(t, "private name outside class");
            return;
        }
        classes.back().used.push_back({t.value, t.line});
    }

    Node* lhs_expression() {
        const Token& t = peek();
        Node* e;
        if (is_kw("new")) {
            e = new_expression();
        } else if (is_kw("super")) {
            e = super_expression();
        } else if (is_kw("import")) {
            i++;
            if (eat_p(".")) fail("import.meta outside a module");
            Node* n = make(N::ImportCall, t);
            expect_p("(");
            if (is_p(")")) fail("import() needs an argument");
            n->a = assignment(false);
            if (eat_p(",")) {
                if (!is_p(")")) {
                    n->b = assignment(false);
                    eat_p(",");
                }
            }
            expect_p(")");
            e = finish(n);
        } else {
            e = primary();
        }
        bool in_chain = false;
        Node* chain_root = nullptr;
        while (true) {
            const Token& op = peek();
            if (is_p(".")) {
                i++;
                Node* m = make(N::Member, op);
                m->start = e->start;
                m->a = e;
                if (peek().type == T::Private) {
                    const Token& pt = next();
                    Node* pn = make(N::PrivateName, pt);
                    pn->str = pt.value;
                    use_private(pt);
                    m->b = finish(pn);
                } else {
                    if (!at_ident_name()) fail("expected property name");
                    Node* k = make(N::Str, next());
                    k->str = toks[i - 1].value;
                    m->b = finish(k);
                }
                e = finish(m);
            } else if (is_p("?.")) {
                i++;
                in_chain = true;
                if (!chain_root) chain_root = e;
                if (is_p("(")) {
                    Node* c = make(N::Call, op);
                    c->start = e->start;
                    c->a = e;
                    c->kids = arguments();
                    c->flags |= F_OPTIONAL;
                    e = finish(c);
                } else if (is_p("[")) {
                    i++;
                    Node* m = make(N::Member, op);
                    m->start = e->start;
                    m->a = e;
                    m->b = expression(false);
                    m->flags |= F_COMPUTED | F_OPTIONAL;
                    expect_p("]");
                    e = finish(m);
                } else if (peek().type == T::Template) {
                    fail("tagged template in optional chain");
                } else {
                    Node* m = make(N::Member, op);
                    m->start = e->start;
                    m->a = e;
                    m->flags |= F_OPTIONAL;
                    if (peek().type == T::Private) {
                        const Token& pt = next();
                        Node* pn = make(N::PrivateName, pt);
                        pn->str = pt.value;
                        use_private(pt);
                        m->b = finish(pn);
                    } else {
                        if (!at_ident_name()) fail("expected property name");
                        Node* k = make(N::Str, next());
                        k->str = toks[i - 1].value;
                        m->b = finish(k);
                    }
                    e = finish(m);
                }
            } else if (is_p("[")) {
                i++;
                Node* m = make(N::Member, op);
                m->start = e->start;
                m->a = e;
                m->b = expression(false);
                m->flags |= F_COMPUTED;
                expect_p("]");
                e = finish(m);
            } else if (is_p("(")) {
                Node* c = make(N::Call, op);
                c->start = e->start;
                c->a = e;
                c->kids = arguments();
                e = finish(c);
            } else if (peek().type == T::Template && peek().tmpl_head) {
                if (in_chain) fail("tagged template in optional chain");
                Node* tt = make(N::TaggedTemplate, op);
                tt->start = e->start;
                tt->a = e;
                tt->b = template_literal(true);
                e = finish(tt);
            } else {
                break;
            }
        }
        if (in_chain) {
            Node* o = make(N::Optional, t);
            o->start = e->start;
            o->a = e;
            e = finish(o);
        }
        return e;
    }

    Node* new_expression() {
        const Token& t = next();  // new
        if (is_p(".")) {
            i++;
            if (!is_word("target") || peek().escaped) fail("expected new.target");
            i++;
            bool ok = false;
            for (auto it = fns.rbegin(); it != fns.rend(); ++it) {
                if (it->is_arrow) continue;
                ok = it->is_function || it->allow_new_target;
                break;
            }
            if (!ok) fail("new.target outside function");
            return finish(make(N::NewTarget, t));
        }
        Node* n = make(N::New, t);
        Node* callee;
        if (is_kw("new")) callee = new_expression();
        else if (is_kw("super")) callee = super_expression();
        else if (is_kw("import")) fail("new import()");
        else callee = primary();
        // member accesses bind to the callee
        while (true) {
            const Token& op = peek();
            if (is_p(".")) {
                i++;
                Node* m = make(N::Member, op);
                m->start = callee->start;
                m->a = callee;
                if (peek().type == T::Private) {
                    const Token& pt = next();
                    Node* pn = make(N::PrivateName, pt);
                    pn->str = pt.value;
                    use_private(pt);
                    m->b = finish(pn);
                } else {
                    if (!at_ident_name()) fail("expected property name");
                    Node* k = make(N::Str, next());
                    k->str = toks[i - 1].value;
                    m->b = finish(k);
                }
                callee = finish(m);
            } else if (is_p("[")) {
                i++;
                Node* m = make(N::Member, op);
                m->start = callee->start;
                m->a = callee;
                m->b = expression(false);
                m->flags |= F_COMPUTED;
                expect_p("]");
                callee = finish(m);
            } else if (peek().type == T::Template && peek().tmpl_head) {
                Node* tt = make(N::TaggedTemplate, op);
                tt->start = callee->start;
                tt->a = callee;
                tt->b = template_literal(true);
                callee = finish(tt);
            } else if (is_p("?.")) {
                fail("optional chain in new expression");
            } else {
                break;
            }
        }
        n->a = callee;
        if (is_p("(")) n->kids = arguments();
        return finish(n);
    }

    Node* super_expression() {
        const Token& t = next();
        Node* s = make(N::Super, t);
        finish(s);
        bool prop_ok = false, call_ok = false;
        for (auto it = fns.rbegin(); it != fns.rend(); ++it) {
            if (it->is_arrow) continue;
            prop_ok = it->allow_super_prop;
            call_ok = it->allow_super_call;
            break;
        }
        if (fns.size() == 1) {
            prop_ok = fns[0].allow_super_prop;
            call_ok = fns[0].allow_super_call;
        }
        if (is_p("(")) {
            if (!call_ok) fail("super() outside a derived constructor");
            Node* c = make(N::Call, t);
            c->a = s;
            c->kids = arguments();
            return finish(c);
        }
        if (!prop_ok) fail("super property outside a method");
        if (is_p(".")) {
            i++;
            if (peek().type == T::Private) fail("super.#private");
            Node* m = make(N::Member, t);
            m->a = s;
            if (!at_ident_name()) fail("expected property name");
            Node* k = make(N::Str, next());
            k->str = toks[i - 1].value;
            m->b = finish(k);
            return finish(m);
        }
        if (is_p("[")) {
            i++;
            Node* m = make(N::Member, t);
            m->a = s;
            m->b = expression(false);
            m->flags |= F_COMPUTED;
            expect_p("]");
            return finish(m);
        }
        fail("bad use of super");
    }

    Node* template_literal(bool tagged) {
        Node* n = make(N::Template, peek());
        while (true) {
            const Token& t = next();
            if (t.type != T::Template) fail("bad template");
            if (t.cooked_invalid && !tagged) fail("bad escape in template");
            Node* q = make(N::Str, t);
            q->str = t.value;
            if (t.cooked_invalid) q->flags |= F_COVER_INIT;  // cooked is undefined
            Node* raw = make(N::Str, t);
            raw->str = t.raw;
            q->a = raw;
            n->kids.push_back(finish(q));
            if (t.tmpl_tail) break;
            Node* e = expression(false);
            n->kids.push_back(e);
            if (peek().type != T::Template || peek().tmpl_head) fail("unterminated template substitution");
        }
        return finish(n);
    }

    Node* primary() {
        const Token& t = peek();
        switch (t.type) {
            case T::Num: {
                i++;
                if (strict && t.legacy_octal) fail("octal literal in strict code");
                Node* n = make(N::Num, t);
                n->num = t.num;
                return finish(n);
            }
            case T::BigInt: {
                i++;
                Node* n = make(N::BigIntLit, t);
                n->str = t.value;
                return finish(n);
            }
            case T::String: {
                i++;
                if (strict && t.legacy_octal) fail("octal escape in strict code");
                Node* n = make(N::Str, t);
                n->str = t.value;
                return finish(n);
            }
            case T::Template: return template_literal(false);
            case T::Regex: {
                i++;
                Node* n = make(N::Regex, t);
                n->str = t.value;
                Node* fl = make(N::Str, t);
                fl->str = t.raw;
                n->a = fl;
                // flags must be known and not repeated
                std::set<char16_t> seen;
                for (char16_t c : t.raw) {
                    if (!u16(u"dgimsuyv").find(c) && u16(u"dgimsuyv").find(c) == u16::npos) fail("bad regex flag");
                    if (u16(u"dgimsuyv").find(c) == u16::npos || seen.count(c)) fail("bad regex flag");
                    seen.insert(c);
                }
                if (seen.count('u') && seen.count('v')) fail("regex flags u and v together");

                std::string err;
                if (!regexp_syntax_ok(t.value, t.raw, &err)) fail("invalid regular expression: " + err);
                return finish(n);
            }
            case T::Private: fail("unexpected private name");
            case T::Punct:
                if (t.value == u"(") return paren();
                if (t.value == u"[") return array_literal();
                if (t.value == u"{") return object_literal();
                fail("unexpected token");
            case T::Keyword:
                if (!t.escaped) {
                    if (t.value == u"this") {
                        i++;
                        return finish(make(N::This, t));
                    }
                    if (t.value == u"null") {
                        i++;
                        return finish(make(N::Null, t));
                    }
                    if (t.value == u"true" || t.value == u"false") {
                        i++;
                        Node* n = make(N::Bool, t);
                        n->num = t.value == u"true";
                        return finish(n);
                    }
                    if (t.value == u"function") return function_expr(false);
                    if (t.value == u"async" && is_kw("function", 1) && !peek(1).nl_before) return function_expr(true);
                    if (t.value == u"class") {
                        i++;
                        Node* name = nullptr;
                        bool outer = strict;
                        strict = true;
                        if (at_ident() && !is_kw("extends")) {
                            name = make(N::Ident, peek());
                            name->str = ident(true);
                            finish(name);
                        }
                        Node* c = class_rest(t, name);
                        strict = outer;
                        return c;
                    }
                    if (t.value == u"new") return new_expression();
                }
                [[fallthrough]];
            case T::Ident: {
                if (t.type == T::Keyword && t.value == u"let" && strict) fail("let in strict code");
                Node* n = make(N::Ident, t);
                n->str = ident();
                if (n->str == u"arguments") mark_arguments();
                return finish(n);
            }
            default: fail("unexpected end of input");
        }
    }
    void mark_arguments() {
        for (auto it = fns.rbegin(); it != fns.rend(); ++it) {
            if (it->is_arrow) continue;
            if (it->node) it->node->flags |= F_USES_ARGUMENTS;
            break;
        }
    }

    Node* function_expr(bool async) {
        const Token& t = peek();
        if (async) i++;
        i++;
        bool gen = eat_p("*");
        Node* name = nullptr;
        if (!is_p("(")) {
            const Token& nt = peek();
            // the name follows the function's own generator/async-ness
            bool ok;
            {
                FnCtx c;
                c.is_async = async;
                c.is_gen = gen;
                fns.push_back(c);
                ok = ident_ok(nt);
                fns.pop_back();
            }
            if (!ok) fail("bad function name");
            if (strict && (nt.value == u"eval" || nt.value == u"arguments")) fail("eval/arguments as function name");
            name = make(N::Ident, nt);
            name->str = nt.value;
            i++;
            finish(name);
        }
        return function_rest(t, async, gen, false, name, 0, true);
    }

    Node* paren() {
        const Token& t = next();
        Node* e = expression(false);
        expect_p(")");
        e->flags |= F_PARENS;
        (void)t;
        return e;
    }

    Node* array_literal() {
        Node* n = make(N::Array, next());
        while (!is_p("]")) {
            if (is_p(",")) {
                i++;
                n->kids.push_back(make(N::Hole, peek()));
                continue;
            }
            if (is_p("...")) {
                Node* s = make(N::Spread, next());
                s->a = assignment(false);
                n->kids.push_back(finish(s));
            } else {
                n->kids.push_back(assignment(false));
            }
            if (!is_p("]")) {
                expect_p(",");
                // a trailing comma after a spread makes it invalid as a rest pattern
                if (n->kids.back()->type == N::Spread && is_p("]")) n->flags |= F_COMPUTED;
            }
        }
        i++;
        return finish(n);
    }

    Node* object_literal() {
        Node* n = make(N::Object, next());
        bool has_proto = false;
        while (!is_p("}")) {
            const Token& t = peek();
            if (is_p("...")) {
                Node* s = make(N::Spread, next());
                s->a = assignment(false);
                n->kids.push_back(finish(s));
                if (!is_p("}")) expect_p(",");
                continue;
            }
            Node* p = make(N::Property, t);
            bool async = false, gen = false;
            int kind = 0;
            if (is_word("async") && !peek().escaped && !peek(1).nl_before && !is_p(",", 1) && !is_p(":", 1) && !is_p("(", 1) &&
                !is_p("}", 1) && !is_p("=", 1)) {
                i++;
                async = true;
            }
            if (eat_p("*")) gen = true;
            if (!async && !gen && (is_word("get") || is_word("set")) && !peek().escaped && !is_p(",", 1) && !is_p(":", 1) &&
                !is_p("(", 1) && !is_p("}", 1) && !is_p("=", 1)) {
                kind = is_word("get") ? 1 : 2;
                i++;
            }
            const Token& kt = peek();
            p->a = property_key(p);
            if (is_p("(")) {
                int fl = kind == 1 ? F_GETTER : kind == 2 ? F_SETTER : 0;
                p->b = function_rest(t, async, gen, true, nullptr, fl);
                p->flags |= kind == 1 ? F_GETTER : kind == 2 ? F_SETTER : F_METHOD;
            } else {
                if (kind || async || gen) fail("bad object literal property");
                if (eat_p(":")) {
                    p->b = assignment(false);
                    if (!(p->flags & F_COMPUTED) && p->a->str == u"__proto__") {
                        if (has_proto) p->flags |= F_PROTO | F_COVER_INIT * 0;
                        if (has_proto) n->flags |= F_PROTO;  // duplicate __proto__ (error unless a pattern)
                        has_proto = true;
                        p->flags |= F_PROTO;
                    }
                } else {
                    // shorthand
                    if ((p->flags & F_COMPUTED) || !(kt.type == T::Ident || kt.type == T::Keyword)) fail("bad shorthand property");
                    i--;
                    Node* id = make(N::Ident, peek());
                    id->str = ident();
                    if (id->str == u"arguments") mark_arguments();
                    finish(id);
                    p->flags |= F_SHORTHAND;
                    if (is_p("=")) {
                        // CoverInitializedName: only valid as a pattern
                        Node* a = make(N::Assign, peek());
                        i++;
                        a->str = u"=";
                        a->a = id;
                        a->b = assignment(false);
                        p->b = finish(a);
                        p->flags |= F_COVER_INIT;
                    } else {
                        p->b = id;
                    }
                }
            }
            n->kids.push_back(finish(p));
            if (!is_p("}")) expect_p(",");
        }
        i++;
        return finish(n);
    }
};

}  // namespace

void parse(Ast& ast, const ParseOptions& opt) {
    Parser p(ast);
    p.program(opt);
}

void parse_function_ctor(Ast& ast, const u16& params, const u16& body, bool generator, bool async) {
    // parse "(params\n) {\nbody\n}" as one function expression, checking each part alone first
    {
        Ast check;
        check.source = body;
        ParseOptions o;
        o.in_function = true;
        // the body must parse as a function body on its own
        u16 wrapped = u"(" + u16(async ? u"async " : u"") + u"function" + (generator ? u"*" : u"") + u"(){\n" + body + u"\n})";
        check.source = wrapped;
        parse(check, o);
        if (check.root->kids.size() != 1) throw SyntaxError{"bad function body", 1};
    }
    u16 src = u"(" + u16(async ? u"async " : u"") + u"function" + (generator ? u"*" : u"") + u" anonymous(" + params +
              u"\n) {\n" + body + u"\n})";
    ast.source = src;
    ParseOptions o;
    parse(ast, o);
    if (ast.root->kids.size() != 1 || ast.root->kids[0]->type != N::ExprStmt || ast.root->kids[0]->a->type != N::Function)
        throw SyntaxError{"bad function parameters", 1};
}

}  // namespace kjs
