// Runtime values: a small tagged union. Heap objects live in object.hpp.
#pragma once

#include <cstdint>
#include <string>

namespace ixo {

struct Obj;

enum class VT : uint8_t {
    Nil,
    Bool,
    Int,
    Float,
    Obj,
    Undef,  // internal: a variable that hasn't been assigned yet / missing argument
    Done,   // internal: iterator exhausted
};

struct Value {
    VT t;
    union {
        bool b;
        int64_t i;
        double f;
        Obj* o;
    };
    constexpr Value() : t(VT::Nil), i(0) {}
    static Value nil() { return Value(); }
    static Value boolean(bool v) {
        Value x;
        x.t = VT::Bool;
        x.b = v;
        return x;
    }
    static Value integer(int64_t v) {
        Value x;
        x.t = VT::Int;
        x.i = v;
        return x;
    }
    static Value number(double v) {
        Value x;
        x.t = VT::Float;
        x.f = v;
        return x;
    }
    static Value object(Obj* p) {
        Value x;
        x.t = VT::Obj;
        x.o = p;
        return x;
    }
    static Value undef() {
        Value x;
        x.t = VT::Undef;
        return x;
    }
    static Value done() {
        Value x;
        x.t = VT::Done;
        return x;
    }
    bool isNil() const { return t == VT::Nil; }
    bool isBool() const { return t == VT::Bool; }
    bool isInt() const { return t == VT::Int; }
    bool isFloat() const { return t == VT::Float; }
    bool isNum() const { return t == VT::Int || t == VT::Float; }
    bool isObj() const { return t == VT::Obj; }
    bool isUndef() const { return t == VT::Undef; }
    bool isDone() const { return t == VT::Done; }
    double num() const { return t == VT::Int ? (double)i : f; }
};

// Kinds of values in a type profile (ObjFunction::typeSeen).
enum ProfBits : uint8_t { PB_INT = 1, PB_FLOAT = 2, PB_BOOL = 4, PB_NIL = 8, PB_OTHER = 16,
                          PB_NOTLIST = 32 };  // (indexing: the container wasn't a list)
inline uint8_t profBit(const Value& v) {
    switch (v.t) {
        case VT::Int: return PB_INT;
        case VT::Float: return PB_FLOAT;
        case VT::Bool: return PB_BOOL;
        case VT::Nil: return PB_NIL;
        default: return PB_OTHER;
    }
}

}  // namespace ixo
