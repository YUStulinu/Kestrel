// The kjs compiler: syntax tree to stack bytecode. Variables are looked up by
// name at run time (environments are hash maps), which keeps `with` and direct
// eval simple. finally blocks are compiled inline at every exit from the try.
#include "compiler.h"

#include <algorithm>
#include <map>
#include <set>

namespace kjs {

namespace {

struct CompileError {
    std::string msg;
    int line;
};

// a control-stack entry: what has to be undone when jumping out
struct Ctrl {
    enum K { Scope, Try, Finally, Iter, AsyncIter, Held, Jump } k;
    Node* finalizer = nullptr;
    int held = 0;          // operand stack values owned by this entry
    // jump targets (loops, labels, switch)
    std::vector<u16> labels;
    bool is_loop = false, is_switch = false;
    std::vector<int> breaks, continues;
    size_t depth_continue = 0;  // ctrl depth that continue unwinds to
};

struct FnState {
    Code* code;
    Node* node;
    std::vector<Ctrl> ctrl;
    bool strict = false, is_gen = false, is_async = false, is_arrow = false, is_script = false, is_eval = false;
    std::vector<u16> pending_labels;
    int held = 0;  // sum of held values in ctrl
    std::map<Str*, int> name_index;
    bool track_completion = false;
};

void collect_bound(Node* p, std::vector<u16>& out) {
    if (!p) return;
    switch (p->type) {
        case N::Ident: out.push_back(p->str); break;
        case N::AssignPattern: collect_bound(p->a, out); break;
        case N::Rest: collect_bound(p->a, out); break;
        case N::ArrayPattern:
            for (Node* k : p->kids) collect_bound(k, out);
            break;
        case N::ObjectPattern:
            for (Node* k : p->kids) collect_bound(k->type == N::Rest ? k->a : k->b, out);
            break;
        default: break;
    }
}

// var-declared names in a statement subtree (not entering functions); annex B
// block functions are collected separately
void collect_vars(Node* n, std::vector<u16>& vars, std::vector<Node*>* block_fns, bool top) {
    if (!n) return;
    switch (n->type) {
        case N::VarDecl:
            if (!(n->flags & (F_LET | F_CONST)))
                for (Node* d : n->kids) collect_bound(d->a, vars);
            break;
        case N::Function:
            if ((n->flags & F_DECL) && !top && block_fns && !(n->flags & (F_ASYNC | F_GENERATOR))) block_fns->push_back(n);
            break;
        case N::Class: break;
        case N::If: collect_vars(n->b, vars, block_fns, false); collect_vars(n->c, vars, block_fns, false); break;
        case N::For:
            collect_vars(n->kids[0], vars, block_fns, false);
            collect_vars(n->a, vars, block_fns, false);
            break;
        case N::ForIn: case N::ForOf:
            collect_vars(n->a, vars, block_fns, false);
            collect_vars(n->c, vars, block_fns, false);
            break;
        case N::While: case N::DoWhile: collect_vars(n->b, vars, block_fns, false); break;
        case N::Labeled: collect_vars(n->a, vars, block_fns, top); break;
        case N::Block: for (Node* k : n->kids) collect_vars(k, vars, block_fns, false); break;
        case N::Try:
            collect_vars(n->a, vars, block_fns, false);
            if (n->b) collect_vars(n->b->b, vars, block_fns, false);
            collect_vars(n->c, vars, block_fns, false);
            break;
        case N::Switch:
            for (Node* c : n->kids)
                for (Node* s : c->kids) collect_vars(s, vars, block_fns, false);
            break;
        case N::With: collect_vars(n->b, vars, block_fns, false); break;
        default: break;
    }
}

// lexically-declared names directly in a statement list
void collect_lex(const std::vector<Node*>& stmts, std::vector<std::pair<u16, int>>& out, bool fns_are_lexical) {
    for (Node* s : stmts) {
        Node* d = s;
        while (d && d->type == N::Labeled) d = d->a;
        if (!d) continue;
        if (d->type == N::VarDecl && (d->flags & (F_LET | F_CONST))) {
            std::vector<u16> names;
            for (Node* x : d->kids) collect_bound(x->a, names);
            for (auto& n : names) out.push_back({n, (d->flags & F_CONST) ? 1 : 0});
        } else if (d->type == N::Class && (d->flags & F_DECL)) {
            out.push_back({d->a->str, 3});
        } else if (d->type == N::Function && (d->flags & F_DECL) && fns_are_lexical) {
            out.push_back({d->a->str, 2});
        }
    }
}

struct Compiler {
    Runtime& rt;
    Ast& ast;
    std::string file;
    std::vector<FnState> fs;

    Compiler(Runtime& r, Ast& a, const std::string& f) : rt(r), ast(a), file(f) {}

    [[noreturn]] void fail(Node* n, const std::string& m) { throw CompileError{m, n ? n->line : 0}; }
    FnState& f() { return fs.back(); }
    Code* code() { return fs.back().code; }

    int cur_line = 0;
    int emit(Op op, int arg = 0) {
        code()->ops.push_back(op);
        code()->args.push_back(arg);
        code()->lines.push_back(cur_line);
        return int(code()->ops.size()) - 1;
    }
    int here() const { return int(fs.back().code->ops.size()); }
    void patch(int at, int target) { code()->args[at] = target; }
    int name(const u16& s) {
        Str* k = rt.intern(s);
        auto it = f().name_index.find(k);
        if (it != f().name_index.end()) return it->second;
        code()->names.push_back(k);
        int i = int(code()->names.size()) - 1;
        f().name_index[k] = i;
        return i;
    }
    int cnum(double d) {
        code()->consts.push_back(Value(d));
        return int(code()->consts.size()) - 1;
    }
    int cstr(const u16& s) {
        code()->consts.push_back(Value(rt.intern(s)));
        return int(code()->consts.size()) - 1;
    }

    // ------------------------------------------------------------ functions
    Code* new_code(Node* n) {
        Code* c = rt.alloc<Code>();
        c->file = file;
        if (n && n->end > n->start) c->source = ast.source.substr(n->start, n->end - n->start);
        return c;
    }

    Code* compile_script(bool is_eval, bool strict_outer) {
        Node* p = ast.root;
        Code* c = new_code(nullptr);
        c->is_script = !is_eval;
        c->is_eval = is_eval;
        c->strict = strict_outer || (p->flags & F_STRICT);
        fs.push_back(FnState{});
        f().code = c;
        f().node = p;
        f().strict = c->strict;
        f().is_script = !is_eval;
        f().is_eval = is_eval;
        f().track_completion = true;
        // declarations
        std::vector<u16> vars;
        std::vector<Node*> block_fns;
        for (Node* s : p->kids) collect_vars(s, vars, &block_fns, true);
        std::vector<std::pair<u16, int>> lex;
        collect_lex(p->kids, lex, false);
        check_lexical_conflicts(p, lex, vars, false);
        std::vector<Node*> top_fns;
        for (Node* s : p->kids) {
            Node* d = s;
            while (d->type == N::Labeled) d = d->a;
            if (d->type == N::Function && (d->flags & F_DECL)) top_fns.push_back(d);
        }
        // the declaration list: 0 var, 1 function, 2 let, 3 const, 4 class, 5 annex-B function var
        std::vector<std::pair<Str*, int>> decls;
        std::set<u16> fn_names;
        for (auto it = top_fns.rbegin(); it != top_fns.rend(); ++it)
            if (fn_names.insert((*it)->a->str).second) decls.push_back({rt.intern((*it)->a->str), 1});
        for (auto& v : vars) decls.push_back({rt.intern(v), 0});
        for (auto& l : lex) decls.push_back({rt.intern(l.first), l.second == 1 ? 3 : l.second == 3 ? 4 : 2});
        if (!c->strict) {
            std::set<u16> lexset;
            for (auto& l : lex) lexset.insert(l.first);
            for (Node* bf : block_fns)
                if (!lexset.count(bf->a->str) && !fn_names.count(bf->a->str)) decls.push_back({rt.intern(bf->a->str), 5});
        }
        c->decl_lists.push_back(decls);
        emit(is_eval ? OP_EVAL_DECLS : OP_GLOBAL_DECLS, 0);
        // hoisted functions (last declaration of a name wins)
        std::set<u16> done;
        for (auto it = top_fns.rbegin(); it != top_fns.rend(); ++it) {
            Node* fn = *it;
            if (!done.insert(fn->a->str).second) continue;
            emit(OP_CLOSURE, function(fn));
            emit(OP_DECL_FN_VAR, name(fn->a->str));
        }
        emit(OP_UNDEF);
        emit(OP_SET_COMPLETION);
        annexb_names.clear();
        if (!c->strict) {
            std::set<u16> lexset;
            for (auto& l : lex) lexset.insert(l.first);
            for (Node* bf : block_fns)
                if (!lexset.count(bf->a->str)) annexb_names.insert(bf);
        }
        for (Node* s : p->kids) statement(s);
        emit(OP_GET_COMPLETION);
        emit(OP_RETURN);
        fs.pop_back();
        return c;
    }
    std::set<Node*> annexb_names;

