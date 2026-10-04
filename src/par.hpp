// Parallel tasks (`par`). Every worker thread runs its own VM with its own
// heap and garbage collector, so Ixo objects are never shared between
// threads. Values cross heaps as Packets (deep copies). Compiled code and
// constants are immutable and shared. Flows and tasks that are used across
// heaps get a mutex-protected shared core (SharedChan / SharedTask).
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "object.hpp"
#include "profiler.hpp"

namespace ixo {

class VM;
class Program;
struct Fiber;

// ------------------------------------------------------------------ packets

struct PValue {
    VT t = VT::Nil;
    union {
        bool b;
        int64_t i;
        double f;
        uint32_t node;
    };
    PValue() : i(0) {}
};

struct PNode {
    OT type = OT::String;
    std::string s;                              // string, error kind, native path, names
    std::string s2;                             // error message
    std::vector<PValue> items;                  // list/tuple/set items, instance fields, upvalues, bound (recv, method)
    std::vector<std::pair<PValue, PValue>> pairs;  // map entries
    std::vector<std::pair<int, PValue>> methods;   // class methods (symbol -> closure)
    std::vector<std::pair<int, bool>> methodPublic;
    std::vector<double> nums;                   // vec/mat/quat/time numbers
    std::vector<std::string> strs;              // class field names
    std::vector<int> ints;                      // class positional fields
    int64_t n0 = 0, n1 = 0, n2 = 0;
    bool flag = false;
    uint64_t id = 0;                            // class id
    const void* ptr = nullptr;                  // closures: the shared, immutable ObjFunction; frozen values
    bool frozen = false;                        // a frozen value: shared by pointer, not copied
    ModuleRt* module = nullptr;                 // module objects: the original module
    std::shared_ptr<void> shared;               // SharedChan / SharedTask / rng state
};

struct ModMeta {
    ModuleRt* original;
    std::string name, path;
    std::vector<std::string> globalNames;
    std::unordered_map<std::string, int> exports;
};

struct Packet {
    std::vector<PNode> nodes;
    std::vector<PValue> roots;
    struct Global {
        ModuleRt* module;  // original module
        uint16_t index;
        PValue value;
    };
    std::vector<Global> globals;   // module variables the code in this packet may use
    std::vector<ModMeta> modules;  // metadata for modules mentioned above
};

// Encodes values from `vm`'s heap. With `globals`, closures bring along the
// module variables their code uses (a snapshot).
Packet encodeValues(VM& vm, const std::vector<Value>& roots, bool globals);
// Decodes into `vm`'s heap, in the context of fiber `f` (its module snapshot).
std::vector<Value> decodeValues(VM& vm, Fiber* f, const Packet& p);

// ------------------------------------------------------------------ worlds

// The module snapshot seen by one par task (and the tasks it forks).
struct World {
    std::unordered_map<ModuleRt*, std::unique_ptr<ModuleRt>> modules;  // original -> local copy
};

// ------------------------------------------------------------------ shared flows and tasks

struct RemoteWaiter {
    VM* vm;
    uint64_t serial;  // the waiting fiber (Fiber::serial)
    bool flag;  // receivers: receiving for a loop (closed -> Done); tasks: push the result
    std::shared_ptr<SelectState> sel;  // receivers in a select
    int index = -1;
};

struct SharedChan {
    std::mutex m;
    std::deque<Packet> buf;
    int cap = 0;
    bool closed = false;
    std::deque<RemoteWaiter> recvWaiters;
    std::deque<std::pair<RemoteWaiter, Packet>> sendWaiters;
};

struct SharedTask {
    std::mutex m;
    int id = 0;
    bool done = false;
    Packet result;
    std::vector<RemoteWaiter> waiters;
};

// ------------------------------------------------------------------ messages & runtime

bool registerSharedSelect(VM& vm, ObjChannel* ch, const std::shared_ptr<SelectState>& sel, int index);
// Sends a value into a shared flow from a thread that isn't a VM (never blocks).
void externalSend(const std::shared_ptr<SharedChan>& sc, Packet pkt);

struct Message {
    enum Kind { Start, Wake } kind;
    uint64_t serial = 0;     // Wake: the fiber to resume (Fiber::serial)
    bool push = true;        // Wake: push the packet's value onto its stack
    int selectIndex = -1;    // Wake: part of a select; push (index, value)
    Packet packet;
    std::shared_ptr<SharedTask> task;  // Start
};

// Roughly how many bytes a packet copies (for the profilers' par line).
size_t packetBytes(const Packet& p);

class ParRuntime {
public:
    // What par copied: into tasks (arguments, closures, module variables) and back (results)
    std::atomic<uint64_t> tasksStarted{0}, bytesIn{0}, bytesOut{0};
    static ParRuntime* get();                      // null until the first par task
    static ParRuntime* start(VM& main, int workers);
    void post(VM* target, Message msg);            // thread-safe
    void submit(Message start);                    // pick a worker
    // Waits until `vm` has messages. Returns false for the main VM when every
    // VM is idle and nothing is pending (program done, or a deadlock).
    bool waitForWork(VM& vm, double timeoutSec);
    void shutdown();
    int workerCount() const { return (int)workers.size(); }

    std::mutex m;
    std::condition_variable cv;
    bool stopping = false;
    int busy = 0;      // VMs that aren't waiting for work
    int external = 0;  // outside sources of work (a listening server): the program isn't done while > 0
    void addExternal(int delta) {
        std::lock_guard<std::mutex> lk(m);
        external += delta;
        cv.notify_all();
    }

private:
    struct Worker {
        std::thread thread;
        VM* vm = nullptr;
        std::unique_ptr<Profiler> profiler;  // when the program runs with --profile
    };

public:
    // After shutdown(): the workers' profiles, to merge into the main one.
    std::vector<Profiler*> workerProfilers() {
        std::vector<Profiler*> out;
        for (auto& w : workers)
            if (w->profiler) out.push_back(w->profiler.get());
        return out;
    }

private:
    std::vector<std::unique_ptr<Worker>> workers;
    size_t nextWorker = 0;
    VM* mainVM = nullptr;
};

// Operations on flows / tasks that other heaps may use.
void makeChannelShared(VM& vm, ObjChannel* ch);
void sharedSend(VM& vm, ObjChannel* ch, Value v);
Value sharedRecv(VM& vm, ObjChannel* ch, bool forIter, bool* blocked);
void sharedClose(VM& vm, ObjChannel* ch);
bool sharedTaskDone(ObjTask* t);
Value sharedAwait(VM& vm, ObjTask* t, bool pushResult, bool* blocked);
void sharedComplete(VM& vm, ObjTask* t, Value result);

}  // namespace ixo
