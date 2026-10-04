#include "profiler.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "vm.hpp"

#include "jit.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#define IXO_PC_SIGNALS 1
#endif

namespace ixo {

namespace {

std::string baseName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

int lineOf(const CallFrame& fr) {
    const Chunk& c = fr.closure->fn->chunk;
    size_t off = fr.ip > c.code.data() ? (size_t)(fr.ip - c.code.data() - 1) : 0;
    return c.lineAt(off);
}

#ifdef IXO_PC_SIGNALS
Profiler* signalProfiler = nullptr;  // the one sampling machine code (the main VM's)

void onProfSignal(int, siginfo_t*, void* uc) {
    Profiler* p = signalProfiler;
    if (!p) return;
    auto* u = static_cast<ucontext_t*>(uc);
    uintptr_t pc = 0;
#if defined(__APPLE__) && defined(__x86_64__)
    pc = (uintptr_t)u->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(__aarch64__)
    pc = (uintptr_t)u->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__x86_64__)
    pc = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    pc = (uintptr_t)u->uc_mcontext.pc;
#else
    (void)u;
#endif
    uintptr_t lr = 0, sp = 0;
#if defined(__APPLE__) && defined(__x86_64__)
    sp = (uintptr_t)u->uc_mcontext->__ss.__rsp;
#elif defined(__APPLE__) && defined(__aarch64__)
    sp = (uintptr_t)u->uc_mcontext->__ss.__sp;
    lr = (uintptr_t)u->uc_mcontext->__ss.__lr;
#elif defined(__linux__) && defined(__x86_64__)
    sp = (uintptr_t)u->uc_mcontext.gregs[REG_RSP];
#elif defined(__linux__) && defined(__aarch64__)
    sp = (uintptr_t)u->uc_mcontext.sp;
    lr = (uintptr_t)u->uc_mcontext.regs[30];
#endif
    if (pc) p->pushPc(pc, lr, sp, p->pendingUs());
}
#endif

}  // namespace

void ProfileHook::check(VM& vm, Fiber* f) {
    if (vm.profiler && vm.profiler->tick.load(std::memory_order_relaxed)) vm.profiler->sample(vm, f);
}

void Profiler::pushPc(uintptr_t pc, uintptr_t lr, uintptr_t sp, float us) {
    uint32_t h = ringHead.load(std::memory_order_relaxed);
    if (h - ringTail.load(std::memory_order_acquire) >= kRing) return;  // full: drop it
    PcSample& s = ring[h % kRing];
    for (auto& p : s.pcs) p = 0;
    s.pcs[0] = pc;
    s.us = us;
    uintptr_t lo, hi;
    jitCodeBounds(&lo, &hi);
    if (pc < lo || pc >= hi) {
        // inside a helper: the nearest return addresses into machine code say who called it
        int n = 1;
        if (lr >= lo && lr < hi) s.pcs[n++] = lr;
        const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(sp);
        size_t room = sp && stackTop > sp ? (stackTop - sp) / sizeof(uintptr_t) : 0;
        for (size_t k = 0; k < room && k < 1024 && n < kPcs; k++)
            if (stack[k] >= lo && stack[k] < hi) s.pcs[n++] = stack[k] - 1;  // inside the call instruction
    }
    ringHead.store(h + 1, std::memory_order_release);
}

// On the timer thread: where is the VM thread's machine code right now?
void Profiler::capturePc(float us) {
#ifdef _WIN32
    HANDLE h = (HANDLE)thread;
    if (!h || SuspendThread(h) == (DWORD)-1) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(h, &c)) {
        // the thread stays stopped while its stack is read
#if defined(_M_ARM64) || defined(__aarch64__)
        pushPc((uintptr_t)c.Pc, (uintptr_t)c.Lr, (uintptr_t)c.Sp, us);
#else
        pushPc((uintptr_t)c.Rip, 0, (uintptr_t)c.Rsp, us);
#endif
    }
    ResumeThread(h);
#elif defined(IXO_PC_SIGNALS)
    pendingUs_.store(us, std::memory_order_relaxed);
    pthread_kill((pthread_t)threadId, SIGPROF);
#else
    (void)us;
#endif
}

void Profiler::start(bool wantMachineCode) {
    began = last = Clock::now();
    stopping = false;
    machineCode = wantMachineCode && jitSupported();
    if (machineCode) {
#ifdef _WIN32
        thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                            GetCurrentThreadId());
        if (!thread) machineCode = false;
        ULONG_PTR low = 0, high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        stackTop = (uintptr_t)high;
#elif defined(IXO_PC_SIGNALS)
        threadId = (uintptr_t)pthread_self();
#ifdef __APPLE__
        stackTop = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#else
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* addr = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &addr, &size) == 0) stackTop = (uintptr_t)addr + size;
            pthread_attr_destroy(&attr);
        }
#endif
        struct sigaction sa {};
        sa.sa_sigaction = onProfSignal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        signalProfiler = this;
#else
        machineCode = false;
