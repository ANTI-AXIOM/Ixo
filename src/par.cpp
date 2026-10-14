#include "par.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_set>

#include "builtins.hpp"
#include "common.hpp"
#include "program.hpp"
#include "vm.hpp"

namespace ixo {

static std::atomic<int> nextParTaskId{1};

// ================================================================== encoding

struct Encoder {
    VM& vm;
    Packet& p;
    bool withGlobals;
    Fiber* fiber;
    std::unordered_map<Obj*, uint32_t> seen;
    std::unordered_set<ObjFunction*> fnSeen;
    std::set<std::pair<ModuleRt*, uint16_t>> globalSeen;
    std::vector<std::pair<ModuleRt*, uint16_t>> globalQueue;
    std::unordered_set<ModuleRt*> modSeen;

    Encoder(VM& vm, Packet& p, bool g) : vm(vm), p(p), withGlobals(g), fiber(vm.current) {}

    static ModuleRt* origin(ModuleRt* m) { return m && m->original ? m->original : m; }
    ModuleRt* localCopy(ModuleRt* orig) { return fiber && fiber->world ? vm.localModule(fiber, orig) : orig; }

    void noteModule(ModuleRt* orig) {
        if (!orig || !modSeen.insert(orig).second) return;
        ModuleRt* m = localCopy(orig);
        p.modules.push_back({orig, m->name, m->path, m->globalNames, m->exports});
    }

    void queueGlobal(ModuleRt* m, uint16_t idx) {
        ModuleRt* orig = origin(m);
        if (!orig || !globalSeen.insert({orig, idx}).second) return;
        noteModule(orig);
        globalQueue.push_back({orig, idx});
    }

    void addFunction(ObjFunction* fn) {
        if (!fn || !fnSeen.insert(fn).second) return;
        for (auto& c : fn->chunk.consts)
            if (isObjType(c, OT::Function)) addFunction(as<ObjFunction>(c));
        if (!withGlobals) return;
        for (uint16_t g : fn->globalsUsed) queueGlobal(fn->module, g);
    }

    [[noreturn]] void cant(Obj* o) {
        vm.panic("a " + vm.typeName(Value::object(o)) + " can't be sent between par tasks");
    }

    PValue enc(Value v) {
        PValue out;
        out.t = v.t;
        switch (v.t) {
            case VT::Bool: out.b = v.b; break;
            case VT::Int: out.i = v.i; break;
            case VT::Float: out.f = v.f; break;
            case VT::Obj: out.node = node(v.o); break;
            default: break;
        }
        return out;
    }

