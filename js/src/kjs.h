// kjs: Kestrel's JavaScript engine. Core types: values, heap cells, objects,
// environments, the runtime. Bytecode VM with a mark-and-sweep collector.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kjs {

using u16 = std::u16string;
struct Runtime;
struct Cell;
struct Str;
struct Sym;
struct Obj;
struct Env;
struct Code;
struct Frame;

// ---------------------------------------------------------------- values
enum class Tag : uint8_t { Undefined, Null, Bool, Number, String, Symbol, Object, Empty };

struct Value {
    Tag tag = Tag::Undefined;
    union {
        bool b;
        double n;
        Str* s;
        Sym* y;
        Obj* o;
        Cell* c;
    };
    Value() : n(0) {}
    static Value undef() { return Value(); }
    static Value null() { Value v; v.tag = Tag::Null; return v; }
    static Value empty() { Value v; v.tag = Tag::Empty; return v; }
    Value(bool x) : tag(Tag::Bool) { b = x; }
    Value(double x) : tag(Tag::Number) { n = x; }
    Value(int x) : tag(Tag::Number) { n = x; }
    Value(Str* x) : tag(Tag::String) { s = x; }
    Value(Sym* x) : tag(Tag::Symbol) { y = x; }
    Value(Obj* x) : tag(Tag::Object) { o = x; }
    bool is_undef() const { return tag == Tag::Undefined; }
    bool is_null() const { return tag == Tag::Null; }
    bool is_nullish() const { return tag == Tag::Undefined || tag == Tag::Null; }
    bool is_bool() const { return tag == Tag::Bool; }
    bool is_num() const { return tag == Tag::Number; }
    bool is_str() const { return tag == Tag::String; }
    bool is_sym() const { return tag == Tag::Symbol; }
    bool is_obj() const { return tag == Tag::Object; }
    bool is_empty() const { return tag == Tag::Empty; }
    // the heap cell, if any (Empty values may carry an internal cell: private names, class data)
    Cell* cell() const { return (tag == Tag::String || tag == Tag::Symbol || tag == Tag::Object || tag == Tag::Empty) ? c : nullptr; }
};

// ---------------------------------------------------------------- heap
struct Cell {
    Cell* next = nullptr;
    bool marked = false;
    virtual ~Cell() {}
    virtual void trace(Runtime&) {}
};

struct Str : Cell {
    u16 v;
    bool interned = false;
    int64_t idx = -2;  // cached array index (-1: not an index, -2: not computed)
    size_t hash = 0;
    explicit Str(u16 s) : v(std::move(s)) {}
};

struct Sym : Cell {
    Value description;  // undefined or string
    bool registered = false;
    void trace(Runtime&) override;
};

// a property key: an interned string or a symbol (pointer identity)
struct Key {
    Cell* p = nullptr;
    bool is_sym = false;
    bool operator==(const Key& o) const { return p == o.p; }
    Str* str() const { return (Str*)p; }
    Sym* sym() const { return (Sym*)p; }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<void*>()(k.p); }
};

enum : uint8_t { WRITABLE = 1, ENUMERABLE = 2, CONFIGURABLE = 4, ACCESSOR = 8 };

struct Prop {
    Value value;  // data value, or getter (as object / undefined) for accessors
    Value setter;
    uint8_t flags = WRITABLE | ENUMERABLE | CONFIGURABLE;
    bool accessor() const { return flags & ACCESSOR; }
};

// a full property descriptor with "present" bits, for defineProperty
struct Desc {
    bool has_value = false, has_get = false, has_set = false, has_writable = false, has_enumerable = false,
         has_configurable = false;
    Value value, get, set;
    bool writable = false, enumerable = false, configurable = false;
    bool is_accessor() const { return has_get || has_set; }
    bool is_data() const { return has_value || has_writable; }
};

enum class Kind : uint8_t {
    Ordinary,
    Array,
    Function,     // bytecode closure
    Native,       // C++ function
    Bound,
    Arguments,
    BooleanBox,
    NumberBox,
    StringBox,
    SymbolBox,
    Error,
    RegExp,
    Date,
    Map,
    Set,
    WeakMap,
    WeakSet,
    Promise,
    Generator,
    AsyncGenerator,
    Proxy,
    ArrayIterator,
    Host,  // a DOM wrapper
};

using NativeFn = Value (*)(Runtime&, Value thisv, const Value* args, int argc, Obj* callee, Value new_target);

struct PrivateName;

struct Obj : Cell {
    Kind kind = Kind::Ordinary;
    Obj* proto = nullptr;
    bool extensible = true;
    bool is_class_constructor = false;
    std::vector<std::pair<Key, Prop>> props;
    std::unordered_map<Key, uint32_t, KeyHash> index;  // built for larger objects
    std::vector<std::pair<PrivateName*, Prop>> privates;
    // Array: dense elements (Empty = hole) and length
    std::vector<Value> elems;
    uint32_t length = 0;
    bool length_writable = true;
    // boxes / internal slots
    Value prim;
    void trace(Runtime&) override;
    int find(Key k) const;
};

