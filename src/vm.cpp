#include "vm.hpp"

// Computed-goto dispatch measured slower with g++ -O2 on this interpreter (the
// big function optimizes worse), so it is opt-in: -DIXO_COMPUTED_GOTO.
#if defined(__GNUC__) && defined(IXO_COMPUTED_GOTO)
#define IXO_CGOTO 1
#endif

#include "debugger.hpp"
#include "disasm.hpp"
#include "jit.hpp"
#include "profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "builtins.hpp"

namespace ixo {

static const size_t kMaxFrames = 200000;

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

double monotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ fibers

Fiber::Fiber() {
    cap = 32;  // 512 bytes; grows when needed (thousands of tasks stay small)
    stack = static_cast<Value*>(std::malloc(cap * sizeof(Value)));
}

Fiber::~Fiber() { std::free(stack); }

void Fiber::grow() {
    if (cap >= (size_t)1 << 26) throw PanicError{"stack overflow (too much data on the stack)"};
    cap *= 2;
    Value* n = static_cast<Value*>(std::realloc(stack, cap * sizeof(Value)));
    if (!n) throw PanicError{"out of memory"};
    stack = n;
}

// ------------------------------------------------------------------ VM setup

std::atomic<uint64_t> ObjClass::nextId{1};

static std::atomic<uint32_t> nextHeapId{0};
static std::atomic<uint64_t> nextFiberSerial{1};

void rangeLoopInit(VM& vm, Value* v);  // values.cpp

struct JitStats {
    uint64_t entries = 0, budgetExits = 0, compiles = 0, typed = 0, demoted = 0;
    double compileSeconds = 0;
    std::map<std::pair<ObjFunction*, uint32_t>, uint64_t> exits;  // (function, bytecode offset)
    std::vector<std::pair<ObjFunction*, std::string>> notes;       // gave up, didn't compile
};

VM::VM() {
    {  // instances keep their fields right after themselves (see FieldArrayAt)
        alignas(16) char mem[sizeof(ObjInstance) + sizeof(Value)];
        auto* t = new (mem) ObjInstance(nullptr, 1);
        alignas(16) char tmem[sizeof(ObjTuple) + sizeof(Value)];
        auto* tt = tupleInit(tmem, 1);
        if (reinterpret_cast<char*>(t->fields.data()) != mem + sizeof(ObjInstance) ||
            reinterpret_cast<char*>(tt->items.data()) != tmem + sizeof(ObjTuple) || sizeof(ObjInstance) != 24 ||
            sizeof(ObjTuple) != 16) {
            std::fprintf(stderr, "ixo: internal error: unexpected instance layout\n");
            std::abort();
        }
    }
    heapId = (uint16_t)nextHeapId.fetch_add(1);
    const char* jitEnv = std::getenv("IXO_JIT");
    jitOn = jitSupported() && !(jitEnv && std::string(jitEnv) == "0");
    if (const char* sync = std::getenv("IXO_JIT_SYNC")) jitAsync = std::string(sync) == "0";
    else jitAsync = true;
    if (const char* ov = std::getenv("IXO_CHECK_OVERFLOW"); ov && std::string(ov) != "0") {
        checkOverflow = true;
        jitOn = false;  // (the machine code wraps)
    }
    if (jitOn && heapId == 0 && std::getenv("IXO_JIT_STATS")) jitStats = new JitStats();
    startTime = monotonicSeconds();
    registerAllBuiltins(*this);
    // Namespaces that can be called (vec(...), rng(seed), Error(...)) need a
    // stable name so par tasks can find the same function in their own heap.
    for (auto& b : builtins) {
        if (!isObjType(b.value, OT::Namespace)) continue;
        auto* ns = as<ObjNamespace>(b.value);
        if (isObjType(ns->callable, OT::Native) && as<ObjNative>(ns->callable)->path.empty())
            as<ObjNative>(ns->callable)->path = "c:" + ns->name;
    }
}

VM::~VM() {
    jitStop();  // the compiler thread stops first: it reads functions
    delete jitStats;
    delete stepHook;
    for (Fiber* f : fibers) delete f;
    pool.each([&](Obj* o) { destroyObj(o); });
    for (Obj* o : largeObjects) {
        destroyObj(o);
        std::free(o);
    }
}

// The symbol table is shared by all VMs (threads): ids are compiled into
// bytecode that par workers run. Names are never removed, so reads by id need
// no lock once published.
namespace {
constexpr int kMaxSymbols = 1 << 18;
std::mutex symMutex;
std::unordered_map<std::string, int> symIds;
std::string* symTable[kMaxSymbols];
std::atomic<int> symCount{0};
}  // namespace

int VM::sym(const std::string& s) {
    std::lock_guard<std::mutex> lock(symMutex);
    auto it = symIds.find(s);
    if (it != symIds.end()) return it->second;
    int id = symCount.load();
    if (id >= kMaxSymbols) {
        std::fputs("internal error: too many distinct names\n", stderr);
        std::abort();
    }
    symTable[id] = new std::string(s);
    symIds[s] = id;
    symCount.store(id + 1);
    return id;
}

const std::string& VM::symName(int id) { return *symTable[id]; }

int VM::defineBuiltin(const std::string& name, Value v, const std::string& sig, const std::string& doc) {
    auto it = builtinMap.find(name);
    if (it != builtinMap.end()) {
        builtins[it->second] = {name, v, sig, doc};
        return it->second;
    }
    int idx = (int)builtins.size();
    builtins.push_back({name, v, sig, doc});
    builtinMap[name] = idx;
    return idx;
}

Value VM::makeNative(const std::string& name, NativeFn fn, int minA, int maxA) {
    return Value::object(alloc<ObjNative>(name, fn, minA, maxA));
}

Value VM::defineNative(const std::string& name, NativeFn fn, int minA, int maxA, const std::string& sig,
                       const std::string& doc) {
    Value v = makeNative(name, fn, minA, maxA);
    as<ObjNative>(v)->path = "b:" + name;
    defineBuiltin(name, v, sig, doc);
    return v;
}

void VM::defineMethod(Tag tag, const std::string& name, NativeFn fn, int minA, int maxA, const std::string& sig,
                      const std::string& doc) {
    auto* n = alloc<ObjNative>(name, fn, minA + 1, maxA < 0 ? -1 : maxA + 1);
    n->isMethod = true;
    n->path = "m:" + std::to_string((int)tag) + ":" + name;
    methods[(int)tag][sym(name)] = {Value::object(n), sig, doc};
}

void VM::defineMethodValue(Tag tag, const std::string& name, Value fn, const std::string& sig, const std::string& doc) {
    methods[(int)tag][sym(name)] = {fn, sig, doc};
}

ObjNamespace* VM::defineNamespace(const std::string& name, const std::string& doc) {
    auto* ns = alloc<ObjNamespace>(name);
    defineBuiltin(name, Value::object(ns), "", doc);
    return ns;
}

void VM::nsFunc(ObjNamespace* ns, const std::string& name, NativeFn fn, int minA, int maxA, const std::string& sig,
                const std::string& doc) {
    Value v = makeNative(ns->name + "." + name, fn, minA, maxA);
    as<ObjNative>(v)->path = "n:" + ns->name + "." + name;
    ns->members[name] = v;
    memberDocs[ns->name + "." + name] = {sig, doc};
}

void VM::nsValue(ObjNamespace* ns, const std::string& name, Value v, const std::string& sig, const std::string& doc) {
    ns->members[name] = v;
    memberDocs[ns->name + "." + name] = {sig, doc};
}

ModuleRt* VM::newModule(const std::string& name, const std::string& path) {
    modules.push_back(std::make_unique<ModuleRt>());
    ModuleRt* m = modules.back().get();
    m->owner = this;
    m->name = name;
    m->path = path;
    return m;
}

int VM::moduleIndex(ModuleRt* m) const {
    for (size_t i = 0; i < modules.size(); i++)
        if (modules[i].get() == m) return (int)i;
    return -1;
}

Value VM::newList(std::vector<Value> items) {
    auto* l = alloc<ObjList>();
    l->adopt(std::move(items));
    grow(l, l->len() * l->elemSize());
    return Value::object(l);
}

Value VM::newTuple(std::vector<Value> items) {
    ObjTuple* t = newTupleN(items.size());
    std::copy(items.begin(), items.end(), t->items.begin());
    return Value::object(t);
}

Value VM::newError(const std::string& kind, const std::string& msg, Value payload) {
    auto* e = alloc<ObjError>(kind, msg);
    e->payload = payload;
    return Value::object(e);
}

void VM::panic(const std::string& msg) { throw PanicError{msg}; }

// ------------------------------------------------------------------ calls

bool VM::isCallable(Value v) const {
    if (!v.isObj()) return false;
    switch (v.o->type) {
        case OT::Closure:
        case OT::Native:
        case OT::Bound:
        case OT::Class: return true;
        case OT::Namespace: return !static_cast<ObjNamespace*>(v.o)->callable.isNil();
        default: return false;
    }
}

int VM::minArityOf(Value v) const {
    if (!v.isObj()) return -1;
    switch (v.o->type) {
        case OT::Closure: return static_cast<ObjClosure*>(v.o)->fn->minArity;
        case OT::Native: {
            auto* n = static_cast<ObjNative*>(v.o);
            return n->minArity;
        }
        case OT::Bound: {
            auto* b = static_cast<ObjBound*>(v.o);
            if (isObjType(b->method, OT::Closure)) {
                auto* c = as<ObjClosure>(b->method);
                return c->fn->methodStyle ? c->fn->minArity : std::max(0, c->fn->minArity - 1);
            }
            if (isObjType(b->method, OT::Native)) return std::max(0, as<ObjNative>(b->method)->minArity - 1);
            return std::max(0, minArityOf(b->method) - 1);
        }
        case OT::Class: return static_cast<ObjClass*>(v.o)->ctorMinArity;
        case OT::Namespace: return minArityOf(static_cast<ObjNamespace*>(v.o)->callable);
        default: return -1;
    }
}

static std::string arityText(int n) { return std::to_string(n) + (n == 1 ? " argument" : " arguments"); }

static std::string arityError(const std::string& name, int minA, int maxA, int given) {
    std::string takes;
    if (maxA < 0)
        takes = "at least " + arityText(minA);
    else if (minA == maxA)
        takes = arityText(minA);
    else
        takes = std::to_string(minA) + " to " + arityText(maxA);
    return name + "() takes " + takes + " but " + std::to_string(given) + (given == 1 ? " was" : " were") + " given";
}

void VM::callClosure(Fiber* f, ObjClosure* c, int argc, size_t base) {
    stepGuard(f);
    ObjFunction* fn = c->fn;
    if (argc != fn->arity) {
        if (fn->lenient) {
            while (argc > fn->arity) {
                f->sp--;
                argc--;
            }
            while (argc < fn->arity) {
                f->push(Value());
                argc++;
            }
        } else {
            if (argc < fn->minArity || argc > fn->arity)
                panic(arityError(fn->name, fn->minArity, fn->arity, argc));
            while (argc < fn->arity) {
                f->push(Value::undef());
                argc++;
            }
        }
    }
    if (f->frames.size() >= kMaxFrames) panic("stack overflow (recursion is too deep in " + fn->name + ")");
    f->frames.push_back({c, fn->chunk.code.data(), base, f->world ? localModule(f, fn->module) : fn->module});
}

bool VM::callValue(Fiber* f, Value callee, int argc) {
    size_t calleeSlot = f->sp - argc - 1;
    if (!callee.isObj()) panic("can't call a " + typeName(callee) + " value");
    switch (callee.o->type) {
        case OT::Closure: {
            auto* c = static_cast<ObjClosure*>(callee.o);
            stepGuard(f);
            if ((c->fn->isFork || c->fn->isPar) && !startingTask) {
                ObjTask* t = c->fn->isPar ? parSpawn(callee, &f->stack[calleeSlot + 1], argc)
                                          : spawn(callee, &f->stack[calleeSlot + 1], argc);
                f->sp = calleeSlot;
                f->push(Value::object(t));
                return false;
            }
            startingTask = false;
            if (jitOn && argc == c->fn->arity && c->fn->jitState != 4 && jitReady(f, c)) {
                JitCtx ctx{};
                ObjFunction* jf = c->fn;
                jf->jitCalls++;
                if (runJit(f, jf, calleeSlot, 0, ctx)) return false;  // done: the result is in place
                // stopping early most of the time: call it in the interpreter (hot loops still use the machine code)
                if (!ctx.budgetExit && !ctx.pausedExit && ++jf->jitBails > 50 && jf->jitBails * 2 > jf->jitCalls) {
                    jf->jitState = 4;
                    jitAllowDirect(jf);
                    if (jitStats) noteJitGiveUp(jf, "calls");
                }
                // stopped early: carry on in the interpreter where the machine code was
                callClosure(f, c, argc, calleeSlot);
                resumeAfterJit(f, ctx);
                if (ctx.budgetExit) instructionBudget = 20000;  // the next safe point lets other tasks run
                return true;
            }
            callClosure(f, c, argc, calleeSlot);
            return true;
        }
        case OT::Native: {
            auto* n = static_cast<ObjNative*>(callee.o);
            if (argc < n->minArity || (n->maxArity >= 0 && argc > n->maxArity))
                panic(arityError(n->name, n->minArity, n->maxArity, argc));
            ObjNative* savedNative = currentNative;
            currentNative = n;
            nativeData = n->data;
            Value r = n->fn(*this, &f->stack[calleeSlot + 1], argc);
            currentNative = savedNative;
            f->sp = calleeSlot;
            if (f->state == FState::Running) f->push(r);
            return false;
        }
        case OT::Bound: {
            auto* b = static_cast<ObjBound*>(callee.o);
            Value m = b->method;
            if (isObjType(m, OT::Closure) && as<ObjClosure>(m)->fn->methodStyle) {
                f->stack[calleeSlot] = b->receiver;
                callClosure(f, as<ObjClosure>(m), argc, calleeSlot);
                return true;
            }
            if (isObjType(m, OT::Native)) {
                auto* n = as<ObjNative>(m);
                f->stack[calleeSlot] = b->receiver;
                int total = argc + 1;
                if (total < n->minArity || (n->maxArity >= 0 && total > n->maxArity)) {
                    if (n->isMethod)
                        panic(arityError(n->name, n->minArity - 1, n->maxArity < 0 ? -1 : n->maxArity - 1, argc));
                    panic(arityError(n->name, n->minArity, n->maxArity, total));
                }
                ObjNative* savedNative = currentNative;
                currentNative = n;
                nativeData = n->data;
                Value r = n->fn(*this, &f->stack[calleeSlot], total);
                currentNative = savedNative;
                f->sp = calleeSlot;
                if (f->state == FState::Running) f->push(r);
                return false;
            }
            // UFCS-bound free function: receiver becomes the first argument
            f->push(Value());
            std::memmove(&f->stack[calleeSlot + 2], &f->stack[calleeSlot + 1], sizeof(Value) * argc);
            f->stack[calleeSlot + 1] = b->receiver;
            f->stack[calleeSlot] = m;
            return callValue(f, m, argc + 1);
        }
        case OT::Class: {
            auto* cls = static_cast<ObjClass*>(callee.o);
            if (cls->ctor) stepGuard(f);
            auto* inst = newInstance(cls);
            f->stack[calleeSlot] = Value::object(inst);
            if (!cls->ctor) {
                if (argc > 0) panic(arityError(cls->name, 0, 0, argc));
                return false;
            }
            startingTask = false;
            callClosure(f, cls->ctor, argc, calleeSlot);
            return true;
        }
        case OT::Namespace: {
            auto* ns = static_cast<ObjNamespace*>(callee.o);
            if (ns->callable.isNil()) panic("'" + ns->name + "' can't be called");
            f->stack[calleeSlot] = ns->callable;
            return callValue(f, ns->callable, argc);
        }
        default: break;
    }
    panic("can't call a " + typeName(callee) + " value");
}

Value VM::fallbackValue(Fiber* f, uint8_t kind, uint16_t index, ObjClosure* caller) {
    switch (kind) {
        case FB_LOCAL: return f->stack[f->frames.back().base + index];
        case FB_UPVAL: {
            ObjUpvalue* u = caller->upvalues[index];
            return u->fiber ? u->fiber->stack[u->slot] : u->closed;
        }
        case FB_GLOBAL: {
            ModuleRt* m = f->frames.empty() ? caller->fn->module : f->frames.back().module;
            return index < m->globals.size() ? sealed(m->globals[index]) : Value::undef();
        }
        case FB_BUILTIN: return builtins[index].value;
        default: return Value::undef();
    }
}

static void checkPrivate(VM& vm, ObjClass* cls, int symId, ObjClosure* caller) {
    auto it = cls->methodPublic.find(symId);
    if (it == cls->methodPublic.end() || it->second) return;
    if (caller && caller->fn->module == cls->module) return;
    vm.panic("method '" + vm.symName(symId) + "' of " + cls->name +
             " is private to its module; mark it with * to make it public (fn " + vm.symName(symId) + "*)");
}

bool VM::invoke(Fiber* f, int symId, int argc, uint8_t fbKind, uint16_t fbIndex, ObjClosure* caller) {
    size_t recvSlot = f->sp - argc - 1;
    Value r = f->stack[recvSlot];
    const std::string& name = symName(symId);
    if (r.isObj()) {
        switch (r.o->type) {
            case OT::Instance: {
                auto* inst = static_cast<ObjInstance*>(r.o);
                auto fi = inst->cls->fieldIndex.find(symId);
                if (fi != inst->cls->fieldIndex.end()) {
                    Value v = inst->fields[fi->second];
                    f->stack[recvSlot] = v;
                    return callValue(f, v, argc);
                }
                auto mi = inst->cls->methods.find(symId);
                if (mi != inst->cls->methods.end()) {
                    checkPrivate(*this, inst->cls, symId, caller);
                    callClosure(f, as<ObjClosure>(mi->second), argc, recvSlot);
                    return true;
                }
                break;
            }
            case OT::Module: {
                ModuleRt* m = static_cast<ObjModule*>(r.o)->rt;
                auto it = m->exports.find(name);
                if (it == m->exports.end()) panic("module '" + m->name + "' has no exported '" + name + "'");
                Value v = sealed(m->globals[it->second]);
                f->stack[recvSlot] = v;
                return callValue(f, v, argc);
            }
            case OT::Namespace: {
                auto* ns = static_cast<ObjNamespace*>(r.o);
                auto it = ns->members.find(name);
                Value v;
                if (it != ns->members.end())
                    v = it->second;
                else if (ns->getFallback)
                    v = ns->getFallback(*this, ns, name, true);
                else
                    panic("'" + ns->name + "' has no member '" + name + "'");
                f->stack[recvSlot] = v;
                return callValue(f, v, argc);
            }
            default: break;
        }
    }
    auto& table = methods[(int)tagOf(r)];
    auto mt = table.find(symId);
    if (mt != table.end()) {
        checkFrozen(r, symId);
        Value m = mt->second.fn;
        if (isObjType(m, OT::Native)) {
            auto* n = as<ObjNative>(m);
            int total = argc + 1;
            if (total < n->minArity || (n->maxArity >= 0 && total > n->maxArity))
                panic(arityError(typeName(r) + "." + name, n->minArity - 1, n->maxArity < 0 ? -1 : n->maxArity - 1,
                                 argc));
            ObjNative* savedNative = currentNative;
            currentNative = n;
            nativeData = n->data;
            Value res = n->fn(*this, &f->stack[recvSlot], total);
            currentNative = savedNative;
            f->sp = recvSlot;
            if (f->state == FState::Running) f->push(res);
            return false;
        }
        callClosure(f, as<ObjClosure>(m), argc, recvSlot);
        return true;
    }
    Value fb = fallbackValue(f, fbKind, fbIndex, caller);
    if (isCallable(fb)) {
        f->push(Value());
        std::memmove(&f->stack[recvSlot + 1], &f->stack[recvSlot], sizeof(Value) * (argc + 1));
        f->stack[recvSlot] = fb;
        return callValue(f, fb, argc + 1);
    }
    if (isObjType(r, OT::Instance))
        panic(typeName(r) + " has no field or method '" + name + "'");
    panic(typeName(r) + " has no method '" + name + "'");
}

void VM::getMember(Fiber* f, int symId, uint8_t fbKind, uint16_t fbIndex, ObjClosure* caller, bool raw) {
    size_t slot = f->sp - 1;
    Value r = f->stack[slot];
    const std::string& name = symName(symId);
    // Which values `x.name` calls: functions declared without a parameter list,
    // and built-in functions that need no arguments (os.platform, time.now).
    auto autoCallable = [&](Value v) {
        if (!v.isObj()) return false;
        OT t = v.o->type;
        if (t == OT::Closure) return static_cast<ObjClosure*>(v.o)->fn->autoCall;
        if (t != OT::Native && t != OT::Bound) return false;
        return minArityOf(v) == 0;
    };
    if (r.isObj()) {
        switch (r.o->type) {
            case OT::Instance: {
                auto* inst = static_cast<ObjInstance*>(r.o);
                auto fi = inst->cls->fieldIndex.find(symId);
                if (fi != inst->cls->fieldIndex.end()) {
                    f->stack[slot] = inst->fields[fi->second];
                    return;
                }
                auto mi = inst->cls->methods.find(symId);
                if (mi != inst->cls->methods.end()) {
                    checkPrivate(*this, inst->cls, symId, caller);
                    auto* c = as<ObjClosure>(mi->second);
                    if (!raw && c->fn->autoCall) {
                        callClosure(f, c, 0, slot);
                        return;
                    }
                    f->stack[slot] = Value::object(alloc<ObjBound>(r, mi->second));
                    return;
                }
                break;
            }
            case OT::Module: {
                ModuleRt* m = static_cast<ObjModule*>(r.o)->rt;
                auto it = m->exports.find(name);
                if (it == m->exports.end()) panic("module '" + m->name + "' has no exported '" + name + "'");
                Value v = sealed(m->globals[it->second]);
                f->stack[slot] = v;
                if (!raw && autoCallable(v)) callValue(f, v, 0);
                return;
            }
            case OT::Namespace: {
                auto* ns = static_cast<ObjNamespace*>(r.o);
                auto it = ns->members.find(name);
                Value v;
                if (it != ns->members.end())
                    v = it->second;
                else if (ns->getFallback)
                    v = ns->getFallback(*this, ns, name, false);
                else
                    panic("'" + ns->name + "' has no member '" + name + "'");
                f->stack[slot] = v;
                if (!raw && autoCallable(v)) callValue(f, v, 0);
                return;
            }
            default: break;
        }
    }
    auto& table = methods[(int)tagOf(r)];
    auto mt = table.find(symId);
    if (mt != table.end()) {
        checkFrozen(r, symId);
        Value m = mt->second.fn;
        bool calls = isObjType(m, OT::Native) ? as<ObjNative>(m)->minArity - 1 == 0 : as<ObjClosure>(m)->fn->autoCall;
        if (!raw && calls) {
            if (isObjType(m, OT::Native)) {
                auto* n = as<ObjNative>(m);
                ObjNative* savedNative = currentNative;
                currentNative = n;
                nativeData = n->data;
                Value res = n->fn(*this, &f->stack[slot], 1);
                currentNative = savedNative;
                f->sp = slot;
                if (f->state == FState::Running) f->push(res);
            } else {
                callClosure(f, as<ObjClosure>(m), 0, slot);
            }
            return;
        }
        f->stack[slot] = Value::object(alloc<ObjBound>(r, m));
        return;
    }
    Value fb = fallbackValue(f, fbKind, fbIndex, caller);
    if (isCallable(fb)) {
        // a function used as a method: x.f is f(x)
        if (!raw) {
            f->push(r);
            f->stack[slot] = fb;
            callValue(f, fb, 1);
            return;
        }
        f->stack[slot] = Value::object(alloc<ObjBound>(r, fb));
        return;
    }
    if (isObjType(r, OT::Instance)) panic(typeName(r) + " has no field or method '" + name + "'");
    panic(typeName(r) + " has no field or method '" + name + "'");
}

// ------------------------------------------------------------------ upvalues

ObjUpvalue* VM::captureUpvalue(Fiber* f, size_t slot) {
    ObjUpvalue* prev = nullptr;
    ObjUpvalue* u = f->openUpvalues;
    while (u && u->slot > slot) {
        prev = u;
        u = u->nextOpen;
    }
    if (u && u->slot == slot) return u;
    auto* created = alloc<ObjUpvalue>();
    created->fiber = f;
    created->slot = slot;
    created->nextOpen = u;
    if (prev)
        prev->nextOpen = created;
    else
        f->openUpvalues = created;
    return created;
}

void VM::closeUpvalues(Fiber* f, size_t fromSlot) {
    while (f->openUpvalues && f->openUpvalues->slot >= fromSlot) {
        ObjUpvalue* u = f->openUpvalues;
        u->closed = f->stack[u->slot];
        u->fiber = nullptr;
        f->openUpvalues = u->nextOpen;
        u->nextOpen = nullptr;
    }
}

// ------------------------------------------------------------------ dispatch

bool VM::gcEnabled() const {
    for (Fiber* f : fibers)
        if (f->nativeDepth > 0) return false;
    return true;
}

void VM::execute(Fiber* f, size_t stopDepth) {
    CallFrame* frame;
    const uint8_t* ip;
    size_t base;
    const Value* consts;
    uint8_t* prof = nullptr;
    const uint8_t* codeBase = nullptr;

#define LOAD()                                                  \
    do {                                                        \
        frame = &f->frames.back();                              \
        ip = frame->ip;                                         \
        base = frame->base;                                     \
        consts = frame->closure->fn->chunk.consts.data();       \
        prof = frame->closure->fn->typeSeen.empty() ? nullptr   \
                                                    : frame->closure->fn->typeSeen.data(); \
        codeBase = frame->closure->fn->chunk.code.data();       \
    } while (0)
// the type of the value just produced (on top), for the optimizing JIT
#define PROFILE()                                              \
    do {                                                       \
        if (prof) prof[ip - codeBase] |= profBit(TOP);         \
    } while (0)
#define SAVE() (frame->ip = ip)
#ifdef IXO_CGOTO
#define TARGET(o) \
    case o:       \
    L_##o:
#define DISPATCH()                          \
    do {                                    \
        if (debugger) goto loop_top;        \
        op = READ_BYTE();                   \
        goto* dispatchTable[op];            \
    } while (0)
#else
#define TARGET(o) case o:
#define DISPATCH() break
#endif
#define READ_BYTE() (*ip++)
#define READ_U16() (ip += 2, (uint16_t)(ip[-2] | (ip[-1] << 8)))
#define READ_U32() \
    (ip += 4, (uint32_t)(ip[-4] | (ip[-3] << 8) | (ip[-2] << 16) | ((uint32_t)ip[-1] << 24)))
#define STK (f->stack)
#define PUSH(v) f->push(v)
#define POP() (f->stack[--f->sp])
#define TOP (f->stack[f->sp - 1])
#define SUSPEND()                                        \
    do {                                                 \
        if (f->state != FState::Running) {               \
            if (stopDepth > 0 && f != stepFiber) panic("can't wait for a flow or task inside a callback"); \
            return;                                      \
        }                                                \
    } while (0)
#define SAFEPOINT()                                                                         \
    do {                                                                                    \
        if (++instructionBudget >= budgetLimit || bytesAllocated > nextGC || gcStress) {    \
            SAVE();                                                                         \
            /* stress mode skips very deep stacks: collecting at every call would be O(depth^2) */ \
            if ((bytesAllocated > nextGC || (gcStress && f->frames.size() < 4096)) &&       \
                f->nativeDepth == 0 && gcEnabled())                                         \
                collectGarbage();                                                           \
            if (instructionBudget >= 20000) {                                               \
                instructionBudget = 0;                                                      \
                if (stopDepth == 0 && f->nativeDepth == 0 && !runQueue.empty()) {           \
                    f->state = FState::Ready;                                               \
                    enqueue(f);                                                             \
                    return;                                                                 \
                }                                                                           \
            }                                                                               \
        }                                                                                   \
    } while (0)

    LOAD();
    if (profiler && f->nativeDepth == 0) profiler->resync();
#ifdef IXO_CGOTO
    // Hot instructions jump straight to the next one's code; the others (and
    // anything while a debugger or profiler is attached) go through the switch.
    static void* dispatchTable[256];
    static std::atomic<bool> tableReady{false};
    if (!tableReady.load(std::memory_order_acquire)) {
        for (auto& p : dispatchTable) p = &&dispatch_switch;
        dispatchTable[OP_CONST] = &&L_OP_CONST;
        dispatchTable[OP_NIL] = &&L_OP_NIL;
        dispatchTable[OP_TRUE] = &&L_OP_TRUE;
        dispatchTable[OP_FALSE] = &&L_OP_FALSE;
        dispatchTable[OP_POP] = &&L_OP_POP;
        dispatchTable[OP_DUP] = &&L_OP_DUP;
        dispatchTable[OP_GET_LOCAL] = &&L_OP_GET_LOCAL;
        dispatchTable[OP_GET_LOCAL_CHK] = &&L_OP_GET_LOCAL_CHK;
        dispatchTable[OP_SET_LOCAL] = &&L_OP_SET_LOCAL;
        dispatchTable[OP_GET_UPVAL] = &&L_OP_GET_UPVAL;
        dispatchTable[OP_GET_GLOBAL] = &&L_OP_GET_GLOBAL;
        dispatchTable[OP_SET_GLOBAL] = &&L_OP_SET_GLOBAL;
        dispatchTable[OP_GET_BUILTIN] = &&L_OP_GET_BUILTIN;
        dispatchTable[OP_ADD] = &&L_OP_ADD;
        dispatchTable[OP_SUB] = &&L_OP_SUB;
        dispatchTable[OP_LT] = &&L_OP_LT;
        dispatchTable[OP_MOD] = &&L_OP_MOD;
        dispatchTable[OP_DIV] = &&L_OP_DIV;
        dispatchTable[OP_INC_LOCAL] = &&L_OP_INC_LOCAL;
        dispatchTable[OP_JUMP] = &&L_OP_JUMP;
        dispatchTable[OP_JUMP_IF_FALSE] = &&L_OP_JUMP_IF_FALSE;
        dispatchTable[OP_LOOP] = &&L_OP_LOOP;
        dispatchTable[OP_GET_FIELD] = &&L_OP_GET_FIELD;
        dispatchTable[OP_SET_FIELD] = &&L_OP_SET_FIELD;
        dispatchTable[OP_ENDSCOPE] = &&L_OP_ENDSCOPE;
        dispatchTable[OP_JUMP_IF_NOT_CMP] = &&L_OP_JUMP_IF_NOT_CMP;
        dispatchTable[OP_FOR_NEXT] = &&L_OP_FOR_NEXT;
        dispatchTable[OP_FOR_RANGE] = &&L_OP_FOR_RANGE;
        dispatchTable[OP_FOR_SEQ] = &&L_OP_FOR_SEQ;
        dispatchTable[OP_CALL] = &&L_OP_CALL;
        dispatchTable[OP_MUL] = &&L_OP_MUL;
        dispatchTable[OP_LE] = &&L_OP_LE;
        dispatchTable[OP_GT] = &&L_OP_GT;
        dispatchTable[OP_GE] = &&L_OP_GE;
        dispatchTable[OP_EQ] = &&L_OP_EQ;
        dispatchTable[OP_NE] = &&L_OP_NE;
        tableReady.store(true, std::memory_order_release);
    }
#endif
    uint8_t op;
    for (;;) {
#ifdef IXO_CGOTO
    loop_top:
#endif
        if (debugger) {
            SAVE();
            if (ip == stepStop && f == stepFiber && f->frames.size() == stepFrames) return;  // jitStep: done
            debugger->check(*this, f);
        }
        op = READ_BYTE();
#ifdef IXO_CGOTO
    dispatch_switch:
#endif
        switch (op) {
            TARGET(OP_CONST) PUSH(consts[READ_U16()]); DISPATCH();
            TARGET(OP_NIL) PUSH(Value()); DISPATCH();
            TARGET(OP_TRUE) PUSH(Value::boolean(true)); DISPATCH();
            TARGET(OP_FALSE) PUSH(Value::boolean(false)); DISPATCH();
            case OP_UNDEF: PUSH(Value::undef()); break;
            TARGET(OP_POP) f->sp--; DISPATCH();
            TARGET(OP_DUP) {
                Value v = TOP;
                PUSH(v);
                DISPATCH();
            }
            case OP_DUP2: {
                Value a = STK[f->sp - 2], b = STK[f->sp - 1];
                PUSH(a);
                PUSH(b);
                break;
            }
            case OP_SWAP: std::swap(STK[f->sp - 1], STK[f->sp - 2]); break;
            case OP_ROT3: {
                Value a = STK[f->sp - 3];
                STK[f->sp - 3] = STK[f->sp - 2];
                STK[f->sp - 2] = STK[f->sp - 1];
                STK[f->sp - 1] = a;
                break;
            }
            TARGET(OP_GET_LOCAL) {
                uint16_t s = READ_U16();
                Value v = STK[base + s];
                PUSH(v);
                DISPATCH();
            }
            TARGET(OP_GET_LOCAL_CHK) {
                uint16_t s = READ_U16();
                uint16_t nm = READ_U16();
                Value v = STK[base + s];
                if (v.isUndef()) {
                    SAVE();
                    panic("'" + as<ObjString>(consts[nm])->s + "' is used before it has a value");
                }
                PUSH(v);
                DISPATCH();
            }
            TARGET(OP_SET_LOCAL) {
                uint16_t s = READ_U16();
                STK[base + s] = POP();
                DISPATCH();
            }
            TARGET(OP_GET_UPVAL) {
                uint16_t i = READ_U16();
                uint16_t nm = READ_U16();
                ObjUpvalue* u = frame->closure->upvalues[i];
                Value v = u->fiber ? u->fiber->stack[u->slot] : u->closed;
                if (v.isUndef()) {
                    SAVE();
                    panic("'" + as<ObjString>(consts[nm])->s + "' is used before it has a value");
                }
                PUSH(v);
                PROFILE();
                DISPATCH();
            }
            case OP_SET_UPVAL: {
                uint16_t i = READ_U16();
                ObjUpvalue* u = frame->closure->upvalues[i];
                Value v = POP();
                if (u->fiber)
                    u->fiber->stack[u->slot] = v;
                else
                    u->closed = v;
                break;
            }
            TARGET(OP_GET_GLOBAL) {
                uint16_t i = READ_U16();
                ModuleRt* m = frame->module;
                Value v = i < m->globals.size() ? m->globals[i] : Value::undef();
                if (v.t == VT::Obj) {
                    sealed(v);
                } else if (v.isUndef()) {
                    SAVE();
                    std::string name = i < m->globalNames.size() ? m->globalNames[i] : "a module variable";
                    panic("'" + name + "' is used before it has a value");
                }
                PUSH(v);
                PROFILE();
                DISPATCH();
            }
            TARGET(OP_SET_GLOBAL) {
                uint16_t i = READ_U16();
                ModuleRt* m = frame->module;
                if (i >= m->globals.size()) m->globals.resize(i + 1, Value::undef());
                m->globals[i] = POP();
                DISPATCH();
            }
            TARGET(OP_GET_BUILTIN) PUSH(builtins[READ_U16()].value); DISPATCH();
            TARGET(OP_ADD) {
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                if (a.t == VT::Int && b.t == VT::Int) {
                    int64_t r;
                    if (__builtin_add_overflow(a.i, b.i, &r) && checkOverflow) {
                        SAVE();
                        intOverflow('+', a.i, b.i);
                    }
                    STK[f->sp - 2] = Value::integer(r);
                } else if (a.isNum() && b.isNum()) {
                    STK[f->sp - 2] = Value::number(a.num() + b.num());
                } else {
                    SAVE();
                    STK[f->sp - 2] = binaryOp(op, a, b);
                }
                f->sp--;
                DISPATCH();
            }
            TARGET(OP_SUB) {
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                if (a.t == VT::Int && b.t == VT::Int) {
                    int64_t r;
                    if (__builtin_sub_overflow(a.i, b.i, &r) && checkOverflow) {
                        SAVE();
                        intOverflow('-', a.i, b.i);
                    }
                    STK[f->sp - 2] = Value::integer(r);
                } else if (a.isNum() && b.isNum()) {
                    STK[f->sp - 2] = Value::number(a.num() - b.num());
                } else {
                    SAVE();
                    STK[f->sp - 2] = binaryOp(op, a, b);
                }
                f->sp--;
                DISPATCH();
            }
            TARGET(OP_LT) {
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                if (a.t == VT::Int && b.t == VT::Int)
                    STK[f->sp - 2] = Value::boolean(a.i < b.i);
                else if (a.isNum() && b.isNum())
                    STK[f->sp - 2] = Value::boolean(a.num() < b.num());
                else {
                    SAVE();
                    STK[f->sp - 2] = binaryOp(op, a, b);
                }
                f->sp--;
                DISPATCH();
            }
#define NUM_BINOP(OPC, INT_EXPR, FLT_EXPR)                                        \
    TARGET(OPC) {                                                                \
        Value b = STK[f->sp - 1], a = STK[f->sp - 2];                            \
        if (a.t == VT::Int && b.t == VT::Int) {                                  \
            STK[f->sp - 2] = INT_EXPR;                                           \
        } else if (a.isNum() && b.isNum()) {                                     \
            double x = a.num(), y = b.num();                                     \
            STK[f->sp - 2] = FLT_EXPR;                                           \
        } else {                                                                 \
            SAVE();                                                              \
            STK[f->sp - 2] = binaryOp(op, a, b);                                 \
        }                                                                        \
        f->sp--;                                                                 \
        DISPATCH();                                                              \
    }
            NUM_BINOP(OP_MUL, (SAVE(), Value::integer(checkedMul(a.i, b.i))), Value::number(x * y))
            NUM_BINOP(OP_LE, Value::boolean(a.i <= b.i), Value::boolean(x <= y))
            NUM_BINOP(OP_GT, Value::boolean(a.i > b.i), Value::boolean(x > y))
            NUM_BINOP(OP_GE, Value::boolean(a.i >= b.i), Value::boolean(x >= y))
            NUM_BINOP(OP_EQ, Value::boolean(a.i == b.i), Value::boolean(x == y))
            NUM_BINOP(OP_NE, Value::boolean(a.i != b.i), Value::boolean(x != y))
#undef NUM_BINOP
            TARGET(OP_MOD) {
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                if (a.t == VT::Int && b.t == VT::Int && b.i != 0) {
                    int64_t m = a.i % b.i;
                    if (m != 0 && ((m < 0) != (b.i < 0))) m += b.i;
                    STK[f->sp - 2] = Value::integer(m);
                } else {
                    SAVE();
                    STK[f->sp - 2] = binaryOp(op, a, b);
                }
                f->sp--;
                DISPATCH();
            }
            TARGET(OP_DIV) {
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                if (a.isNum() && b.isNum() && !(b.t == VT::Int && b.i == 0 && a.t == VT::Int)) {
                    STK[f->sp - 2] = Value::number(a.num() / b.num());
                } else {
                    SAVE();
                    STK[f->sp - 2] = binaryOp(op, a, b);
                }
                f->sp--;
                DISPATCH();
            }
            TARGET(OP_INC_LOCAL) {
                uint16_t s = READ_U16();
                int8_t delta = (int8_t)READ_BYTE();
                Value& v = STK[base + s];
                if (v.t == VT::Int) {
                    if (__builtin_add_overflow(v.i, (int64_t)delta, &v.i) && checkOverflow) {
                        SAVE();
                        intOverflow('+', (int64_t)((uint64_t)v.i - (uint64_t)(int64_t)delta), delta);
                    }
                } else if (v.t == VT::Float) {
                    v.f += delta;
                } else {
                    SAVE();
                    if (v.isUndef()) panic("a variable is used before it has a value");
                    Value r = binaryOp(OP_ADD, v, Value::integer(delta));
                    STK[base + s] = r;
                }
                DISPATCH();
            }
            TARGET(OP_JUMP_IF_NOT_CMP) {
                uint8_t cmp = READ_BYTE();
                uint32_t off = READ_U32();
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                bool r;
                if (a.t == VT::Int && b.t == VT::Int) {
                    switch (cmp) {
                        case 0: r = a.i < b.i; break;
                        case 1: r = a.i <= b.i; break;
                        case 2: r = a.i > b.i; break;
                        case 3: r = a.i >= b.i; break;
                        case 4: r = a.i == b.i; break;
                        default: r = a.i != b.i; break;
                    }
                } else if (a.isNum() && b.isNum()) {
                    double x = a.num(), y = b.num();
                    switch (cmp) {
                        case 0: r = x < y; break;
                        case 1: r = x <= y; break;
                        case 2: r = x > y; break;
                        case 3: r = x >= y; break;
                        case 4: r = x == y; break;
                        default: r = x != y; break;
                    }
                } else {
                    SAVE();
                    static const Op ops[] = {OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE};
                    r = truthy(binaryOp(ops[cmp < 6 ? cmp : 5], a, b));
                }
                f->sp -= 2;
                if (!r) ip += off;
                DISPATCH();
            }
            TARGET(OP_FOR_NEXT) {
                const uint8_t* opStart = ip - 1;
                uint16_t s = READ_U16();
                uint32_t off = READ_U32();
                auto* it = as<ObjIterator>(STK[base + s]);
                Value out;
                if (it->pending) {
                    // a flow delivered this while we waited
                    it->pending = false;
                    out = POP();
                } else {
                    Obj* src = it->src.o;
                    if (src->type == OT::Range) {
                        auto* r = static_cast<ObjRange*>(src);
                        bool more = r->step > 0 ? (r->inclusive ? it->cur <= r->end : it->cur < r->end)
                                                : (r->inclusive ? it->cur >= r->end : it->cur > r->end);
                        if (!more) {
                            ip += off;
                            DISPATCH();
                        }
                        PUSH(Value::integer(it->cur));
                        it->cur += r->step;
                        DISPATCH();
                    }
                    if (src->type == OT::List && !it->pairs) {
                        auto* l = static_cast<ObjList*>(src);
                        if (it->idx >= l->len()) {
                            ip += off;
                            DISPATCH();
                        }
                        PUSH(l->get(it->idx++));
                        DISPATCH();
                    }
                    SAVE();
                    if (!iterNext(f, it, out)) {
                        // waiting on a flow: run this instruction again once the value is pushed
                        it->pending = true;
                        frame->ip = opStart;
                        SUSPEND();
                    }
                }
                if (out.isDone()) {
                    ip += off;
                    DISPATCH();
                }
                PUSH(out);
                DISPATCH();
            }
            case OP_SELECT: {
                uint8_t n = READ_BYTE();
                uint8_t flags = READ_BYTE();
                SAVE();
                double timeout = -1;
                if (flags & 1) {
                    Value t = POP();
                    if (!t.isNum()) panic("'after' takes a number of milliseconds, got a " + typeName(t));
                    timeout = std::max(0.0, t.num());
                }
                std::vector<ObjChannel*> chans;
                for (size_t k = f->sp - n; k < f->sp; k++) {
                    if (!isObjType(STK[k], OT::Channel)) panic("'<:' receives from a flow, got a " + typeName(STK[k]));
                    chans.push_back(as<ObjChannel>(STK[k]));
                }
                f->sp -= n;
                bool blocked = false;
                Value r = selectRecv(chans, timeout, flags & 2, &blocked);
                if (blocked) {
                    SUSPEND();
                }
                PUSH(r);
                break;
            }
            case OP_TEST_KEY: {
                uint16_t k = READ_U16();
                Value v = TOP;
                TOP = Value::boolean(isObjType(v, OT::Map) && (k == 0xFFFF || as<ObjMap>(v)->find(consts[k]) != nullptr));
                break;
            }
            case OP_KEY: {
                uint16_t k = READ_U16();
                Value* x = as<ObjMap>(TOP)->find(consts[k]);
                TOP = x ? *x : Value();
                break;
            }
            case OP_IDIV: {
                SAVE();
                Value b = POP();
                Value a = TOP;
                TOP = binaryOp(OP_IDIV, a, b);
                break;
            }
            case OP_POW: case OP_ROOT:
            case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
            case OP_IS: case OP_ISNOT: case OP_IN: case OP_NOTIN: {
                SAVE();
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                STK[f->sp - 2] = binaryOp(op, a, b);
                f->sp--;
                break;
            }
            case OP_NEG: {
                Value& v = TOP;
                if (v.t == VT::Int)
                    v.i = (int64_t)(0ULL - (uint64_t)v.i);
                else if (v.t == VT::Float)
                    v.f = -v.f;
                else {
                    SAVE();
                    v = binaryOp(OP_NEG, v, Value());
                }
                break;
            }
            case OP_NOT: TOP = Value::boolean(!truthy(TOP)); break;
            case OP_BNOT: {
                Value& v = TOP;
                if (v.t != VT::Int) {
                    SAVE();
                    panic("'~' needs an int, got " + typeName(v));
                }
                v.i = ~v.i;
                break;
            }
            TARGET(OP_JUMP) {
                uint32_t off = READ_U32();
                ip += off;
                DISPATCH();
            }
            TARGET(OP_JUMP_IF_FALSE) {
                uint32_t off = READ_U32();
                if (!truthy(POP())) ip += off;
                DISPATCH();
            }
            case OP_JUMP_IF_FALSE_KEEP: {
                uint32_t off = READ_U32();
                if (!truthy(TOP))
                    ip += off;
                else
                    f->sp--;
                break;
            }
            case OP_JUMP_IF_TRUE_KEEP: {
                uint32_t off = READ_U32();
                if (truthy(TOP))
                    ip += off;
                else
                    f->sp--;
                break;
            }
            case OP_JUMP_IF_NOT_ERROR: {
                uint32_t off = READ_U32();
                if (!isObjType(TOP, OT::Error)) ip += off;
                break;
            }
            case OP_JUMP_IF_DONE: {
                uint32_t off = READ_U32();
                if (TOP.isDone()) {
                    f->sp--;
                    ip += off;
                }
                break;
            }
            case OP_JUMP_IF_SET: {
                uint16_t s = READ_U16();
                uint32_t off = READ_U32();
                if (!STK[base + s].isUndef()) ip += off;
                break;
            }
            TARGET(OP_LOOP) {
                uint32_t off = READ_U32();
                ip -= off;
                if (jitOn) {
                    // a hot loop: continue it in machine code (on-stack replacement)
                    ObjFunction* lf = frame->closure->fn;
                    if (!prof && lf->jitState == 0 && lf->heapId == heapId) {  // start its type profile
                        lf->typeSeen.assign(lf->chunk.code.size() + (size_t)lf->arity + 1, 0);
                        prof = lf->typeSeen.data();
                    }
                    if (lf->jitState != 3 &&
                        (lf->jitState >= 2 || lf->wantJit || (warm && !lf->warmChecked && (warmApply(lf), false)) || ++lf->hotness >= kJitTurns) &&
                        jitReady(f, frame->closure, (int64_t)(ip - lf->chunk.code.data()), (int)(f->sp - base))) {
                        uint32_t at = (uint32_t)(ip - lf->chunk.code.data());
                        auto& heads = lf->jit->loopHeaders;
                        auto h = std::find(heads.begin(), heads.end(), at);
                        if (h != heads.end() && !(lf->osrBails > 100 && lf->osrBails * 2 > lf->osrEntries)) {
                            SAVE();
                            JitCtx ctx{};
                            lf->osrEntries++;
                            if (runJit(f, lf, base, (uint64_t)(h - heads.begin()) + 1, ctx)) {
                                // it returned: finish this call like RETURN
                                Value result = STK[base];
                                closeUpvalues(f, base);
                                f->frames.pop_back();
                                f->sp = base;
                                PUSH(result);
                                if (f->frames.size() == stopDepth) return;
                                LOAD();
                                DISPATCH();
                            }
                            if (!ctx.budgetExit && !ctx.pausedExit && ++lf->osrBails == 101 && jitStats && lf->osrBails * 2 > lf->osrEntries)
                                noteJitGiveUp(lf, "loops");  // a turn for other tasks isn't a failure
                            resumeAfterJit(f, ctx);
                            LOAD();
                            SUSPEND();  // a helper paused the task (it waits, or others get a turn)
                            if (ctx.budgetExit) {
                                instructionBudget = 20000;  // let other tasks run now
                                SAFEPOINT();
                            }
                            DISPATCH();
                        }
                    }
                }
                SAFEPOINT();
                DISPATCH();
            }
            case OP_CALL_NAMED: {
                int argc = READ_BYTE();
                int nn = READ_BYTE();
                std::vector<std::string> names;
                for (int k = 0; k < nn; k++) names.push_back(as<ObjString>(consts[READ_U16()])->s);
                SAVE();
                argc = placeNamedArgs(f, argc, names);
                Value callee = STK[f->sp - argc - 1];
                bool pushed = callValue(f, callee, argc);
                SUSPEND();
                if (pushed) LOAD();
                SAFEPOINT();
                break;
            }
            TARGET(OP_CALL) {
                int argc = READ_BYTE();
                SAVE();
                Value callee = STK[f->sp - argc - 1];
                bool pushed = callValue(f, callee, argc);
                SUSPEND();
                if (pushed) LOAD();
                else PROFILE();
                SAFEPOINT();
                DISPATCH();
            }
            case OP_AUTOCALL: {
                Value v = TOP;
                if (v.isObj() && (v.o->type == OT::Closure || v.o->type == OT::Native || v.o->type == OT::Bound) &&
                    minArityOf(v) == 0) {
                    SAVE();
                    bool pushed = callValue(f, v, 0);
                    SUSPEND();
                    if (pushed) LOAD();
                }
                break;
            }
            case OP_INVOKE: {
                uint16_t s = READ_U16();
                int argc = READ_BYTE();
                uint8_t fk = READ_BYTE();
                uint16_t fi = READ_U16();
                uint16_t icx = READ_U16();
                {
                    Value r = STK[f->sp - argc - 1];
                    ObjFunction* cf = frame->closure->fn;
                    if (isObjType(r, OT::Instance) && cf->heapId == heapId) {
                        auto* inst = as<ObjInstance>(r);
                        InlineCache& c = cf->ic[icx];
                        if (c.clsId == inst->cls->id && c.index < 0) {
                            SAVE();
                            callClosure(f, as<ObjClosure>(c.method), argc, f->sp - argc - 1);
                            LOAD();
                            SAFEPOINT();
                            DISPATCH();
                        }
                        if (!inst->cls->fieldIndex.count(s)) {
                            auto mi = inst->cls->methods.find(s);
                            if (mi != inst->cls->methods.end() && isObjType(mi->second, OT::Closure)) {
                                SAVE();
                                checkPrivate(*this, inst->cls, s, frame->closure);
                                c.clsId = inst->cls->id;
                                c.index = -1;
                                c.method = mi->second;
                                callClosure(f, as<ObjClosure>(mi->second), argc, f->sp - argc - 1);
                                LOAD();
                                SAFEPOINT();
                                DISPATCH();
                            }
                        }
                    }
                }
                SAVE();
                bool pushed = invoke(f, s, argc, fk, fi, frame->closure);
                SUSPEND();
                if (pushed) LOAD();
                else PROFILE();
                SAFEPOINT();
                break;
            }
            case OP_GET_MEMBER:
            case OP_GET_MEMBER_RAW: {
                uint16_t s = READ_U16();
                uint8_t fk = READ_BYTE();
                uint16_t fi = READ_U16();
                uint16_t icx = READ_U16();
                if (isObjType(TOP, OT::Instance) && frame->closure->fn->heapId == heapId) {
                    auto* inst = as<ObjInstance>(TOP);
                    InlineCache& c = frame->closure->fn->ic[icx];
                    if (c.clsId == inst->cls->id && c.index >= 0) {
                        TOP = inst->fields[(size_t)c.index];
                        PROFILE();
                        DISPATCH();
                    }
                    if (op == OP_GET_MEMBER && c.clsId == inst->cls->id && c.index < 0 && isObjType(c.method, OT::Closure)) {
                        // obj.alive: a cached method without parameters
                        SAVE();
                        callClosure(f, as<ObjClosure>(c.method), 0, f->sp - 1);
                        LOAD();
                        SAFEPOINT();
                        DISPATCH();
                    }
                    auto it = inst->cls->fieldIndex.find(s);
                    if (it != inst->cls->fieldIndex.end()) {
                        c.clsId = inst->cls->id;
                        c.index = it->second;
                        TOP = inst->fields[(size_t)it->second];
                        PROFILE();
                        DISPATCH();
                    }
                    if (op == OP_GET_MEMBER) {
                        auto mi = inst->cls->methods.find(s);
                        if (mi != inst->cls->methods.end() && isObjType(mi->second, OT::Closure) &&
                            as<ObjClosure>(mi->second)->fn->autoCall) {
                            SAVE();
                            checkPrivate(*this, inst->cls, s, frame->closure);
                            c.clsId = inst->cls->id;
                            c.index = -1;
                            c.method = mi->second;
                            callClosure(f, as<ObjClosure>(mi->second), 0, f->sp - 1);
                            LOAD();
                            SAFEPOINT();
                            DISPATCH();
                        }
                    }
                }
                SAVE();
                size_t depthBefore = f->frames.size();
                getMember(f, s, fk, fi, frame->closure, op == OP_GET_MEMBER_RAW);
                SUSPEND();
                if (f->frames.size() != depthBefore) LOAD();
                else PROFILE();
                break;
            }
            case OP_SET_MEMBER: {
                uint16_t s = READ_U16();
                uint16_t icx = READ_U16();
                Value v = POP();
                Value obj = POP();
                if (isObjType(obj, OT::Instance)) {
                    auto* inst = as<ObjInstance>(obj);
                    bool own = frame->closure->fn->heapId == heapId;
                    if (own) {
                        InlineCache& c = frame->closure->fn->ic[icx];
                        if (c.clsId == inst->cls->id && c.index >= 0) {
                            inst->fields[(size_t)c.index] = v;
                            DISPATCH();
                        }
                    }
                    auto it = inst->cls->fieldIndex.find(s);
                    if (it != inst->cls->fieldIndex.end() && inst->cls->isData) {
                        SAVE();
                        panic("can't change '" + symName(s) + "' of " + inst->cls->name +
                              ": data records don't change; make a changed copy with .with(" + symName(s) + ": ...)");
                    }
                    if (it != inst->cls->fieldIndex.end()) {
                        if (own) {
                            InlineCache& c = frame->closure->fn->ic[icx];
                            c.clsId = inst->cls->id;
                            c.index = it->second;
                        }
                        inst->fields[it->second] = v;
                        break;
                    }
                    SAVE();
                    std::string fields;
                    for (auto& fname : inst->cls->fields) fields += (fields.empty() ? "" : ", ") + fname;
                    panic(inst->cls->name + " has no field '" + symName(s) + "'" +
                          (fields.empty() ? "" : " (fields: " + fields + ")") +
                          "; declare it in the class body (or set self." + symName(s) + " in init)");
                }
                SAVE();
                panic("can't set '" + symName(s) + "' on a " + typeName(obj));
            }
            TARGET(OP_GET_FIELD) {
                uint16_t i = READ_U16();
                Value obj = TOP;
                if (!isObjType(obj, OT::Instance)) {
                    SAVE();
                    panic("internal: field access on " + typeName(obj));
                }
                TOP = as<ObjInstance>(obj)->fields[i];
                PROFILE();
                DISPATCH();
            }
            TARGET(OP_SET_FIELD) {
                uint16_t i = READ_U16();
                Value v = POP();
                Value obj = POP();
                as<ObjInstance>(obj)->fields[i] = v;
                DISPATCH();
            }
            case OP_GET_INDEX: {
                SAVE();
                Value idx = STK[f->sp - 1], obj = STK[f->sp - 2];
                STK[f->sp - 2] = getIndex(obj, idx);
                f->sp--;
                PROFILE();
                if (prof && !isObjType(obj, OT::List)) prof[ip - codeBase] |= PB_NOTLIST;
                break;
            }
            case OP_SET_INDEX: {
                SAVE();
                Value v = STK[f->sp - 1], idx = STK[f->sp - 2], obj = STK[f->sp - 3];
                setIndex(obj, idx, v);
                f->sp -= 3;
                if (prof && !isObjType(obj, OT::List)) prof[ip - codeBase] |= PB_NOTLIST;
                break;
            }
            case OP_CLOSURE: {
                auto* fn = as<ObjFunction>(consts[READ_U16()]);
                auto* c = alloc<ObjClosure>(fn);
                for (int i = 0; i < fn->upvalueCount; i++) {
                    uint8_t isLocal = READ_BYTE();
                    uint16_t index = READ_U16();
                    c->upvalues[i] = isLocal ? captureUpvalue(f, base + index) : frame->closure->upvalues[index];
                }
                PUSH(Value::object(c));
                break;
            }
            TARGET(OP_ENDSCOPE) {
                uint16_t n = READ_U16();
                closeUpvalues(f, f->sp - n);
                f->sp -= n;
                DISPATCH();
            }
            case OP_ENDSCOPE_KEEP: {
                uint16_t n = READ_U16();
                Value top = POP();
                closeUpvalues(f, f->sp - n);
                f->sp -= n;
                PUSH(top);
                break;
            }
            case OP_RETURN: {
                Value result = POP();
                closeUpvalues(f, base);
                f->frames.pop_back();
                f->sp = base;
                PUSH(result);
                if (f->frames.size() == stopDepth) return;
                LOAD();
                PROFILE();
                break;
            }
            case OP_LIST: {
                uint16_t n = READ_U16();
                std::vector<Value> items(STK + f->sp - n, STK + f->sp);
                f->sp -= n;
                PUSH(newList(std::move(items)));
                break;
            }
            case OP_TUPLE: {
                uint16_t n = READ_U16();
                std::vector<Value> items(STK + f->sp - n, STK + f->sp);
                f->sp -= n;
                PUSH(newTuple(std::move(items)));
                break;
            }
            case OP_MAP: {
                uint16_t n = READ_U16();
                SAVE();
                auto* m = alloc<ObjMap>();
                m->entries.reserve(n);
                for (size_t i = f->sp - 2 * n; i < f->sp; i += 2) {
                    requireHashable(*this, STK[i]);
                    m->set(STK[i], STK[i + 1]);
                }
                f->sp -= 2 * n;
                PUSH(Value::object(m));
                break;
            }
            case OP_SET: {
                uint16_t n = READ_U16();
                SAVE();
                auto* s = alloc<ObjSet>();
                s->items.reserve(n);
                for (size_t i = f->sp - n; i < f->sp; i++) {
                    requireHashable(*this, STK[i]);
                    s->add(STK[i]);
                }
                f->sp -= n;
                PUSH(Value::object(s));
                break;
            }
            case OP_RANGE: {
                uint8_t flags = READ_BYTE();
                SAVE();
                Value step = (flags & 2) ? POP() : Value::integer(1);
                Value end = POP();
                Value start = POP();
                bool openStart = start.isNil(), openEnd = end.isNil();
                Value r = makeRange(*this, openStart ? Value::integer(0) : start, openEnd ? Value::integer(0) : end, step,
                                    flags & 1);
                as<ObjRange>(r)->openStart = openStart;
                as<ObjRange>(r)->openEnd = openEnd;
                PUSH(r);
                break;
            }
            case OP_ITER_INIT: {
                uint8_t mode = READ_BYTE();
                SAVE();
                TOP = makeIterator(TOP, mode == 1, mode == 2);
                break;
            }
            case OP_JUMP_IF_NOT_SEQ: {
                // inlined xs.map(...) & co: only for the built-in sequences
                uint32_t off = READ_U32();
                Value v = TOP;
                bool seq = false;
                if (v.isObj()) {
                    switch (v.o->type) {
                        case OT::List: case OT::Tuple: case OT::String: case OT::Set: seq = true; break;
                        case OT::Range: seq = !as<ObjRange>(v)->openStart && !as<ObjRange>(v)->openEnd; break;
                        default: break;
                    }
                }
                if (!seq) ip += off;
                break;
            }
            case OP_LIST_APPEND: {
                uint16_t s = READ_U16();
                Value v = POP();
                as<ObjList>(STK[base + s])->push(v);
                break;
            }
            case OP_STR_APPEND: {
                uint16_t s = READ_U16();
                uint8_t form = READ_BYTE();
                Value x = TOP;
                Value& cur = STK[base + s];
                if (cur.t == VT::Int && x.t == VT::Int) {
                    cur.i = (int64_t)((uint64_t)cur.i + (uint64_t)x.i);
                } else {
                    SAVE();
                    strAppend(cur, x, form != 0, 1);
                }
                f->sp--;
                break;
            }
            case OP_STR_APPEND_G: {
                uint16_t g = READ_U16();
                uint8_t form = READ_BYTE();
                ModuleRt* m = frame->module;
                Value x = TOP;
                if (g >= m->globals.size() || m->globals[g].isUndef()) {
                    SAVE();
                    std::string name = g < m->globalNames.size() ? m->globalNames[g] : "a module variable";
                    panic("'" + name + "' is used before it has a value");
                }
                Value& cur = m->globals[g];
                if (cur.t == VT::Int && x.t == VT::Int) {
                    cur.i = (int64_t)((uint64_t)cur.i + (uint64_t)x.i);
                } else {
                    SAVE();
                    strAppend(cur, x, form != 0, 2);
                }
                f->sp--;
                break;
            }
            case OP_STR_SEAL: {
                uint16_t s = READ_U16();
                if (isObjType(STK[base + s], OT::String)) seal(as<ObjString>(STK[base + s]));
                break;
            }
            case OP_FSTR_BEGIN: PUSH(vstr(std::string())); break;
            case OP_FSTR_ADD: {
                uint16_t spec = READ_U16();
                SAVE();
                fstrAdd(STK[f->sp - 2], TOP, spec, consts);
                f->sp--;
                break;
            }
            case OP_FSTR_LIT: {
                uint16_t k = READ_U16();
                auto* b = as<ObjString>(TOP);
                const std::string& lit = as<ObjString>(consts[k])->s;
                b->s += lit;
                b->kind = 0;
                grow(b, lit.size());
                break;
            }
            case OP_SEQ_INIT: {
                if (isObjType(TOP, OT::List)) {
                    PUSH(Value::integer(0));
                } else {
                    SAVE();
                    TOP = makeIterator(TOP, false, false);
                    PUSH(Value());
                }
                break;
            }
            TARGET(OP_FOR_SEQ) {
                const uint8_t* opStart = ip - 1;
                uint16_t s = READ_U16();
                uint32_t off = READ_U32();
                Value* k = &STK[base + s + 1];
                if (k->t == VT::Int) {  // a list: the position is in the slot after it
                    auto* l = as<ObjList>(STK[base + s]);
                    if ((size_t)k->i >= l->len()) {
                        ip += off;
                        DISPATCH();
                    }
                    Value v = l->get((size_t)k->i++);
                    PUSH(v);
                    PROFILE();
                    DISPATCH();
                }
                SAVE();
                Value out;
                int r = iterStep(f, as<ObjIterator>(STK[base + s]), out);
                if (r == 2) {  // waiting on a flow: run this instruction again once the value is pushed
                    frame->ip = opStart;
                    SUSPEND();
                }
                if (r == 1) {
                    ip += off;
                    DISPATCH();
                }
                PUSH(out);
                DISPATCH();
            }
            case OP_RANGE_INIT: {
                Value* v = &STK[f->sp - 3];
                if (v[0].t != VT::Int || v[1].t != VT::Int || v[2].t != VT::Int || v[2].i == 0) {
                    SAVE();
                    rangeLoopInit(*this, v);
                }
                break;
            }
            TARGET(OP_FOR_RANGE) {
                uint16_t s = READ_U16();
                uint8_t fl = READ_BYTE();
                uint32_t off = READ_U32();
                Value* v = &STK[base + s];
                int64_t cur = v[0].i, end = v[1].i, step = v[2].i;
                bool more = step > 0 ? ((fl & 1) ? cur <= end : cur < end) : ((fl & 1) ? cur >= end : cur > end);
                if (!more) {
                    ip += off;
                    DISPATCH();
                }
                PUSH(Value::integer(cur));
                v[0].i = (int64_t)((uint64_t)cur + (uint64_t)step);
                DISPATCH();
            }
            case OP_INPLACE: {
                // a += b: numbers add as usual; an object whose class defines `+=` changes itself
                uint8_t bop = READ_BYTE();
                Value b = STK[f->sp - 1], a = STK[f->sp - 2];
                if (bop == OP_ADD && a.t == VT::Int && b.t == VT::Int) {
                    int64_t r;
                    if (__builtin_add_overflow(a.i, b.i, &r) && checkOverflow) {
                        SAVE();
                        intOverflow('+', a.i, b.i);
                    }
                    STK[f->sp - 2] = Value::integer(r);
                    f->sp--;
                    break;
                }
                SAVE();
                const char* name = nullptr;
                switch (bop) {
                    case OP_ADD: name = "+="; break;
                    case OP_SUB: name = "-="; break;
                    case OP_MUL: name = "*="; break;
                    case OP_DIV: name = "/="; break;
                    case OP_MOD: name = "%="; break;
                    default: break;
                }
                Value out;
                if (name && isObjType(a, OT::Instance) && callOperator(a, name, &b, 1, out)) {
                    f->sp--;  // a stays: it changed itself
                    break;
                }
                STK[f->sp - 2] = binaryOp(bop, a, b);
                f->sp--;
                break;
            }
            case OP_ITER_KEY: {
                // the position of the item FOR_NEXT just gave (or its map key)
                uint16_t s = READ_U16();
                auto* it = as<ObjIterator>(STK[base + s]);
                Obj* src = it->src.o;
                Value k;
                switch (src->type) {
                    case OT::List:
                        if (isObjType(TOP, OT::Tuple) && as<ObjTuple>(TOP)->items.size() == 2) {
                            ObjFunction* lf = frame->closure->fn;
                            size_t off = (size_t)(ip - lf->chunk.code.data());
                            if (pairLoopsTold.insert(((uint64_t)(uintptr_t)lf << 20) ^ off).second) {
                                flushOut();
                                std::fprintf(stderr,
                                             "%s:%d: warning: this loop's items are pairs; `for a, b in list` gives the "
                                             "position and the item since Ixo 0.6; write `for (a, b) in list` to unpack them\n",
                                             lf->file.c_str(), lf->chunk.lineAt(off > 0 ? off - 1 : 0));
                            }
                        }
                        k = Value::integer((int64_t)it->idx - 1);
                        break;
                    case OT::Tuple: k = Value::integer((int64_t)it->idx - 1); break;
                    case OT::String: k = Value::integer(it->cur - 1); break;
                    case OT::Range: {
                        auto* r = static_cast<ObjRange*>(src);
                        k = Value::integer((it->cur - r->start) / r->step - 1);
                        break;
                    }
                    case OT::Map: k = it->key; break;
                    default: break;
                }
                PUSH(k);
                break;
            }
            case OP_ITER_NEXT: {
                uint16_t s = READ_U16();
                SAVE();
                auto* it = as<ObjIterator>(STK[base + s]);
                Value out;
                if (!iterNext(f, it, out)) {
                    SUSPEND();
                }
                PUSH(out);
                break;
            }
            case OP_DESTRUCT: {
                uint8_t n = READ_BYTE();
                SAVE();
                Value v = POP();
                SeqView items;
                if (!seqOf(v, items)) panic("can't unpack a " + typeName(v) + " into " + std::to_string(n) + " variables");
                if (items.size() != n)
                    panic("expected " + std::to_string(n) + " values to unpack, got " + std::to_string(items.size()));
                std::vector<Value> copy = items.vec();
                for (auto& x : copy) PUSH(x);
                break;
            }
            case OP_CLASS: {
                auto* tmpl = as<ObjClass>(consts[READ_U16()]);
                auto* c = alloc<ObjClass>();
                c->name = tmpl->name;
                c->fields = tmpl->fields;
                c->fieldIndex = tmpl->fieldIndex;
                c->positional = tmpl->positional;
                c->module = tmpl->module;
                c->ctorArity = tmpl->ctorArity;
                c->ctorMinArity = tmpl->ctorMinArity;
                c->isData = tmpl->isData;
                classById[c->id] = c;
                PUSH(Value::object(c));
                break;
            }
            case OP_METHOD: {
                uint16_t s = READ_U16();
                uint8_t pub = READ_BYTE();
                Value m = POP();
                auto* c = as<ObjClass>(TOP);
                c->methods[s] = m;
                c->methodPublic[s] = pub != 0;
                break;
            }
            case OP_CTOR: {
                Value m = POP();
                as<ObjClass>(TOP)->ctor = as<ObjClosure>(m);
                break;
            }
            case OP_TEST_TYPE: {
                uint8_t tag = READ_BYTE();
                Value v = TOP;
                bool ok;
                if (tag == 0xFF)
                    ok = v.isNum();
                else if (tag == 0xFE)
                    ok = isObjType(v, OT::Error);
                else
                    ok = (int)tagOf(v) == tag;
                TOP = Value::boolean(ok);
                break;
            }
            case OP_INSTANCE_OF:
            case OP_MATCH_VALUE: {
                SAVE();
                Value pat = POP();
                Value v = TOP;
                TOP = Value::boolean(matchValue(v, pat));
                break;
            }
            case OP_MAKE_NS: {
                uint16_t nm = READ_U16();
                uint16_t cnt = READ_U16();
                auto* ns = alloc<ObjNamespace>(as<ObjString>(consts[nm])->s);
                size_t first = f->sp - cnt;
                for (uint16_t k = 0; k < cnt; k++) {
                    uint16_t mn = READ_U16();
                    ns->members[as<ObjString>(consts[mn])->s] = STK[first + k];
                }
                f->sp = first;
                PUSH(Value::object(ns));
                break;
            }
            case OP_TEST_LEN: {
                uint8_t cmp = READ_BYTE();
                uint8_t kind = READ_BYTE();
                uint16_t n = READ_U16();
                Value v = TOP;
                bool ok = false;
                SeqView items;
                bool right = (kind == 0 && isObjType(v, OT::List)) || (kind == 1 && isObjType(v, OT::Tuple));
                if (right && seqOf(v, items)) ok = cmp == 0 ? items.size() == n : items.size() >= n;
                TOP = Value::boolean(ok);
                break;
            }
            case OP_POS_FIELD: {
                uint16_t i = READ_U16();
                auto* inst = as<ObjInstance>(TOP);
                if (i >= inst->cls->positional.size()) {
                    SAVE();
                    panic(inst->cls->name + " has only " + std::to_string(inst->cls->positional.size()) +
                          " fields to match against");
                }
                TOP = inst->fields[inst->cls->positional[i]];
                break;
            }
            case OP_ELEM: {
                int16_t i = (int16_t)READ_U16();
                Value v = TOP;
                SeqView items;
                seqOf(v, items);
                TOP = items[i >= 0 ? (size_t)i : items.size() + i];
                break;
            }
            case OP_SLICE_FROM: {
                uint16_t start = READ_U16();
                uint16_t fromEnd = READ_U16();
                SeqView items(*as<ObjList>(TOP));
                std::vector<Value> out(items.begin() + start, items.end() - fromEnd);
                TOP = newList(std::move(out));
                break;
            }
            case OP_TRY: {
                if (!isObjType(TOP, OT::Error)) break;
                Value result = POP();
                closeUpvalues(f, base);
                f->frames.pop_back();
                f->sp = base;
                PUSH(result);
                if (f->frames.size() == stopDepth) return;
                LOAD();
                break;
            }
            case OP_FORK: {
                int argc = READ_BYTE();
                SAVE();
                Value callee = STK[f->sp - argc - 1];
                if (!isCallable(callee)) panic("fork needs a function call, got a " + typeName(callee));
                ObjTask* t = spawn(callee, &STK[f->sp - argc], argc);
                f->sp -= argc + 1;
                PUSH(Value::object(t));
                break;
            }
            case OP_PAR: {
                int argc = READ_BYTE();
                SAVE();
                Value callee = STK[f->sp - argc - 1];
                if (!isCallable(callee)) panic("par needs a function call, got a " + typeName(callee));
                ObjTask* t = parSpawn(callee, &STK[f->sp - argc], argc);
                f->sp -= argc + 1;
                PUSH(Value::object(t));
                break;
            }
            case OP_REGION_BEGIN: {
                regionsOpen++;  // allocations are logged from here (the marker first)
                auto* mk = alloc<ObjMarker>();
                f->regions.push_back({mk, totalAllocated});
                break;
            }
            case OP_REGION_END: {
                if (f->regions.empty()) break;
                auto [mk, startAlloc] = f->regions.back();
                f->regions.pop_back();
                if (regionsOpen > 0) regionsOpen--;
                // Only worth a collection when the region allocated a fair amount.
                if (totalAllocated - startAlloc >= 64 * 1024 && f->nativeDepth == 0 && gcEnabled()) {
                    SAVE();
                    regionCollect(mk);
                }
                break;
            }
            case OP_GET_GLOBAL_OPT: {
                uint16_t i = READ_U16();
                ModuleRt* m = frame->module;
                PUSH(sealed(i < m->globals.size() ? m->globals[i] : Value::undef()));
                break;
            }
            case OP_JUMP_IF_DEFINED: {
                uint32_t off = READ_U32();
                if (!TOP.isUndef()) ip += off;
                break;
            }
            case OP_SEND: {
                SAVE();
                Value ch = POP();
                Value v = POP();
                if (!isObjType(ch, OT::Channel)) panic("':>' sends to a flow, got a " + typeName(ch));
                channelSend(as<ObjChannel>(ch), v);
                SUSPEND();
                PUSH(Value());
                break;
            }
            case OP_RECV:
            case OP_RECV_ITER: {
                SAVE();
                Value ch = POP();
                if (!isObjType(ch, OT::Channel)) panic("'<:' receives from a flow, got a " + typeName(ch));
                bool blocked = false;
                Value v = channelRecv(as<ObjChannel>(ch), op == OP_RECV_ITER, &blocked);
                if (blocked) {
                    SUSPEND();
                }
                PUSH(v);
                break;
            }
            case OP_WITH_BEGIN: f->groups.emplace_back(); break;
            case OP_WITH_END: {
                const uint8_t* opStart = ip - 1;
                auto& group = f->groups.back();
                ObjTask* pending = nullptr;
                bool waiting = false;
                for (ObjTask* t : group) {
                    if (t->shared) {
                        if (sharedTaskDone(t)) continue;
                        ip = opStart;
                        SAVE();
                        if (stopDepth > 0) panic("can't wait for tasks inside a callback");
                        bool blocked = false;
                        sharedAwait(*this, t, false, &blocked);
                        if (blocked) {
                            waiting = true;
                            break;
                        }
                        ip = opStart + 1;
                        continue;
                    }
                    if (!t->done) {
                        pending = t;
                        break;
                    }
                }
                if (waiting) return;
                if (pending) {
                    ip = opStart;
                    SAVE();
                    if (stopDepth > 0) panic("can't wait for tasks inside a callback");
                    pending->waiters.push_back({f, false});
                    f->state = FState::Blocked;
                    return;
                }
                f->groups.pop_back();
                break;
            }
            case OP_SLEEP: {
                SAVE();
                Value ms = POP();
                if (!ms.isNum()) panic("retry delay must be a number of milliseconds");
                sleepCurrent(ms.num(), false);
                SUSPEND();
                break;
            }
            case OP_CHECK_TYPE: {
                auto* t = as<ObjTypeDesc>(consts[READ_U16()]);
                uint16_t what = READ_U16();
                if (!checkType(TOP, t)) {
                    SAVE();
                    panic(as<ObjString>(consts[what])->s + " should be " + t->text + ", got " + typeName(TOP));
                }
                break;
            }
            case OP_TODO: {
                uint16_t m = READ_U16();
                SAVE();
                std::string msg = as<ObjString>(consts[m])->s;
                panic("reached a 'todo'" + (msg.empty() ? std::string("") : ": " + msg));
            }
            case OP_IMPORT: {
                uint16_t idx = READ_U16();
                ModuleRt* m = modules[idx].get();
                if (m->initialized) {
                    PUSH(Value());
                    break;
                }
                m->initialized = true;
                SAVE();
                PUSH(Value::object(m->script));
                callClosure(f, m->script, 0, f->sp - 1);
                LOAD();
                break;
            }
            case OP_IMPORT_DONE: {
                uint16_t idx = READ_U16();
                ModuleRt* m = modules[idx].get();
                if (!m->obj) m->obj = alloc<ObjModule>(m);
                TOP = Value::object(m->obj);
                break;
            }
            case OP_EXPORT_GET: {
                const std::string& name = as<ObjString>(consts[READ_U16()])->s;
                Value src = TOP;
                if (isObjType(src, OT::Module)) {
                    ModuleRt* m = as<ObjModule>(src)->rt;
                    auto it = m->exports.find(name);
                    TOP = it == m->exports.end() ? Value() : sealed(m->globals[it->second]);
                } else if (isObjType(src, OT::Namespace)) {
                    auto* ns = as<ObjNamespace>(src);
                    auto it = ns->members.find(name);
                    TOP = it == ns->members.end() ? Value() : it->second;
                } else {
                    TOP = Value();
                }
                break;
            }
            case OP_FORMAT: {
                uint16_t spec = READ_U16();
                SAVE();
                Value v = TOP;
                if (spec == 0xFFFF)
                    TOP = isObjType(v, OT::String) ? v : vstr(toStr(v));
                else
                    TOP = vstr(formatValue(v, as<ObjString>(consts[spec])->s));
                break;
            }
            case OP_CONCAT: {
                uint16_t n = READ_U16();
                std::string s;
                for (size_t i = f->sp - n; i < f->sp; i++) s += as<ObjString>(STK[i])->s;
                f->sp -= n;
                PUSH(vstr(std::move(s)));
                break;
            }
            default: SAVE(); panic("internal: bad opcode " + std::to_string(op));
        }
    }
#undef LOAD
#undef SAVE
#undef READ_BYTE
#undef READ_U16
#undef READ_U32
#undef STK
#undef PUSH
#undef POP
#undef TOP
#undef SUSPEND
#undef SAFEPOINT
}

// ------------------------------------------------------------------ scheduling

Fiber* VM::newFiber() {
    Fiber* f = new Fiber();
    f->id = nextTaskId;
    f->serial = nextFiberSerial.fetch_add(1);
    f->index = fibers.size();
    fibers.push_back(f);
    fiberBySerial[f->serial] = f;
    return f;
}

Fiber* VM::sleepTop() {
    while (!sleepHeap.empty()) {
        const SleepEntry& e = sleepHeap.top();
        auto it = fiberBySerial.find(e.serial);
        if (it != fiberBySerial.end() && it->second->asleep && it->second->sleepGen == e.gen) return it->second;
        sleepHeap.pop();  // woken, gone, or asleep again with a newer entry
    }
    return nullptr;
}

Fiber* VM::fiberFor(uint64_t serial) {
    auto it = fiberBySerial.find(serial);
    return it == fiberBySerial.end() ? nullptr : it->second;
}

void VM::destroyFiber(Fiber* f) {
    closeUpvalues(f, 0);
    if (f->task) f->task->fiber = nullptr;
    if (f->index < fibers.size() && fibers[f->index] == f) {  // swap with the last one
        fibers[f->index] = fibers.back();
        fibers[f->index]->index = f->index;
        fibers.pop_back();
    } else {
        fibers.erase(std::remove(fibers.begin(), fibers.end(), f), fibers.end());
    }
    if (f->queued) runQueue.erase(std::remove(runQueue.begin(), runQueue.end(), f), runQueue.end());
    sleepRemove(f);
    fiberBySerial.erase(f->serial);
    delete f;
}

ObjTask* VM::spawn(Value callee, const Value* args, int argc) {
    auto* t = alloc<ObjTask>(nextTaskId++);
    Fiber* nf = newFiber();
    nf->id = t->id;
    nf->push(callee);
    for (int i = 0; i < argc; i++) nf->push(args[i]);
    nf->startArgc = argc;
    nf->task = t;
    nf->world = current ? current->world : nullptr;
    t->fiber = nf;
    enqueue(nf);
    if (current && !current->groups.empty()) current->groups.back().push_back(t);
    return t;
}

void VM::finishFiber(Fiber* f, Value result) {
    f->state = FState::Done;
    if (f->task && f->task->shared) {
        f->task->done = true;
        sharedComplete(*this, f->task, result);
        return;
    }
    if (f->task) {
        ObjTask* t = f->task;
        t->done = true;
        t->result = result;
        auto waiters = std::move(t->waiters);
        t->waiters.clear();
        for (auto& w : waiters) wake(w.fiber, result, w.pushResult);
    }
}

void VM::blockCurrent() {
    if (!current) panic("internal: no running task");
    if (current->nativeDepth > 0) panic("can't wait for a flow or task inside a callback (like a sort comparator)");
    current->state = FState::Blocked;
}

void VM::wake(Fiber* f, Value result, bool pushResult) {
    if (f->state == FState::Done) return;
    f->selectSel.reset();
    if (pushResult) f->push(result);
    f->state = FState::Ready;
    sleepRemove(f);
    enqueue(f);
}

void VM::sleepCurrent(double ms, bool pushResult) {
    Fiber* f = current;
    if (!f || f->nativeDepth > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds((long long)(ms * 1000)));
        return;
    }
    f->state = FState::Sleeping;
    f->wakeAt = monotonicSeconds() + ms / 1000.0;
    f->startArgc = pushResult ? -2 : -1;  // -2: push nil when waking
    sleepAdd(f);
}