#endif
    }
    timer = std::thread([this] {
        auto prev = Clock::now();
        auto onTick = [&] {
            auto now = Clock::now();
            float us = (float)std::chrono::duration<double, std::micro>(now - prev).count();
            prev = now;
            if (machineCode && inJit.load(std::memory_order_relaxed) > 0) capturePc(us);
            tick.store(true, std::memory_order_relaxed);
        };
#ifdef _WIN32
        // a high-resolution timer: plain Sleep(1) can take 15 ms
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        while (!stopping) {
            if (t) {
                LARGE_INTEGER due;
                due.QuadPart = -10000;  // 1 ms, relative
                SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(t, INFINITE);
            } else {
                Sleep(1);
            }
            onTick();
        }
        if (t) CloseHandle(t);
#else
        while (!stopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            onTick();
        }
#endif
    });
}

void Profiler::stop() {
    stopping = true;
    if (timer.joinable()) timer.join();
#ifdef _WIN32
    if (thread) CloseHandle((HANDLE)thread);
    thread = nullptr;
#elif defined(IXO_PC_SIGNALS)
    if (signalProfiler == this) signalProfiler = nullptr;
#endif
    drainPcs({});
    wall = std::chrono::duration<double, std::micro>(Clock::now() - began).count();
}

void Profiler::record(const std::vector<ObjFunction*>& stack, const std::string& line, double w) {
    if (stack.empty() || w <= 0) return;
    sampled += w;
    samples++;
    std::unordered_set<ObjFunction*> seen;
    std::string folded;
    for (ObjFunction* fn : stack) {
        auto [it, fresh] = fns.try_emplace(fn);
        if (fresh) {
            it->second.name = fn->name.empty() ? "<script>" : fn->name;
            int first = fn->chunk.lines.empty() ? 0 : fn->chunk.lines[0];
            it->second.where = baseName(fn->file) + ":" + std::to_string(first);
        }
        if (seen.insert(fn).second) it->second.total += w;  // recursion counts once
        if (!folded.empty()) folded += ';';
        folded += it->second.name;
    }
    fns[stack.back()].self += w;
    lines[line] += w;
    stacks[folded] += w;
}

// Charges the machine code samples taken since the last call; `outer` is the
// interpreter's stack (outermost first) that entered the machine code.
double Profiler::drainPcs(const std::vector<ObjFunction*>& outer) {
    double charged = 0;
    uint32_t t = ringTail.load(std::memory_order_relaxed), h = ringHead.load(std::memory_order_acquire);
    for (; t != h; t++) {
        const PcSample& s = ring[t % kRing];
        ObjFunction* fn = nullptr;
        uint32_t off = 0;
        bool found = false;
        for (int k = 0; k < kPcs && s.pcs[k] && !found; k++) found = jitFindPc(s.pcs[k], &fn, &off);
        if (!found) continue;  // not machine code after all: the interpreter's sample covers it
        std::vector<ObjFunction*> stack = outer;
        if (stack.empty() || stack.back() != fn) stack.push_back(fn);
        record(stack, baseName(fn->file) + ":" + std::to_string(fn->chunk.lineAt(off)), s.us);
        charged += s.us;
    }
    ringTail.store(t, std::memory_order_release);
    return charged;
}

void Profiler::sample(VM& vm, Fiber* f) {
    (void)vm;
    tick.store(false, std::memory_order_relaxed);
    auto now = Clock::now();
    double w = std::chrono::duration<double, std::micro>(now - last).count();
    last = now;
    // innermost first: the running fiber, then whoever called into it (callbacks)
    std::vector<const CallFrame*> frames;
    for (Fiber* x = f; x; x = x->caller)
        for (auto it = x->frames.rbegin(); it != x->frames.rend(); ++it) frames.push_back(&*it);
    std::vector<ObjFunction*> stack;
    for (size_t i = frames.size(); i-- > 0;) stack.push_back(frames[i]->closure->fn);
    if (machineCode) w -= drainPcs(stack);
    if (w <= 0 || frames.empty()) return;
    record(stack, baseName(frames[0]->closure->fn->file) + ":" + std::to_string(lineOf(*frames[0])), w);
}

std::string Profiler::report(size_t topN) const {
    char buf[512];
    std::string out;
    snprintf(buf, sizeof buf, "\n-- profile: %.1f ms running Ixo code (%.1f ms wall), %ld samples --\n", sampled / 1000,
             wall / 1000, samples);
    out += buf;
    if (workersMerged > 0) {
        snprintf(buf, sizeof buf, "   (includes %d par worker%s: time adds up across cores)\n", workersMerged,
                 workersMerged == 1 ? "" : "s");
        out += buf;
    }
    if (samples == 0) return out + "  (nothing sampled: the program ended too quickly)\n";
    std::vector<const FnStats*> v;
    for (auto& [fn, st] : fns) v.push_back(&st);
    std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->self != b->self ? a->self > b->self : a->total > b->total; });
    out += "   self   total   function\n";
    for (size_t i = 0; i < v.size() && i < topN; i++) {
        if (v[i]->self <= 0 && i >= 5) break;
        snprintf(buf, sizeof buf, "  %5.1f%%  %5.1f%%   %s  (%s)\n", 100 * v[i]->self / sampled,
                 100 * v[i]->total / sampled, v[i]->name.c_str(), v[i]->where.c_str());
        out += buf;
    }
    std::vector<std::pair<std::string, double>> ls(lines.begin(), lines.end());
    std::sort(ls.begin(), ls.end(), [](auto& a, auto& b) { return a.second > b.second; });
    out += "  hottest lines:\n";
    for (size_t i = 0; i < ls.size() && i < 8; i++) {
        snprintf(buf, sizeof buf, "  %5.1f%%   %s\n", 100 * ls[i].second / sampled, ls[i].first.c_str());
        out += buf;
    }
    return out;
}

