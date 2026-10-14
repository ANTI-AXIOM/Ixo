// Heap objects managed by the garbage collector, and the bytecode chunk.
#pragma once

#include <atomic>
#include <deque>
#include <iterator>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "value.hpp"

namespace ixo {

class VM;
struct Fiber;
struct ModuleRt;

enum class OT : uint8_t {
    String, List, Tuple, Map, Set, Function, Closure, Upvalue, Native, Bound, Class, Instance,
    Range, Error, Channel, Task, Module, Namespace, Iterator, Vec, Mat, Quat, Rng, Time, TypeDesc,
    Marker,  // internal: start of a memory region
    Interface,
    GpuBuffer,
    Free,    // internal: an empty slot in the VM's object pool
};

// Tags used for per-type method tables and type patterns.
enum class Tag : uint8_t {
    Nil, Bool, Int, Float, Str, List, Tuple, Map, Set, Range, Fn, Class, Instance, Error, Flow,
    Task, Module, Vec, Mat, Quat, Rng, Time, Iter, GpuBuf, COUNT
};

// Frozen values (freeze(x)) belong to no heap: no GC marks or frees them, and
// par tasks share them instead of copying.
constexpr uint16_t kFrozenHeap = 0xFFFF;

// The header every heap object starts with: 12 bytes (no vtable: freeing
// switches on the type; no list link: the collector walks the pools).
struct Obj {
    OT type;
    bool marked = false;
    bool frozen = false;  // deeply read-only; changing it panics
    uint8_t sizeClass = 0;  // how its memory was obtained (VM::freeObj): 0 new, 255 malloc, else a pool
    uint16_t heapId = 0;  // which VM's heap owns it (each par worker has its own heap)
    // Strings: who may grow it in place (seen by no one else yet): 0 nobody, 1 the loop
    // that made it (STR_APPEND), 2 the module variable that holds it (STR_APPEND_G).
    uint8_t appendable = 0;
    // Per type: strings remember whether they are ASCII (0 not known yet, 1 ASCII,
    // 2 not), which makes length, indexing and slicing O(1) for them.
    mutable uint8_t kind = 0;
    uint32_t size = 0;    // approximate bytes, for GC pacing
    explicit Obj(OT t) : type(t) {}
};

struct ObjInterface : Obj {
    std::string name;
    std::vector<int> required;  // method symbols a class must have
    ObjInterface() : Obj(OT::Interface) {}
};

struct ObjMarker : Obj {
    ObjMarker() : Obj(OT::Marker) {}
};

struct ObjString : Obj {
    mutable uint32_t hashv = 0;  // 0: not computed yet (it fits in the header's spare bytes)
    std::string s;
    explicit ObjString(std::string v) : Obj(OT::String), s(std::move(v)) {}
    size_t hash() const {
        if (!hashv) {
            uint64_t h = std::hash<std::string>()(s);
            uint32_t x = (uint32_t)(h ^ (h >> 32));
            hashv = x ? x : 1;
        }
        return hashv;
    }
};

// A list stores its items in the narrowest form that fits all of them (its kind,
// in the header byte): bools 1 byte, ints 8, floats 8, anything else as 16-byte
// values. A value that doesn't fit turns the list into `values` once, for good.
// Every kind is a std::vector of its element type (the same three pointers), so
// machine code only needs the kind to know the element size.
enum ListKind : uint8_t { LK_VALUES = 0, LK_BOOLS = 1, LK_INTS = 2, LK_FLOATS = 3, LK_EMPTY = 4 };

struct ObjList : Obj {
    union {
        std::vector<Value> vals;   // LK_VALUES and LK_EMPTY (no kind chosen yet)
        std::vector<uint8_t> bools;
        std::vector<int64_t> ints;
        std::vector<double> floats;
    };
    ObjList() : Obj(OT::List) {
        new (&vals) std::vector<Value>();
        kind = LK_EMPTY;
    }
    ~ObjList() { destroyStore(); }
    ObjList(const ObjList&) = delete;
    ListKind lk() const { return (ListKind)kind; }
    bool typed() const { return kind >= LK_BOOLS && kind <= LK_FLOATS; }
    size_t len() const {
        switch (kind) {
            case LK_BOOLS: return bools.size();
            case LK_INTS: return ints.size();
            case LK_FLOATS: return floats.size();
            default: return vals.size();
        }
    }
    size_t elemSize() const { return kind == LK_BOOLS ? 1 : (kind == LK_INTS || kind == LK_FLOATS) ? 8 : sizeof(Value); }
    Value get(size_t i) const {
        switch (kind) {
            case LK_BOOLS: return Value::boolean(bools[i] != 0);
            case LK_INTS: return Value::integer(ints[i]);
            case LK_FLOATS: return Value::number(floats[i]);
            default: return vals[i];
        }
    }
    // the kind a single value would make an empty list
    static ListKind kindFor(const Value& v) {
        return v.t == VT::Bool ? LK_BOOLS : v.t == VT::Int ? LK_INTS : v.t == VT::Float ? LK_FLOATS : LK_VALUES;
    }
    bool fits(const Value& v) const {
        switch (kind) {
            case LK_BOOLS: return v.t == VT::Bool;
            case LK_INTS: return v.t == VT::Int;
            case LK_FLOATS: return v.t == VT::Float;
            default: return true;
        }
    }
    void set(size_t i, const Value& v) {
        if (!fits(v)) widen();
        switch (kind) {
            case LK_BOOLS: bools[i] = v.b ? 1 : 0; break;
            case LK_INTS: ints[i] = v.i; break;
            case LK_FLOATS: floats[i] = v.f; break;
            default: vals[i] = v; break;
        }
    }
    void push(const Value& v) {
        if (kind == LK_EMPTY && vals.empty()) become(kindFor(v));
        if (!fits(v)) widen();
        switch (kind) {
            case LK_BOOLS: bools.push_back(v.b ? 1 : 0); break;
            case LK_INTS: ints.push_back(v.i); break;
            case LK_FLOATS: floats.push_back(v.f); break;
            default: vals.push_back(v); break;
        }
    }
    Value pop() {
        Value v = get(len() - 1);
        switch (kind) {
            case LK_BOOLS: bools.pop_back(); break;
            case LK_INTS: ints.pop_back(); break;
            case LK_FLOATS: floats.pop_back(); break;
            default: vals.pop_back(); break;
        }
        return v;
    }
    void reserve(size_t n) {
        switch (kind) {
            case LK_BOOLS: bools.reserve(n); break;
            case LK_INTS: ints.reserve(n); break;
            case LK_FLOATS: floats.reserve(n); break;
            default: vals.reserve(n); break;
        }
    }
    size_t capacity() const {
        switch (kind) {
            case LK_BOOLS: return bools.capacity();
            case LK_INTS: return ints.capacity();
            case LK_FLOATS: return floats.capacity();
            default: return vals.capacity();
        }
    }
    void shrinkToFit() {
        switch (kind) {
            case LK_BOOLS: bools.shrink_to_fit(); break;
            case LK_INTS: ints.shrink_to_fit(); break;
            case LK_FLOATS: floats.shrink_to_fit(); break;
            default: vals.shrink_to_fit(); break;
        }
    }
    void clear() {  // and its memory
        destroyStore();
        new (&vals) std::vector<Value>();
        kind = LK_EMPTY;
    }
    // Takes items, choosing the narrowest kind that fits them all.
    void adopt(std::vector<Value>&& v) {
        ListKind k = v.empty() ? LK_EMPTY : kindFor(v[0]);
        for (size_t i = 1; i < v.size() && k != LK_VALUES; i++)
            if (kindFor(v[i]) != k) k = LK_VALUES;
        destroyStore();
        new (&vals) std::vector<Value>();
        kind = k == LK_EMPTY ? LK_EMPTY : LK_VALUES;
        if (k == LK_VALUES || k == LK_EMPTY) {
            vals = std::move(v);
            return;
        }
        become(k);
        reserve(v.size());
        for (auto& x : v) push(x);
    }
    // This (new, empty) list becomes `times` copies of src, at src's kind.
    void assignRepeat(const ObjList& src, size_t times) {
        auto rep = [&](auto& dst, const auto& from) {
            dst.reserve(from.size() * times);
            if (from.size() == 1) dst.assign(times, from[0]);
            else
                for (size_t t = 0; t < times; t++) dst.insert(dst.end(), from.begin(), from.end());
        };
        become(src.lk() == LK_EMPTY || !times ? LK_EMPTY : src.lk());
        switch (kind) {
            case LK_BOOLS: rep(bools, src.bools); break;
            case LK_INTS: rep(ints, src.ints); break;
            case LK_FLOATS: rep(floats, src.floats); break;
            case LK_EMPTY: break;
            default: rep(vals, src.vals); break;
        }
    }
    // Every item as a 16-byte value: what code that wants a std::vector<Value> gets.
    // A typed list turns into `values` for good, so references stay valid.
    std::vector<Value>& items() {
        if (kind != LK_VALUES) widen();
        return vals;
    }
    void widen() {
        if (kind == LK_VALUES) return;
        if (kind == LK_EMPTY) {
            kind = LK_VALUES;
            return;
        }
        std::vector<Value> out;
        out.reserve(len());
        for (size_t i = 0; i < len(); i++) out.push_back(get(i));
        destroyStore();
        new (&vals) std::vector<Value>(std::move(out));
        kind = LK_VALUES;
    }

private:
    // an empty list takes kind k
    void become(ListKind k) {
        destroyStore();
        switch (k) {
            case LK_BOOLS: new (&bools) std::vector<uint8_t>(); break;
            case LK_INTS: new (&ints) std::vector<int64_t>(); break;
            case LK_FLOATS: new (&floats) std::vector<double>(); break;
            default: new (&vals) std::vector<Value>(); break;
        }
        kind = k;
    }
    void destroyStore() {
        switch (kind) {
            case LK_BOOLS: bools.~vector(); break;
            case LK_INTS: ints.~vector(); break;
            case LK_FLOATS: floats.~vector(); break;
            default: vals.~vector(); break;
        }
    }
};

// (tuples: see ObjTuple below, next to FieldArrayAt)

// Hashing / equality for map keys and set elements (1 and 1.0 are the same key).
size_t hashValue(const Value& v);
bool keyEquals(const Value& a, const Value& b);
struct ValueHash {
    size_t operator()(const Value& v) const { return hashValue(v); }
};
struct ValueKeyEq {
    bool operator()(const Value& a, const Value& b) const { return keyEquals(a, b); }
};

// The hash index of a map or set: positions of the keys in its own vector
// (which holds the keys). Small collections have no index: a lookup scans the
// keys, which is faster than hashing for a few of them and allocates nothing.
// Bigger ones use open addressing; a slot is (hash << 32) | (position + 2),
// with 0 for empty and 1 for deleted.
struct KeyIndex {
    static constexpr size_t kScan = 8;  // up to this many keys: no index
    std::vector<uint64_t> table;
    size_t filled = 0;  // slots not empty (live or deleted)