struct PrivateName : Cell {
    Value description;
    bool is_method = false, is_accessor = false, is_static = false;
    Value method, getter, setter;
    void trace(Runtime&) override;
};

struct Binding {
    Value v;
    bool mutable_ = true;
    bool initialized = true;  // false while in the TDZ
    bool deletable = false;
    bool strict_const = true;  // assigning to an immutable binding throws only in strict code or for const
};

struct Env : Cell {
    Env* parent = nullptr;
    bool var_scope = false;       // receives var declarations (function, global, strict eval)
    Obj* object = nullptr;        // object environment (global object or `with`)
    bool with_env = false;
    std::unordered_map<Str*, Binding> vars;
    // function environments
    Value this_value;
    bool has_this = false;
    bool this_initialized = true;
    Obj* function = nullptr;      // the closure whose call created this env
    Value new_target;
    Obj* home_object = nullptr;   // for super
    void trace(Runtime&) override;
};

// ---------------------------------------------------------------- functions
enum class FnKind : uint8_t { Normal, Arrow, Method, Getter, Setter, ClassConstructor, DerivedConstructor, Generator, Async, AsyncGenerator, AsyncArrow };

struct Code;  // compiled function body (defined in compiler.h)

struct Function : Obj {
    Code* code = nullptr;
    Env* env = nullptr;
    Obj* home_object = nullptr;
    Value this_value;          // arrows capture via env; kept for bound/class fields
    Obj* fields_init = nullptr; // class field initializer function (for constructors)
    Value bound_target, bound_this;
    std::vector<Value> bound_args;
    NativeFn native = nullptr;
    int native_magic = 0;
    std::vector<Value> slots;   // native closure data
    void trace(Runtime&) override;
};

struct Throw {
    Value v;
};

// ---------------------------------------------------------------- runtime
struct Realm {
    Obj *global = nullptr, *object_proto = nullptr, *function_proto = nullptr, *array_proto = nullptr,
        *string_proto = nullptr, *number_proto = nullptr, *boolean_proto = nullptr, *symbol_proto = nullptr,
        *error_proto = nullptr, *iterator_proto = nullptr, *array_iterator_proto = nullptr,
        *generator_proto = nullptr, *generator_function_proto = nullptr, *async_function_proto = nullptr,
        *async_generator_proto = nullptr, *async_generator_function_proto = nullptr, *promise_proto = nullptr,
        *promise_ctor = nullptr, *regexp_proto = nullptr, *date_proto = nullptr, *map_proto = nullptr,
        *set_proto = nullptr, *weakmap_proto = nullptr, *weakset_proto = nullptr, *object_ctor = nullptr,
        *array_ctor = nullptr, *function_ctor = nullptr, *throw_type_error = nullptr, *async_from_sync_iter_proto = nullptr,
        *string_iterator_proto = nullptr, *map_iterator_proto = nullptr, *set_iterator_proto = nullptr,
        *regexp_string_iterator_proto = nullptr;
    Obj* errors[8] = {};  // Error, TypeError, RangeError, SyntaxError, ReferenceError, EvalError, URIError, AggregateError
    Env* global_env = nullptr;
};

enum ErrType { E_ERROR, E_TYPE, E_RANGE, E_SYNTAX, E_REFERENCE, E_EVAL, E_URI, E_AGGREGATE };

struct Job {
    Value fn;
    std::vector<Value> args;
};

struct Runtime {
    Cell* heap = nullptr;
    size_t allocated = 0, threshold = 8 << 20;
    std::unordered_map<u16, Str*> interned;
    std::vector<Value> roots;        // temporary roots held by native code
    std::vector<Frame*> frames;      // live interpreter frames
    std::vector<Job> jobs;           // the promise job queue
    std::unordered_map<u16, Sym*> symbol_registry;
    Realm realm;
    int depth = 0;
    int unsafe_depth = 0;  // >0 while native code or an implicit call is on the stack (no GC then)
    Obj* eval_fn = nullptr;
    volatile bool interrupt = false;  // set by a watchdog: the VM throws at the next backward jump
    // well-known symbols
    Sym *sym_iterator, *sym_async_iterator, *sym_has_instance, *sym_to_primitive, *sym_to_string_tag,
        *sym_species, *sym_is_concat_spreadable, *sym_unscopables, *sym_match, *sym_match_all, *sym_replace,
        *sym_search, *sym_split;
    std::function<void(const u16&)> print;  // host print hook

    Runtime();
    ~Runtime();

    template <class T, class... A>
    T* alloc(A&&... a) {
        T* p = new T(std::forward<A>(a)...);
        p->next = heap;
        heap = p;
        allocated += sizeof(T);
        return p;
    }
    void maybe_gc();
    void gc();
    void mark(Value v) {
        if (Cell* c = v.cell()) mark(c);
    }
    void mark(Cell* c);
    std::vector<Cell*> gray;