void VM::awaitTask(ObjTask* t) {
    if (t->done) return;
    if (t->fiber == current) panic("a task can't wait for itself");
    t->waiters.push_back({current, true});
    blockCurrent();
}

void VM::channelSend(ObjChannel* ch, Value v) {
    if (ch->shared) return sharedSend(*this, ch, v);
    if (ch->closed) panic("can't send to a closed flow");
    while (!ch->recvWaiters.empty()) {
        RecvWaiter w = ch->recvWaiters.front();
        ch->recvWaiters.pop_front();
        if (w.sel && !w.sel->tryFire()) continue;  // a select that already took something else
        wake(w.fiber, w.sel ? selectResult(w.index, v) : v, true);
        return;
    }
    if ((int)ch->buf.size() < ch->cap) {
        ch->buf.push_back(v);
        return;
    }
    ch->sendWaiters.push_back({current, v});
    blockCurrent();
}

Value VM::channelRecv(ObjChannel* ch, bool forIter, bool* blocked) {
    *blocked = false;
    if (ch->shared) return sharedRecv(*this, ch, forIter, blocked);
    if (!ch->buf.empty()) {
        Value v = ch->buf.front();
        ch->buf.pop_front();
        if (!ch->sendWaiters.empty()) {
            auto [s, sv] = ch->sendWaiters.front();
            ch->sendWaiters.pop_front();
            ch->buf.push_back(sv);
            wake(s, Value(), true);
        }
        return v;
    }
    if (!ch->sendWaiters.empty()) {
        auto [s, sv] = ch->sendWaiters.front();
        ch->sendWaiters.pop_front();
        wake(s, Value(), true);
        return sv;
    }
    if (ch->closed) return forIter ? Value::done() : Value();
    ch->recvWaiters.push_back({current, forIter, nullptr, -1});
    blockCurrent();
    *blocked = true;
    return Value();
}