    bool active() const { return !table.empty(); }
    void clear() {
        table.clear();
        filled = 0;
    }
    // As Python's dict: the key's own hash (an int is itself), so nearby keys sit in
    // nearby slots, and a probe sequence that mixes in the hash's high bits, so keys
    // that differ only there (multiples of 1024...) don't pile up.
    static uint32_t hashOf(const Value& k) {
        uint64_t x = hashValue(k);
        return (uint32_t)(x ^ (x >> 32));
    }
    static size_t nextSlot(size_t i, uint32_t& perturb, size_t mask) {
        perturb >>= 5;
        return (i * 5 + 1 + perturb) & mask;
    }
    // The position of k, or -1. keyAt(pos) gives the key stored at a position.
    template <class KeyAt>
    int64_t find(const Value& k, size_t n, KeyAt keyAt) const {
        if (!active()) {
            for (size_t i = 0; i < n; i++)
                if (keyEquals(keyAt(i), k)) return (int64_t)i;
            return -1;
        }
        uint32_t h = hashOf(k), perturb = h;
        size_t mask = table.size() - 1;
        for (size_t i = h & mask;; i = nextSlot(i, perturb, mask)) {
            uint64_t e = table[i];
            if (e == 0) return -1;
            if (e != 1 && (uint32_t)(e >> 32) == h) {
                size_t pos = (size_t)(uint32_t)e - 2;
                if (keyEquals(keyAt(pos), k)) return (int64_t)pos;
            }
        }
    }
    // k (not present) is now at `pos`; n is the number of positions after adding it.
    template <class KeyAt>
    void added(const Value& k, size_t pos, size_t n, KeyAt keyAt) {
        if (!active()) {
            if (n <= kScan) return;
            rebuild(n, keyAt);
            return;
        }
        if ((filled + 1) * 4 > table.size() * 3) {
            rebuild(n, keyAt);
            return;
        }
        place(hashOf(k), pos);
    }
    void removed(const Value& k, size_t pos) {
        if (!active()) return;
        uint32_t h = hashOf(k), perturb = h;
        size_t mask = table.size() - 1;
        for (size_t i = h & mask;; i = nextSlot(i, perturb, mask)) {
            uint64_t e = table[i];
            if (e == 0) return;
            if (e != 1 && (size_t)(uint32_t)e - 2 == pos) {
                table[i] = 1;
                return;
            }
        }
    }
    // Indexes positions [0, n) again (skipping Undef tombstones).
    template <class KeyAt>
    void rebuild(size_t n, KeyAt keyAt) {
        size_t live = 0;
        for (size_t i = 0; i < n; i++)
            if (!keyAt(i).isUndef()) live++;
        if (live <= kScan) {
            clear();
            return;
        }
        size_t cap = 16;
        while (cap * 3 < live * 4 * 2) cap *= 2;  // at most 3/8 full after rebuilding
        table.assign(cap, 0);
        filled = 0;
        for (size_t i = 0; i < n; i++)
            if (!keyAt(i).isUndef()) place(hashOf(keyAt(i)), i);
    }

private:
    void place(uint32_t h, size_t pos) {
        size_t mask = table.size() - 1;
        size_t i = h & mask;
        uint32_t perturb = h;
        while (table[i] > 1) i = nextSlot(i, perturb, mask);
        if (table[i] == 0) filled++;
        table[i] = ((uint64_t)h << 32) | (uint64_t)(pos + 2);
    }
};

// Insertion-ordered hash map. Deleted entries leave an Undef key tombstone.
struct ObjMap : Obj {
    std::vector<std::pair<Value, Value>> entries;
    KeyIndex index;
    size_t live = 0;
    ObjMap() : Obj(OT::Map) {}
    int64_t position(const Value& k) const {
        if (live == 0) return -1;
        return index.find(k, entries.size(), [&](size_t i) -> const Value& { return entries[i].first; });
    }
    Value* find(const Value& k) {
        int64_t p = position(k);
        return p < 0 ? nullptr : &entries[(size_t)p].second;
    }
    void set(const Value& k, const Value& v) {
        int64_t p = position(k);
        if (p >= 0) {
            entries[(size_t)p].second = v;
            return;
        }
        entries.push_back({k, v});
        live++;
        index.added(k, entries.size() - 1, entries.size(), [&](size_t i) -> const Value& { return entries[i].first; });
    }
    bool drop(const Value& k) {
        int64_t p = position(k);
        if (p < 0) return false;
        index.removed(k, (size_t)p);
        entries[(size_t)p].first = Value::undef();
        entries[(size_t)p].second = Value();
        live--;
        if (entries.size() > 16 && live * 2 < entries.size()) compact();
        return true;
    }
    void compact() {
        std::vector<std::pair<Value, Value>> fresh;
        fresh.reserve(live);
        for (auto& e : entries)
            if (!e.first.isUndef()) fresh.push_back(e);
        entries.swap(fresh);
        index.rebuild(entries.size(), [&](size_t i) -> const Value& { return entries[i].first; });
    }
    void clear() {
        entries.clear();
        index.clear();
        live = 0;
    }
};

struct ObjSet : Obj {
    std::vector<Value> items;  // Undef = tombstone
    KeyIndex index;
    size_t live = 0;
    ObjSet() : Obj(OT::Set) {}
    int64_t position(const Value& v) const {
        if (live == 0) return -1;
        return index.find(v, items.size(), [&](size_t i) -> const Value& { return items[i]; });
    }
    bool has(const Value& v) const { return position(v) >= 0; }
    bool add(const Value& v) {
        if (position(v) >= 0) return false;
        items.push_back(v);
        live++;
        index.added(v, items.size() - 1, items.size(), [&](size_t i) -> const Value& { return items[i]; });
        return true;
    }
    bool drop(const Value& v) {
        int64_t p = position(v);
        if (p < 0) return false;
        index.removed(v, (size_t)p);
        items[(size_t)p] = Value::undef();
        live--;
        if (items.size() > 16 && live * 2 < items.size()) {
            std::vector<Value> fresh;
            for (auto& x : items)
                if (!x.isUndef()) fresh.push_back(x);
            items.swap(fresh);
            index.rebuild(items.size(), [&](size_t i) -> const Value& { return items[i]; });
        }
        return true;
    }
};

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<Value> consts;
    std::vector<int> lines;  // source line of each code byte
    int lineAt(size_t offset) const { return offset < lines.size() ? lines[offset] : (lines.empty() ? 0 : lines.back()); }
};

