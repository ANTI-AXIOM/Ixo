// Sampling profiler for `ixo run --profile`.
//
// A timer thread raises a flag about once a millisecond; the interpreter
// checks it before every instruction (through ProfileHook) and records the
// current call stack, weighted by the real time since the previous sample.
// Time spent inside natives is charged to the Ixo line that called them.
// While machine code (the JIT) runs, the timer thread instead reads the
// thread's instruction address, which maps back to the function and line.
// par workers have their own profilers, merged into the main one at the end.
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "debugger.hpp"

namespace ixo {

class VM;
struct Fiber;
struct ObjFunction;

class Profiler {
public:
    std::atomic<bool> tick{false};
    std::atomic<int> inJit{0};  // the VM thread is running machine code

    // machineCode: also sample the JIT's code (only for the thread calling start()).
    void start(bool machineCode = false);
    void stop();
    // From the timer thread or a signal handler: the instruction address, the
    // link register (ARM64, else 0) and the stack pointer of the sampled thread.
    void pushPc(uintptr_t pc, uintptr_t lr, uintptr_t sp, float us);
    float pendingUs() const { return pendingUs_.load(std::memory_order_relaxed); }
    void resync() { last = Clock::now(); }  // time before this isn't Ixo code running (idle, scheduler)
    void sample(VM& vm, Fiber* f);
    std::string report(size_t topN = 15) const;
    std::string folded() const;  // "outer;inner weight" lines for flame graph tools
    void merge(const Profiler& other);  // a par worker's samples
    int workersMerged = 0;

private:
    using Clock = std::chrono::steady_clock;
    struct FnStats {
        std::string name, where;
        double self = 0, total = 0;
    };
    // stack: outermost first
    void record(const std::vector<ObjFunction*>& stack, const std::string& line, double w);
    double drainPcs(const std::vector<ObjFunction*>& outer);  // returns the time it charged
    void capturePc(float us);
    bool machineCode = false;
    // pcs[0] is where the thread was; the others are return addresses into
    // machine code found on its stack (when it was inside a helper).
    static constexpr int kPcs = 6;
    struct PcSample {
        uintptr_t pcs[kPcs];
        float us;
    };
    static constexpr uint32_t kRing = 1 << 14;
    std::vector<PcSample> ring = std::vector<PcSample>(kRing);
    std::atomic<uint32_t> ringHead{0}, ringTail{0};
    std::atomic<float> pendingUs_{0};
    void* thread = nullptr;  // the sampled thread (a HANDLE on Windows)
    uintptr_t threadId = 0;  // the sampled thread (a pthread_t elsewhere)
    uintptr_t stackTop = 0;  // its stack's end: scans stop there
    Clock::time_point last, began;
    std::thread timer;
    std::atomic<bool> stopping{false};
    std::unordered_map<ObjFunction*, FnStats> fns;
    std::map<std::string, double> lines;          // "file:line" -> self time
    std::map<std::string, double> stacks;         // folded stacks -> time
    double sampled = 0;                           // microseconds
    long samples = 0;
    double wall = 0;
};

// Lets the interpreter take samples before any instruction (not only at
// safe points), so time lands on the line that spends it.
class ProfileHook : public Debugger {
public:
    void check(VM& vm, Fiber* f) override;
    void onPanic(VM&, Fiber*, const std::string&) override {}
    bool allowsJit() const override { return true; }
};

// Memory profiler for `ixo run --memprofile`: samples where memory is
// allocated (about every 64 KB), counts allocations by type, times the
// garbage collector, and reports what is still alive at the end.
class MemProfiler {
public:
    void onAlloc(VM& vm, int type, size_t bytes, bool counts = true);  // counts: a new object
    void gcStart(VM& vm);
    void gcEnd(VM& vm);
    std::string report(VM& vm, size_t topN = 12);

private:
    size_t sinceSample = 0;
    std::map<std::string, double> sites;  // "fn (file:line)" -> bytes
    std::map<int, std::pair<double, long>> byType;  // bytes, count
    double total = 0;
    size_t peak = 0;
    long gcs = 0;
    double gcSeconds = 0;
    std::chrono::steady_clock::time_point gcBegan;
};

}  // namespace ixo