Value VM::selectResult(int index, Value v) { return newTuple({Value::integer(index), v}); }

// Can a receive on this flow go ahead without waiting?
static bool flowReady(ObjChannel* ch) {
    if (ch->shared) {
        std::lock_guard<std::mutex> lk(ch->shared->m);
        return !ch->shared->buf.empty() || !ch->shared->sendWaiters.empty() || ch->shared->closed;
    }
    return !ch->buf.empty() || !ch->sendWaiters.empty() || ch->closed;
}

Value VM::selectRecv(const std::vector<ObjChannel*>& chans, double timeoutMs, bool noWait, bool* blocked) {
    *blocked = false;
    size_t n = chans.size();
    // start at a rotating position so one busy flow doesn't starve the others
    size_t start = n ? (size_t)(selectRotor++ % n) : 0;
    for (size_t k = 0; k < n; k++) {
        size_t i = (start + k) % n;
        if (!flowReady(chans[i])) continue;
        bool b = false;
        Value v = channelRecv(chans[i], false, &b);
        if (!b) return selectResult((int)i, v);
        // raced with another thread: it registered us as a plain receiver; wait for it
        *blocked = true;
        current->selectSel.reset();
        return Value();
    }
    if (noWait) return selectResult(-1, Value());
    auto sel = std::make_shared<SelectState>();
    for (size_t i = 0; i < n; i++) {
        ObjChannel* ch = chans[i];
        if (ch->shared) {
            if (registerSharedSelect(*this, ch, sel, (int)i)) {
                // something arrived while we were registering: take it now if nobody fired us yet
                if (sel->tryFire()) {
                    bool b = false;
                    Value v = channelRecv(ch, false, &b);
                    if (!b) return selectResult((int)i, v);
                    *blocked = true;
                    return Value();
                }
                break;  // already fired by a sender: its wake-up is on the way
            }
        } else {
            ch->recvWaiters.push_back({current, false, sel, (int)i});
        }
    }
    if (timeoutMs >= 0 && !sel->fired) {
        Fiber* f = current;
        f->state = FState::Sleeping;
        f->wakeAt = monotonicSeconds() + timeoutMs / 1000.0;
        f->startArgc = -1;
        f->selectSel = sel;
        f->selectTimeoutIndex = (int)n;
        sleepAdd(f);
    } else {
        blockCurrent();
    }
    *blocked = true;
    return Value();
}

