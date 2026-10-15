// The virtual machine: a stack-based bytecode interpreter with cooperative
// lightweight tasks (fibers), channels ("flows"), and a mark-and-sweep GC
// that only runs at safe points in the dispatch loop.
#pragma once

#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <queue>
#include <new>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jit.hpp"
#include "object.hpp"
#include "opcodes.hpp"
#include "par.hpp"

namespace ixo {

enum class FState : uint8_t { Ready, Running, Blocked, Sleeping, Done };

struct CallFrame {
    ObjClosure* closure;
    const uint8_t* ip;
    size_t base;        // stack index of slot 0
    ModuleRt* module;   // where the function's module variables live for this task
};

struct Fiber {
    int id = 0;
    Fiber* caller = nullptr;  // callbacks (callNested): the fiber whose native called us
    std::shared_ptr<SelectState> selectSel;  // sleeping in a select with a timeout
    int selectTimeoutIndex = -1;
    // A deque: pushing a frame never moves existing ones, so the dispatch loop
    // can keep a pointer to the current frame across nested calls from natives.
    std::deque<CallFrame> frames;
    Value* stack = nullptr;
    size_t sp = 0, cap = 0;
    ObjUpvalue* openUpvalues = nullptr;
    FState state = FState::Ready;
    ObjTask* task = nullptr;
    std::vector<std::vector<ObjTask*>> groups;  // `with tasks` blocks
    double wakeAt = 0;
    int startArgc = -1;  // >= 0: first run calls stack[0] with this many arguments
    int nativeDepth = 0;
    size_t index = 0;        // its position in VM::fibers (removal in O(1))
    uint32_t queued = 0;     // times it is in the run queue
    uint64_t sleepGen = 0;   // bumped when it starts or stops sleeping (stale heap entries)
    bool asleep = false;
    int jitDepth = 2;   // nested machine-code calls its stack has room for (grows x4 when reached)
    bool isMain = false;
    uint64_t serial = 0;  // unique id, used to find the fiber from other threads
    std::shared_ptr<World> world;  // par tasks: module snapshot (null: the real modules)
    std::vector<std::pair<Obj*, uint64_t>> regions;  // open memory regions: (marker, totalAllocated at start)

    Fiber();
    ~Fiber();
    Fiber(const Fiber&) = delete;
    void push(Value v) {
        if (sp == cap) grow();
        stack[sp++] = v;
    }
    void grow();
};

struct PanicError {
    std::string msg;
};

// Thrown when an instruction run for machine code (VM::jitStep) needs to call
// a function written in Ixo: the machine code then stops before it instead.
struct JitStepAbort {};

struct BuiltinInfo {
    std::string name;
    Value value;
    std::string sig, doc;
};

struct MethodInfo {
    Value fn;
    std::string sig, doc;
};

struct StackEntry {
    std::string function, file;
    int line;
};

struct WarmProfile {
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int>>> fns;  // key -> loop entry points
    std::string savePath;  // empty: read-only (a built executable)
    uint64_t hits = 0;
};
uint64_t warmKey(const ObjFunction* fn);
// The JIT's thresholds: a function compiles after this many calls, a loop after
// this many turns (the counter is shared: calls and turns both add one).
constexpr uint32_t kJitCalls = 200, kJitTurns = 2000;
int runCacheCommand(const std::vector<std::string>& args);

class VM {
public:
    VM();
    ~VM();

