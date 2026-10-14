// Value semantics: printing, equality, ordering, hashing, operators,
// indexing, iteration and gradual type checks.
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <charconv>
#include <cmath>
#include <cstring>

#include "vm.hpp"

namespace ixo {

// ------------------------------------------------------------------ utf-8

bool isAscii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

// byte offset of the i-th code point (i may equal the length)
static size_t utf8Offset(const std::string& s, size_t cp) {
    if (isAscii(s)) return std::min(cp, s.size());
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (n == cp) return i;
            n++;
        }
    }
    return s.size();
}

static size_t utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// ------------------------------------------------------------------ type info

Tag VM::tagOf(Value v) {
    switch (v.t) {
        case VT::Nil: return Tag::Nil;
        case VT::Bool: return Tag::Bool;
        case VT::Int: return Tag::Int;
        case VT::Float: return Tag::Float;
        case VT::Obj: break;
        default: return Tag::Nil;
    }
    switch (v.o->type) {
        case OT::String: return Tag::Str;
        case OT::List: return Tag::List;
        case OT::Tuple: return Tag::Tuple;
        case OT::Map: return Tag::Map;
        case OT::Set: return Tag::Set;
        case OT::Range: return Tag::Range;
        case OT::Closure:
        case OT::Native:
        case OT::Bound:
        case OT::Function: return Tag::Fn;
        case OT::Class: return Tag::Class;
        case OT::Instance: return Tag::Instance;
        case OT::Error: return Tag::Error;
        case OT::Channel: return Tag::Flow;
        case OT::Task: return Tag::Task;
        case OT::Module:
        case OT::Namespace: return Tag::Module;
        case OT::Vec: return Tag::Vec;
        case OT::Mat: return Tag::Mat;
        case OT::Quat: return Tag::Quat;
        case OT::Rng: return Tag::Rng;
        case OT::GpuBuffer: return Tag::GpuBuf;
        case OT::Time: return Tag::Time;
        case OT::Iterator: return Tag::Iter;
        default: return Tag::Nil;
    }
}

std::string VM::typeName(Value v) {
    switch (v.t) {
        case VT::Nil: return "nil";
        case VT::Bool: return "bool";
        case VT::Int: return "int";
        case VT::Float: return "float";
        case VT::Undef: return "<unset>";
        case VT::Done: return "<done>";
        case VT::Obj: break;
    }
    switch (v.o->type) {
        case OT::String: return "str";
        case OT::List: return "list";
        case OT::Tuple: return "tuple";
        case OT::Map: return "map";
        case OT::Set: return "set";
        case OT::Range: return "range";
        case OT::Closure:
        case OT::Native:
        case OT::Bound:
        case OT::Function: return "fn";
        case OT::Class: return "class";
        case OT::Instance: return static_cast<ObjInstance*>(v.o)->cls->name;
        case OT::Error: return "Error";
        case OT::Channel: return "flow";
        case OT::Task: return "task";
        case OT::Module: return "module";
        case OT::Namespace: return static_cast<ObjNamespace*>(v.o)->name;
        case OT::Vec: return "vec";
        case OT::Mat: return "mat";
        case OT::Quat: return "quat";
        case OT::Rng: return "rng";
        case OT::GpuBuffer: return "gpu buffer";
        case OT::Time: return "time";
        case OT::Iterator: return "iterator";
        case OT::Interface: return "interface";
        default: return "object";
    }
}

// ------------------------------------------------------------------ printing

static std::string fmtFloat(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

static std::string quoteStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default: o += c;
        }
    }
    return o + "\"";
}

std::string VM::toStr(Value v) { return toStrDepth(v, false, 0); }
std::string VM::repr(Value v) { return toStrDepth(v, true, 0); }

std::string VM::toStrDepth(Value v, bool quote, int depth) {
    if (depth > 20) return "...";
    switch (v.t) {
        case VT::Nil: return "nil";
        case VT::Bool: return v.b ? "true" : "false";
        case VT::Int: return std::to_string(v.i);
        case VT::Float: return fmtFloat(v.f);
        case VT::Undef: return "<unset>";
        case VT::Done: return "<done>";
        case VT::Obj: break;
    }
    auto join = [&](SeqView items, const char* open, const char* close) {
        std::string s = open;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) s += ", ";
            s += toStrDepth(items[i], true, depth + 1);
        }
        return s + close;
    };
    switch (v.o->type) {
        case OT::String: return quote ? quoteStr(as<ObjString>(v)->s) : as<ObjString>(v)->s;
        case OT::List: return join(SeqView(*as<ObjList>(v)), "[", "]");
        case OT::Tuple: {
            auto& items = as<ObjTuple>(v)->items;
            if (items.size() == 1) return "(" + toStrDepth(items[0], true, depth + 1) + ",)";
            return join(items, "(", ")");
        }
        case OT::Map: {
            auto* m = as<ObjMap>(v);
            if (m->live == 0) return "{}";
            std::string s = "{";
            bool first = true;
            for (auto& [k, val] : m->entries) {
                if (k.isUndef()) continue;
                if (!first) s += ", ";
                first = false;
                s += toStrDepth(k, true, depth + 1) + ": " + toStrDepth(val, true, depth + 1);
            }
            return s + "}";
        }
        case OT::Set: {
            auto* st = as<ObjSet>(v);
            if (st->live == 0) return "set()";
            std::string s = "{";
            bool first = true;
            for (auto& x : st->items) {
                if (x.isUndef()) continue;
                if (!first) s += ", ";
                first = false;
                s += toStrDepth(x, true, depth + 1);
            }
            return s + "}";
        }
        case OT::Range: {
            auto* r = as<ObjRange>(v);
            std::string s = (r->openStart ? std::string() : std::to_string(r->start)) + (r->inclusive ? "..=" : "..<") +
                            (r->openEnd ? std::string() : std::to_string(r->end));
            if (r->step != 1) s += ", " + std::to_string(r->step);
            return s;
        }
        case OT::Closure: return "<fn " + as<ObjClosure>(v)->fn->name + ">";
        case OT::Native: return "<fn " + as<ObjNative>(v)->name + ">";
        case OT::Bound: return "<bound method>";
        case OT::Function: return "<fn " + as<ObjFunction>(v)->name + ">";
        case OT::Class: return "<class " + as<ObjClass>(v)->name + ">";
        case OT::Instance: {
            auto* inst = as<ObjInstance>(v);
            auto it = inst->cls->methods.find(sym("str"));
            if (it != inst->cls->methods.end() && current) {
                Value bound = Value::object(alloc<ObjBound>(v, it->second));
                Value r = callNested(bound, nullptr, 0);
                return isObjType(r, OT::String) ? as<ObjString>(r)->s : toStrDepth(r, quote, depth + 1);
            }
            if (inst->fields.empty()) return inst->cls->name;
            std::string s = inst->cls->name + "(";
            for (size_t i = 0; i < inst->fields.size(); i++) {
                if (i) s += ", ";
                s += inst->cls->fields[i] + ": " + toStrDepth(inst->fields[i], true, depth + 1);
            }
            return s + ")";
        }
        case OT::Error: {
            auto* e = as<ObjError>(v);
            std::string s = e->kind == "Error" ? "Error" : "Error." + e->kind;
            if (!e->msg.empty() && e->msg != e->kind) s += ": " + e->msg;
            return s;
        }
        case OT::Channel: return "<flow>";
        case OT::Task: return "<task #" + std::to_string(as<ObjTask>(v)->id) + ">";
        case OT::Module: return "<module " + as<ObjModule>(v)->rt->name + ">";
        case OT::Namespace: return "<" + as<ObjNamespace>(v)->name + ">";
        case OT::Iterator: return "<iterator>";
        case OT::Vec: {
            std::string s = "vec(";
            auto& d = as<ObjVec>(v)->v;
            for (size_t i = 0; i < d.size(); i++) s += (i ? ", " : "") + fmtFloat(d[i]);
            return s + ")";
        }
        case OT::Mat: {
            auto* m = as<ObjMat>(v);
            std::string s = "mat[" + std::to_string(m->rows) + "x" + std::to_string(m->cols) + "](";
            for (int r = 0; r < m->rows; r++) {
                if (r) s += ", ";
                s += "[";
                for (int c = 0; c < m->cols; c++) s += (c ? ", " : "") + fmtFloat(m->at(r, c));
                s += "]";
            }
            return s + ")";
        }
        case OT::Quat: {
            auto* q = as<ObjQuat>(v);
            return "quat(" + fmtFloat(q->x) + ", " + fmtFloat(q->y) + ", " + fmtFloat(q->z) + ", " + fmtFloat(q->w) + ")";
        }
        case OT::Rng: return "<rng>";
        case OT::GpuBuffer: {
            auto* b = static_cast<ObjGpuBuffer*>(v.o);
            return std::string("<gpu buffer of ") + std::to_string(b->n) + (b->floats ? " floats>" : " ints>");
        }
        case OT::Time: {
            Value t = v;
            auto& table = methods[(int)Tag::Time];
            auto it = table.find(sym("show"));
            if (it != table.end()) {
                Value args[2] = {t, vstr("YYYY-MM-DD HH:mm:ss")};
                return as<ObjString>(as<ObjNative>(it->second.fn)->fn(*this, args, 2))->s;
            }
            return "<time>";
        }
        case OT::TypeDesc: return "<type " + as<ObjTypeDesc>(v)->text + ">";
        case OT::Interface: return "<interface " + as<ObjInterface>(v)->name + ">";
        default: return "<object>";
    }
}

