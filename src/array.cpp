// Numeric arrays: xs.array, array(n), array(items).
//
// An array is a packed list (ints, floats or bools) whose arithmetic and ordering
// comparisons go item by item, a plain number standing for every item:
// ys = xs * 2.0 + offsets, xs[xs > 0], (a - b).abs.max. Everything a list does, an
// array does too (indexing, len, sum, map...); == compares whole arrays. Large arrays
// are worked on by all cores.
#include <algorithm>
#include <cmath>
#include <thread>

#include "builtins.hpp"
#include "vm.hpp"

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

// f(lo, hi) over [0, n): split across the cores when n is large (the parts are independent)
template <class F>
static void parallelFor(size_t n, F f) {
#ifdef __EMSCRIPTEN__
    f((size_t)0, n);
#else
    static const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    if (n < (size_t(1) << 20) || cores < 2) {
        f((size_t)0, n);
        return;
    }
    unsigned parts = std::min(cores, 16u);
    size_t step = (n + parts - 1) / parts;
    std::vector<std::thread> ts;
    for (unsigned p = 1; p < parts; p++) {
        size_t lo = p * step, hi = std::min(n, lo + step);
        if (lo < hi) ts.emplace_back(f, lo, hi);
    }
    f((size_t)0, std::min(n, step));
    for (auto& t : ts) t.join();
#endif
}

static ObjList* newArray(VM& vm, ListKind k, size_t n) {
    auto* l = vm.alloc<ObjList>();
    l->become(k);
    switch (k) {
        case LK_BOOLS: l->bools.resize(n); break;
        case LK_INTS: l->ints.resize(n); break;
        default: l->floats.resize(n); break;
    }
    l->appendable = 1;
    vm.grow(l, n * l->elemSize());
    return l;
}

// A copy of numbers as an array (ints stay ints, any float makes floats, bools stay bools).
static Value toArray(VM& vm, Value v, const char* what) {
    std::vector<Value> items;
    if (isObjType(v, OT::List)) {
        auto* src = as<ObjList>(v);
        ListKind k = src->lk();
        if (k == LK_INTS || k == LK_FLOATS || k == LK_BOOLS) {
            ObjList* l = newArray(vm, k, 0);
            if (k == LK_INTS) l->ints = src->ints;
            else if (k == LK_FLOATS) l->floats = src->floats;
            else l->bools = src->bools;
            vm.grow(l, l->len() * l->elemSize());
            return Value::object(l);
        }
    }
    SeqView sv;
    if (seqOf(v, sv)) {
        for (Value x : sv) items.push_back(x);
    } else if (isObjType(v, OT::Range)) {
        auto* r = as<ObjRange>(v);
        for (int64_t i = r->start; r->step > 0 ? (r->inclusive ? i <= r->end : i < r->end) : (r->inclusive ? i >= r->end : i > r->end);
             i += r->step)
            items.push_back(Value::integer(i));
    } else {
        vm.panic(std::string(what) + ": an array is made from a list, tuple or range of numbers, not a " + vm.typeName(v));
    }
    bool anyFloat = false, allBool = !items.empty();
    for (auto& x : items) {
        if (x.t == VT::Float) anyFloat = true;
        else if (x.t != VT::Int && x.t != VT::Bool)
            vm.panic(std::string(what) + ": an array holds numbers, not " + vm.typeName(x));
        if (x.t != VT::Bool) allBool = false;
    }
    ListKind k = allBool ? LK_BOOLS : anyFloat || items.empty() ? LK_FLOATS : LK_INTS;
    ObjList* l = newArray(vm, k, items.size());
    for (size_t i = 0; i < items.size(); i++) {
        Value x = items[i];
        if (k == LK_BOOLS) l->bools[i] = x.b ? 1 : 0;
        else if (k == LK_INTS) l->ints[i] = x.t == VT::Bool ? (int64_t)x.b : x.i;
        else l->floats[i] = x.t == VT::Float ? x.f : x.t == VT::Int ? (double)x.i : (double)x.b;
    }
    return Value::object(l);
}