    // ---------------------------------------------------------- memory
    // Small objects come from free lists of 8-byte size classes (no malloc
    // for most allocations); bigger ones from malloc.
    // The pools are also how the collector finds every object: it walks each
    // chunk slot by slot (every slot starts with a header saying its size).
    struct ObjPool {
        static constexpr size_t kGrain = 8, kClasses = 64, kChunk = 256 * 1024;
        void* freeList[kClasses + 1] = {};  // empty slots: type Free, the next one at offset 8
        char* bump = nullptr;
        char* bumpEnd = nullptr;
        struct Chunk {
            char* start;
            char* used;  // how far slots were handed out (the last chunk: bump)
        };
        std::vector<Chunk> chunks;
        static void* nextFree(void* p) {
            void* n;
            std::memcpy(&n, static_cast<char*>(p) + 8, sizeof n);
            return n;
        }
        void* get(uint8_t c) {
            if (void* p = freeList[c]) {
                freeList[c] = nextFree(p);
                return p;
            }
            size_t n = (size_t)c * kGrain;
            if ((size_t)(bumpEnd - bump) < n) {
                if (!chunks.empty()) chunks.back().used = bump;
                bump = static_cast<char*>(std::malloc(kChunk));
                if (!bump) throw std::bad_alloc();
                chunks.push_back({bump, bump});
                bumpEnd = bump + kChunk;
            }
            void* p = bump;
            bump += n;
            return p;
        }
        void put(void* p, uint8_t c) {
            auto* o = static_cast<Obj*>(p);
            o->type = OT::Free;
            o->marked = false;
            o->sizeClass = c;
            void* n = freeList[c];
            std::memcpy(static_cast<char*>(p) + 8, &n, sizeof n);
            freeList[c] = p;
        }
        // Calls f on every object in the pool (f may free it).
        template <class F>
        void each(F f) {
            for (size_t k = 0; k < chunks.size(); k++) {
                char* end = k + 1 == chunks.size() ? bump : chunks[k].used;
                for (char* p = chunks[k].start; p < end;) {
                    Obj* o = reinterpret_cast<Obj*>(p);
                    p += (size_t)o->sizeClass * kGrain;
                    if (o->type != OT::Free) f(o);
                }
            }
        }
        ~ObjPool() {
            for (auto& c : chunks) std::free(c.start);
        }
    };
    ObjPool pool;
    std::vector<Obj*> largeObjects;  // the ones too big for the pools
    // While a memory region (@arena) is open: what was allocated, in order (the
    // region's marker first), so its end can free what it made.
    std::vector<Obj*> regionLog;
    int regionsOpen = 0;
    void* rawAlloc(size_t bytes, uint8_t& sizeClass) {
        if (bytes <= ObjPool::kGrain * ObjPool::kClasses) {
            sizeClass = (uint8_t)((bytes + ObjPool::kGrain - 1) / ObjPool::kGrain);
            return pool.get(sizeClass);
        }
        sizeClass = 255;
        void* p = std::malloc(bytes);
        if (!p) throw std::bad_alloc();
        return p;
    }
    template <class T>
    T* track(T* o, size_t bytes) {
        o->heapId = heapId;
        if (o->sizeClass == 255) largeObjects.push_back(o);
        if (regionsOpen) regionLog.push_back(o);
        o->size = (uint32_t)(bytes + 16);
        bytesAllocated += o->size;
        totalAllocated += o->size;
        if (memProfiler) noteAlloc(o);
        return o;
    }
    // memory from rawAlloc goes back if a constructor throws (the pools must stay walkable)
    void unAlloc(void* mem, uint8_t c) {
        if (c == 255) std::free(mem);
        else pool.put(mem, c);
    }
    template <class T, class... A>
    T* alloc(A&&... args) {
        uint8_t c;
        void* mem = rawAlloc(sizeof(T), c);
        T* o;
        try {
            o = new (mem) T(std::forward<A>(args)...);
        } catch (...) {
            unAlloc(mem, c);
            throw;
        }
        o->sizeClass = c;
        return track(o, sizeof(T));
    }
    ObjTuple* newTupleN(size_t n) {  // n nils, to fill in
        size_t bytes = sizeof(ObjTuple) + n * sizeof(Value);
        uint8_t c;
        void* mem = rawAlloc(bytes, c);
        auto* t = tupleInit(mem, n);
        t->sizeClass = c;
        return track(t, bytes);
    }
    ObjInstance* newInstance(ObjClass* cls) {
        size_t n = cls->fields.size();
        size_t bytes = sizeof(ObjInstance) + n * sizeof(Value);
        uint8_t c;
        void* mem = rawAlloc(bytes, c);
        auto* o = new (mem) ObjInstance(cls, n);
        o->sizeClass = c;
        return track(o, bytes);
    }
    void freeObj(Obj* o) {
        uint8_t c = o->sizeClass;
        destroyObj(o);
        if (c == 0) ::operator delete(o);  // made with new (frozen values; never collected)
        else unAlloc(o, c);
    }
    ObjString* str(std::string s) {
        size_t extra = s.size();
        ObjString* o = alloc<ObjString>(std::move(s));
        grow(o, extra);
        return o;
    }
    // An object took `extra` more bytes (its items, characters, fields).
    void grow(Obj* o, size_t extra) {
        o->size += (uint32_t)extra;
        bytesAllocated += extra;
        totalAllocated += extra;
        if (memProfiler) noteGrow(o, extra);
    }
    class MemProfiler* memProfiler = nullptr;
    void noteAlloc(Obj* o);
    void noteGrow(Obj* o, size_t extra);
    // Calls f(type, bytes) for every live object of this heap.
    template <class F>
    void eachObject(F f) {
        pool.each([&](Obj* o) { f(o->type, o->size); });
        for (Obj* o : largeObjects) f(o->type, o->size);
    }
    std::string typeNameOf(OT t);
    Value vstr(std::string s) { return Value::object(str(std::move(s))); }
    Value newList(std::vector<Value> items = {});
    Value newTuple(std::vector<Value> items);
    Value newError(const std::string& kind, const std::string& msg, Value payload = Value());
    // Machine code: compile c's function now if it's hot (no fiber needed). True if it has code.
    bool jitTry(ObjClosure* c);  // from machine code: compile c's function now (true if it has code)
    Value binaryOp(uint8_t op, Value a, Value b);  // arithmetic and comparisons (may panic)
    Value arrayOp(uint8_t op, Value a, Value b);   // numeric arrays, item by item (array.cpp)
    Value arrayIndex(Value arr, Value idx);        // xs[mask], xs[positions]
    void collectGarbage();
    bool collectNow();  // natives (gc()): collect if it's safe right now
    size_t bytesAllocated = 0;
    uint64_t totalAllocated = 0;  // never decreases: used to size memory regions
    size_t nextGC = 4 * 1024 * 1024;  // the first collection (then paced by the live size)
    bool gcStress = false;
    size_t gcCount = 0;