    uint32_t node(Obj* o) {
        auto it = seen.find(o);
        if (it != seen.end()) return it->second;
        uint32_t idx = (uint32_t)p.nodes.size();
        p.nodes.emplace_back();
        seen[o] = idx;
        PNode n;
        n.type = o->type;
        if (o->frozen) {
            // immutable and owned by no heap: every VM can use the same object
            n.frozen = true;
            n.ptr = o;
            p.nodes[idx] = std::move(n);
            return idx;
        }
        switch (o->type) {
            case OT::String: n.s = static_cast<ObjString*>(o)->s; break;
            case OT::List:
                for (Value x : SeqView(*static_cast<ObjList*>(o))) n.items.push_back(enc(x));
                break;
            case OT::Tuple:
                for (auto& x : static_cast<ObjTuple*>(o)->items) n.items.push_back(enc(x));
                break;
            case OT::Set:
                for (auto& x : static_cast<ObjSet*>(o)->items)
                    if (!x.isUndef()) n.items.push_back(enc(x));
                break;
            case OT::Map:
                for (auto& [k, x] : static_cast<ObjMap*>(o)->entries)
                    if (!k.isUndef()) n.pairs.push_back({enc(k), enc(x)});
                break;
            case OT::Range: {
                auto* r = static_cast<ObjRange*>(o);
                n.n0 = r->start;
                n.n1 = r->end;
                n.n2 = r->step;
                n.flag = r->inclusive;
                break;
            }
            case OT::Error: {
                auto* e = static_cast<ObjError*>(o);
                n.s = e->kind;
                n.s2 = e->msg;
                n.items.push_back(enc(e->payload));
                break;
            }
            case OT::Closure: {
                auto* c = static_cast<ObjClosure*>(o);
                n.ptr = c->fn;
                addFunction(c->fn);
                for (auto* u : c->upvalues) n.items.push_back(enc(u->fiber ? u->fiber->stack[u->slot] : u->closed));
                break;
            }
            case OT::Native: {
                auto* nat = static_cast<ObjNative*>(o);
                if (nat->path.empty()) cant(o);
                n.s = nat->path;
                break;
            }
            case OT::Bound: {
                auto* b = static_cast<ObjBound*>(o);
                n.items.push_back(enc(b->receiver));
                n.items.push_back(enc(b->method));
                break;
            }
            case OT::Class: {
                auto* c = static_cast<ObjClass*>(o);
                n.id = c->id;
                n.s = c->name;
                n.module = origin(c->module);
                n.n0 = c->ctorArity;
                n.n1 = c->ctorMinArity;
                n.strs = c->fields;
                n.ints = c->positional;
                for (auto& [sym, m] : c->methods) n.methods.push_back({sym, enc(m)});
                for (auto& [sym, pub] : c->methodPublic) n.methodPublic.push_back({sym, pub});
                n.items.push_back(c->ctor ? enc(Value::object(c->ctor)) : PValue());
                break;
            }
            case OT::Instance: {
                auto* inst = static_cast<ObjInstance*>(o);
                n.items.push_back(enc(Value::object(inst->cls)));
                for (auto& x : inst->fields) n.items.push_back(enc(x));
                break;
            }
            case OT::Channel: {
                auto* ch = static_cast<ObjChannel*>(o);
                makeChannelShared(vm, ch);
                n.shared = ch->shared;
                break;
            }
            case OT::Task: {
                auto* t = static_cast<ObjTask*>(o);
                if (!t->shared) vm.panic("tasks started with fork can't be sent between par tasks (start them with par)");
                n.shared = t->shared;
                n.n0 = t->id;
                break;
            }
            case OT::Module: {
                ModuleRt* orig = origin(static_cast<ObjModule*>(o)->rt);
                n.module = orig;
                noteModule(orig);
                if (withGlobals)
                    for (auto& [name, idx] : localCopy(orig)->exports) queueGlobal(orig, (uint16_t)idx);
                break;
            }
            case OT::Namespace: {
                auto* ns = static_cast<ObjNamespace*>(o);
                n.s = ns->name;
                // enums are namespaces made by the program: send their members
                if (vm.builtinIndex(ns->name) < 0)
                    for (auto& [k, v] : ns->members) n.pairs.push_back({enc(vm.vstr(k)), enc(v)});
                break;
            }
            case OT::Interface: {
                auto* i = static_cast<ObjInterface*>(o);
                n.s = i->name;
                n.ints = i->required;
                break;
            }
            case OT::Vec: n.nums = static_cast<ObjVec*>(o)->v; break;
            case OT::Mat: {
                auto* m = static_cast<ObjMat*>(o);
                n.n0 = m->rows;
                n.n1 = m->cols;
                n.nums = m->d;
                break;
            }
            case OT::Quat: {
                auto* q = static_cast<ObjQuat*>(o);
                n.nums = {q->x, q->y, q->z, q->w};
                break;
            }
            case OT::Time: n.nums = {static_cast<ObjTime*>(o)->epoch}; break;
            case OT::Rng: n.shared = std::make_shared<std::mt19937_64>(static_cast<ObjRng*>(o)->gen); break;
            default: cant(o);
        }
        p.nodes[idx] = std::move(n);
        return idx;
    }

    void finish() {
        for (size_t i = 0; i < globalQueue.size(); i++) {
            auto [orig, idx] = globalQueue[i];
            ModuleRt* m = localCopy(orig);
            Value v = idx < m->globals.size() ? m->globals[idx] : Value::undef();
            PValue pv = enc(v);  // may queue more globals
            p.globals.push_back({orig, idx, pv});
        }
    }
};

size_t packetBytes(const Packet& p) {
    size_t n = 0;
    for (auto& nd : p.nodes)
        n += 32 + nd.s.size() + nd.s2.size() + nd.items.size() * sizeof(PValue) +
             nd.pairs.size() * 2 * sizeof(PValue) + nd.nums.size() * sizeof(double);
    return n + (p.globals.size() + p.roots.size()) * sizeof(PValue);
}

Packet encodeValues(VM& vm, const std::vector<Value>& roots, bool globals) {
    Packet p;
    Encoder e(vm, p, globals);
    for (auto& v : roots) p.roots.push_back(e.enc(v));
    e.finish();
    return p;
}

// ================================================================== decoding

struct Decoder {
    VM& vm;
    Fiber* f;
    const Packet& p;
    std::vector<Value> vals;
    std::vector<bool> reused;