namespace {
// One side of an operation: an array's items, or one number for every item
struct Side {
    const double* f = nullptr;
    const int64_t* i = nullptr;
    std::vector<int64_t> bools;  // bools as 0 / 1
    double fs = 0;
    int64_t is = 0;
    bool scalar = false, isInt = false;
    double F(size_t k) const { return scalar ? fs : f ? f[k] : (double)i[k]; }
    int64_t I(size_t k) const { return scalar ? is : i[k]; }
};
}  // namespace

static bool sideOf(VM& vm, Value v, Side& s, size_t& n, bool& sized) {
    if (v.t == VT::Int || v.t == VT::Bool) {
        s.scalar = s.isInt = true;
        s.is = v.t == VT::Int ? v.i : (int64_t)v.b;
        s.fs = (double)s.is;
        return true;
    }
    if (v.t == VT::Float) {
        s.scalar = true;
        s.fs = v.f;
        return true;
    }
    if (!isObjType(v, OT::List)) return false;
    auto* l = as<ObjList>(v);
    size_t len = l->len();
    if (sized && len != n) vm.panic("the arrays have different lengths (" + std::to_string(n) + " and " + std::to_string(len) + ")");
    n = len;
    sized = true;
    switch (l->lk()) {
        case LK_FLOATS: s.f = l->floats.data(); break;
        case LK_INTS:
            s.i = l->ints.data();
            s.isInt = true;
            break;
        case LK_BOOLS:
            s.bools.assign(l->bools.begin(), l->bools.end());
            s.i = s.bools.data();
            s.isInt = true;
            break;
        case LK_EMPTY: s.isInt = true; break;
        default: return false;
    }
    return true;
}

static int64_t floorDiv(int64_t x, int64_t y) {
    int64_t q = x / y;
    return (x % y != 0 && ((x < 0) != (y < 0))) ? q - 1 : q;
}
static int64_t floorMod(int64_t x, int64_t y) {
    int64_t r = x % y;
    return (r != 0 && ((r < 0) != (y < 0))) ? r + y : r;
}
static double floorModF(double x, double y) {
    double r = std::fmod(x, y);
    return (r != 0 && ((r < 0) != (y < 0))) ? r + y : r;
}