    // ---------------------------------------------------------- symbols
    // Symbols are shared by every VM in the process (bytecode compiled by one VM
    // runs in par workers too).
    static int sym(const std::string& s);
    static const std::string& symName(int id);

    // ---------------------------------------------------------- builtins
    int defineBuiltin(const std::string& name, Value v, const std::string& sig = "", const std::string& doc = "");
    Value defineNative(const std::string& name, NativeFn fn, int minA, int maxA, const std::string& sig = "",
                       const std::string& doc = "");
    Value makeNative(const std::string& name, NativeFn fn, int minA, int maxA);
    void defineMethod(Tag tag, const std::string& name, NativeFn fn, int minA, int maxA, const std::string& sig = "",
                      const std::string& doc = "");
    void defineMethodValue(Tag tag, const std::string& name, Value fn, const std::string& sig, const std::string& doc);
    ObjNamespace* defineNamespace(const std::string& name, const std::string& doc = "");
    void nsFunc(ObjNamespace* ns, const std::string& name, NativeFn fn, int minA, int maxA,
                const std::string& sig = "", const std::string& doc = "");
    void nsValue(ObjNamespace* ns, const std::string& name, Value v, const std::string& sig = "",
                 const std::string& doc = "");
    int builtinIndex(const std::string& name) const {
        auto it = builtinMap.find(name);
        return it == builtinMap.end() ? -1 : it->second;
    }
    std::vector<BuiltinInfo> builtins;
    std::unordered_map<std::string, int> builtinMap;
    std::unordered_map<int, MethodInfo> methods[(int)Tag::COUNT];
    // Signatures / docs of namespace members, keyed "fs.read" (checker, LSP).
    std::unordered_map<std::string, std::pair<std::string, std::string>> memberDocs;
    std::vector<std::string> nativeModules;  // namespaces importable with `use`

    // ---------------------------------------------------------- modules
    ModuleRt* newModule(const std::string& name, const std::string& path);
    std::vector<std::unique_ptr<ModuleRt>> modules;
    int moduleIndex(ModuleRt* m) const;

    // ---------------------------------------------------------- execution
    // Runs a module's top-level code (and everything it forks) to completion.
    // Returns false if the program panicked.
    bool runModule(ModuleRt* m);
    // Calls a function from native code (sort comparators, str()); the callee
    // may not block on flows or tasks.
    Value callNested(Value callee, const Value* args, int argc);
    // a task that has already finished with this result (gpu launches: they run in order)
    ObjTask* finishedTask(Value result = Value()) {
        auto* t = alloc<ObjTask>(nextTaskId++);
        t->done = true;
        t->result = result;
        return t;
    }
    // Runs one @test function in a fresh task; returns an error message or "".
    std::string runTest(Value fn);
    [[noreturn]] void panic(const std::string& msg);

    std::string lastPanic;
    std::vector<StackEntry> lastTrace;
    std::function<void(const std::string& msg, const std::vector<StackEntry>& trace)> onPanic;