void VM::channelClose(ObjChannel* ch) {
    if (ch->shared) return sharedClose(*this, ch);
    if (ch->closed) return;
    ch->closed = true;
    auto waiters = std::move(ch->recvWaiters);
    ch->recvWaiters.clear();
    for (auto& w : waiters) {
        if (w.sel && !w.sel->tryFire()) continue;
        Value v = w.forIter ? Value::done() : Value();
        wake(w.fiber, w.sel ? selectResult(w.index, v) : v, true);
    }
    auto senders = std::move(ch->sendWaiters);
    ch->sendWaiters.clear();
    for (auto& [f, v] : senders) wake(f, Value(), true);
}

std::vector<StackEntry> VM::captureTrace(Fiber* f) {
    std::vector<StackEntry> out;
    if (!f) return out;
    for (auto it = f->frames.rbegin(); it != f->frames.rend(); ++it) {
        ObjFunction* fn = it->closure->fn;
        size_t off = it->ip > fn->chunk.code.data() ? (size_t)(it->ip - fn->chunk.code.data() - 1) : 0;
        out.push_back({fn->name, fn->file, fn->chunk.lineAt(off)});
        if (out.size() > 30) break;
    }
    return out;
}

void VM::schedule() {
    while (!exitRequested) {
        if (par) drainInbox();
        if (runQueue.empty()) {
            if (sleeping == 0) {
                if (!par) break;
                if (isWorker) {
                    if (par->stopping) break;
                    par->waitForWork(*this, -1);
                    continue;
                }
                // main VM: wait for par tasks; stop when every VM is idle
                if (!par->waitForWork(*this, -1)) break;
                continue;
            }
            Fiber* first = sleepTop();
            if (!first) continue;  // (only stale entries were left)
            double wait = first->wakeAt - monotonicSeconds();
            if (wait > 0) {
                if (par)
                    par->waitForWork(*this, wait);
                else
                    std::this_thread::sleep_for(std::chrono::microseconds((long long)(wait * 1e6)));
            }
            double now = monotonicSeconds();
            std::vector<Fiber*> due;
            while (Fiber* s = sleepTop()) {
                if (s->wakeAt > now + 1e-6) break;
                sleepHeap.pop();
                due.push_back(s);
            }
            for (Fiber* s : due) {
                if (s->selectSel) {
                    // a select's `after`: only if no flow delivered first
                    auto sel = s->selectSel;
                    if (sel->tryFire())
                        wake(s, selectResult(s->selectTimeoutIndex, Value()), true);
                    else
                        sleepRemove(s);
                    continue;
                }
                bool push = s->startArgc == -2;
                s->startArgc = -1;
                wake(s, Value(), push);
            }
            continue;
        }
        Fiber* f = runQueue.front();
        runQueue.pop_front();
        if (f->queued) f->queued--;
        if (f->state == FState::Done) continue;
        current = f;
        f->state = FState::Running;
        if (f->startArgc < 0 && f->frames.empty()) {
            // started with a native call that blocked; its result has been pushed
        } else if (f->startArgc >= 0) {
            int argc = f->startArgc;
            f->startArgc = -1;
            startingTask = true;
            bool pushed = callValue(f, f->stack[0], argc);
            startingTask = false;
            if (pushed && f->state == FState::Running) execute(f, 0);
        } else {
            execute(f, 0);
        }
        if (f->state == FState::Running) {
            Value result = f->sp > 0 ? f->stack[f->sp - 1] : Value();
            finishFiber(f, result);
            if (f == mainFiber) mainResult = result;
            if (f != mainFiber) destroyFiber(f);
        }
        current = nullptr;
    }
    current = nullptr;
}