Value VM::arrayOp(uint8_t op, Value a, Value b) {
    Side x, y;
    size_t n = 0;
    bool sized = false;
    auto opName = [&]() -> std::string {
        switch (op) {
            case OP_ADD: return "+";
            case OP_SUB: return "-";
            case OP_MUL: return "*";
            case OP_DIV: return "/";
            case OP_MOD: return "%";
            case OP_IDIV: return "//";
            case OP_POW: return "**";
            case OP_LT: return "<";
            case OP_LE: return "<=";
            case OP_GT: return ">";
            case OP_GE: return ">=";
            case OP_EQ: return "==";
            case OP_NE: return "!=";
            case OP_NEG: return "-";
            default: return "this operator";
        }
    };
    bool okA = sideOf(*this, a, x, n, sized);
    bool okB = op == OP_NEG || sideOf(*this, b, y, n, sized);
    if (!okA || !okB)
        panic("can't use '" + opName() + "' on " + typeName(a) + " and " + typeName(b) +
              " (arrays work item by item with numbers and arrays of the same length)");
    if (op == OP_NEG) {
        y.scalar = y.isInt = true;
        y.is = 0;
    }
    bool cmp = op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE || op == OP_EQ || op == OP_NE;
    bool ints = x.isInt && y.isInt && op != OP_DIV;
    if (op == OP_POW && ints) {
        for (size_t k = 0; k < (y.scalar ? 1 : n); k++)
            if (y.I(k) < 0) ints = false;  // negative powers of ints are fractions
    }
    if (!cmp && op != OP_ADD && op != OP_SUB && op != OP_MUL && op != OP_DIV && op != OP_MOD && op != OP_IDIV &&
        op != OP_POW && op != OP_NEG)
        panic("arrays don't support '" + opName() + "'");
    if (ints && (op == OP_MOD || op == OP_IDIV))
        for (size_t k = 0; k < (y.scalar ? 1 : n); k++)
            if (y.I(k) == 0) panic("division by zero in an array " + opName());
    ObjList* out = newArray(*this, cmp ? LK_BOOLS : ints ? LK_INTS : LK_FLOATS, n);
    if (cmp) {
        uint8_t* r = out->bools.data();
        parallelFor(n, [&](size_t lo, size_t hi) {
            for (size_t k = lo; k < hi; k++) {
                bool v;
                if (x.isInt && y.isInt) {
                    int64_t p = x.I(k), q = y.I(k);
                    v = op == OP_LT ? p < q : op == OP_LE ? p <= q : op == OP_GT ? p > q : op == OP_GE ? p >= q : op == OP_EQ ? p == q : p != q;
                } else {
                    double p = x.F(k), q = y.F(k);
                    v = op == OP_LT ? p < q : op == OP_LE ? p <= q : op == OP_GT ? p > q : op == OP_GE ? p >= q : op == OP_EQ ? p == q : p != q;
                }
                r[k] = v ? 1 : 0;
            }
        });
    } else if (ints) {
        int64_t* r = out->ints.data();
        parallelFor(n, [&](size_t lo, size_t hi) {
            switch (op) {
                case OP_ADD: for (size_t k = lo; k < hi; k++) r[k] = (int64_t)((uint64_t)x.I(k) + (uint64_t)y.I(k)); break;
                case OP_SUB: for (size_t k = lo; k < hi; k++) r[k] = (int64_t)((uint64_t)x.I(k) - (uint64_t)y.I(k)); break;
                case OP_NEG: for (size_t k = lo; k < hi; k++) r[k] = (int64_t)(0 - (uint64_t)x.I(k)); break;
                case OP_MUL: for (size_t k = lo; k < hi; k++) r[k] = (int64_t)((uint64_t)x.I(k) * (uint64_t)y.I(k)); break;
                case OP_MOD: for (size_t k = lo; k < hi; k++) r[k] = floorMod(x.I(k), y.I(k)); break;
                case OP_IDIV: for (size_t k = lo; k < hi; k++) r[k] = floorDiv(x.I(k), y.I(k)); break;
                case OP_POW:
                    for (size_t k = lo; k < hi; k++) {
                        int64_t base = x.I(k), e = y.I(k), p = 1;
                        while (e > 0) {
                            if (e & 1) p = (int64_t)((uint64_t)p * (uint64_t)base);
                            base = (int64_t)((uint64_t)base * (uint64_t)base);
                            e >>= 1;
                        }
                        r[k] = p;
                    }
                    break;
                default: break;
            }
        });
    } else {
        double* r = out->floats.data();
        parallelFor(n, [&](size_t lo, size_t hi) {
            switch (op) {
                case OP_ADD: for (size_t k = lo; k < hi; k++) r[k] = x.F(k) + y.F(k); break;
                case OP_SUB: for (size_t k = lo; k < hi; k++) r[k] = x.F(k) - y.F(k); break;
                case OP_NEG: for (size_t k = lo; k < hi; k++) r[k] = -x.F(k); break;
                case OP_MUL: for (size_t k = lo; k < hi; k++) r[k] = x.F(k) * y.F(k); break;
                case OP_DIV: for (size_t k = lo; k < hi; k++) r[k] = x.F(k) / y.F(k); break;
                case OP_MOD: for (size_t k = lo; k < hi; k++) r[k] = floorModF(x.F(k), y.F(k)); break;
                case OP_IDIV: for (size_t k = lo; k < hi; k++) r[k] = std::floor(x.F(k) / y.F(k)); break;
                case OP_POW: for (size_t k = lo; k < hi; k++) r[k] = std::pow(x.F(k), y.F(k)); break;
                default: break;
            }
        });
    }
    return Value::object(out);
}

// xs[mask] (the items where a bool array is true) and xs[positions] (an int array)
Value VM::arrayIndex(Value arr, Value idx) {
    auto* l = as<ObjList>(arr);
    auto* ix = as<ObjList>(idx);
    size_t n = l->len();
    std::vector<size_t> pick;
    if (ix->lk() == LK_BOOLS) {
        if (ix->len() != n) panic("a mask must be as long as the array (" + std::to_string(ix->len()) + " and " + std::to_string(n) + ")");
        for (size_t k = 0; k < n; k++)
            if (ix->bools[k]) pick.push_back(k);
    } else if (ix->lk() == LK_INTS || ix->lk() == LK_EMPTY) {
        for (size_t k = 0; k < ix->len(); k++) {
            int64_t p = ix->ints[k];
            if (p < 0) p += (int64_t)n;
            if (p < 0 || p >= (int64_t)n) panic("index " + std::to_string(ix->ints[k]) + " is out of range for an array of length " + std::to_string(n));
            pick.push_back((size_t)p);
        }
    } else {
        panic("an array is indexed by an int, a range, a bool mask or a list of positions, not a " + typeName(idx));
    }
    ListKind k = l->lk() == LK_EMPTY ? LK_FLOATS : l->lk();
    ObjList* out = newArray(*this, k, pick.size());
    for (size_t j = 0; j < pick.size(); j++) {
        if (k == LK_BOOLS) out->bools[j] = l->bools[pick[j]];
        else if (k == LK_INTS) out->ints[j] = l->ints[pick[j]];
        else out->floats[j] = l->floats[pick[j]];
    }
    return Value::object(out);
}