    Decoder(VM& vm, Fiber* f, const Packet& p) : vm(vm), f(f), p(p) {}

    Value val(const PValue& pv) {
        Value v;
        v.t = pv.t;
        switch (pv.t) {
            case VT::Bool: v.b = pv.b; break;
            case VT::Int: v.i = pv.i; break;
            case VT::Float: v.f = pv.f; break;
            case VT::Obj: v = vals[pv.node]; break;
            default: break;
        }
        return v;
    }

    ModuleRt* worldModule(ModuleRt* orig) {
        if (!orig) return nullptr;
        if (!f || !f->world || orig->owner == &vm) return orig;
        auto& slot = f->world->modules[orig];
        if (!slot) {
            slot = std::make_unique<ModuleRt>();
            slot->owner = &vm;
            slot->original = orig;
            for (auto& meta : p.modules)
                if (meta.original == orig) {
                    slot->name = meta.name;
                    slot->path = meta.path;
                    slot->globalNames = meta.globalNames;
                    slot->exports = meta.exports;
                    slot->globals.assign(meta.globalNames.size(), Value::undef());
                }
        }
        return slot.get();
    }

    Value resolveNative(const std::string& path) {
        auto nsOf = [&](const std::string& name) -> ObjNamespace* {
            int idx = vm.builtinIndex(name);
            if (idx < 0 || !isObjType(vm.builtins[idx].value, OT::Namespace)) return nullptr;
            return as<ObjNamespace>(vm.builtins[idx].value);
        };
        std::string rest = path.substr(2);
        switch (path[0]) {
            case 'b': {
                int idx = vm.builtinIndex(rest);
                if (idx >= 0) return vm.builtins[idx].value;
                break;
            }
            case 'n': {
                size_t dot = rest.find('.');
                if (ObjNamespace* ns = nsOf(rest.substr(0, dot))) {
                    auto it = ns->members.find(rest.substr(dot + 1));
                    if (it != ns->members.end()) return it->second;
                }
                break;
            }
            case 'c':
                if (ObjNamespace* ns = nsOf(rest)) return ns->callable;
                break;
            case 'm': {
                size_t colon = rest.find(':');
                int tag = std::atoi(rest.substr(0, colon).c_str());
                auto& table = vm.methods[tag];
                auto it = table.find(VM::sym(rest.substr(colon + 1)));
                if (it != table.end()) return it->second.fn;
                break;
            }
            case 'e':
                if (ObjNamespace* ns = nsOf("Error"))
                    if (ns->getFallback) return ns->getFallback(vm, ns, rest, true);
                break;
        }
        vm.panic("internal: can't find the function '" + path + "' in a par task");
    }