bool VM::runCallable(Value callee, Value* result, bool isTest) {
    Fiber* f = newFiber();
    f->isMain = true;
    f->push(callee);
    f->startArgc = 0;
    Fiber* savedMain = mainFiber;
    mainFiber = f;
    enqueue(f);
    bool ok = true;
    try {
        schedule();
        if (f->state != FState::Done) {
            current = f;
            lastTrace = captureTrace(f);
            throw PanicError{"deadlock: the program is waiting on a flow or task that can never be ready"};
        }
        if (result) *result = mainResult;
    } catch (PanicError& e) {
        ok = false;
        lastPanic = e.msg;
        if (!(e.msg.rfind("deadlock", 0) == 0)) lastTrace = captureTrace(current);
        if (debugger && current) debugger->onPanic(*this, current, e.msg);
        if (onPanic && !isTest) onPanic(e.msg, lastTrace);
        // abandon every task: a panic stops the program
        for (Fiber* x : std::vector<Fiber*>(fibers))
            if (x != f) destroyFiber(x);
        nestedPool.clear();  // those fibers were just destroyed
        runQueue.clear();
        sleepHeap = {};
        sleeping = 0;
        current = nullptr;
    }
    mainFiber = savedMain;
    destroyFiber(f);
    return ok;
}

bool VM::runModule(ModuleRt* m) {
    m->initialized = true;
    Value result;
    bool ok = runCallable(Value::object(m->script), &result, false);
    lastResult = ok ? result : Value();
    if (ok && isObjType(result, OT::Error) && m->name == "<main>") {
        lastPanic = "unhandled error: " + toStr(result);
        lastTrace.clear();
        if (onPanic) onPanic(lastPanic, lastTrace);
        return false;
    }
    return ok;
}

