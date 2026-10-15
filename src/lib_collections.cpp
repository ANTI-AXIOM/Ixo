// Collection builtins: list, tuple, map, set, range.
#include <algorithm>
#include <cmath>

#include "builtins.hpp"
#include "simd.hpp"

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

static ObjList* L(Value v) { return as<ObjList>(v); }
static SeqView V(Value v) { return SeqView(*as<ObjList>(v)); }
// Edits a list as plain values, then stores the result back at its narrowest kind.
template <class F> static auto editList(Value v, F&& f) {
    auto* l = L(v);
    std::vector<Value> items = V(v).vec();
    auto r = f(items);
    l->adopt(std::move(items));
    return r;
}

// Collects any iterable into a vector (list, tuple, str, set, map keys, range).
static std::vector<Value> toVector(VM& vm, Value v, const char* fname) {
    std::vector<Value> out;
    if (!v.isObj()) vm.panic(std::string(fname) + "() expected something to loop over, got " + vm.typeName(v));
    switch (v.o->type) {
        case OT::List: return V(v).vec();
        case OT::Tuple: return SeqView(as<ObjTuple>(v)->items).vec();
        case OT::Set:
            for (auto& x : as<ObjSet>(v)->items)
                if (!x.isUndef()) out.push_back(x);
            return out;
        case OT::Map:
            for (auto& [k, x] : as<ObjMap>(v)->entries)
                if (!k.isUndef()) out.push_back(k);
            return out;
        case OT::Range: {
            auto* r = as<ObjRange>(v);
            int64_t cnt = r->count();
            if (cnt > 100000000) vm.panic("range is too large to turn into a list");
            for (int64_t i = 0; i < cnt; i++) out.push_back(Value::integer(r->start + i * r->step));
            return out;
        }
        case OT::String: {
            const std::string& s = as<ObjString>(v)->s;
            for (size_t i = 0; i < s.size();) {
                size_t len = 1;
                unsigned char c = s[i];
                if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
                out.push_back(vm.vstr(s.substr(i, len)));
                i += len;
            }
            return out;
        }
        default: break;
    }
    vm.panic(std::string(fname) + "() expected something to loop over, got " + vm.typeName(v));
}

// ------------------------------------------------------------------ constructors

NATIVE(c_list) {
    if (n == 0) return vm.newList();
    return vm.newList(toVector(vm, a[0], "list"));
}
NATIVE(c_tuple) {
    if (n == 0) return vm.newTuple({});
    return vm.newTuple(toVector(vm, a[0], "tuple"));
}
NATIVE(c_map) {
    auto* m = vm.alloc<ObjMap>();
    if (n > 0) {
        for (auto& p : toVector(vm, a[0], "map")) {
            SeqView kv;
            if (!seqOf(p, kv) || kv.size() != 2) vm.panic("map() expects a list of (key, value) pairs");
            requireHashable(vm, kv[0]);
            m->set(kv[0], kv[1]);
        }
    }
    return Value::object(m);
}
NATIVE(c_set) {
    auto* s = vm.alloc<ObjSet>();
    if (n > 0)
        for (auto& x : toVector(vm, a[0], "set")) {
            requireHashable(vm, x);
            s->add(x);
        }
    return Value::object(s);
}
NATIVE(n_len) {
    Value v = a[0];
    if (!v.isObj()) vm.panic("len() of a " + vm.typeName(v));
    switch (v.o->type) {
        case OT::String: {
            auto* s = as<ObjString>(v);
            return Value::integer((int64_t)(VM::ascii(s) ? s->s.size() : utf8Length(s->s)));
        }
        case OT::List: return Value::integer((int64_t)L(v)->len());
        case OT::Tuple: return Value::integer((int64_t)as<ObjTuple>(v)->items.size());
        case OT::Map: return Value::integer((int64_t)as<ObjMap>(v)->live);
        case OT::Set: return Value::integer((int64_t)as<ObjSet>(v)->live);
        case OT::Range: return Value::integer(as<ObjRange>(v)->count());
        case OT::Channel: return Value::integer((int64_t)as<ObjChannel>(v)->buf.size());
        case OT::Vec: return Value::integer((int64_t)as<ObjVec>(v)->v.size());
        default: vm.panic("len() of a " + vm.typeName(v));
    }
}
NATIVE(n_zip) {
    auto x = toVector(vm, a[0], "zip");
    auto y = toVector(vm, a[1], "zip");
    std::vector<Value> out;
    for (size_t i = 0; i < x.size() && i < y.size(); i++) out.push_back(vm.newTuple({x[i], y[i]}));
    return vm.newList(std::move(out));
}
NATIVE(n_sum) {
    if (isObjType(a[0], OT::List)) {  // packed numbers: no boxing, and SIMD for ints
        auto* l = as<ObjList>(a[0]);
        if (l->lk() == LK_INTS) return Value::integer(simdSumInts(l->ints.data(), l->ints.size()));
        if (l->lk() == LK_FLOATS) return Value::number(simdSumFloats(l->floats.data(), l->floats.size()));
        if (l->lk() == LK_EMPTY) return Value::integer(0);
    }
    auto items = toVector(vm, a[0], "sum");
    Value acc = Value::integer(0);
    bool allInt = true;
    int64_t isum = 0;
    double fsum = 0;
    for (auto& v : items) {
        if (!v.isNum()) vm.panic("sum() of a list containing a " + vm.typeName(v));
        if (v.isInt())
            isum += v.i;
        else
            allInt = false;
        fsum += v.num();
    }
    (void)acc;
    return allInt ? Value::integer(isum) : Value::number(fsum);
}