// Remembers where a member lives for the class seen last at one GET/SET_MEMBER
// or INVOKE instruction, so the next access by an instance of that class
// skips the lookups.
struct InlineCache {
    uint64_t clsId = UINT64_MAX;  // ObjClass::id (never reused, unlike pointers)
    int32_t index = -1;           // field slot, or -1 for a method
    Value method;                 // INVOKE: the method closure
};

struct ObjFunction : Obj {
    std::string name;
    std::string file;
    int arity = 0;       // parameter count (not counting self)
    int minArity = 0;    // parameters without defaults
    int upvalueCount = 0;
    bool lenient = false;      // placeholder lambda: extra args are dropped
    bool isFork = false;       // fork fn: calls start a task
    bool methodStyle = false;  // slot 0 holds self (methods, constructors, extension methods)
    bool isCtor = false;
    bool exported = true;      // methods: public?
    ModuleRt* module = nullptr;
    bool isPar = false;        // par fn: calls start a parallel task
    Chunk chunk;
    std::vector<std::string> paramNames;
    std::vector<uint16_t> globalsUsed;  // module globals the code reads or writes (par snapshots)
    // debug info: which stack slot holds which variable, for code offsets [start, end)
    struct LocalName {
        std::string name;
        uint16_t slot;
        uint32_t start, end;
    };
    std::vector<LocalName> localNames;
    std::vector<InlineCache> ic;  // written only by the VM that owns the function
    // JIT (main VM only): 0 not tried, 2 compiled, 3 can't be compiled,
    // 4 compiled but calls keep stopping early (only hot loops use it)
    uint8_t jitState = 0;
    uint32_t jitCalls = 0, jitBails = 0;
    uint32_t osrEntries = 0, osrBails = 0;  // entering hot loops; stopped when it keeps stopping early
    // Where hot loops start and their stack depth: extra entry points for the
    // JIT (code after an instruction it doesn't compile is reached from these).
    std::vector<std::pair<uint32_t, int>> jitSeeds;
    uint8_t jitRecompiles = 0;
    bool wantJit = false;          // $jit (or warm start): compile on the first call
    bool warmChecked = false;      // looked up in the warm-start profile
    bool noJit = false;            // $nojit: always interpreted
    bool autoCall = false;         // declared without a parameter list: obj.name calls it
    // A comparator lambda of the form `_a < _b`, `_b > _a`, `_a.age <= _b.age`: sort
    // compares directly instead of calling it. cmpOp: 0 < 1 <= 2 > 3 >=, -1 none.
    int8_t cmpOp = -1;
    bool cmpSwap = false;  // the second parameter is on the left (_b < _a)
    int cmpField = -1;     // compare this field of both (a symbol), -1: the values themselves
    int keyField = -1;     // a lambda `_.age`: it only reads this field of its argument (sort_by reads it directly)
    int gpuKernel = -1;    // a gpu fn's launcher: the kernel it runs (gpu.hpp)
    uint32_t hotness = 0;          // calls + loop iterations, until it's compiled
    // Type profile for the optimizing JIT (main VM only, filled while interpreted):
    // for each instruction that produces a value whose type the code doesn't fix
    // (a field, an item, a call's result...), the kinds of values seen, at the
    // offset of the instruction after it. Then one byte per parameter. See profBit.
    std::vector<uint8_t> typeSeen;
    bool noTier2 = false;          // its typed machine code kept failing its guesses
    // machine code may call this one's machine code directly (compiled, a plain function:
    // not a task, not a method using captured variables); kept by jitAllowDirect
    uint8_t jitDirect = 0;
    struct JitCode* jit = nullptr;
    std::vector<std::string> upvalueNames;
    ObjFunction() : Obj(OT::Function) {}
};