    // strings and keys
    Str* str(const u16& s);
    Str* str(const char* s);
    Str* intern(const u16& s);
    Str* intern(const char* s);
    Key key(const char* s) { return Key{intern(s), false}; }
    Key key(Str* s) { return Key{s->interned ? s : intern(s->v), false}; }
    Key key(Sym* s) { return Key{s, true}; }

    // objects
    Obj* new_object(Obj* proto = nullptr);
    Obj* new_array(uint32_t n = 0);
    Function* new_native(const char* name, NativeFn fn, int length, int magic = 0);
    Obj* make_error(ErrType t, const u16& msg);
    [[noreturn]] void throw_error(ErrType t, const std::string& msg);

    // the high-level object operations (ECMA-262 7.3)
    Value get(Value base, Key k);
    Value get(Obj* o, Key k, Value receiver);
    Value get(Value base, const char* k) { return get(base, key(k)); }
    bool set(Value base, Key k, Value v, bool strict);
    bool set(Obj* o, Key k, Value v, Value receiver);
    bool define(Obj* o, Key k, const Desc& d, bool throw_on_fail = false);
    void define_data(Obj* o, Key k, Value v, uint8_t flags = WRITABLE | CONFIGURABLE);
    void define_data(Obj* o, const char* k, Value v, uint8_t flags = WRITABLE | CONFIGURABLE) { define_data(o, key(k), v, flags); }
    bool get_own(Obj* o, Key k, Desc& d);
    bool has_property(Obj* o, Key k);
    bool has_own(Obj* o, Key k);
    bool delete_property(Obj* o, Key k);
    std::vector<Key> own_keys(Obj* o);
    Obj* get_proto(Obj* o);
    bool set_proto(Obj* o, Obj* proto);
    bool prevent_extensions(Obj* o);
    bool is_extensible(Obj* o);
    Value call(Value f, Value thisv, const std::vector<Value>& args);
    Value call(Value f, Value thisv, const Value* args, int argc);
    Value construct(Value f, const std::vector<Value>& args, Value new_target = Value());
    bool is_callable(Value v);
    bool is_constructor(Value v);
    bool is_array(Value v);
    Value invoke(Value v, Key k, const std::vector<Value>& args);
    Value get_method(Value v, Key k);
    Obj* species_constructor(Obj* o, Obj* def);
    Obj* get_prototype_from_ctor(Value ctor, Obj* def);
    uint64_t length_of(Value o);
    Obj* array_from_list(const std::vector<Value>& v);
    std::vector<Value> list_from_array_like(Value v);

    // conversions (ECMA-262 7.1)
    Value to_primitive(Value v, int hint = 0);  // 0 default, 1 number, 2 string
    bool to_boolean(Value v);
    double to_number(Value v);
    Value to_numeric(Value v);
    double to_integer(Value v);
    int32_t to_int32(Value v);
    uint32_t to_uint32(Value v);
    Str* to_string(Value v);
    u16 str_of(Value v) { return to_string(v)->v; }
    Obj* to_object(Value v);
    Key to_key(Value v);
    uint64_t to_length(Value v);
    uint64_t to_index(Value v);
    Value key_to_value(Key k);

    // operators
    bool strict_equals(Value a, Value b);
    bool loose_equals(Value a, Value b);
    bool same_value(Value a, Value b);
    bool same_value_zero(Value a, Value b);
    Value typeof_(Value v);
    bool instance_of(Value v, Value target);
    Value add(Value a, Value b);

    // iterators
    struct Iter {
        Value iterator, next;
        bool done = false;
    };
    Iter get_iterator(Value v, bool async = false);
    bool iter_step(Iter& it, Value& out);
    void iter_close(Iter& it, bool on_throw);
    Obj* iter_result(Value v, bool done);

    // promises and jobs
    Obj* new_promise(Value& resolve, Value& reject);
    Value promise_resolve(Value ctor, Value v);
    void promise_then(Value p, Value on_ok, Value on_err);
    void run_jobs();

    // scripts
    Value eval_script(const u16& src, const std::string& name, bool strict = false);
    Value direct_eval(const u16& src, Env* env, bool strict, Frame* caller);
    Code* compile(const u16& src, const std::string& name, bool strict, bool is_eval, Env* scope, u16* error, ErrType* et);
    Value run_code(Code* c, Env* env, Value thisv, bool is_eval);

    // RAII temporary roots for native code
    struct Root {
        Runtime& rt;
        size_t n;
        Root(Runtime& r, Value v) : rt(r), n(r.roots.size()) { r.roots.push_back(v); }
        ~Root() { rt.roots.resize(n); }
    };
};

// helpers
u16 to_u16(const std::string& s);
std::string to_utf8(const u16& s);
u16 number_to_u16(double d, int radix = 10);
double string_to_number(const u16& s);
bool is_array_index(const u16& s, uint32_t* out);

void init_builtins(Runtime& rt);
void init_regexp(Runtime& rt);
void init_json(Runtime& rt);
void init_promise(Runtime& rt);
void init_collections(Runtime& rt);
void init_date(Runtime& rt);

}  // namespace kjs