    std::vector<Value> run() {
        size_t n = p.nodes.size();
        vals.assign(n, Value());
        reused.assign(n, false);
        // pass 1a: classes (instances need them to exist)
        for (size_t i = 0; i < n; i++) {
            const PNode& nd = p.nodes[i];
            if (nd.type != OT::Class || nd.frozen) continue;
            auto it = vm.classById.find(nd.id);
            if (it != vm.classById.end()) {
                vals[i] = Value::object(it->second);
                reused[i] = true;
                continue;
            }
            auto* c = vm.alloc<ObjClass>();
            c->id = nd.id;
            c->name = nd.s;
            c->fields = nd.strs;
            for (size_t k = 0; k < nd.strs.size(); k++) c->fieldIndex[VM::sym(nd.strs[k])] = (int)k;
            c->positional = nd.ints;
            c->module = nd.module;
            c->ctorArity = (int)nd.n0;
            c->ctorMinArity = (int)nd.n1;
            vm.classById[c->id] = c;
            vals[i] = Value::object(c);
        }
        // pass 1b: every other object
        for (size_t i = 0; i < n; i++) {
            const PNode& nd = p.nodes[i];
            if (nd.frozen) {
                vals[i] = Value::object(static_cast<Obj*>(const_cast<void*>(nd.ptr)));
                reused[i] = true;
                continue;
            }
            Obj* o = nullptr;
            switch (nd.type) {
                case OT::Class: continue;
                case OT::String: o = vm.str(nd.s); break;
                case OT::List: o = vm.alloc<ObjList>(); break;
                case OT::Tuple: o = vm.newTupleN(nd.items.size()); break;
                case OT::Set: o = vm.alloc<ObjSet>(); break;
                case OT::Map: o = vm.alloc<ObjMap>(); break;
                case OT::Range: o = vm.alloc<ObjRange>(nd.n0, nd.n1, nd.n2, nd.flag); break;
                case OT::Error: o = vm.alloc<ObjError>(nd.s, nd.s2); break;
                case OT::Closure:
                    o = vm.alloc<ObjClosure>(const_cast<ObjFunction*>(static_cast<const ObjFunction*>(nd.ptr)));
                    break;
                case OT::Native: vals[i] = resolveNative(nd.s); continue;
                case OT::Bound: o = vm.alloc<ObjBound>(Value(), Value()); break;
                case OT::Instance: {
                    Value cls = vals[nd.items[0].node];
                    o = vm.newInstance(as<ObjClass>(cls));
                    break;
                }
                case OT::Channel: {
                    auto sc = std::static_pointer_cast<SharedChan>(nd.shared);
                    auto* ch = vm.alloc<ObjChannel>(sc->cap);
                    ch->shared = sc;
                    o = ch;
                    break;
                }
                case OT::Task: {
                    auto* t = vm.alloc<ObjTask>((int)nd.n0);
                    t->shared = std::static_pointer_cast<SharedTask>(nd.shared);
                    o = t;
                    break;
                }
                case OT::Module: {
                    ModuleRt* m = worldModule(nd.module);
                    if (!m->obj) m->obj = vm.alloc<ObjModule>(m);
                    o = m->obj;
                    break;
                }
                case OT::Namespace: {
                    int idx = vm.builtinIndex(nd.s);
                    if (idx >= 0 && nd.pairs.empty()) {
                        vals[i] = vm.builtins[idx].value;
                        continue;
                    }
                    o = vm.alloc<ObjNamespace>(nd.s);
                    break;
                }
                case OT::Interface: {
                    auto* itf = vm.alloc<ObjInterface>();
                    itf->name = nd.s;
                    itf->required = nd.ints;
                    o = itf;
                    break;
                }
                case OT::Vec: {
                    auto* v = vm.alloc<ObjVec>();
                    v->v = nd.nums;
                    o = v;
                    break;
                }
                case OT::Mat: {
                    auto* m = vm.alloc<ObjMat>();
                    m->rows = (int)nd.n0;
                    m->cols = (int)nd.n1;
                    m->d = nd.nums;
                    o = m;
                    break;
                }
                case OT::Quat: {
                    auto* q = vm.alloc<ObjQuat>();
                    q->x = nd.nums[0];
                    q->y = nd.nums[1];
                    q->z = nd.nums[2];
                    q->w = nd.nums[3];
                    o = q;
                    break;
                }
                case OT::Time: o = vm.alloc<ObjTime>(nd.nums[0]); break;
                case OT::Rng: {
                    auto* r = vm.alloc<ObjRng>(0);
                    r->gen = *std::static_pointer_cast<std::mt19937_64>(nd.shared);
                    o = r;
                    break;
                }
                default: vm.panic("internal: bad packet");
            }
            vals[i] = Value::object(o);
        }
        // pass 2: contents (maps and sets last: their keys must be complete to hash)
        for (int round = 0; round < 2; round++) {
            for (size_t i = 0; i < n; i++) {
                const PNode& nd = p.nodes[i];
                bool hashed = nd.type == OT::Map || nd.type == OT::Set;
                if (hashed != (round == 1) || nd.frozen) continue;
                Obj* o = vals[i].isObj() ? vals[i].o : nullptr;
                switch (nd.type) {
                    case OT::List:
                        for (auto& x : nd.items) static_cast<ObjList*>(o)->push(val(x));
                        break;
                    case OT::Tuple:
                        for (size_t k = 0; k < nd.items.size(); k++) static_cast<ObjTuple*>(o)->items[k] = val(nd.items[k]);
                        break;
                    case OT::Set:
                        for (auto& x : nd.items) static_cast<ObjSet*>(o)->add(val(x));
                        break;
                    case OT::Map:
                        for (auto& [k, x] : nd.pairs) static_cast<ObjMap*>(o)->set(val(k), val(x));
                        break;
                    case OT::Namespace:
                        if (o && !nd.pairs.empty() && o->heapId == vm.heapId && vm.builtinIndex(nd.s) < 0)
                            for (auto& [k, x] : nd.pairs)
                                static_cast<ObjNamespace*>(o)->members[as<ObjString>(val(k))->s] = val(x);
                        break;
                    case OT::Error: static_cast<ObjError*>(o)->payload = val(nd.items[0]); break;
                    case OT::Closure: {
                        auto* c = static_cast<ObjClosure*>(o);
                        for (size_t k = 0; k < nd.items.size() && k < c->upvalues.size(); k++) {
                            auto* u = vm.alloc<ObjUpvalue>();
                            u->closed = val(nd.items[k]);
                            c->upvalues[k] = u;
                        }
                        break;
                    }
                    case OT::Bound: {
                        auto* b = static_cast<ObjBound*>(o);
                        b->receiver = val(nd.items[0]);
                        b->method = val(nd.items[1]);
                        break;
                    }
                    case OT::Class: {
                        if (reused[i]) break;
                        auto* c = static_cast<ObjClass*>(o);
                        for (auto& [sym, m] : nd.methods) c->methods[sym] = val(m);
                        for (auto& [sym, pub] : nd.methodPublic) c->methodPublic[sym] = pub;
                        Value ctor = val(nd.items[0]);
                        c->ctor = isObjType(ctor, OT::Closure) ? as<ObjClosure>(ctor) : nullptr;
                        break;
                    }
                    case OT::Instance: {
                        auto* inst = static_cast<ObjInstance*>(o);
                        for (size_t k = 1; k < nd.items.size() && k - 1 < inst->fields.size(); k++)
                            inst->fields[k - 1] = val(nd.items[k]);
                        break;
                    }
                    default: break;
                }
            }
        }
        // module variables used by the code, into this task's snapshot
        if (f && f->world) {
            for (auto& g : p.globals) {
                ModuleRt* m = worldModule(g.module);
                if (m == g.module) continue;
                if (m->globals.size() <= g.index) m->globals.resize(g.index + 1, Value::undef());
                if (m->globals[g.index].isUndef()) m->globals[g.index] = val(g.value);
            }
        }
        std::vector<Value> out;
        for (auto& r : p.roots) out.push_back(val(r));
        return out;
    }
};

std::vector<Value> decodeValues(VM& vm, Fiber* f, const Packet& p) {
    Decoder d(vm, f, p);
    return d.run();
}

// ================================================================== shared flows

void makeChannelShared(VM& vm, ObjChannel* ch) {
    if (ch->shared) return;
    auto sc = std::make_shared<SharedChan>();
    sc->cap = ch->cap;
    sc->closed = ch->closed;
    for (auto& v : ch->buf) sc->buf.push_back(encodeValues(vm, {v}, true));
    for (auto& w : ch->recvWaiters) sc->recvWaiters.push_back({&vm, w.fiber->serial, w.forIter, w.sel, w.index});
    for (auto& [f, v] : ch->sendWaiters) sc->sendWaiters.push_back({{&vm, f->serial, true, nullptr, -1}, encodeValues(vm, {v}, true)});
    ch->buf.clear();
    ch->recvWaiters.clear();
    ch->sendWaiters.clear();
    ch->shared = sc;
}

static void wakeRemote(const RemoteWaiter& w, Packet pkt, bool push) {
    Message msg;
    msg.kind = Message::Wake;
    msg.serial = w.serial;
    msg.push = push;
    msg.selectIndex = w.sel ? w.index : -1;
    msg.packet = std::move(pkt);
    w.vm->par->post(w.vm, std::move(msg));
}

static Packet nilPacket() {
    Packet p;
    p.roots.push_back(PValue());
    return p;
}

static Packet donePacket() {
    Packet p;
    PValue d;
    d.t = VT::Done;
    p.roots.push_back(d);
    return p;
}

void sharedSend(VM& vm, ObjChannel* ch, Value v) {
    SharedChan& sc = *ch->shared;
    Packet pkt = encodeValues(vm, {v}, true);
    std::unique_lock<std::mutex> lk(sc.m);
    if (sc.closed) {
        lk.unlock();
        vm.panic("can't send to a closed flow");
    }
    while (!sc.recvWaiters.empty()) {
        RemoteWaiter w = sc.recvWaiters.front();
        sc.recvWaiters.pop_front();
        if (w.sel && !w.sel->tryFire()) continue;  // a select that took something else
        lk.unlock();
        wakeRemote(w, std::move(pkt), true);
        return;
    }
    if ((int)sc.buf.size() < sc.cap) {
        sc.buf.push_back(std::move(pkt));
        return;
    }
    sc.sendWaiters.push_back({{&vm, vm.current->serial, true, nullptr, -1}, std::move(pkt)});
    lk.unlock();
    vm.blockCurrent();
}

Value sharedRecv(VM& vm, ObjChannel* ch, bool forIter, bool* blocked) {
    SharedChan& sc = *ch->shared;
    *blocked = false;
    std::unique_lock<std::mutex> lk(sc.m);
    if (!sc.buf.empty()) {
        Packet pkt = std::move(sc.buf.front());
        sc.buf.pop_front();
        if (!sc.sendWaiters.empty()) {
            auto sw = std::move(sc.sendWaiters.front());
            sc.sendWaiters.pop_front();
            sc.buf.push_back(std::move(sw.second));
            lk.unlock();
            wakeRemote(sw.first, nilPacket(), true);
        } else {
            lk.unlock();
        }
        return decodeValues(vm, vm.current, pkt)[0];
    }
    if (!sc.sendWaiters.empty()) {
        auto sw = std::move(sc.sendWaiters.front());
        sc.sendWaiters.pop_front();
        lk.unlock();
        wakeRemote(sw.first, nilPacket(), true);
        return decodeValues(vm, vm.current, sw.second)[0];
    }
    if (sc.closed) return forIter ? Value::done() : Value();
    sc.recvWaiters.push_back({&vm, vm.current->serial, forIter, nullptr, -1});
    lk.unlock();
    vm.blockCurrent();
    *blocked = true;
    return Value();
}

// Registers a select on a shared flow. Returns true (without registering) if
// the flow has something ready right now.
bool registerSharedSelect(VM& vm, ObjChannel* ch, const std::shared_ptr<SelectState>& sel, int index) {
    SharedChan& sc = *ch->shared;
    std::lock_guard<std::mutex> lk(sc.m);
    if (!sc.buf.empty() || !sc.sendWaiters.empty() || sc.closed) return true;
    sc.recvWaiters.push_back({&vm, vm.current->serial, false, sel, index});
    return false;
}

void externalSend(const std::shared_ptr<SharedChan>& sc, Packet pkt) {
    std::unique_lock<std::mutex> lk(sc->m);
    if (sc->closed) return;
    while (!sc->recvWaiters.empty()) {
        RemoteWaiter w = sc->recvWaiters.front();
        sc->recvWaiters.pop_front();
        if (w.sel && !w.sel->tryFire()) continue;
        lk.unlock();
        wakeRemote(w, std::move(pkt), true);
        return;
    }
    sc->buf.push_back(std::move(pkt));
}

void sharedClose(VM& vm, ObjChannel* ch) {
    (void)vm;
    SharedChan& sc = *ch->shared;
    std::deque<RemoteWaiter> recv;
    std::deque<std::pair<RemoteWaiter, Packet>> send;
    {
        std::lock_guard<std::mutex> lk(sc.m);
        if (sc.closed) return;
        sc.closed = true;
        recv.swap(sc.recvWaiters);
        send.swap(sc.sendWaiters);
    }
    for (auto& w : recv) {
        if (w.sel && !w.sel->tryFire()) continue;
        wakeRemote(w, w.flag ? donePacket() : nilPacket(), true);
    }
    for (auto& [w, pkt] : send) wakeRemote(w, nilPacket(), true);
}

// ================================================================== shared tasks

bool sharedTaskDone(ObjTask* t) {
    std::lock_guard<std::mutex> lk(t->shared->m);
    return t->shared->done;
}

Value sharedAwait(VM& vm, ObjTask* t, bool pushResult, bool* blocked) {
    SharedTask& st = *t->shared;
    *blocked = false;
    std::unique_lock<std::mutex> lk(st.m);
    if (st.done) {
        Packet copy = st.result;
        lk.unlock();
        return decodeValues(vm, vm.current, copy)[0];
    }
    st.waiters.push_back({&vm, vm.current->serial, pushResult, nullptr, -1});
    lk.unlock();
    vm.blockCurrent();
    *blocked = true;
    return Value();
}

void sharedComplete(VM& vm, ObjTask* t, Value result) {
    SharedTask& st = *t->shared;
    Packet pkt = encodeValues(vm, {result}, false);
    if (ParRuntime* rt = ParRuntime::get()) rt->bytesOut += packetBytes(pkt);
    std::vector<RemoteWaiter> waiters;
    {
        std::lock_guard<std::mutex> lk(st.m);
        st.done = true;
        st.result = pkt;
        waiters.swap(st.waiters);
    }
    for (auto& w : waiters) wakeRemote(w, pkt, w.flag);
}

// ================================================================== runtime

static std::atomic<ParRuntime*> runtimeInstance{nullptr};

ParRuntime* ParRuntime::get() { return runtimeInstance.load(); }

ParRuntime* ParRuntime::start(VM& main, int n) {
    auto* rt = new ParRuntime();
    rt->mainVM = &main;
    main.par = rt;
    rt->busy = 1 + n;
    std::vector<std::string> args = main.programArgs;
    int ready = 0;
    for (int i = 0; i < n; i++) {
        auto w = std::make_unique<Worker>();
        Worker* wp = w.get();
        rt->workers.push_back(std::move(w));
        wp->thread = std::thread([rt, wp, args, &ready]() {
            auto prog = std::make_unique<Program>();
            VM& vm = prog->vm;
            vm.isWorker = true;
            vm.jitOn = false;  // workers never compile: no counters shared with the main program
            vm.programArgs = args;
            int ai = vm.builtinIndex("args");
            if (ai >= 0 && isObjType(vm.builtins[ai].value, OT::List))
                for (auto& a : args) as<ObjList>(vm.builtins[ai].value)->push(vm.vstr(a));
            VM* mainVm = rt->mainVM;
            if (mainVm->profiler) {
                wp->profiler = std::make_unique<Profiler>();
                wp->profiler->start();
                vm.profiler = wp->profiler.get();
            }
            vm.debugger = mainVm->debugger;  // breakpoints work in par tasks too
            {
                std::lock_guard<std::mutex> lk(rt->m);
                vm.par = rt;
                wp->vm = &vm;
                ready++;
                rt->cv.notify_all();
            }
            vm.workerLoop();
            if (vm.profiler) vm.profiler->stop();
        });
    }
    {
        std::unique_lock<std::mutex> lk(rt->m);
        rt->cv.wait(lk, [&] { return ready == n; });
    }
    runtimeInstance.store(rt);
    return rt;
}

void ParRuntime::post(VM* target, Message msg) {
    std::lock_guard<std::mutex> lk(m);
    target->inbox.push_back(std::move(msg));
    if (target->idle) {
        target->idle = false;
        busy++;
    }
    cv.notify_all();
}

void ParRuntime::submit(Message start) {
    VM* target;
    {
        std::lock_guard<std::mutex> lk(m);
        // prefer an idle worker, otherwise round robin
        target = nullptr;
        for (size_t k = 0; k < workers.size(); k++) {
            Worker* w = workers[(nextWorker + k) % workers.size()].get();
            if (w->vm->idle && w->vm->inbox.empty()) {
                target = w->vm;
                nextWorker = (nextWorker + k + 1) % workers.size();
                break;
            }
        }
        if (!target) {
            target = workers[nextWorker % workers.size()]->vm;
            nextWorker = (nextWorker + 1) % workers.size();
        }
    }
    post(target, std::move(start));
}

bool ParRuntime::waitForWork(VM& vm, double timeoutSec) {
    std::unique_lock<std::mutex> lk(m);
    if (!vm.inbox.empty() || stopping) return true;
    if (timeoutSec >= 0) {
        // sleeping tasks will wake up: stay counted as busy
        cv.wait_for(lk, std::chrono::duration<double>(timeoutSec), [&] { return !vm.inbox.empty() || stopping; });
        return true;
    }
    vm.idle = true;
    busy--;
    cv.notify_all();
    while (vm.inbox.empty() && !stopping) {
        if (&vm == mainVM && busy == 0 && external == 0) {
            vm.idle = false;
            busy++;
            return false;
        }
        cv.wait(lk);
    }
    if (vm.idle) {
        vm.idle = false;
        busy++;
    }
    return true;
}

void ParRuntime::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m);
        stopping = true;
        cv.notify_all();
    }
    for (auto& w : workers)
        if (w->thread.joinable()) w->thread.join();
}

