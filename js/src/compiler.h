// Bytecode: the opcodes, the compiled function (Code), and the compiler entry.
#pragma once
#include "ast.h"
#include "kjs.h"

namespace kjs {

enum Op : uint8_t {
    // constants and stack
    OP_UNDEF, OP_NULL, OP_TRUE, OP_FALSE, OP_NUM, OP_STR, OP_EMPTY, OP_BIGINT,
    OP_POP, OP_DUP, OP_DUP2, OP_SWAP, OP_ROT3, OP_ROTL3, OP_ROT4, OP_PICK, OP_TRUNCATE,
    // variables (operand: name index)
    OP_GET_VAR, OP_GET_VAR_TYPEOF, OP_GET_VAR_CALL, OP_SET_VAR, OP_INIT_VAR, OP_INIT_VAR_FN, OP_DELETE_VAR,
    OP_DECL_VAR, OP_DECL_LEX, OP_DECL_CONST, OP_DECL_FN_VAR, OP_CHECK_REF,
    OP_PUSH_SCOPE, OP_POP_SCOPE, OP_COPY_SCOPE, OP_PUSH_WITH, OP_GLOBAL_DECLS, OP_EVAL_DECLS,
    // properties
    OP_GET_PROP, OP_GET_ELEM, OP_GET_ELEM_KEEP, OP_SET_PROP, OP_SET_ELEM, OP_DELETE_PROP, OP_DELETE_ELEM,
    OP_GET_SUPER, OP_SET_SUPER, OP_GET_PRIVATE, OP_SET_PRIVATE, OP_PRIVATE_IN, OP_TO_KEY, OP_TO_OBJECT_CHECK,
    // literals
    OP_NEW_OBJECT, OP_NEW_ARRAY, OP_ARRAY_PUSH, OP_ARRAY_HOLE, OP_ARRAY_SPREAD, OP_DEFINE_INIT, OP_DEFINE_METHOD,
    OP_DEFINE_GETTER, OP_DEFINE_SETTER, OP_COPY_DATA, OP_COPY_DATA_EXCEPT, OP_SET_PROTO_LIT, OP_TEMPLATE, OP_REGEX,
    OP_SET_FN_NAME,
    // operators
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP, OP_SHL, OP_SHR, OP_USHR, OP_BAND, OP_BOR, OP_BXOR, OP_EQ, OP_NE,
    OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE, OP_INSTANCEOF, OP_IN, OP_NOT, OP_NEG, OP_PLUS, OP_BNOT, OP_TYPEOF,
    OP_INC, OP_DEC, OP_TO_NUMERIC,
    // control
    OP_JMP, OP_JMP_FALSE, OP_JMP_TRUE, OP_JMP_FALSE_KEEP, OP_JMP_TRUE_KEEP, OP_JMP_NULLISH_KEEP, OP_JMP_NOT_NULLISH_KEEP,
    OP_JMP_UNDEF_KEEP, OP_JMP_NOT_UNDEF, OP_JMP_NULLISH,
    OP_CALL, OP_CALL_SPREAD, OP_NEW, OP_NEW_SPREAD, OP_SUPER_CALL, OP_SUPER_CALL_SPREAD, OP_CALL_EVAL,
    OP_CALL_EVAL_SPREAD, OP_RETURN, OP_THROW, OP_THROW_ERROR, OP_TRY_PUSH, OP_TRY_POP, OP_SET_COMPLETION,
    OP_GET_COMPLETION,
    // functions and classes
    OP_CLOSURE, OP_THIS, OP_NEW_TARGET, OP_ARGUMENTS, OP_REST_ARGS, OP_GET_ARG, OP_CLASS, OP_CLASS_METHOD,
    OP_CLASS_FIELD, OP_CLASS_PRIVATE, OP_DEFINE_FIELD, OP_INIT_FIELDS, OP_CLASS_SET_HOME, OP_IMPORT_CALL,
    // iteration
    OP_GET_ITER, OP_GET_ASYNC_ITER, OP_ITER_NEXT, OP_ITER_VALUE, OP_ITER_REST, OP_ITER_CLOSE, OP_ITER_CLOSE_UNDER,
    OP_ITER_CLOSE_IF_NOT_DONE, OP_FOR_IN, OP_FOR_IN_NEXT, OP_ITER_DONE, OP_ASYNC_ITER_RESULT,
    // generators and async
    OP_INITIAL_YIELD, OP_YIELD, OP_AWAIT, OP_YIELD_STAR_INIT, OP_YIELD_STAR, OP_RESUME_MODE, OP_ASYNC_GEN_YIELD,
    OP_DEBUGGER, OP_NOP,
};

// a lexical scope created by OP_PUSH_SCOPE
struct ScopeInfo {
    std::vector<std::pair<Str*, int>> names;  // name, 0 let / 1 const / 2 function (initialized) / 3 class
};

struct Code : Cell {
    std::vector<uint8_t> ops;
    std::vector<int32_t> args;  // one operand per op (unused ones are 0)
    std::vector<int32_t> lines;
    std::vector<Value> consts;  // numbers, strings
    std::vector<Str*> names;
    std::vector<Code*> functions;
    std::vector<ScopeInfo> scopes;
    std::vector<std::vector<std::pair<Str*, int>>> decl_lists;  // for GLOBAL_DECLS / EVAL_DECLS
    std::vector<Obj*> templates;                               // cached template objects
    std::vector<std::vector<std::pair<Value, Value>>> template_strs;  // cooked (or undefined), raw
    u16 name;
    u16 source;            // the function's source text (Function.prototype.toString)
    int nparams = 0;       // "length"
    FnKind kind = FnKind::Normal;
    bool strict = false, is_arrow = false, is_async = false, is_generator = false, is_method = false;
    bool is_class_ctor = false, is_derived = false, simple_params = true, uses_arguments = false, uses_eval = false;
    bool is_script = false, is_eval = false, has_name_binding = false, is_class_field = false;
    bool needs_function_env = true;
    std::string file;
    void trace(Runtime&) override;
};

Code* compile_program(Runtime& rt, Ast& ast, const std::string& file, bool is_eval, bool strict_outer);
Code* compile_function_node(Runtime& rt, Ast& ast, Node* fn, const std::string& file);

}  // namespace kjs
