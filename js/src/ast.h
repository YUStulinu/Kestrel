// The syntax tree, the lexer and the parser of kjs.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "kjs.h"

namespace kjs {

enum class T : uint8_t {
    Eof, Ident, Keyword, Punct, Num, BigInt, String, Template, Regex, Private,
};

struct Token {
    T type = T::Eof;
    u16 value;        // identifier name, punctuator, string value, regex body, template cooked
    u16 raw;          // template raw / regex flags
    double num = 0;
    bool nl_before = false;
    bool escaped = false;      // identifier written with \u escapes
    bool legacy_octal = false; // 0777 or "\07" (strict mode error)
    bool cooked_invalid = false;
    bool tmpl_tail = false;    // template part ends with ` (otherwise ${)
    bool tmpl_head = false;    // template part starts with ` (otherwise })
    uint32_t start = 0, end = 0;
    int line = 1;
};

enum class N : uint8_t {
    Program, VarDecl, Declarator, Function, Class, ClassMember, Return, If, For, ForIn, ForOf, While, DoWhile, Break,
    Continue, Throw, Try, Switch, Case, Labeled, Block, ExprStmt, Empty, Debugger, With,
    Ident, Num, Str, Template, TaggedTemplate, Regex, Bool, Null, This, Super, Array, Object, Property, Unary, Update,
    Binary, Logical, Assign, Cond, Call, New, Member, Optional, Seq, Spread, Yield, Await, NewTarget, PrivateName,
    PrivateIn, ArrayPattern, ObjectPattern, AssignPattern, Rest, Hole, BigIntLit, ImportCall, StaticBlock,
};

struct Node {
    N type;
    std::vector<Node*> kids;  // meaning depends on type
    Node* a = nullptr;        // first operand / object / target
    Node* b = nullptr;        // second operand / property / value
    Node* c = nullptr;        // third (alternate, update)
    u16 str;                  // names, operators, string values
    double num = 0;
    int flags = 0;
    uint32_t start = 0, end = 0;
    int line = 0;
};

// node flags
enum : int {
    F_COMPUTED = 1, F_OPTIONAL = 2, F_PREFIX = 4, F_STATIC = 8, F_ASYNC = 16, F_GENERATOR = 32, F_ARROW = 64,
    F_STRICT = 128, F_EXPR_BODY = 256, F_METHOD = 512, F_GETTER = 1024, F_SETTER = 2048, F_SHORTHAND = 4096,
    F_DELEGATE = 8192, F_AWAIT = 16384, F_PARENS = 32768, F_CONST = 1 << 16, F_LET = 1 << 17, F_FIELD = 1 << 18,
    F_SIMPLE_PARAMS = 1 << 19, F_DERIVED = 1 << 20, F_CLASS_CTOR = 1 << 21, F_DECL = 1 << 22, F_USES_ARGUMENTS = 1 << 23,
    F_USES_EVAL = 1 << 24, F_USES_THIS = 1 << 25, F_PROTO = 1 << 26, F_CHAIN_END = 1 << 27, F_COVER_INIT = 1 << 28,
    F_USING = 1 << 29,
};

// Function node: kids = params, b = body (Block or expression), str = name, a = name ident (for decls)
// Class node: a = name, b = superclass, kids = members
// ClassMember: a = key, b = value (Function or field init), flags: F_STATIC, F_GETTER, F_SETTER, F_FIELD, F_COMPUTED
// Property (object literal): a = key, b = value, flags: F_COMPUTED, F_SHORTHAND, F_METHOD, F_GETTER, F_SETTER; type Spread for ...

struct SyntaxError {
    std::string msg;
    int line;
};

struct Ast {
    std::vector<std::unique_ptr<Node>> pool;
    Node* root = nullptr;
    u16 source;
    Node* make(N t) {
        pool.push_back(std::make_unique<Node>());
        pool.back()->type = t;
        return pool.back().get();
    }
};

// parse a script (or eval code / function body); throws SyntaxError
struct ParseOptions {
    bool strict = false;
    bool is_eval = false;
    bool in_function = false;        // eval inside a function: new.target, super allowed per flags
    bool allow_new_target = false;
    bool allow_super_property = false;
    bool allow_super_call = false;
    bool allow_arguments = true;     // false inside class field initializers
    bool in_class_field = false;
    std::vector<u16> private_names;  // private names visible to eval code
};
void parse(Ast& ast, const ParseOptions& opt);
// parse the parameters and body of `new Function(params, body)`
void parse_function_ctor(Ast& ast, const u16& params, const u16& body, bool generator, bool async);

bool regexp_syntax_ok(const u16& pattern, const u16& flags, std::string* err);
bool is_id_start(char32_t c);
bool is_id_part(char32_t c);
bool is_js_space(char32_t c);
bool is_line_term(char32_t c);

}  // namespace kjs