// ------------------------------------------------------------------ equality & ordering

bool VM::equals(Value a, Value b) {
    if (a.isNum() && b.isNum()) {
        if (a.t == VT::Int && b.t == VT::Int) return a.i == b.i;
        return a.num() == b.num();
    }
    if (a.t != b.t) return false;
    switch (a.t) {
        case VT::Nil: case VT::Undef: case VT::Done: return true;
        case VT::Bool: return a.b == b.b;
        default: break;
    }
    if (a.o == b.o) return true;
    if (a.o->type != b.o->type) return false;
    switch (a.o->type) {
        case OT::String: return as<ObjString>(a)->s == as<ObjString>(b)->s;
        case OT::List:
        case OT::Tuple: {
            SeqView x, y;
            seqOf(a, x);
            seqOf(b, y);
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); i++)
                if (!equals(x[i], y[i])) return false;
            return true;
        }
        case OT::Map: {
            auto* x = as<ObjMap>(a);
            auto* y = as<ObjMap>(b);
            if (x->live != y->live) return false;
            for (auto& [k, v] : x->entries) {
                if (k.isUndef()) continue;
                Value* other = y->find(k);
                if (!other || !equals(v, *other)) return false;
            }
            return true;
        }
        case OT::Set: {
            auto* x = as<ObjSet>(a);
            auto* y = as<ObjSet>(b);
            if (x->live != y->live) return false;
            for (auto& v : x->items)
                if (!v.isUndef() && !y->has(v)) return false;
            return true;
        }
        case OT::Range: {
            auto* x = as<ObjRange>(a);
            auto* y = as<ObjRange>(b);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive;
        }
        case OT::Error: return as<ObjError>(a)->kind == as<ObjError>(b)->kind;
        case OT::Vec: return as<ObjVec>(a)->v == as<ObjVec>(b)->v;
        case OT::Mat: {
            auto* x = as<ObjMat>(a);
            auto* y = as<ObjMat>(b);
            return x->rows == y->rows && x->cols == y->cols && x->d == y->d;
        }
        case OT::Quat: {
            auto* x = as<ObjQuat>(a);
            auto* y = as<ObjQuat>(b);
            return x->x == y->x && x->y == y->y && x->z == y->z && x->w == y->w;
        }
        case OT::Time: return as<ObjTime>(a)->epoch == as<ObjTime>(b)->epoch;
        case OT::Instance: {
            Value r;
            if (callOperator(a, "==", &b, 1, r)) return truthy(r);
            return false;
        }
        default: return false;
    }
}

