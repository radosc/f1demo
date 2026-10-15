// Fallout INT bytecode interpreter (stack machine compatible with compiled SSL scripts).
#pragma once
#include "core/base.h"

namespace fo {

enum VmType : u16 {
    VT_INT = 0xC001,
    VT_FLOAT = 0xA001,
    VT_STRING = 0x9001,  // interned string id
    VT_PTR = 0xE001,     // object handle (uid), 0 = null
};

struct Value {
    u16 type = VT_INT;
    union {
        i32 i;
        float f;
        u32 u;
    };
    i32 raw = 0; // raw operand of a static string push (also an identifier offset for externals)
    Value() : i(0) {}
    static Value Int(i32 v) { Value x; x.type = VT_INT; x.i = v; return x; }
    static Value Float(float v) { Value x; x.type = VT_FLOAT; x.f = v; return x; }
    static Value Ptr(u32 v) { Value x; x.type = VT_PTR; x.u = v; return x; }
    static Value Str(const std::string& s);
    bool isString() const { return type == VT_STRING; }
    bool truthy() const;
    std::string toString() const;
    i32 toInt() const;
    float toFloat() const;
};

// Global string pool used by all programs (scripts are small; strings are never freed).
u32 vmIntern(const std::string& s);
const std::string& vmString(u32 id);

struct ProgramProc {
    std::string name;
    u32 flags = 0;
    int time = 0;
    int condition = 0;
    int body = 0;
    int args = 0;
};

// Shared bytecode of one .int file.
struct ProgramData {
    std::string name;
    Bytes code;
    std::vector<ProgramProc> procs;
    int identBase = 0;   // offset of identifier table (size field)
    int stringBase = 0;  // offset of static string table (size field)
    bool parse(const std::string& nm, Bytes bytes, std::string* error);
    int findProc(const std::string& procName) const;
    std::string identifier(int offset) const;
    std::string staticString(int offset) const;
};

class Vm;
using OpHandler = void (*)(Vm& vm);

class VmError : public std::exception {
public:
    explicit VmError(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
    std::string msg;
};

class Vm {
public:
    enum Flags : u32 {
        F_EXITED = 0x01,
        F_ERROR = 0x04,
        F_STOPPED = 0x08,
        F_WAITING = 0x10,
        F_CHILD = 0x20,
        F_RETURNED = 0x40,
        F_CRITICAL = 0x80,
    };

    std::shared_ptr<ProgramData> prog;
    std::vector<Value> stack;   // data stack
    std::vector<Value> rstack;  // return stack
    int ip = 0;
    int fp = -1;                // frame pointer (locals/args)
    int bp = -1;                // base pointer (globals)
    u32 flags = 0;
    u32 currentOpcode = 0;
    void* userData = nullptr;   // owning ScriptInstance
    u32 waitUntil = 0;
    bool initialized = false;
    int instructionBudget = 2000000;

    explicit Vm(std::shared_ptr<ProgramData> p) : prog(std::move(p)) {}

    // Runs the program from offset 0 (globals init + "start" body). Returns false on error.
    bool runInit();
    // Calls a procedure by index with no arguments (engine entry point). Returns false on error.
    bool callProc(int index);
    bool callProcByName(const std::string& name);
    bool failed() const { return (flags & F_ERROR) != 0; }

    // stack helpers for opcode handlers
    void push(const Value& v) { stack.push_back(v); }
    void pushInt(i32 v) { stack.push_back(Value::Int(v)); }
    void pushFloat(float v) { stack.push_back(Value::Float(v)); }
    void pushPtr(u32 uid) { stack.push_back(Value::Ptr(uid)); }
    void pushString(const std::string& s) { stack.push_back(Value::Str(s)); }
    Value pop();
    i32 popInt() { return pop().toInt(); }
    float popFloat() { return pop().toFloat(); }
    std::string popString() { return pop().toString(); }
    u32 popPtr();
    void rpush(const Value& v) { rstack.push_back(v); }
    Value rpop();
    [[noreturn]] void error(const std::string& msg);

    static void registerOp(u16 opcode, OpHandler h, const char* name);
    static const char* opName(u16 opcode);

private:
    void runLoop();
    void setupCall(int address, int returnAddress);
};

} // namespace fo