std::string VM::runTest(Value fn) {
    Value result;
    if (!runCallable(fn, &result, true)) {
        std::string msg = lastPanic;
        for (auto& e : lastTrace) msg += "\n  at " + e.function + " (" + e.file + ":" + std::to_string(e.line) + ")";
        return msg;
    }
    if (isObjType(result, OT::Error)) return "returned an error: " + toStr(result);
    return "";
}

// Calls back into Ixo from a native (sort comparators, str methods). The call
// runs on a separate fiber so the caller's stack never moves while the native
// still holds pointers into it; it may not wait on flows or tasks.
// ------------------------------------------------------------------ JIT

void VM::intOverflow(char op, int64_t x, int64_t y) {
    panic("integer overflow: " + std::to_string(x) + " " + op + " " + std::to_string(y) +
          " doesn't fit in an int (ints wrap around; ixo test and IXO_CHECK_OVERFLOW=1 stop here instead)");
}

bool VM::jitShouldDemote(const ObjFunction* fn) const {
    if (!fn->jit || !fn->jit->tier2 || fn->noTier2) return false;  // (demoted once: a failed recompile keeps its code)
    uint64_t fails = fn->jit->guessFails;
    return fails > 64 && fails * 16 > (uint64_t)fn->jitCalls + fn->osrEntries;
}

// ------------------------------------------------------------------ the compiler thread
// Machine code is made on a thread of its own, so compiling never pauses the program
// (IXO_JIT_SYNC=1: on the program's thread). Meanwhile the function keeps running in
// the interpreter, or in the machine code it has; the new code is installed at the
// next call or loop entry (jitInstall). The compile works from copies of what the
// program changes as it runs: the type profile, the inline caches, the loop entries.
// Functions with more bytecode than this compile on the compiler thread (the ones
// whose compiles could pause the program for more than about 0.1 ms)
constexpr size_t kAsyncCompileBytes = 600;

struct JitJob {
    ObjFunction* fn = nullptr;
    ObjClosure* cl = nullptr;
    std::vector<std::pair<uint32_t, int>> seeds;
    std::vector<uint8_t> profile;
    std::vector<InlineCache> ics;
    bool noTier2 = false, demote = false;
    const char* reason = "";
    std::unique_ptr<JitCode> code;
    bool ok = false;
    std::string why;
    double took = 0;
};
struct JitThread {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::unique_ptr<JitJob>> todo, done;
    std::atomic<bool> ready{false};
    bool stop = false;
    std::thread th;
};

static void jitWorkerLoop(JitThread* t, VM* vm) {
    for (;;) {
        std::unique_ptr<JitJob> job;
        {
            std::unique_lock<std::mutex> lk(t->m);
            t->cv.wait(lk, [t] { return t->stop || !t->todo.empty(); });
            if (t->stop) return;
            job = std::move(t->todo.front());
            t->todo.pop_front();
        }
        job->code = std::make_unique<JitCode>();
        double t0 = monotonicSeconds();
        job->ok = jitCompileFrom(job->fn, job->cl, *job->code, &job->why, job->seeds, vm, &job->profile, &job->ics, job->noTier2);
        job->took = monotonicSeconds() - t0;
        std::lock_guard<std::mutex> g(t->m);
        t->done.push_back(std::move(job));
        t->ready.store(true, std::memory_order_release);
    }
}

void VM::jitEnqueue(ObjFunction* fn, ObjClosure* c, const char* why, bool demote) {
    if (fn->jitQueued) return;
    fn->jitQueued = true;
    if (!jitThread) {
        jitThread = new JitThread();
        jitThread->th = std::thread(jitWorkerLoop, jitThread, this);
    }
    auto job = std::make_unique<JitJob>();
    job->fn = fn;
    job->cl = c;
    job->seeds = fn->jitSeeds;
    job->profile = fn->typeSeen;
    job->ics = fn->ic;
    job->noTier2 = fn->noTier2 || demote;
    job->demote = demote;
    job->reason = why;
    extraRoots.push_back(Value::object(c));  // alive until its code is installed
    std::lock_guard<std::mutex> g(jitThread->m);
    jitThread->todo.push_back(std::move(job));
    jitThread->cv.notify_one();
}

void VM::jitStop() {
    if (!jitThread) return;
    {
        std::lock_guard<std::mutex> g(jitThread->m);
        jitThread->stop = true;
    }
    jitThread->cv.notify_all();
    jitThread->th.join();
    delete jitThread;
    jitThread = nullptr;
}

void VM::jitInstall() {
    if (!jitThread || !jitThread->ready.load(std::memory_order_acquire)) return;
    std::deque<std::unique_ptr<JitJob>> done;
    {
        std::lock_guard<std::mutex> g(jitThread->m);
        done.swap(jitThread->done);
        jitThread->ready.store(false, std::memory_order_relaxed);
    }
    static const bool log = std::getenv("IXO_JIT_LOG") != nullptr;
    for (auto& job : done) {
        ObjFunction* fn = job->fn;
        fn->jitQueued = false;
        for (size_t i = extraRoots.size(); i-- > 0;)
            if (extraRoots[i].isObj() && extraRoots[i].o == job->cl) {
                extraRoots.erase(extraRoots.begin() + (std::ptrdiff_t)i);
                break;
            }
        if (jitStats) {
            jitStats->compiles++;
            jitStats->compileSeconds += job->took;
        }
        bool compiled = fn->jitState == 2 || fn->jitState == 4;
        if (job->ok) {
            jitRetire(fn->jit);
            fn->jit = job->code.release();
            if (!compiled) fn->jitState = 2;
            jitAllowDirect(fn);
            if (jitStats && fn->jit->tier2) jitStats->typed++;
            if (jitStats && job->demote) jitStats->demoted++;
            if (log && job->demote)
                std::fprintf(stderr, "jit: %s compiled again without typed guesses (they kept failing)\n", fn->name.c_str());
            else if (log)
                std::fprintf(stderr, "jit: compiled %s in %.0f us (%s%s)\n", fn->name.c_str(), job->took * 1e6, job->reason,
                             fn->jit->tier2 ? ", typed" : "");
        } else if (compiled) {  // the code it has stays
            if (log) std::fprintf(stderr, "jit: %s keeps its code (compiling it again didn't work: it %s)\n", fn->name.c_str(), job->why.c_str());
        } else {
            fn->jitState = 3;
            jitAllowDirect(fn);
            if (jitStats) jitStats->notes.push_back({fn, "isn't compiled: it " + job->why});
            if (log || fn->wantJit) {
                flushOut();
                std::fprintf(stderr, "note: %s runs in the interpreter: the JIT can't compile it (it %s)\n", fn->name.c_str(),
                             job->why.c_str());
            }
        }
    }
    jitRetire(nullptr);
}

// Code that was replaced may still be running deeper in the native stack: it's
// freed (and its memory reused) once no machine code runs on this thread.
void VM::jitRetire(JitCode* old) {
    if (old) retiredCode.push_back(old);
    if (jitActive == 0 && !retiredCode.empty()) {
        for (JitCode* c : retiredCode) jitFreeCode(c);
        retiredCode.clear();
    }
}

void VM::jitDemote(ObjFunction* fn, ObjClosure* c) {
    fn->noTier2 = true;
    auto code = std::make_unique<JitCode>();
    std::string why;
    if (jitCompile(fn, c, *code, &why, fn->jitSeeds, this)) {
        jitRetire(fn->jit);
        fn->jit = code.release();
        jitAllowDirect(fn);
        if (jitStats) jitStats->demoted++;
        if (std::getenv("IXO_JIT_LOG"))
            std::fprintf(stderr, "jit: %s compiled again without typed guesses (they kept failing)\n", fn->name.c_str());
    } else if (std::getenv("IXO_JIT_LOG")) {
        std::fprintf(stderr, "jit: %s keeps its typed code (compiling it without typed guesses failed: it %s)\n",
                     fn->name.c_str(), why.c_str());
    }
}

bool VM::jitReady(Fiber* f, ObjClosure* c, int64_t seedAt, int seedDepth) {
    ObjFunction* fn = c->fn;
    if (isWorker || (debugger && !debugger->allowsJit()) || memProfiler || f->world || fn->heapId != heapId) return false;
    jitInstall();
    bool compiled = fn->jitState == 2 || fn->jitState == 4;
    if (fn->jitQueued) return compiled;  // being compiled: what it has (or the interpreter) meanwhile
    if (compiled && jitShouldDemote(fn)) {
        if (jitAsync && fn->chunk.code.size() > kAsyncCompileBytes) {
            fn->noTier2 = true;
            jitEnqueue(fn, c, "without typed guesses", true);
            return true;
        }
        jitDemote(fn, c);
    }
    // promotion: code made without types (too early to know them) is made again with
    // them once the function has run a while
    if (compiled && fn->jit && !fn->jit->tier2 && !fn->noTier2 && !fn->promoted && fn->jitCalls >= 1000 && !fn->typeSeen.empty()) {
        fn->promoted = true;
        if (jitAsync && fn->chunk.code.size() > kAsyncCompileBytes) {
            jitEnqueue(fn, c, "promoted to typed", false);
            return true;
        }
        auto code = std::make_unique<JitCode>();
        std::string why;
        if (jitCompile(fn, c, *code, &why, fn->jitSeeds, this) && code->tier2) {
            jitRetire(fn->jit);
            fn->jit = code.release();
            jitAllowDirect(fn);
        } else {
            jitFreeCode(code.release());
        }
    }
    bool newSeed = seedAt >= 0 && (!compiled || fn->jit->relearn ||
                                   std::find(fn->jit->loopHeaders.begin(), fn->jit->loopHeaders.end(), (uint32_t)seedAt) ==
                                       fn->jit->loopHeaders.end());
    if (compiled && (!newSeed || fn->jitRecompiles >= 8)) return true;
    if (fn->jitState == 3) return false;
    if (!compiled) {  // the type profile: what the optimizing JIT will assume
        if (fn->typeSeen.empty()) fn->typeSeen.assign(fn->chunk.code.size() + (size_t)fn->arity + 1, 0);
        if (seedAt < 0 && f->sp >= (size_t)fn->arity)
            for (int k = 0; k < fn->arity; k++)
                fn->typeSeen[fn->chunk.code.size() + (size_t)k] |= profBit(f->stack[f->sp - (size_t)fn->arity + (size_t)k]);
    }
    if (fn->noJit) {  // $nojit
        fn->jitState = 3;
        jitAllowDirect(fn);
        return false;
    }
    if (!compiled && warm && !fn->warmChecked) warmApply(fn);
    if (!compiled && !fn->wantJit && seedAt < 0 && ++fn->hotness < kJitCalls) return false;
    if (newSeed && std::find(fn->jitSeeds.begin(), fn->jitSeeds.end(), std::pair<uint32_t, int>((uint32_t)seedAt, seedDepth)) !=
                       fn->jitSeeds.end()) {
        if (compiled) fn->jitRecompiles++;  // already an entry: compiled again for what the interpreter just learned
        fn->jitSeeds.push_back({(uint32_t)seedAt, seedDepth});
    } else if (newSeed) {
        fn->jitSeeds.push_back({(uint32_t)seedAt, seedDepth});
        if (compiled) fn->jitRecompiles++;
    }
    // big functions (the compiles that take long) go to the compiler thread; small ones
    // compile here at once, in a few microseconds, rather than run interpreted meanwhile
    if (jitAsync && fn->chunk.code.size() > kAsyncCompileBytes) {
        jitEnqueue(fn, c, seedAt >= 0 ? "a hot loop" : fn->wantJit ? "$jit" : "called often", false);
        return compiled;
    }
    static const bool log = std::getenv("IXO_JIT_LOG") != nullptr;
    auto code = std::make_unique<JitCode>();
    std::string why;
    double t0 = monotonicSeconds();
    bool ok = jitCompile(fn, c, *code, &why, fn->jitSeeds, this);
    double took = monotonicSeconds() - t0;
    if (jitStats) {
        jitStats->compiles++;
        jitStats->compileSeconds += took;
    }
    if (ok) {
        if (compiled) jitRetire(fn->jit);
        fn->jit = code.release();
        fn->jitState = 2;
        jitAllowDirect(fn);
        if (jitStats && fn->jit->tier2) jitStats->typed++;
        if (log)
            std::fprintf(stderr, "jit: compiled %s in %.0f us (%s%s)\n", fn->name.c_str(), took * 1e6,
                         seedAt >= 0 ? "a hot loop" : fn->wantJit ? "$jit" : "called often",
                         fn->jit->tier2 ? ", typed" : "");
        return true;
    }
    if (compiled) {  // a new entry point didn't work out: keep the code we have
        if (log) std::fprintf(stderr, "jit: %s keeps its code (a new loop entry didn't compile: it %s)\n", fn->name.c_str(), why.c_str());
        fn->jitSeeds.pop_back();
        return true;
    }
    fn->jitState = 3;
    jitAllowDirect(fn);
    if (jitStats) jitStats->notes.push_back({fn, "isn't compiled: it " + why});
    if (log || fn->wantJit) {
        flushOut();
        std::fprintf(stderr, "note: %s runs in the interpreter: the JIT can't compile it (it %s)\n", fn->name.c_str(),
                     why.c_str());
    }
    return false;
}

bool VM::jitTry(ObjClosure* c) {
    ObjFunction* fn = c->fn;
    if (isWorker || (debugger && !debugger->allowsJit()) || memProfiler || fn->heapId != heapId) return false;
    jitInstall();
    if (fn->jitState == 2) return true;
    if (fn->jitState == 3 || fn->jitState == 4 || fn->noJit) return false;
    if (jitAsync && fn->chunk.code.size() > kAsyncCompileBytes) {  // called from machine code: compiled soon
        jitEnqueue(fn, c, "called from machine code", false);
        return false;
    }
    // called from machine code: the caller is hot, so compile right away
    auto code = std::make_unique<JitCode>();
    std::string why;
    double t0 = monotonicSeconds();
    bool ok = jitCompile(fn, c, *code, &why, fn->jitSeeds, this);
    double took = monotonicSeconds() - t0;
    if (jitStats) {
        jitStats->compiles++;
        jitStats->compileSeconds += took;
    }
    if (ok) {
        fn->jit = code.release();
        fn->jitState = 2;
        jitAllowDirect(fn);
        if (jitStats && fn->jit->tier2) jitStats->typed++;
        static const bool log = std::getenv("IXO_JIT_LOG") != nullptr;
        if (log) std::fprintf(stderr, "jit: compiled %s in %.0f us (called from machine code)\n", fn->name.c_str(), took * 1e6);
        return true;
    }
    fn->jitState = 3;
    jitAllowDirect(fn);
    if (jitStats) jitStats->notes.push_back({fn, "isn't compiled: it " + why});
    if (std::getenv("IXO_JIT_LOG")) std::fprintf(stderr, "jit: %s stays interpreted (it %s)\n", fn->name.c_str(), why.c_str());
    return false;
}