// ------------------------------------------------------------------ natives

static ObjList* needArray(VM& vm, Value v, const char* what) {
    if (!isArray(v)) vm.panic(std::string(".") + what + " works on arrays: xs.array." + what);
    return as<ObjList>(v);
}

NATIVE(a_array) { return toArray(vm, a[0], "array"); }
NATIVE(a_list) {
    auto* l = as<ObjList>(a[0]);
    std::vector<Value> items;
    items.reserve(l->len());
    for (size_t i = 0; i < l->len(); i++) items.push_back(l->get(i));
    return vm.newList(std::move(items));
}
NATIVE(g_array) {
    if (a[0].t == VT::Int) {  // array(n) or array(n, fill)
        if (a[0].i < 0) vm.panic("array(n): n can't be negative");
        Value fill = n > 1 ? a[1] : Value::number(0.0);
        size_t len = (size_t)a[0].i;
        if (fill.t == VT::Int) {
            ObjList* l = newArray(vm, LK_INTS, len);
            std::fill(l->ints.begin(), l->ints.end(), fill.i);
            return Value::object(l);
        }
        if (fill.t == VT::Bool) {
            ObjList* l = newArray(vm, LK_BOOLS, len);
            std::fill(l->bools.begin(), l->bools.end(), fill.b ? 1 : 0);
            return Value::object(l);
        }
        if (fill.t != VT::Float) vm.panic("array(n, fill): fill is a number, not a " + vm.typeName(fill));
        ObjList* l = newArray(vm, LK_FLOATS, len);
        std::fill(l->floats.begin(), l->floats.end(), fill.f);
        return Value::object(l);
    }
    return toArray(vm, a[0], "array");
}

template <class F>
static Value mapFloats(VM& vm, Value v, const char* what, F f) {
    ObjList* l = needArray(vm, v, what);
    size_t n = l->len();
    ObjList* out = newArray(vm, LK_FLOATS, n);
    double* r = out->floats.data();
    if (l->lk() == LK_FLOATS) {
        const double* s = l->floats.data();
        parallelFor(n, [&](size_t lo, size_t hi) {
            for (size_t k = lo; k < hi; k++) r[k] = f(s[k]);
        });
    } else {
        for (size_t k = 0; k < n; k++) r[k] = f(l->get(k).num());
    }
    return Value::object(out);
}