    void check_lexical_conflicts(Node* at, const std::vector<std::pair<u16, int>>& lex, const std::vector<u16>& vars,
                                 bool fns_dup_ok) {
        std::set<u16> seen;
        std::map<u16, int> kind;
        for (auto& l : lex) {
            if (seen.count(l.first)) {
                // annex B: duplicate function declarations in a sloppy block
                if (!(fns_dup_ok && l.second == 2 && kind[l.first] == 2)) fail(at, "duplicate lexical declaration");
            }
            seen.insert(l.first);
            kind[l.first] = l.second;
        }
        for (auto& v : vars)
            if (seen.count(v)) fail(at, "lexical declaration conflicts with var");
    }

    int function(Node* fn) {
        Code* c = compile_function(fn);
        code()->functions.push_back(c);
        return int(code()->functions.size()) - 1;
    }

    Code* compile_function(Node* fn) {
        Code* c = new_code(fn);
        bool outer_strict = fs.empty() ? false : f().strict;
        c->strict = outer_strict || (fn->flags & F_STRICT);
        c->is_arrow = fn->flags & F_ARROW;
        c->is_async = fn->flags & F_ASYNC;
        c->is_generator = fn->flags & F_GENERATOR;
        c->is_method = fn->flags & (F_METHOD | F_GETTER | F_SETTER | F_CLASS_CTOR);
        c->is_class_ctor = fn->flags & F_CLASS_CTOR;
        c->is_derived = fn->flags & F_DERIVED;
        c->simple_params = fn->flags & F_SIMPLE_PARAMS;
        c->name = fn->str;
        c->has_name_binding = fn->a && !(fn->flags & F_DECL);  // a named function expression
        if (c->is_generator && c->is_async) c->kind = FnKind::AsyncGenerator;
        else if (c->is_generator) c->kind = FnKind::Generator;
        else if (c->is_async) c->kind = c->is_arrow ? FnKind::AsyncArrow : FnKind::Async;
        else if (c->is_arrow) c->kind = FnKind::Arrow;
        else if (fn->flags & F_CLASS_CTOR) c->kind = (fn->flags & F_DERIVED) ? FnKind::DerivedConstructor : FnKind::ClassConstructor;
        else if (fn->flags & F_GETTER) c->kind = FnKind::Getter;
        else if (fn->flags & F_SETTER) c->kind = FnKind::Setter;
        else if (fn->flags & F_METHOD) c->kind = FnKind::Method;
        int np = 0;
        for (Node* p : fn->kids) {
            if (p->type == N::Rest || p->type == N::AssignPattern) break;
            np++;
        }
        c->nparams = np;
        fs.push_back(FnState{});
        f().code = c;
        f().node = fn;
        f().strict = c->strict;
        f().is_gen = c->is_generator;
        f().is_async = c->is_async;
        f().is_arrow = c->is_arrow;
        std::set<Node*> saved_annexb = std::move(annexb_names);
        annexb_names.clear();
        cur_line = fn->line;

        // parameters
        std::vector<u16> param_names;
        for (Node* p : fn->kids) collect_bound(p, param_names);
        std::set<u16> param_set(param_names.begin(), param_names.end());
        std::vector<u16> vars;
        std::vector<Node*> block_fns;
        std::vector<Node*> body_stmts;
        bool expr_body = fn->flags & F_EXPR_BODY;
        if (!expr_body && fn->b) body_stmts = fn->b->kids;
        for (Node* s : body_stmts) collect_vars(s, vars, &block_fns, true);
        std::vector<std::pair<u16, int>> lex;
        collect_lex(body_stmts, lex, false);
        check_lexical_conflicts(fn, lex, vars, false);
        for (auto& l : lex)
            if (param_set.count(l.first)) fail(fn, "lexical declaration conflicts with parameter");
        std::vector<Node*> top_fns;
        for (Node* s : body_stmts) {
            Node* d = s;
            while (d->type == N::Labeled) d = d->a;
            if (d->type == N::Function && (d->flags & F_DECL)) top_fns.push_back(d);
        }
        std::set<u16> fn_names;
        for (Node* t : top_fns) fn_names.insert(t->a->str);
        bool uses_eval = contains_direct_eval(fn);
        c->uses_eval = uses_eval;
        bool lex_args = false;
        for (auto& l : lex)
            if (l.first == u"arguments") lex_args = true;
        bool needs_arguments = !c->is_arrow && !param_set.count(u"arguments") &&
                               ((fn->flags & F_USES_ARGUMENTS) || uses_eval) &&
                               !(fn_names.count(u"arguments") && !uses_eval) && !lex_args && !(fn->flags & F_FIELD);
        c->uses_arguments = needs_arguments;

        // the named function expression's own binding is created by the VM (an outer scope)
        bool has_param_exprs = false;
        for (Node* p : fn->kids)
            if (p->type != N::Ident) has_param_exprs = true;
        for (auto& pn : param_names) emit(OP_DECL_VAR, name(pn));
        if (needs_arguments) {
            emit(OP_ARGUMENTS, (c->strict || !c->simple_params) ? 0 : 1);
            emit(OP_INIT_VAR_FN, name(u"arguments"));
        }
        // bind the parameters
        for (size_t k = 0; k < fn->kids.size(); k++) {
            Node* p = fn->kids[k];
            cur_line = p->line;
            if (p->type == N::Rest) {
                emit(OP_REST_ARGS, int(k));
                pattern(p->a, 2);
            } else {
                emit(OP_GET_ARG, int(k));
                pattern(p, 2);
            }
        }
        if (has_param_exprs && !vars.empty()) {
            // a separate var environment (parameter expressions cannot see body vars)
            code()->scopes.push_back(ScopeInfo{});
            emit(OP_PUSH_SCOPE, -(int(code()->scopes.size())));  // negative: a var scope
            for (auto& v : vars) {
                emit(OP_DECL_VAR, name(v));
                if (param_set.count(v)) {
                    // the initial value is the parameter's
                    emit(OP_GET_VAR, name(v));
                    emit(OP_INIT_VAR_FN, name(v));
                }
            }
        } else {
            for (auto& v : vars)
                if (!param_set.count(v)) emit(OP_DECL_VAR, name(v));
        }
        // annex B block functions are also vars (when no conflict)
        std::set<u16> lexset;
        for (auto& l : lex) lexset.insert(l.first);
        if (!c->strict) {
            for (Node* bf : block_fns) {
                const u16& n = bf->a->str;
                if (lexset.count(n) || param_set.count(n)) continue;
                emit(OP_DECL_VAR, name(n));
                annexb_names.insert(bf);
            }
        }
        // lexical declarations of the body
        for (auto& l : lex) emit(l.second == 1 ? OP_DECL_CONST : OP_DECL_LEX, name(l.first));
        std::set<u16> done;
        for (auto it = top_fns.rbegin(); it != top_fns.rend(); ++it) {
            Node* t = *it;
            if (!done.insert(t->a->str).second) continue;
            emit(OP_CLOSURE, function(t));
            emit(OP_INIT_VAR_FN, name(t->a->str));
        }
        if (c->is_generator) emit(OP_INITIAL_YIELD);
        if (expr_body) {
            expression(fn->b);
            if (c->is_async && !c->is_generator) {
                emit(OP_RETURN);
            } else {
                emit(OP_RETURN);
            }
        } else {
            for (Node* s : body_stmts) statement(s);
            emit(OP_UNDEF);
            emit(OP_RETURN);
        }
        annexb_names = std::move(saved_annexb);
        fs.pop_back();
        return c;
    }

    bool contains_direct_eval(Node* n) {
        if (!n) return false;
        if (n->type == N::Call && n->a && n->a->type == N::Ident && n->a->str == u"eval" && !(n->a->flags & F_PARENS)) return true;
        // arrows share the enclosing function's arguments and this
        if (n->type == N::Function && !(n->flags & F_ARROW) && n != (fs.empty() ? nullptr : f().node)) return false;
        if (n->type == N::Class) {
            // eval inside a class member can still see the enclosing function's arguments? no: methods have their own
            if (contains_direct_eval(n->b)) return true;
            for (Node* m : n->kids)
                if (m->flags & F_COMPUTED && contains_direct_eval(m->a)) return true;
            return false;
        }
        for (Node* k : n->kids)
            if (contains_direct_eval(k)) return true;
        return contains_direct_eval(n->a) || contains_direct_eval(n->b) || contains_direct_eval(n->c);
    }