int VM::compare(Value a, Value b) {
    if (isObjType(a, OT::Instance)) {
        Value r;
        if (callOperator(a, "<", &b, 1, r)) {
            if (truthy(r)) return -1;
            callOperator(b, "<", &a, 1, r);
            return truthy(r) ? 1 : 0;
        }
    }
    if (a.isNum() && b.isNum()) {
        if (a.t == VT::Int && b.t == VT::Int) return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
        double x = a.num(), y = b.num();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (isObjType(a, OT::String) && isObjType(b, OT::String)) {
        int c = as<ObjString>(a)->s.compare(as<ObjString>(b)->s);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    bool seqA = isObjType(a, OT::List) || isObjType(a, OT::Tuple);
    bool seqB = isObjType(b, OT::List) || isObjType(b, OT::Tuple);
    if (seqA && seqB && a.o->type == b.o->type) {
        SeqView x, y;
        seqOf(a, x);
        seqOf(b, y);
        for (size_t i = 0; i < x.size() && i < y.size(); i++) {
            int c = compare(x[i], y[i]);
            if (c) return c;
        }
        return x.size() < y.size() ? -1 : x.size() > y.size() ? 1 : 0;
    }
    if (a.t == VT::Bool && b.t == VT::Bool) return (int)a.b - (int)b.b;
    if (isObjType(a, OT::Time) && isObjType(b, OT::Time)) {
        double x = as<ObjTime>(a)->epoch, y = as<ObjTime>(b)->epoch;
        return x < y ? -1 : x > y ? 1 : 0;
    }
    panic("can't compare " + typeName(a) + " and " + typeName(b));
}

size_t hashValue(const Value& v) {
    switch (v.t) {
        case VT::Nil: return 0x9e3779b9;
        case VT::Bool: return v.b ? 1231 : 1237;
        case VT::Int: return std::hash<int64_t>()(v.i);
        case VT::Float: {
            double d = v.f;
            if (d == std::floor(d) && std::fabs(d) < 9.2e18) return std::hash<int64_t>()((int64_t)d);
            return std::hash<double>()(d);
        }
        case VT::Obj: break;
        default: return 7;
    }
    switch (v.o->type) {
        case OT::String: return static_cast<ObjString*>(v.o)->hash();
        case OT::Tuple: {
            size_t h = 0x345678;
            for (auto& x : static_cast<ObjTuple*>(v.o)->items) h = h * 1000003 ^ hashValue(x);
            return h;
        }
        case OT::Error: return std::hash<std::string>()(static_cast<ObjError*>(v.o)->kind);
        case OT::Range: {
            auto* r = static_cast<ObjRange*>(v.o);
            return std::hash<int64_t>()(r->start * 31 + r->end * 17 + r->step);
        }
        default: return std::hash<const void*>()(v.o);
    }
}

bool keyEquals(const Value& a, const Value& b) {
    if (a.isNum() && b.isNum()) {
        if (a.t == VT::Int && b.t == VT::Int) return a.i == b.i;
        return a.num() == b.num();
    }
    if (a.t != b.t) return false;
    switch (a.t) {
        case VT::Nil: case VT::Undef: case VT::Done: return true;
        case VT::Bool: return a.b == b.b;
        default: break;
    }
    if (a.o == b.o) return true;
    if (a.o->type != b.o->type) return false;
    switch (a.o->type) {
        case OT::String: return static_cast<ObjString*>(a.o)->s == static_cast<ObjString*>(b.o)->s;
        case OT::Tuple: {
            auto& x = static_cast<ObjTuple*>(a.o)->items;
            auto& y = static_cast<ObjTuple*>(b.o)->items;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); i++)
                if (!keyEquals(x[i], y[i])) return false;
            return true;
        }
        case OT::Error: return static_cast<ObjError*>(a.o)->kind == static_cast<ObjError*>(b.o)->kind;
        case OT::Range: {
            auto* x = static_cast<ObjRange*>(a.o);
            auto* y = static_cast<ObjRange*>(b.o);
            return x->start == y->start && x->end == y->end && x->step == y->step && x->inclusive == y->inclusive;
        }
        default: return false;
    }
}

void requireHashable(VM& vm, Value v) {
    if (!v.isObj()) return;
    switch (v.o->type) {
        case OT::List: vm.panic("a list can't be a map key or set element (use a tuple instead)");
        case OT::Map: vm.panic("a map can't be a map key or set element");
        case OT::Set: vm.panic("a set can't be a map key or set element");
        case OT::Tuple:
            for (auto& x : as<ObjTuple>(v)->items) requireHashable(vm, x);
            return;
        default: return;
    }
}

// ------------------------------------------------------------------ arguments

int64_t VM::argInt(Value* args, int i, const char* fname) {
    Value v = args[i];
    if (v.t == VT::Int) return v.i;
    if (v.t == VT::Float && v.f == std::floor(v.f) && std::fabs(v.f) < 9.2e18) return (int64_t)v.f;
    panic(std::string(fname) + "() expected a whole number, got " + (v.t == VT::Float ? toStr(v) : typeName(v)));
}

double VM::argNum(Value* args, int i, const char* fname) {
    Value v = args[i];
    if (v.isNum()) return v.num();
    panic(std::string(fname) + "() expected a number, got " + typeName(v));
}

ObjString* VM::argStr(Value* args, int i, const char* fname) {
    Value v = args[i];
    if (isObjType(v, OT::String)) return as<ObjString>(v);
    panic(std::string(fname) + "() expected a str, got " + typeName(v));
}

ObjList* VM::argList(Value* args, int i, const char* fname) {
    Value v = args[i];
    if (isObjType(v, OT::List)) return as<ObjList>(v);
    panic(std::string(fname) + "() expected a list, got " + typeName(v));
}

Value VM::getMethod(Value receiver, const std::string& name) {
    int s = sym(name);
    if (isObjType(receiver, OT::Instance)) {
        auto* inst = as<ObjInstance>(receiver);
        auto it = inst->cls->methods.find(s);
        if (it != inst->cls->methods.end()) return Value::object(alloc<ObjBound>(receiver, it->second));
    }
    auto& table = methods[(int)tagOf(receiver)];
    auto it = table.find(s);
    if (it != table.end()) {
        checkFrozen(receiver, s);
        return Value::object(alloc<ObjBound>(receiver, it->second.fn));
    }
    return Value();
}

// ------------------------------------------------------------------ operator methods

bool VM::callOperator(Value self, const char* op, const Value* args, int argc, Value& out) {
    if (!isObjType(self, OT::Instance) || !current) return false;
    auto* inst = as<ObjInstance>(self);
    auto it = inst->cls->methods.find(sym(op));
    if (it == inst->cls->methods.end()) return false;
    Value bound = Value::object(alloc<ObjBound>(self, it->second));
    out = callNested(bound, args, argc);
    return true;
}

bool VM::implements(Value v, ObjInterface* iface) {
    if (!isObjType(v, OT::Instance)) return false;
    auto* cls = as<ObjInstance>(v)->cls;
    for (int s : iface->required)
        if (!cls->methods.count(s)) return false;
    return true;
}

// `match` against a value: a class, interface or enum means "is one of
// those"; anything else compares with ==.
bool VM::matchValue(Value v, Value pat) {
    if (isObjType(pat, OT::Class)) return isObjType(v, OT::Instance) && as<ObjInstance>(v)->cls == as<ObjClass>(pat);
    if (isObjType(pat, OT::Interface)) return implements(v, as<ObjInterface>(pat));
    if (isObjType(pat, OT::Namespace) && isObjType(v, OT::Instance)) {
        const std::string& prefix = as<ObjNamespace>(pat)->name + ".";
        return as<ObjInstance>(v)->cls->name.compare(0, prefix.size(), prefix) == 0;
    }
    return equals(v, pat);
}

// ------------------------------------------------------------------ ranges

static int64_t wholeNumber(VM& vm, Value v, const char* what) {
    if (v.t == VT::Int) return v.i;
    if (v.t == VT::Float && v.f == std::floor(v.f) && std::fabs(v.f) < 9.2e18) return (int64_t)v.f;
    if (v.t == VT::Float) vm.panic(std::string(what) + " must be a whole number, got " + vm.toStr(v));
    vm.panic(std::string(what) + " must be a number, got " + vm.typeName(v));
}

// for x in a..b: the bounds checked as makeRange and makeIterator would, as ints in place.
void rangeLoopInit(VM& vm, Value* v) {
    int64_t s = v[0].isNil() ? 0 : wholeNumber(vm, v[0], "range start");
    int64_t e = v[1].isNil() ? 0 : wholeNumber(vm, v[1], "range end");
    int64_t st = wholeNumber(vm, v[2], "range step");
    if (st == 0) vm.panic("range step can't be 0");
    if (v[0].isNil() || v[1].isNil()) vm.panic("a range with an open end (like 2..) can only slice: xs[2..]");
    v[0] = Value::integer(s);
    v[1] = Value::integer(e);
    v[2] = Value::integer(st);
}

