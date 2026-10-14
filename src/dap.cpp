// Debug adapter (Debug Adapter Protocol over stdio) for `ixo dap`.
//
// The program runs on the main thread with VM::debugger set; a second thread
// reads protocol messages. Every VM is a DAP thread: the main program is
// thread 1 and each `par` worker gets its own. When a VM stops (breakpoint,
// step, pause, panic) its thread blocks in stop() and the protocol thread
// answers stackTrace / scopes / variables / evaluate from the frozen VM;
// the other VMs keep running. Values are formatted here without calling back
// into Ixo (no user `str` methods), so inspecting never runs program code.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "builtins.hpp"
#include "compiler.hpp"
#include "debugger.hpp"
#include "json.hpp"
#include "lexer.hpp"
#include "par.hpp"
#include "parser.hpp"
#include "program.hpp"
#include "resolver.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace ixo {

namespace {

std::string baseName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

bool hiddenFile(const std::string& f) { return f.empty() || f[0] == '<'; }

// Code offset of the instruction a frame is at: the top frame's ip points at
// the next instruction to run, callers' just after their call.
size_t frameOffset(const CallFrame& fr, bool top) {
    const Chunk& c = fr.closure->fn->chunk;
    size_t off = (size_t)(fr.ip - c.code.data());
    return top || off == 0 ? off : off - 1;
}

constexpr int kFrameIdStride = 100000;  // frame id = thread id * stride + index + 1

class Adapter : public Debugger {
public:
    int run() {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::thread reader([this] { readLoop(); });
        reader.detach();
        // wait for launch + configurationDone
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return launched && configured; });
        }
        runProgram();
        // the adapter stays up until the client disconnects (the reader exits the process)
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return false; });
        return 0;
    }

    // ---------------------------------------------------------- VM threads

    void check(VM& vm, Fiber* f) override {
        Thread& th = threadOf(vm);
        if (th.evaluating || f->frames.empty()) return;
        const CallFrame& fr = f->frames.back();
        ObjFunction* fn = fr.closure->fn;
        int line = fn->chunk.lineAt(frameOffset(fr, true));
        auto& st = th.locs[f];
        size_t d = f->frames.size();
        if (st.size() > d) st.resize(d);
        bool newLine;
        if (st.size() == d && st[d - 1].fn == fn && st[d - 1].base == fr.base) {
            newLine = st[d - 1].line != line;
            st[d - 1].line = line;
        } else {
            st.resize(d, Loc{});
            st[d - 1] = Loc{fn, fr.base, line};
            newLine = true;
        }
        bool pause = th.pauseRequested.load(std::memory_order_relaxed);
        if ((!newLine && !pause) || line <= 0 || hiddenFile(fn->file)) return;
        size_t depth = chainDepth(f);
        const char* reason = nullptr;
        if (pause)
            reason = "pause";
        else if (Breakpoint* bp = breakpointAt(th, fn, line))
            reason = breakpointFires(th, vm, f, *bp) ? "breakpoint" : nullptr;
        else
            switch (th.mode) {
                case Mode::StepIn: reason = "step"; break;
                case Mode::StepOver:
                    if (depth < th.stepDepth || (depth == th.stepDepth && !(fn == th.stepFn && line == th.stepLine)))
                        reason = "step";
                    break;
                case Mode::StepOut:
                    if (depth < th.stepDepth) reason = "step";
                    break;
                case Mode::Run: break;
            }
        if (reason) stop(th, vm, f, reason, "");
    }

    void onPanic(VM& vm, Fiber* f, const std::string& msg) override {
        if (!stopOnPanic || f->frames.empty()) return;
        stop(threadOf(vm), vm, f, "exception", msg);
    }