    // ------------------------------------------------------------ control flow helpers
    Ctrl& push_ctrl(Ctrl::K k, int held = 0) {
        Ctrl c;
        c.k = k;
        c.held = held;
        f().ctrl.push_back(c);
        f().held += held;
        return f().ctrl.back();
    }
    void pop_ctrl() {
        f().held -= f().ctrl.back().held;
        f().ctrl.pop_back();
    }

    // unwinds the control stack down to `depth` (exclusive), emitting cleanups
    void unwind_to(size_t depth) {
        auto saved = f().ctrl;
        int saved_held = f().held;
        while (f().ctrl.size() > depth) {
            Ctrl c = f().ctrl.back();
            pop_ctrl();
            switch (c.k) {
                case Ctrl::Scope: emit(OP_POP_SCOPE); break;
                case Ctrl::Try: emit(OP_TRY_POP); break;
                case Ctrl::Finally:
                    emit(OP_TRY_POP);
                    block_body(c.finalizer);
                    break;
                case Ctrl::Iter: emit(OP_ITER_CLOSE, 0); break;
                case Ctrl::AsyncIter:
                    emit(OP_ITER_CLOSE, 1);  // rec -> result of return()
                    emit(OP_AWAIT);
                    emit(OP_ASYNC_ITER_RESULT, 1);  // must be an object; popped
                    break;
                case Ctrl::Held:
                    for (int k = 0; k < c.held; k++) emit(OP_POP);
                    break;
                case Ctrl::Jump: break;
            }
        }
        f().ctrl = saved;
        f().held = saved_held;
    }

    void emit_return() {
        // value on the stack
        bool has_cleanup = false;
        for (auto& c : f().ctrl)
            if (c.k != Ctrl::Jump) has_cleanup = true;
        if (has_cleanup) {
            // keep the return value safe below the held values: move it out first
            emit(OP_SET_COMPLETION);
            emit(OP_TRUNCATE, f().held);
            unwind_to(0);
            emit(OP_GET_COMPLETION);
        }
        if (f().is_async && f().is_gen) emit(OP_AWAIT);
        emit(OP_RETURN);
    }

    // ------------------------------------------------------------ statements
    void block_body(Node* b) {
        // a block: its own scope when it declares lexical names
        std::vector<std::pair<u16, int>> lex;
        collect_lex(b->kids, lex, true);
        std::vector<u16> vars;
        for (Node* s : b->kids) collect_vars(s, vars, nullptr, false);
        check_lexical_conflicts(b, lex, vars, !f().strict);
        if (lex.empty()) {
            for (Node* s : b->kids) statement(s);
            return;
        }
        enter_scope(lex);
        hoist_block_functions(b->kids);
        for (Node* s : b->kids) statement(s);
        emit(OP_POP_SCOPE);
        pop_ctrl();
    }
    void enter_scope(const std::vector<std::pair<u16, int>>& lex) {
        ScopeInfo si;
        for (auto& l : lex) si.names.push_back({rt.intern(l.first), l.second});
        code()->scopes.push_back(si);
        emit(OP_PUSH_SCOPE, int(code()->scopes.size()) - 1);
        push_ctrl(Ctrl::Scope);
    }
    void hoist_block_functions(const std::vector<Node*>& stmts) {
        std::set<u16> done;
        for (auto it = stmts.rbegin(); it != stmts.rend(); ++it) {
            Node* d = *it;
            while (d->type == N::Labeled) d = d->a;
            if (d->type == N::Function && (d->flags & F_DECL)) {
                if (!done.insert(d->a->str).second) continue;
                emit(OP_CLOSURE, function(d));
                emit(OP_INIT_VAR, name(d->a->str));
            }
        }
    }

    void completion_undef() {
        if (f().track_completion) {
            emit(OP_UNDEF);
            emit(OP_SET_COMPLETION);
        }
    }

    void statement(Node* s) {
        cur_line = s->line;
        switch (s->type) {
            case N::ExprStmt:
                expression(s->a);
                if (f().track_completion) emit(OP_SET_COMPLETION);
                else emit(OP_POP);
                break;
            case N::VarDecl: var_decl(s); break;
            case N::Function:
                if (annexb_names.count(s)) {
                    // annex B: copy the block binding to the var scope at the declaration
                    emit(OP_GET_VAR, name(s->a->str));
                    emit(OP_INIT_VAR_FN, name(s->a->str));
                }
                break;  // hoisted
            case N::Class:
                class_expr(s, s->a->str);
                emit(OP_INIT_VAR, name(s->a->str));
                break;
            case N::Empty: case N::Debugger: break;
            case N::Block: block_body(s); break;
            case N::If: {
                completion_undef();
                expression(s->a);
                int jf = emit(OP_JMP_FALSE);
                statement(s->b);
                if (s->c) {
                    int je = emit(OP_JMP);
                    patch(jf, here());
                    statement(s->c);
                    patch(je, here());
                } else {
                    patch(jf, here());
                }
                break;
            }
            case N::While: {
                completion_undef();
                Ctrl& j = push_jump(true);
                (void)j;
                int top = here();
                expression(s->a);
                int jf = emit(OP_JMP_FALSE);
                statement(s->b);
                patch_continues(top);
                emit(OP_JMP, top);
                patch(jf, here());
                patch_breaks();
                break;
            }
            case N::DoWhile: {
                completion_undef();
                push_jump(true);
                int top = here();
                statement(s->b);
                patch_continues(here());
                expression(s->a);
                emit(OP_JMP_TRUE, top);
                patch_breaks();
                break;
            }
            case N::For: for_stmt(s); break;
            case N::ForIn: for_in(s); break;
            case N::ForOf: for_of(s); break;
            case N::Return:
                if (s->a) expression(s->a);
                else emit(OP_UNDEF);
                if (f().is_async && !f().is_gen) {
                    // an async function returns its value through the promise
                }
                emit_return();
                break;
            case N::Break: case N::Continue: jump(s); break;
            case N::Throw:
                expression(s->a);
                emit(OP_THROW);
                break;
            case N::Try: try_stmt(s); break;
            case N::Switch: switch_stmt(s); break;
            case N::Labeled: {
                f().pending_labels.push_back(s->str);
                Node* body = s->a;
                bool loop = body->type == N::For || body->type == N::ForIn || body->type == N::ForOf || body->type == N::While ||
                            body->type == N::DoWhile;
                if (loop) {
                    statement(body);
                } else {
                    Ctrl& j = push_ctrl(Ctrl::Jump);
                    j.labels = f().pending_labels;
                    f().pending_labels.clear();
                    statement(body);
                    patch_breaks();
                }
                break;
            }
            case N::With: {
                completion_undef();
                expression(s->a);
                emit(OP_PUSH_WITH);
                push_ctrl(Ctrl::Scope);
                statement(s->b);
                emit(OP_POP_SCOPE);
                pop_ctrl();
                break;
            }
            default: fail(s, "unsupported statement");
        }
    }

    Ctrl& push_jump(bool loop) {
        Ctrl& j = push_ctrl(Ctrl::Jump);
        j.is_loop = loop;
        j.labels = f().pending_labels;
        f().pending_labels.clear();
        j.depth_continue = f().ctrl.size();
        return j;
    }
    void patch_continues(int target) {
        for (int at : f().ctrl.back().continues) patch(at, target);
        f().ctrl.back().continues.clear();
    }
    void patch_breaks() {
        for (int at : f().ctrl.back().breaks) patch(at, here());
        pop_ctrl();
    }

    void jump(Node* s) {
        bool brk = s->type == N::Break;
        for (int k = int(f().ctrl.size()) - 1; k >= 0; k--) {
            Ctrl& c = f().ctrl[k];
            if (c.k != Ctrl::Jump) continue;
            bool match;
            if (s->str.empty()) match = brk ? (c.is_loop || c.is_switch) : c.is_loop;
            else match = std::find(c.labels.begin(), c.labels.end(), s->str) != c.labels.end();
            if (!match) continue;
            if (brk) {
                unwind_to(k + 1);
                int at = emit(OP_JMP);
                f().ctrl[k].breaks.push_back(at);
            } else {
                unwind_to(c.depth_continue);
                int at = emit(OP_JMP);
                f().ctrl[k].continues.push_back(at);
            }
            return;
        }
        fail(s, "bad break/continue target");
    }