// ------------------------------------------------------------------ list

NATIVE(l_len) { return Value::integer((int64_t)L(a[0])->len()); }
NATIVE(l_add) {
    auto* l = L(a[0]);
    size_t before = l->elemSize();
    l->push(a[1]);
    // keep the list's own size in step so freeing it gives the bytes back
    size_t after = l->elemSize();
    vm.grow(l, after + (after > before ? (l->len() - 1) * (after - before) : 0));
    return a[0];
}
// After removing items: a list far below its room gives the rest back.
static void shrinkSparse(std::vector<Value>& v) {
    if (v.capacity() > 64 && v.size() * 4 < v.capacity()) v.shrink_to_fit();
}
static void shrinkSparse(ObjList* l) {
    if (l->capacity() > 64 && l->len() * 4 < l->capacity()) l->shrinkToFit();
}

NATIVE(l_take) {
    auto* l = L(a[0]);
    if (!l->len()) return Value();
    Value x = l->pop();
    shrinkSparse(l);
    return x;
}
// for x in xs -> s += x compiled as one call (Compiler::trySumIdiom): the same
// result as the loop: in order for floats; ints wrap (or stop at an overflow when
// that's checked); packed ints add with SIMD
NATIVE(n_sum_into) {
    Value acc = a[0], seq = a[1];
    auto add = [&](Value x) {
        if (acc.t == VT::Int && x.t == VT::Int) {
            int64_t r;
            if (__builtin_add_overflow(acc.i, x.i, &r) && vm.checkOverflow) vm.intOverflow('+', acc.i, x.i);
            acc = Value::integer(r);
        } else if (acc.isNum() && x.isNum()) {
            acc = Value::number(acc.num() + x.num());
        } else {
            acc = vm.binaryOp(OP_ADD, acc, x);
        }
    };
    if (isObjType(seq, OT::List)) {
        auto* l = as<ObjList>(seq);
        if (l->lk() == LK_INTS && acc.t == VT::Int && !vm.checkOverflow)
            return Value::integer((int64_t)((uint64_t)acc.i + (uint64_t)simdSumInts(l->ints.data(), l->ints.size())));
        if (l->lk() == LK_FLOATS && acc.isNum()) {
            double s = acc.num();
            for (double x : l->floats) s += x;
            return Value::number(s);
        }
        for (size_t i = 0; i < l->len(); i++) add(l->get(i));
        return acc;
    }
    if (isObjType(seq, OT::Range)) {
        auto* r = as<ObjRange>(seq);
        if (r->step == 0) vm.panic("a range's step can't be 0");
        for (int64_t i = r->start; r->step > 0 ? (r->inclusive ? i <= r->end : i < r->end) : (r->inclusive ? i >= r->end : i > r->end);
             i += r->step)
            add(Value::integer(i));
        return acc;
    }
    for (Value x : toVector(vm, seq, "for")) add(x);
    return acc;
}

// words.counts: how many times each item occurs, in first-seen order
NATIVE(l_counts) {
    std::vector<Value> items = toVector(vm, a[0], "counts");
    auto* m = vm.alloc<ObjMap>();
    Value keep = Value::object(m);
    vm.extraRoots.push_back(keep);
    for (auto& x : items) {
        if (Value* c = m->find(x))
            c->i++;
        else
            m->set(x, Value::integer(1));
    }
    vm.extraRoots.pop_back();
    return keep;
}

// A binary min-heap kept in a plain list, as Python's heapq: heap_push / heap_pop.
NATIVE(l_heap_push) {
    auto* l = L(a[0]);
    Value x = a[1];
    l->push(x);
    size_t i = l->len() - 1;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        Value pv = l->get(p);
        if (vm.compare(x, pv) >= 0) break;
        l->set(i, pv);
        i = p;
    }
    l->set(i, x);
    return a[0];
}
NATIVE(l_heap_pop) {
    auto* l = L(a[0]);
    size_t len = l->len();
    if (!len) return Value();
    Value top = l->get(0), x = l->pop();
    if (--len) {
        vm.extraRoots.push_back(top);  // comparisons may call Ixo `<` methods (and collect)
        size_t i = 0;
        for (size_t c; (c = 2 * i + 1) < len; i = c) {
            if (c + 1 < len && vm.compare(l->get(c + 1), l->get(c)) < 0) c++;
            if (vm.compare(l->get(c), x) >= 0) break;
            l->set(i, l->get(c));
        }
        l->set(i, x);
        vm.extraRoots.pop_back();
    }
    return top;
}
NATIVE(l_get) {
    auto* l = L(a[0]);
    if (!a[1].isNum()) vm.panic("list.get() needs an int index");
    int64_t i = vm.argInt(a, 1, "get");
    if (i < 0) i += (int64_t)l->len();
    if (i < 0 || i >= (int64_t)l->len()) return n > 2 ? a[2] : Value();
    return l->get((size_t)i);
}
NATIVE(l_set) {
    auto* l = L(a[0]);
    int64_t i = vm.argInt(a, 1, "set");
    if (i < 0) i += (int64_t)l->len();
    if (i < 0 || i >= (int64_t)l->len())
        vm.panic("index " + std::to_string(i) + " is out of range for a list of length " + std::to_string(l->len()));
    l->set((size_t)i, a[2]);
    return Value();
}
NATIVE(l_insert) {
    int64_t i = vm.argInt(a, 1, "insert");
    Value x = a[2];
    return editList(a[0], [&](std::vector<Value>& v) {
        if (i < 0) i += (int64_t)v.size() + 1;
        i = std::clamp<int64_t>(i, 0, (int64_t)v.size());
        v.insert(v.begin() + i, x);
        return Value();
    });
}
NATIVE(l_drop) {
    int64_t i = vm.argInt(a, 1, "drop");
    return editList(a[0], [&](std::vector<Value>& v) {
        if (i < 0) i += (int64_t)v.size();
        if (i < 0 || i >= (int64_t)v.size()) return Value();
        Value x = v[(size_t)i];
        v.erase(v.begin() + i);
        shrinkSparse(v);
        return x;
    });
}
NATIVE(l_remove) {
    auto* l = L(a[0]);
    for (size_t i = 0; i < l->len(); i++)
        if (vm.equals(l->get(i), a[1]))
            return editList(a[0], [&](std::vector<Value>& v) {
                v.erase(v.begin() + i);
                shrinkSparse(v);
                return Value::boolean(true);
            });
    return Value::boolean(false);
}
NATIVE(l_wipe) {
    L(a[0])->clear();  // and its memory
    return Value();
}