// ================================================================== VM side

ModuleRt* VM::localModule(Fiber* f, ModuleRt* m) {
    if (!f || !f->world || !m || m->owner == this) return m;
    auto& slot = f->world->modules[m];
    if (!slot) {
        slot = std::make_unique<ModuleRt>();
        slot->owner = this;
        slot->original = m;
        slot->name = m->name;
    }
    return slot.get();
}

ParRuntime* VM::ensurePar() {
    if (!par) {
        int n = (int)std::thread::hardware_concurrency();
        if (const char* e = std::getenv("IXO_WORKERS")) n = std::atoi(e);
        if (n < 1) n = 1;
#ifdef __EMSCRIPTEN__
        // browser threads come from a pool made at startup (PTHREAD_POOL_SIZE in
        // playground/build.sh, one of them runs main): more would wait forever
        if (n > 3) n = 3;
#endif
        ParRuntime::start(*this, n);
    }
    return par;
}

ObjTask* VM::parSpawn(Value callee, const Value* args, int argc) {
    ensurePar();
    std::vector<Value> roots{callee};
    for (int i = 0; i < argc; i++) roots.push_back(args[i]);
    Message msg;
    msg.kind = Message::Start;
    msg.packet = encodeValues(*this, roots, true);
    par->tasksStarted++;
    par->bytesIn += packetBytes(msg.packet);
    msg.task = std::make_shared<SharedTask>();
    msg.task->id = nextParTaskId.fetch_add(1);
    auto* t = alloc<ObjTask>(msg.task->id);
    t->shared = msg.task;
    if (current && !current->groups.empty()) current->groups.back().push_back(t);
    par->submit(std::move(msg));
    return t;
}