    void var_decl(Node* s) {
        bool lexical = s->flags & (F_LET | F_CONST);
        for (Node* d : s->kids) {
            cur_line = d->line;
            if (d->b) {
                if (d->a->type == N::Ident) named_expression(d->b, d->a->str);
                else expression(d->b);
                pattern(d->a, lexical ? 1 : 0);
            } else if (lexical) {
                emit(OP_UNDEF);
                pattern(d->a, 1);
            }
        }
    }

    // binds/assigns the value on the stack to a pattern; mode 0 assign/var, 1 lexical init, 2 parameter
    void pattern(Node* p, int mode) {
        switch (p->type) {
            case N::Ident:
                if (mode == 1) emit(OP_INIT_VAR, name(p->str));
                else if (mode == 2) emit(OP_INIT_VAR_FN, name(p->str));
                else {
                    emit(OP_SET_VAR, name(p->str));
                    emit(OP_POP);
                }
                break;
            case N::Member: {
                // value on stack; evaluate the target then store
                if (p->a->type == N::Super) {
                    emit(OP_THIS);
                    if (p->flags & F_COMPUTED) {
                        expression(p->b);
                        emit(OP_TO_KEY);
                    } else {
                        emit(OP_STR, cstr(p->b->str));
                    }
                    emit(OP_SET_SUPER, 1);  // value this key -> value
                    emit(OP_POP);
                    break;
                }
                expression(p->a);
                if (p->b->type == N::PrivateName) {
                    emit(OP_SWAP);  // obj value
                    emit(OP_SET_PRIVATE, name(u"#" + p->b->str));
                } else if (p->flags & F_COMPUTED) {
                    expression(p->b);
                    emit(OP_TO_KEY);
                    emit(OP_ROTL3);  // value obj key -> obj key value
                    emit(OP_SET_ELEM);
                } else {
                    emit(OP_SWAP);
                    emit(OP_SET_PROP, name(p->b->str));
                }
                emit(OP_POP);
                break;
            }
            case N::Call:
                // annex B f() = x : evaluate the call, then a ReferenceError
                emit(OP_POP);
                expression(p);
                emit(OP_POP);
                emit(OP_THROW_ERROR, cstr(u"invalid assignment target"));
                break;
            case N::AssignPattern: {
                int j = emit(OP_JMP_NOT_UNDEF);
                emit(OP_POP);
                if (p->a->type == N::Ident) named_expression(p->b, p->a->str);
                else expression(p->b);
                patch(j, here());
                pattern(p->a, mode);
                break;
            }
            case N::ObjectPattern: {
                emit(OP_TO_OBJECT_CHECK);
                bool has_rest = !p->kids.empty() && p->kids.back()->type == N::Rest;
                if (has_rest) {
                    emit(OP_NEW_ARRAY);  // the keys taken so far
                    emit(OP_SWAP);       // keys obj
                }
                for (Node* pr : p->kids) {
                    if (pr->type == N::Rest) {
                        // keys obj -> copy
                        emit(OP_SWAP);         // obj keys
                        emit(OP_COPY_DATA_EXCEPT);  // -> rest
                        pattern(pr->a, mode);
                        return;
                    }
                    emit(OP_DUP);
                    if (pr->flags & F_COMPUTED) {
                        expression(pr->a);
                        emit(OP_TO_KEY);
                    } else {
                        emit(OP_STR, cstr(pr->a->str));
                    }
                    if (has_rest) {
                        // remember the key: keys obj obj key -> record in keys
                        emit(OP_DUP);           // keys obj obj key key
                        emit(OP_PICK, 4);       // ... key key keys
                        emit(OP_SWAP);          // ... key keys key
                        emit(OP_ARRAY_PUSH);    // ... key keys
                        emit(OP_POP);           // keys obj obj key
                    }
                    emit(OP_GET_ELEM);
                    pattern(pr->b, mode);
                }
                emit(OP_POP);
                if (has_rest) emit(OP_POP);
                break;
            }
            case N::ArrayPattern: {
                emit(OP_GET_ITER);
                // iterator record on the stack; close it if the pattern throws
                int tp = emit(OP_TRY_PUSH);
                push_ctrl(Ctrl::Try);
                for (Node* el : p->kids) {
                    if (el->type == N::Hole) {
                        emit(OP_ITER_VALUE);
                        emit(OP_POP);
                    } else if (el->type == N::Rest) {
                        emit(OP_ITER_REST);
                        pattern(el->a, mode);
                    } else {
                        emit(OP_ITER_VALUE);
                        pattern(el, mode);
                    }
                }
                pop_ctrl();
                emit(OP_TRY_POP);
                emit(OP_ITER_CLOSE_IF_NOT_DONE);
                int je = emit(OP_JMP);
                patch(tp, here());
                emit(OP_ITER_CLOSE_UNDER);  // rec exc -> exc (closing unless done)
                emit(OP_THROW);
                patch(je, here());
                break;
            }
            default: fail(p, "invalid destructuring target");
        }
    }

    void for_stmt(Node* s) {
        completion_undef();
        Node* init = s->kids[0];
        bool lexical = init && init->type == N::VarDecl && (init->flags & (F_LET | F_CONST));
        std::vector<std::pair<u16, int>> lex;
        if (lexical) {
            collect_lex({init}, lex, false);
            std::vector<u16> body_vars;
            collect_vars(s->a, body_vars, nullptr, false);
            check_lexical_conflicts(s, lex, body_vars, false);
            enter_scope(lex);
        }
        if (init) {
            if (init->type == N::VarDecl) var_decl(init);
            else {
                expression(init);
                emit(OP_POP);
            }
        }
        bool per_iteration = lexical && !(init->flags & F_CONST);
        if (per_iteration) emit(OP_COPY_SCOPE);
        push_jump(true);
        int top = here();
        int jf = -1;
        if (s->kids[1]) {
            expression(s->kids[1]);
            jf = emit(OP_JMP_FALSE);
        }
        statement(s->a);
        patch_continues(here());
        if (per_iteration) emit(OP_COPY_SCOPE);
        if (s->kids[2]) {
            expression(s->kids[2]);
            emit(OP_POP);
        }
        emit(OP_JMP, top);
        if (jf >= 0) patch(jf, here());
        patch_breaks();
        if (lexical) {
            emit(OP_POP_SCOPE);
            pop_ctrl();
        }
    }

    // the left side of for-in/of: declares/assigns the value on the stack
    void for_target(Node* left, bool fresh_scope) {
        if (left->type == N::VarDecl) {
            Node* d = left->kids[0];
            bool lexical = left->flags & (F_LET | F_CONST);
            pattern(d->a, lexical ? 1 : 0);
        } else {
            pattern(left, 0);
        }
        (void)fresh_scope;
    }
    std::vector<std::pair<u16, int>> for_lex(Node* left) {
        std::vector<std::pair<u16, int>> lex;
        if (left->type == N::VarDecl && (left->flags & (F_LET | F_CONST))) collect_lex({left}, lex, false);
        return lex;
    }

    void for_in(Node* s) {
        completion_undef();
        auto lex = for_lex(s->a);
        {
            std::vector<u16> body_vars;
            collect_vars(s->c, body_vars, nullptr, false);
            check_lexical_conflicts(s, lex, body_vars, false);
            std::set<u16> seen;
            for (auto& l : lex) {
                if (seen.count(l.first)) fail(s, "duplicate binding in for-in");
                seen.insert(l.first);
            }
        }
        // annex B initializer
        if (s->a->type == N::VarDecl && s->a->kids[0]->b) {
            named_expression(s->a->kids[0]->b, s->a->kids[0]->a->str);
            pattern(s->a->kids[0]->a, 0);
        }
        // the object is evaluated in a scope holding the uninitialized bindings (TDZ)
        if (!lex.empty()) enter_scope(lex);
        expression(s->b);
        if (!lex.empty()) {
            emit(OP_POP_SCOPE);
            pop_ctrl();
        }
        emit(OP_FOR_IN);
        push_ctrl(Ctrl::Held, 1);
        push_jump(true);
        int top = here();
        int jdone = emit(OP_FOR_IN_NEXT);  // enum -> enum key, or jumps out
        if (!lex.empty()) enter_scope(lex);
        for_target(s->a, true);
        statement(s->c);
        if (!lex.empty()) {
            emit(OP_POP_SCOPE);
            pop_ctrl();
        }
        patch_continues(here());
        emit(OP_JMP, top);
        patch(jdone, here());
        patch_breaks();
        pop_ctrl();
        emit(OP_POP);
    }