private:
    enum class Mode { Run, StepIn, StepOver, StepOut };
    enum class Cmd { None, Continue, Next, StepIn, StepOut };
    struct Loc {
        ObjFunction* fn = nullptr;
        size_t base = 0;
        int line = -1;
    };
    struct FrameRef {
        Fiber* fiber;
        size_t index;
        size_t limit;  // stack top of this frame (the next frame's base)
    };
    struct Breakpoint {
        std::string condition, hitCondition, logMessage;
        long hits = 0;
        bool warned = false;
    };

    // One per VM: the main program and each par worker.
    struct Thread {
        int id = 1;
        std::string name;
        VM* vm = nullptr;
        bool isMain = false;
        // used only on the VM's own thread
        Mode mode = Mode::Run;
        size_t stepDepth = 0;
        ObjFunction* stepFn = nullptr;
        int stepLine = 0;
        std::unordered_map<Fiber*, std::vector<Loc>> locs;
        std::unordered_map<ObjFunction*, std::string> canonOf;
        std::unordered_map<std::string, std::unordered_map<int, Breakpoint>> bpCopy;  // its snapshot
        int bpCopyGen = -1;
        bool evaluating = false;  // check() ignores code the debugger itself runs
        std::atomic<bool> pauseRequested{false};
        // while stopped (guarded by mu)
        bool stopped = false;
        Cmd cmd = Cmd::None;
        std::vector<FrameRef> frames;
        std::function<void()> job;  // work the protocol thread hands over
        bool jobDone = false;
    };

    struct VarRef {
        enum K { Locals, Globals, Value_, Dead } k;
        int thread;
        size_t frame;
        Value v;
    };

    std::mutex sendMu;
    int seq = 1;

    std::mutex mu;  // guards everything below that several threads touch
    std::condition_variable cv;
    bool launched = false, configured = false;
    std::map<int, std::unique_ptr<Thread>> threads;
    int nextThreadId = 2;
    std::unordered_map<std::string, std::unordered_map<int, Breakpoint>> breakpoints;  // canonical path -> line -> bp
    std::atomic<int> bpGeneration{0};
    bool stopOnPanic = true;
    std::vector<VarRef> refs;

    // launch settings
    std::string program, cwd, testName;
    std::vector<std::string> args;
    bool stopOnEntry = false, typeCheck = true;

    Program* program_ = nullptr;
    int evalCounter = 0;
    std::unordered_map<std::string, Value> evalCache;  // expression + context -> closure

    Thread& threadOf(VM& vm) {
        thread_local Thread* cached = nullptr;
        thread_local VM* cachedVm = nullptr;
        if (cached && cachedVm == &vm) return *cached;
        Thread* t = nullptr;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& [id, th] : threads)
                if (th->vm == &vm) t = th.get();
            if (!t) {
                auto th = std::make_unique<Thread>();
                th->vm = &vm;
                th->isMain = !vm.isWorker;
                th->id = th->isMain ? 1 : nextThreadId++;
                th->name = th->isMain ? "main" : "par worker " + std::to_string(th->id - 1);
                t = th.get();
                threads[t->id] = std::move(th);
                fresh = !t->isMain;
            }
        }
        if (fresh) {
            Json b = Json::object();
            b.set("reason", "started");
            b.set("threadId", t->id);
            event("thread", b);
        }
        cached = t;
        cachedVm = &vm;
        return *t;
    }

    Thread* threadById(int id) {
        auto it = threads.find(id);
        return it == threads.end() ? nullptr : it->second.get();
    }

    // ---------------------------------------------------------- protocol I/O

    bool readMessage(Json& out) {
        size_t length = 0;
        std::string line;
        while (true) {
            line.clear();
            int c;
            while ((c = std::fgetc(stdin)) != EOF && c != '\n') line += (char)c;
            if (c == EOF) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.rfind("Content-Length:", 0) == 0) length = (size_t)std::stoul(line.substr(15));
        }
        std::string body(length, '\0');
        if (std::fread(body.data(), 1, length, stdin) != length) return false;
        std::string err;
        out = Json::parse(body, &err);
        return true;
    }

    void send(Json msg) {
        std::lock_guard<std::mutex> lk(sendMu);
        msg.set("seq", seq++);
        std::string body = msg.dump();
        std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        std::fwrite(header.data(), 1, header.size(), stdout);
        std::fwrite(body.data(), 1, body.size(), stdout);
        std::fflush(stdout);
    }
    void respond(const Json& req, Json body = Json(), bool ok = true, const std::string& message = "") {
        Json m = Json::object();
        m.set("type", "response");
        m.set("request_seq", req.get("seq") ? Json((int)req.get("seq")->num()) : Json(0));
        m.set("success", ok);
        m.set("command", req.get("command") ? req.get("command")->s : "");
        if (!message.empty()) m.set("message", message);
        if (!body.isNull()) m.set("body", std::move(body));
        send(std::move(m));
    }
    void event(const std::string& name, Json body = Json::object()) {
        Json m = Json::object();
        m.set("type", "event");
        m.set("event", name);
        m.set("body", std::move(body));
        send(std::move(m));
    }
    void output(const std::string& category, const std::string& text) {
        Json b = Json::object();
        b.set("category", category);
        b.set("output", text);
        event("output", b);
    }

    // ---------------------------------------------------------- protocol thread

    void readLoop() {
        Json msg;
        while (readMessage(msg)) {
            try {
                handle(msg);
            } catch (const std::exception& e) {
                respond(msg, Json(), false, std::string("internal error: ") + e.what());
            }
        }
        std::fflush(stdout);
        std::_Exit(0);  // the client went away
    }

    static int threadArg(const Json& a) {
        const Json* t = a.get("threadId");
        return t ? (int)t->num() : 1;
    }
    static int frameThread(const Json& a) {
        const Json* f = a.get("frameId");
        return f ? (int)f->num() / kFrameIdStride : 1;
    }
    static size_t frameIndex(const Json& a) {
        const Json* f = a.get("frameId");
        if (!f) return 0;
        int n = (int)f->num() % kFrameIdStride;
        return n > 0 ? (size_t)(n - 1) : 0;
    }

    void handle(const Json& req) {
        std::string command = req.get("command") ? req.get("command")->s : "";
        Json empty = Json::object();
        const Json& a = req.get("arguments") ? *req.get("arguments") : empty;
        if (command == "initialize") {
            Json caps = Json::object();
            caps.set("supportsConfigurationDoneRequest", true);
            caps.set("supportsEvaluateForHovers", true);
            caps.set("supportsTerminateRequest", true);
            caps.set("supportsConditionalBreakpoints", true);
            caps.set("supportsHitConditionalBreakpoints", true);
            caps.set("supportsLogPoints", true);
            caps.set("supportsSetVariable", true);
            Json filters = Json::array();
            Json f = Json::object();
            f.set("filter", "panic");
            f.set("label", "Panics");
            f.set("default", true);
            filters.push(f);
            caps.set("exceptionBreakpointFilters", filters);
            respond(req, caps);
            event("initialized");
            return;
        }
        if (command == "launch") {
            std::lock_guard<std::mutex> lk(mu);
            program = a.get("program") ? a.get("program")->s : "";
            cwd = a.get("cwd") ? a.get("cwd")->s : "";
            testName = a.get("test") ? a.get("test")->s : "";
            stopOnEntry = a.get("stopOnEntry") && a.get("stopOnEntry")->k == Json::Bool && a.get("stopOnEntry")->b;
            typeCheck = !(a.get("noTypecheck") && a.get("noTypecheck")->k == Json::Bool && a.get("noTypecheck")->b);
            args.clear();
            if (const Json* xs = a.get("args"))
                for (auto& x : xs->arr) args.push_back(x.s);
            if (program.empty()) {
                respond(req, Json(), false, "launch needs a 'program' (the .ixo file to run)");
                return;
            }
            launched = true;
            cv.notify_all();
            respond(req);
            return;
        }
        if (command == "setBreakpoints") {
            std::string path = a.at("source", "path") ? a.at("source", "path")->s : "";
            Json out = Json::array();
            std::unordered_map<int, Breakpoint> lines;
            if (const Json* bps = a.get("breakpoints"))
                for (auto& bp : bps->arr) {
                    int line = (int)(bp.get("line") ? bp.get("line")->num() : 0);
                    Breakpoint b;
                    b.condition = bp.get("condition") ? bp.get("condition")->s : "";
                    b.hitCondition = bp.get("hitCondition") ? bp.get("hitCondition")->s : "";
                    b.logMessage = bp.get("logMessage") ? bp.get("logMessage")->s : "";
                    lines[line] = b;
                    Json v = Json::object();
                    v.set("verified", true);
                    v.set("line", line);
                    out.push(v);
                }
            {
                std::lock_guard<std::mutex> lk(mu);
                breakpoints[canonicalPath(path)] = lines;
                bpGeneration++;
            }
            Json body = Json::object();
            body.set("breakpoints", out);
            respond(req, body);
            return;
        }
        if (command == "setExceptionBreakpoints") {
            bool on = false;
            if (const Json* fs = a.get("filters"))
                for (auto& x : fs->arr)
                    if (x.s == "panic") on = true;
            std::lock_guard<std::mutex> lk(mu);
            stopOnPanic = on;
            respond(req);
            return;
        }
        if (command == "configurationDone") {
            std::lock_guard<std::mutex> lk(mu);
            configured = true;
            cv.notify_all();
            respond(req);
            return;
        }
        if (command == "threads") {
            Json ts = Json::array();
            {
                std::lock_guard<std::mutex> lk(mu);
                if (threads.empty()) {
                    Json t = Json::object();
                    t.set("id", 1);
                    t.set("name", "main");
                    ts.push(t);
                }
                for (auto& [id, th] : threads) {
                    Json t = Json::object();
                    t.set("id", id);
                    t.set("name", th->name);
                    ts.push(t);
                }
            }
            Json body = Json::object();
            body.set("threads", ts);
            respond(req, body);
            return;
        }
        if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
            {
                std::lock_guard<std::mutex> lk(mu);
                Thread* th = threadById(threadArg(a));
                if (th && th->stopped) {
                    th->cmd = command == "continue" ? Cmd::Continue
                              : command == "next"   ? Cmd::Next
                              : command == "stepIn" ? Cmd::StepIn
                                                    : Cmd::StepOut;
                    cv.notify_all();
                }
            }
            Json body = Json::object();
            if (command == "continue") body.set("allThreadsContinued", false);
            respond(req, body);
            return;
        }
        if (command == "pause") {
            {
                std::lock_guard<std::mutex> lk(mu);
                Thread* th = a.get("threadId") ? threadById(threadArg(a)) : nullptr;
                if (th) {
                    th->pauseRequested = true;
                } else {
                    for (auto& [id, t] : threads) t->pauseRequested = true;
                    if (threads.empty()) pendingPause = true;
                }
            }
            respond(req);
            return;
        }
        if (command == "disconnect" || command == "terminate") {
            respond(req);
            if (command == "terminate") event("terminated");
            flushOut();
            std::fflush(stdout);
            std::_Exit(0);
        }
        if (command == "stackTrace") return onStackTrace(req, a);
        if (command == "scopes") return onScopes(req, a);
        if (command == "variables") return onVariables(req, a);
        if (command == "evaluate") return onEvaluate(req, a);
        if (command == "setVariable") return onSetVariable(req, a);
        respond(req, Json(), false, "unsupported request: " + command);
    }
    bool pendingPause = false;  // pause before the program started

    void onStackTrace(const Json& req, const Json& a) {
        std::lock_guard<std::mutex> lk(mu);
        Json list = Json::array();
        Thread* th = threadById(threadArg(a));
        for (size_t i = 0; th && th->stopped && i < th->frames.size(); i++) {
            const CallFrame& fr = th->frames[i].fiber->frames[th->frames[i].index];
            ObjFunction* fn = fr.closure->fn;
            Json sf = Json::object();
            sf.set("id", th->id * kFrameIdStride + (int)i + 1);
            sf.set("name", fn->name.empty() ? "<script>" : fn->name);
            sf.set("line", fn->chunk.lineAt(frameOffset(fr, i == 0)));
            sf.set("column", 1);
            if (!hiddenFile(fn->file)) {
                Json src = Json::object();
                src.set("name", baseName(fn->file));
                src.set("path", canonicalPath(fn->file));
                sf.set("source", src);
            } else {
                sf.set("presentationHint", "subtle");
            }
            list.push(sf);
        }
        Json body = Json::object();
        body.set("stackFrames", list);
        body.set("totalFrames", (int)list.arr.size());
        respond(req, body);
    }

    int addRef(VarRef r) {
        refs.push_back(r);
        return (int)refs.size();
    }

    void onScopes(const Json& req, const Json& a) {
        std::lock_guard<std::mutex> lk(mu);
        Thread* th = threadById(frameThread(a));
        size_t frame = frameIndex(a);
        Json scopes = Json::array();
        if (th && th->stopped && frame < th->frames.size()) {
            Json l = Json::object();
            l.set("name", "Locals");
            l.set("presentationHint", "locals");
            l.set("variablesReference", addRef({VarRef::Locals, th->id, frame, Value()}));
            l.set("expensive", false);
            scopes.push(l);
            Json g = Json::object();
            g.set("name", "Module");
            g.set("variablesReference", addRef({VarRef::Globals, th->id, frame, Value()}));
            g.set("expensive", false);
            scopes.push(g);
        }
        Json body = Json::object();
        body.set("scopes", scopes);
        respond(req, body);
    }

    // Variables visible in a frame: (name, value), innermost binding of each name.
    std::vector<std::pair<std::string, Value>> localsOf(std::vector<FrameRef>& frames, size_t frame) {
        std::vector<std::pair<std::string, Value>> out;
        if (frame >= frames.size()) return out;
        FrameRef& ref = frames[frame];
        const CallFrame& fr = ref.fiber->frames[ref.index];
        ObjFunction* fn = fr.closure->fn;
        size_t off = frameOffset(fr, frame == 0);
        std::unordered_map<std::string, const ObjFunction::LocalName*> best;
        for (auto& ln : fn->localNames) {
            if (off < ln.start || off >= ln.end || fr.base + ln.slot >= ref.limit) continue;
            auto& b = best[ln.name];
            if (!b || ln.start >= b->start) b = &ln;
        }
        std::vector<const ObjFunction::LocalName*> v;
        for (auto& [n, ln] : best) v.push_back(ln);
        std::sort(v.begin(), v.end(), [](auto* x, auto* y) { return x->slot < y->slot; });
        for (auto* ln : v) {
            Value val = ref.fiber->stack[fr.base + ln->slot];
            if (!val.isUndef()) out.push_back({ln->name, val});
        }
        ObjClosure* cl = fr.closure;
        for (size_t i = 0; i < fn->upvalueNames.size() && i < (size_t)fn->upvalueCount; i++) {
            ObjUpvalue* u = cl->upvalues[i];
            if (!u) continue;
            Value val = u->fiber ? u->fiber->stack[u->slot] : u->closed;
            if (!val.isUndef()) out.push_back({fn->upvalueNames[i], val});
        }
        return out;
    }

    std::vector<std::pair<std::string, Value>> globalsOf(std::vector<FrameRef>& frames, size_t frame) {
        std::vector<std::pair<std::string, Value>> out;
        if (frame >= frames.size()) return out;
        const CallFrame& fr = frames[frame].fiber->frames[frames[frame].index];
        ModuleRt* m = fr.module ? fr.module : fr.closure->fn->module;
        if (!m) return out;
        for (size_t i = 0; i < m->globals.size() && i < m->globalNames.size(); i++) {
            const std::string& n = m->globalNames[i];
            if (n.empty() || n[0] == '$' || n.rfind("__ixo_", 0) == 0 || m->globals[i].isUndef()) continue;
            Value v = m->globals[i];
            // functions, classes and modules are noise here
            if (isObjType(v, OT::Closure) || isObjType(v, OT::Class) || isObjType(v, OT::Module) ||
                isObjType(v, OT::Native) || isObjType(v, OT::Namespace) || isObjType(v, OT::Interface))
                continue;
            out.push_back({n, v});
        }
        return out;
    }

    // ---------------------------------------------------------- values

    static bool hasChildren(Value v) {
        if (!v.isObj()) return false;
        switch (v.o->type) {
            case OT::List: return as<ObjList>(v)->len() != 0;
            case OT::Tuple: return !as<ObjTuple>(v)->items.empty();
            case OT::Map: return as<ObjMap>(v)->live > 0;
            case OT::Set: return as<ObjSet>(v)->live > 0;
            case OT::Instance: return !as<ObjInstance>(v)->fields.empty();
            case OT::Namespace: return !as<ObjNamespace>(v)->members.empty();
            default: return false;
        }
    }

    std::vector<std::pair<std::string, Value>> childrenOf(VM& vm, Value v) {
        std::vector<std::pair<std::string, Value>> out;
        const size_t limit = 1000;
        if (!v.isObj()) return out;
        switch (v.o->type) {
            case OT::List: {
                auto* l = as<ObjList>(v);
                for (size_t i = 0; i < l->len() && i < limit; i++) out.push_back({"[" + std::to_string(i) + "]", l->get(i)});
                break;
            }
            case OT::Tuple: {
                auto& items = as<ObjTuple>(v)->items;
                for (size_t i = 0; i < items.size() && i < limit; i++) out.push_back({"[" + std::to_string(i) + "]", items[i]});
                break;
            }
            case OT::Map:
                for (auto& [k, x] : as<ObjMap>(v)->entries) {
                    if (k.isUndef()) continue;
                    if (out.size() >= limit) break;
                    out.push_back({display(vm, k, 1), x});
                }
                break;
            case OT::Set:
                for (auto& x : as<ObjSet>(v)->items) {
                    if (x.isUndef()) continue;
                    if (out.size() >= limit) break;
                    out.push_back({"[" + std::to_string(out.size()) + "]", x});
                }
                break;
            case OT::Instance: {
                auto* inst = as<ObjInstance>(v);
                for (size_t i = 0; i < inst->fields.size(); i++)
                    out.push_back({i < inst->cls->fields.size() ? inst->cls->fields[i] : "?", inst->fields[i]});
                break;
            }
            case OT::Namespace:
                for (auto& [k, x] : as<ObjNamespace>(v)->members)
                    if (!k.empty() && k[0] != '$') out.push_back({k, x});
                std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first < b.first; });
                break;
            default: break;
        }
        return out;
    }

    // Display form that never runs Ixo code.
    std::string display(VM& vm, Value v, int depth = 0) {
        if (!v.isObj()) return vm.repr(v);
        auto list = [&](SeqView items, const char* open, const char* close) {
            if (depth > 2) return std::string(open) + "..." + close;
            std::string s = open;
            size_t n = 0;
            for (Value x : items) {
                if (x.isUndef()) continue;
                if (n) s += ", ";
                if (n++ >= 20 || s.size() > 200) {
                    s += "...";
                    break;
                }
                s += display(vm, x, depth + 1);
            }
            return s + close;
        };
        switch (v.o->type) {
            case OT::List: return list(SeqView(*as<ObjList>(v)).vec(), "[", "]");
            case OT::Set: return list(as<ObjSet>(v)->items, "{", "}");
            case OT::Tuple: {
                auto& items = as<ObjTuple>(v)->items;
                return list(items, "(", items.size() == 1 ? ",)" : ")");
            }
            case OT::Map: {
                if (depth > 2) return "{...}";
                std::string s = "{";
                size_t n = 0;
                for (auto& [k, x] : as<ObjMap>(v)->entries) {
                    if (k.isUndef()) continue;
                    if (n) s += ", ";
                    if (n++ >= 20 || s.size() > 200) {
                        s += "...";
                        break;
                    }
                    s += display(vm, k, depth + 1) + ": " + display(vm, x, depth + 1);
                }
                return s + "}";
            }
            case OT::Instance: {
                auto* inst = as<ObjInstance>(v);
                if (inst->fields.empty()) return inst->cls->name;
                if (depth > 2) return inst->cls->name + "(...)";
                std::string s = inst->cls->name + "(";
                for (size_t i = 0; i < inst->fields.size(); i++) {
                    if (i) s += ", ";
                    if (s.size() > 200) {
                        s += "...";
                        break;
                    }
                    s += (i < inst->cls->fields.size() ? inst->cls->fields[i] + ": " : "") +
                         display(vm, inst->fields[i], depth + 1);
                }
                return s + ")";
            }
            case OT::Error: {
                auto* e = as<ObjError>(v);
                return "Error." + e->kind + "(\"" + e->msg + "\")";
            }
            case OT::Namespace: return as<ObjNamespace>(v)->name;
            case OT::String: {
                std::string s = vm.repr(v);
                return s.size() > 300 ? s.substr(0, 300) + "...\"" : s;
            }
            case OT::Channel: return "flow";
            case OT::Task: return "task";
            case OT::Iterator: return "iterator";
            default: return vm.repr(v);
        }
    }

    Json variable(Thread& th, const std::string& name, Value v) {
        Json j = Json::object();
        j.set("name", name);
        j.set("value", display(*th.vm, v));
        j.set("type", th.vm->typeName(v));
        j.set("variablesReference", hasChildren(v) ? addRef({VarRef::Value_, th.id, 0, v}) : 0);
        return j;
    }

    void onVariables(const Json& req, const Json& a) {
        std::lock_guard<std::mutex> lk(mu);
        size_t r = (size_t)(a.get("variablesReference") ? a.get("variablesReference")->num() : 0);
        Json vars = Json::array();
        if (r >= 1 && r <= refs.size() && refs[r - 1].k != VarRef::Dead) {
            VarRef ref = refs[r - 1];
            Thread* th = threadById(ref.thread);
            if (th && th->stopped) {
                std::vector<std::pair<std::string, Value>> items = ref.k == VarRef::Locals    ? localsOf(th->frames, ref.frame)
                                                                   : ref.k == VarRef::Globals ? globalsOf(th->frames, ref.frame)
                                                                                              : childrenOf(*th->vm, ref.v);
                for (auto& [n, v] : items) vars.push(variable(*th, n, v));
            }
        }
        Json body = Json::object();
        body.set("variables", vars);
        respond(req, body);
    }

    // Expressions: a name followed by .field, [index] or ["key"] parts.
    bool evalPath(Thread& th, const std::string& expr, size_t frame, Value& out, std::string& err) {
        VM& vm = *th.vm;
        size_t i = 0;
        auto skipWs = [&] {
            while (i < expr.size() && expr[i] == ' ') i++;
        };
        auto ident = [&]() {
            size_t s = i;
            while (i < expr.size() && (std::isalnum((unsigned char)expr[i]) || expr[i] == '_')) i++;
            return expr.substr(s, i - s);
        };
        skipWs();
        std::string root = ident();
        if (root.empty()) {
            err = "can only show variables and their fields (like p.x or xs[0])";
            return false;
        }
        bool found = false;
        for (auto& [n, v] : localsOf(th.frames, frame))
            if (n == root) {
                out = v;
                found = true;
            }
        if (!found)
            for (auto& [n, v] : globalsOf(th.frames, frame))
                if (n == root) {
                    out = v;
                    found = true;
                }
        if (!found && frame < th.frames.size()) {
            const CallFrame& fr = th.frames[frame].fiber->frames[th.frames[frame].index];
            ModuleRt* m = fr.module ? fr.module : fr.closure->fn->module;
            for (size_t k = 0; m && k < m->globalNames.size(); k++)
                if (m->globalNames[k] == root && !m->globals[k].isUndef()) {
                    out = m->globals[k];
                    found = true;
                }
        }
        if (!found) {
            err = "'" + root + "' isn't defined here";
            return false;
        }
        while (true) {
            skipWs();
            if (i >= expr.size()) return true;
            if (expr[i] == '.') {
                i++;
                std::string name = ident();
                bool ok = false;
                if (isObjType(out, OT::Instance)) {
                    auto* inst = as<ObjInstance>(out);
                    for (size_t k = 0; k < inst->cls->fields.size(); k++)
                        if (inst->cls->fields[k] == name) {
                            out = inst->fields[k];
                            ok = true;
                        }
                } else if (isObjType(out, OT::Map)) {
                    for (auto& [k, x] : as<ObjMap>(out)->entries)
                        if (isObjType(k, OT::String) && as<ObjString>(k)->s == name) {
                            out = x;
                            ok = true;
                            break;
                        }
                } else if (isObjType(out, OT::Namespace)) {
                    auto& mem = as<ObjNamespace>(out)->members;
                    auto it = mem.find(name);
                    if (it != mem.end()) {
                        out = it->second;
                        ok = true;
                    }
                }
                if (!ok) {
                    err = "no field '" + name + "'";
                    return false;
                }
            } else if (expr[i] == '[') {
                size_t close = expr.find(']', i);
                if (close == std::string::npos) {
                    err = "missing ']'";
                    return false;
                }
                std::string key = expr.substr(i + 1, close - i - 1);
                i = close + 1;
                while (!key.empty() && key.front() == ' ') key.erase(0, 1);
                while (!key.empty() && key.back() == ' ') key.pop_back();
                SeqView seq;
                const SeqView* items = seqOf(out, seq) ? &seq : nullptr;
                if (items) {
                    char* end = nullptr;
                    long long k = std::strtoll(key.c_str(), &end, 10);
                    if (key.empty() || *end) {
                        err = "index must be a number";
                        return false;
                    }
                    if (k < 0) k += (long long)items->size();
                    if (k < 0 || k >= (long long)items->size()) {
                        err = "index out of range";
                        return false;
                    }
                    out = (*items)[(size_t)k];
                } else if (isObjType(out, OT::Map)) {
                    bool ok = false;
                    bool quoted = key.size() >= 2 && (key[0] == '"' || key[0] == '\'');
                    std::string sk = quoted ? key.substr(1, key.size() - 2) : key;
                    char* end = nullptr;
                    long long ik = std::strtoll(key.c_str(), &end, 10);
                    bool isInt = !quoted && !key.empty() && !*end;
                    if (!quoted && !isInt) {
                        err = "map keys: use a number or a quoted string";
                        return false;
                    }
                    // compare without allocating (this may run on the protocol thread)
                    for (auto& [k, x] : as<ObjMap>(out)->entries) {
                        if (quoted && isObjType(k, OT::String) && as<ObjString>(k)->s == sk) ok = true;
                        if (isInt && k.t == VT::Int && k.i == ik) ok = true;
                        if (ok) {
                            out = x;
                            break;
                        }
                    }
                    if (!ok) {
                        err = "no key " + key;
                        return false;
                    }
                } else {
                    err = "can't index a " + vm.typeName(out);
                    return false;
                }
            } else {
                err = "can only show variables and their fields (like p.x or xs[0])";
                return false;
            }
        }
    }

    void onSetVariable(const Json& req, const Json& a) {
        std::unique_lock<std::mutex> lk(mu);
        size_t r = (size_t)(a.get("variablesReference") ? a.get("variablesReference")->num() : 0);
        std::string name = a.get("name") ? a.get("name")->s : "";
        std::string text = a.get("value") ? a.get("value")->s : "";
        if (r < 1 || r > refs.size() || refs[r - 1].k == VarRef::Dead) return respond(req, Json(), false, "unknown variable");
        VarRef ref = refs[r - 1];
        Thread* th = threadById(ref.thread);
        if (!th || !th->stopped) return respond(req, Json(), false, "the program is running");
        if (!th->isMain) return respond(req, Json(), false, "values in par tasks can't be changed from the debugger");
        size_t frame = ref.k == VarRef::Value_ ? 0 : ref.frame;
        Value v;
        std::string err;
        bool ok = false;
        bool ran = onVmThread(*th, lk, [&] {
            if (!evaluate(*th, frame, text, v, err)) return;
            ok = assign(*th, ref, name, v, err);
        });
        if (!ran) return respond(req, Json(), false, "the program is running");
        if (!ok) return respond(req, Json(), false, err);
        Json body = Json::object();
        body.set("value", display(*th->vm, v));
        body.set("type", th->vm->typeName(v));
        body.set("variablesReference", hasChildren(v) ? addRef({VarRef::Value_, th->id, 0, v}) : 0);
        respond(req, body);
    }

    // Stores a new value into a local, module variable, field, element or map entry.
    bool assign(Thread& th, const VarRef& ref, const std::string& name, Value v, std::string& err) {
        if (ref.k == VarRef::Locals) {
            FrameRef& fref = th.frames[ref.frame];
            const CallFrame& fr = fref.fiber->frames[fref.index];
            ObjFunction* fn = fr.closure->fn;
            size_t off = frameOffset(fr, ref.frame == 0);
            const ObjFunction::LocalName* best = nullptr;
            for (auto& ln : fn->localNames)
                if (ln.name == name && off >= ln.start && off < ln.end && (!best || ln.start >= best->start)) best = &ln;
            if (best) {
                fref.fiber->stack[fr.base + best->slot] = v;
                return true;
            }
            for (size_t i = 0; i < fn->upvalueNames.size() && i < fr.closure->upvalues.size(); i++)
                if (fn->upvalueNames[i] == name && fr.closure->upvalues[i]) {
                    ObjUpvalue* u = fr.closure->upvalues[i];
                    if (u->fiber)
                        u->fiber->stack[u->slot] = v;
                    else
                        u->closed = v;
                    return true;
                }
            err = "no variable '" + name + "' here";
            return false;
        }
        if (ref.k == VarRef::Globals) {
            const CallFrame& fr = th.frames[ref.frame].fiber->frames[th.frames[ref.frame].index];
            ModuleRt* m = fr.module ? fr.module : fr.closure->fn->module;
            for (size_t i = 0; m && i < m->globalNames.size(); i++)
                if (m->globalNames[i] == name) {
                    m->globals[i] = v;
                    return true;
                }
            err = "no module variable '" + name + "'";
            return false;
        }
        Value c = ref.v;
        if (c.isObj() && c.o->frozen) {
            err = "frozen values can't be changed";
            return false;
        }
        if (isObjType(c, OT::List) && name.size() > 2 && name[0] == '[') {
            size_t i = (size_t)std::strtoul(name.c_str() + 1, nullptr, 10);
            auto* l = as<ObjList>(c);
            if (i < l->len()) {
                l->set(i, v);
                return true;
            }
        } else if (isObjType(c, OT::Instance)) {
            auto* inst = as<ObjInstance>(c);
            for (size_t i = 0; i < inst->cls->fields.size() && i < inst->fields.size(); i++)
                if (inst->cls->fields[i] == name) {
                    inst->fields[i] = v;
                    return true;
                }
        } else if (isObjType(c, OT::Map)) {
            for (auto& [k, x] : as<ObjMap>(c)->entries)
                if (!k.isUndef() && display(*th.vm, k, 1) == name) {
                    x = v;
                    return true;
                }
        } else if (isObjType(c, OT::Tuple)) {
            err = "tuples can't be changed";
            return false;
        }
        err = "can't change '" + name + "'";
        return false;
    }

    void onEvaluate(const Json& req, const Json& a) {
        std::unique_lock<std::mutex> lk(mu);
        Thread* th = threadById(a.get("frameId") ? frameThread(a) : 1);
        if (!th || !th->stopped) return respond(req, Json(), false, "the program is running");
        std::string expr = a.get("expression") ? a.get("expression")->s : "";
        std::string context = a.get("context") ? a.get("context")->s : "";
        size_t frame = frameIndex(a);
        Value v;
        std::string err;
        // hovering only reads variables (no side effects); the console runs any expression
        bool ok = evalPath(*th, expr, frame, v, err);
        if (!ok && context != "hover") {
            if (!th->isMain) return respond(req, Json(), false, err + " (in par tasks the console shows variables and their fields)");
            std::string perr;
            bool done = onVmThread(*th, lk, [&] { ok = evaluate(*th, frame, expr, v, perr); });
            if (!done) return respond(req, Json(), false, "the program is running");
            err = perr;
        }
        if (!ok) return respond(req, Json(), false, err);
        Json body = Json::object();
        body.set("result", display(*th->vm, v));
        body.set("type", th->vm->typeName(v));
        body.set("variablesReference", hasChildren(v) ? addRef({VarRef::Value_, th->id, 0, v}) : 0);
        respond(req, body);
    }

    // ---------------------------------------------------------- VM thread helpers

    static size_t chainDepth(Fiber* f) {
        size_t d = 0;
        for (Fiber* x = f; x; x = x->caller) d += x->frames.size();
        return d;
    }

    Breakpoint* breakpointAt(Thread& th, ObjFunction* fn, int line) {
        int gen = bpGeneration.load();
        if (gen != th.bpCopyGen) {
            std::lock_guard<std::mutex> lk(mu);
            th.bpCopy = breakpoints;
            th.bpCopyGen = gen;
        }
        if (th.bpCopy.empty()) return nullptr;
        auto it = th.canonOf.find(fn);
        if (it == th.canonOf.end()) it = th.canonOf.emplace(fn, canonicalPath(fn->file)).first;
        auto b = th.bpCopy.find(it->second);
        if (b == th.bpCopy.end()) return nullptr;
        auto l = b->second.find(line);
        return l == b->second.end() ? nullptr : &l->second;
    }

    // "total is {total}" with each {path} read from the frame (no code runs).
    std::string interpolatePaths(Thread& th, const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] != '{') {
                out += text[i];
                continue;
            }
            size_t close = text.find('}', i);
            if (close == std::string::npos) {
                out += text.substr(i);
                break;
            }
            std::string expr = text.substr(i + 1, close - i - 1);
            Value v;
            std::string err;
            out += evalPath(th, expr, 0, v, err) ? (isObjType(v, OT::String) ? as<ObjString>(v)->s : display(*th.vm, v))
                                                 : "<" + err + ">";
            i = close;
        }
        return out;
    }

    // Hit counts, conditions and log messages decide whether a breakpoint stops.
    bool breakpointFires(Thread& th, VM& vm, Fiber* f, Breakpoint& bp) {
        bp.hits++;
        if (!bp.hitCondition.empty()) {
            std::string hc = bp.hitCondition;
            hc.erase(std::remove(hc.begin(), hc.end(), ' '), hc.end());
            std::string op;
            size_t k = 0;
            while (k < hc.size() && !std::isdigit((unsigned char)hc[k])) op += hc[k++];
            long n = std::strtol(hc.c_str() + k, nullptr, 10);
            bool ok = op == ">=" ? bp.hits >= n
                      : op == ">" ? bp.hits > n
                      : op == "<=" ? bp.hits <= n
                      : op == "<" ? bp.hits < n
                      : op == "%" ? (n > 0 && bp.hits % n == 0)
                                  : bp.hits == n;  // "5" or "==5"
            if (!ok) return false;
        }
        std::vector<FrameRef> here = buildFrames(f);
        if (!bp.condition.empty()) {
            if (!th.isMain) {
                // par tasks can't compile code: stop, and say why once
                if (!bp.warned) output("console", "breakpoint conditions don't run in par tasks; stopping anyway\n");
                bp.warned = true;
                return true;
            }
            Value v;
            std::string err;
            auto saved = std::move(th.frames);
            th.frames = here;
            bool ok = evaluate(th, 0, bp.condition, v, err);
            th.frames = std::move(saved);
            if (!ok) {
                output("console", "breakpoint condition '" + bp.condition + "' failed: " + err + "\n");
                return true;
            }
            if (!vm.truthy(v)) return false;
        }
        if (!bp.logMessage.empty()) {
            auto saved = std::move(th.frames);
            th.frames = here;
            std::string text;
            if (th.isMain) {
                // "x is {x}": the text is an f-string
                Value v;
                std::string err;
                std::string src = "f\"";
                for (char c : bp.logMessage) src += c == '"' ? std::string("\\\"") : std::string(1, c);
                src += "\"";
                text = evaluate(th, 0, src, v, err) ? (isObjType(v, OT::String) ? as<ObjString>(v)->s : display(vm, v))
                                                    : bp.logMessage + "  (" + err + ")";
            } else {
                text = "[" + th.name + "] " + interpolatePaths(th, bp.logMessage);
            }
            th.frames = std::move(saved);
            output("console", text + "\n");
            return false;  // logpoints don't stop
        }
        return true;
    }

    // ---------------------------------------------------------- evaluating expressions

    std::vector<FrameRef> buildFrames(Fiber* f) {
        std::vector<FrameRef> out;
        for (Fiber* x = f; x; x = x->caller)
            for (size_t i = x->frames.size(); i-- > 0;) {
                size_t limit = i + 1 < x->frames.size() ? x->frames[i + 1].base : x->sp;
                out.push_back({x, i, limit});
            }
        return out;
    }

    // Replaces the word `self` (outside strings) so it can be a parameter.
    static std::string renameSelf(const std::string& e) {
        std::string out;
        char quote = 0;
        for (size_t i = 0; i < e.size(); i++) {
            char c = e[i];
            if (quote) {
                out += c;
                if (c == '\\' && i + 1 < e.size()) out += e[++i];
                else if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                out += c;
                continue;
            }
            bool wordStart = i == 0 || !(std::isalnum((unsigned char)e[i - 1]) || e[i - 1] == '_');
            if (wordStart && e.compare(i, 4, "self") == 0 &&
                (i + 4 == e.size() || !(std::isalnum((unsigned char)e[i + 4]) || e[i + 4] == '_'))) {
                out += "self__";
                i += 3;
                continue;
            }
            out += c;
        }
        return out;
    }

    // Evaluates an Ixo expression in a frame of the main program: its variables
    // are parameters of a small function compiled in the frame's module. Runs
    // on the main VM's thread.
    bool evaluate(Thread& th, size_t frame, const std::string& expr, Value& out, std::string& err) {
        VM& vm = *th.vm;
        std::vector<FrameRef>& at = th.frames;
        if (!program_ || !th.isMain || frame >= at.size()) {
            err = th.isMain ? "no program" : "par tasks can only show variables and their fields";
            return false;
        }
        const CallFrame& fr = at[frame].fiber->frames[at[frame].index];
        ObjFunction* fnObj = fr.closure->fn;
        auto u = program_->byPath.find(canonicalPath(fnObj->file));
        if (u == program_->byPath.end() || !u->second->rt) {
            err = "can't evaluate code in " + (fnObj->file.empty() ? std::string("this frame") : baseName(fnObj->file));
            return false;
        }
        ModuleUnit* mod = u->second;
        std::vector<std::pair<std::string, Value>> vars = localsOf(at, frame);
        std::vector<std::pair<std::string, Value>> params;
        std::unordered_set<std::string> seen;
        for (auto& [n, v] : vars) {
            std::string name = n == "self" ? "self__" : n;
            bool ident = !name.empty() && (std::isalpha((unsigned char)name[0]) || name[0] == '_');
            for (char c : name)
                if (!(std::isalnum((unsigned char)c) || c == '_')) ident = false;
            if (ident && seen.insert(name).second) params.push_back({name, v});
        }
        std::string body = renameSelf(expr);
        std::string key = body + "\x01" + canonicalPath(fnObj->file);
        for (auto& [n, v] : params) key += "\x01" + n;
        Value closure;
        auto cached = evalCache.find(key);
        if (cached != evalCache.end()) {
            closure = cached->second;
        } else {
            std::string fname = "__ixo_eval_" + std::to_string(++evalCounter);
            std::string src = "fn " + fname + "(";
            for (size_t i = 0; i < params.size(); i++) src += (i ? ", " : "") + params[i].first;
            src += ") = " + body + "\n";
            Diagnostics diags;
            Lexer lexer(src, "<debug console>", diags);
            Parser parser(lexer.tokenize(), "<debug console>", diags);
            auto chunk = parser.parseProgram();
            if (!diags.hasErrors()) {
                Resolver resolver(*mod, diags, *program_);
                resolver.resolveModule(chunk.get());
            }
            ObjFunction* script = nullptr;
            if (!diags.hasErrors()) {
                Compiler compiler(vm, *mod, diags);
                script = compiler.compileModule(chunk.get(), false, false);
            }
            mod->replChunks.push_back(std::move(chunk));
            if (diags.hasErrors() || !script) {
                err = diags.list.empty() ? "can't compile that" : diags.list[0].message;
                return false;
            }
            ObjFunction* fn = nullptr;
            for (auto& c : script->chunk.consts)
                if (isObjType(c, OT::Function) && as<ObjFunction>(c)->name == fname) fn = as<ObjFunction>(c);
            if (!fn) {
                err = "can't compile that";
                return false;
            }
            closure = Value::object(vm.alloc<ObjClosure>(fn));
            vm.extraRoots.push_back(closure);
            evalCache[key] = closure;
        }
        std::vector<Value> args;
        for (auto& [n, v] : params) args.push_back(v);
        bool saved = th.evaluating;
        th.evaluating = true;
        try {
            out = vm.callNested(closure, args.data(), (int)args.size());
            th.evaluating = saved;
        } catch (PanicError& e) {
            th.evaluating = saved;
            err = e.msg;
            return false;
        }
        return true;
    }

    // Runs `job` on the stopped VM's thread and waits for it. Call with `mu` held.
    bool onVmThread(Thread& th, std::unique_lock<std::mutex>& lk, std::function<void()> job) {
        if (!th.stopped) return false;
        th.job = std::move(job);
        th.jobDone = false;
        cv.notify_all();
        cv.wait(lk, [&] { return th.jobDone || !th.stopped; });
        return th.jobDone;
    }

    void stop(Thread& th, VM& vm, Fiber* f, const std::string& reason, const std::string& text) {
        flushOut();  // output printed before the stop shows up first
        std::unique_lock<std::mutex> lk(mu);
        th.pauseRequested = false;
        th.frames = buildFrames(f);
        th.stopped = true;
        th.cmd = Cmd::None;
        (void)vm;
        Json body = Json::object();
        body.set("reason", reason);
        body.set("threadId", th.id);
        body.set("allThreadsStopped", false);
        if (!text.empty()) {
            body.set("description", "panic: " + text);
            body.set("text", text);
        }
        event("stopped", body);
        while (true) {
            cv.wait(lk, [&] { return th.cmd != Cmd::None || th.job; });
            if (th.cmd != Cmd::None) break;
            auto job = std::move(th.job);
            th.job = nullptr;
            lk.unlock();
            job();
            lk.lock();
            th.jobDone = true;
            cv.notify_all();
        }
        Cmd c = th.cmd;
        th.cmd = Cmd::None;
        th.stopped = false;
        th.frames.clear();
        for (auto& r : refs)
            if (r.thread == th.id) r.k = VarRef::Dead;
        bool anyStopped = false;
        for (auto& [id, t] : threads)
            if (t->stopped) anyStopped = true;
        if (!anyStopped) refs.clear();
        const CallFrame& fr = f->frames.back();
        switch (c) {
            case Cmd::Continue: th.mode = Mode::Run; break;
            case Cmd::StepIn: th.mode = Mode::StepIn; break;
            case Cmd::Next:
                th.mode = Mode::StepOver;
                th.stepDepth = chainDepth(f);
                th.stepFn = fr.closure->fn;
                th.stepLine = th.stepFn->chunk.lineAt(frameOffset(fr, true));
                break;
            case Cmd::StepOut:
                th.mode = Mode::StepOut;
                th.stepDepth = chainDepth(f);
                break;
            case Cmd::None: break;
        }
    }

    void runProgram() {
        gStdinDisabled = true;
        setOutputHook([this](const std::string& s) { output("stdout", s); });
        if (!cwd.empty()) {
            std::error_code ec;
            std::filesystem::current_path(cwd, ec);
        }
        int code = 1;
        {
            Program prog;
            program_ = &prog;
            prog.opts.typeCheck = typeCheck;
            Value argList = prog.vm.builtins[prog.vm.builtinIndex("args")].value;
            for (auto& x : args) as<ObjList>(argList)->push(prog.vm.vstr(x));
            prog.vm.onPanic = [this](const std::string& msg, const std::vector<StackEntry>& trace) {
                std::string s = "panic: " + msg + "\n";
                for (auto& e : trace) s += "    at " + e.function + " (" + e.file + ":" + std::to_string(e.line) + ")\n";
                output("stderr", s);
            };
            Diagnostics diags;
            ModuleUnit* m = prog.loadFile(program, diags, prog.opts);
            if (!m) {
                output("stderr", "ixo: can't read '" + program + "'\n");
            } else {
                for (auto& d : diags.list)
                    output(d.severity == Diagnostic::Error ? "stderr" : "console",
                           d.file + ":" + std::to_string(d.pos.line) + ":" + std::to_string(d.pos.col) + ": " +
                               (d.severity == Diagnostic::Error ? "error: " : "warning: ") + d.message + "\n");
                if (!diags.hasErrors()) {
                    Thread& mainThread = threadOf(prog.vm);
                    if (stopOnEntry) mainThread.mode = Mode::StepIn;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        if (pendingPause) mainThread.pauseRequested = true;
                    }
                    prog.vm.debugger = this;
                    bool ok = prog.run(m);
                    if (ok && !testName.empty()) {
                        // debug one @test function: run it after the module's top-level code
                        FnDecl* found = nullptr;
                        for (FnDecl* t : m->tests)
                            if (t->name == testName && t->decl) found = t;
                        if (!found) {
                            output("stderr", "no @test function named '" + testName + "' in " + program + "\n");
                            ok = false;
                        } else {
                            std::string msg = prog.vm.runTest(m->rt->globals[found->decl->globalIndex]);
                            flushOut();
                            if (msg.empty()) {
                                output("console", "test " + testName + " passed\n");
                            } else {
                                output("stderr", "test " + testName + " failed: " + msg + "\n");
                                ok = false;
                            }
                        }
                    }
                    prog.vm.debugger = nullptr;
                    program_ = nullptr;
                    flushOut();
                    if (ParRuntime* rt = ParRuntime::get()) {
                        if (ok) rt->shutdown();
                    }
                    code = ok ? prog.vm.exitCode : 1;
                }
            }
        }
        flushOut();
        Json ex = Json::object();
        ex.set("exitCode", code);
        event("exited", ex);
        event("terminated");
    }
};

}  // namespace

int runDebugAdapter() {
    static Adapter a;  // outlives the detached reader thread
    return a.run();
}

}  // namespace ixo
