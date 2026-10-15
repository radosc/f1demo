#include "game/vm.h"

#include <climits>

namespace fo {

// ------------------------------------------------------------------ string pool
static std::vector<std::string> g_strPool = { std::string() };
static std::unordered_map<std::string, u32> g_strIndex = { { std::string(), 0u } };

u32 vmIntern(const std::string& s)
{
    auto it = g_strIndex.find(s);
    if (it != g_strIndex.end()) return it->second;
    u32 id = (u32)g_strPool.size();
    g_strPool.push_back(s);
    g_strIndex[s] = id;
    return id;
}

const std::string& vmString(u32 id) { return id < g_strPool.size() ? g_strPool[id] : g_strPool[0]; }

Value Value::Str(const std::string& s)
{
    Value v;
    v.type = VT_STRING;
    v.u = vmIntern(s);
    return v;
}

bool Value::truthy() const
{
    switch (type) {
    case VT_FLOAT: return (u & 0x7FFFFFFF) != 0;
    case VT_STRING: return true;
    default: return i != 0;
    }
}

std::string Value::toString() const
{
    switch (type) {
    case VT_STRING: return vmString(u);
    case VT_FLOAT: return strFormat("%.5f", f);
    case VT_PTR: return strFormat("obj#%u", u);
    default: return std::to_string(i);
    }
}

i32 Value::toInt() const
{
    switch (type) {
    case VT_FLOAT: return (i32)f;
    case VT_STRING: return atoi(vmString(u).c_str());
    default: return i;
    }
}

float Value::toFloat() const
{
    switch (type) {
    case VT_FLOAT: return f;
    case VT_STRING: return (float)atof(vmString(u).c_str());
    default: return (float)i;
    }
}

// ------------------------------------------------------------------ program data
bool ProgramData::parse(const std::string& nm, Bytes bytes, std::string* error)
{
    name = nm;
    code = std::move(bytes);
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    if (code.size() < 46) return fail("file too small");
    int count = rdBE32s(&code[42]);
    if (count < 0 || 46 + (size_t)count * 24 > code.size()) return fail("bad procedure table");
    identBase = 46 + count * 24;
    if ((size_t)identBase + 4 > code.size()) return fail("bad identifier table");
    int identSize = rdBE32s(&code[identBase]);
    stringBase = identBase + identSize + 4;
    if (identSize < 0 || (size_t)stringBase + 4 > code.size()) return fail("bad identifier size");
    procs.clear();
    for (int i = 0; i < count; i++) {
        const u8* p = &code[46 + i * 24];
        ProgramProc pr;
        int nameOff = rdBE32s(p);
        pr.flags = rdBE32(p + 4);
        pr.time = rdBE32s(p + 8);
        pr.condition = rdBE32s(p + 12);
        pr.body = rdBE32s(p + 16);
        pr.args = rdBE32s(p + 20);
        pr.name = identifier(nameOff);
        procs.push_back(pr);
    }
    return true;
}

std::string ProgramData::identifier(int offset) const
{
    size_t p = (size_t)identBase + (size_t)offset;
    if (offset < 0 || p >= code.size()) return {};
    size_t e = p;
    while (e < code.size() && code[e]) e++;
    return std::string((const char*)&code[p], e - p);
}

std::string ProgramData::staticString(int offset) const
{
    // static strings are addressed relative to the table's size field + 4 (see interpretGetString)
    size_t p = (size_t)stringBase + 4 + (size_t)offset;
    if (offset < 0 || p >= code.size()) return {};
    size_t e = p;
    while (e < code.size() && code[e]) e++;
    return std::string((const char*)&code[p], e - p);
}

int ProgramData::findProc(const std::string& procName) const
{
    for (size_t i = 0; i < procs.size(); i++)
        if (strIEquals(procs[i].name, procName)) return (int)i;
    return -1;
}

// ------------------------------------------------------------------ opcode table
static OpHandler g_ops[1024];
static const char* g_opNames[1024];

void Vm::registerOp(u16 opcode, OpHandler h, const char* name)
{
    g_ops[opcode & 0x3FF] = h;
    g_opNames[opcode & 0x3FF] = name;
}

const char* Vm::opName(u16 opcode)
{
    const char* n = g_opNames[opcode & 0x3FF];
    return n ? n : "?";
}

// exported variables (shared between all programs)
static std::unordered_map<std::string, Value> g_exports;

Value Vm::pop()
{
    if (stack.empty()) error("data stack underflow");
    Value v = stack.back();
    stack.pop_back();
    return v;
}

Value Vm::rpop()
{
    if (rstack.empty()) error("return stack underflow");
    Value v = rstack.back();
    rstack.pop_back();
    return v;
}

u32 Vm::popPtr()
{
    Value v = pop();
    if (v.type == VT_PTR) return v.u;
    if (v.type == VT_INT) return (u32)v.i; // 0 = null; ints are tolerated like the original
    return 0;
}

void Vm::error(const std::string& msg)
{
    throw VmError(strFormat("%s @%05X (op %s): %s", prog ? prog->name.c_str() : "?", ip, opName((u16)currentOpcode), msg.c_str()));
}

void Vm::setupCall(int address, int returnAddress)
{
    rpush(Value::Int(ip));
    rpush(Value::Int(returnAddress));
    pushInt((i32)(flags & 0xFFFF));
    pushPtr(0);  // check-wait function
    pushInt(0);  // window id
    pushInt(0);  // argument count
    flags &= ~0xFFFFu;
    ip = address;
}

void Vm::runLoop()
{
    int budget = instructionBudget;
    const Bytes& code = prog->code;
    while (!(flags & (F_EXITED | F_ERROR | F_STOPPED | F_RETURNED | F_WAITING))) {
        if (--budget < 0) error("instruction budget exceeded (infinite loop?)");
        if (ip < 0 || (size_t)ip + 2 > code.size()) error("instruction pointer out of range");
        u16 op = rdBE16(&code[ip]);
        currentOpcode = op;
        ip += 2;
        if (!(op & 0x8000)) error(strFormat("bad opcode %04X", op));
        // push of a typed constant: low bits 0x001 with type bits
        if ((op & 0x3FF) == 1) {
            if ((size_t)ip + 4 > code.size()) error("truncated constant");
            i32 raw = rdBE32s(&code[ip]);
            ip += 4;
            Value v;
            v.i = raw;
            u16 t = op & 0xF7FF;
            if (t == VT_STRING || (op & 0x1000)) {
                v.type = VT_STRING;
                v.raw = raw;
                v.u = vmIntern(prog->staticString(raw));
            } else if (t == VT_FLOAT) {
                v.type = VT_FLOAT;
            } else {
                v.type = VT_INT;
            }
            stack.push_back(v);
            continue;
        }
        OpHandler h = g_ops[op & 0x3FF];
        if (!h) error(strFormat("undefined opcode %04X", op));
        h(*this);
    }
    flags &= ~F_RETURNED;
}

bool Vm::runInit()
{
    ip = 0;
    flags = 0;
    stack.clear();
    rstack.clear();
    fp = bp = -1;
    try {
        runLoop();
    } catch (const VmError& e) {
        flags |= F_ERROR | F_EXITED;
        LOGE("Script error: %s", e.what());
        return false;
    }
    initialized = true;
    return true;
}

bool Vm::callProc(int index)
{
    if (failed() || index < 0 || index >= (int)prog->procs.size()) return false;
    const ProgramProc& p = prog->procs[index];
    int savedIp = ip;
    size_t savedStack = stack.size();
    size_t savedRstack = rstack.size();
    try {
        setupCall(p.body, 24);
        if (p.flags & 0x10) flags |= F_CRITICAL;
        runLoop();
    } catch (const VmError& e) {
        flags |= F_ERROR;
        LOGE("Script error: %s", e.what());
        // restore a sane state so the engine can continue
        ip = savedIp;
        if (stack.size() > savedStack) stack.resize(savedStack);
        if (rstack.size() > savedRstack) rstack.resize(savedRstack);
        return false;
    }
    return true;
}

bool Vm::callProcByName(const std::string& name)
{
    int idx = prog->findProc(name);
    return idx >= 0 && callProc(idx);
}

// ------------------------------------------------------------------ core opcodes
namespace {

int compareValues(Vm& vm, const Value& a, const Value& b, bool& isString)
{
    // returns <0, 0, >0 comparing a to b following the original coercion rules
    isString = false;
    if (a.type == VT_STRING || b.type == VT_STRING) {
        isString = true;
        std::string sa = a.toString(), sb = b.toString();
        int c = strcmp(sa.c_str(), sb.c_str());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    if (a.type == VT_FLOAT || b.type == VT_FLOAT) {
        float fa = a.toFloat(), fb = b.toFloat();
        return fa < fb ? -1 : (fa > fb ? 1 : 0);
    }
    (void)vm;
    // ints and pointers compare by raw value
    i64 ia = a.type == VT_PTR ? (i64)a.u : (i64)a.i;
    i64 ib = b.type == VT_PTR ? (i64)b.u : (i64)b.i;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

void opNoop(Vm&) {}
void opCriticalStart(Vm& vm) { vm.flags |= Vm::F_CRITICAL; }
void opCriticalDone(Vm& vm) { vm.flags &= ~Vm::F_CRITICAL; }
void opJmp(Vm& vm) { vm.ip = vm.popInt(); }

void opCall(Vm& vm)
{
    int idx = vm.popInt();
    if (idx < 0 || idx >= (int)vm.prog->procs.size()) vm.error(strFormat("call to invalid procedure %d", idx));
    const ProgramProc& p = vm.prog->procs[idx];
    if (p.flags & 0x04) vm.error("imported procedures are not supported: " + p.name);
    vm.ip = p.body;
    if (p.flags & 0x10) vm.flags |= Vm::F_CRITICAL;
}

void opCallAt(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    LOGW("%s: call_at is not supported", vm.prog->name.c_str());
}

void opCallWhen(Vm& vm)
{
    vm.popInt();
    vm.popInt();
    LOGW("%s: call_when is not supported", vm.prog->name.c_str());
}

void opChildUnsupported(Vm& vm)
{
    std::string n = vm.popString();
    LOGW("%s: child programs are not supported (%s)", vm.prog->name.c_str(), n.c_str());
}

void opAtoD(Vm& vm) { vm.push(vm.rpop()); }
void opDtoA(Vm& vm) { vm.rpush(vm.pop()); }
void opExit(Vm& vm) { vm.flags |= Vm::F_EXITED; }
void opDetach(Vm&) {}
void opExitProg(Vm& vm) { vm.flags |= Vm::F_EXITED; }
void opStopProg(Vm& vm) { vm.flags |= Vm::F_STOPPED; }

void opFetchGlobal(Vm& vm)
{
    int addr = vm.popInt();
    size_t pos = (size_t)(vm.bp + addr);
    if (vm.bp < 0 || pos >= vm.stack.size()) vm.error(strFormat("global %d out of range", addr));
    vm.push(vm.stack[pos]);
}

void opStoreGlobal(Vm& vm)
{
    int addr = vm.popInt();
    Value v = vm.pop();
    size_t pos = (size_t)(vm.bp + addr);
    if (vm.bp < 0 || pos >= vm.stack.size()) vm.error(strFormat("global %d out of range", addr));
    vm.stack[pos] = v;
}

void opFetchExternal(Vm& vm)
{
    Value addr = vm.pop();
    std::string name = vm.prog->identifier(addr.type == VT_STRING ? addr.raw : addr.i);
    auto it = g_exports.find(strLower(name));
    if (it == g_exports.end()) {
        LOGW("%s: external variable '%s' does not exist (using 0)", vm.prog->name.c_str(), name.c_str());
        vm.pushInt(0);
        return;
    }
    vm.push(it->second);
}

void opStoreExternal(Vm& vm)
{
    Value addr = vm.pop();
    Value v = vm.pop();
    std::string name = strLower(vm.prog->identifier(addr.type == VT_STRING ? addr.raw : addr.i));
    auto it = g_exports.find(name);
    if (it == g_exports.end()) LOGW("%s: storing to unexported variable '%s'", vm.prog->name.c_str(), name.c_str());
    g_exports[name] = v;
}

void opExportVar(Vm& vm)
{
    Value addr = vm.pop();
    std::string name = strLower(vm.prog->identifier(addr.type == VT_STRING ? addr.raw : addr.i));
    if (!g_exports.count(name)) g_exports[name] = Value::Int(0);
}

void opExportProc(Vm& vm)
{
    vm.popInt();
    vm.popInt();
}

void opSwap(Vm& vm)
{
    Value a = vm.pop(), b = vm.pop();
    vm.push(a);
    vm.push(b);
}

void opSwapA(Vm& vm)
{
    Value a = vm.rpop(), b = vm.rpop();
    vm.rpush(a);
    vm.rpush(b);
}

void opPop(Vm& vm) { vm.pop(); }

void opDup(Vm& vm)
{
    Value v = vm.pop();
    vm.push(v);
    vm.push(v);
}

void opPopReturn(Vm& vm) { vm.ip = vm.rpop().i; }

void opPopExit(Vm& vm)
{
    vm.ip = vm.rpop().i;
    vm.flags |= Vm::F_RETURNED;
}

void opPopAddress(Vm& vm) { vm.rpop(); }

void popFlags(Vm& vm)
{
    vm.popInt();           // window id
    vm.popPtr();           // check wait func
    u32 f = (u32)vm.popInt() & 0xFFFF;
    vm.flags = (vm.flags & ~0xFFFFu) | f;
}

void opPopFlags(Vm& vm) { popFlags(vm); }
void opPopFlagsReturn(Vm& vm) { popFlags(vm); opPopReturn(vm); }
void opPopFlagsExit(Vm& vm) { popFlags(vm); opPopExit(vm); }

void opPopFlagsReturnVal(Vm& vm)
{
    Value v = vm.pop();
    popFlags(vm);
    opPopReturn(vm);
    vm.push(v);
}

void opPopFlagsReturnValExit(Vm& vm)
{
    Value v = vm.pop();
    popFlags(vm);
    opPopExit(vm);
    vm.push(v);
}

void opCheckArgCount(Vm& vm)
{
    int expected = vm.popInt();
    int idx = vm.popInt();
    if (idx >= 0 && idx < (int)vm.prog->procs.size() && vm.prog->procs[idx].args != expected)
        vm.error("wrong number of arguments to " + vm.prog->procs[idx].name);
}

void opLookupStringProc(Vm& vm)
{
    std::string name = vm.popString();
    for (size_t i = 1; i < vm.prog->procs.size(); i++) {
        if (strIEquals(vm.prog->procs[i].name, name)) {
            vm.pushInt((i32)i);
            return;
        }
    }
    vm.error("could not find procedure " + name);
}

void opPopBase(Vm& vm) { vm.fp = vm.rpop().i; }

void opPopToBase(Vm& vm)
{
    if (vm.fp < 0) vm.error("pop_to_base without frame");
    if ((int)vm.stack.size() < vm.fp) vm.error("frame below stack");
    vm.stack.resize((size_t)vm.fp);
}

void opPushBase(Vm& vm)
{
    int argc = vm.popInt();
    vm.rpush(Value::Int(vm.fp));
    vm.fp = (int)vm.stack.size() - argc;
}

void opSetGlobal(Vm& vm) { vm.bp = (int)vm.stack.size(); }

void opFetchProcAddress(Vm& vm)
{
    int idx = vm.popInt();
    if (idx < 0 || idx >= (int)vm.prog->procs.size()) vm.error("bad procedure index");
    vm.pushInt(vm.prog->procs[idx].body);
}

void opDump(Vm& vm)
{
    int n = vm.popInt();
    for (int i = 0; i < n; i++) vm.pop();
}

void opIf(Vm& vm)
{
    Value v = vm.pop();
    if (v.truthy()) vm.pop();
    else vm.ip = vm.popInt();
}

void opWhile(Vm& vm)
{
    Value v = vm.pop();
    if (!v.truthy()) vm.ip = vm.popInt();
}

void opStore(Vm& vm)
{
    int addr = vm.popInt();
    Value v = vm.pop();
    size_t pos = (size_t)(vm.fp + addr);
    if (vm.fp < 0 || pos >= vm.stack.size()) vm.error(strFormat("local %d out of range", addr));
    vm.stack[pos] = v;
}

void opFetch(Vm& vm)
{
    int addr = vm.popInt();
    size_t pos = (size_t)(vm.fp + addr);
    if (vm.fp < 0 || pos >= vm.stack.size()) vm.error(strFormat("local %d out of range", addr));
    vm.push(vm.stack[pos]);
}

template <int Kind> void opCompare(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    bool str;
    int c = compareValues(vm, a, b, str);
    bool r = false;
    switch (Kind) {
    case 0: r = c == 0; break;
    case 1: r = c != 0; break;
    case 2: r = c <= 0; break;
    case 3: r = c >= 0; break;
    case 4: r = c < 0; break;
    case 5: r = c > 0; break;
    }
    vm.pushInt(r ? 1 : 0);
}

void opAdd(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    if (a.type == VT_STRING || b.type == VT_STRING) {
        vm.pushString(a.toString() + b.toString());
        return;
    }
    if (a.type == VT_FLOAT || b.type == VT_FLOAT) {
        vm.pushFloat(a.toFloat() + b.toFloat());
        return;
    }
    i64 r = (i64)a.i + (i64)b.i;
    if (r > INT_MAX || r < INT_MIN) vm.pushFloat((float)r);
    else vm.pushInt((i32)r);
}

void opSub(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    if (a.type == VT_FLOAT || b.type == VT_FLOAT) vm.pushFloat(a.toFloat() - b.toFloat());
    else vm.pushInt(a.i - b.i);
}

void opMul(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    if (a.type == VT_FLOAT || b.type == VT_FLOAT) vm.pushFloat(a.toFloat() * b.toFloat());
    else vm.pushInt(a.i * b.i);
}

void opDiv(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    if (a.type == VT_FLOAT || b.type == VT_FLOAT) {
        float d = b.toFloat();
        if (d == 0.0f) {
            LOGW("%s: division by zero", vm.prog->name.c_str());
            vm.pushFloat(0.0f);
        } else {
            vm.pushFloat(a.toFloat() / d);
        }
    } else {
        if (b.i == 0) {
            LOGW("%s: division by zero", vm.prog->name.c_str());
            vm.pushInt(0);
        } else {
            vm.pushInt(a.i / b.i);
        }
    }
}

void opMod(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    int d = b.toInt();
    vm.pushInt(d == 0 ? 0 : a.toInt() % d);
}

void opAnd(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    vm.pushInt(a.truthy() && b.truthy() ? 1 : 0);
}

void opOr(Vm& vm)
{
    Value b = vm.pop();
    Value a = vm.pop();
    vm.pushInt(a.truthy() || b.truthy() ? 1 : 0);
}

void opBwAnd(Vm& vm)
{
    i32 b = vm.popInt(), a = vm.popInt();
    vm.pushInt(a & b);
}

void opBwOr(Vm& vm)
{
    i32 b = vm.popInt(), a = vm.popInt();
    vm.pushInt(a | b);
}

void opBwXor(Vm& vm)
{
    i32 b = vm.popInt(), a = vm.popInt();
    vm.pushInt(a ^ b);
}

void opBwNot(Vm& vm) { vm.pushInt(~vm.popInt()); }

void opFloor(Vm& vm)
{
    Value v = vm.pop();
    if (v.type == VT_FLOAT) vm.pushInt((i32)v.f);
    else vm.push(v);
}

void opNot(Vm& vm)
{
    Value v = vm.pop();
    vm.pushInt(v.i == 0 ? 1 : 0);
}

void opNegate(Vm& vm)
{
    Value v = vm.pop();
    if (v.type == VT_FLOAT) vm.pushFloat(-v.f);
    else vm.pushInt(-v.toInt());
}

void opWait(Vm& vm)
{
    vm.popInt();
    LOGW("%s: wait() is not supported", vm.prog->name.c_str());
}

void opCancel(Vm& vm) { vm.popInt(); }
void opCancelAll(Vm&) {}

struct CoreOps {
    CoreOps()
    {
        Vm::registerOp(0x8000, opNoop, "noop");
        Vm::registerOp(0x8002, opCriticalStart, "critical_start");
        Vm::registerOp(0x8003, opCriticalDone, "critical_done");
        Vm::registerOp(0x8004, opJmp, "jmp");
        Vm::registerOp(0x8005, opCall, "call");
        Vm::registerOp(0x8006, opCallAt, "call_at");
        Vm::registerOp(0x8007, opCallWhen, "call_when");
        Vm::registerOp(0x8008, opChildUnsupported, "callstart");
        Vm::registerOp(0x8009, opChildUnsupported, "exec");
        Vm::registerOp(0x800A, opChildUnsupported, "spawn");
        Vm::registerOp(0x800B, opChildUnsupported, "fork");
        Vm::registerOp(0x800C, opAtoD, "a_to_d");
        Vm::registerOp(0x800D, opDtoA, "d_to_a");
        Vm::registerOp(0x800E, opExit, "exit");
        Vm::registerOp(0x800F, opDetach, "detach");
        Vm::registerOp(0x8010, opExitProg, "exit_prog");
        Vm::registerOp(0x8011, opStopProg, "stop_prog");
        Vm::registerOp(0x8012, opFetchGlobal, "fetch_global");
        Vm::registerOp(0x8013, opStoreGlobal, "store_global");
        Vm::registerOp(0x8014, opFetchExternal, "fetch_external");
        Vm::registerOp(0x8015, opStoreExternal, "store_external");
        Vm::registerOp(0x8016, opExportVar, "export_var");
        Vm::registerOp(0x8017, opExportProc, "export_proc");
        Vm::registerOp(0x8018, opSwap, "swap");
        Vm::registerOp(0x8019, opSwapA, "swapa");
        Vm::registerOp(0x801A, opPop, "pop");
        Vm::registerOp(0x801B, opDup, "dup");
        Vm::registerOp(0x801C, opPopReturn, "pop_return");
        Vm::registerOp(0x801D, opPopExit, "pop_exit");
        Vm::registerOp(0x801E, opPopAddress, "pop_address");
        Vm::registerOp(0x801F, opPopFlags, "pop_flags");
        Vm::registerOp(0x8020, opPopFlagsReturn, "pop_flags_return");
        Vm::registerOp(0x8021, opPopFlagsExit, "pop_flags_exit");
        Vm::registerOp(0x8022, opPopFlagsReturn, "pop_flags_return_extern");
        Vm::registerOp(0x8023, opPopFlagsExit, "pop_flags_exit_extern");
        Vm::registerOp(0x8024, opPopFlagsReturnVal, "pop_flags_return_val_extern");
        Vm::registerOp(0x8025, opPopFlagsReturnValExit, "pop_flags_return_val_exit");
        Vm::registerOp(0x8026, opPopFlagsReturnValExit, "pop_flags_return_val_exit_extern");
        Vm::registerOp(0x8027, opCheckArgCount, "check_arg_count");
        Vm::registerOp(0x8028, opLookupStringProc, "lookup_string_proc");
        Vm::registerOp(0x8029, opPopBase, "pop_base");
        Vm::registerOp(0x802A, opPopToBase, "pop_to_base");
        Vm::registerOp(0x802B, opPushBase, "push_base");
        Vm::registerOp(0x802C, opSetGlobal, "set_global");
        Vm::registerOp(0x802D, opFetchProcAddress, "fetch_proc_address");
        Vm::registerOp(0x802E, opDump, "dump");
        Vm::registerOp(0x802F, opIf, "if");
        Vm::registerOp(0x8030, opWhile, "while");
        Vm::registerOp(0x8031, opStore, "store");
        Vm::registerOp(0x8032, opFetch, "fetch");
        Vm::registerOp(0x8033, opCompare<0>, "equal");
        Vm::registerOp(0x8034, opCompare<1>, "not_equal");
        Vm::registerOp(0x8035, opCompare<2>, "less_equal");
        Vm::registerOp(0x8036, opCompare<3>, "greater_equal");
        Vm::registerOp(0x8037, opCompare<4>, "less");
        Vm::registerOp(0x8038, opCompare<5>, "greater");
        Vm::registerOp(0x8039, opAdd, "add");
        Vm::registerOp(0x803A, opSub, "sub");
        Vm::registerOp(0x803B, opMul, "mul");
        Vm::registerOp(0x803C, opDiv, "div");
        Vm::registerOp(0x803D, opMod, "mod");
        Vm::registerOp(0x803E, opAnd, "and");
        Vm::registerOp(0x803F, opOr, "or");
        Vm::registerOp(0x8040, opBwAnd, "bwand");
        Vm::registerOp(0x8041, opBwOr, "bwor");
        Vm::registerOp(0x8042, opBwXor, "bwxor");
        Vm::registerOp(0x8043, opBwNot, "bwnot");
        Vm::registerOp(0x8044, opFloor, "floor");
        Vm::registerOp(0x8045, opNot, "not");
        Vm::registerOp(0x8046, opNegate, "negate");
        Vm::registerOp(0x8047, opWait, "wait");
        Vm::registerOp(0x8048, opCancel, "cancel");
        Vm::registerOp(0x8049, opCancelAll, "cancelall");
        Vm::registerOp(0x804A, opCriticalStart, "start_critical");
        Vm::registerOp(0x804B, opCriticalDone, "end_critical");
    }
};
CoreOps g_coreOps;

} // namespace

// Exported variable access for save games / debugging.
std::unordered_map<std::string, Value>& vmExports() { return g_exports; }

} // namespace fo