NATIVE(a_sqrt) { return mapFloats(vm, a[0], "sqrt", [](double x) { return std::sqrt(x); }); }
NATIVE(a_exp) { return mapFloats(vm, a[0], "exp", [](double x) { return std::exp(x); }); }
NATIVE(a_ln) { return mapFloats(vm, a[0], "ln", [](double x) { return std::log(x); }); }
NATIVE(a_sin) { return mapFloats(vm, a[0], "sin", [](double x) { return std::sin(x); }); }
NATIVE(a_cos) { return mapFloats(vm, a[0], "cos", [](double x) { return std::cos(x); }); }
NATIVE(a_floor) { return mapFloats(vm, a[0], "floor", [](double x) { return std::floor(x); }); }
NATIVE(a_ceil) { return mapFloats(vm, a[0], "ceil", [](double x) { return std::ceil(x); }); }
NATIVE(a_round) { return mapFloats(vm, a[0], "round", [](double x) { return std::nearbyint(x); }); }
NATIVE(a_abs) {
    ObjList* l = needArray(vm, a[0], "abs");
    if (l->lk() == LK_INTS) {
        ObjList* out = newArray(vm, LK_INTS, l->len());
        for (size_t k = 0; k < l->len(); k++) out->ints[k] = l->ints[k] < 0 ? -l->ints[k] : l->ints[k];
        return Value::object(out);
    }
    return mapFloats(vm, a[0], "abs", [](double x) { return std::fabs(x); });
}
NATIVE(a_clamp) {
    ObjList* l = needArray(vm, a[0], "clamp");
    if (!a[1].isNum() || !a[2].isNum()) vm.panic("clamp(lo, hi) takes two numbers");
    if (l->lk() == LK_INTS && a[1].t == VT::Int && a[2].t == VT::Int) {
        ObjList* out = newArray(vm, LK_INTS, l->len());
        for (size_t k = 0; k < l->len(); k++) out->ints[k] = std::clamp(l->ints[k], a[1].i, a[2].i);
        return Value::object(out);
    }
    double lo = a[1].num(), hi = a[2].num();
    return mapFloats(vm, a[0], "clamp", [lo, hi](double x) { return std::clamp(x, lo, hi); });
}
NATIVE(a_mean) {
    ObjList* l = needArray(vm, a[0], "mean");
    if (!l->len()) return Value::number(NAN);
    double s = 0;
    for (size_t k = 0; k < l->len(); k++) s += l->get(k).num();
    return Value::number(s / (double)l->len());
}
NATIVE(a_dot) {
    ObjList* l = needArray(vm, a[0], "dot");
    Side x, y;
    size_t len = 0;
    bool sized = false;
    if (!sideOf(vm, a[0], x, len, sized) || !sideOf(vm, a[1], y, len, sized) || y.scalar)
        vm.panic("dot takes an array of the same length");
    (void)l;
    if (x.isInt && y.isInt) {
        int64_t s = 0;
        for (size_t k = 0; k < len; k++) s = (int64_t)((uint64_t)s + (uint64_t)x.I(k) * (uint64_t)y.I(k));
        return Value::integer(s);
    }
    double s = 0;
    for (size_t k = 0; k < len; k++) s += x.F(k) * y.F(k);
    return Value::number(s);
}

void registerArrays(VM& vm) {
    vm.defineNative("array", g_array, 1, 2, "fn(any, num?) -> array[float]",
                    "A numeric array: array(n) is n zeros, array(n, fill), array(items) from numbers.");
    for (Tag t : {Tag::List, Tag::Tuple, Tag::Range})
        vm.defineMethod(t, "array", a_array, 0, 0, "fn() -> array[T]",
                        "The numbers as an array, whose arithmetic goes item by item: xs.array * 2.0.");
    vm.defineMethod(Tag::List, "list", a_list, 0, 0, "fn() -> list[T]", "The items as a plain list (from an array).");
    struct M {
        const char* name;
        NativeFn fn;
        const char* sig;
        const char* doc;
    };
    for (const M& m : {M{"sqrt", a_sqrt, "fn() -> array[float]", "Square root of every item (arrays)."},
                       M{"exp", a_exp, "fn() -> array[float]", "e to the power of every item (arrays)."},
                       M{"ln", a_ln, "fn() -> array[float]", "Natural logarithm of every item (arrays)."},
                       M{"sin", a_sin, "fn() -> array[float]", "Sine of every item (arrays)."},
                       M{"cos", a_cos, "fn() -> array[float]", "Cosine of every item (arrays)."},
                       M{"floor", a_floor, "fn() -> array[float]", "Every item rounded down (arrays)."},
                       M{"ceil", a_ceil, "fn() -> array[float]", "Every item rounded up (arrays)."},
                       M{"round", a_round, "fn() -> array[float]", "Every item rounded to the nearest whole number (arrays)."},
                       M{"abs", a_abs, "fn() -> array[T]", "Absolute value of every item (arrays)."},
                       M{"mean", a_mean, "fn() -> float", "The average of the items (arrays)."}})
        vm.defineMethod(Tag::List, m.name, m.fn, 0, 0, m.sig, m.doc);
    vm.defineMethod(Tag::List, "clamp", a_clamp, 2, 2, "fn(num, num) -> array[T]", "Every item limited to [lo, hi] (arrays).");
    vm.defineMethod(Tag::List, "dot", a_dot, 1, 1, "fn(array[num]) -> num", "The sum of the products of the items (arrays).");
}

}  // namespace ixo