Value makeRange(VM& vm, Value start, Value end, Value step, bool inclusive) {
    int64_t s = wholeNumber(vm, start, "range start");
    int64_t e = wholeNumber(vm, end, "range end");
    int64_t st = wholeNumber(vm, step, "range step");
    if (st == 0) vm.panic("range step can't be 0");
    return Value::object(vm.alloc<ObjRange>(s, e, st, inclusive));
}

// ------------------------------------------------------------------ operators

static int64_t ipow(int64_t base, int64_t exp) {
    uint64_t result = 1, b = (uint64_t)base;
    while (exp > 0) {
        if (exp & 1) result *= b;
        b *= b;
        exp >>= 1;
    }
    return (int64_t)result;
}

Value vecBinary(VM& vm, uint8_t op, Value a, Value b, bool* handled);  // math.cpp

Value VM::binaryOp(uint8_t op, Value a, Value b) {
    auto opText = [&]() -> std::string {
        switch (op) {
            case OP_ADD: return "+";
            case OP_SUB: return "-";
            case OP_MUL: return "*";
            case OP_DIV: return "/";
            case OP_MOD: return "%";
            case OP_POW: return "**";
            case OP_ROOT: return "root";
            case OP_IDIV: return "//";
            case OP_BAND: return "&";
            case OP_BOR: return "|";
            case OP_BXOR: return "^";
            case OP_SHL: return "<<";
            case OP_SHR: return ">>";
            case OP_NEG: return "-";
            default: return "?";
        }
    };
    auto fail = [&]() -> Value {
        if (op == OP_NEG) panic("can't negate a " + typeName(a));
        std::string msg = "can't use '" + opText() + "' on " + typeName(a) + " and " + typeName(b);
        if (op == OP_ADD && (isObjType(a, OT::String) || isObjType(b, OT::String)))
            msg += " (build strings with f\"...{x}\" or x.str)";
        panic(msg);
    };
    switch (op) {
        case OP_EQ: return Value::boolean(equals(a, b));
        case OP_NE: return Value::boolean(!equals(a, b));
        case OP_LT: return Value::boolean(compare(a, b) < 0);
        case OP_LE: return Value::boolean(compare(a, b) <= 0);
        case OP_GT: return Value::boolean(compare(a, b) > 0);
        case OP_GE: return Value::boolean(compare(a, b) >= 0);
        case OP_IS:
        case OP_ISNOT: {
            bool same;
            if (a.t != b.t)
                same = false;
            else if (a.t == VT::Obj)
                same = a.o == b.o || (a.o->type == OT::String && b.o->type == OT::String &&
                                      as<ObjString>(a)->s == as<ObjString>(b)->s);
            else
                same = equals(a, b);
            // `x is Point` / `x is Shape`: a type test when the right side is a class, interface or enum
            if (!same && isObjType(a, OT::Instance) &&
                (isObjType(b, OT::Class) || isObjType(b, OT::Interface) || isObjType(b, OT::Namespace)))
                same = matchValue(a, b);
            return Value::boolean(op == OP_IS ? same : !same);
        }
        case OP_IN: return Value::boolean(containsValue(b, a));
        case OP_NOTIN: return Value::boolean(!containsValue(b, a));
        default: break;
    }
    if (isObjType(a, OT::Instance)) {
        const char* name = nullptr;
        switch (op) {
            case OP_ADD: name = "+"; break;
            case OP_SUB: name = "-"; break;
            case OP_MUL: name = "*"; break;
            case OP_DIV: name = "/"; break;
            case OP_MOD: name = "%"; break;
            case OP_POW: name = "**"; break;
            case OP_NEG: name = "neg"; break;
            default: break;
        }
        Value out;
        if (name && callOperator(a, name, &b, op == OP_NEG ? 0 : 1, out)) return out;
    }
    bool handled = false;
    Value r = vecBinary(*this, op, a, b, &handled);
    if (handled) return r;
    if (a.isNum() && (b.isNum() || op == OP_NEG)) {
        bool ints = a.t == VT::Int && (op == OP_NEG || b.t == VT::Int);
        switch (op) {
            case OP_NEG: return ints ? Value::integer((int64_t)(0ULL - (uint64_t)a.i)) : Value::number(-a.num());
            case OP_ADD: return ints ? Value::integer((int64_t)((uint64_t)a.i + (uint64_t)b.i)) : Value::number(a.num() + b.num());
            case OP_SUB: return ints ? Value::integer((int64_t)((uint64_t)a.i - (uint64_t)b.i)) : Value::number(a.num() - b.num());
            case OP_MUL: return ints ? Value::integer((int64_t)((uint64_t)a.i * (uint64_t)b.i)) : Value::number(a.num() * b.num());
            case OP_DIV:
                if (ints && b.i == 0) panic("division by zero");
                return Value::number(a.num() / b.num());
            case OP_MOD:
                if (ints) {
                    if (b.i == 0) panic("modulo by zero");
                    int64_t m = a.i % b.i;
                    if (m != 0 && ((m < 0) != (b.i < 0))) m += b.i;
                    return Value::integer(m);
                } else {
                    double y = b.num();
                    if (y == 0) panic("modulo by zero");
                    double m = std::fmod(a.num(), y);
                    if (m != 0 && ((m < 0) != (y < 0))) m += y;
                    return Value::number(m);
                }
            case OP_IDIV: {
                if (ints) {
                    if (b.i == 0) panic("division by zero");
                    int64_t q = a.i / b.i;
                    if ((a.i % b.i != 0) && ((a.i < 0) != (b.i < 0))) q--;
                    return Value::integer(q);
                }
                if (b.num() == 0) panic("division by zero");
                return Value::number(std::floor(a.num() / b.num()));
            }
            case OP_POW:
                if (ints && b.i >= 0) return Value::integer(ipow(a.i, b.i));
                return Value::number(std::pow(a.num(), b.num()));
            case OP_ROOT: {
                double n = b.num();
                if (n == 0) panic("0th root is undefined");
                double x = a.num();
                double res;
                if (n == 2)
                    res = std::sqrt(x);
                else if (n == 3)
                    res = std::cbrt(x);
                else if (x < 0 && std::fmod(n, 2) == 1)
                    res = -std::pow(-x, 1.0 / n);
                else
                    res = std::pow(x, 1.0 / n);
                if (ints && std::fabs(res - std::round(res)) < 1e-9 && std::fabs(res) < 9e18)
                    return Value::integer((int64_t)std::llround(res));
                return Value::number(res);
            }
            case OP_BAND: if (ints) return Value::integer(a.i & b.i); break;
            case OP_BOR: if (ints) return Value::integer(a.i | b.i); break;
            case OP_BXOR: if (ints) return Value::integer(a.i ^ b.i); break;
            case OP_SHL: if (ints) return Value::integer((int64_t)((uint64_t)a.i << (b.i & 63))); break;
            case OP_SHR: if (ints) return Value::integer(a.i >> (b.i & 63)); break;
            default: break;
        }
        return fail();
    }
    if (a.isObj() && b.isObj() && a.o->type == b.o->type) {
        switch (a.o->type) {
            case OT::String:
                if (op == OP_ADD) return vstr(as<ObjString>(a)->s + as<ObjString>(b)->s);
                break;
            case OT::List:
                if (op == OP_ADD) {
                    std::vector<Value> out = SeqView(*as<ObjList>(a)).vec();
                    SeqView y(*as<ObjList>(b));
                    out.insert(out.end(), y.begin(), y.end());
                    return newList(std::move(out));
                }
                break;
            case OT::Tuple:
                if (op == OP_ADD) {
                    std::vector<Value> out = SeqView(as<ObjTuple>(a)->items).vec();
                    auto& y = as<ObjTuple>(b)->items;
                    out.insert(out.end(), y.begin(), y.end());
                    return newTuple(std::move(out));
                }
                break;
            case OT::Set: {
                auto* x = as<ObjSet>(a);
                auto* y = as<ObjSet>(b);
                auto* out = alloc<ObjSet>();
                if (op == OP_BOR || op == OP_ADD) {
                    for (auto& v : x->items) if (!v.isUndef()) out->add(v);
                    for (auto& v : y->items) if (!v.isUndef()) out->add(v);
                } else if (op == OP_BAND) {
                    for (auto& v : x->items) if (!v.isUndef() && y->has(v)) out->add(v);
                } else if (op == OP_SUB) {
                    for (auto& v : x->items) if (!v.isUndef() && !y->has(v)) out->add(v);
                } else if (op == OP_BXOR) {
                    for (auto& v : x->items) if (!v.isUndef() && !y->has(v)) out->add(v);
                    for (auto& v : y->items) if (!v.isUndef() && !x->has(v)) out->add(v);
                } else {
                    break;
                }
                return Value::object(out);
            }
            case OT::Map:
                if (op == OP_ADD || op == OP_BOR) {
                    auto* out = alloc<ObjMap>();
                    for (auto& [k, v] : as<ObjMap>(a)->entries) if (!k.isUndef()) out->set(k, v);
                    for (auto& [k, v] : as<ObjMap>(b)->entries) if (!k.isUndef()) out->set(k, v);
                    return Value::object(out);
                }
                break;
            case OT::Time:
                if (op == OP_SUB) return Value::number(as<ObjTime>(a)->epoch - as<ObjTime>(b)->epoch);
                break;
            default: break;
        }
    }
    if (op == OP_MUL) {
        Value s = isObjType(a, OT::String) ? a : b;
        Value n = isObjType(a, OT::String) ? b : a;
        if (isObjType(s, OT::String) && n.t == VT::Int) {
            if (n.i < 0) panic("can't repeat a string a negative number of times");
            std::string out;
            out.reserve(as<ObjString>(s)->s.size() * (size_t)n.i);
            for (int64_t i = 0; i < n.i; i++) out += as<ObjString>(s)->s;
            return vstr(std::move(out));
        }
        Value l = isObjType(a, OT::List) ? a : b;
        Value m = isObjType(a, OT::List) ? b : a;
        if (isObjType(l, OT::List) && m.t == VT::Int) {
            auto* src = as<ObjList>(l);
            auto* out = alloc<ObjList>();
            out->assignRepeat(*src, (size_t)std::max<int64_t>(m.i, 0));  // packed lists stay packed
            grow(out, out->len() * out->elemSize());
            return Value::object(out);
        }
    }
    if (isObjType(a, OT::Time) && b.isNum() && (op == OP_ADD || op == OP_SUB))
        return Value::object(alloc<ObjTime>(as<ObjTime>(a)->epoch + (op == OP_ADD ? b.num() : -b.num())));
    return fail();
}