static void sortValues(VM& vm, std::vector<Value>& items, Value cmp) {
    if (cmp.isNil()) {
        mergeSort(items, [&](const Value& x, const Value& y) { return vm.compare(x, y) < 0; });
        return;
    }
    if (!vm.isCallable(cmp)) vm.panic("sort() expects a comparison function like sort(_a < _b)");
    if (isObjType(cmp, OT::Closure)) {
        // sort(_b < _a), sort(_a.age < _b.age): compare without calling the lambda
        ObjFunction* f = as<ObjClosure>(cmp)->fn;
        if (f->cmpOp >= 0) {
            auto field = [&](const Value& v, Value& out) {
                if (f->cmpField < 0) {
                    out = v;
                    return true;
                }
                if (!isObjType(v, OT::Instance)) return false;
                auto* inst = as<ObjInstance>(v);
                auto it = inst->cls->fieldIndex.find(f->cmpField);
                if (it == inst->cls->fieldIndex.end()) return false;
                out = inst->fields[it->second];
                return true;
            };
            bool direct = true;
            for (auto& v : items) {
                Value tmp;
                if (!field(v, tmp)) direct = false;
            }
            // all numbers or all strings: comparing them directly matches `<` exactly
            bool nums = true, strs = true;
            for (auto& v : items) {
                Value k;
                if (!field(v, k)) continue;
                if (!k.isNum()) nums = false;
                if (!isObjType(k, OT::String)) strs = false;
            }
            bool plain = nums || strs;
            if (direct && plain) {
                mergeSort(items, [&](const Value& x, const Value& y) {
                    Value a0, b0;
                    field(f->cmpSwap ? y : x, a0);
                    field(f->cmpSwap ? x : y, b0);
                    int c = vm.compare(a0, b0);
                    switch (f->cmpOp) {
                        case 0: return c < 0;
                        case 1: return c <= 0;
                        case 2: return c > 0;
                        default: return c >= 0;
                    }
                });
                return;
            }
        }
    }
    mergeSort(items, [&](const Value& x, const Value& y) {
        Value args[2] = {x, y};
        Value r = vm.callNested(cmp, args, 2);
        if (r.isNum()) return r.num() < 0;
        return vm.truthy(r);
    });
}