struct ObjUpvalue : Obj {
    Fiber* fiber = nullptr;  // non-null while open
    size_t slot = 0;
    Value closed;
    ObjUpvalue* nextOpen = nullptr;
    ObjUpvalue() : Obj(OT::Upvalue) {}
};

struct ObjClosure : Obj {
    ObjFunction* fn;
    std::vector<ObjUpvalue*> upvalues;
    explicit ObjClosure(ObjFunction* f) : Obj(OT::Closure), fn(f), upvalues(f->upvalueCount, nullptr) {}
};

using NativeFn = Value (*)(VM& vm, Value* args, int argc);

struct ObjNative : Obj {
    std::string name;
    NativeFn fn;
    int minArity, maxArity;  // maxArity -1 = variadic; counts include the receiver for methods
    bool isMethod = false;   // args[0] is the receiver
    Value data;              // extra state for the native (vm.nativeData)
    std::string path;        // how to find the same native in another heap: "print", "fs.read", "m:3:add"
    ObjNative(std::string n, NativeFn f, int mn, int mx) : Obj(OT::Native), name(std::move(n)), fn(f), minArity(mn), maxArity(mx) {}
};

// A method (or UFCS function) with its receiver bound: p.move, 5.clamp
struct ObjBound : Obj {
    Value receiver;
    Value method;
    ObjBound(Value r, Value m) : Obj(OT::Bound), receiver(r), method(m) {}
};