void VM::noteJitGiveUp(ObjFunction* fn, const char* what) {
    jitStats->notes.push_back({fn, std::string(what) == "calls"
                                       ? "is called in the interpreter now: its machine code kept stopping early"
                                       : "loops stay in the interpreter now: their machine code kept stopping early"});
}

void VM::printJitStats() {
    if (!jitStats) return;
    JitStats& s = *jitStats;
    auto where = [](ObjFunction* fn, uint32_t off) {
        std::string file = fn->file;
        size_t sl = file.find_last_of("/\\");
        if (sl != std::string::npos) file = file.substr(sl + 1);
        return (fn->name.empty() ? std::string("<script>") : fn->name) + "  (" + file + ":" +
               std::to_string(fn->chunk.lineAt(off)) + ")";
    };
    uint64_t total = s.budgetExits;
    for (auto& [k, n] : s.exits) total += n;
    std::fprintf(stderr, "\n-- jit: entered machine code %llu times; it handed over to the interpreter %llu times --\n",
                 (unsigned long long)s.entries, (unsigned long long)total);
    std::fprintf(stderr, "  compiled %llu time%s in %.2f ms\n", (unsigned long long)s.compiles, s.compiles == 1 ? "" : "s",
                 s.compileSeconds * 1000);
    if (s.typed || s.demoted)
        std::fprintf(stderr, "  %llu with typed code (values in registers), %llu compiled again without it\n",
                     (unsigned long long)s.typed, (unsigned long long)s.demoted);
    if (warm)
        std::fprintf(stderr, "  warm start: %llu function%s known from earlier runs\n", (unsigned long long)warm->hits,
                     warm->hits == 1 ? "" : "s");
    if (s.budgetExits)
        std::fprintf(stderr, "  %10llu  to let other tasks run (normal)\n", (unsigned long long)s.budgetExits);
    std::vector<std::pair<uint64_t, std::pair<ObjFunction*, uint32_t>>> v;
    for (auto& [k, n] : s.exits) v.push_back({n, k});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < v.size() && i < 15; i++) {
        ObjFunction* fn = v[i].second.first;
        uint32_t off = v[i].second.second;
        uint8_t op = off < fn->chunk.code.size() ? fn->chunk.code[off] : 0;
        std::fprintf(stderr, "  %10llu  at %-14s %s\n", (unsigned long long)v[i].first, opName(op), where(fn, off).c_str());
    }
    for (auto& [fn, why] : s.notes) {
        std::string name = fn->name.empty() ? "<script>" : fn->name;
        std::fprintf(stderr, "  %s %s\n", name.c_str(), why.c_str());
    }
}

void VM::resumeAfterJit(Fiber* f, JitCtx& ctx) {
    if (ctx.chainLen < 0) panic("internal: the JIT lost track of its calls");
    if (jitStats) {
        if (ctx.budgetExit) jitStats->budgetExits++;
        else if (ctx.bailClosure) jitStats->exits[{ctx.bailClosure->fn, ctx.bailOffset}]++;
    }
    // IXO_JIT_TRACE=1: where machine code stops (the first 50 times)
    static const bool trace = std::getenv("IXO_JIT_TRACE") != nullptr;
    static int traced = 0;
    if (trace && traced++ < 50)
        std::fprintf(stderr, "jit stop: %s at %u (chain %lld)\n", ctx.bailClosure ? ctx.bailClosure->fn->name.c_str() : "?",
                     ctx.bailOffset, (long long)ctx.chainLen);
    CallFrame& top = f->frames.back();
    if (ctx.chainLen == 0) {
        top.ip = top.closure->fn->chunk.code.data() + ctx.bailOffset;
    } else {
        // the last waiting function is the outermost: the frame already running
        top.ip = top.closure->fn->chunk.code.data() + ctx.chain[ctx.chainLen - 1].resume;
        for (int64_t k = ctx.chainLen - 2; k >= 0; k--) {
            auto& w = ctx.chain[k];
            f->frames.push_back({w.closure, w.closure->fn->chunk.code.data() + w.resume, (size_t)(w.base - f->stack),
                                 w.closure->fn->module});
        }
        ObjClosure* in = ctx.bailClosure;
        f->frames.push_back({in, in->fn->chunk.code.data() + ctx.bailOffset, (size_t)(ctx.bailBase - f->stack),
                             in->fn->module});
    }
    f->sp = (size_t)(ctx.bailBase - f->stack) + ctx.bailDepth;
    if (jitPanic) {
        std::exception_ptr e = jitPanic;
        jitPanic = nullptr;
        f->frames.back().ip++;  // inside the instruction, for the line in the trace
        std::rethrow_exception(e);
    }
}

namespace {
// Installed as the debugger while jitStep runs, so the dispatch loop checks
// where the instruction ends; it passes everything on to a real hook.
struct StepHook : Debugger {
    Debugger* inner = nullptr;
    void check(VM& vm, Fiber* f) override {
        if (inner) inner->check(vm, f);
    }
    void onPanic(VM& vm, Fiber* f, const std::string& msg) override {
        if (inner) inner->onPanic(vm, f, msg);
    }
    bool allowsJit() const override { return true; }
};
}  // namespace

// Runs instruction `s` of a compiled function in the interpreter, on the same
// fiber, then returns to the machine code. Anything that would call a function
// written in Ixo gives up first (the fiber's stack must not move under the
// machine code), and the machine code stops at the instruction instead.
int VM::jitStep(JitCtx& ctx, Value* frameBase, JitOpSite& s) {
    Fiber* f = current;
    if (!f || frameBase < f->stack || frameBase >= f->stack + f->cap || s.aborts > 2) return JIT_EXIT;
    size_t base = (size_t)(frameBase - f->stack);
    size_t savedSp = f->sp;
    Value* in = frameBase + s.depth - s.pops;
    Value small[8];  // the inputs, put back if it gives up
    std::unique_ptr<Value[]> big;
    Value* inputs = small;
    if (s.pops > 8) {
        big.reset(new Value[(size_t)s.pops]);
        inputs = big.get();
    }
    std::copy(in, in + s.pops, inputs);
    ObjFunction* fn = s.self->fn;
    ObjClosure* self = s.self;
    if (fn->upvalueCount > 0 && isObjType(frameBase[0], OT::Closure) && as<ObjClosure>(frameBase[0])->fn == fn)
        self = as<ObjClosure>(frameBase[0]);  // one of the function's closures: its captured variables
    const uint8_t* code = fn->chunk.code.data();
    f->sp = base + (size_t)s.depth;
    f->frames.push_back({self, code + s.off, base, fn->module});
    size_t level = f->frames.size();
    Value* stackWas = f->stack;
    if (!stepHook) stepHook = new StepHook();
    auto* hook = static_cast<StepHook*>(stepHook);
    struct Saved {
        VM& vm;
        Debugger* debugger;
        Debugger* inner;
        Fiber* fiber;
        size_t frames;
        const uint8_t* stop;
        ~Saved() {
            vm.debugger = debugger;
            static_cast<StepHook*>(vm.stepHook)->inner = inner;
            vm.stepFiber = fiber;
            vm.stepFrames = frames;
            vm.stepStop = stop;
        }
    } saved{*this, debugger, hook->inner, stepFiber, stepFrames, stepStop};
    if (debugger != stepHook) hook->inner = debugger;
    debugger = stepHook;
    stepFiber = f;
    stepFrames = level;
    stepStop = code + s.next;
    int outcome = 0;  // 0 ran, 1 gave up, 2 panicked
    try {
        execute(f, level - 1);
    } catch (JitStepAbort&) {
        outcome = 1;
    } catch (...) {
        jitPanic = std::current_exception();
        outcome = 2;
    }
    if (f->stack != stackWas) {  // can't happen: nothing on this fiber grows its stack here
        std::fprintf(stderr, "ixo: internal error: the stack moved under machine code\n");
        std::abort();
    }
    if (outcome != 0) {
        while (f->frames.size() >= level) f->frames.pop_back();
        if (outcome == 1) {
            std::copy(inputs, inputs + s.pops, in);
            s.aborts++;
        }
        f->sp = savedSp;
        return JIT_EXIT;
    }
    if (f->state != FState::Running) {  // it waits (or let others run): continue after it later
        ctx.pausedExit = 1;
        ctx.bailOffset = (uint32_t)(f->frames[level - 1].ip - code);
        ctx.bailDepth = (uint32_t)(f->sp - base);
        while (f->frames.size() >= level) f->frames.pop_back();
        return JIT_PAUSED;
    }
    f->frames.pop_back();
    f->sp = savedSp;
    if (bytesAllocated > nextGC) ctx.budget = 0;  // leave machine code at the next loop turn to collect
    return JIT_DONE;
}

// The instructions machine code meets most outside numbers (indexing maps and
// strings, f-strings, building strings, operators on other values), done the
// way the interpreter does them but without entering it. Others go to jitStep.
int VM::jitFastOp(JitCtx& ctx, Value* frameBase, JitOpSite& s) {
    Fiber* f = current;
    if (!f || frameBase < f->stack || frameBase >= f->stack + f->cap) return JIT_EXIT;
    ObjFunction* fn = s.self->fn;
    const uint8_t* at = fn->chunk.code.data() + s.off;
    auto u16 = [&](int k) { return (uint16_t)(at[k] | (at[k + 1] << 8)); };
    uint8_t op = at[0];
    switch (op) {
        case OP_GET_INDEX: case OP_SET_INDEX: case OP_FORMAT: case OP_CONCAT: case OP_LIST_APPEND: case OP_ADD:
        case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_IDIV: case OP_LT: case OP_LE: case OP_GT:
        case OP_GE: case OP_EQ: case OP_NE: case OP_INPLACE: case OP_STR_APPEND: case OP_STR_SEAL:
        case OP_FSTR_BEGIN: case OP_FSTR_ADD: case OP_FSTR_LIT: case OP_LIST: case OP_TUPLE: case OP_MAP:
        case OP_GET_BUILTIN:
            break;
        case OP_STR_APPEND_G: {
            ModuleRt* m = fn->module;
            uint16_t g = u16(1);
            if (!m || g >= m->globals.size() || m->globals[g].isUndef()) return jitStep(ctx, frameBase, s);
            break;
        }
        case OP_GET_UPVAL: case OP_SET_UPVAL: {
            if (!isObjType(frameBase[0], OT::Closure)) return jitStep(ctx, frameBase, s);
            auto* cl = as<ObjClosure>(frameBase[0]);
            uint16_t k = u16(1);
            if (cl->fn != fn || k >= cl->upvalues.size()) return jitStep(ctx, frameBase, s);
            ObjUpvalue* u = cl->upvalues[k];
            Value* top = frameBase + s.depth;
            if (op == OP_GET_UPVAL) {
                Value v = u->fiber ? u->fiber->stack[u->slot] : u->closed;
                if (v.isUndef()) return jitStep(ctx, frameBase, s);  // it reports the unset variable
                top[0] = v;
            } else if (u->fiber) {
                u->fiber->stack[u->slot] = top[-1];
            } else {
                u->closed = top[-1];
            }
            return JIT_DONE;
        }
        default: return jitStep(ctx, frameBase, s);
    }
    Value* top = frameBase + s.depth;
    size_t savedSp = f->sp;
    f->sp = (size_t)(frameBase - f->stack) + (size_t)s.depth;  // the collector sees the frame
    try {
        switch (op) {
            case OP_GET_INDEX: top[-2] = getIndex(top[-2], top[-1]); break;
            case OP_SET_INDEX: setIndex(top[-3], top[-2], top[-1]); break;
            case OP_FORMAT: {
                uint16_t spec = u16(1);
                Value v = top[-1];
                if (spec == 0xFFFF)
                    top[-1] = isObjType(v, OT::String) ? v : vstr(toStr(v));
                else
                    top[-1] = vstr(formatValue(v, as<ObjString>(fn->chunk.consts[spec])->s));
                break;
            }
            case OP_CONCAT: {
                uint16_t n = u16(1);
                size_t len = 0;
                for (uint16_t k = 0; k < n; k++) len += as<ObjString>(top[(int)k - n])->s.size();
                std::string out;
                out.reserve(len);
                for (uint16_t k = 0; k < n; k++) out += as<ObjString>(top[(int)k - n])->s;
                top[-(int)n] = vstr(std::move(out));
                break;
            }
            case OP_LIST_APPEND: as<ObjList>(frameBase[u16(1)])->push(top[-1]); break;
            case OP_STR_APPEND: strAppend(frameBase[u16(1)], top[-1], at[3] != 0, 1); break;
            case OP_STR_APPEND_G: strAppend(fn->module->globals[u16(1)], top[-1], at[3] != 0, 2); break;
            case OP_STR_SEAL: {
                Value v = frameBase[u16(1)];
                if (isObjType(v, OT::String)) seal(as<ObjString>(v));
                break;
            }
            case OP_FSTR_BEGIN: top[0] = vstr(std::string()); break;
            case OP_GET_BUILTIN: top[0] = builtins[u16(1)].value; break;
            case OP_LIST: case OP_TUPLE: {
                uint16_t n = u16(1);
                std::vector<Value> items(top - n, top);
                top[-(int)n] = op == OP_LIST ? newList(std::move(items)) : newTuple(std::move(items));
                break;
            }
            case OP_MAP: {
                uint16_t n = u16(1);
                auto* m = alloc<ObjMap>();
                m->entries.reserve(n);
                for (Value* p = top - 2 * n; p < top; p += 2) {
                    requireHashable(*this, p[0]);
                    m->set(p[0], p[1]);
                }
                top[-2 * (int)n] = Value::object(m);
                break;
            }
            case OP_FSTR_ADD: fstrAdd(top[-2], top[-1], u16(1), fn->chunk.consts.data()); break;
            case OP_FSTR_LIT: {
                auto* b = as<ObjString>(top[-1]);
                const std::string& lit = as<ObjString>(fn->chunk.consts[u16(1)])->s;
                b->s += lit;
                b->kind = 0;
                grow(b, lit.size());
                break;
            }
            case OP_INPLACE: {
                uint8_t bop = at[1];
                const char* name = bop == OP_ADD ? "+=" : bop == OP_SUB ? "-=" : bop == OP_MUL ? "*="
                                 : bop == OP_DIV ? "/=" : bop == OP_MOD ? "%=" : nullptr;
                Value out;
                if (name && isObjType(top[-2], OT::Instance) && callOperator(top[-2], name, &top[-1], 1, out)) break;
                top[-2] = binaryOp(bop, top[-2], top[-1]);
                break;
            }
            default: top[-2] = binaryOp(op, top[-2], top[-1]); break;
        }
    } catch (...) {
        jitPanic = std::current_exception();
        f->sp = savedSp;
        return JIT_EXIT;
    }
    f->sp = savedSp;
    if (bytesAllocated > nextGC) ctx.budget = 0;  // leave machine code at the next loop turn to collect
    return JIT_DONE;
}

int VM::iterStep(Fiber* f, ObjIterator* it, Value& out) {
    if (it->pending) {  // a flow delivered this while we waited
        it->pending = false;
        out = f->stack[--f->sp];
        return out.isDone() ? 1 : 0;
    }
    Obj* src = it->src.o;
    if (src->type == OT::Range) {
        auto* r = static_cast<ObjRange*>(src);
        bool more = r->step > 0 ? (r->inclusive ? it->cur <= r->end : it->cur < r->end)
                                : (r->inclusive ? it->cur >= r->end : it->cur > r->end);
        if (!more) return 1;
        out = Value::integer(it->cur);
        it->cur += r->step;
        return 0;
    }
    if (src->type == OT::List && !it->pairs) {
        auto* l = static_cast<ObjList*>(src);
        if (it->idx >= l->len()) return 1;
        out = l->get(it->idx++);
        return 0;
    }
    if (!iterNext(f, it, out)) {
        it->pending = true;
        return 2;
    }
    return out.isDone() ? 1 : 0;
}

// s += x for a local that only a loop grows: in place while the string is the
// loop's own, else exactly what `+=` (or `s = s + x`) does.
void VM::strAppend(Value& local, Value x, bool plainAdd, uint8_t owner) {
    if (isObjType(local, OT::String) && isObjType(x, OT::String)) {
        auto* cur = as<ObjString>(local);
        const std::string& add = as<ObjString>(x)->s;
        if (cur->appendable == owner) {
            if (cur->kind == 1 && !VM::ascii(as<ObjString>(x))) cur->kind = 2;
            else if (cur->kind != 1) cur->kind = 0;
            cur->s += add;
            cur->hashv = 0;
            grow(cur, add.size());
            return;
        }
        std::string out;
        out.reserve(std::max<size_t>(32, 2 * (cur->s.size() + add.size())));
        out += cur->s;
        out += add;
        ObjString* fresh = str(std::move(out));
        fresh->appendable = owner;
        local = Value::object(fresh);
        return;
    }
    Value out;
    if (!plainAdd && isObjType(local, OT::Instance) && callOperator(local, "+=", &x, 1, out)) return;
    local = binaryOp(OP_ADD, local, x);
}