    // ---------------------------------------------------------- helpers for natives
    std::string toStr(Value v);  // display form: strings unquoted
    std::string repr(Value v);   // strings quoted
    bool equals(Value a, Value b);
    int compare(Value a, Value b);  // -1/0/1, panics if not comparable
    bool truthy(Value v) const {
        switch (v.t) {
            case VT::Nil: return false;
            case VT::Bool: return v.b;
            case VT::Obj: return v.o->type != OT::Error;
            default: return true;
        }
    }
    std::string typeName(Value v);
    Tag tagOf(Value v);
    int64_t argInt(Value* args, int i, const char* fname);
    double argNum(Value* args, int i, const char* fname);
    ObjString* argStr(Value* args, int i, const char* fname);
    ObjList* argList(Value* args, int i, const char* fname);
    bool isCallable(Value v) const;
    int minArityOf(Value v) const;   // -1 if not callable
    Value getMethod(Value receiver, const std::string& name);  // bound method or nil
    // Calls an operator method ("+", "==", "[]", ...) of an instance, if its class has one.
    bool callOperator(Value self, const char* op, const Value* args, int argc, Value& out);
    // Panics if `method` would change a frozen receiver.
    void checkFrozen(Value receiver, int method);
    Value freeze(Value v);
    // Reorders [callee, args...] on the stack so the last `names.size()` args land at
    // their parameters' positions. Returns the new argument count.
    int placeNamedArgs(Fiber* f, int argc, const std::vector<std::string>& names);
    bool implements(Value v, ObjInterface* iface);
    bool matchValue(Value v, Value pattern);

    // ---------------------------------------------------------- tasks
    Fiber* current = nullptr;
    void blockCurrent();                         // park the current fiber
    void wake(Fiber* f, Value result, bool pushResult);
    void sleepCurrent(double ms, bool pushResult);
    ObjTask* spawn(Value callee, const Value* args, int argc);
    void awaitTask(ObjTask* t);  // natives: block until done (result pushed)
    void channelSend(ObjChannel* ch, Value v);   // may block
    Value channelRecv(ObjChannel* ch, bool forIter, bool* blocked);
    // select: (arm index, value) from the first flow with something to give
    Value selectRecv(const std::vector<ObjChannel*>& chans, double timeoutMs, bool noWait, bool* blocked);
    Value selectResult(int index, Value v);
    void channelClose(ObjChannel* ch);
    Value nativeData;
    ObjNative* currentNative = nullptr;

    // Runs `callee` with no arguments as the main task; false if it panicked.
    bool runCallable(Value callee, Value* result, bool isTest);
    Value lastResult;                 // value returned by the last runModule
    std::vector<Value> extraRoots;    // values the host keeps alive (REPL)
    std::vector<std::string> programArgs;
    int exitCode = 0;
    // `for a, b in list` sites that met pairs (told once each: the meaning changed in 0.6)
    std::unordered_set<uint64_t> pairLoopsTold;
    bool exitRequested = false;