    void for_of(Node* s) {
        completion_undef();
        bool is_await = s->flags & F_AWAIT;
        auto lex = for_lex(s->a);
        {
            std::vector<u16> body_vars;
            collect_vars(s->c, body_vars, nullptr, false);
            check_lexical_conflicts(s, lex, body_vars, false);
            std::set<u16> seen;
            for (auto& l : lex) {
                if (seen.count(l.first)) fail(s, "duplicate binding in for-of");
                seen.insert(l.first);
            }
        }
        if (!lex.empty()) enter_scope(lex);
        expression(s->b);
        if (!lex.empty()) {
            emit(OP_POP_SCOPE);
            pop_ctrl();
        }
        emit(is_await ? OP_GET_ASYNC_ITER : OP_GET_ITER);
        push_ctrl(is_await ? Ctrl::AsyncIter : Ctrl::Iter, 1);
        push_jump(true);
        int top = here();
        emit(OP_ITER_NEXT, is_await ? 1 : 0);  // rec -> rec result
        if (is_await) {
            emit(OP_AWAIT);
            emit(OP_ASYNC_ITER_RESULT);  // checks it is an object
        }
        int jdone = emit(OP_ITER_DONE);       // rec result -> rec value, or jumps (popping result)
        // the body: errors close the iterator
        int tp = emit(OP_TRY_PUSH);
        push_ctrl(Ctrl::Try);
        if (!lex.empty()) enter_scope(lex);
        for_target(s->a, true);
        statement(s->c);
        if (!lex.empty()) {
            emit(OP_POP_SCOPE);
            pop_ctrl();
        }
        pop_ctrl();
        emit(OP_TRY_POP);
        int jc = emit(OP_JMP);
        // continue lands here: it has unwound the try already
        patch_continues(here());
        patch(jc, here());
        emit(OP_JMP, top);
        patch(tp, here());
        emit(OP_ITER_CLOSE_UNDER, is_await ? 2 : 1);  // rec exc -> exc, closing the iterator
        if (is_await) emit(OP_AWAIT), emit(OP_POP);
        emit(OP_THROW);
        patch(jdone, here());
        patch_breaks();
        pop_ctrl();
        emit(OP_POP);
    }

    void try_stmt(Node* s) {
        completion_undef();
        if (s->c) {
            // try { } [catch] finally { }
            int tf = emit(OP_TRY_PUSH);
            Ctrl& fc = push_ctrl(Ctrl::Finally);
            fc.finalizer = s->c;
            if (s->b) {
                try_catch(s->a, s->b);
            } else {
                block_body(s->a);
            }
            pop_ctrl();
            emit(OP_TRY_POP);
            // normal completion: the finally block, keeping the completion value
            finally_body(s->c);
            int je = emit(OP_JMP);
            patch(tf, here());
            // the exception is on the stack
            finally_body(s->c);
            emit(OP_THROW);
            patch(je, here());
        } else {
            try_catch(s->a, s->b);
        }
    }
    void finally_body(Node* fin) {
        // the finally block's own completion does not replace the try's unless abrupt
        if (f().track_completion) {
            emit(OP_GET_COMPLETION);
            push_ctrl(Ctrl::Held, 1);
            block_body(fin);
            pop_ctrl();
            emit(OP_SET_COMPLETION);
        } else {
            block_body(fin);
        }
    }
    void try_catch(Node* body, Node* handler) {
        int tc = emit(OP_TRY_PUSH);
        push_ctrl(Ctrl::Try);
        block_body(body);
        pop_ctrl();
        emit(OP_TRY_POP);
        int je = emit(OP_JMP);
        patch(tc, here());
        completion_undef();
        Node* param = handler->a;
        Node* cbody = handler->b;
        if (param) {
            std::vector<u16> names;
            collect_bound(param, names);
            std::set<u16> seen;
            for (auto& n : names) {
                if (seen.count(n)) fail(param, "duplicate catch parameter");
                seen.insert(n);
            }
            // the body may not redeclare the parameter lexically
            std::vector<std::pair<u16, int>> blex;
            collect_lex(cbody->kids, blex, true);
            for (auto& l : blex)
                if (seen.count(l.first)) fail(cbody, "catch parameter redeclared");
            // annex B allows var redeclaration of a simple catch parameter, but not for-of
            if (param->type != N::Ident) {
                std::vector<u16> vars;
                for (Node* st : cbody->kids) collect_vars(st, vars, nullptr, false);
                for (auto& v : vars)
                    if (seen.count(v)) fail(cbody, "catch parameter redeclared by var");
            }
            check_catch_forof(cbody, seen);
            std::vector<std::pair<u16, int>> lex;
            for (auto& n : names) lex.push_back({n, 0});
            enter_scope(lex);
            pattern(param, 1);
            block_body(cbody);
            emit(OP_POP_SCOPE);
            pop_ctrl();
        } else {
            emit(OP_POP);
            block_body(cbody);
        }
        patch(je, here());
    }
    void check_catch_forof(Node* n, const std::set<u16>& names) {
        if (!n) return;
        if (n->type == N::Function || n->type == N::Class) return;
        if (n->type == N::ForOf && n->a->type == N::VarDecl && !(n->a->flags & (F_LET | F_CONST))) {
            std::vector<u16> b;
            collect_bound(n->a->kids[0]->a, b);
            for (auto& x : b)
                if (names.count(x)) fail(n, "catch parameter redeclared by for-of var");
        }
        for (Node* k : n->kids) check_catch_forof(k, names);
        if (n->type != N::ForOf) {
            check_catch_forof(n->a, names);
            check_catch_forof(n->b, names);
        }
        check_catch_forof(n->c, names);
    }

    void switch_stmt(Node* s) {
        completion_undef();
        expression(s->a);
        std::vector<Node*> all;
        for (Node* c : s->kids)
            for (Node* st : c->kids) all.push_back(st);
        std::vector<std::pair<u16, int>> lex;
        collect_lex(all, lex, true);
        std::vector<u16> vars;
        for (Node* st : all) collect_vars(st, vars, nullptr, false);
        check_lexical_conflicts(s, lex, vars, !f().strict);
        if (!lex.empty()) {
            enter_scope(lex);
            hoist_block_functions(all);
        }
        push_ctrl(Ctrl::Held, 1);
        Ctrl& j = push_ctrl(Ctrl::Jump);
        j.is_switch = true;
        j.labels = f().pending_labels;
        f().pending_labels.clear();
        std::vector<int> to_body(s->kids.size(), -1);
        int default_idx = -1;
        for (size_t k = 0; k < s->kids.size(); k++) {
            Node* c = s->kids[k];
            if (!c->a) {
                default_idx = int(k);
                continue;
            }
            emit(OP_DUP);
            expression(c->a);
            emit(OP_SEQ);
            to_body[k] = emit(OP_JMP_TRUE);
        }
        int jdefault = emit(OP_JMP);
        std::vector<int> body_start(s->kids.size());
        for (size_t k = 0; k < s->kids.size(); k++) {
            body_start[k] = here();
            for (Node* st : s->kids[k]->kids) statement(st);
        }
        int end = here();
        for (size_t k = 0; k < s->kids.size(); k++)
            if (to_body[k] >= 0) patch(to_body[k], body_start[k]);
        patch(jdefault, default_idx >= 0 ? body_start[default_idx] : end);
        patch_breaks();
        pop_ctrl();
        emit(OP_POP);
        if (!lex.empty()) {
            emit(OP_POP_SCOPE);
            pop_ctrl();
        }
    }

    // ------------------------------------------------------------ expressions
    bool is_anonymous_fn(Node* e) {
        if (e->type == N::Function) return !e->a && !(e->flags & F_DECL);
        if (e->type == N::Class) return !e->a;
        return false;
    }
    void named_expression(Node* e, const u16& nm) {
        if (is_anonymous_fn(e)) {
            if (e->type == N::Class) {
                class_expr(e, nm);
            } else {
                emit(OP_CLOSURE, function(e));
                emit(OP_SET_FN_NAME, cstr(nm));
            }
        } else {
            expression(e);
        }
    }

