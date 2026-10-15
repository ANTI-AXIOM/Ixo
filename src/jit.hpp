// The JIT: hot functions and loops become machine code (x86-64 and ARM64).
//
// It compiles int and float arithmetic, comparisons, branches, loops, list
// indexing, fields, calls to other compiled functions and methods, and calls
// to built-ins through small helpers. The machine code works directly on the
// VM's stack slots, so its state is always the interpreter's state: when a
// check fails (an unexpected type, division by zero, a stack or recursion
// limit) or it meets an instruction it doesn't compile, it records where it
// was and the interpreter carries on from that exact instruction. Hot loops
// can also be entered in the middle of a call (on-stack replacement).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ixo {

class VM;
struct ObjFunction;
struct ObjClosure;
struct Value;

struct JitCtx {
    Value* stackEnd;      // end of the fiber's stack: recursive calls must fit below
    int64_t depth;        // nested native calls
    int64_t maxDepth;     // give up beyond this (the interpreter reports deep recursion)
    VM* vm;               // for allocating range iterators
    uint32_t bailOffset;  // on return 1: the bytecode offset to continue at
    uint32_t bailDepth;   //              and the stack depth (slots above the frame base)
    ObjClosure* bailClosure;  // the function whose machine code stopped (the innermost)
    Value* bailBase;          // and its frame
    // The functions in machine code that were waiting on the call that stopped,
    // innermost first. The VM rebuilds their frames, so the interpreter carries
    // on exactly where everything was (nothing runs twice).
    struct Waiting {
        ObjClosure* closure;
        Value* base;
        uint32_t resume;  // bytecode offset just after their CALL
    };
    Waiting* chain;
    int64_t chainLen, chainCap;
    ObjClosure* waitingSelf;  // set right before ixoJitWait
    // Loops count down; at zero the machine code returns to the interpreter so
    // other tasks get a turn (then the loop continues in machine code).
    int64_t budget;
    uint8_t budgetExit;  // the last stop was only for that
    uint8_t pausedExit;  // the task paused (it waits): not a failure of the machine code either
};

// One instruction of a compiled function, for the helpers that run it (or a
// call it makes) without leaving machine code.
struct JitOpSite {
    ObjClosure* self;       // the compiled function
    uint32_t off, next;     // bytecode offsets: the instruction, the one after
    int depth, fall, pops;  // stack depth before and after it; slots it consumes
    uint32_t aborts = 0;    // times it turned out to need an Ixo call (then it stops there)
};

// Helpers called from machine code answer with these codes.
enum JitHelperResult : int64_t {
    JIT_DONE = 0,     // carry on in machine code
    JIT_EXIT = 2,     // not done: the interpreter runs this instruction (or reports its panic)
    JIT_STEP = 3,     // not a call machine code makes: run the instruction in the interpreter
    JIT_PAUSED = 4,   // done, but the task paused (it waits, or others get a turn): ctx says where
};

// 0: returned (result in base[0]); 1: stopped early (see bailOffset / bailDepth).
// `entry` 0 starts the function; k > 0 enters at the k-th loop header.
using JitFn = int (*)(Value* base, JitCtx* ctx, uint64_t entry);

struct JitCode {
    JitFn fn = nullptr;
    int maxDepth = 0;                      // stack slots a call needs
    std::vector<uint32_t> loopHeaders;     // bytecode offsets of the OSR entries (entry = index + 1)
    std::vector<std::shared_ptr<void>> keep;  // data the code points to (call sites)
    // For the profiler: (machine code offset, bytecode offset), by machine code offset.
    std::vector<std::pair<uint32_t, uint32_t>> pcMap;
    // The typed tier: values in registers, by types partly guessed from the
    // interpreter's profile. Wrong guesses hand over to the interpreter and are
    // counted; too many and the function is compiled again without guessing.
    bool tier2 = false;
    uint64_t guessFails = 0;
    // loops the interpreter never ran before this was compiled: the first time
    // in, they go back to it once, to learn their types (then are compiled again)
    std::unique_ptr<uint8_t[]> learned;
    uint8_t relearn = 0;  // set when one did: compile again at the next loop entry
    void* mem = nullptr;  // its machine code (freed with jitFreeCode once no one runs it)
    size_t bytes = 0;
};

bool jitSupported();  // this CPU/OS can run the generated code
// Compiles fn (whose closure is `self`, for recursive calls). False with a
// reason when the function uses something the JIT doesn't handle.
// `seeds`: loop headers (bytecode offset, stack depth) to compile from as well.
bool jitCompile(ObjFunction* fn, ObjClosure* self, JitCode& out, std::string* why,
                const std::vector<std::pair<uint32_t, int>>& seeds = {}, VM* vm = nullptr);
// Can this function be compiled at all (ignoring runtime facts)? "" if so.
std::string jitCheck(ObjFunction* fn);
// The compiler thread: compiles from copies of what the program changes as it runs
// (the type profile, the inline caches, the loop entries), taken when it asked.
struct InlineCache;
bool jitCompileFrom(ObjFunction* fn, ObjClosure* self, JitCode& out, std::string* why,
                    const std::vector<std::pair<uint32_t, int>>& seeds, VM* vm, const std::vector<uint8_t>* profile,
                    const std::vector<InlineCache>* ics, bool noTier2);
void* jitAllocExec(const std::vector<uint8_t>& code);  // executable memory, for the program's life (kernels)
void jitFreeCode(JitCode* code);  // code no one runs any more: its memory is reused, then the object deleted
// The compiled function and bytecode offset at a machine code address (for the
// profiler, on the VM's thread). False if the address isn't compiled code.
bool jitFindPc(uintptr_t pc, ObjFunction** fn, uint32_t* offset);
// The lowest and highest address of any machine code (a quick first check).
void jitCodeBounds(uintptr_t* lo, uintptr_t* hi);

}  // namespace ixo