    // ---------------------------------------------------------- par (parallel tasks)
    uint16_t heapId = 0;
    bool isWorker = false;
    class ParRuntime* ensurePar();  // starts the par worker threads if needed
    bool jitOn = false;  // IXO_JIT=0 turns the JIT off
    // The compiler thread (vm.cpp): machine code is made while the program runs on, and
    // installed at the next call or loop entry. IXO_JIT_SYNC=1: compiled on this thread.
    struct JitThread* jitThread = nullptr;
    bool jitAsync = false;
    int jitActive = 0;  // machine code running on this thread (nested calls count)
    std::vector<struct JitCode*> retiredCode;  // replaced code: freed once none of it runs
    void jitEnqueue(ObjFunction* fn, ObjClosure* c, const char* why, bool demote);
    void jitInstall();
    void jitStop();
    void jitRetire(struct JitCode* old);
    // ints wrap around; ixo test (and IXO_CHECK_OVERFLOW=1) stop at an overflow instead
    bool checkOverflow = false;
    bool noInline = false;  // debugging and profiling: calls stay calls (every function shows)
    [[noreturn]] void intOverflow(char op, int64_t x, int64_t y);
    int64_t checkedMul(int64_t x, int64_t y) {
        int64_t r;
        if (__builtin_mul_overflow(x, y, &r) && checkOverflow) intOverflow('*', x, y);
        return r;
    }
    // Machine code running one instruction (or a built-in) without stopping.
    int jitStep(JitCtx& ctx, Value* frameBase, JitOpSite& s);
    int jitFastOp(JitCtx& ctx, Value* frameBase, JitOpSite& s);  // common ones directly, else jitStep
    void strAppend(Value& local, Value x, bool plainAdd, uint8_t owner);  // STR_APPEND(_G): owner 1 / 2
    // Is this string ASCII? Computed once and remembered in its header.
    static bool ascii(const ObjString* s) {
        if (!s->kind) s->kind = isAsciiText(s->s) ? 1 : 2;
        return s->kind == 1;
    }
    static bool isAsciiText(const std::string& s) {
        for (unsigned char c : s)
            if (c >= 0x80) return false;
        return true;
    }
    // The shared one-character string for an ASCII byte (no allocation per character).
    Value charStr(unsigned char c) {
        if (!asciiChars[c]) {
            asciiChars[c] = str(std::string(1, (char)c));
            asciiChars[c]->kind = 1;
        }
        return Value::object(asciiChars[c]);
    }
    ObjString* asciiChars[128] = {};
    // A module variable's value is being read: a string it grew in place is now shared.
    static Value sealed(Value v) {
        if (v.t == VT::Obj && v.o->type == OT::String && static_cast<ObjString*>(v.o)->appendable)
            seal(static_cast<ObjString*>(v.o));
        return v;
    }
    // No more growing in place: give back most of the spare room growing left.
    static void seal(ObjString* s) {
        if (!s->appendable) return;
        s->appendable = 0;
        if (s->s.capacity() > 64 && s->s.capacity() > s->s.size() + s->s.size() / 4) s->s.shrink_to_fit();
    }
    void fstrAdd(Value builder, Value v, uint16_t spec, const Value* consts);  // FSTR_ADD
    int jitNative(JitCtx& ctx, Value* frameBase, const JitOpSite& s, ObjNative* n, Value* result, Value* args, int total);
    // While jitStep runs: the fiber, its frame count and where the instruction ends.
    Fiber* stepFiber = nullptr;
    size_t stepFrames = 0;
    const uint8_t* stepStop = nullptr;
    class Debugger* stepHook = nullptr;
    void stepGuard(Fiber* f) {  // an Ixo call while stepping: give up (nothing has changed yet)
        if (f == stepFiber && f->frames.size() >= stepFrames) throw JitStepAbort{};
    }
    std::exception_ptr jitPanic;  // a panic inside a helper, raised once the frames are back
    // Warm start (warm.cpp): functions that got hot in earlier runs compile at
    // after a few calls. warmApply looks a function up once and lowers its threshold.
    std::shared_ptr<struct WarmProfile> warm;
    void warmLoad(const std::string& mainPath);
    void warmApply(ObjFunction* fn);
    void warmSave();
    // IXO_JIT_STATS=1: where machine code hands over to the interpreter, and why.
    struct JitStats* jitStats = nullptr;
    void noteJitGiveUp(ObjFunction* fn, const char* what);  // calls or loops stay interpreted now
    void printJitStats();
    // Runs a compiled function (entry 0) or enters it at a loop header. True when
    // it returned (result in stack[base]); else ctx says where to continue.
    bool runJit(Fiber* f, ObjFunction* fn, size_t base, uint64_t entry, struct JitCtx& ctx);
    // Compiles when hot; true if compiled and usable here. A loop header (seedAt,
    // seedDepth) not yet in the code is added as an entry point (recompiling).
    bool jitReady(Fiber* f, ObjClosure* c, int64_t seedAt = -1, int seedDepth = 0);
    // Typed machine code whose guesses keep failing: compiled again the plain way.
    void jitDemote(ObjFunction* fn, ObjClosure* c);
    static void jitAllowDirect(ObjFunction* fn) {
        fn->jitDirect = fn->jitState == 2 && fn->jit && !fn->isFork && !fn->isPar &&
                        !(fn->upvalueCount > 0 && fn->methodStyle);
    }
    bool jitShouldDemote(const ObjFunction* fn) const;
    ParRuntime* par = nullptr;
    std::deque<Message> inbox;  // guarded by par->m
    bool idle = false;          // guarded by par->m
    ObjRng* defaultRng = nullptr;
    std::unordered_map<uint64_t, ObjClass*> classById;  // classes known in this heap (weak)
    ModuleRt* localModule(Fiber* f, ModuleRt* m);
    ObjTask* parSpawn(Value callee, const Value* args, int argc);
    void drainInbox();
    void workerLoop();          // par workers: run tasks until the runtime stops
    Fiber* newFiber();
    Fiber* fiberFor(uint64_t serial);
    std::deque<Fiber*> runQueue;
    std::unordered_map<uint64_t, Fiber*> fiberBySerial;
    void closeUpvalues(Fiber* f, size_t fromSlot);
    std::vector<StackEntry> captureTrace(Fiber* f);
    double startTime = 0;
    uint64_t instructionBudget = 0;  // for preemption
    uint64_t selectRotor = 0;
    uint64_t budgetLimit = 20000;    // safepoint slow path every N ticks
    class Profiler* profiler = nullptr;
    class Debugger* debugger = nullptr;  // called before every instruction when set

private:
    friend struct GcAccess;