void Profiler::merge(const Profiler& o) {
    for (auto& [fn, st] : o.fns) {
        auto [it, fresh] = fns.try_emplace(fn, st);
        if (!fresh) {
            it->second.self += st.self;
            it->second.total += st.total;
        }
    }
    for (auto& [k, v] : o.lines) lines[k] += v;
    for (auto& [k, v] : o.stacks) stacks[k] += v;
    sampled += o.sampled;
    samples += o.samples;
    workersMerged++;
}

std::string Profiler::folded() const {
    std::string out;
    for (auto& [stack, w] : stacks) out += stack + " " + std::to_string((long long)(w + 0.5)) + "\n";
    return out;
}

// ------------------------------------------------------------------ memory

void MemProfiler::onAlloc(VM& vm, int type, size_t bytes, bool counts) {
    total += (double)bytes;
    auto& t = byType[type];
    t.first += (double)bytes;
    if (counts) t.second++;
    if (vm.bytesAllocated > peak) peak = vm.bytesAllocated;
    sinceSample += bytes;
    if (sinceSample < 64 * 1024) return;
    std::string site = "(outside Ixo code)";
    if (vm.current && !vm.current->frames.empty()) {
        const CallFrame& fr = vm.current->frames.back();
        ObjFunction* fn = fr.closure->fn;
        const Chunk& c = fn->chunk;
        size_t off = fr.ip > c.code.data() ? (size_t)(fr.ip - c.code.data() - 1) : 0;
        site = (fn->name.empty() ? std::string("<script>") : fn->name) + "  (" + baseName(fn->file) + ":" +
               std::to_string(c.lineAt(off)) + ")";
    }
    sites[site] += (double)sinceSample;
    sinceSample = 0;
}

void MemProfiler::gcStart(VM& vm) {
    if (vm.bytesAllocated > peak) peak = vm.bytesAllocated;
    gcBegan = std::chrono::steady_clock::now();
}

void MemProfiler::gcEnd(VM&) {
    gcs++;
    gcSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - gcBegan).count();
}

static std::string human(double b) {
    char buf[32];
    if (b >= 1024.0 * 1024 * 1024) snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024.0 * 1024) snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(buf, sizeof buf, "%.1f KB", b / 1024);
    else snprintf(buf, sizeof buf, "%.0f B", b);
    return buf;
}

std::string MemProfiler::report(VM& vm, size_t topN) {
    char buf[512];
    std::string out;
    snprintf(buf, sizeof buf, "\n-- memory: %s allocated, peak heap %s, %ld collection%s (%.1f ms) --\n", human(total).c_str(),
             human((double)peak).c_str(), gcs, gcs == 1 ? "" : "s", gcSeconds * 1000);
    out += buf;
    std::vector<std::pair<std::string, double>> s(sites.begin(), sites.end());
    std::sort(s.begin(), s.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (!s.empty()) out += "  allocated most (sampled):\n";
    for (size_t i = 0; i < s.size() && i < topN; i++) {
        snprintf(buf, sizeof buf, "  %9s  %s\n", human(s[i].second).c_str(), s[i].first.c_str());
        out += buf;
    }
    std::vector<std::pair<int, std::pair<double, long>>> t(byType.begin(), byType.end());
    std::sort(t.begin(), t.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
    out += "  allocated by type:\n";
    for (size_t i = 0; i < t.size() && i < 8; i++) {
        snprintf(buf, sizeof buf, "  %9s  %8ld  %s\n", human(t[i].second.first).c_str(), t[i].second.second,
                 vm.typeNameOf((OT)t[i].first).c_str());
        out += buf;
    }
    // what is still alive now
    std::map<int, std::pair<double, long>> live;
    double liveTotal = 0;
    vm.eachObject([&](OT type, uint32_t size) {
        auto& l = live[(int)type];
        l.first += size;
        l.second++;
        liveTotal += size;
    });
    std::vector<std::pair<int, std::pair<double, long>>> lv(live.begin(), live.end());
    std::sort(lv.begin(), lv.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
    snprintf(buf, sizeof buf, "  alive at the end: %s\n", human(liveTotal).c_str());
    out += buf;
    for (size_t i = 0; i < lv.size() && i < 6; i++) {
        snprintf(buf, sizeof buf, "  %9s  %8ld  %s\n", human(lv[i].second.first).c_str(), lv[i].second.second,
                 vm.typeNameOf((OT)lv[i].first).c_str());
        out += buf;
    }
    return out;
}

}  // namespace ixo