// ------------------------------------------------------------------ indexing

static int64_t normIndex(VM& vm, int64_t i, size_t len, const std::string& what) {
    int64_t n = (int64_t)len;
    int64_t j = i < 0 ? i + n : i;
    if (j < 0 || j >= n)
        vm.panic("index " + std::to_string(i) + " is out of range for a " + what + " of length " + std::to_string(len));
    return j;
}

// Converts a range to [lo, hi) bounds for slicing (negative values count from the end).
static void sliceBounds(ObjRange* r, size_t len, size_t& lo, size_t& hi) {
    int64_t n = (int64_t)len;
    int64_t s = r->start < 0 ? r->start + n : r->start;
    int64_t e = r->end < 0 ? r->end + n : r->end;
    if (r->inclusive) e += 1;
    s = std::clamp<int64_t>(s, 0, n);
    e = std::clamp<int64_t>(e, 0, n);
    if (e < s) e = s;
    lo = (size_t)s;
    hi = (size_t)e;
}

// The positions a slice selects: a range with an optional step and open ends
// (negative bounds count from the end; a negative step walks backwards).
static std::vector<size_t> sliceIndices(ObjRange* r, size_t len) {
    int64_t n = (int64_t)len, st = r->step;
    std::vector<size_t> out;
    auto norm = [&](int64_t x) { return x < 0 ? x + n : x; };
    if (st > 0) {
        int64_t s = r->openStart ? 0 : norm(r->start);
        int64_t e = r->openEnd ? n : norm(r->end) + (r->inclusive ? 1 : 0);
        s = std::clamp<int64_t>(s, 0, n);
        e = std::clamp<int64_t>(e, 0, n);
        for (int64_t i = s; i < e; i += st) out.push_back((size_t)i);
    } else {
        int64_t s = r->openStart ? n - 1 : norm(r->start);
        int64_t e = r->openEnd ? -1 : norm(r->end) - (r->inclusive ? 1 : 0);
        s = std::clamp<int64_t>(s, -1, n - 1);
        e = std::clamp<int64_t>(e, -1, n - 1);
        for (int64_t i = s; i > e; i += st) out.push_back((size_t)i);
    }
    return out;
}

static bool plainSlice(ObjRange* r) { return r->step == 1 && !r->openStart && !r->openEnd; }

// ------------------------------------------------------------------ frozen values

void VM::checkFrozen(Value receiver, int method) {
    if (!receiver.isObj() || !receiver.o->frozen) return;
    static const std::unordered_set<int> mutating = [] {
        std::unordered_set<int> s;
        for (const char* n : {"add", "push", "insert", "set", "drop", "remove", "take", "pop", "sort!", "rev!", "wipe",
                              "clear", "merge"})
            s.insert(VM::sym(n));
        return s;
    }();
    if (mutating.count(method))
        panic("can't change a frozen " + typeName(receiver) + " (." + symName(method) + "); freeze(x) makes it read-only");
}