    std::vector<Fiber*> fibers;          // all live fibers
    // Sleeping tasks: a min-heap by wake-up time. Entries name the fiber by serial and
    // generation, so a woken or finished fiber's old entries are simply skipped.
    struct SleepEntry {
        double at;
        uint64_t serial, gen;
        bool operator>(const SleepEntry& o) const { return at > o.at; }
    };
    std::priority_queue<SleepEntry, std::vector<SleepEntry>, std::greater<SleepEntry>> sleepHeap;
    size_t sleeping = 0;
    void sleepAdd(Fiber* f) {
        f->sleepGen++;
        f->asleep = true;
        sleeping++;
        sleepHeap.push({f->wakeAt, f->serial, f->sleepGen});
    }
    void sleepRemove(Fiber* f) {
        if (!f->asleep) return;
        f->asleep = false;
        f->sleepGen++;
        sleeping--;
    }
    Fiber* sleepTop();  // the next fiber to wake (dropping stale entries), or null
    void enqueue(Fiber* f) {
        f->queued++;
        runQueue.push_back(f);
    }
    Fiber* mainFiber = nullptr;
    Value mainResult;
    bool startingTask = false;
    std::vector<Fiber*> nestedPool;  // idle fibers for callNested
    int nextTaskId = 1;
    std::vector<Obj*> grayStack;

    // dispatch
    void execute(Fiber* f, size_t stopDepth);
    bool callValue(Fiber* f, Value callee, int argc);  // true if a new frame was pushed
    void callClosure(Fiber* f, ObjClosure* c, int argc, size_t base);
    bool invoke(Fiber* f, int symId, int argc, uint8_t fbKind, uint16_t fbIndex, ObjClosure* caller);
    void getMember(Fiber* f, int symId, uint8_t fbKind, uint16_t fbIndex, ObjClosure* caller, bool raw);
    // After machine code stopped early: rebuild the interpreter frames it was in
    // (the current top frame is the outermost) and set ip / sp.
    void resumeAfterJit(Fiber* f, JitCtx& ctx);
    std::vector<JitCtx::Waiting> jitChain;
    Value fallbackValue(Fiber* f, uint8_t kind, uint16_t index, ObjClosure* caller);
    ObjUpvalue* captureUpvalue(Fiber* f, size_t slot);
    Value getIndex(Value obj, Value idx);
    void setIndex(Value obj, Value idx, Value v);
    Value makeIterator(Value v, bool pairs, bool keyed = false);
    bool iterNext(Fiber* f, ObjIterator* it, Value& out);  // false if blocked
    // One step of a `for` over an iterator: 0 a value in out, 1 done, 2 waiting on a flow.
    int iterStep(Fiber* f, ObjIterator* it, Value& out);
    bool containsValue(Value container, Value v);
    bool checkType(Value& v, ObjTypeDesc* t);
    std::string formatValue(Value v, const std::string& spec);

    // scheduling
    void finishFiber(Fiber* f, Value result);
    void schedule();  // runs until nothing is runnable
    void destroyFiber(Fiber* f);

    // gc
    void markValue(Value v);
    void markObject(Obj* o);
    void markRoots();
    void traceGray();
    void regionCollect(Obj* marker);
    void blacken(Obj* o);
    void maybeCollect() {
        if (bytesAllocated > nextGC || gcStress) collectGarbage();
    }
    bool gcEnabled() const;

    std::string toStrDepth(Value v, bool quote, int depth);
    friend class Compiler;
    friend struct Decoder;
    friend struct Encoder;
    friend class ParRuntime;
};

void requireHashable(VM& vm, Value v);
Value makeRange(VM& vm, Value start, Value end, Value step, bool inclusive);
size_t utf8Length(const std::string& s);
bool isAscii(const std::string& s);

double nowSeconds();  // wall clock, seconds since epoch
double monotonicSeconds();

}  // namespace ixo