void VM::fstrAdd(Value builder, Value v, uint16_t spec, const Value* consts) {
    auto* b = as<ObjString>(builder);
    size_t before = b->s.size();
    b->kind = 0;
    if (spec != 0xFFFF)
        b->s += formatValue(v, as<ObjString>(consts[spec])->s);
    else if (isObjType(v, OT::String))
        b->s += as<ObjString>(v)->s;
    else
        b->s += toStr(v);
    grow(b, b->s.size() - before);
}

// Calls a built-in from machine code like the interpreter does (args[0..total)).
int VM::jitNative(JitCtx& ctx, Value* frameBase, const JitOpSite& s, ObjNative* n, Value* result, Value* args,
                  int total) {
    Fiber* f = current;
    if (!f || frameBase < f->stack || frameBase >= f->stack + f->cap) return JIT_EXIT;
    if (total < n->minArity || (n->maxArity >= 0 && total > n->maxArity)) return JIT_STEP;  // it reports that
    size_t savedSp = f->sp;
    f->sp = (size_t)(frameBase - f->stack) + (size_t)s.depth;  // the collector sees the frame
    ObjNative* savedNative = currentNative;
    currentNative = n;
    nativeData = n->data;
    Value r;
    try {
        r = n->fn(*this, args, total);
    } catch (...) {
        currentNative = savedNative;
        jitPanic = std::current_exception();
        f->sp = savedSp;
        return JIT_EXIT;
    }
    currentNative = savedNative;
    if (f->state != FState::Running) {  // it waits: its result comes when the task wakes
        ctx.pausedExit = 1;
        ctx.bailOffset = s.next;
        ctx.bailDepth = (uint32_t)(s.depth - s.pops);
        f->sp = (size_t)(frameBase - f->stack) + ctx.bailDepth;
        return JIT_PAUSED;
    }
    *result = r;
    f->sp = savedSp;
    if (bytesAllocated > nextGC) ctx.budget = 0;
    return JIT_DONE;
}

bool VM::runJit(Fiber* f, ObjFunction* fn, size_t base, uint64_t entry, JitCtx& ctx) {
    JitCode& jc = *fn->jit;
    // deeper recursion continues in the interpreter; a fiber that gets that deep gets more room next time
    ctx.maxDepth = f->jitDepth;
    if (jitChain.size() < 2100) jitChain.resize(2100);
    ctx.chain = jitChain.data();
    ctx.chainLen = 0;
    ctx.budget = 100000;  // loop rounds before the machine code lets other tasks run
    ctx.budgetExit = 0;
    ctx.pausedExit = 0;
    ctx.chainCap = (int64_t)jitChain.size();
    // room for recursion up to that depth, so it doesn't keep stopping for stack space
    size_t want = base + (size_t)jc.maxDepth * (size_t)(ctx.maxDepth + 1) + 8;
    while (f->cap < want) f->grow();
    ctx.stackEnd = f->stack + f->cap;
    ctx.depth = 0;
    ctx.vm = this;
    if (jitStats) jitStats->entries++;
    struct InJit {  // tells the profiler machine code is running
        Profiler* p;
        explicit InJit(Profiler* q) : p(q) {
            if (p) p->inJit++;
        }
        ~InJit() {
            if (p) p->inJit--;
        }
    } inJit{profiler};
    struct Active {  // replaced code isn't freed while this runs
        int& n;
        explicit Active(int& x) : n(x) { n++; }
        ~Active() { n--; }
    } active{jitActive};
    int r = jc.fn(f->stack + base, &ctx, entry);
    if (r != 0) {
        if (ctx.chainLen + 2 >= ctx.maxDepth && f->jitDepth < 2000) f->jitDepth = std::min(2000, f->jitDepth * 4);
        return false;
    }
    f->sp = base + 1;
    return true;
}

int VM::placeNamedArgs(Fiber* f, int argc, const std::vector<std::string>& names) {
    size_t calleeSlot = f->sp - argc - 1;
    Value callee = f->stack[calleeSlot];
    ObjFunction* fn = nullptr;
    std::string what;
    if (isObjType(callee, OT::Closure)) {
        fn = as<ObjClosure>(callee)->fn;
    } else if (isObjType(callee, OT::Bound) && isObjType(as<ObjBound>(callee)->method, OT::Closure)) {
        fn = as<ObjClosure>(as<ObjBound>(callee)->method)->fn;
    } else if (isObjType(callee, OT::Class) && as<ObjClass>(callee)->ctor) {
        fn = as<ObjClass>(callee)->ctor->fn;
    }
    if (!fn) panic("named arguments only work with functions and classes written in Ixo, not a " + typeName(callee));
    std::string fname = fn->isCtor ? fn->name : fn->name.substr(fn->name.rfind('.') + 1);
    int positional = argc - (int)names.size();
    const auto& params = fn->paramNames;
    std::vector<Value> args(std::max<size_t>((size_t)positional, params.size()), Value::undef());
    for (int i = 0; i < positional; i++) args[(size_t)i] = f->stack[calleeSlot + 1 + (size_t)i];
    std::vector<bool> given(args.size(), false);
    for (int i = 0; i < positional; i++) given[(size_t)i] = true;
    for (size_t k = 0; k < names.size(); k++) {
        auto it = std::find(params.begin(), params.end(), names[k]);
        if (it == params.end()) {
            std::string list;
            for (auto& p : params) list += (list.empty() ? "" : ", ") + p;
            panic(fname + "() has no parameter '" + names[k] + "'" + (list.empty() ? "" : " (parameters: " + list + ")"));
        }
        size_t idx = (size_t)(it - params.begin());
        if (given[idx]) panic("the argument '" + names[k] + "' of " + fname + "() is given twice");
        args[idx] = f->stack[calleeSlot + 1 + (size_t)positional + k];
        given[idx] = true;
    }
    for (int i = 0; i < fn->minArity && (size_t)i < args.size(); i++)
        if (!given[(size_t)i]) panic(fname + "() is missing the argument '" + params[(size_t)i] + "'");
    // trailing parameters nobody gave get their defaults
    while (args.size() > (size_t)positional && !given[args.size() - 1]) {
        args.pop_back();
        given.pop_back();
    }
    f->sp = calleeSlot + 1;
    for (auto& v : args) f->push(v);
    return (int)args.size();
}

Value VM::callNested(Value callee, const Value* args, int argc) {
    Fiber* outer = current;
    if (!outer) panic("internal: callNested without a running task");
    std::vector<Value> copy(args, args + argc);
    Fiber* f;
    if (!nestedPool.empty()) {
        f = nestedPool.back();
        nestedPool.pop_back();
    } else {
        f = newFiber();
    }
    f->state = FState::Running;
    f->nativeDepth = 1;
    f->task = nullptr;
    f->world = outer->world;
    f->sp = 0;
    f->frames.clear();
    f->push(callee);
    for (auto& a : copy) f->push(a);
    f->caller = outer;
    current = f;
    struct Guard {
        VM& vm;
        Fiber* f;
        Fiber* outer;
        ~Guard() {
            vm.closeUpvalues(f, 0);
            f->sp = 0;
            f->frames.clear();
            f->groups.clear();
            f->nativeDepth = 0;
            f->state = FState::Done;
            f->caller = nullptr;
            vm.current = outer;
            vm.nestedPool.push_back(f);
        }
    } guard{*this, f, outer};
    bool pushed = callValue(f, callee, argc);
    if (pushed) execute(f, 0);
    return f->sp > 0 ? f->stack[f->sp - 1] : Value();
}

// ------------------------------------------------------------------ garbage collection

void VM::markObject(Obj* o) {
    if (!o || o->marked || o->heapId != heapId) return;
    o->marked = true;
    grayStack.push_back(o);
}

void VM::markValue(Value v) {
    if (v.t == VT::Obj) markObject(v.o);
}

void VM::blacken(Obj* o) {
    switch (o->type) {
        case OT::String: break;
        case OT::List:
            if (!static_cast<ObjList*>(o)->typed())
                for (auto& v : static_cast<ObjList*>(o)->vals) markValue(v);
            break;
        case OT::Tuple:
            for (auto& v : static_cast<ObjTuple*>(o)->items) markValue(v);
            break;
        case OT::Map:
            for (auto& [k, v] : static_cast<ObjMap*>(o)->entries) {
                markValue(k);
                markValue(v);
            }
            break;
        case OT::Set:
            for (auto& v : static_cast<ObjSet*>(o)->items) markValue(v);
            break;
        case OT::Function:
            for (auto& v : static_cast<ObjFunction*>(o)->chunk.consts) markValue(v);
            break;
        case OT::Closure: {
            auto* c = static_cast<ObjClosure*>(o);
            markObject(c->fn);
            for (auto* u : c->upvalues) markObject(u);
            break;
        }
        case OT::Upvalue: markValue(static_cast<ObjUpvalue*>(o)->closed); break;
        case OT::Native: markValue(static_cast<ObjNative*>(o)->data); break;
        case OT::Bound: {
            auto* b = static_cast<ObjBound*>(o);
            markValue(b->receiver);
            markValue(b->method);
            break;
        }
        case OT::Class: {
            auto* c = static_cast<ObjClass*>(o);
            for (auto& [k, v] : c->methods) markValue(v);
            markObject(c->ctor);
            break;
        }
        case OT::Instance: {
            auto* i = static_cast<ObjInstance*>(o);
            markObject(i->cls);
            for (auto& v : i->fields) markValue(v);
            break;
        }
        case OT::Range: break;
        case OT::Error: markValue(static_cast<ObjError*>(o)->payload); break;
        case OT::Channel: {
            auto* ch = static_cast<ObjChannel*>(o);
            for (auto& v : ch->buf) markValue(v);
            for (auto& [f, v] : ch->sendWaiters) markValue(v);
            break;
        }
        case OT::Task: markValue(static_cast<ObjTask*>(o)->result); break;
        case OT::Module: break;
        case OT::Interface: break;
        case OT::Namespace: {
            auto* ns = static_cast<ObjNamespace*>(o);
            for (auto& [k, v] : ns->members) markValue(v);
            markValue(ns->callable);
            break;
        }
        case OT::Iterator:
            markValue(static_cast<ObjIterator*>(o)->src);
            markValue(static_cast<ObjIterator*>(o)->key);
            break;
        case OT::TypeDesc: markObject(static_cast<ObjTypeDesc*>(o)->inner); break;
        default: break;
    }
}

void VM::markRoots() {
    for (Fiber* f : fibers) {
        for (auto& r : f->regions) markObject(r.first);
        for (size_t i = 0; i < f->sp; i++) markValue(f->stack[i]);
        for (auto& fr : f->frames) markObject(fr.closure);
        for (ObjUpvalue* u = f->openUpvalues; u; u = u->nextOpen) markObject(u);
        markObject(f->task);
        for (auto& g : f->groups)
            for (ObjTask* t : g) markObject(t);
        if (f->world)
            for (auto& [orig, m] : f->world->modules) {
                for (auto& v : m->globals) markValue(v);
                markObject(m->obj);
            }
    }
    markObject(defaultRng);
    for (auto& m : modules) {
        for (auto& v : m->globals) markValue(v);
        markObject(m->script);
        markObject(m->obj);
        markValue(m->nativeNs);
    }
    for (auto& b : builtins) markValue(b.value);
    for (auto& table : methods)
        for (auto& [k, mi] : table) markValue(mi.fn);
    markValue(nativeData);
    markObject(currentNative);
    markValue(mainResult);
    markValue(lastResult);
    for (auto& v : extraRoots) markValue(v);
    for (ObjString* c : asciiChars) markObject(c);
}

void VM::traceGray() {
    while (!grayStack.empty()) {
        Obj* o = grayStack.back();
        grayStack.pop_back();
        blacken(o);
    }
}

static void forgetClass(std::unordered_map<uint64_t, ObjClass*>& classById, Obj* o) {
    if (o->type != OT::Class) return;
    auto it = classById.find(static_cast<ObjClass*>(o)->id);
    if (it != classById.end() && it->second == o) classById.erase(it);
}

// End of a memory region: everything allocated since `marker` (logged after it)
// that nothing references any more is freed now. Reachable objects simply stay,
// so nothing can dangle.
void VM::regionCollect(Obj* marker) {
    markRoots();
    traceGray();
    size_t at = regionLog.size();
    while (at > 0 && regionLog[at - 1] != marker) at--;
    size_t freed = 0;
    std::vector<Obj*> deadLarge;
    if (at > 0) {
        for (size_t k = at; k < regionLog.size(); k++) {
            Obj* o = regionLog[k];
            if (o->marked) continue;
            freed += o->size;
            forgetClass(classById, o);
            regionLog[k] = nullptr;
            if (o->sizeClass == 255) deadLarge.push_back(o);  // leaves the large list first
            else freeObj(o);
        }
        if (!deadLarge.empty()) {
            std::unordered_set<Obj*> dead(deadLarge.begin(), deadLarge.end());
            largeObjects.erase(std::remove_if(largeObjects.begin(), largeObjects.end(),
                                              [&](Obj* o) { return dead.count(o) > 0; }),
                               largeObjects.end());
            for (Obj* o : deadLarge) freeObj(o);
        }
        // the survivors stay in the log for an enclosing region; the marker leaves it
        regionLog.erase(regionLog.begin() + (std::ptrdiff_t)(at - 1));
        regionLog.erase(std::remove(regionLog.begin(), regionLog.end(), nullptr), regionLog.end());
    }
    if (regionsOpen == 0) regionLog.clear();
    pool.each([](Obj* o) { o->marked = false; });
    for (Obj* o : largeObjects) o->marked = false;
    bytesAllocated = bytesAllocated > freed ? bytesAllocated - freed : 0;
}

bool VM::collectNow() {
    if (!gcEnabled()) return false;
    collectGarbage();
    return true;
}

void VM::noteAlloc(Obj* o) { memProfiler->onAlloc(*this, (int)o->type, o->size); }
void VM::noteGrow(Obj* o, size_t extra) { memProfiler->onAlloc(*this, (int)o->type, extra, false); }

std::string VM::typeNameOf(OT t) {
    switch (t) {
        case OT::String: return "str";
        case OT::List: return "list";
        case OT::Tuple: return "tuple";
        case OT::Map: return "map";
        case OT::Set: return "set";
        case OT::Function: return "function code";
        case OT::Closure: return "fn";
        case OT::Upvalue: return "captured variable";
        case OT::Native: return "built-in fn";
        case OT::Bound: return "bound method";
        case OT::Class: return "class";
        case OT::Instance: return "instance";
        case OT::Range: return "range";
        case OT::Error: return "Error";
        case OT::Channel: return "flow";
        case OT::Task: return "task";
        case OT::Module: return "module";
        case OT::Namespace: return "namespace";
        case OT::Iterator: return "iterator";
        case OT::Vec: return "vec";
        case OT::Mat: return "mat";
        case OT::Quat: return "quat";
        case OT::Rng: return "rng";
        case OT::GpuBuffer: return "gpu buffer";
        case OT::Time: return "time";
        case OT::TypeDesc: return "type";
        case OT::Marker: return "region marker";
        case OT::Free: return "free slot";
        case OT::Interface: return "interface";
    }
    return "object";
}

void VM::collectGarbage() {
    if (memProfiler) memProfiler->gcStart(*this);
    struct GcEnd {
        VM& vm;
        ~GcEnd() {
            if (vm.memProfiler) vm.memProfiler->gcEnd(vm);
        }
    } gcEnd{*this};
    gcCount++;
    // IXO_GC_STATS=1: how long each collection marks and sweeps
    static const bool stats = std::getenv("IXO_GC_STATS") != nullptr;
    double t0 = stats ? monotonicSeconds() : 0;
    markRoots();
    traceGray();
    double t1 = stats ? monotonicSeconds() : 0;
    // an open region keeps only what survives in its log
    if (!regionLog.empty())
        regionLog.erase(std::remove_if(regionLog.begin(), regionLog.end(), [](Obj* o) { return !o->marked; }),
                        regionLog.end());
    size_t live = 0, kept = 0, freed = 0;
    // the pools, chunk by chunk
    pool.each([&](Obj* o) {
        if (o->marked) {
            o->marked = false;
            live += o->size;
            kept++;
        } else {
            forgetClass(classById, o);
            freeObj(o);
            freed++;
        }
    });
    // and the big objects
    size_t w = 0;
    for (size_t r = 0; r < largeObjects.size(); r++) {
        Obj* o = largeObjects[r];
        if (o->marked) {
            o->marked = false;
            live += o->size;
            kept++;
            largeObjects[w++] = o;
        } else {
            forgetClass(classById, o);
            freeObj(o);
            freed++;
        }
    }
    largeObjects.resize(w);
    bytesAllocated = live;
    // The next collection when the heap has doubled. When this one freed little
    // (the program is building up data), wait longer: marking the same live
    // objects again would be wasted work. A big heap (over 32 MB live) grows by
    // half instead, so garbage doesn't pile up to the size of the live data.
    size_t growth = live > ((size_t)32 << 20)
                        ? (live > ((size_t)64 << 20) ? live / 3 : live / 2)
                        : (freed * 4 < kept ? std::min<size_t>(live * 3, live + ((size_t)64 << 20)) : live);
    nextGC = std::max<size_t>(live + std::min<size_t>(growth, (size_t)1 << 30), 4 * 1024 * 1024);
    if (stats)
        std::fprintf(stderr, "gc %zu: marked %zu objects in %.1f ms, swept %zu in %.1f ms, %.1f MB live\n", gcCount,
                     kept, (t1 - t0) * 1000, freed, (monotonicSeconds() - t1) * 1000, live / 1048576.0);
}

}  // namespace ixo