// A deep, read-only copy that no heap owns (it lives until the program ends).
Value VM::freeze(Value v) {
    std::unordered_map<Obj*, Obj*> done;
    std::function<Value(Value)> fz = [&](Value x) -> Value {
        if (!x.isObj() || x.o->frozen) return x;
        Obj* o = x.o;
        auto it = done.find(o);
        if (it != done.end()) return Value::object(it->second);
        auto own = [&](Obj* c) {
            c->frozen = true;
            c->heapId = kFrozenHeap;
            done[o] = c;
            return c;
        };
        switch (o->type) {
            case OT::String: {
                auto* s = static_cast<ObjString*>(own(new ObjString(static_cast<ObjString*>(o)->s)));
                s->hash();  // computed now: later reads from other threads don't write
                VM::ascii(s);
                return Value::object(s);
            }
            case OT::List: {
                auto* l = static_cast<ObjList*>(own(new ObjList()));
                for (Value e : SeqView(*static_cast<ObjList*>(o))) l->push(fz(e));
                return Value::object(l);
            }
            case OT::Tuple: {
                auto& src = static_cast<ObjTuple*>(o)->items;
                void* mem = ::operator new(sizeof(ObjTuple) + src.size() * sizeof(Value));
                auto* t = static_cast<ObjTuple*>(own(tupleInit(mem, src.size())));
                for (size_t k = 0; k < src.size(); k++) t->items[k] = fz(src[k]);
                return Value::object(t);
            }
            case OT::Set: {
                auto* s = static_cast<ObjSet*>(own(new ObjSet()));
                for (auto& e : static_cast<ObjSet*>(o)->items)
                    if (!e.isUndef()) s->add(fz(e));
                return Value::object(s);
            }
            case OT::Map: {
                auto* m = static_cast<ObjMap*>(own(new ObjMap()));
                for (auto& [k, e] : static_cast<ObjMap*>(o)->entries)
                    if (!k.isUndef()) m->set(fz(k), fz(e));
                return Value::object(m);
            }
            case OT::Range: {
                auto* r = static_cast<ObjRange*>(o);
                auto* c = static_cast<ObjRange*>(own(new ObjRange(r->start, r->end, r->step, r->inclusive)));
                c->openStart = r->openStart;
                c->openEnd = r->openEnd;
                return Value::object(c);
            }
            default:
                panic("can't freeze a " + typeName(x) +
                      ": only data can be frozen (numbers, strings, lists, tuples, maps, sets, ranges)");
        }
    };
    return fz(v);
}

Value VM::getIndex(Value obj, Value idx) {
    if (!obj.isObj()) panic("can't index a " + typeName(obj));
    if (obj.o->type == OT::Instance) {
        Value out;
        if (callOperator(obj, "[]", &idx, 1, out)) return out;
    }
    switch (obj.o->type) {
        case OT::List:
        case OT::Tuple: {
            bool isList = obj.o->type == OT::List;
            SeqView items;
            seqOf(obj, items);
            if (isObjType(idx, OT::Range)) {
                auto* r = as<ObjRange>(idx);
                if (plainSlice(r)) {
                    size_t lo, hi;
                    sliceBounds(r, items.size(), lo, hi);
                    std::vector<Value> out(items.begin() + lo, items.begin() + hi);
                    return isList ? newList(std::move(out)) : newTuple(std::move(out));
                }
                std::vector<Value> out;
                for (size_t i : sliceIndices(r, items.size())) out.push_back(items[i]);
                return isList ? newList(std::move(out)) : newTuple(std::move(out));
            }
            if (!idx.isNum()) panic("list index must be an int, got " + typeName(idx));
            return items[normIndex(*this, wholeNumber(*this, idx, "index"), items.size(), typeName(obj))];
        }
        case OT::String: {
            const std::string& s = as<ObjString>(obj)->s;
            bool asc = ascii(as<ObjString>(obj));
            size_t len = asc ? s.size() : utf8Length(s);
            if (isObjType(idx, OT::Range)) {
                auto* r = as<ObjRange>(idx);
                if (plainSlice(r)) {
                    size_t lo, hi;
                    sliceBounds(r, len, lo, hi);
                    if (asc) return vstr(s.substr(lo, hi - lo));
                    size_t a = utf8Offset(s, lo), b = utf8Offset(s, hi);
                    return vstr(s.substr(a, b - a));
                }
                std::string out;
                for (size_t i : sliceIndices(r, len)) {
                    size_t off = utf8Offset(s, i);
                    out += s.substr(off, utf8CharLen((unsigned char)s[off]));
                }
                return vstr(out);
            }
            if (!idx.isNum()) panic("string index must be an int, got " + typeName(idx));
            int64_t i = normIndex(*this, wholeNumber(*this, idx, "index"), len, "str");
            if (asc) return charStr((unsigned char)s[(size_t)i]);
            size_t off = utf8Offset(s, (size_t)i);
            return vstr(s.substr(off, utf8CharLen((unsigned char)s[off])));
        }
        case OT::GpuBuffer: {
            auto* b = static_cast<ObjGpuBuffer*>(obj.o);
            int64_t i = normIndex(*this, wholeNumber(*this, idx, "index"), b->n, "gpu buffer");
            return b->floats ? Value::number(b->fl()[i]) : Value::integer(b->in()[i]);
        }
        case OT::Map: {
            Value* v = as<ObjMap>(obj)->find(idx);
            if (!v) panic("key " + repr(idx) + " not found in map (use .get(key) to get nil instead)");
            return *v;
        }
        case OT::Range: {
            auto* r = as<ObjRange>(obj);
            int64_t i = normIndex(*this, wholeNumber(*this, idx, "index"), (size_t)r->count(), "range");
            return Value::integer(r->start + i * r->step);
        }
        case OT::Vec: {
            auto& v = as<ObjVec>(obj)->v;
            return Value::number(v[normIndex(*this, wholeNumber(*this, idx, "index"), v.size(), "vec")]);
        }
        case OT::Mat: {
            auto* m = as<ObjMat>(obj);
            if (isObjType(idx, OT::Tuple) && as<ObjTuple>(idx)->items.size() == 2) {
                auto& t = as<ObjTuple>(idx)->items;
                int64_t r = normIndex(*this, wholeNumber(*this, t[0], "row"), m->rows, "matrix row");
                int64_t c = normIndex(*this, wholeNumber(*this, t[1], "column"), m->cols, "matrix column");
                return Value::number(m->at((int)r, (int)c));
            }
            int64_t r = normIndex(*this, wholeNumber(*this, idx, "row"), m->rows, "matrix");
            auto* row = alloc<ObjVec>();
            for (int c = 0; c < m->cols; c++) row->v.push_back(m->at((int)r, c));
            return Value::object(row);
        }
        default: break;
    }
    panic("can't index a " + typeName(obj));
}

