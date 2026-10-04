// What an LLVM backend would compile Ixo into WITHOUT type specialization: the
// same runtime model as the VM (16-byte tagged values, a type check on every
// operation with an out-of-line fallback, wrapping ints, fields behind a class
// guard, bounds-checked lists, calls through module variables). The workloads in
// this folder are written against it and compiled by clang -O2, to measure how
// much LLVM's optimizer gains over Ixo's own JIT on the same representation.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dyn {

enum Tag : uint8_t { NIL, BOOL, INT, FLOAT, OBJ };

struct Value {
    Tag t;
    union {
        bool b;
        int64_t i;
        double f;
        void* o;
    };
};
static_assert(sizeof(Value) == 16, "Ixo values are 16 bytes");

inline Value I(int64_t v) { Value x; x.t = INT; x.i = v; return x; }
inline Value F(double v) { Value x; x.t = FLOAT; x.f = v; return x; }
inline Value B(bool v) { Value x; x.t = BOOL; x.b = v; return x; }
inline Value O(void* p) { Value x; x.t = OBJ; x.o = p; return x; }

// the runtime's slow paths (operator methods, errors): never inlined
[[noreturn]] __attribute__((noinline)) inline void fail(const char* what) {
    std::fprintf(stderr, "panic: %s\n", what);
    std::exit(1);
}
__attribute__((noinline)) inline Value slowArith(int op, Value a, Value b) {
    (void)op; (void)a; (void)b;
    fail("not numbers");
}

inline double num(Value v) { return v.t == INT ? (double)v.i : v.f; }
inline bool isNum(Value v) { return v.t == INT || v.t == FLOAT; }

inline Value add(Value a, Value b) {
    if (a.t == INT && b.t == INT) return I((int64_t)((uint64_t)a.i + (uint64_t)b.i));
    if (isNum(a) && isNum(b)) return F(num(a) + num(b));
    return slowArith(0, a, b);
}
inline Value sub(Value a, Value b) {
    if (a.t == INT && b.t == INT) return I((int64_t)((uint64_t)a.i - (uint64_t)b.i));
    if (isNum(a) && isNum(b)) return F(num(a) - num(b));
    return slowArith(1, a, b);
}
inline Value mul(Value a, Value b) {
    if (a.t == INT && b.t == INT) return I((int64_t)((uint64_t)a.i * (uint64_t)b.i));
    if (isNum(a) && isNum(b)) return F(num(a) * num(b));
    return slowArith(2, a, b);
}
inline Value div(Value a, Value b) {
    if (isNum(a) && isNum(b) && !(b.t == INT && b.i == 0 && a.t == INT)) return F(num(a) / num(b));
    return slowArith(3, a, b);
}
inline Value mod(Value a, Value b) {  // floored, like Ixo
    if (a.t == INT && b.t == INT && b.i != 0) {
        int64_t m = a.i % b.i;
        if (m != 0 && ((m ^ b.i) < 0)) m += b.i;
        return I(m);
    }
    return slowArith(4, a, b);
}
inline bool lt(Value a, Value b) {
    if (a.t == INT && b.t == INT) return a.i < b.i;
    if (isNum(a) && isNum(b)) return num(a) < num(b);
    return slowArith(5, a, b).b;
}
inline bool le(Value a, Value b) {
    if (a.t == INT && b.t == INT) return a.i <= b.i;
    if (isNum(a) && isNum(b)) return num(a) <= num(b);
    return slowArith(6, a, b).b;
}
inline bool truthy(Value v) { return v.t == BOOL ? v.b : v.t != NIL; }
inline Value sqrtv(Value v) {
    if (!isNum(v)) fail("sqrt of a non-number");
    return F(std::sqrt(num(v)));
}

// objects
struct Class { int id; };
struct Instance {
    uint8_t type;  // 1 = instance
    Class* cls;
    Value fields[1];  // more follow
};
struct List {
    uint8_t type;  // 2 = list
    std::vector<Value> items;
};
inline Instance* newInstance(Class* c, int n) {
    auto* p = static_cast<Instance*>(std::malloc(sizeof(Instance) + sizeof(Value) * (size_t)(n - 1)));
    p->type = 1;
    p->cls = c;
    return p;
}
// obj.field: guarded like an inline cache hit
inline Value& field(Value o, Class* expected, int k) {
    if (o.t != OBJ || static_cast<Instance*>(o.o)->type != 1 || static_cast<Instance*>(o.o)->cls != expected)
        fail("field cache miss");
    return static_cast<Instance*>(o.o)->fields[k];
}
inline Value& index(Value l, Value i) {
    if (l.t != OBJ || static_cast<List*>(l.o)->type != 2 || i.t != INT) fail("index");
    auto& v = static_cast<List*>(l.o)->items;
    int64_t k = i.i < 0 ? i.i + (int64_t)v.size() : i.i;
    if (k < 0 || k >= (int64_t)v.size()) fail("index out of range");
    return v[(size_t)k];
}

// module variables: a function is called through its global, guarded
struct Function;
using Code = Value (*)(Value* args);
struct Function {
    uint8_t type;  // 3 = function
    int arity;
    Code code;
};
inline Value call1(Value callee, Value a) {
    if (callee.t != OBJ || static_cast<Function*>(callee.o)->type != 3 || static_cast<Function*>(callee.o)->arity != 1)
        fail("not callable");
    return static_cast<Function*>(callee.o)->code(&a);
}

}  // namespace dyn