    void expression(Node* e) {
        switch (e->type) {
            case N::Num: emit(OP_NUM, cnum(e->num)); break;
            case N::Str: emit(OP_STR, cstr(e->str)); break;
            case N::BigIntLit: emit(OP_BIGINT, cstr(e->str)); break;
            case N::Bool: emit(e->num ? OP_TRUE : OP_FALSE); break;
            case N::Null: emit(OP_NULL); break;
            case N::This: emit(OP_THIS); break;
            case N::Ident:
                if (e->str == u"undefined" && false) emit(OP_UNDEF);
                else emit(OP_GET_VAR, name(e->str));
                break;
            case N::Template: {
                emit(OP_STR, cstr(e->kids[0]->str));
                for (size_t k = 1; k < e->kids.size(); k += 2) {
                    expression(e->kids[k]);
                    emit(OP_TEMPLATE, -1);  // acc value -> acc + ToString(value)
                    if (!e->kids[k + 1]->str.empty()) {
                        emit(OP_STR, cstr(e->kids[k + 1]->str));
                        emit(OP_ADD);
                    }
                }
                break;
            }
            case N::TaggedTemplate: {
                // the tag and its this value
                callee(e->a);
                {
                    std::vector<std::pair<Value, Value>> strs;
                    for (size_t k = 0; k < e->b->kids.size(); k += 2) {
                        Node* q = e->b->kids[k];
                        strs.push_back({(q->flags & F_COVER_INIT) ? Value() : Value(rt.intern(q->str)), Value(rt.intern(q->a->str))});
                    }
                    code()->template_strs.push_back(strs);
                }
                code()->templates.push_back(nullptr);
                emit(OP_TEMPLATE, int(code()->templates.size()) - 1);
                int argc = 1;
                for (size_t k = 1; k < e->b->kids.size(); k += 2) {
                    expression(e->b->kids[k]);
                    argc++;
                }
                emit(OP_CALL, argc);
                break;
            }
            case N::Regex:
                emit(OP_STR, cstr(e->str));
                emit(OP_STR, cstr(e->a->str));
                emit(OP_REGEX);
                break;
            case N::Array: {
                emit(OP_NEW_ARRAY);
                for (Node* el : e->kids) {
                    if (el->type == N::Hole) emit(OP_ARRAY_HOLE);
                    else if (el->type == N::Spread) {
                        expression(el->a);
                        emit(OP_ARRAY_SPREAD);
                    } else {
                        expression(el);
                        emit(OP_ARRAY_PUSH);
                    }
                }
                break;
            }
            case N::Object: object_literal(e); break;
            case N::Function:
                emit(OP_CLOSURE, function(e));
                break;
            case N::Class: class_expr(e, e->a ? e->a->str : u""); break;
            case N::Unary: unary(e); break;
            case N::Update: update(e); break;
            case N::Binary: {
                expression(e->a);
                expression(e->b);
                emit(binop(e->str, e));
                break;
            }
            case N::Logical: {
                expression(e->a);
                Op j = e->str == u"&&" ? OP_JMP_FALSE_KEEP : e->str == u"||" ? OP_JMP_TRUE_KEEP : OP_JMP_NOT_NULLISH_KEEP;
                int at = emit(j);
                emit(OP_POP);
                expression(e->b);
                patch(at, here());
                break;
            }
            case N::Cond: {
                expression(e->a);
                int jf = emit(OP_JMP_FALSE);
                expression(e->b);
                int je = emit(OP_JMP);
                patch(jf, here());
                expression(e->c);
                patch(je, here());
                break;
            }
            case N::Assign: assign(e); break;
            case N::Seq:
                for (size_t k = 0; k < e->kids.size(); k++) {
                    expression(e->kids[k]);
                    if (k + 1 < e->kids.size()) emit(OP_POP);
                }
                break;
            case N::Member: member(e, false); break;
            case N::Call: call(e); break;
            case N::New: {
                expression(e->a);
                if (has_spread(e->kids)) {
                    spread_args(e->kids);
                    emit(OP_NEW_SPREAD);
                } else {
                    for (Node* a : e->kids) expression(a);
                    emit(OP_NEW, int(e->kids.size()));
                }
                break;
            }
            case N::Optional: {
                chain_ends.push_back({});
                expression(e->a);
                int je = emit(OP_JMP);
                for (int at : chain_ends.back()) patch(at, here());
                chain_ends.pop_back();
                // short-circuited: the base (nullish) was left on the stack
                emit(OP_POP);
                emit(OP_UNDEF);
                patch(je, here());
                break;
            }
            case N::Yield: yield(e); break;
            case N::Await:
                expression(e->a);
                emit(OP_AWAIT);
                break;
            case N::NewTarget: emit(OP_NEW_TARGET); break;
            case N::PrivateIn:
                expression(e->b);
                emit(OP_PRIVATE_IN, name(u"#" + e->a->str));
                break;
            case N::ImportCall:
                expression(e->a);
                if (e->b) {
                    expression(e->b);
                    emit(OP_IMPORT_CALL, 1);
                } else {
                    emit(OP_IMPORT_CALL, 0);
                }
                break;
            case N::Super: fail(e, "bad super");
            default: fail(e, "unsupported expression");
        }
    }
    std::vector<std::vector<int>> chain_ends;

    Op binop(const u16& o, Node* e) {
        if (o == u"+") return OP_ADD;
        if (o == u"-") return OP_SUB;
        if (o == u"*") return OP_MUL;
        if (o == u"/") return OP_DIV;
        if (o == u"%") return OP_MOD;
        if (o == u"**") return OP_EXP;
        if (o == u"<<") return OP_SHL;
        if (o == u">>") return OP_SHR;
        if (o == u">>>") return OP_USHR;
        if (o == u"&") return OP_BAND;
        if (o == u"|") return OP_BOR;
        if (o == u"^") return OP_BXOR;
        if (o == u"==") return OP_EQ;
        if (o == u"!=") return OP_NE;
        if (o == u"===") return OP_SEQ;
        if (o == u"!==") return OP_SNE;
        if (o == u"<") return OP_LT;
        if (o == u">") return OP_GT;
        if (o == u"<=") return OP_LE;
        if (o == u">=") return OP_GE;
        if (o == u"instanceof") return OP_INSTANCEOF;
        if (o == u"in") return OP_IN;
        fail(e, "unknown operator");
    }

    void unary(Node* e) {
        const u16& o = e->str;
        if (o == u"typeof" && e->a->type == N::Ident) {
            emit(OP_GET_VAR_TYPEOF, name(e->a->str));
            emit(OP_TYPEOF);
            return;
        }
        if (o == u"delete") {
            Node* t = e->a;
            while (t->type == N::Optional && false) t = t->a;
            if (t->type == N::Ident) {
                emit(OP_DELETE_VAR, name(t->str));
            } else if (t->type == N::Member && t->a->type != N::Super) {
                expression(t->a);
                if (t->flags & F_COMPUTED) {
                    expression(t->b);
                    emit(OP_DELETE_ELEM, f().strict);
                } else {
                    emit(OP_DELETE_PROP, name(t->b->str) * 2 + f().strict);
                }
            } else if (t->type == N::Member) {
                // delete super.x: evaluate the key, then a ReferenceError
                if (t->flags & F_COMPUTED) {
                    expression(t->b);
                    emit(OP_POP);
                }
                emit(OP_THROW_ERROR, cstr(u"\x01" u"delete of a super property"));
            } else if (t->type == N::Optional) {
                // delete a?.b
                chain_ends.push_back({});
                Node* m = t->a;
                if (m->type == N::Member) {
                    if (m->a->type == N::Member || m->a->type == N::Call || m->a->type == N::Ident || true) expression(m->a);
                    if (m->flags & F_OPTIONAL) chain_ends.back().push_back(emit(OP_JMP_NULLISH_KEEP));
                    if (m->flags & F_COMPUTED) {
                        expression(m->b);
                        emit(OP_DELETE_ELEM, f().strict);
                    } else {
                        emit(OP_DELETE_PROP, name(m->b->str) * 2 + f().strict);
                    }
                } else {
                    expression(m);
                    emit(OP_POP);
                    emit(OP_TRUE);
                }
                int je = emit(OP_JMP);
                for (int at : chain_ends.back()) patch(at, here());
                chain_ends.pop_back();
                emit(OP_POP);
                emit(OP_TRUE);
                patch(je, here());
            } else {
                expression(t);
                emit(OP_POP);
                emit(OP_TRUE);
            }
            return;
        }
        expression(e->a);
        if (o == u"!") emit(OP_NOT);
        else if (o == u"-") emit(OP_NEG);
        else if (o == u"+") emit(OP_PLUS);
        else if (o == u"~") emit(OP_BNOT);
        else if (o == u"typeof") emit(OP_TYPEOF);
        else if (o == u"void") {
            emit(OP_POP);
            emit(OP_UNDEF);
        } else fail(e, "unknown unary operator");
    }