void VM::setIndex(Value obj, Value idx, Value v) {
    if (obj.isObj() && obj.o->frozen) panic("can't change a frozen " + typeName(obj) + "; freeze(x) makes it read-only");
    if (isObjType(obj, OT::Instance)) {
        Value args[2] = {idx, v};
        Value out;
        if (callOperator(obj, "[]=", args, 2, out)) return;
    }
    if (isObjType(obj, OT::List)) {
        auto* l = as<ObjList>(obj);
        if (!idx.isNum()) panic("list index must be an int, got " + typeName(idx));
        size_t at = normIndex(*this, wholeNumber(*this, idx, "index"), l->len(), "list");
        size_t before = l->elemSize();
        l->set(at, v);
        if (l->elemSize() > before) grow(l, l->len() * (l->elemSize() - before));
        return;
    }
    if (isObjType(obj, OT::Map)) {
        requireHashable(*this, idx);
        as<ObjMap>(obj)->set(idx, v);
        return;
    }
    if (isObjType(obj, OT::GpuBuffer)) {  // 32-bit on the device: the value must fit
        auto* b = static_cast<ObjGpuBuffer*>(obj.o);
        int64_t i = normIndex(*this, wholeNumber(*this, idx, "index"), b->n, "gpu buffer");
        if (!v.isNum()) panic("a gpu buffer holds numbers, not a " + typeName(v));
        if (b->floats) {
            b->fl()[i] = (float)v.num();
        } else {
            if (!v.isInt()) panic("a gpu buffer of ints can't hold " + repr(v));
            if (v.i < INT32_MIN || v.i > INT32_MAX) panic("gpu ints are 32-bit: " + repr(v) + " doesn't fit");
            b->in()[i] = (int32_t)v.i;
        }
        return;
    }
    if (isObjType(obj, OT::Vec)) {
        auto& d = as<ObjVec>(obj)->v;
        if (!v.isNum()) panic("vec components must be numbers");
        d[normIndex(*this, wholeNumber(*this, idx, "index"), d.size(), "vec")] = v.num();
        return;
    }
    if (isObjType(obj, OT::Mat) && isObjType(idx, OT::Tuple)) {
        auto* m = as<ObjMat>(obj);
        auto& t = as<ObjTuple>(idx)->items;
        if (t.size() == 2 && v.isNum()) {
            int64_t r = normIndex(*this, wholeNumber(*this, t[0], "row"), m->rows, "matrix row");
            int64_t c = normIndex(*this, wholeNumber(*this, t[1], "column"), m->cols, "matrix column");
            m->at((int)r, (int)c) = v.num();
            return;
        }
    }
    if (isObjType(obj, OT::Tuple) || isObjType(obj, OT::String))
        panic("a " + typeName(obj) + " can't be changed (it's immutable)");
    panic("can't assign by index to a " + typeName(obj));
}

bool VM::containsValue(Value c, Value v) {
    if (!c.isObj()) panic("can't use 'in' with a " + typeName(c));
    switch (c.o->type) {
        case OT::List:
            for (Value x : SeqView(*as<ObjList>(c)))
                if (equals(x, v)) return true;
            return false;
        case OT::Tuple:
            for (auto& x : as<ObjTuple>(c)->items)
                if (equals(x, v)) return true;
            return false;
        case OT::String:
            if (!isObjType(v, OT::String)) panic("'in' on a string needs a string, got " + typeName(v));
            return as<ObjString>(c)->s.find(as<ObjString>(v)->s) != std::string::npos;
        case OT::Map: return as<ObjMap>(c)->find(v) != nullptr;
        case OT::Set: return as<ObjSet>(c)->has(v);
        case OT::Range: {
            if (!v.isNum()) return false;
            double d = v.num();
            if (d != std::floor(d)) return false;
            return as<ObjRange>(c)->contains((int64_t)d);
        }
        default: break;
    }
    panic("can't use 'in' with a " + typeName(c));
}

// ------------------------------------------------------------------ iteration

Value VM::makeIterator(Value v, bool pairs, bool keyed) {
    if (v.isObj()) {
        if (keyed && (v.o->type == OT::Set || v.o->type == OT::Channel))
            panic(std::string("for a, b in x gives positions or keys, and a ") + typeName(v) +
                  " has neither; to unpack items, write for (a, b) in x");
        switch (v.o->type) {
            case OT::List: case OT::Tuple: case OT::String: case OT::Map: case OT::Set: case OT::Channel: {
                auto* it = alloc<ObjIterator>(v, pairs);
                it->keyed = keyed;
                return Value::object(it);
            }
            case OT::Range: {
                if (as<ObjRange>(v)->openStart || as<ObjRange>(v)->openEnd)
                    panic("a range with an open end (like 2..) can only slice: xs[2..]");
                auto* it = alloc<ObjIterator>(v, pairs);
                it->keyed = keyed;
                it->cur = as<ObjRange>(v)->start;
                return Value::object(it);
            }
            case OT::Iterator: return v;
            default: break;
        }
    }
    panic("can't loop over a " + typeName(v));
}

bool VM::iterNext(Fiber* f, ObjIterator* it, Value& out) {
    (void)f;
    Value src = it->src;
    switch (src.o->type) {
        case OT::List: {
            auto* l = as<ObjList>(src);
            out = it->idx < l->len() ? l->get(it->idx++) : Value::done();
            return true;
        }
        case OT::Tuple: {
            auto& items = as<ObjTuple>(src)->items;
            out = it->idx < items.size() ? items[it->idx++] : Value::done();
            return true;
        }
        case OT::String: {
            const std::string& s = as<ObjString>(src)->s;
            if (it->idx >= s.size()) {
                out = Value::done();
                return true;
            }
            size_t n = utf8CharLen((unsigned char)s[it->idx]);
            out = n == 1 && (unsigned char)s[it->idx] < 0x80 ? charStr((unsigned char)s[it->idx]) : vstr(s.substr(it->idx, n));
            it->idx += n;
            it->cur++;  // characters so far (the position for `for i, c in s`)
            return true;
        }
        case OT::Range: {
            auto* r = as<ObjRange>(src);
            bool more = r->step > 0 ? (r->inclusive ? it->cur <= r->end : it->cur < r->end)
                                    : (r->inclusive ? it->cur >= r->end : it->cur > r->end);
            if (!more) {
                out = Value::done();
                return true;
            }
            out = Value::integer(it->cur);
            it->cur += r->step;
            return true;
        }
        case OT::Map: {
            auto& e = as<ObjMap>(src)->entries;
            while (it->idx < e.size() && e[it->idx].first.isUndef()) it->idx++;
            if (it->idx >= e.size()) {
                out = Value::done();
                return true;
            }
            auto& kv = e[it->idx++];
            if (it->keyed) {
                it->key = kv.first;
                out = kv.second;
            } else {
                out = it->pairs ? newTuple({kv.first, kv.second}) : kv.first;
            }
            return true;
        }
        case OT::Set: {
            auto& items = as<ObjSet>(src)->items;
            while (it->idx < items.size() && items[it->idx].isUndef()) it->idx++;
            out = it->idx < items.size() ? items[it->idx++] : Value::done();
            return true;
        }
        case OT::Channel: {
            bool blocked = false;
            out = channelRecv(as<ObjChannel>(src), true, &blocked);
            return !blocked;
        }
        default: out = Value::done(); return true;
    }
}

// ------------------------------------------------------------------ gradual type checks

