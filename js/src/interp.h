// Interpreter internals shared by the VM, the runtime and the builtins.
#pragma once
#include "compiler.h"
#include "kjs.h"

namespace kjs {

struct Handler {
    int pc;
    size_t sp;
    Env* env;
};

enum class Suspend : uint8_t { None, Yield, YieldStar, Await, Initial };

struct Frame : Cell {
    Code* code = nullptr;
    Function* callee = nullptr;
    Env* env = nullptr;
    std::vector<Value> stack;
    std::vector<Value> args;
    std::vector<Handler> handlers;
    Value completion;
    Value this_value;
    Value new_target;
    Obj* generator = nullptr;
    int pc = 0;
    Suspend suspended = Suspend::None;
    void trace(Runtime& rt) override;
};

struct Result {
    enum K { Return, Yield, Await } k;
    Value v;
};

// resumption of a suspended frame
struct Resume {
    enum K { None, Next, Throw, Return } k = None;
    Value v;
};

Result execute(Runtime& rt, Frame* f, Resume r);
Value call_function(Runtime& rt, Function* fn, Value thisv, const Value* args, int argc, Value new_target);
Function* make_closure(Runtime& rt, Code* c, Env* env);
void set_function_name(Runtime& rt, Obj* fn, Value key, const char* prefix = nullptr);
void make_constructor(Runtime& rt, Function* fn);
void initialize_instance(Runtime& rt, Obj* obj, Function* ctor);

// an iterator record kept on the operand stack
struct IterRecord : Obj {
    Value iterator, next;
    bool done = false;
    int phase = 0;  // yield* in async generators
    void trace(Runtime& rt) override;
};

// generators
struct Generator : Obj {
    enum State { SuspendedStart, SuspendedYield, Executing, Completed } state = SuspendedStart;
    Frame* frame = nullptr;
    // async generators: queued requests (mode, value, promise capability)
    struct Req {
        Resume::K mode;
        Value value, promise, resolve, reject;
    };
    std::vector<Req> queue;
    bool awaiting_return = false;
    void trace(Runtime& rt) override;
};

Value generator_resume(Runtime& rt, Generator* g, Resume::K mode, Value v);
void async_gen_enqueue(Runtime& rt, Generator* g, Resume::K mode, Value v, Value& promise_out);
Value start_async_function(Runtime& rt, Frame* f);

// class field records stored on constructors
struct FieldRecord {
    Value key;  // property key value, or a PrivateName object (as Value with Cell)
    PrivateName* priv = nullptr;
    Value init;  // function or undefined
};
struct ClassData : Cell {
    std::vector<FieldRecord> fields;        // instance fields
    std::vector<PrivateName*> methods;      // instance private methods/accessors
    std::vector<FieldRecord> static_fields; // pending static fields (consumed in order)
    size_t static_next = 0;
    void trace(Runtime& rt) override;
};
ClassData* class_data(Runtime& rt, Function* ctor, bool create);

// environments
Binding* find_binding(Env* e, Str* name);
Env* new_env(Runtime& rt, Env* parent);

// private names
Value private_get(Runtime& rt, Value obj, PrivateName* pn);
void private_set(Runtime& rt, Value obj, PrivateName* pn, Value v);
void private_add(Runtime& rt, Obj* o, PrivateName* pn, Value v);

// the template object of a tagged template
Obj* template_object(Runtime& rt, Code* c, int idx);

// arguments objects
Obj* make_arguments(Runtime& rt, Frame* f, bool mapped);

}  // namespace kjs