    // reads a reference target, leaving what is needed to write it back
    // kinds: 0 identifier, 1 property (obj on stack), 2 element (obj key), 3 super prop (this key), 4 private (obj)
    int ref_read(Node* t) {
        if (t->type == N::Ident) {
            emit(OP_GET_VAR, name(t->str));
            return 0;
        }
        if (t->type == N::Member) {
            if (t->a->type == N::Super) {
                emit(OP_THIS);
                if (t->flags & F_COMPUTED) {
                    expression(t->b);
                    emit(OP_TO_KEY);
                } else {
                    emit(OP_STR, cstr(t->b->str));
                }
                emit(OP_DUP2);
                emit(OP_GET_SUPER);
                return 3;
            }
            expression(t->a);
            if (t->b->type == N::PrivateName) {
                emit(OP_DUP);
                emit(OP_GET_PRIVATE, name(u"#" + t->b->str));
                return 4;
            }
            if (t->flags & F_COMPUTED) {
                expression(t->b);
                emit(OP_TO_KEY);
                emit(OP_DUP2);
                emit(OP_GET_ELEM);
                return 2;
            }
            emit(OP_DUP);
            emit(OP_GET_PROP, name(t->b->str));
            return 1;
        }
        if (t->type == N::Call) {
            // annex B: the call runs, then a ReferenceError
            expression(t);
            emit(OP_POP);
            emit(OP_THROW_ERROR, cstr(u"invalid assignment target"));
            emit(OP_UNDEF);
            return 0;
        }
        fail(t, "invalid assignment target");
    }
    // value on top, the reference parts below: writes and leaves the value
    void ref_write(Node* t, int kind) {
        switch (kind) {
            case 0: emit(OP_SET_VAR, name(t->str)); break;
            case 1: emit(OP_SET_PROP, name(t->b->str)); break;
            case 2: emit(OP_SET_ELEM); break;
            case 3: emit(OP_SET_SUPER, 0); break;
            case 4: emit(OP_SET_PRIVATE, name(u"#" + t->b->str)); break;
        }
    }

    void update(Node* e) {
        bool prefix = e->flags & F_PREFIX;
        int kind = ref_read(e->a);
        emit(OP_TO_NUMERIC);
        if (!prefix) {
            // keep the old value below the reference parts
            int parts = kind == 0 ? 0 : kind == 1 || kind == 4 ? 1 : 2;
            emit(OP_DUP);
            if (parts == 1) emit(OP_ROT3);       // obj old old -> old obj old
            else if (parts == 2) emit(OP_ROT4);  // a b old old -> old a b old
        }
        emit(e->str == u"++" ? OP_INC : OP_DEC);
        ref_write(e->a, kind);
        if (!prefix) emit(OP_POP);
    }

    void assign(Node* e) {
        const u16& o = e->str;
        Node* t = e->a;
        if (o == u"=") {
            if (t->type == N::Ident) {
                named_expression(e->b, t->str);
                emit(OP_SET_VAR, name(t->str));
                return;
            }
            if (t->type == N::Member) {
                if (t->a->type == N::Super) {
                    emit(OP_THIS);
                    if (t->flags & F_COMPUTED) {
                        expression(t->b);
                        emit(OP_TO_KEY);
                    } else {
                        emit(OP_STR, cstr(t->b->str));
                    }
                    expression(e->b);
                    emit(OP_SET_SUPER, 0);
                    return;
                }
                expression(t->a);
                if (t->b->type == N::PrivateName) {
                    expression(e->b);
                    emit(OP_SET_PRIVATE, name(u"#" + t->b->str));
                    return;
                }
                if (t->flags & F_COMPUTED) {
                    expression(t->b);
                    expression(e->b);
                    emit(OP_SET_ELEM);  // converts the key after evaluating the value? (ToPropertyKey happens first in spec)
                    return;
                }
                expression(e->b);
                emit(OP_SET_PROP, name(t->b->str));
                return;
            }
            if (t->type == N::Call) {
                expression(t);
                emit(OP_POP);
                emit(OP_THROW_ERROR, cstr(u"invalid assignment target"));
                emit(OP_UNDEF);
                return;
            }
            // destructuring assignment: the value is the result
            expression(e->b);
            emit(OP_DUP);
            pattern(t, 0);
            return;
        }
        if (o == u"&&=" || o == u"||=" || o == u"??=") {
            int kind = ref_read(t);
            Op j = o == u"&&=" ? OP_JMP_FALSE_KEEP : o == u"||=" ? OP_JMP_TRUE_KEEP : OP_JMP_NOT_NULLISH_KEEP;
            int at = emit(j);
            emit(OP_POP);
            if (t->type == N::Ident) named_expression(e->b, t->str);
            else expression(e->b);
            ref_write(t, kind);
            int je = emit(OP_JMP);
            patch(at, here());
            // short-circuit: drop the reference parts under the value
            int parts = kind == 0 ? 0 : kind == 1 || kind == 4 ? 1 : 2;
            for (int k = 0; k < parts; k++) {
                emit(OP_SWAP);
                emit(OP_POP);
            }
            patch(je, here());
            return;
        }
        int kind = ref_read(t);
        expression(e->b);
        emit(binop(o.substr(0, o.size() - 1), e));
        ref_write(t, kind);
    }

    void member(Node* e, bool keep_object) {
        if (e->a->type == N::Super) {
            emit(OP_THIS);
            if (e->flags & F_COMPUTED) {
                expression(e->b);
                emit(OP_TO_KEY);
            } else {
                emit(OP_STR, cstr(e->b->str));
            }
            emit(OP_GET_SUPER);
            return;
        }
        expression(e->a);
        if (e->flags & F_OPTIONAL) chain_ends.back().push_back(emit(OP_JMP_NULLISH_KEEP));
        if (keep_object) emit(OP_DUP);
        if (e->b->type == N::PrivateName) {
            emit(OP_GET_PRIVATE, name(u"#" + e->b->str));
        } else if (e->flags & F_COMPUTED) {
            expression(e->b);
            emit(OP_GET_ELEM);
        } else {
            emit(OP_GET_PROP, name(e->b->str));
        }
    }

    // pushes [function, this] for a call
    void callee(Node* c) {
        if (c->type == N::Member) {
            if (c->a->type == N::Super) {
                emit(OP_THIS);
                if (c->flags & F_COMPUTED) {
                    expression(c->b);
                    emit(OP_TO_KEY);
                } else {
                    emit(OP_STR, cstr(c->b->str));
                }
                emit(OP_GET_SUPER);
                emit(OP_THIS);
                return;
            }
            member(c, true);  // obj fn
            emit(OP_SWAP);    // fn obj
            return;
        }
        if (c->type == N::Ident) {
            emit(OP_GET_VAR_CALL, name(c->str));  // fn this
            return;
        }
        if (c->type == N::Optional) {
            // (a?.b)() : the chain inside, then call with its base as this
            Node* inner = c->a;
            chain_ends.push_back({});
            if (inner->type == N::Member) {
                member(inner, true);
                emit(OP_SWAP);
            } else {
                expression(inner);
                emit(OP_UNDEF);
            }
            int je = emit(OP_JMP);
            for (int at : chain_ends.back()) patch(at, here());
            chain_ends.pop_back();
            emit(OP_POP);
            emit(OP_UNDEF);
            emit(OP_UNDEF);
            patch(je, here());
            return;
        }
        expression(c);
        emit(OP_UNDEF);
    }

    bool has_spread(const std::vector<Node*>& args) {
        for (Node* a : args)
            if (a->type == N::Spread) return true;
        return false;
    }
    void spread_args(const std::vector<Node*>& args) {
        emit(OP_NEW_ARRAY);
        for (Node* a : args) {
            if (a->type == N::Spread) {
                expression(a->a);
                emit(OP_ARRAY_SPREAD);
            } else {
                expression(a);
                emit(OP_ARRAY_PUSH);
            }
        }
    }

    void call(Node* e) {
        if (e->a->type == N::Super) {
            // super(...)
            if (has_spread(e->kids)) {
                spread_args(e->kids);
                emit(OP_SUPER_CALL_SPREAD);
            } else {
                for (Node* a : e->kids) expression(a);
                emit(OP_SUPER_CALL, int(e->kids.size()));
            }
            return;
        }
        bool direct_eval = e->a->type == N::Ident && e->a->str == u"eval" && !(e->a->flags & F_PARENS) && !(e->flags & F_OPTIONAL);
        callee(e->a);
        if (e->flags & F_OPTIONAL) {
            // fn this : skip if fn is nullish
            emit(OP_SWAP);
            chain_ends.back().push_back(emit(OP_JMP_NULLISH));  // this fn: both become one undefined
            emit(OP_SWAP);
        }
        if (has_spread(e->kids)) {
            spread_args(e->kids);
            emit(direct_eval ? OP_CALL_EVAL_SPREAD : OP_CALL_SPREAD, f().strict);
        } else {
            for (Node* a : e->kids) expression(a);
            if (direct_eval) emit(OP_CALL_EVAL, int(e->kids.size()) * 2 + (f().strict ? 1 : 0));
            else emit(OP_CALL, int(e->kids.size()));
        }
    }