void VM::drainInbox() {
    std::deque<Message> msgs;
    {
        std::lock_guard<std::mutex> lk(par->m);
        if (inbox.empty()) return;
        msgs.swap(inbox);
    }
    for (auto& msg : msgs) {
        if (msg.kind == Message::Start) {
            Fiber* nf = newFiber();
            nf->world = std::make_shared<World>();
            std::vector<Value> vals = decodeValues(*this, nf, msg.packet);
            for (auto& v : vals) nf->push(v);
            nf->startArgc = (int)vals.size() - 1;
            auto* t = alloc<ObjTask>(msg.task->id);
            t->shared = msg.task;
            t->fiber = nf;
            nf->task = t;
            enqueue(nf);
            continue;
        }
        Fiber* f = fiberFor(msg.serial);
        if (!f || f->state == FState::Done) continue;
        Value v;
        if (msg.push) v = decodeValues(*this, f, msg.packet)[0];
        if (msg.selectIndex >= 0) v = selectResult(msg.selectIndex, v);
        wake(f, v, msg.push);
    }
}

void VM::workerLoop() {
    while (!par->stopping) {
        try {
            schedule();
        } catch (PanicError& e) {
            lastTrace = captureTrace(current);
            if (onPanic) onPanic("in a par task: " + e.msg, lastTrace);
            flushOut();
            std::fflush(stderr);
            hardExit(1);
        }
    }
}

}  // namespace ixo
