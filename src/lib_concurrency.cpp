// Flows (channels) and tasks.
#include <algorithm>
#include <thread>

#include "builtins.hpp"

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

NATIVE(n_flow) {
    int64_t cap = n > 0 ? vm.argInt(a, 0, "flow") : 0;
    if (cap < 0) vm.panic("flow capacity can't be negative");
    return Value::object(vm.alloc<ObjChannel>((int)cap));
}

static ObjChannel* CH(Value v) { return as<ObjChannel>(v); }

NATIVE(f_send) {
    vm.channelSend(CH(a[0]), a[1]);
    return Value();
}
NATIVE(f_recv) {
    bool blocked = false;
    return vm.channelRecv(CH(a[0]), false, &blocked);
}
NATIVE(f_close) {
    vm.channelClose(CH(a[0]));
    return Value();
}
NATIVE(f_len) { return Value::integer((int64_t)CH(a[0])->buf.size()); }
NATIVE(f_is_closed) { return Value::boolean(CH(a[0])->closed); }

NATIVE(t_await) {
    auto* t = as<ObjTask>(a[0]);
    if (t->shared) {
        bool blocked = false;
        return sharedAwait(vm, t, true, &blocked);
    }
    if (t->done) return t->result;
    vm.awaitTask(t);
    return Value();
}
NATIVE(t_done) {
    auto* t = as<ObjTask>(a[0]);
    return Value::boolean(t->shared ? sharedTaskDone(t) : t->done);
}
NATIVE(t_result) {
    auto* t = as<ObjTask>(a[0]);
    if (t->shared) {
        if (!sharedTaskDone(t)) return Value();
        bool blocked = false;
        return sharedAwait(vm, t, true, &blocked);
    }
    return t->result;
}
NATIVE(n_cores) { return Value::integer((int64_t)std::max(1u, std::thread::hardware_concurrency())); }

NATIVE(n_sleep) {
    vm.sleepCurrent(vm.argNum(a, 0, "sleep"), true);
    return Value();
}

void registerConcurrency(VM& vm) {
    vm.defineNative("flow", n_flow, 0, 1, "fn(int?) -> flow",
                    "A flow (channel) between tasks: flow[int](10) is buffered, flow() is unbuffered.");
    vm.defineNative("cores", n_cores, 0, 0, "fn() -> int", "Number of CPU cores (how many par tasks can run at once).");
    vm.defineNative("sleep", n_sleep, 1, 1, "fn(num)", "Pauses the current task for n milliseconds (old spelling: write n.wait, as in 500ms.wait).");
    vm.defineMethod(Tag::Flow, "send", f_send, 1, 1, "fn(T)", "Sends a value (same as value :> flow).");
    vm.defineMethod(Tag::Flow, "recv", f_recv, 0, 0, "fn() -> T?", "Receives a value; nil once the flow is closed and empty.");
    vm.defineMethod(Tag::Flow, "close", f_close, 0, 0, "fn()", "Signals that no more values will be sent.");
    vm.defineMethod(Tag::Flow, "len", f_len, 0, 0, "fn() -> int", "Number of buffered values.");
    vm.defineMethod(Tag::Flow, "is_closed", f_is_closed, 0, 0, "fn() -> bool", "True after close.");
    vm.defineMethod(Tag::Task, "await", t_await, 0, 0, "fn() -> any", "Waits for the task and returns its result.");
    vm.defineMethod(Tag::Task, "done", t_done, 0, 0, "fn() -> bool", "True once the task has finished.");
    vm.defineMethod(Tag::Task, "result", t_result, 0, 0, "fn() -> any", "The task's result (nil until done).");
}

}  // namespace ixo