    void yield(Node* e) {
        if (e->flags & F_DELEGATE) {
            expression(e->a);
            emit(OP_YIELD_STAR_INIT, f().is_async);  // -> rec received mode
            // suspends (and re-runs) until the inner iterator is done; a finished
            // return() falls through to the return path, otherwise jumps on
            int jdone = emit(OP_YIELD_STAR);
            emit_return();
            code()->args[jdone] = here() * 2 + (f().is_async ? 1 : 0);
            return;
        }
        if (e->a) expression(e->a);
        else emit(OP_UNDEF);
        if (f().is_async) {
            emit(OP_AWAIT);
            emit(OP_ASYNC_GEN_YIELD);
        } else {
            emit(OP_YIELD);
        }
        // the resumption: next(v) pushes v; throw(e) throws; return(v) returns
        int jret = emit(OP_RESUME_MODE, 0);
        int je = emit(OP_JMP);
        patch(jret, here());
        emit_return();
        patch(je, here());
    }

    void object_literal(Node* e) {
        emit(OP_NEW_OBJECT);
        int protos = 0;
        for (Node* p : e->kids) {
            if (p->type == N::Spread) {
                expression(p->a);
                emit(OP_COPY_DATA);
                continue;
            }
            if (p->flags & F_COVER_INIT) fail(p, "invalid shorthand property initializer");
            bool computed = p->flags & F_COMPUTED;
            if ((p->flags & F_PROTO) && !computed && !(p->flags & (F_SHORTHAND | F_METHOD | F_GETTER | F_SETTER))) {
                if (++protos > 1) fail(p, "duplicate __proto__");
                expression(p->b);
                emit(OP_SET_PROTO_LIT);
                continue;
            }
            if (computed) {
                expression(p->a);
                emit(OP_TO_KEY);
            } else {
                emit(OP_STR, cstr(p->a->str));
            }
            if (p->flags & (F_METHOD | F_GETTER | F_SETTER)) {
                emit(OP_CLOSURE, function(p->b));
                emit((p->flags & F_GETTER) ? OP_DEFINE_GETTER : (p->flags & F_SETTER) ? OP_DEFINE_SETTER : OP_DEFINE_METHOD, 0);
            } else {
                if (is_anonymous_fn(p->b)) {
                    if (p->b->type == N::Class) {
                        // name from the key on the stack
                        emit(OP_DUP);
                        class_expr(p->b, u"", true);
                    } else {
                        emit(OP_CLOSURE, function(p->b));
                        emit(OP_SET_FN_NAME, -1);  // from the key below
                    }
                } else {
                    expression(p->b);
                }
                emit(OP_DEFINE_INIT);
            }
        }
    }

    // a class: leaves the constructor on the stack. name_from_stack: the key is on
    // the stack (computed property name) and is consumed
    void class_expr(Node* c, const u16& binding_name, bool name_from_stack = false) {
        bool has_name = c->a != nullptr;
        // the class scope holds the inner name binding and the private names
        std::vector<std::pair<u16, int>> lex;
        if (has_name) lex.push_back({c->a->str, 1});
        std::set<u16> privates;
        for (Node* m : c->kids)
            if (m->a && m->a->type == N::PrivateName) privates.insert(m->a->str);
        for (auto& p : privates) lex.push_back({u"#" + p, 1});
        enter_scope(lex);
        bool saved_strict = f().strict;
        f().strict = true;
        // private names are created before anything else
        for (auto& p : privates) {
            // the PrivateName gets static/method info as the members are defined
            emit(OP_STR, cstr(p));
            emit(OP_CLASS_PRIVATE, name(u"#" + p));
        }
        if (c->b) {
            expression(c->b);
        } else {
            emit(OP_EMPTY);
        }
        Node* ctor = nullptr;
        for (Node* m : c->kids)
            if (m->flags & F_CLASS_CTOR) ctor = m->b;
        int ctor_idx = -1;
        if (ctor) {
            ctor_idx = function(ctor);
        }
        // CLASS: superclass -> ctor proto ; arg packs ctor index and derived flag
        code()->consts.push_back(Value(rt.intern(has_name ? c->a->str : binding_name)));
        int name_const = int(code()->consts.size()) - 1;
        emit(OP_NUM, cnum(name_const));
        emit(OP_CLASS, (ctor_idx + 1) * 4 + (c->b ? 1 : 0) + (name_from_stack ? 2 : 0));
        // stack: ctor proto
        std::vector<Node*> static_inits;  // fields and blocks, in order
        for (Node* m : c->kids) {
            if (m->flags & F_CLASS_CTOR) continue;
            bool is_static = m->flags & F_STATIC;
            if (m->type == N::StaticBlock) {
                static_inits.push_back(m);
                continue;
            }
            // the home object: ctor (static) or proto
            if (m->flags & F_FIELD) {
                // the key is evaluated now; the initializer runs per instance
                emit(is_static ? OP_SWAP : OP_NOP);  // put the home object on top: proto ctor / ctor proto
                emit(OP_DUP);                        // x home home
                if (m->a->type == N::PrivateName) {
                    emit(OP_GET_VAR, name(u"#" + m->a->str));
                } else if (m->flags & F_COMPUTED) {
                    expression(m->a);
                    emit(OP_TO_KEY);
                } else {
                    emit(OP_STR, cstr(m->a->str));
                }
                if (m->b) {
                    emit(OP_CLOSURE, function(m->b));
                } else {
                    emit(OP_UNDEF);
                }
                emit(OP_CLASS_FIELD, is_static ? 1 : 0);  // home key init -> (records on the ctor or keeps for static)
                emit(is_static ? OP_SWAP : OP_NOP);
                if (is_static) static_inits.push_back(m);
                continue;
            }
            // methods and accessors
            if (is_static) emit(OP_SWAP);  // proto ctor : ctor on top
            emit(OP_DUP);
            if (m->a->type == N::PrivateName) {
                emit(OP_GET_VAR, name(u"#" + m->a->str));
            } else if (m->flags & F_COMPUTED) {
                expression(m->a);
                emit(OP_TO_KEY);
            } else {
                emit(OP_STR, cstr(m->a->str));
            }
            emit(OP_CLOSURE, function(m->b));
            int kind = (m->flags & F_GETTER) ? 1 : (m->flags & F_SETTER) ? 2 : 0;
            emit(OP_CLASS_METHOD, kind * 2 + (is_static ? 1 : 0));  // home key fn -> home
            emit(OP_POP);
            if (is_static) emit(OP_SWAP);
        }
        // stack: ctor proto
        emit(OP_POP);
        // the inner name binding
        if (has_name) {
            emit(OP_DUP);
            emit(OP_INIT_VAR, name(c->a->str));
        }
        // static fields and blocks run with this = ctor
        for (Node* m : static_inits) {
            emit(OP_DUP);
            if (m->type == N::StaticBlock) {
                emit(OP_CLOSURE, function(m->b));
                emit(OP_CLASS_SET_HOME);  // ctor fn -> fn (home = ctor), then call with this = ctor
                emit(OP_SWAP);           // fn ctor
                emit(OP_CALL, 0);
                emit(OP_POP);
            } else {
                emit(OP_INIT_FIELDS, 1);  // ctor -> runs the next static field
            }
        }
        f().strict = saved_strict;
        emit(OP_POP_SCOPE);
        pop_ctrl();
    }
};

}  // namespace

Code* compile_program(Runtime& rt, Ast& ast, const std::string& file, bool is_eval, bool strict_outer) {
    Compiler c(rt, ast, file);
    try {
        return c.compile_script(is_eval, strict_outer);
    } catch (CompileError& e) {
        throw SyntaxError{e.msg, e.line};
    }
}

Code* compile_function_node(Runtime& rt, Ast& ast, Node* fn, const std::string& file) {
    Compiler c(rt, ast, file);
    try {
        return c.compile_function(fn);
    } catch (CompileError& e) {
        throw SyntaxError{e.msg, e.line};
    }
}

}  // namespace kjs