struct ObjClass : Obj {
    std::string name;
    std::vector<std::string> fields;
    std::unordered_map<int, int> fieldIndex;  // symbol -> field slot
    std::vector<int> positional;              // field slots for Point(x, y) patterns
    std::unordered_map<int, Value> methods;   // symbol -> closure
    std::unordered_map<int, bool> methodPublic;
    ObjClosure* ctor = nullptr;
    ModuleRt* module = nullptr;
    int ctorArity = 0, ctorMinArity = 0;
    uint64_t id;  // unique per class creation; identifies the class across heaps
    static std::atomic<uint64_t> nextId;
    ObjClass() : Obj(OT::Class), id(nextId.fetch_add(1)) {}
};

// An instance's fields: stored right after the instance, in the same memory
// (a class's field count never changes), so no pointer is needed. The count sits
// in the 4 bytes after the 12-byte header that alignment would leave empty, and
// the values start Off bytes after it, at sizeof(the object).
// Tuples (fixed size too) store their items the same way.
template <size_t Off>
struct FieldArrayAt {
    uint32_t n = 0;
    Value* data() { return reinterpret_cast<Value*>(reinterpret_cast<char*>(this) + Off); }
    const Value* data() const { return reinterpret_cast<const Value*>(reinterpret_cast<const char*>(this) + Off); }
    Value& operator[](size_t i) { return data()[i]; }
    const Value& operator[](size_t i) const { return data()[i]; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    Value* begin() { return data(); }
    Value* end() { return data() + n; }
    const Value* begin() const { return data(); }
    const Value* end() const { return data() + n; }
};
using TupleItems = FieldArrayAt<4>;   // at 12; items at 16
using InstFields = FieldArrayAt<12>;  // at 12, then the class pointer; fields at 24

// A tuple: its items follow it in memory, like an instance's fields.
struct ObjTuple : Obj {
    TupleItems items;  // last: the n items follow (VM::newTupleN makes room)
    explicit ObjTuple(size_t n) : Obj(OT::Tuple) {
        items.n = (uint32_t)n;
        for (size_t i = 0; i < n; i++) new (&items.data()[i]) Value();
    }
};
inline ObjTuple* tupleInit(void* mem, size_t n) { return new (mem) ObjTuple(n); }

// Read-only items of a list (any kind, never widened) or a tuple. Items come out
// as values (copies).
struct SeqView {
    const void* p = nullptr;
    size_t n = 0;
    uint8_t k = LK_VALUES;
    SeqView() = default;
    SeqView(const std::vector<Value>& v) : p(v.data()), n(v.size()) {}
    template <size_t Off>
    SeqView(const FieldArrayAt<Off>& f) : p(f.data()), n(f.size()) {}
    SeqView(const ObjList& l) : n(l.len()), k(l.kind == LK_EMPTY ? (uint8_t)LK_VALUES : l.kind) {
        switch (k) {
            case LK_BOOLS: p = l.bools.data(); break;
            case LK_INTS: p = l.ints.data(); break;
            case LK_FLOATS: p = l.floats.data(); break;
            default: p = l.vals.data(); break;
        }
    }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    Value operator[](size_t i) const {
        switch (k) {
            case LK_BOOLS: return Value::boolean(static_cast<const uint8_t*>(p)[i] != 0);
            case LK_INTS: return Value::integer(static_cast<const int64_t*>(p)[i]);
            case LK_FLOATS: return Value::number(static_cast<const double*>(p)[i]);
            default: return static_cast<const Value*>(p)[i];
        }
    }
    struct iterator {
        const SeqView* v;
        size_t i;
        Value operator*() const { return (*v)[i]; }
        iterator& operator++() { ++i; return *this; }
        iterator operator+(std::ptrdiff_t d) const { return {v, (size_t)((std::ptrdiff_t)i + d)}; }
        iterator operator-(std::ptrdiff_t d) const { return {v, (size_t)((std::ptrdiff_t)i - d)}; }
        std::ptrdiff_t operator-(const iterator& o) const { return (std::ptrdiff_t)i - (std::ptrdiff_t)o.i; }
        bool operator!=(const iterator& o) const { return i != o.i; }
        bool operator==(const iterator& o) const { return i == o.i; }
        using iterator_category = std::forward_iterator_tag;
        using value_type = Value;
        using difference_type = std::ptrdiff_t;
        using pointer = const Value*;
        using reference = Value;
    };
    iterator begin() const { return {this, 0}; }
    iterator end() const { return {this, n}; }
    std::vector<Value> vec() const {
        std::vector<Value> out;
        out.reserve(n);
        for (size_t i = 0; i < n; i++) out.push_back((*this)[i]);
        return out;
    }
};

inline bool seqOf(Value v, SeqView& out);  // below, once ObjList is complete

struct ObjInstance : Obj {
    InstFields fields;  // the n values follow the instance (VM::newInstance makes room)
    ObjClass* cls;
    ObjInstance(ObjClass* c, size_t n) : Obj(OT::Instance), cls(c) {
        fields.n = (uint32_t)n;
        for (size_t i = 0; i < n; i++) new (&fields.data()[i]) Value();
    }
};

struct ObjRange : Obj {
    int64_t start, end, step;
    bool inclusive;
    bool openStart = false, openEnd = false;  // xs[..3], xs[2..]: only for slicing
    ObjRange(int64_t s, int64_t e, int64_t st, bool inc) : Obj(OT::Range), start(s), end(e), step(st), inclusive(inc) {}
    int64_t count() const {
        if (step > 0) {
            int64_t last = inclusive ? end : end - 1;
            return last < start ? 0 : (last - start) / step + 1;
        }
        int64_t last = inclusive ? end : end + 1;
        return last > start ? 0 : (start - last) / (-step) + 1;
    }
    bool contains(int64_t v) const {
        if (step > 0) {
            if (v < start || (inclusive ? v > end : v >= end)) return false;
            return (v - start) % step == 0;
        }
        if (v > start || (inclusive ? v < end : v <= end)) return false;
        return (start - v) % (-step) == 0;
    }
};

struct ObjError : Obj {
    std::string kind;  // "Error", "NotFound", "MaxRetries", ...
    std::string msg;
    Value payload;
    ObjError(std::string k, std::string m) : Obj(OT::Error), kind(std::move(k)), msg(std::move(m)) {}
};

struct Waiter {
    Fiber* fiber;
    bool pushResult;
};

struct SharedChan;
struct SharedTask;

// A select waits on several flows at once; the first one to deliver "fires" it
// and the task's registrations on the others become stale (and are skipped).
struct SelectState {
    std::atomic<bool> fired{false};
    bool tryFire() {
        bool expected = false;
        return fired.compare_exchange_strong(expected, true);
    }
};

struct RecvWaiter {
    Fiber* fiber;
    bool forIter;                         // receiving for a loop (closed -> Done)
    std::shared_ptr<SelectState> sel;     // part of a select: deliver (index, value)
    int index = -1;
};

struct ObjChannel : Obj {
    std::shared_ptr<SharedChan> shared;  // set once the flow is used by par tasks
    std::deque<Value> buf;
    int cap;
    bool closed = false;
    std::deque<RecvWaiter> recvWaiters;
    std::deque<std::pair<Fiber*, Value>> sendWaiters;
    explicit ObjChannel(int c) : Obj(OT::Channel), cap(c) {}
};

struct ObjTask : Obj {
    std::shared_ptr<SharedTask> shared;  // par tasks: state shared between threads
    int id;
    Fiber* fiber = nullptr;
    bool done = false;
    Value result;
    std::vector<Waiter> waiters;
    explicit ObjTask(int i) : Obj(OT::Task), id(i) {}
};

struct ObjModule : Obj {
    ModuleRt* rt;
    explicit ObjModule(ModuleRt* m) : Obj(OT::Module), rt(m) {}
};

// Native namespace: fs, math, Error, rng, ... Members may be called; the
// optional fallbacks handle dynamic members (Error.Anything).
struct ObjNamespace : Obj {
    std::string name;
    std::unordered_map<std::string, Value> members;
    Value callable;  // namespace itself can be called: rng(seed), vec(...)
    // forCall: the member is being called (Error.NotFound(msg)) rather than read
    Value (*getFallback)(VM& vm, ObjNamespace* ns, const std::string& name, bool forCall) = nullptr;
    explicit ObjNamespace(std::string n) : Obj(OT::Namespace), name(std::move(n)) {}
};

struct ObjIterator : Obj {
    Value src;
    size_t idx = 0;
    int64_t cur = 0;
    bool pairs = false;    // maps give (key, value) tuples
    bool keyed = false;    // for k, v in map: maps give the value; the key is in `key`
    bool pending = false;  // FOR_NEXT waited on a flow; the value arrives on the stack
    Value key;             // keyed map iteration: the current key
    ObjIterator(Value s, bool p) : Obj(OT::Iterator), src(s), pairs(p) {}
};

struct ObjVec : Obj {
    std::vector<double> v;
    ObjVec() : Obj(OT::Vec) {}
};

struct ObjMat : Obj {
    int rows = 0, cols = 0;
    std::vector<double> d;  // row-major
    ObjMat() : Obj(OT::Mat) {}
    double& at(int r, int c) { return d[(size_t)r * cols + c]; }
};

struct ObjQuat : Obj {
    double x = 0, y = 0, z = 0, w = 1;
    ObjQuat() : Obj(OT::Quat) {}
};

// A GPU buffer (gpu.hpp): n 32-bit ints or floats, in memory the device uses and
// the CPU reads and writes directly (mapped), or plain memory without a device.
struct VkBufferMem;
struct ObjGpuBuffer;
void gpuReleaseBuffer(ObjGpuBuffer* b);
struct ObjGpuBuffer : Obj {
    bool floats = true;
    uint32_t n = 0;
    void* data = nullptr;        // n words
    VkBufferMem* dev = nullptr;  // the device's buffer, if any
    ObjGpuBuffer() : Obj(OT::GpuBuffer) {}
    ~ObjGpuBuffer() { gpuReleaseBuffer(this); }
    ObjGpuBuffer(const ObjGpuBuffer&) = delete;
    float* fl() { return static_cast<float*>(data); }
    int32_t* in() { return static_cast<int32_t*>(data); }
};

struct ObjRng : Obj {
    std::mt19937_64 gen;
    explicit ObjRng(uint64_t seed) : Obj(OT::Rng), gen(seed) {}
};

struct ObjTime : Obj {
    double epoch;  // seconds since 1970-01-01 UTC
    explicit ObjTime(double e) : Obj(OT::Time), epoch(e) {}
};

// Runtime type descriptor used for gradual type checks on annotated
// parameters, returns and variables.
struct ObjTypeDesc : Obj {
    enum K { Any, Nil, Bool, Int, Float, Num, Str, List, Map, Set, Tuple, Fn, Class, Interface, EnumOf, Optional, Result,
             Flow, Task, Range, Vec, Mat, Quat, Rng, Time, Error, Iter } k = Any;
    std::string className;          // Class: exact name; EnumOf: the enum's name
    std::vector<int> syms;          // Interface: required methods
    ObjTypeDesc* inner = nullptr;
    std::string text;  // how the annotation was written, for messages
    ObjTypeDesc() : Obj(OT::TypeDesc) {}
};

// Module state at runtime.
struct ModuleRt {
    VM* owner = nullptr;
    ModuleRt* original = nullptr;  // par workers: the module this is a snapshot of
    std::string name, path;
    std::vector<Value> globals;
    std::vector<std::string> globalNames;
    std::unordered_map<std::string, int> exports;  // exported name -> global index
    ObjClosure* script = nullptr;
    ObjModule* obj = nullptr;
    Value nativeNs;  // native modules: the namespace object
    bool initialized = false;
};

inline bool isObjType(const Value& v, OT t) { return v.t == VT::Obj && v.o->type == t; }
template <class T>
inline T* as(const Value& v) {
    return static_cast<T*>(v.o);
}

// Runs the destructor of an object's real type (objects have no vtable).
inline void destroyObj(Obj* o) {
    switch (o->type) {
        case OT::String: static_cast<ObjString*>(o)->~ObjString(); break;
        case OT::List: static_cast<ObjList*>(o)->~ObjList(); break;
        case OT::Tuple: static_cast<ObjTuple*>(o)->~ObjTuple(); break;
        case OT::Map: static_cast<ObjMap*>(o)->~ObjMap(); break;
        case OT::Set: static_cast<ObjSet*>(o)->~ObjSet(); break;
        case OT::Function: static_cast<ObjFunction*>(o)->~ObjFunction(); break;
        case OT::Closure: static_cast<ObjClosure*>(o)->~ObjClosure(); break;
        case OT::Upvalue: static_cast<ObjUpvalue*>(o)->~ObjUpvalue(); break;
        case OT::Native: static_cast<ObjNative*>(o)->~ObjNative(); break;
        case OT::Bound: static_cast<ObjBound*>(o)->~ObjBound(); break;
        case OT::Class: static_cast<ObjClass*>(o)->~ObjClass(); break;
        case OT::Instance: static_cast<ObjInstance*>(o)->~ObjInstance(); break;
        case OT::Range: static_cast<ObjRange*>(o)->~ObjRange(); break;
        case OT::Error: static_cast<ObjError*>(o)->~ObjError(); break;
        case OT::Channel: static_cast<ObjChannel*>(o)->~ObjChannel(); break;
        case OT::Task: static_cast<ObjTask*>(o)->~ObjTask(); break;
        case OT::Module: static_cast<ObjModule*>(o)->~ObjModule(); break;
        case OT::Namespace: static_cast<ObjNamespace*>(o)->~ObjNamespace(); break;
        case OT::Iterator: static_cast<ObjIterator*>(o)->~ObjIterator(); break;
        case OT::Vec: static_cast<ObjVec*>(o)->~ObjVec(); break;
        case OT::Mat: static_cast<ObjMat*>(o)->~ObjMat(); break;
        case OT::Quat: static_cast<ObjQuat*>(o)->~ObjQuat(); break;
        case OT::Rng: static_cast<ObjRng*>(o)->~ObjRng(); break;
        case OT::GpuBuffer: static_cast<ObjGpuBuffer*>(o)->~ObjGpuBuffer(); break;
        case OT::Time: static_cast<ObjTime*>(o)->~ObjTime(); break;
        case OT::TypeDesc: static_cast<ObjTypeDesc*>(o)->~ObjTypeDesc(); break;
        case OT::Marker: static_cast<ObjMarker*>(o)->~ObjMarker(); break;
        case OT::Interface: static_cast<ObjInterface*>(o)->~ObjInterface(); break;
        case OT::Free: break;
    }
}

// The items of a list or a tuple, read-only; false for any other value.
inline bool seqOf(Value v, SeqView& out) {
    if (v.t != VT::Obj) return false;
    if (v.o->type == OT::List) {
        out = SeqView(*static_cast<ObjList*>(v.o));
        return true;
    }
    if (v.o->type == OT::Tuple) {
        out = SeqView(static_cast<ObjTuple*>(v.o)->items);
        return true;
    }
    return false;
}

}  // namespace ixo