bool VM::checkType(Value& v, ObjTypeDesc* t) {
    switch (t->k) {
        case ObjTypeDesc::Any: return true;
        case ObjTypeDesc::Nil: return v.isNil();
        case ObjTypeDesc::Bool: return v.isBool();
        case ObjTypeDesc::Int: return v.isInt();
        case ObjTypeDesc::Float:
            if (v.isInt()) {
                v = Value::number((double)v.i);
                return true;
            }
            return v.isFloat();
        case ObjTypeDesc::Num: return v.isNum();
        case ObjTypeDesc::Str: return isObjType(v, OT::String);
        case ObjTypeDesc::List: return isObjType(v, OT::List);
        case ObjTypeDesc::Map: return isObjType(v, OT::Map);
        case ObjTypeDesc::Set: return isObjType(v, OT::Set);
        case ObjTypeDesc::Tuple: return isObjType(v, OT::Tuple);
        case ObjTypeDesc::Fn: return isCallable(v);
        case ObjTypeDesc::Class:
            return isObjType(v, OT::Instance) && as<ObjInstance>(v)->cls->name == t->className;
        case ObjTypeDesc::Interface: {
            if (!isObjType(v, OT::Instance)) return false;
            auto* cls = as<ObjInstance>(v)->cls;
            for (int s : t->syms)
                if (!cls->methods.count(s)) return false;
            return true;
        }
        case ObjTypeDesc::EnumOf: {
            if (!isObjType(v, OT::Instance)) return false;
            const std::string& n = as<ObjInstance>(v)->cls->name;
            return n.size() > t->className.size() && n.compare(0, t->className.size(), t->className) == 0 &&
                   n[t->className.size()] == '.';
        }
        case ObjTypeDesc::Optional: return v.isNil() || checkType(v, t->inner);
        case ObjTypeDesc::Result: return isObjType(v, OT::Error) || checkType(v, t->inner);
        case ObjTypeDesc::Flow: return isObjType(v, OT::Channel);
        case ObjTypeDesc::Task: return isObjType(v, OT::Task);
        case ObjTypeDesc::Range: return isObjType(v, OT::Range);
        case ObjTypeDesc::Vec: return isObjType(v, OT::Vec);
        case ObjTypeDesc::Mat: return isObjType(v, OT::Mat);
        case ObjTypeDesc::Quat: return isObjType(v, OT::Quat);
        case ObjTypeDesc::Rng: return isObjType(v, OT::Rng);
        case ObjTypeDesc::Time: return isObjType(v, OT::Time);
        case ObjTypeDesc::Error: return isObjType(v, OT::Error);
        case ObjTypeDesc::Iter:
            return isObjType(v, OT::List) || isObjType(v, OT::Tuple) || isObjType(v, OT::String) ||
                   isObjType(v, OT::Map) || isObjType(v, OT::Set) || isObjType(v, OT::Range) ||
                   isObjType(v, OT::Channel);
    }
    return true;
}

// ------------------------------------------------------------------ f-string formatting

// Spec: [[fill]align][sign][0][width][,][.precision][type]
std::string VM::formatValue(Value v, const std::string& spec) {
    char fill = ' ', align = 0, sign = '-', type = 0;
    bool zero = false, comma = false;
    int width = -1, prec = -1;
    size_t i = 0;
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (spec.size() >= 2 && isAlign(spec[1])) {
        fill = spec[0];
        align = spec[1];
        i = 2;
    } else if (!spec.empty() && isAlign(spec[0])) {
        align = spec[0];
        i = 1;
    }
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) sign = spec[i++];
    if (i < spec.size() && spec[i] == '0') {
        zero = true;
        i++;
    }
    while (i < spec.size() && std::isdigit((unsigned char)spec[i])) width = std::max(width, 0) * 10 + (spec[i++] - '0');
    if (i < spec.size() && spec[i] == ',') {
        comma = true;
        i++;
    }
    if (i < spec.size() && spec[i] == '.') {
        i++;
        prec = 0;
        while (i < spec.size() && std::isdigit((unsigned char)spec[i])) prec = prec * 10 + (spec[i++] - '0');
    }
    if (i < spec.size()) type = spec[i++];
    if (i != spec.size()) panic("invalid format spec '" + spec + "'");

    std::string body;
    bool numeric = v.isNum();
    if (numeric) {
        double d = v.num();
        char buf[512];
        switch (type) {
            case 'f': case 'e': case 'g': {
                // to_chars rounds exactly like printf and is much faster; printf if it doesn't fit
                auto fmt = type == 'f' ? std::chars_format::fixed : type == 'e' ? std::chars_format::scientific
                                                                     : std::chars_format::general;
                auto r = std::to_chars(buf, buf + sizeof buf, d, fmt, prec < 0 ? 6 : prec);
                if (r.ec == std::errc()) {
                    body.assign(buf, r.ptr);
                } else {
                    const char* f = type == 'f' ? "%.*f" : type == 'e' ? "%.*e" : "%.*g";
                    snprintf(buf, sizeof buf, f, prec < 0 ? 6 : prec, d);
                    body = buf;
                }
                break;
            }
            case '%': snprintf(buf, sizeof buf, "%.*f%%", prec < 0 ? 0 : prec, d * 100); body = buf; break;
            case 'd': body = std::to_string(v.isInt() ? v.i : (int64_t)std::llround(d)); break;
            case 'x':
            case 'b': {
                int64_t n = v.isInt() ? v.i : (int64_t)d;
                if (type == 'x') {
                    snprintf(buf, sizeof buf, "%llx", (unsigned long long)n);
                    body = buf;
                } else {
                    uint64_t u = (uint64_t)n;
                    if (!u) body = "0";
                    while (u) {
                        body.insert(body.begin(), char('0' + (u & 1)));
                        u >>= 1;
                    }
                }
                break;
            }
            case 0:
                if (prec >= 0) {
                    snprintf(buf, sizeof buf, "%.*f", prec, d);
                    body = buf;
                } else {
                    body = toStr(v);
                }
                break;
            case 's': body = toStr(v); break;
            default: panic(std::string("unknown format type '") + type + "' for a number");
        }
        if (comma) {
            size_t start = body[0] == '-' ? 1 : 0;
            size_t end = body.find_first_of(".e%");
            if (end == std::string::npos) end = body.size();
            for (int p = (int)end - 3; p > (int)start; p -= 3) body.insert((size_t)p, ",");
        }
        if (sign == '+' && d >= 0) body = "+" + body;
        if (sign == ' ' && d >= 0) body = " " + body;
    } else {
        body = toStr(v);
        if (prec >= 0 && (size_t)prec < utf8Length(body)) body = body.substr(0, utf8Offset(body, (size_t)prec));
    }
    int len = (int)utf8Length(body);
    if (width > len) {
        int pad = width - len;
        if (zero && numeric && !align) {
            size_t at = (body[0] == '-' || body[0] == '+') ? 1 : 0;
            body.insert(at, std::string(pad, '0'));
        } else {
            char a = align ? align : (numeric ? '>' : '<');
            if (a == '<')
                body += std::string(pad, fill);
            else if (a == '>')
                body = std::string(pad, fill) + body;
            else
                body = std::string(pad / 2, fill) + body + std::string(pad - pad / 2, fill);
        }
    }
    return body;
}

}  // namespace ixo