// Sorting a list of only ints (or only floats without NaN) needs no compare callbacks.
static bool sortTypedInPlace(ObjList* l) {
    if (l->lk() == LK_INTS) {
        std::sort(l->ints.begin(), l->ints.end());
        return true;
    }
    if (l->lk() == LK_FLOATS) {
        for (double d : l->floats)
            if (d != d) return false;
        std::stable_sort(l->floats.begin(), l->floats.end());
        return true;
    }
    return false;
}
static bool sortedTyped(VM& vm, Value src, Value& out) {
    if (!isObjType(src, OT::List) || !L(src)->typed() || L(src)->lk() == LK_BOOLS) return false;
    Value copy = vm.newList(V(src).vec());
    if (!sortTypedInPlace(L(copy))) return false;
    out = copy;
    return true;
}
NATIVE(l_sort) {
    Value cmp = n > 1 ? a[1] : Value();
    if (n < 2 || a[1].isNil()) {
        if (Value fast; sortedTyped(vm, a[0], fast)) return fast;
    }
    std::vector<Value> items = toVector(vm, a[0], "sort");
    sortValues(vm, items, cmp);
    return vm.newList(std::move(items));
}
// Natives that call back into Ixo (callNested) must copy their arguments
// first: the VM stack that `a` points into can move during the callback.
NATIVE(l_sort_inplace) {
    Value list = a[0];
    Value cmp = n > 1 ? a[1] : Value();
    if (cmp.isNil() && sortTypedInPlace(L(list))) return list;
    std::vector<Value> items = V(list).vec();
    sortValues(vm, items, cmp);
    L(list)->adopt(std::move(items));
    return list;
}
// sort_by(_.age): the field of each instance, without calling the lambda (false: call it)
static bool fieldKeys(Value keyFn, const std::vector<Value>& items, std::vector<Value>& keys) {
    if (!isObjType(keyFn, OT::Closure) || as<ObjClosure>(keyFn)->fn->keyField < 0) return false;
    int sym = as<ObjClosure>(keyFn)->fn->keyField;
    ObjClass* last = nullptr;
    int at = -1;
    keys.resize(items.size());
    for (size_t i = 0; i < items.size(); i++) {
        if (!isObjType(items[i], OT::Instance)) return false;
        auto* inst = as<ObjInstance>(items[i]);
        if (inst->cls != last) {  // (a method or getter of that name: the lambda decides)
            auto it = inst->cls->fieldIndex.find(sym);
            if (it == inst->cls->fieldIndex.end()) return false;
            last = inst->cls;
            at = it->second;
        }
        keys[i] = inst->fields[(size_t)at];
    }
    return true;
}
NATIVE(l_sort_by) {
    std::vector<Value> items = toVector(vm, a[0], "sort_by");
    Value keyFn = a[1];
    std::vector<Value> keys;
    if (!fieldKeys(keyFn, items, keys)) {
        keys.clear();
        for (auto& x : items) keys.push_back(vm.callNested(keyFn, &x, 1));
    }
    std::vector<uint32_t> idx(items.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = (uint32_t)i;
    bool ints = true;
    for (auto& k : keys) ints &= k.t == VT::Int;
    if (ints)  // (stable, as mergeSort: the same order)
        std::stable_sort(idx.begin(), idx.end(), [&](uint32_t p, uint32_t q) { return keys[p].i < keys[q].i; });
    else
        std::stable_sort(idx.begin(), idx.end(), [&](uint32_t p, uint32_t q) { return vm.compare(keys[p], keys[q]) < 0; });
    std::vector<Value> out;
    out.reserve(idx.size());
    for (auto i : idx) out.push_back(items[i]);
    return vm.newList(std::move(out));
}
NATIVE(l_rev) {
    std::vector<Value> items = toVector(vm, a[0], "rev");
    std::reverse(items.begin(), items.end());
    return vm.newList(std::move(items));
}
NATIVE(l_rev_inplace) {
    auto* l = L(a[0]);
    switch (l->lk()) {
        case LK_BOOLS: std::reverse(l->bools.begin(), l->bools.end()); break;
        case LK_INTS: std::reverse(l->ints.begin(), l->ints.end()); break;
        case LK_FLOATS: std::reverse(l->floats.begin(), l->floats.end()); break;
        default: std::reverse(l->vals.begin(), l->vals.end()); break;
    }
    return a[0];
}
NATIVE(l_first) {
    auto items = toVector(vm, a[0], "first");
    return items.empty() ? Value() : items.front();
}
NATIVE(l_last) {
    auto items = toVector(vm, a[0], "last");
    return items.empty() ? Value() : items.back();
}
NATIVE(l_is_empty) { return Value::boolean(L(a[0])->len() == 0); }
// The position of the first item equal to x in a packed list (len() when there's
// none), with == as VM::equals has it; -1: not a packed list, look the slow way.
static int64_t findPacked(ObjList* l, Value x) {
    switch (l->lk()) {
        case LK_INTS:
            if (x.t == VT::Int) return (int64_t)simdFindInt(l->ints.data(), l->ints.size(), x.i);
            if (x.t == VT::Float) {  // an int equals a float when it converts to it
                for (size_t i = 0; i < l->ints.size(); i++)
                    if ((double)l->ints[i] == x.f) return (int64_t)i;
                return (int64_t)l->ints.size();
            }
            return (int64_t)l->ints.size();
        case LK_FLOATS:
            if (x.isNum()) return (int64_t)simdFindFloat(l->floats.data(), l->floats.size(), x.num());
            return (int64_t)l->floats.size();
        case LK_BOOLS:
            if (x.t == VT::Bool)
                for (size_t i = 0; i < l->bools.size(); i++)
                    if ((l->bools[i] != 0) == x.b) return (int64_t)i;
            return (int64_t)l->bools.size();
        case LK_EMPTY: return 0;
        default: return -1;
    }
}
NATIVE(l_has) {
    int64_t at = findPacked(L(a[0]), a[1]);
    if (at >= 0) return Value::boolean((size_t)at < L(a[0])->len());
    for (Value x : V(a[0]))
        if (vm.equals(x, a[1])) return Value::boolean(true);
    return Value::boolean(false);
}
NATIVE(l_find) {
    auto* l = L(a[0]);
    int64_t at = findPacked(l, a[1]);
    if (at >= 0) return (size_t)at < l->len() ? Value::integer(at) : Value();
    for (size_t i = 0; i < l->len(); i++)
        if (vm.equals(l->get(i), a[1])) return Value::integer((int64_t)i);
    return Value();
}
NATIVE(l_copy) { return vm.newList(V(a[0]).vec()); }
NATIVE(l_slice) {
    SeqView v = V(a[0]);
    int64_t len = (int64_t)v.size();
    int64_t lo = vm.argInt(a, 1, "slice");
    int64_t hi = n > 2 ? vm.argInt(a, 2, "slice") : len;
    if (lo < 0) lo += len;
    if (hi < 0) hi += len;
    lo = std::clamp<int64_t>(lo, 0, len);
    hi = std::clamp<int64_t>(hi, lo, len);
    return vm.newList(std::vector<Value>(v.begin() + lo, v.begin() + hi));
}
NATIVE(l_join) {
    auto items = toVector(vm, a[0], "join");
    std::string sep = n > 1 ? vm.argStr(a, 1, "join")->s : "";
    std::string out;
    for (size_t i = 0; i < items.size(); i++) {
        if (i) out += sep;
        out += vm.toStr(items[i]);
    }
    return vm.vstr(out);
}
// min and max of a packed list of ints; floats without NaN (else: as compare() does it)
static bool packedMinMax(Value v, bool max, Value& out) {
    if (!isObjType(v, OT::List)) return false;
    auto* l = as<ObjList>(v);
    if (l->lk() == LK_INTS && !l->ints.empty()) {
        out = Value::integer(max ? simdMaxInts(l->ints.data(), l->ints.size()) : simdMinInts(l->ints.data(), l->ints.size()));
        return true;
    }
    if (l->lk() == LK_FLOATS && !l->floats.empty()) {
        double best = l->floats[0];
        for (double d : l->floats) {
            if (d != d) return false;
            if (max ? d > best : d < best) best = d;
        }
        out = Value::number(best);
        return true;
    }
    return false;
}
NATIVE(l_sum_fast) {
    if (isObjType(a[0], OT::List) && as<ObjList>(a[0])->lk() == LK_FLOATS) {
        auto* l = as<ObjList>(a[0]);
        return Value::number(simdSumFloatsFast(l->floats.data(), l->floats.size()));
    }
    return n_sum(vm, a, n);
}
NATIVE(l_min) {
    if (Value r; packedMinMax(a[0], false, r)) return r;
    auto items = toVector(vm, a[0], "min");
    if (items.empty()) return Value();
    Value best = items[0];
    for (auto& x : items)
        if (vm.compare(x, best) < 0) best = x;
    return best;
}
NATIVE(l_max) {
    if (Value r; packedMinMax(a[0], true, r)) return r;
    auto items = toVector(vm, a[0], "max");
    if (items.empty()) return Value();
    Value best = items[0];
    for (auto& x : items)
        if (vm.compare(x, best) > 0) best = x;
    return best;
}
NATIVE(l_prod) {
    auto items = toVector(vm, a[0], "prod");
    bool allInt = true;
    int64_t ip = 1;
    double fp = 1;
    for (auto& v : items) {
        if (!v.isNum()) vm.panic("prod() of a list containing a " + vm.typeName(v));
        if (v.isInt())
            ip = (int64_t)((uint64_t)ip * (uint64_t)v.i);
        else
            allInt = false;
        fp *= v.num();
    }
    return allInt ? Value::integer(ip) : Value::number(fp);
}
NATIVE(l_uniq) {
    auto items = toVector(vm, a[0], "uniq");
    auto* seen = vm.alloc<ObjSet>();
    std::vector<Value> out;
    for (auto& x : items) {
        requireHashable(vm, x);
        if (seen->add(x)) out.push_back(x);
    }
    return vm.newList(std::move(out));
}
NATIVE(l_flat) {
    std::vector<Value> out;
    for (auto& x : toVector(vm, a[0], "flat")) {
        if (isObjType(x, OT::List) || isObjType(x, OT::Tuple)) {
            auto inner = toVector(vm, x, "flat");
            out.insert(out.end(), inner.begin(), inner.end());
        } else {
            out.push_back(x);
        }
    }
    return vm.newList(std::move(out));
}
NATIVE(l_indexed) {
    auto items = toVector(vm, a[0], "indexed");
    std::vector<Value> out;
    for (size_t i = 0; i < items.size(); i++) out.push_back(vm.newTuple({Value::integer((int64_t)i), items[i]}));
    return vm.newList(std::move(out));
}
NATIVE(l_list) { return vm.newList(toVector(vm, a[0], "list")); }
NATIVE(l_set_conv) { return c_set(vm, a, 1); }
NATIVE(l_chunks) {
    auto items = toVector(vm, a[0], "chunks");
    int64_t k = vm.argInt(a, 1, "chunks");
    if (k <= 0) vm.panic("chunks() size must be positive");
    std::vector<Value> out;
    for (size_t i = 0; i < items.size(); i += (size_t)k)
        out.push_back(vm.newList(std::vector<Value>(items.begin() + i, items.begin() + std::min(items.size(), i + (size_t)k))));
    return vm.newList(std::move(out));
}

// ------------------------------------------------------------------ tuple

NATIVE(t_len) { return Value::integer((int64_t)as<ObjTuple>(a[0])->items.size()); }
NATIVE(t_has) {
    for (auto& x : as<ObjTuple>(a[0])->items)
        if (vm.equals(x, a[1])) return Value::boolean(true);
    return Value::boolean(false);
}

// ------------------------------------------------------------------ map

static ObjMap* M(Value v) { return as<ObjMap>(v); }

NATIVE(mp_len) { return Value::integer((int64_t)M(a[0])->live); }
NATIVE(mp_get) {
    Value* v = M(a[0])->find(a[1]);
    return v ? *v : (n > 2 ? a[2] : Value());
}
// m[k] += v (and -=) on a map: a missing key starts out empty, so the result is v
// (-v for -=): counts[w] += 1, totals[k] += 2.5, names[k] += "x", groups[k] += [x]
NATIVE(mp_update) {
    Value m = a[0], k = a[1], v = a[2];
    int64_t op = a[3].i;
    requireHashable(vm, k);
    Value* cur = M(m)->find(k);
    Value r;
    if (cur) {
        Value c = *cur;
        r = vm.binaryOp((uint8_t)op, c, v);
    } else if (op == OP_SUB && v.isNum()) {
        r = v.isInt() ? Value::integer((int64_t)(0 - (uint64_t)v.i)) : Value::number(-v.f);
    } else if (op == OP_ADD && isObjType(v, OT::List)) {
        std::vector<Value> items = V(v).vec();
        r = vm.newList(std::move(items));
    } else {
        r = v;
    }
    M(m)->set(k, r);
    return r;
}
NATIVE(mp_set) {
    requireHashable(vm, a[1]);
    M(a[0])->set(a[1], a[2]);
    return Value();
}
NATIVE(mp_drop) { return Value::boolean(M(a[0])->drop(a[1])); }
NATIVE(mp_has) { return Value::boolean(M(a[0])->find(a[1]) != nullptr); }
NATIVE(mp_keys) {
    std::vector<Value> out;
    for (auto& [k, v] : M(a[0])->entries)
        if (!k.isUndef()) out.push_back(k);
    return vm.newList(std::move(out));
}
NATIVE(mp_values) {
    std::vector<Value> out;
    for (auto& [k, v] : M(a[0])->entries)
        if (!k.isUndef()) out.push_back(v);
    return vm.newList(std::move(out));
}
NATIVE(mp_pairs) {
    std::vector<Value> out;
    for (auto& [k, v] : M(a[0])->entries)
        if (!k.isUndef()) out.push_back(vm.newTuple({k, v}));
    return vm.newList(std::move(out));
}
NATIVE(mp_is_empty) { return Value::boolean(M(a[0])->live == 0); }
NATIVE(mp_wipe) {
    M(a[0])->clear();
    return Value();
}
NATIVE(mp_copy) {
    auto* m = vm.alloc<ObjMap>();
    for (auto& [k, v] : M(a[0])->entries)
        if (!k.isUndef()) m->set(k, v);
    return Value::object(m);
}
NATIVE(mp_merge) {
    if (!isObjType(a[1], OT::Map)) vm.panic("merge() expects a map");
    for (auto& [k, v] : M(a[1])->entries)
        if (!k.isUndef()) M(a[0])->set(k, v);
    return a[0];
}

// ------------------------------------------------------------------ set

static ObjSet* ST(Value v) { return as<ObjSet>(v); }

NATIVE(st_len) { return Value::integer((int64_t)ST(a[0])->live); }
NATIVE(st_add) {
    requireHashable(vm, a[1]);
    return Value::boolean(ST(a[0])->add(a[1]));
}
NATIVE(st_drop) { return Value::boolean(ST(a[0])->drop(a[1])); }
NATIVE(st_has) { return Value::boolean(ST(a[0])->has(a[1])); }
NATIVE(st_count) { return Value::integer(ST(a[0])->has(a[1]) ? 1 : 0); }
NATIVE(st_is_empty) { return Value::boolean(ST(a[0])->live == 0); }
NATIVE(st_wipe) {
    auto* s = ST(a[0]);
    s->items.clear();
    s->index.clear();
    s->live = 0;
    return Value();
}
NATIVE(st_union) {
    auto* out = vm.alloc<ObjSet>();
    for (auto& x : ST(a[0])->items) if (!x.isUndef()) out->add(x);
    for (auto& x : toVector(vm, a[1], "union")) out->add(x);
    return Value::object(out);
}
NATIVE(st_inter) {
    auto* out = vm.alloc<ObjSet>();
    auto other = toVector(vm, a[1], "inter");
    auto* o = vm.alloc<ObjSet>();
    for (auto& x : other) o->add(x);
    for (auto& x : ST(a[0])->items) if (!x.isUndef() && o->has(x)) out->add(x);
    return Value::object(out);
}
NATIVE(st_diff) {
    auto* out = vm.alloc<ObjSet>();
    auto* o = vm.alloc<ObjSet>();
    for (auto& x : toVector(vm, a[1], "diff")) o->add(x);
    for (auto& x : ST(a[0])->items) if (!x.isUndef() && !o->has(x)) out->add(x);
    return Value::object(out);
}
NATIVE(st_copy) {
    auto* out = vm.alloc<ObjSet>();
    for (auto& x : ST(a[0])->items) if (!x.isUndef()) out->add(x);
    return Value::object(out);
}

// ------------------------------------------------------------------ range

NATIVE(r_len) { return Value::integer(as<ObjRange>(a[0])->count()); }
NATIVE(r_has) {
    if (!a[1].isNum() || a[1].num() != std::floor(a[1].num())) return Value::boolean(false);
    return Value::boolean(as<ObjRange>(a[0])->contains((int64_t)a[1].num()));
}
NATIVE(r_step) {
    auto* r = as<ObjRange>(a[0]);
    return makeRange(vm, Value::integer(r->start), Value::integer(r->end), a[1], r->inclusive);
}

void registerCollections(VM& vm) {
    vm.defineNative("list", c_list, 0, 1, "fn(iter?) -> list", "A new list, optionally from anything iterable.");
    vm.defineNative("tuple", c_tuple, 0, 1, "fn(iter?) -> tuple", "A tuple from anything iterable.");
    vm.defineNative("map", c_map, 0, 1, "fn(list?) -> map", "A new map, optionally from (key, value) pairs.");
    vm.defineNative("set", c_set, 0, 1, "fn(iter?) -> set", "A new set, optionally from anything iterable.");
    vm.defineNative("len", n_len, 1, 1, "fn(any) -> int", "Length of a string or collection.");
    vm.defineNative("zip", n_zip, 2, 2, "fn(iter, iter) -> list", "Pairs up two sequences into a list of tuples.");
    vm.defineNative("sum", n_sum, 1, 1, "fn(iter) -> num", "Sum of the numbers.");

    Tag T_ = Tag::List;
    vm.defineMethod(T_, "len", l_len, 0, 0, "fn() -> int", "Number of items.");
    vm.defineMethod(T_, "add", l_add, 1, 1, "fn(T) -> list[T]", "Appends an item (returns the list).");
    vm.defineMethod(T_, "push", l_add, 1, 1, "fn(T) -> list[T]", "Same as add.");
    vm.defineMethod(T_, "take", l_take, 0, 0, "fn() -> T?", "Removes and returns the last item (nil if empty).");
    vm.defineNative("__sum_into", n_sum_into, 2, 2, "fn(num, any) -> num", "");
    for (Tag t : {Tag::List, Tag::Tuple, Tag::Str, Tag::Range, Tag::Set})
        vm.defineMethod(t, "counts", l_counts, 0, 0, "fn() -> map[any, int]",
                        "How many times each item occurs, as a map in first-seen order: words.counts.");
    vm.defineMethod(T_, "heap_push", l_heap_push, 1, 1, "fn(T) -> list[T]", "Adds an item to a list kept as a min-heap (a priority queue).");
    vm.defineMethod(T_, "heap_pop", l_heap_pop, 0, 0, "fn() -> T?", "Removes and returns the smallest item of a list kept as a min-heap (nil if empty).");
    vm.defineMethod(T_, "pop", l_take, 0, 0, "fn() -> T?", "Same as take.");
    vm.defineMethod(T_, "get", l_get, 1, 2, "fn(int, any) -> T?", "The item at an index, or nil/default when out of range.");
    vm.defineMethod(T_, "set", l_set, 2, 2, "fn(int, T)", "Replaces the item at an index.");
    vm.defineMethod(T_, "insert", l_insert, 2, 2, "fn(int, T)", "Inserts before an index.");
    vm.defineMethod(T_, "drop", l_drop, 1, 1, "fn(int) -> T?", "Removes and returns the item at an index.");
    vm.defineMethod(T_, "remove", l_remove, 1, 1, "fn(T) -> bool", "Removes the first item equal to the value.");
    vm.defineMethod(T_, "wipe", l_wipe, 0, 0, "fn()", "Removes every item.");
    vm.defineMethod(T_, "clear", l_wipe, 0, 0, "fn()", "Same as wipe.");
    vm.defineMethod(T_, "sort", l_sort, 0, 1, "fn(fn(T, T) -> bool?) -> list[T]", "Sorted copy; optional comparator like sort(_a > _b).");
    vm.defineMethod(T_, "sort!", l_sort_inplace, 0, 1, "fn(fn(T, T) -> bool?) -> list[T]", "Sorts in place.");
    vm.defineMethod(T_, "sort_by", l_sort_by, 1, 1, "fn(fn(T) -> any) -> list[T]", "Sorted copy ordered by a key: sort_by(_.age).");
    vm.defineMethod(T_, "rev", l_rev, 0, 0, "fn() -> list[T]", "Reversed copy.");
    vm.defineMethod(T_, "rev!", l_rev_inplace, 0, 0, "fn() -> list[T]", "Reverses in place.");
    vm.defineMethod(T_, "first", l_first, 0, 0, "fn() -> T?", "First item or nil.");
    vm.defineMethod(T_, "last", l_last, 0, 0, "fn() -> T?", "Last item or nil.");
    vm.defineMethod(T_, "is_empty", l_is_empty, 0, 0, "fn() -> bool", "True if there are no items.");
    vm.defineMethod(T_, "has", l_has, 1, 1, "fn(T) -> bool", "True if an item equals the value.");
    vm.defineMethod(T_, "find", l_find, 1, 1, "fn(T) -> int?", "Index of the first equal item, or nil.");
    vm.defineMethod(T_, "copy", l_copy, 0, 0, "fn() -> list[T]", "Shallow copy.");
    vm.defineMethod(T_, "slice", l_slice, 1, 2, "fn(int, int?) -> list[T]", "Items from start up to (not including) end.");
    vm.defineMethod(T_, "join", l_join, 0, 1, "fn(str?) -> str", "Joins the items as strings.");
    vm.defineMethod(T_, "min", l_min, 0, 0, "fn() -> T?", "Smallest item.");
    vm.defineMethod(T_, "max", l_max, 0, 0, "fn() -> T?", "Largest item.");
    vm.defineMethod(T_, "sum", n_sum, 0, 0, "fn() -> num", "Sum of the items.");
    vm.defineMethod(T_, "sum_fast", l_sum_fast, 0, 0, "fn() -> num",
                    "Sum of the items, floats added in any order (faster; the last digits may differ from sum).");
    vm.defineMethod(T_, "prod", l_prod, 0, 0, "fn() -> num", "Product of the items.");
    vm.defineMethod(T_, "uniq", l_uniq, 0, 0, "fn() -> list[T]", "Copy without duplicates (first occurrence kept).");
    vm.defineMethod(T_, "flat", l_flat, 0, 0, "fn() -> list", "Flattens one level of nested lists.");
    vm.defineMethod(T_, "indexed", l_indexed, 0, 0, "fn() -> list[(int, T)]", "List of (index, item) pairs.");
    vm.defineMethod(T_, "zip", n_zip, 1, 1, "fn(iter) -> list", "Pairs items with another sequence.");
    vm.defineMethod(T_, "set", l_set, 2, 2, "fn(int, T)", "Replaces the item at an index.");
    vm.defineMethod(T_, "to_set", l_set_conv, 0, 0, "fn() -> set[T]", "A set of the items.");
    vm.defineMethod(T_, "chunks", l_chunks, 1, 1, "fn(int) -> list[list[T]]", "Splits into lists of n items.");

    for (Tag t : {Tag::Tuple, Tag::Range, Tag::Set, Tag::Str, Tag::Map}) {
        vm.defineMethod(t, "list", l_list, 0, 0, "fn() -> list", "The items as a list.");
        vm.defineMethod(t, "indexed", l_indexed, 0, 0, "fn() -> list", "List of (index, item) pairs.");
        vm.defineMethod(t, "min", l_min, 0, 0, "fn() -> any", "Smallest item.");
        vm.defineMethod(t, "max", l_max, 0, 0, "fn() -> any", "Largest item.");
        vm.defineMethod(t, "sum", n_sum, 0, 0, "fn() -> num", "Sum of the items.");
        vm.defineMethod(t, "sort", l_sort, 0, 1, "fn(fn(any, any) -> bool?) -> list", "Sorted list of the items.");
        vm.defineMethod(t, "sort_by", l_sort_by, 1, 1, "fn(fn(any) -> any) -> list", "Sorted by a key.");
        vm.defineMethod(t, "zip", n_zip, 1, 1, "fn(iter) -> list", "Pairs items with another sequence.");
    }
    vm.defineMethod(Tag::Tuple, "len", t_len, 0, 0, "fn() -> int", "Number of items.");
    vm.defineMethod(Tag::Tuple, "has", t_has, 1, 1, "fn(any) -> bool", "True if an item equals the value.");
    vm.defineMethod(Tag::Tuple, "first", l_first, 0, 0, "fn() -> any", "First item.");
    vm.defineMethod(Tag::Tuple, "last", l_last, 0, 0, "fn() -> any", "Last item.");
    vm.defineMethod(Tag::Tuple, "rev", l_rev, 0, 0, "fn() -> list", "Reversed items as a list.");
    vm.defineMethod(Tag::Range, "rev", l_rev, 0, 0, "fn() -> list[int]", "Reversed list of the numbers.");
    vm.defineMethod(Tag::Range, "first", l_first, 0, 0, "fn() -> int?", "First number.");
    vm.defineMethod(Tag::Range, "last", l_last, 0, 0, "fn() -> int?", "Last number.");
    vm.defineMethod(Tag::Range, "len", r_len, 0, 0, "fn() -> int", "How many numbers the range has.");
    vm.defineMethod(Tag::Range, "has", r_has, 1, 1, "fn(int) -> bool", "True if the number is in the range.");
    vm.defineMethod(Tag::Range, "step", r_step, 1, 1, "fn(int) -> range", "The same range with another step.");
    vm.defineMethod(Tag::Range, "prod", l_prod, 0, 0, "fn() -> int", "Product of the numbers.");

    Tag M_ = Tag::Map;
    vm.defineMethod(M_, "len", mp_len, 0, 0, "fn() -> int", "Number of entries.");
    vm.defineMethod(M_, "get", mp_get, 1, 2, "fn(K, any) -> V?", "Value for a key, or nil/default.");
    vm.defineMethod(M_, "$update", mp_update, 3, 3, "fn(K, any, int) -> any", "");
    vm.defineMethod(M_, "set", mp_set, 2, 2, "fn(K, V)", "Sets the value for a key.");
    vm.defineMethod(M_, "drop", mp_drop, 1, 1, "fn(K) -> bool", "Removes a key; true if it was there.");
    vm.defineMethod(M_, "has", mp_has, 1, 1, "fn(K) -> bool", "True if the key is present.");
    vm.defineMethod(M_, "keys", mp_keys, 0, 0, "fn() -> list[K]", "The keys, in insertion order.");
    vm.defineMethod(M_, "values", mp_values, 0, 0, "fn() -> list[V]", "The values, in insertion order.");
    vm.defineMethod(M_, "pairs", mp_pairs, 0, 0, "fn() -> list[(K, V)]", "(key, value) tuples.");
    vm.defineMethod(M_, "is_empty", mp_is_empty, 0, 0, "fn() -> bool", "True if there are no entries.");
    vm.defineMethod(M_, "wipe", mp_wipe, 0, 0, "fn()", "Removes every entry.");
    vm.defineMethod(M_, "clear", mp_wipe, 0, 0, "fn()", "Same as wipe.");
    vm.defineMethod(M_, "copy", mp_copy, 0, 0, "fn() -> map[K, V]", "Shallow copy.");
    vm.defineMethod(M_, "merge", mp_merge, 1, 1, "fn(map[K, V]) -> map[K, V]", "Adds all entries of another map.");

    Tag S_ = Tag::Set;
    vm.defineMethod(S_, "len", st_len, 0, 0, "fn() -> int", "Number of elements.");
    vm.defineMethod(S_, "add", st_add, 1, 1, "fn(T) -> bool", "Adds an element; true if it was new.");
    vm.defineMethod(S_, "drop", st_drop, 1, 1, "fn(T) -> bool", "Removes an element.");
    vm.defineMethod(S_, "has", st_has, 1, 1, "fn(T) -> bool", "True if the element is present.");
    vm.defineMethod(S_, "count", st_count, 1, 1, "fn(T) -> int", "1 if present, else 0.");
    vm.defineMethod(S_, "is_empty", st_is_empty, 0, 0, "fn() -> bool", "True if empty.");
    vm.defineMethod(S_, "wipe", st_wipe, 0, 0, "fn()", "Removes every element.");
    vm.defineMethod(S_, "union", st_union, 1, 1, "fn(iter) -> set[T]", "Elements in either.");
    vm.defineMethod(S_, "inter", st_inter, 1, 1, "fn(iter) -> set[T]", "Elements in both.");
    vm.defineMethod(S_, "diff", st_diff, 1, 1, "fn(iter) -> set[T]", "Elements not in the other.");
    vm.defineMethod(S_, "copy", st_copy, 0, 0, "fn() -> set[T]", "Shallow copy.");
}

}  // namespace ixo
