#include "jit.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "disasm.hpp"
#include "opcodes.hpp"
#include "vm.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#define IXO_JIT_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define IXO_JIT_ARM64 1
#endif
#if defined(IXO_JIT_X64) || defined(IXO_JIT_ARM64)
#define IXO_JIT_ANY 1
#endif

#ifdef IXO_JIT_ANY
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif
#endif

namespace ixo {

namespace {

// ------------------------------------------------------------------ decoding

struct Ins {
    size_t off = 0, next = 0;
    uint8_t op = 0;
    uint32_t a = 0, b = 0;   // operands
    uint32_t sym = 0, fi = 0;  // member name, fallback index (GET_MEMBER, INVOKE)
    uint8_t fk = 0;            // fallback kind
    int64_t target = -1;     // jump target (bytecode offset)
    int depth = -1;          // stack depth before the instruction
    bool exitOnly = false;   // not compiled: the machine code hands over to the interpreter here
    bool step = false;       // run by the interpreter for the machine code (no jumps; pops, then pushes)
    int pops = 0, pushes = 0;
    int fall = -1;           // stack depth after it (straight on)
};

void stepOp(Ins& in, int pops, int pushes) {
    in.step = true;
    in.pops = pops;
    in.pushes = pushes;
}

bool readIns(ObjFunction* fn, const std::vector<uint8_t>& code, size_t& pc, Ins& in, std::string* why) {
    auto u8 = [&]() -> uint32_t { return pc < code.size() ? code[pc++] : 0; };
    auto u16 = [&]() -> uint32_t {
        uint32_t v = pc + 1 < code.size() ? (uint32_t)(code[pc] | (code[pc + 1] << 8)) : 0;
        pc += 2;
        return v;
    };
    auto u32 = [&]() -> uint32_t {
        uint32_t v = pc + 3 < code.size() ? (uint32_t)(code[pc] | (code[pc + 1] << 8) | (code[pc + 2] << 16) |
                                                       ((uint32_t)code[pc + 3] << 24))
                                          : 0;
        pc += 4;
        return v;
    };
    in.off = pc;
    in.op = (uint8_t)u8();
    switch (in.op) {
        case OP_NIL: case OP_TRUE: case OP_FALSE: case OP_UNDEF: case OP_POP: case OP_DUP: case OP_RETURN:
        case OP_AUTOCALL: case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD: case OP_IDIV: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_EQ: case OP_NE: case OP_NEG: case OP_NOT: case OP_DIV:
        case OP_GET_INDEX: case OP_SET_INDEX:
            break;
        case OP_GET_FIELD: case OP_SET_FIELD: in.a = u16(); break;
        case OP_CONST: case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_GET_GLOBAL: case OP_SET_GLOBAL: case OP_ENDSCOPE:
        case OP_ENDSCOPE_KEEP:
            in.a = u16();
            break;
        case OP_GET_LOCAL_CHK:
        case OP_CHECK_TYPE:
            in.a = u16();
            in.b = u16();
            break;
        case OP_INC_LOCAL:
            in.a = u16();
            in.b = u8();
            break;
        case OP_CALL: case OP_RANGE: case OP_ITER_INIT: in.a = u8(); break;
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_FALSE_KEEP:
        case OP_JUMP_IF_TRUE_KEEP:
        case OP_JUMP_IF_NOT_ERROR: {
            uint32_t rel = u32();
            in.target = (int64_t)pc + rel;
            break;
        }
        case OP_JUMP_IF_SET: {
            in.a = u16();
            uint32_t rel = u32();
            in.target = (int64_t)pc + rel;
            break;
        }
        case OP_LOOP: {
            uint32_t rel = u32();
            in.target = (int64_t)pc - rel;
            break;
        }
        case OP_JUMP_IF_NOT_CMP: {
            in.a = u8();
            uint32_t rel = u32();
            in.target = (int64_t)pc + rel;
            break;
        }
        case OP_FOR_NEXT: case OP_FOR_SEQ: {
            in.a = u16();
            uint32_t rel = u32();
            in.target = (int64_t)pc + rel;
            break;
        }
        // instructions the interpreter runs for the machine code, which then carries on
        case OP_GET_BUILTIN: case OP_GET_GLOBAL_OPT: case OP_ITER_KEY: in.a = u16(); stepOp(in, 0, 1); break;
        case OP_LIST: case OP_TUPLE: case OP_SET: case OP_CONCAT: in.a = u16(); stepOp(in, (int)in.a, 1); break;
        case OP_MAP: in.a = u16(); stepOp(in, 2 * (int)in.a, 1); break;
        case OP_TODO: in.a = u16(); stepOp(in, 0, 0); break;
        case OP_LIST_APPEND: in.a = u16(); stepOp(in, 1, 0); break;
        case OP_FORMAT: case OP_POS_FIELD: case OP_ELEM: case OP_TEST_KEY: case OP_KEY: case OP_EXPORT_GET:
            in.a = u16();
            stepOp(in, 1, 1);
            break;
        case OP_POW: case OP_ROOT: case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: case OP_IS:
        case OP_ISNOT: case OP_IN: case OP_NOTIN: case OP_INSTANCE_OF: case OP_MATCH_VALUE: case OP_SEND:
            stepOp(in, 2, 1);
            break;
        case OP_BNOT: case OP_RECV: case OP_RECV_ITER: stepOp(in, 1, 1); break;
        case OP_SLEEP: stepOp(in, 1, 0); break;
        case OP_SWAP: stepOp(in, 2, 2); break;
        case OP_ROT3: stepOp(in, 3, 3); break;
        case OP_DUP2: stepOp(in, 2, 4); break;
        case OP_DESTRUCT: in.a = u8(); stepOp(in, 1, (int)in.a); break;
        case OP_TEST_TYPE: u8(); stepOp(in, 1, 1); break;
        case OP_TEST_LEN: u8(); u8(); u16(); stepOp(in, 1, 1); break;
        case OP_SLICE_FROM: u16(); u16(); stepOp(in, 1, 1); break;
        case OP_STR_APPEND: in.a = u16(); u8(); stepOp(in, 1, 0); break;
        case OP_STR_APPEND_G: in.a = u16(); u8(); stepOp(in, 1, 0); break;
        case OP_GET_UPVAL: in.a = u16(); u16(); stepOp(in, 0, 1); break;
        case OP_SET_UPVAL: in.a = u16(); stepOp(in, 1, 0); break;
        case OP_STR_SEAL: in.a = u16(); stepOp(in, 0, 0); break;
        case OP_FSTR_BEGIN: stepOp(in, 0, 1); break;
        case OP_FSTR_ADD: in.a = u16(); stepOp(in, 2, 1); break;
        case OP_FSTR_LIT: in.a = u16(); stepOp(in, 1, 1); break;
        case OP_JUMP_IF_NOT_SEQ: {
            uint32_t rel = u32();
            in.target = (int64_t)pc + rel;
            break;
        }
        case OP_RANGE_INIT: case OP_SEQ_INIT: break;
        case OP_FOR_RANGE: {
            in.a = u16();
            in.b = u8();
            uint32_t rel = u32();
            in.target = (int64_t)pc + rel;
            break;
        }
        case OP_INPLACE:
            // numbers: the plain operator (anything else, like an object's `+=`, runs in the interpreter)
            in.op = (uint8_t)u8();
            if (in.op != OP_ADD && in.op != OP_SUB && in.op != OP_MUL && in.op != OP_MOD && in.op != OP_IDIV &&
                in.op != OP_DIV) {
                in.op = OP_INPLACE;
                stepOp(in, 2, 1);
            }
            break;
        case OP_GET_MEMBER: case OP_GET_MEMBER_RAW:
            in.sym = u16();
            in.fk = (uint8_t)u8();
            in.fi = u16();
            in.b = u16();  // inline cache: a field read runs natively
            break;
        case OP_INVOKE:
            in.sym = u16();
            in.a = u8();  // argc
            in.fk = (uint8_t)u8();
            in.fi = u16();
            in.b = u16();  // inline cache: the method
            break;
        case OP_SET_MEMBER:
            u16();
            in.b = u16();
            break;
        default:
            // not compiled: the machine code hands over to the interpreter here
            pc = in.off + instructionLength(fn, in.off);
            in.exitOnly = true;
            break;
    }
    in.next = pc;
    return true;
}

// Stack depth after `in` on the fall-through path (and at its jump target).
bool effect(const Ins& in, int d, int& fall, int& jump) {
    fall = jump = -1;
    if (in.exitOnly) return true;  // nothing after it runs natively
    if (in.step) {
        fall = d - in.pops + in.pushes;
        return true;
    }
    switch (in.op) {
        case OP_CONST: case OP_NIL: case OP_TRUE: case OP_FALSE: case OP_UNDEF: case OP_GET_LOCAL:
        case OP_GET_LOCAL_CHK: case OP_DUP: case OP_GET_GLOBAL:
            fall = d + 1;
            break;
        case OP_POP: case OP_SET_LOCAL: case OP_SET_GLOBAL: fall = d - 1; break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD: case OP_IDIV: case OP_LT: case OP_LE: case OP_GT:
        case OP_GE: case OP_EQ: case OP_NE: case OP_DIV: case OP_GET_INDEX:
            fall = d - 1;
            break;
        case OP_GET_FIELD: case OP_GET_MEMBER: case OP_GET_MEMBER_RAW: fall = d; break;
        case OP_SET_FIELD: case OP_SET_MEMBER: fall = d - 2; break;
        case OP_SET_INDEX: fall = d - 3; break;
        case OP_INVOKE: fall = d - (int)in.a; break;
        case OP_NEG: case OP_NOT: case OP_AUTOCALL: case OP_INC_LOCAL: case OP_ITER_INIT: case OP_CHECK_TYPE:
            fall = d;
            break;
        case OP_JUMP_IF_FALSE: fall = jump = d - 1; break;
        case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP:
            fall = d - 1;  // the value is popped going on, kept when jumping
            jump = d;
            break;
        case OP_JUMP_IF_SET: case OP_JUMP_IF_NOT_ERROR: case OP_JUMP_IF_NOT_SEQ: fall = jump = d; break;
        case OP_RANGE_INIT: fall = d; break;
        case OP_FOR_RANGE:
            fall = d + 1;
            jump = d;
            break;
        case OP_JUMP_IF_NOT_CMP: fall = jump = d - 2; break;
        case OP_JUMP: case OP_LOOP: jump = d; break;
        case OP_RETURN: break;
        case OP_CALL: fall = d - (int)in.a; break;
        case OP_ENDSCOPE: case OP_ENDSCOPE_KEEP: fall = d - (int)in.a; break;
        case OP_RANGE: fall = d - ((in.a & 2) ? 2 : 1); break;
        case OP_FOR_NEXT: case OP_FOR_SEQ:
            fall = d + 1;
            jump = d;
            break;
        case OP_SEQ_INIT: fall = d + 1; break;
        default: return false;
    }
    return true;
}

struct Analysis {
    std::vector<Ins> ins;
    std::unordered_map<size_t, size_t> at;  // bytecode offset -> instruction index
    int maxDepth = 0;
    std::vector<uint32_t> loopHeaders;
};

// Decodes the function and computes the stack depth at every instruction.
bool analyze(ObjFunction* fn, Analysis& an, std::string* why, const std::vector<std::pair<uint32_t, int>>& seeds = {}) {
    if (fn->upvalueCount > 0 && fn->methodStyle) {
        *why = "is a method that uses variables from an enclosing function";
        return false;
    }
    if (fn->isFork || fn->isPar || fn->lenient) {
        *why = "is a task or a placeholder lambda";
        return false;
    }
    const auto& code = fn->chunk.code;
    size_t pc = 0;
    while (pc < code.size()) {
        Ins in;
        if (!readIns(fn, code, pc, in, why)) return false;
        an.at[in.off] = an.ins.size();
        an.ins.push_back(in);
    }
    for (auto& in : an.ins) {
        if (in.op == OP_CHECK_TYPE) {
            Value t = fn->chunk.consts[in.a];
            if (!isObjType(t, OT::TypeDesc)) return false;
            auto k = as<ObjTypeDesc>(t)->k;
            if (k != ObjTypeDesc::Int && k != ObjTypeDesc::Num && k != ObjTypeDesc::Bool && k != ObjTypeDesc::Any)
                stepOp(in, 1, 1);  // other types: the interpreter checks
        }
        if (in.op == OP_ITER_INIT && in.a != 0) in.exitOnly = true;  // key/value loops: the interpreter
        if (in.target >= 0 && !an.at.count((size_t)in.target)) {
            *why = "has an unusual jump";
            return false;
        }
    }
    // RANGE is always followed by ITER_INIT (a `for` over a range)
    for (size_t i = 0; i < an.ins.size(); i++) {
        if (an.ins[i].op == OP_RANGE && (i + 1 >= an.ins.size() || an.ins[i + 1].op != OP_ITER_INIT))
            stepOp(an.ins[i], (an.ins[i].a & 2) ? 3 : 2, 1);  // a range value (not a loop): the interpreter makes it

    }
    // depths
    std::vector<size_t> work{0};
    an.ins[0].depth = 1 + fn->arity;
    for (auto& [at, depth] : seeds) {  // hot loops seen by the interpreter, with their real stack depth
        auto it = an.at.find(at);
        if (it == an.at.end() || depth < 1) continue;
        if (an.ins[it->second].depth < 0) {
            an.ins[it->second].depth = depth;
            work.push_back(it->second);
        } else if (an.ins[it->second].depth != depth) {
            *why = "has an unusual stack shape";
            return false;
        }
    }
    while (!work.empty()) {
        size_t i = work.back();
        work.pop_back();
        Ins& in = an.ins[i];
        int fall, jump;
        if (!effect(in, in.depth, fall, jump)) return false;
        in.fall = fall;
        auto flow = [&](size_t j, int d) {
            if (d < 1) return false;
            if (an.ins[j].depth < 0) {
                an.ins[j].depth = d;
                work.push_back(j);
                return true;
            }
            return an.ins[j].depth == d;
        };
        if (fall >= 0 && i + 1 < an.ins.size() && !flow(i + 1, fall)) {
            *why = "has an unusual stack shape";
            return false;
        }
        if (jump >= 0 && in.target >= 0 && !flow(an.at[(size_t)in.target], jump)) {
            *why = "has an unusual stack shape";
            return false;
        }
        an.maxDepth = std::max({an.maxDepth, in.depth + 2, fall + 1, jump + 1});
    }
    for (auto& in : an.ins)
        if (in.op == OP_LOOP && in.target >= 0) {
            uint32_t t = (uint32_t)in.target;
            if (std::find(an.loopHeaders.begin(), an.loopHeaders.end(), t) == an.loopHeaders.end() &&
                an.ins[an.at[t]].depth >= 0)
                an.loopHeaders.push_back(t);
        }
    return true;
}


// ------------------------------------------------------------------ types (the typed tier)
//
// Which slots hold an int, a float or a bool at each instruction, so the typed
// tier can keep them unboxed in registers. Types come from the code itself
// (constants, arithmetic, comparisons, counted loops) and from the interpreter's
// type profile (fields, items, calls, module variables, parameters). A type from
// the profile is a guess: the code checks it where the value appears and hands
// over to the interpreter when it's wrong.

// TY_DEF: some value (not an unset variable), of a kind not known; TY_ANY might be unset too.
constexpr uint8_t TY_ANY = 0, TY_INT = 1, TY_FLOAT = 2, TY_BOOL = 3, TY_NIL = 4, TY_UNDEF = 5, TY_DEF = 6, TY_NONE = 255;

bool tyNum(uint8_t t) { return t == TY_INT || t == TY_FLOAT; }
bool tyUnboxed(uint8_t t) { return t == TY_INT || t == TY_FLOAT || t == TY_BOOL; }

uint8_t tyMerge(uint8_t a, uint8_t b) {
    if (a == TY_NONE) return b;
    if (b == TY_NONE) return a;
    if (a == b) return a;
    bool defA = a != TY_ANY && a != TY_UNDEF, defB = b != TY_ANY && b != TY_UNDEF;
    return defA && defB ? TY_DEF : TY_ANY;
}

uint8_t tyOfConst(const Value& v) {
    switch (v.t) {
        case VT::Int: return TY_INT;
        case VT::Float: return TY_FLOAT;
        case VT::Bool: return TY_BOOL;
        case VT::Nil: return TY_NIL;
        default: return TY_DEF;
    }
}

uint8_t tyOfProfile(uint8_t bits) {
    bits &= 31;
    return bits == PB_INT ? TY_INT : bits == PB_FLOAT ? TY_FLOAT : bits == PB_BOOL ? TY_BOOL : TY_ANY;
}

// Dividing by a constant d >= 2 with a multiplication (Hacker's Delight 10-1):
// q = mulhigh(m, x) (+ x when m < 0), shifted right by s, plus 1 when x < 0,
// is x / d rounded toward zero.
struct DivMagic {
    int64_t m;
    int s;
};
DivMagic divMagic(int64_t d) {
    const uint64_t two63 = 1ull << 63;
    uint64_t ad = (uint64_t)d;
    uint64_t t = two63 + ((uint64_t)d >> 63);
    uint64_t anc = t - 1 - t % ad;
    int p = 63;
    uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
    uint64_t q2 = two63 / ad, r2 = two63 - q2 * ad;
    uint64_t delta;
    do {
        p++;
        q1 = 2 * q1;
        r1 = 2 * r1;
        if (r1 >= anc) {
            q1++;
            r1 -= anc;
        }
        q2 = 2 * q2;
        r2 = 2 * r2;
        if (r2 >= ad) {
            q2++;
            r2 -= ad;
        }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    return {(int64_t)(q2 + 1), p - 64};
}

struct TypeInfo {
    int slots = 0;
    std::vector<std::vector<uint8_t>> at;  // per instruction: each slot's type on entry (TY_NONE: unreachable)
    std::vector<int> guess;                // per instruction: the slot whose type its result guesses, or -1
    std::vector<uint8_t> params;           // guessed parameter types (checked on entry)
    std::vector<bool> captured;            // locals that closures share: never typed
    std::vector<bool> blockStart;          // reached from more than straight-line code
    bool useful = false;                   // anything typed at all
};

// The compiler thread compiles from copies: these point at them (null: the function's own)
thread_local const std::vector<uint8_t>* tlsProfile = nullptr;
thread_local const std::vector<InlineCache>* tlsIcs = nullptr;
thread_local const std::vector<std::pair<uint32_t, int>>* tlsSeeds = nullptr;
thread_local int tlsNoTier2 = -1;
const std::vector<uint8_t>& profileOf(const ObjFunction* fn) { return tlsProfile ? *tlsProfile : fn->typeSeen; }
const InlineCache& icOf(const ObjFunction* fn, size_t i) { return tlsIcs ? (*tlsIcs)[i] : fn->ic[i]; }
const std::vector<std::pair<uint32_t, int>>& seedsOf(const ObjFunction* fn) { return tlsSeeds ? *tlsSeeds : fn->jitSeeds; }

// The slots of locals that closures made in this function capture.
void capturedLocals(ObjFunction* fn, const Analysis& an, std::vector<bool>& out) {
    const auto& code = fn->chunk.code;
    for (auto& in : an.ins) {
        if (in.op != OP_CLOSURE) continue;
        size_t pc = in.off + 1;
        if (pc + 2 > code.size()) continue;
        uint16_t k = (uint16_t)(code[pc] | (code[pc + 1] << 8));
        pc += 2;
        if (k >= fn->chunk.consts.size() || !isObjType(fn->chunk.consts[k], OT::Function)) continue;
        auto* inner = as<ObjFunction>(fn->chunk.consts[k]);
        for (int u = 0; u < inner->upvalueCount && pc + 3 <= code.size(); u++, pc += 3) {
            uint16_t idx = (uint16_t)(code[pc + 1] | (code[pc + 2] << 8));
            if (code[pc] && idx < out.size()) out[idx] = true;
        }
    }
}

void inferTypes(ObjFunction* fn, const Analysis& an, TypeInfo& ti, const std::vector<std::pair<uint32_t, int>>& seeds) {
    size_t n = an.ins.size();
    ti.slots = an.maxDepth + 2;
    ti.at.assign(n, std::vector<uint8_t>((size_t)ti.slots, TY_NONE));
    ti.guess.assign(n, -1);
    ti.captured.assign((size_t)ti.slots, false);
    ti.blockStart.assign(n, false);
    capturedLocals(fn, an, ti.captured);
    const auto& prof = profileOf(fn);
    size_t codeSize = fn->chunk.code.size();
    auto seen = [&](size_t off) -> uint8_t { return off < prof.size() ? prof[off] : 0; };
    // where control flow joins: those instructions start with every value in its slot
    for (size_t i = 0; i < n; i++) {
        const Ins& in = an.ins[i];
        if (in.target >= 0) {
            auto it = an.at.find((size_t)in.target);
            if (it != an.at.end()) ti.blockStart[it->second] = true;
        }
    }
    for (uint32_t h : an.loopHeaders) ti.blockStart[an.at.at(h)] = true;
    for (auto& [h, d] : seeds) {
        auto it = an.at.find(h);
        if (it != an.at.end()) ti.blockStart[it->second] = true;
    }
    ti.blockStart[0] = true;
    // the guessed parameter types
    ti.params.assign((size_t)fn->arity + 1, TY_ANY);
    for (int k = 1; k <= fn->arity; k++)
        if (!ti.captured[(size_t)k]) ti.params[(size_t)k] = tyOfProfile(seen(codeSize + (size_t)k - 1));
    std::vector<uint8_t> entry((size_t)ti.slots, TY_ANY);
    for (int k = 1; k <= fn->arity; k++) entry[(size_t)k] = ti.params[(size_t)k];
    std::vector<size_t> work;
    auto flow = [&](size_t j, const std::vector<uint8_t>& st, int depth) {
        auto& cur = ti.at[j];
        bool changed = false;
        for (int k = 0; k < depth && k < ti.slots; k++) {
            uint8_t m = tyMerge(cur[(size_t)k], st[(size_t)k]);
            if (ti.captured[(size_t)k]) m = TY_ANY;
            if (m != cur[(size_t)k]) {
                cur[(size_t)k] = m;
                changed = true;
            }
        }
        if (changed) work.push_back(j);
    };
    flow(0, entry, an.ins[0].depth);
    auto run = [&]() {
        while (!work.empty()) {
            size_t i = work.back();
            work.pop_back();
            const Ins& in = an.ins[i];
            int d = in.depth;
            if (d < 0 || in.exitOnly) continue;
            std::vector<uint8_t> st = ti.at[i];
            for (int k = d; k < ti.slots; k++) st[(size_t)k] = TY_NONE;
            auto num2 = [&](uint8_t x, uint8_t y) { return tyNum(x) && tyNum(y); };
            // a guessed result: only where the next instruction has no other way in
            auto guessAt = [&](int slot) {
                uint8_t t = TY_ANY;
                if (i + 1 < n && !ti.blockStart[i + 1]) t = tyOfProfile(seen(in.next));
                if (t != TY_ANY) ti.guess[i] = slot;
                return t == TY_ANY ? TY_DEF : t;
            };
            uint8_t x = d >= 2 ? st[(size_t)d - 2] : TY_ANY, y = d >= 1 ? st[(size_t)d - 1] : TY_ANY;
            int fall = in.fall, jump = -1;
            {
                int f2, j2;
                effect(in, d, f2, j2);
                jump = j2;
            }
            if (in.step) {
                for (int k = d - in.pops; k < d - in.pops + in.pushes; k++)
                    if (k >= 0) st[(size_t)k] = TY_DEF;
                if (in.op == OP_GET_UPVAL) st[(size_t)d] = guessAt(d);
                if (in.op == OP_STR_APPEND && in.a < (uint32_t)ti.slots) {
                    // numbers just add (the typed tier does it); strings grow
                    uint8_t t = st[in.a];
                    st[in.a] = t == TY_INT && y == TY_INT ? TY_INT : tyNum(t) && tyNum(y) ? TY_FLOAT : TY_DEF;
                }
            } else {
                switch (in.op) {
                    case OP_CONST: st[(size_t)d] = tyOfConst(fn->chunk.consts[in.a]); break;
                    case OP_NIL: st[(size_t)d] = TY_NIL; break;
                    case OP_TRUE: case OP_FALSE: st[(size_t)d] = TY_BOOL; break;
                    case OP_UNDEF: st[(size_t)d] = TY_UNDEF; break;
                    case OP_DUP: st[(size_t)d] = y; break;
                    case OP_GET_LOCAL: st[(size_t)d] = st[in.a]; break;
                    case OP_GET_LOCAL_CHK: st[(size_t)d] = st[in.a] == TY_UNDEF ? TY_ANY : st[in.a]; break;
                    case OP_SET_LOCAL: st[in.a] = y; break;
                    case OP_GET_GLOBAL: st[(size_t)d] = guessAt(d); break;
                    case OP_ADD: case OP_SUB: case OP_MUL:
                        st[(size_t)d - 2] = x == TY_INT && y == TY_INT ? TY_INT : num2(x, y) ? TY_FLOAT : TY_DEF;
                        break;
                    case OP_DIV: st[(size_t)d - 2] = num2(x, y) ? TY_FLOAT : TY_DEF; break;
                    case OP_MOD: case OP_IDIV: st[(size_t)d - 2] = x == TY_INT && y == TY_INT ? TY_INT : TY_DEF; break;
                    case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE:
                        st[(size_t)d - 2] = num2(x, y) ? TY_BOOL : TY_DEF;
                        break;
                    case OP_NEG: st[(size_t)d - 1] = tyNum(y) ? y : TY_DEF; break;
                    case OP_NOT: st[(size_t)d - 1] = y == TY_BOOL ? TY_BOOL : TY_DEF; break;
                    case OP_GET_FIELD: case OP_GET_MEMBER: case OP_GET_MEMBER_RAW: {
                        bool sqrtOf = in.op == OP_GET_MEMBER && tyNum(y) && VM::symName((int)in.sym) == "sqrt";
                        st[(size_t)d - 1] = sqrtOf ? TY_FLOAT : guessAt(d - 1);
                        if (sqrtOf) ti.guess[i] = -1;
                        // never run yet: x.len is an int for every built-in value (checked like a guess)
                        if (!sqrtOf && in.op == OP_GET_MEMBER && seen(in.next) == 0 && i + 1 < n && !ti.blockStart[i + 1] &&
                            VM::symName((int)in.sym) == "len") {
                            st[(size_t)d - 1] = TY_INT;
                            ti.guess[i] = d - 1;
                        }
                        break;
                    }
                    case OP_GET_INDEX: st[(size_t)d - 2] = guessAt(d - 2); break;
                    case OP_CALL: case OP_INVOKE: st[(size_t)d - (int)in.a - 1] = guessAt(d - (int)in.a - 1); break;
                    case OP_AUTOCALL:
                        if (!tyUnboxed(y) && y != TY_NIL) st[(size_t)d - 1] = guessAt(d - 1);
                        break;
                    case OP_INC_LOCAL:
                        if (st[in.a] != TY_INT) st[in.a] = TY_ANY;
                        break;
                    case OP_ITER_INIT: st[(size_t)d - 1] = TY_DEF; break;
                    case OP_CHECK_TYPE: {
                        auto k = as<ObjTypeDesc>(fn->chunk.consts[in.a])->k;
                        if (k == ObjTypeDesc::Int) st[(size_t)d - 1] = TY_INT;
                        else if (k == ObjTypeDesc::Bool) st[(size_t)d - 1] = TY_BOOL;
                        else if (k != ObjTypeDesc::Any && !tyNum(y)) st[(size_t)d - 1] = TY_DEF;
                        break;
                    }
                    case OP_RANGE_INIT:
                        for (int k = d - 3; k < d; k++) st[(size_t)k] = TY_INT;
                        break;
                    case OP_SEQ_INIT:  // the typed tier always walks a list by position (else it stops)
                        st[(size_t)d - 1] = TY_DEF;
                        st[(size_t)d] = TY_INT;
                        break;
                    case OP_ENDSCOPE_KEEP: st[(size_t)d - 1 - in.a] = y; break;
                    case OP_RANGE: st[(size_t)(d - ((in.a & 2) ? 3 : 2))] = TY_DEF; break;
                    default: break;
                }
            }
            // on to the next instruction(s)
            if (in.op == OP_FOR_RANGE) {
                std::vector<uint8_t> f2 = st;
                f2[(size_t)d] = TY_INT;
                if (i + 1 < n && fall >= 0) flow(i + 1, f2, fall);
                if (jump >= 0) flow(an.at.at((size_t)in.target), st, jump);
                continue;
            }
            if (in.op == OP_FOR_SEQ || in.op == OP_FOR_NEXT) {
                std::vector<uint8_t> f2 = st;
                f2[(size_t)d] = in.op == OP_FOR_SEQ ? guessAt(d) : TY_DEF;
                f2[in.a + 1] = in.op == OP_FOR_SEQ ? TY_INT : TY_ANY;
                std::vector<uint8_t> j2 = st;
                j2[in.a + 1] = in.op == OP_FOR_SEQ ? TY_INT : TY_ANY;
                if (i + 1 < n && fall >= 0) flow(i + 1, f2, fall);
                if (jump >= 0) flow(an.at.at((size_t)in.target), j2, jump);
                continue;
            }
            if (fall >= 0 && i + 1 < n) flow(i + 1, st, fall);
            if (jump >= 0 && in.target >= 0) flow(an.at.at((size_t)in.target), st, jump);
        }
    };
    run();
    // loops entered from the interpreter that the code above doesn't reach: nothing known
    for (auto& [h, depth] : seeds) {
        auto it = an.at.find(h);
        if (it == an.at.end()) continue;
        auto& cur = ti.at[it->second];
        bool unreached = true;
        for (int k = 0; k < depth && k < ti.slots; k++)
            if (cur[(size_t)k] != TY_NONE) unreached = false;
        if (unreached) {
            std::vector<uint8_t> any((size_t)ti.slots, TY_ANY);
            flow(it->second, any, depth);
            run();
        }
    }
    for (size_t i = 0; i < n && !ti.useful; i++)
        for (int k = 0; k < an.ins[i].depth && k < ti.slots; k++)
            if (tyUnboxed(ti.at[i][(size_t)k])) ti.useful = true;
}

#ifdef IXO_JIT_ANY

#ifdef IXO_JIT_X64

// ------------------------------------------------------------------ x86-64 assembler

enum Reg { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
#ifdef _WIN32
constexpr int ARG0 = RCX, ARG1 = RDX, ARG2 = R8;
#else
constexpr int ARG0 = RDI, ARG1 = RSI, ARG2 = RDX;
#endif
enum CC { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_S = 8, CC_NS = 9, CC_L = 0xC,
          CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

struct Asm {
    std::vector<uint8_t> b;
    struct Fix {
        size_t at;
        int label;
    };
    std::vector<Fix> fixes;
    std::vector<int64_t> labels;

    void u8(uint8_t x) { b.push_back(x); }
    void u32(uint32_t x) {
        for (int i = 0; i < 4; i++) u8((uint8_t)(x >> (8 * i)));
    }
    void u64(uint64_t x) {
        for (int i = 0; i < 8; i++) u8((uint8_t)(x >> (8 * i)));
    }
    void rex(bool w, int reg, int base, bool force = false) {
        uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((base & 8) ? 1 : 0));
        if (r != 0x40 || force) u8(r);
    }
    // [base + disp32]
    void mem(int reg, int base, int32_t disp) {
        u8((uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
        if ((base & 7) == 4) u8(0x24);
        u32((uint32_t)disp);
    }
    void rr(uint8_t op, int dst, int src) {  // op r/m64(dst), r64(src)
        rex(true, src, dst);
        u8(op);
        u8((uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    void load(int reg, int base, int32_t disp) {
        rex(true, reg, base);
        u8(0x8B);
        mem(reg, base, disp);
    }
    void store(int base, int32_t disp, int reg) {
        rex(true, reg, base);
        u8(0x89);
        mem(reg, base, disp);
    }
    void movImm(int reg, uint64_t v) {
        rex(true, 0, reg);
        u8((uint8_t)(0xB8 + (reg & 7)));
        u64(v);
    }
    void movR(int dst, int src) { rr(0x89, dst, src); }
    void storeByte(int base, int32_t disp, uint8_t v) {
        rex(false, 0, base);
        u8(0xC6);
        mem(0, base, disp);
        u8(v);
    }
    void cmpByte(int base, int32_t disp, uint8_t v) {
        rex(false, 0, base);
        u8(0x80);
        mem(7, base, disp);
        u8(v);
    }
    void xorByte(int base, int32_t disp, uint8_t v) {
        rex(false, 0, base);
        u8(0x80);
        mem(6, base, disp);
        u8(v);
    }
    void aluLoad(uint8_t op, int reg, int base, int32_t disp) {  // reg op= [base+disp]; op: 03 add, 2B sub, 3B cmp
        rex(true, reg, base);
        u8(op);
        mem(reg, base, disp);
    }
    void imulLoad(int reg, int base, int32_t disp) {
        rex(true, reg, base);
        u8(0x0F);
        u8(0xAF);
        mem(reg, base, disp);
    }
    void addMemImm8(int base, int32_t disp, int8_t v) {
        rex(true, 0, base);
        u8(0x83);
        mem(0, base, disp);
        u8((uint8_t)v);
    }
    void incMem(int base, int32_t disp) {
        rex(true, 0, base);
        u8(0xFF);
        mem(0, base, disp);
    }
    void decMem(int base, int32_t disp) {
        rex(true, 0, base);
        u8(0xFF);
        mem(1, base, disp);
    }
    void negMem(int base, int32_t disp) {
        rex(true, 0, base);
        u8(0xF7);
        mem(3, base, disp);
    }
    void movzxByte(int reg, int base, int32_t disp) {  // reg32 = zero-extended byte
        rex(false, reg, base);
        u8(0x0F);
        u8(0xB6);
        mem(reg, base, disp);
    }
    void lea(int reg, int base, int32_t disp) {
        rex(true, reg, base);
        u8(0x8D);
        mem(reg, base, disp);
    }
    void cmpImm32(int reg, int32_t v) {
        rex(true, 0, reg);
        u8(0x81);
        u8((uint8_t)(0xC0 | (7 << 3) | (reg & 7)));
        u32((uint32_t)v);
    }
    void cmpEaxImm8(int8_t v) {
        u8(0x83);
        u8(0xF8);
        u8((uint8_t)v);
    }
    void testR(int a, int b2) { rr(0x85, a, b2); }
    void subR(int a, int b2) { rr(0x29, a, b2); }
    void cmpR(int a, int b2) { rr(0x39, a, b2); }  // flags of a - b
    void shlImm(int reg, uint8_t n) {
        rex(true, 0, reg);
        u8(0xC1);
        u8((uint8_t)(0xE0 | (reg & 7)));
        u8(n);
    }
    void shrImm(int reg, uint8_t n) {
        rex(true, 0, reg);
        u8(0xC1);
        u8((uint8_t)(0xE8 | (reg & 7)));
        u8(n);
    }
    // SSE2 (xmm0 / xmm1 only): F2/66 prefix, REX.B for r8-r15 bases, 0F op
    void sse(uint8_t prefix, bool w, uint8_t op, int xmm, int base, int32_t disp) {
        u8(prefix);
        rex(w, xmm, base);
        u8(0x0F);
        u8(op);
        mem(xmm, base, disp);
    }
    void movsdLoad(int xmm, int base, int32_t disp) { sse(0xF2, false, 0x10, xmm, base, disp); }
    void movsdStore(int base, int32_t disp, int xmm) { sse(0xF2, false, 0x11, xmm, base, disp); }
    void cvtsi2sdLoad(int xmm, int base, int32_t disp) { sse(0xF2, true, 0x2A, xmm, base, disp); }
    void sdRR(uint8_t op, int dst, int src) {  // addsd 58, mulsd 59, subsd 5C, divsd 5E, sqrtsd 51, movsd 10
        u8(0xF2);
        if ((dst | src) & 8) u8((uint8_t)(0x40 | ((dst & 8) ? 4 : 0) | ((src & 8) ? 1 : 0)));
        u8(0x0F);
        u8(op);
        u8((uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }
    void ucomisdRR(int a2, int b2) {  // flags from a2 vs b2 (unordered: ZF PF CF all set)
        u8(0x66);
        if ((a2 | b2) & 8) u8((uint8_t)(0x40 | ((a2 & 8) ? 4 : 0) | ((b2 & 8) ? 1 : 0)));
        u8(0x0F);
        u8(0x2E);
        u8((uint8_t)(0xC0 | ((a2 & 7) << 3) | (b2 & 7)));
    }
    void loadI32(int reg, int base, int32_t disp) {  // movsxd reg, dword [base+disp]
        rex(true, reg, base);
        u8(0x63);
        mem(reg, base, disp);
    }
    void xorR(int a, int b2) { rr(0x31, a, b2); }
    void addR(int a, int b2) { rr(0x01, a, b2); }
    void subImm8(int reg, int8_t v) {
        rex(true, 0, reg);
        u8(0x83);
        u8((uint8_t)(0xC0 | (5 << 3) | (reg & 7)));
        u8((uint8_t)v);
    }
    void negR(int reg) {
        rex(true, 0, reg);
        u8(0xF7);
        u8((uint8_t)(0xC0 | (3 << 3) | (reg & 7)));
    }
    void cqo() {
        u8(0x48);
        u8(0x99);
    }
    void idivR(int reg) {
        rex(true, 0, reg);
        u8(0xF7);
        u8((uint8_t)(0xC0 | (7 << 3) | (reg & 7)));
    }
    void setccAl(int cc) {
        u8(0x0F);
        u8((uint8_t)(0x90 | cc));
        u8(0xC0);
    }
    void movzxEaxAl() {
        u8(0x0F);
        u8(0xB6);
        u8(0xC0);
    }
    void xorEaxEax() {
        u8(0x31);
        u8(0xC0);
    }
    void movEaxImm(uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void testEaxEax() {
        u8(0x85);
        u8(0xC0);
    }
    void push(int r) {
        if (r & 8) u8(0x41);
        u8((uint8_t)(0x50 + (r & 7)));
    }
    void pop(int r) {
        if (r & 8) u8(0x41);
        u8((uint8_t)(0x58 + (r & 7)));
    }
    void subRsp(int32_t v) {
        u8(0x48);
        u8(0x81);
        u8(0xEC);
        u32((uint32_t)v);
    }
    void addRsp(int32_t v) {
        u8(0x48);
        u8(0x81);
        u8(0xC4);
        u32((uint32_t)v);
    }
    void callR(int reg) {
        if (reg & 8) u8(0x41);
        u8(0xFF);
        u8((uint8_t)(0xD0 | (reg & 7)));
    }
    void ret() { u8(0xC3); }
    void enter() {}  // (AArch64: saves the link register)
    void leave() {}
    void store32(int base, int32_t disp, int reg) {  // mov dword [base+disp], reg32
        rex(false, reg, base);
        u8(0x89);
        mem(reg, base, disp);
    }
    void storeDword(int base, int32_t disp, uint32_t v) {  // mov dword [base+disp], imm32
        rex(false, 0, base);
        u8(0xC7);
        mem(0, base, disp);
        u32(v);
    }

    int newLabel() {
        labels.push_back(-1);
        return (int)labels.size() - 1;
    }
    void bind(int l) { labels[(size_t)l] = (int64_t)b.size(); }
    void jcc(int cc, int l) {
        u8(0x0F);
        u8((uint8_t)(0x80 | cc));
        fixes.push_back({b.size(), l});
        u32(0);
    }
    void jmp(int l) {
        u8(0xE9);
        fixes.push_back({b.size(), l});
        u32(0);
    }
    void call(int l) {
        u8(0xE8);
        fixes.push_back({b.size(), l});
        u32(0);
    }
    // --- register forms for the typed tier (values live in registers)
    void imulRR(int dst, int src) {  // dst *= src
        rex(true, dst, src);
        u8(0x0F);
        u8(0xAF);
        u8((uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }
    void cvtsi2sdRR(int xmm, int gpr) {  // xmm = (double)gpr
        u8(0xF2);
        rex(true, xmm, gpr);
        u8(0x0F);
        u8(0x2A);
        u8((uint8_t)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
    }
    void movqXG(int xmm, int gpr) {  // xmm = the bits of gpr
        u8(0x66);
        rex(true, xmm, gpr);
        u8(0x0F);
        u8(0x6E);
        u8((uint8_t)(0xC0 | ((xmm & 7) << 3) | (gpr & 7)));
    }
    void movsdRR(int dst, int src) { sdRR(0x10, dst, src); }
    void mulHigh(int reg) {  // rdx = the high 64 bits of rax * reg (signed)
        rex(true, 0, reg);
        u8(0xF7);
        u8((uint8_t)(0xE8 | (reg & 7)));
    }
    void sarImm(int reg, uint8_t n) {
        rex(true, 0, reg);
        u8(0xC1);
        u8((uint8_t)(0xF8 | (reg & 7)));
        u8(n);
    }
    void andR(int a2, int b2) { rr(0x21, a2, b2); }
    // xmm6 and xmm7 belong to the caller on Windows: the typed tier saves them (all 128 bits)
    void movupsStore(int base, int32_t disp, int xmm) {
        rex(false, xmm, base);
        u8(0x0F);
        u8(0x11);
        mem(xmm, base, disp);
    }
    void movupsLoad(int xmm, int base, int32_t disp) {
        rex(false, xmm, base);
        u8(0x0F);
        u8(0x10);
        mem(xmm, base, disp);
    }

    bool finish() {
        for (auto& f : fixes) {
            int64_t t = labels[(size_t)f.label];
            if (t < 0) return false;
            int32_t rel = (int32_t)(t - (int64_t)(f.at + 4));
            std::memcpy(&b[f.at], &rel, 4);
        }
        return true;
    }
};

#else  // IXO_JIT_ARM64

// ------------------------------------------------------------------ AArch64 assembler
//
// The same operations as the x86-64 assembler, so the code generator is shared:
// the x86 register names map to AArch64 registers, memory operands become a
// load or store through x16/x17, and the condition codes keep their x86 meaning
// (after fcmp, the float meaning: "above" is GT, so NaN compares false).

enum Reg { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, ARG0, ARG1, ARG2 };
enum CC { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_S = 8, CC_NS = 9, CC_L = 0xC,
          CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

struct Asm {
    std::vector<uint8_t> b;
    struct Fix {
        size_t at;
        int label;
        int kind;  // 0 b/bl (26 bits), 1 b.cond (19 bits)
    };
    std::vector<Fix> fixes;
    std::vector<int64_t> labels;
    bool floatFlags = false;  // the flags came from fcmp

    static constexpr int SP = 31, XZR = 31, T0 = 16, T1 = 17;
    static int X(int r) {
        // (RSI and RDI: registers of their own, kept for the caller; only the typed tier uses them)
        static const int map[] = {9, 10, 11, 19, 31, 29, 24, 25, 12, 13, 14, 15, 20, 21, 22, 23, 0, 1, 2};
        return map[r];
    }
    void ins(uint32_t w) {
        for (int i = 0; i < 4; i++) b.push_back((uint8_t)(w >> (8 * i)));
    }
    void movz(int d, uint64_t v) {  // a 64-bit constant into register d (raw number)
        ins(0xD2800000u | ((uint32_t)(v & 0xFFFF) << 5) | (uint32_t)d);
        for (int hw = 1; hw < 4; hw++) {
            uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
            if (part) ins(0xF2800000u | ((uint32_t)hw << 21) | (part << 5) | (uint32_t)d);
        }
    }
    // load / store `size` bytes at [base + disp] (size 1, 4 or 8; `sx`: sign-extending 32-bit load; `fp`: a d register)
    void mem(bool load, int size, int t, int base, int32_t disp, bool sx = false, bool fp = false) {
        uint32_t imm, reg;
        if (fp) {
            imm = load ? 0xFD400000u : 0xFD000000u;
            reg = load ? 0xFC606800u : 0xFC206800u;
        } else if (size == 8) {
            imm = load ? 0xF9400000u : 0xF9000000u;
            reg = load ? 0xF8606800u : 0xF8206800u;
        } else if (size == 4) {
            imm = load ? (sx ? 0xB9800000u : 0xB9400000u) : 0xB9000000u;
            reg = load ? (sx ? 0xB8A06800u : 0xB8606800u) : 0xB8206800u;
        } else {
            imm = load ? 0x39400000u : 0x39000000u;
            reg = load ? 0x38606800u : 0x38206800u;
        }
        if (disp >= 0 && disp % size == 0 && disp / size < 4096) {
            ins(imm | ((uint32_t)(disp / size) << 10) | ((uint32_t)base << 5) | (uint32_t)t);
        } else {
            movz(T1, (uint64_t)(int64_t)disp);
            ins(reg | ((uint32_t)T1 << 16) | ((uint32_t)base << 5) | (uint32_t)t);
        }
    }
    void addImm(int d, int n, int32_t v) {  // d = n + v (SP allowed)
        if (v >= 0 && v < 4096) {
            ins(0x91000000u | ((uint32_t)v << 10) | ((uint32_t)n << 5) | (uint32_t)d);
        } else if (v < 0 && -v < 4096) {
            ins(0xD1000000u | ((uint32_t)(-v) << 10) | ((uint32_t)n << 5) | (uint32_t)d);
        } else {
            movz(T1, (uint64_t)(int64_t)v);
            ins(0x8B000000u | ((uint32_t)T1 << 16) | ((uint32_t)n << 5) | (uint32_t)d);
        }
    }
    void cmpImm(int n, int64_t v, bool w32 = false) {
        floatFlags = false;
        uint32_t sf = w32 ? 0 : 0x80000000u;
        if (v >= 0 && v < 4096)
            ins(0x7100001Fu | sf | ((uint32_t)v << 10) | ((uint32_t)n << 5));
        else if (v < 0 && -v < 4096)
            ins(0x3100001Fu | sf | ((uint32_t)(-v) << 10) | ((uint32_t)n << 5));  // cmn
        else {
            movz(T1, (uint64_t)v);
            ins(0x6B00001Fu | sf | ((uint32_t)T1 << 16) | ((uint32_t)n << 5));
        }
    }

    void load(int reg, int base, int32_t disp) { mem(true, 8, X(reg), X(base), disp); }
    void store(int base, int32_t disp, int reg) { mem(false, 8, X(reg), X(base), disp); }
    void movImm(int reg, uint64_t v) { movz(X(reg), v); }
    void movR(int dst, int src) {
        if (X(dst) == SP || X(src) == SP)
            ins(0x91000000u | ((uint32_t)X(src) << 5) | (uint32_t)X(dst));
        else
            ins(0xAA0003E0u | ((uint32_t)X(src) << 16) | (uint32_t)X(dst));
    }
    void storeByte(int base, int32_t disp, uint8_t v) {
        ins(0x52800000u | ((uint32_t)v << 5) | T0);
        mem(false, 1, T0, X(base), disp);
    }
    void cmpByte(int base, int32_t disp, uint8_t v) {
        mem(true, 1, T0, X(base), disp);
        cmpImm(T0, v, true);
    }
    void xorByte(int base, int32_t disp, uint8_t v) {
        mem(true, 1, T0, X(base), disp);
        ins(0x52800000u | ((uint32_t)v << 5) | T1);
        ins(0x4A000000u | ((uint32_t)T1 << 16) | ((uint32_t)T0 << 5) | T0);  // eor w16, w16, w17
        mem(false, 1, T0, X(base), disp);
    }
    void aluLoad(uint8_t op, int reg, int base, int32_t disp) {  // 03 add, 2B sub, 3B cmp
        mem(true, 8, T0, X(base), disp);
        int r = X(reg);
        if (op == 0x03) ins(0x8B000000u | ((uint32_t)T0 << 16) | ((uint32_t)r << 5) | (uint32_t)r);
        else if (op == 0x2B) ins(0xCB000000u | ((uint32_t)T0 << 16) | ((uint32_t)r << 5) | (uint32_t)r);
        else {
            floatFlags = false;
            ins(0xEB00001Fu | ((uint32_t)T0 << 16) | ((uint32_t)r << 5));
        }
    }
    void imulLoad(int reg, int base, int32_t disp) {
        mem(true, 8, T0, X(base), disp);
        int r = X(reg);
        ins(0x9B007C00u | ((uint32_t)T0 << 16) | ((uint32_t)r << 5) | (uint32_t)r);
    }
    void addMemImm8(int base, int32_t disp, int8_t v) {
        mem(true, 8, T0, X(base), disp);
        if (v >= 0) ins(0x91000000u | ((uint32_t)v << 10) | ((uint32_t)T0 << 5) | T0);
        else ins(0xD1000000u | ((uint32_t)(-v) << 10) | ((uint32_t)T0 << 5) | T0);
        mem(false, 8, T0, X(base), disp);
    }
    void incMem(int base, int32_t disp) { addMemImm8(base, disp, 1); }
    void decMem(int base, int32_t disp) {  // sets the flags like x86 dec (for "jump if <= 0")
        mem(true, 8, T0, X(base), disp);
        floatFlags = false;
        ins(0xF1000400u | ((uint32_t)T0 << 5) | T0);  // subs x16, x16, #1
        mem(false, 8, T0, X(base), disp);
    }
    void negMem(int base, int32_t disp) {
        mem(true, 8, T0, X(base), disp);
        ins(0xCB0003E0u | ((uint32_t)T0 << 16) | T0);
        mem(false, 8, T0, X(base), disp);
    }
    void movzxByte(int reg, int base, int32_t disp) { mem(true, 1, X(reg), X(base), disp); }
    void lea(int reg, int base, int32_t disp) { addImm(X(reg), X(base), disp); }
    void cmpImm32(int reg, int32_t v) { cmpImm(X(reg), v); }
    void cmpEaxImm8(int8_t v) { cmpImm(X(RAX), v); }
    void testR(int a2, int b2) {
        floatFlags = false;
        ins(0xEA00001Fu | ((uint32_t)X(b2) << 16) | ((uint32_t)X(a2) << 5));
    }
    void xorR(int a2, int b2) { ins(0xCA000000u | ((uint32_t)X(b2) << 16) | ((uint32_t)X(a2) << 5) | (uint32_t)X(a2)); }
    void addR(int a2, int b2) { ins(0x8B000000u | ((uint32_t)X(b2) << 16) | ((uint32_t)X(a2) << 5) | (uint32_t)X(a2)); }
    void subR(int a2, int b2) { ins(0xCB000000u | ((uint32_t)X(b2) << 16) | ((uint32_t)X(a2) << 5) | (uint32_t)X(a2)); }
    void cmpR(int a2, int b2) {
        floatFlags = false;
        ins(0xEB00001Fu | ((uint32_t)X(b2) << 16) | ((uint32_t)X(a2) << 5));
    }
    void shlImm(int reg, uint8_t n) {
        int r = X(reg);
        ins(0xD3400000u | ((uint32_t)((64 - n) & 63) << 16) | ((uint32_t)(63 - n) << 10) | ((uint32_t)r << 5) | (uint32_t)r);
    }
    void shrImm(int reg, uint8_t n) {
        int r = X(reg);
        ins(0xD340FC00u | ((uint32_t)n << 16) | ((uint32_t)r << 5) | (uint32_t)r);
    }
    void loadI32(int reg, int base, int32_t disp) { mem(true, 4, X(reg), X(base), disp, true); }
    void movsdLoad(int xmm, int base, int32_t disp) { mem(true, 8, xmm, X(base), disp, false, true); }
    void movsdStore(int base, int32_t disp, int xmm) { mem(false, 8, xmm, X(base), disp, false, true); }
    void cvtsi2sdLoad(int xmm, int base, int32_t disp) {
        mem(true, 8, T0, X(base), disp);
        ins(0x9E620000u | ((uint32_t)T0 << 5) | (uint32_t)xmm);  // scvtf d, x16
    }
    void sdRR(uint8_t op, int dst, int src) {
        if (op == 0x51) {  // fsqrt
            ins(0x1E61C000u | ((uint32_t)src << 5) | (uint32_t)dst);
            return;
        }
        uint32_t base = op == 0x58 ? 0x1E602800u : op == 0x5C ? 0x1E603800u : op == 0x59 ? 0x1E600800u : 0x1E601800u;
        ins(base | ((uint32_t)src << 16) | ((uint32_t)dst << 5) | (uint32_t)dst);
    }
    void ucomisdRR(int a2, int b2) {
        ins(0x1E602000u | ((uint32_t)b2 << 16) | ((uint32_t)a2 << 5));
        floatFlags = true;
    }
    void subImm8(int reg, int8_t v) { addImm(X(reg), X(reg), -v); }
    void negR(int reg) { ins(0xCB0003E0u | ((uint32_t)X(reg) << 16) | (uint32_t)X(reg)); }
    void cqo() {}
    void idivR(int reg) {  // rax = rax / reg (toward zero), rdx = the remainder (sign of rax)
        int a2 = X(RAX), d = X(RDX), r = X(reg);
        ins(0x9AC00C00u | ((uint32_t)r << 16) | ((uint32_t)a2 << 5) | T0);                      // sdiv x16, rax, r
        ins(0x9B008000u | ((uint32_t)r << 16) | ((uint32_t)a2 << 10) | ((uint32_t)T0 << 5) | (uint32_t)d);  // msub
        ins(0xAA0003E0u | ((uint32_t)T0 << 16) | (uint32_t)a2);
    }
    static uint32_t cond(int cc, bool fl) {
        if (fl) {
            switch (cc) {
                case CC_A: return 12;   // GT
                case CC_AE: return 10;  // GE
                case CC_B: return 11;   // LT
                case CC_BE: return 13;  // LE
                case CC_E: return 0;
                case CC_NE: return 1;
                default: break;
            }
        }
        switch (cc) {
            case CC_B: return 3;    // LO
            case CC_AE: return 2;   // HS
            case CC_E: return 0;
            case CC_NE: return 1;
            case CC_BE: return 9;   // LS
            case CC_A: return 8;    // HI
            case CC_S: return 4;    // MI
            case CC_NS: return 5;   // PL
            case CC_L: return 11;
            case CC_GE: return 10;
            case CC_LE: return 13;
            default: return 12;     // GT
        }
    }
    void setccAl(int cc) { ins(0x9A9F07E0u | ((cond(cc, floatFlags) ^ 1) << 12) | (uint32_t)X(RAX)); }  // cset
    void movzxEaxAl() {}
    void xorEaxEax() { movz(X(RAX), 0); }
    void movEaxImm(uint32_t v) { movz(X(RAX), v); }
    void testEaxEax() {  // 32 bits, like x86 (helpers returning int leave the top half undefined)
        floatFlags = false;
        ins(0x6A00001Fu | ((uint32_t)X(RAX) << 16) | ((uint32_t)X(RAX) << 5));
    }
    void push(int r) { ins(0xF81F0C00u | ((uint32_t)SP << 5) | (uint32_t)X(r)); }   // str x, [sp, #-16]!
    void pop(int r) { ins(0xF8410400u | ((uint32_t)SP << 5) | (uint32_t)X(r)); }    // ldr x, [sp], #16
    void subRsp(int32_t v) { ins(0xD10003FFu | ((uint32_t)((v + 15) / 16 * 16) << 10)); }
    void addRsp(int32_t v) { ins(0x910003FFu | ((uint32_t)((v + 15) / 16 * 16) << 10)); }
    void enter() {
        ins(0xA9BF7BFDu);  // stp x29, x30, [sp, #-16]!
        ins(0x910003FDu);  // mov x29, sp
    }
    void leave() { ins(0xA8C17BFDu); }  // ldp x29, x30, [sp], #16
    void callR(int reg) {
        ins(0xD63F0000u | ((uint32_t)X(reg) << 5));  // blr
        ins(0xAA0003E0u | (0u << 16) | (uint32_t)X(RAX));  // the result: x0 -> rax
    }
    void ret() {
        ins(0xAA0003E0u | ((uint32_t)X(RAX) << 16) | 0u);  // rax -> x0
        ins(0xD65F03C0u);
    }
    void store32(int base, int32_t disp, int reg) { mem(false, 4, X(reg), X(base), disp); }
    void storeDword(int base, int32_t disp, uint32_t v) {
        ins(0x52800000u | ((v & 0xFFFF) << 5) | T0);
        if (v >> 16) ins(0x72A00000u | ((v >> 16) << 5) | T0);  // movk w16, #hi, lsl 16
        mem(false, 4, T0, X(base), disp);
    }

    int newLabel() {
        labels.push_back(-1);
        return (int)labels.size() - 1;
    }
    void bind(int l) { labels[(size_t)l] = (int64_t)b.size(); }
    void jcc(int cc, int l) {
        fixes.push_back({b.size(), l, 1});
        ins(0x54000000u | cond(cc, floatFlags));
    }
    void jmp(int l) {
        fixes.push_back({b.size(), l, 0});
        ins(0x14000000u);
    }
    void call(int l) {
        fixes.push_back({b.size(), l, 0});
        ins(0x94000000u);
        ins(0xAA0003E0u | (uint32_t)X(RAX));  // x0 -> rax
    }
    // --- register forms for the typed tier (values live in registers)
    void imulRR(int dst, int src) {  // mul
        ins(0x9B007C00u | ((uint32_t)X(src) << 16) | ((uint32_t)X(dst) << 5) | (uint32_t)X(dst));
    }
    void cvtsi2sdRR(int xmm, int gpr) { ins(0x9E620000u | ((uint32_t)X(gpr) << 5) | (uint32_t)xmm); }  // scvtf
    void movqXG(int xmm, int gpr) { ins(0x9E670000u | ((uint32_t)X(gpr) << 5) | (uint32_t)xmm); }      // fmov d, x
    void movsdRR(int dst, int src) { ins(0x1E604000u | ((uint32_t)src << 5) | (uint32_t)dst); }        // fmov d, d
    void mulHigh(int reg) {  // smulh: rdx = the high 64 bits of rax * reg
        ins(0x9B407C00u | ((uint32_t)X(reg) << 16) | ((uint32_t)X(RAX) << 5) | (uint32_t)X(RDX));
    }
    void sarImm(int reg, uint8_t n) {  // asr
        ins(0x9340FC00u | ((uint32_t)n << 16) | ((uint32_t)X(reg) << 5) | (uint32_t)X(reg));
    }
    void andR(int a2, int b2) { ins(0x8A000000u | ((uint32_t)X(b2) << 16) | ((uint32_t)X(a2) << 5) | (uint32_t)X(a2)); }
    void movupsStore(int, int32_t, int) {}  // (d registers 2-7 belong to the callee here: nothing to save)
    void movupsLoad(int, int, int32_t) {}

    bool finish() {
        for (auto& f : fixes) {
            int64_t t = labels[(size_t)f.label];
            if (t < 0) return false;
            int64_t rel = (t - (int64_t)f.at) / 4;
            uint32_t w;
            std::memcpy(&w, &b[f.at], 4);
            if (f.kind == 0) {
                if (rel < -(1 << 25) || rel >= (1 << 25)) return false;
                w |= (uint32_t)(rel & 0x3FFFFFF);
            } else {
                if (rel < -(1 << 18) || rel >= (1 << 18)) return false;
                w |= (uint32_t)(rel & 0x7FFFF) << 5;
            }
            std::memcpy(&b[f.at], &w, 4);
        }
        return true;
    }
};

#endif  // IXO_JIT_X64 / IXO_JIT_ARM64

// Machine code lives in shared chunks, packed and 16-byte aligned, rather than
// one allocation per function (a page each, and 64 KB of address space on
// Windows). A chunk's pages are writable only while new code is copied in.
constexpr size_t kCodeChunk = 256 * 1024;
std::mutex codeArenaLock;
char* codeChunk = nullptr;
size_t codeUsed = 0, codeCap = 0;

size_t pageSize() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

// Code memory is mapped twice where the system allows it: once writable, once
// executable, so installing a function is a plain copy (no page-protection
// changes, which cost more than compiling a small function).
struct ExecChunk {
    char* exec;
    char* write;  // null: no second mapping (protection is switched instead)
    size_t size;
};
std::vector<ExecChunk> execChunks;  // under codeArenaLock

char* newExecChunk(size_t n) {
    char* exec = nullptr;
    char* write = nullptr;
#ifdef _WIN32
    if (HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                      (DWORD)((uint64_t)n >> 32), (DWORD)n, nullptr)) {
        write = static_cast<char*>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, n));
        exec = static_cast<char*>(MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, n));
        CloseHandle(h);  // the views keep the memory
        if (!write || !exec) {
            if (write) UnmapViewOfFile(write);
            if (exec) UnmapViewOfFile(exec);
            write = exec = nullptr;
        }
    }
    if (!exec) exec = static_cast<char*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#elif defined(__APPLE__) && defined(IXO_JIT_ARM64)
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    exec = p == MAP_FAILED ? nullptr : static_cast<char*>(p);
#else
#if defined(__linux__) && defined(SYS_memfd_create)
    int fd = (int)syscall(SYS_memfd_create, "ixo-jit", 1u /* MFD_CLOEXEC */);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)n) == 0) {
            void* w = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            void* x = mmap(nullptr, n, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
            if (w != MAP_FAILED && x != MAP_FAILED) {
                write = static_cast<char*>(w);
                exec = static_cast<char*>(x);
            } else {
                if (w != MAP_FAILED) munmap(w, n);
                if (x != MAP_FAILED) munmap(x, n);
            }
        }
        close(fd);  // the mappings keep the memory
    }
#endif
    if (!exec) {
        void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        exec = p == MAP_FAILED ? nullptr : static_cast<char*>(p);
    }
#endif
    if (exec) execChunks.push_back({exec, write, n});
    return exec;
}

// Copies code to dst (inside a chunk) and leaves its pages executable.
bool writeExec(char* dst, const uint8_t* src, size_t n) {
    for (auto it = execChunks.rbegin(); it != execChunks.rend(); ++it) {
        if (dst < it->exec || dst >= it->exec + it->size) continue;
        if (!it->write) break;
        std::memcpy(it->write + (dst - it->exec), src, n);
#ifdef _WIN32
        FlushInstructionCache(GetCurrentProcess(), dst, n);
#else
        __builtin___clear_cache(dst, dst + n);
#endif
        return true;
    }
    size_t page = pageSize();
    char* lo = reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(dst) & ~(uintptr_t)(page - 1));
    size_t len = (size_t)(dst + n - lo);
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(lo, len, PAGE_READWRITE, &old)) return false;
    std::memcpy(dst, src, n);
    if (!VirtualProtect(lo, len, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), dst, n);
#elif defined(__APPLE__) && defined(IXO_JIT_ARM64)
    (void)lo;
    (void)len;
    pthread_jit_write_protect_np(0);
    std::memcpy(dst, src, n);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(dst, n);
#else
    if (mprotect(lo, len, PROT_READ | PROT_WRITE) != 0) return false;
    std::memcpy(dst, src, n);
    if (mprotect(lo, len, PROT_READ | PROT_EXEC) != 0) return false;
    __builtin___clear_cache(dst, dst + n);
#endif
    return true;
}

// freed pieces of code memory (code replaced and no longer running), reused first
std::vector<std::pair<char*, size_t>> freeCode;  // under codeArenaLock

void* allocExec(const std::vector<uint8_t>& code) {
    std::lock_guard<std::mutex> g(codeArenaLock);
    size_t n = (code.size() + 15) & ~(size_t)15;
    char* dst = nullptr;
    for (size_t i = 0; i < freeCode.size(); i++)
        if (freeCode[i].second >= n) {
            dst = freeCode[i].first;
            if (freeCode[i].second - n >= 64) {
                freeCode[i].first += n;
                freeCode[i].second -= n;
            } else {
                freeCode.erase(freeCode.begin() + (std::ptrdiff_t)i);
            }
            if (!writeExec(dst, code.data(), code.size())) return nullptr;
            return dst;
        }
    if (n > kCodeChunk / 4) {  // a big function gets pages of its own
        size_t page = pageSize();
        dst = newExecChunk((n + page - 1) / page * page);
    } else {
        if (!codeChunk || codeUsed + n > codeCap) {
            codeChunk = newExecChunk(kCodeChunk);
            codeUsed = 0;
            codeCap = codeChunk ? kCodeChunk : 0;
        }
        dst = codeChunk ? codeChunk + codeUsed : nullptr;
        codeUsed += n;
    }
    if (!dst || !writeExec(dst, code.data(), code.size())) return nullptr;
    return dst;
}

// Every piece of machine code, for mapping addresses back to bytecode (the
// profiler). Code is never freed, so the entries stay valid.
struct CodeRange {
    uintptr_t start, end;
    ObjFunction* fn;
    const JitCode* code;
};
std::mutex codeRangesLock;
std::vector<CodeRange> codeRanges;  // sorted by start
std::atomic<uintptr_t> codeLo{UINTPTR_MAX}, codeHi{0};

void unregisterCode(uintptr_t start) {
    std::lock_guard<std::mutex> g(codeRangesLock);
    for (auto it = codeRanges.begin(); it != codeRanges.end(); ++it)
        if (it->start == start) {
            codeRanges.erase(it);
            return;
        }
}

void registerCode(uintptr_t start, size_t size, ObjFunction* fn, const JitCode* code) {
    std::lock_guard<std::mutex> g(codeRangesLock);
    if (start < codeLo.load()) codeLo.store(start);
    if (start + size > codeHi.load()) codeHi.store(start + size);
    CodeRange r{start, start + size, fn, code};
    auto at = std::upper_bound(codeRanges.begin(), codeRanges.end(), start,
                               [](uintptr_t s, const CodeRange& c) { return s < c.start; });
    codeRanges.insert(at, r);
}

// Offsets of fields the generated code reads, measured on real objects.
struct Layout {
    int32_t objType, iterSrc, iterCur, iterPending, iterPairs, rangeEnd, rangeStep, rangeIncl, rangeOpenStart,
        rangeOpenEnd, ctxStackEnd, ctxDepth, ctxMax, ctxBailOff, ctxBailDepth, ctxBailClosure, ctxBailBase,
        ctxWaitingSelf, ctxBudget, ctxBudgetExit, objFrozen, instCls, instFields, clsId, listItems, icClsId, icIndex, iterIdx,
        strAppendable, objSize, mapLive, objKind, closFn, fnJitDirect, fnArity, fnJit, fnJitCalls, jcFn, jcMaxDepth,
        closUpvals, upFiber, upSlot, upClosed, fiberStack;
    bool vectorTriple = false;  // std::vector is three pointers: begin, end, end of capacity
    Layout() {
        ObjIterator it{Value(), false};
        ObjRange r{0, 0, 1, false};
        JitCtx c{};
        auto d = [](const void* base, const void* field) {
            return (int32_t)((const char*)field - (const char*)base);
        };
        objType = d(&it, &it.type);
        iterSrc = d(&it, &it.src);
        iterCur = d(&it, &it.cur);
        iterPending = d(&it, &it.pending);
        iterPairs = d(&it, &it.pairs);
        iterIdx = d(&it, &it.idx);
        rangeEnd = d(&r, &r.end);
        rangeStep = d(&r, &r.step);
        rangeIncl = d(&r, &r.inclusive);
        rangeOpenStart = d(&r, &r.openStart);
        rangeOpenEnd = d(&r, &r.openEnd);
        ctxStackEnd = d(&c, &c.stackEnd);
        ctxDepth = d(&c, &c.depth);
        ctxMax = d(&c, &c.maxDepth);
        ctxBailOff = d(&c, &c.bailOffset);
        ctxBailDepth = d(&c, &c.bailDepth);
        ctxBailClosure = d(&c, &c.bailClosure);
        ctxBailBase = d(&c, &c.bailBase);
        ctxWaitingSelf = d(&c, &c.waitingSelf);
        ctxBudget = d(&c, &c.budget);
        ctxBudgetExit = d(&c, &c.budgetExit);
        objFrozen = d(&it, &it.frozen);
        static ObjClass cls;  // only for measuring offsets
        alignas(16) static char instMem[sizeof(ObjInstance)];
        auto* inst = new (instMem) ObjInstance(&cls, 0);
        instCls = d(inst, &inst->cls);
        instFields = (int32_t)sizeof(ObjInstance);  // the fields follow the instance
        clsId = d(&cls, &cls.id);
        ObjList lst;
        listItems = d(&lst, &lst.vals);  // every kind's vector starts here (a union)
        objKind = d(&lst, &lst.kind);
        ObjString str{std::string()};
        strAppendable = d(&str, &str.appendable);
        objSize = d(&str, &str.size);
        ObjMap mp;
        mapLive = d(&mp, &mp.live);
        std::vector<Value> v;
        v.reserve(4);
        v.push_back(Value());
        uintptr_t w[3];
        std::memcpy(w, &v, sizeof w);
        vectorTriple = sizeof(v) == sizeof w && w[0] == (uintptr_t)v.data() && w[1] == (uintptr_t)(v.data() + 1) &&
                       w[2] == (uintptr_t)(v.data() + 4);
        // the typed kinds' vectors must look the same (machine code reads all of them)
        auto triple = [&](auto& vec) {
            vec.reserve(4);
            vec.push_back(0);
            uintptr_t t[3];
            std::memcpy(t, &vec, sizeof t);
            return sizeof(vec) == sizeof t && t[0] == (uintptr_t)vec.data() && t[1] == (uintptr_t)(vec.data() + 1) &&
                   t[2] == (uintptr_t)(vec.data() + 4);
        };
        std::vector<int64_t> vi;
        std::vector<double> vd;
        std::vector<uint8_t> vb;
        vectorTriple = vectorTriple && triple(vi) && triple(vd) && triple(vb);
        InlineCache ic;
        icClsId = d(&ic, &ic.clsId);
        icIndex = d(&ic, &ic.index);
        static ObjFunction fnm;  // only for measuring offsets
        static ObjClosure clm(&fnm);
        closFn = d(&clm, &clm.fn);
        fnJitDirect = d(&fnm, &fnm.jitDirect);
        fnArity = d(&fnm, &fnm.arity);
        fnJit = d(&fnm, &fnm.jit);
        fnJitCalls = d(&fnm, &fnm.jitCalls);
        static JitCode jcm;
        jcFn = d(&jcm, &jcm.fn);
        jcMaxDepth = d(&jcm, &jcm.maxDepth);
        closUpvals = d(&clm, &clm.upvalues);  // the vector's first word: its data
        static ObjUpvalue upm;
        upFiber = d(&upm, &upm.fiber);
        upSlot = d(&upm, &upm.slot);
        upClosed = d(&upm, &upm.closed);
        static Fiber fbm;
        fiberStack = d(&fbm, &fbm.stack);
    }
};

const Layout& layout() {
    static Layout l;
    return l;
}

constexpr uint8_t TAG_FLOAT = (uint8_t)VT::Float;
constexpr uint8_t TAG_NIL = (uint8_t)VT::Nil, TAG_BOOL = (uint8_t)VT::Bool, TAG_INT = (uint8_t)VT::Int,
                  TAG_OBJ = (uint8_t)VT::Obj, TAG_UNDEF = (uint8_t)VT::Undef;
constexpr int32_t kValTag = 0, kValPayload = 8;  // Value: tag byte, then the 8-byte payload
static_assert(sizeof(Value) == 16, "the JIT assumes 16-byte values");

// Makes the range iterator for `for x in a..b` (RANGE + ITER_INIT), called
// from generated code. 0 on success, 1 if the bounds aren't ints.
extern "C" int ixoJitMakeIter(JitCtx* ctx, Value* first, uint64_t flags) {
    bool hasStep = flags & 2;
    Value s = first[0], e = first[1], st = hasStep ? first[2] : Value::integer(1);
    if (s.t != VT::Int || e.t != VT::Int || st.t != VT::Int || st.i == 0) return 1;
    VM& vm = *ctx->vm;
    auto* r = vm.alloc<ObjRange>(s.i, e.i, st.i, (flags & 1) != 0);
    auto* it = vm.alloc<ObjIterator>(Value::object(r), false);
    it->cur = s.i;
    first[0] = Value::object(it);
    return 0;
}

// A compiled function's call stopped early: it waits (at `resume`) for the
// interpreter to finish that call.
extern "C" void ixoJitWait(JitCtx* ctx, Value* base, uint64_t resume) {
    if (ctx->chainLen >= 0 && ctx->chainLen < ctx->chainCap)
        ctx->chain[ctx->chainLen++] = {ctx->waitingSelf, base, (uint32_t)resume};
    else
        ctx->chainLen = -1;  // can't happen: the chain has room for maxDepth calls
}

// A call from machine code to another function: its machine code (compiled now
// if it's hot). 0: returned; 1: stopped early (the chain says where);
// 2: can't run it in machine code (the interpreter makes the call instead).
static int runCallee(JitCtx* ctx, ObjClosure* cl, Value* base);

// Class(args) from machine code: the instance, then its constructor's machine code.
static int64_t jitConstruct(JitCtx* ctx, Value* base, ObjClass* cls, int argc) {
    ObjClosure* ctor = cls->ctor;
    if (!ctor) return JIT_STEP;
    ObjFunction* fn = ctor->fn;
    if (fn->arity != argc || fn->isFork || fn->isPar || fn->upvalueCount > 0) return JIT_EXIT;
    VM& vm = *ctx->vm;
    if (fn->jitState != 2 && !vm.jitTry(ctor)) return JIT_EXIT;
    if (ctx->depth >= ctx->maxDepth || base + fn->jit->maxDepth + 2 > ctx->stackEnd) return JIT_EXIT;
    auto* inst = vm.newInstance(cls);
    base[0] = Value::object(inst);
    if (vm.bytesAllocated > vm.nextGC) ctx->budget = 0;
    int r = runCallee(ctx, ctor, base);
    if (r == 2) base[0] = Value::object(cls);  // didn't run: the interpreter makes the call
    return r;
}

extern "C" int64_t ixoJitCall(JitCtx* ctx, Value* base, uint64_t sitePtr) {
    auto* site = reinterpret_cast<JitOpSite*>((uintptr_t)sitePtr);
    int argc = site->pops - 1;
    Value c = base[0];
    if (c.t != VT::Obj) return JIT_STEP;
    if (c.o->type == OT::Native)
        return ctx->vm->jitNative(*ctx, base - (site->depth - site->pops), *site, static_cast<ObjNative*>(c.o), base,
                                  base + 1, argc);
    if (c.o->type == OT::Class) return jitConstruct(ctx, base, static_cast<ObjClass*>(c.o), argc);
    if (c.o->type != OT::Closure) return JIT_STEP;
    auto* cl = static_cast<ObjClosure*>(c.o);
    ObjFunction* fn = cl->fn;
    if (fn->arity != (int)argc || fn->isFork || fn->isPar || (fn->upvalueCount > 0 && fn->methodStyle)) return 2;
    if (fn->jitState != 2 && !ctx->vm->jitTry(cl)) return 2;
    if (ctx->vm->jitShouldDemote(fn)) ctx->vm->jitDemote(fn, cl);
    if (ctx->depth >= ctx->maxDepth || base + fn->jit->maxDepth + 1 > ctx->stackEnd) return 2;
    fn->jitCalls++;
    ctx->depth++;
    int r = fn->jit->fn(base, ctx, 0);
    ctx->depth--;
    if (r == 1 && (ctx->vm->jitPanic || ctx->pausedExit || ctx->budgetExit)) return r;  // not a failure
    if (r != 0 && ++fn->jitBails > 50 && fn->jitBails * 2 > fn->jitCalls) {
        fn->jitState = 4;
        fn->jitDirect = 0;
        if (ctx->vm->jitStats) ctx->vm->noteJitGiveUp(fn, "calls");
    }
    return r;
}

// After machine code called another function's machine code directly and it
// stopped early: what ixoJitCall does then. Returns r.
extern "C" int64_t ixoJitDirectBail(JitCtx* ctx, ObjFunction* fn, uint64_t r) {
    if (r == 1 && (ctx->vm->jitPanic || ctx->pausedExit || ctx->budgetExit)) return (int64_t)r;  // not a failure
    if (++fn->jitBails > 50 && fn->jitBails * 2 > fn->jitCalls) {
        fn->jitState = 4;
        fn->jitDirect = 0;
        if (ctx->vm->jitStats) ctx->vm->noteJitGiveUp(fn, "calls");
    }
    return (int64_t)r;
}

// Arithmetic and comparisons that aren't int-int (floats, mixed): 0 done (the
// result is in a[0]), 1 not numbers or a zero divisor (the interpreter handles it).
extern "C" int64_t ixoJitNum(JitCtx* ctx, Value* a, uint64_t op) {
    if (op == OP_NEG) {
        if (!a[0].isNum()) return 1;
        a[0] = a[0].isInt() ? Value::integer((int64_t)(0ULL - (uint64_t)a[0].i)) : Value::number(-a[0].f);
        return 0;
    }
    Value x = a[0], y = a[1];
    if (!x.isNum() || !y.isNum()) return 1;
    if ((op == OP_DIV || op == OP_MOD || op == OP_IDIV) && y.num() == 0) return 1;
    a[0] = ctx->vm->binaryOp((uint8_t)op, x, y);
    return 0;
}

// JUMP_IF_NOT_CMP on values that aren't both ints: 1 true, 0 false, 2 stop
// here (a panic, reported once the interpreter has the frames).
extern "C" int64_t ixoJitCmp(JitCtx* ctx, Value* a, uint64_t cmp) {
    Value x = a[0], y = a[1];
    if (!x.isNum() || !y.isNum()) {
        static const Op ops[] = {OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE};
        VM& vm = *ctx->vm;
        try {
            return vm.truthy(vm.binaryOp(ops[cmp < 6 ? cmp : 5], x, y)) ? 1 : 0;
        } catch (...) {
            vm.jitPanic = std::current_exception();
            return 2;
        }
    }
    bool r;
    if (x.isInt() && y.isInt()) {
        int64_t p = x.i, q = y.i;
        r = cmp == 0 ? p < q : cmp == 1 ? p <= q : cmp == 2 ? p > q : cmp == 3 ? p >= q : cmp == 4 ? p == q : p != q;
    } else {
        double p = x.num(), q = y.num();
        r = cmp == 0 ? p < q : cmp == 1 ? p <= q : cmp == 2 ? p > q : cmp == 3 ? p >= q : cmp == 4 ? p == q : p != q;
    }
    return r ? 1 : 0;
}

// A member call site in machine code (obj.method(args), obj.alive, x.f(a)).
struct CallSite {
    InlineCache* ic;
    int sym;
    uint8_t fk;
    uint16_t fi;
    int argc;
    ModuleRt* module;  // the calling function's (for x.f(a) meaning f(x, a))
    bool member;       // obj.name (GET_MEMBER): calls only what takes no arguments
    JitOpSite op;
    int cachedTag = -1;  // the built-in method last found for a receiver of this type
    Value cachedMethod;
};

static int runCallee(JitCtx* ctx, ObjClosure* cl, Value* base) {
    ObjFunction* fn = cl->fn;
    if (fn->jitState != 2 && !ctx->vm->jitTry(cl)) return 2;
    if (ctx->vm->jitShouldDemote(fn)) ctx->vm->jitDemote(fn, cl);
    if (ctx->depth >= ctx->maxDepth || base + fn->jit->maxDepth + 2 > ctx->stackEnd) return 2;
    fn->jitCalls++;
    ctx->depth++;
    int res = fn->jit->fn(base, ctx, 0);
    ctx->depth--;
    if (res == 1 && (ctx->vm->jitPanic || ctx->pausedExit || ctx->budgetExit)) return res;
    if (res != 0 && ++fn->jitBails > 50 && fn->jitBails * 2 > fn->jitCalls) {
        fn->jitState = 4;
        fn->jitDirect = 0;
        if (ctx->vm->jitStats) ctx->vm->noteJitGiveUp(fn, "calls");
    }
    return res;
}

// The member's machine code: a method in the inline cache (base[0] is the
// receiver, its `self`), or a function used as a method (x.f(a) is f(x, a):
// the receiver and arguments move up a slot for the function). Codes as ixoJitCall.
extern "C" int64_t ixoJitInvoke(JitCtx* ctx, Value* base, uint64_t sitePtr) {
    auto* site = reinterpret_cast<CallSite*>((uintptr_t)sitePtr);
    Value r = base[0];
    VM& vm = *ctx->vm;
    if (r.t == VT::Obj && r.o->type == OT::Instance) {
        auto* inst = static_cast<ObjInstance*>(r.o);
        InlineCache* ic = site->ic;
        if (ic->clsId != inst->cls->id || ic->index >= 0 || !isObjType(ic->method, OT::Closure)) return JIT_STEP;
        auto* cl = as<ObjClosure>(ic->method);
        ObjFunction* fn = cl->fn;
        if (fn->arity != site->argc || fn->isFork || fn->isPar || fn->upvalueCount > 0) return JIT_EXIT;
        return runCallee(ctx, cl, base);
    }
    // not an instance: a built-in method of its type comes first
    if (r.t == VT::Obj && r.o->frozen) return JIT_STEP;  // the interpreter checks what may change it
    int tag = (int)vm.tagOf(r);
    Value m;
    if (site->cachedTag == tag) {
        m = site->cachedMethod;
    } else {
        auto& table = vm.methods[tag];
        auto mt = table.find(site->sym);
        if (mt != table.end()) {
            m = mt->second.fn;
            site->cachedTag = tag;
            site->cachedMethod = m;  // built-in methods live as long as the VM
        }
    }
    if (isObjType(m, OT::Native)) {
        auto* n = as<ObjNative>(m);
        if (site->member && n->minArity - 1 != 0) return JIT_STEP;  // a bound method value
        return vm.jitNative(*ctx, base - (site->op.depth - site->op.pops), site->op, n, base, base, site->argc + 1);
    }
    if (isObjType(m, OT::Closure)) {  // a method written in Ixo (the prelude)
        auto* cl = as<ObjClosure>(m);
        ObjFunction* fn = cl->fn;
        if (site->member && !fn->autoCall) return JIT_STEP;
        if (fn->arity != site->argc || fn->isFork || fn->isPar || fn->upvalueCount > 0) return JIT_EXIT;
        return runCallee(ctx, cl, base);
    }
    if (!m.isNil()) return JIT_STEP;
    Value fb;
    if (site->fk == FB_GLOBAL && site->module && site->fi < site->module->globals.size())
        fb = site->module->globals[site->fi];
    if (!isObjType(fb, OT::Closure)) return JIT_STEP;
    auto* cl = as<ObjClosure>(fb);
    ObjFunction* fn = cl->fn;
    if (fn->arity != site->argc + 1 || fn->isFork || fn->isPar || fn->upvalueCount > 0 || fn->methodStyle)
        return JIT_EXIT;
    if (fn->jitState != 2 && !vm.jitTry(cl)) return JIT_EXIT;
    for (int k = site->argc; k >= 0; k--) base[k + 1] = base[k];
    base[0] = fb;
    int res = runCallee(ctx, cl, base);
    if (res == 2) {  // didn't run: put things back for the interpreter
        for (int k = 0; k <= site->argc; k++) base[k] = base[k + 1];
    }
    return res;
}

// Runs one instruction in the interpreter for the machine code (see VM::jitStep).
extern "C" int64_t ixoJitStep(JitCtx* ctx, Value* frameBase, uint64_t sitePtr) {
    return ctx->vm->jitFastOp(*ctx, frameBase, *reinterpret_cast<JitOpSite*>((uintptr_t)sitePtr));
}

// for x in list (without RANGE): the iterator, for lists and ranges only.
extern "C" int64_t ixoJitIterInit(JitCtx* ctx, Value* slot, uint64_t) {
    Value v = slot[0];
    if (v.t != VT::Obj) return 1;
    if (v.o->type == OT::List) {
        slot[0] = Value::object(ctx->vm->alloc<ObjIterator>(v, false));
        return 0;
    }
    if (v.o->type == OT::Range && !static_cast<ObjRange*>(v.o)->openStart && !static_cast<ObjRange*>(v.o)->openEnd) {
        auto* it = ctx->vm->alloc<ObjIterator>(v, false);
        it->cur = static_cast<ObjRange*>(v.o)->start;
        slot[0] = Value::object(it);
        return 0;
    }
    return 1;
}

struct Gen {
    ObjFunction* fn;
    ObjClosure* self;
    Analysis& an;
    VM* vm = nullptr;  // for the heap counters (inline list appends)
    Asm a;
    int frameSize = 0;
    int32_t saveArea = 32;  // after the helper-call shadow space
    int entryLabel = -1, okLabel = -1, bailCommon = -1, epilogue = -1;
    std::vector<int> insLabel;
    // bail stubs: (label, bytecode offset, depth)
    struct Stub {
        int label;
        uint32_t off, depth;
    };
    std::vector<Stub> stubs;
    std::map<size_t, int> stubFor;  // instruction index -> its stub label
    std::map<size_t, int> stepFor;  // instruction index -> its step stub label
    std::vector<size_t> stepList;
    // out-of-line paths for numbers that aren't both ints (a helper call)
    struct Slow {
        int label, cont;
        size_t ins;
        int d;
        uint8_t op;
        int target;  // JUMP_IF_NOT_CMP: where it jumps
    };
    std::vector<Slow> slows;
    JitCode* code = nullptr;
    // slots an instruction consumes (what jitStep puts back if it gives up)
    static int popsOf(const Ins& in) {
        if (in.step) return in.pops;
        switch (in.op) {
            case OP_CALL: case OP_INVOKE: return (int)in.a + 1;
            case OP_GET_INDEX: case OP_SET_FIELD: case OP_SET_MEMBER: case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_DIV: case OP_MOD: case OP_IDIV: case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ:
            case OP_NE:
                return 2;
            case OP_SET_INDEX: return 3;
            case OP_GET_FIELD: case OP_GET_MEMBER: case OP_GET_MEMBER_RAW: case OP_NEG: case OP_NOT:
            case OP_AUTOCALL: case OP_CHECK_TYPE:
                return 1;
            case OP_RANGE_INIT: return 3;
            case OP_SEQ_INIT: return 1;
            default: return 0;
        }
    }
    JitOpSite opSiteOf(size_t i) {
        const Ins& in = an.ins[i];
        JitOpSite s{};
        s.self = self;
        s.off = (uint32_t)in.off;
        s.next = (uint32_t)in.next;
        s.depth = in.depth;
        s.fall = in.fall;
        s.pops = popsOf(in);
        return s;
    }
    JitOpSite* opSite(size_t i) {
        auto s = std::make_shared<JitOpSite>(opSiteOf(i));
        code->keep.push_back(s);
        return s.get();
    }
    CallSite* siteFor(const Ins& in, int argc) {
        auto s = std::make_shared<CallSite>();
        s->ic = &fn->ic[in.b];
        s->sym = (int)in.sym;
        s->fk = in.fk;
        s->fi = (uint16_t)in.fi;
        s->argc = argc;
        s->module = fn->module;
        s->member = in.op == OP_GET_MEMBER;
        s->op = opSiteOf(an.at[in.off]);
        code->keep.push_back(s);
        return s.get();
    }
    // Runs instruction i in the interpreter, then continues with the next one
    // (or stops here when that can't work out).
    int stepAt(size_t i) {
        auto it = stepFor.find(i);
        if (it != stepFor.end()) return it->second;
        const Ins& in = an.ins[i];
        if (in.fall < 0 || i + 1 >= an.ins.size() || an.ins[i + 1].depth != in.fall) return bailAt(i);
        int l = a.newLabel();
        stepFor[i] = l;
        stepList.push_back(i);
        return l;
    }
    // after a helper call: 0 done, 1 stopped (wait for the interpreter), 2 stop
    // here, 3 run it in the interpreter, 4 paused (ctx says where)
    // RAX = the closure running this code: a function with captured variables
    // has one closure per creation, found in its frame's slot 0
    void loadSelf() {
        if (fn->upvalueCount > 0) a.load(RAX, RBX, slot(0) + kValPayload);
        else a.movImm(RAX, (uint64_t)(uintptr_t)self);
    }
    void afterCall(size_t i, int ok, const Ins& in) {
        a.testR(RAX, RAX);
        a.jcc(CC_E, ok);
        int stopped = a.newLabel();
        a.cmpImm32(RAX, 1);
        a.jcc(CC_E, stopped);
        a.cmpImm32(RAX, JIT_PAUSED);
        a.jcc(CC_E, bailCommon);
        a.cmpImm32(RAX, JIT_STEP);
        a.jcc(CC_E, stepAt(i));
        a.jmp(bailAt(i));
        a.bind(stopped);
        loadSelf();
        a.store(R12, layout().ctxWaitingSelf, RAX);
        a.movR(ARG0, R12);
        a.movR(ARG1, RBX);
        a.movImm(ARG2, (uint64_t)in.next);
        a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitWait);
        a.callR(RAX);
        a.movEaxImm(1);
        a.jmp(epilogue);
    }
    int slowFor(size_t i, int d, uint8_t op, int cont, int target = -1) {
        int l = a.newLabel();
        slows.push_back({l, cont, i, d, op, target});
        return l;
    }
    // RAX = a list: jumps by its kind to bools / words (ints or floats) labels;
    // values and empty lists fall through. Uses R9.
    void listKind(int bools, int ints, int floats) {
        a.movzxByte(R9, RAX, layout().objKind);
        a.cmpImm32(R9, LK_BOOLS);
        a.jcc(CC_E, bools);
        a.cmpImm32(R9, LK_INTS);
        a.jcc(CC_E, ints);
        a.cmpImm32(R9, LK_FLOATS);
        a.jcc(CC_E, floats);
    }
    // RDX = element address of item RCX (negative counts from the end) in the list
    // RAX with elements of 1 << shift bytes; out of range goes to `bad`.
    void listElement(int shift, int bad) {
        const Layout& L = layout();
        a.load(RDX, RAX, L.listItems);
        a.load(R8, RAX, L.listItems + 8);
        a.subR(R8, RDX);
        if (shift) a.shrImm(R8, (uint8_t)shift);  // the length
        int pos = a.newLabel();
        a.testR(RCX, RCX);
        a.jcc(CC_NS, pos);
        a.addR(RCX, R8);
        a.bind(pos);
        a.cmpR(RCX, R8);
        a.jcc(CC_AE, bad);  // out of range (or still negative): the interpreter reports it
        if (shift) a.shlImm(RCX, (uint8_t)shift);
        a.addR(RDX, RCX);
    }
    // slot k = the item at RDX of a list of kind lk
    void readElement(int k, ListKind lk) {
        if (lk == LK_INTS) {
            a.load(RCX, RDX, 0);
            storeInt(k, RCX);
        } else if (lk == LK_FLOATS) {
            a.load(RCX, RDX, 0);
            a.store(RBX, slot(k) + kValPayload, RCX);
            a.storeByte(RBX, slot(k) + kValTag, TAG_FLOAT);
        } else if (lk == LK_BOOLS) {
            a.movzxByte(RCX, RDX, 0);
            a.store(RBX, slot(k) + kValPayload, RCX);
            a.storeByte(RBX, slot(k) + kValTag, TAG_BOOL);
        } else {
            a.load(RCX, RDX, 0);
            a.store(RBX, slot(k), RCX);
            a.load(RCX, RDX, 8);
            a.store(RBX, slot(k) + 8, RCX);
        }
    }
    // the item at RDX of a list of kind lk = slot v (whose tag must fit, else `bad`)
    void writeElement(int v, ListKind lk, int bad) {
        if (lk == LK_INTS || lk == LK_FLOATS) {
            a.cmpByte(RBX, slot(v) + kValTag, lk == LK_INTS ? TAG_INT : TAG_FLOAT);
            a.jcc(CC_NE, bad);
            a.load(RCX, RBX, slot(v) + kValPayload);
            a.store(RDX, 0, RCX);
        } else if (lk == LK_BOOLS) {
            int zero = a.newLabel(), done = a.newLabel();
            a.cmpByte(RBX, slot(v) + kValTag, TAG_BOOL);
            a.jcc(CC_NE, bad);
            a.cmpByte(RBX, slot(v) + kValPayload, 0);
            a.jcc(CC_E, zero);
            a.storeByte(RDX, 0, 1);
            a.jmp(done);
            a.bind(zero);
            a.storeByte(RDX, 0, 0);
            a.bind(done);
        } else {
            a.load(RCX, RBX, slot(v));
            a.store(RDX, 0, RCX);
            a.load(RCX, RBX, slot(v) + 8);
            a.store(RDX, 8, RCX);
        }
    }
    // Jumps to `falsy` or `truthy` like the interpreter's truthy(): nil, false
    // and errors are false; everything else is true.
    void truthiness(int k, int falsy, int truthy) {
        int notBool = a.newLabel(), notNil = a.newLabel();
        a.movzxByte(RAX, RBX, slot(k) + kValTag);
        a.cmpEaxImm8(TAG_BOOL);
        a.jcc(CC_NE, notBool);
        a.cmpByte(RBX, slot(k) + kValPayload, 0);
        a.jcc(CC_E, falsy);
        a.jmp(truthy);
        a.bind(notBool);
        a.testEaxEax();  // nil
        a.jcc(CC_NE, notNil);
        a.jmp(falsy);
        a.bind(notNil);
        a.cmpEaxImm8(TAG_OBJ);
        a.jcc(CC_NE, truthy);
        a.load(RCX, RBX, slot(k) + kValPayload);
        a.cmpByte(RCX, layout().objType, (uint8_t)OT::Error);
        a.jcc(CC_E, falsy);
        a.jmp(truthy);
    }
    void intOr(int k, int label) {
        a.cmpByte(RBX, slot(k) + kValTag, TAG_INT);
        a.jcc(CC_NE, label);
    }
    // xmm = slot k as a double (an int converts); anything else goes to `other`
    void loadNum(int xmm, int k, int other) {
        int isF = a.newLabel(), done = a.newLabel();
        a.cmpByte(RBX, slot(k) + kValTag, TAG_FLOAT);
        a.jcc(CC_E, isF);
        a.cmpByte(RBX, slot(k) + kValTag, TAG_INT);
        a.jcc(CC_NE, other);
        a.cvtsi2sdLoad(xmm, RBX, slot(k) + kValPayload);
        a.jmp(done);
        a.bind(isF);
        a.movsdLoad(xmm, RBX, slot(k) + kValPayload);
        a.bind(done);
    }

    Gen(ObjFunction* f, ObjClosure* s, Analysis& an) : fn(f), self(s), an(an) {}

    static int32_t slot(int k) { return 16 * k; }

    int bailAt(size_t i) {
        auto it = stubFor.find(i);
        if (it != stubFor.end()) return it->second;
        int l = a.newLabel();
        stubs.push_back({l, (uint32_t)an.ins[i].off, (uint32_t)an.ins[i].depth});
        stubFor[i] = l;
        return l;
    }
    void needInt(size_t i, int k) {
        a.cmpByte(RBX, slot(k) + kValTag, TAG_INT);
        a.jcc(CC_NE, bailAt(i));
    }
    void copySlot(int from, int to) {
        a.load(RAX, RBX, slot(from));
        a.store(RBX, slot(to), RAX);
        a.load(RAX, RBX, slot(from) + 8);
        a.store(RBX, slot(to) + 8, RAX);
    }
    void storeInt(int k, int reg) {
        a.storeByte(RBX, slot(k) + kValTag, TAG_INT);
        a.store(RBX, slot(k) + kValPayload, reg);
    }
    void storeBoolFromAl(int k) {
        a.movzxEaxAl();
        a.store(RBX, slot(k) + kValPayload, RAX);
        a.storeByte(RBX, slot(k) + kValTag, TAG_BOOL);
    }

    // ---------------------------------------------------------- the typed tier
    // Ints, floats and bools whose type is known live unboxed in registers
    // between instructions. Memory (the 16-byte slots) is brought up to date
    // where control flow joins, before anything the interpreter might see, and
    // before every instruction done the plain way (which then works as in
    // tier 1). `ty` is what each slot holds; `memTy` the tag its memory has.
    bool t2 = false;
    TypeInfo* ti = nullptr;
    struct Loc {
        int8_t reg = -1;  // general register, or 16 + k for xmm k
        bool dirty = false;
        uint32_t used = 0;
        int8_t alias = -1;  // a copy of that variable, sharing its register until it changes
        int8_t mem = -1;    // a copy of that variable (not unboxed), read from its slot until it changes
        bool ptr = false;   // reg holds the object pointer of a value that stays in memory
    };
    // what an object variable is known to be: an instance of this class (kAnyInstance:
    // of some class), or not known (kUnknown); until the variable changes
    static constexpr uint64_t kUnknown = UINT64_MAX, kAnyInstance = UINT64_MAX - 1;
    std::vector<uint64_t> instCls;
    std::vector<uint8_t> thawed;  // known not frozen (an object never changes that)
    std::vector<uint8_t> listOk;  // known to hold a list (a value's type never changes either)
    int memSlot(int k) const { return loc[(size_t)k].mem >= 0 ? loc[(size_t)k].mem : k; }
    std::vector<Loc> loc;
    std::vector<uint8_t> ty, memTy;
    int owner[32];
    uint32_t clock = 0;
    int live = 0;            // slots in use after the last instruction
    int pinA = -1, pinB = -1;  // slots whose registers the current instruction needs
    struct Cold {
        int label;
        std::vector<std::tuple<int, int, uint8_t, uint8_t>> spill;  // slot, register, type, memory's tag
        size_t ins;
        bool guess;  // a wrong guess: counts toward giving up on the typed tier
    };
    std::vector<Cold> colds;
    std::vector<int> osrLabel;
    int32_t xmmSave = 0;
    bool saveFp = false;  // the function uses float registers the caller keeps
    // Registers handed out (bit r; xmm k: bit 16 + k). A first, dry run finds them,
    // so the real one saves only the ones the caller keeps that are used.
    uint32_t used = 0, saveMask = 0xFFFFFFFFu;
    bool dry = false;
    bool saves(int r) const { return (saveMask >> r) & 1; }
    // Innermost loops keep their busiest typed variables in fixed registers
    // across iterations ("homes"): every way into one of the loop's join points
    // brings them there; code that arrives with everything in memory (the plain
    // way) enters through a stub that loads them.
    struct Region {
        size_t head = 0, end = 0;
        std::vector<std::pair<int, int>> map;  // slot, home register
    };
    std::vector<Region> regions;
    std::vector<int> regionOf;  // per instruction
    std::vector<int> canon;     // per instruction: the entry from memory (-1: none yet)
    int curRegion = -1;
    std::vector<int> homeReg;   // per slot, in the current region: its home register or -1
    std::vector<uint8_t> homeTy;  // ... and the type it holds there
    bool reserved[32] = {};
    std::vector<std::pair<size_t, int>> relabeled;  // join points pointed at their loading stubs for now
    std::vector<int> unprofiledLoop;  // per instruction: a loop head whose loop has no type profile yet (its index)
    int pinC = -1;

    static bool isX(int r) { return r >= 16; }
    // IXO_JIT_DEOPT_STRESS=1: every guess about a type counts as wrong (tests the way back)
    void guessCheck(int label) {
        static const bool stress = std::getenv("IXO_JIT_DEOPT_STRESS") != nullptr;
        if (stress) a.jmp(label);
        else a.jcc(CC_NE, label);
    }
    static uint8_t tagOf(uint8_t t) {
        return t == TY_INT ? TAG_INT : t == TY_FLOAT ? TAG_FLOAT : t == TY_BOOL ? TAG_BOOL : TAG_NIL;
    }
    uint8_t T(int k) const { return k >= 0 && k < (int)ty.size() ? ty[(size_t)k] : TY_ANY; }
    void unmap(int k) {
        if (k < 0 || k >= (int)loc.size()) return;
        Loc& l = loc[(size_t)k];
        if (l.alias >= 0 || l.mem >= 0) {
            l = Loc{};
            return;
        }
        detachAliases(k);
        if (l.reg >= 0) owner[l.reg] = -1;
        loc[(size_t)k] = Loc{};
        instCls[(size_t)k] = kUnknown;
        thawed[(size_t)k] = 0;
        listOk[(size_t)k] = 0;
    }
    // slot k's register goes (its value stays the same, in memory): copies sharing the register go to memory
    void dropReg(int k) {
        Loc& l = loc[(size_t)k];
        if (l.reg < 0) return;
        for (size_t t = 0; t < loc.size(); t++)
            if (loc[t].alias == k) {
                writeBack((int)t);
                loc[t] = Loc{};
            }
        owner[l.reg] = -1;
        l.reg = -1;
        l.dirty = false;
        l.ptr = false;
    }
    // slot k's register is about to change or go: copies of it keep the old value in memory
    void detachAliases(int k) {
        for (size_t t = 0; t < loc.size(); t++)
            if (loc[t].alias == k || loc[t].mem == k) {
                writeBack((int)t);
                loc[t] = Loc{};
            }
    }
    // a register only slot k uses (a copy of a variable gets its own)
    int ownReg(int k) {
        Loc& l = loc[(size_t)k];
        if (l.alias < 0) return regOf(k);
        int r = l.reg;
        int nr = allocReg(isX(r));
        if (isX(r)) a.movsdRR(nr - 16, r - 16);
        else a.movR(nr, r);
        Loc& m = loc[(size_t)k];
        m = Loc{(int8_t)nr, true, ++clock, -1};
        owner[nr] = k;
        return nr;
    }
    // what going to instruction j from here needs, done on the way (this side keeps its state)
    struct Edge {
        int label;
        size_t target;
        std::vector<std::tuple<int, int, uint8_t, uint8_t>> spill;  // slot, register, type, memory's tag
        std::vector<std::tuple<int, int, uint8_t>> loads;            // slot, home, type
    };
    std::vector<Edge> edges;
    int edgeTo(size_t j, int depth) {
        Edge e;
        e.target = j;
        int r = regionOf[j];
        std::vector<std::pair<int, int>> hs;
        if (r >= 0 && r == curRegion) hs = homesAt(j);
        for (int k = 0; k < depth && k < (int)loc.size(); k++) {
            const Loc& l = loc[(size_t)k];
            bool atHome = false;
            for (auto& h : hs) atHome |= h.first == k && h.second == l.reg;
            if (l.mem >= 0) e.spill.push_back({k, -2 - l.mem, ty[(size_t)k], memTy[(size_t)k]});
            else if (l.reg >= 0 && l.dirty && !atHome) e.spill.push_back({k, l.reg, ty[(size_t)k], memTy[(size_t)k]});
        }
        for (auto& [k, reg] : hs)
            if (loc[(size_t)k].reg != reg) e.loads.push_back({k, reg, ty[(size_t)k]});
        if (e.spill.empty() && e.loads.empty()) return insLabel[j];
        e.label = a.newLabel();
        edges.push_back(e);
        return e.label;
    }
    void emitEdges() {
        for (size_t n = 0; n < edges.size(); n++) {
            Edge e = edges[n];
            a.bind(e.label);
            for (auto& [k, r, t, mt] : e.spill) {
                if (r <= -2) {
                    copySlot(-2 - r, k);
                    continue;
                }
                if (isX(r)) a.movsdStore(RBX, slot(k) + kValPayload, r - 16);
                else a.store(RBX, slot(k) + kValPayload, r);
                if (mt != t) a.storeByte(RBX, slot(k) + kValTag, tagOf(t));
            }
            for (auto& [k, reg, t] : e.loads) {
                uint8_t tt = ti->at[e.target][(size_t)k];
                if (isX(reg)) a.movsdLoad(reg - 16, RBX, slot(k) + kValPayload);
                else if (tt == TY_BOOL) a.movzxByte(reg, RBX, slot(k) + kValPayload);
                else a.load(reg, RBX, slot(k) + kValPayload);
                (void)t;
            }
            a.jmp(insLabel[e.target]);
        }
    }
    // a slot about to receive a new value (whatever memory holds is not known)
    void fresh(int k) {
        unmap(k);
        memTy[(size_t)k] = TY_ANY;
    }
    void writeBack(int k) {
        Loc& l = loc[(size_t)k];
        if (l.mem >= 0) {  // a copy of a variable: now it gets its own
            int from = l.mem;
            l = Loc{};
            copySlot(from, k);
            memTy[(size_t)k] = memTy[(size_t)from];
            return;
        }
        if (l.reg < 0 || !l.dirty) return;
        if (isX(l.reg)) a.movsdStore(RBX, slot(k) + kValPayload, l.reg - 16);
        else a.store(RBX, slot(k) + kValPayload, l.reg);
        if (memTy[(size_t)k] != ty[(size_t)k]) {
            a.storeByte(RBX, slot(k) + kValTag, tagOf(ty[(size_t)k]));
            memTy[(size_t)k] = ty[(size_t)k];
        }
        l.dirty = false;
    }
    void flushBelow(int depth) {
        for (int k = 0; k < depth && k < (int)loc.size(); k++) writeBack(k);
    }
    void dropAll() {
        for (auto& l : loc) l = Loc{};
        for (int& o : owner) o = -1;
        for (auto& c : instCls) c = kUnknown;
        for (auto& t : thawed) t = 0;
        for (auto& l : listOk) l = 0;
    }
    // every slot from k up is dead
    void killFrom(int k) {
        for (int j = std::max(k, 0); j < (int)loc.size(); j++)
            if (loc[(size_t)j].alias >= 0) loc[(size_t)j] = Loc{};
        for (int j = std::max(k, 0); j < (int)loc.size(); j++) unmap(j);
    }
    int allocReg(bool fp) {
#ifdef IXO_JIT_X64
#ifdef _WIN32
        static const int gprs[] = {R10, R11, RSI, RDI, RBP, R15, R14, R13};
#else
        static const int gprs[] = {R10, R11, RSI, RDI, RBP, R15, R14, R13};
#endif
        const int ngpr = 8;
#else
        static const int gprs[] = {R10, R11, RSI, RDI, R15, R14, R13};
        const int ngpr = 7;
#endif
        static const int xmms[] = {16 + 2, 16 + 3, 16 + 4, 16 + 5, 16 + 6, 16 + 7, 16 + 8, 16 + 9,
                                   16 + 13, 16 + 12, 16 + 11, 16 + 10};
        const int* pool = fp ? xmms : gprs;
        int n = fp ? 12 : ngpr;
        int best = -1;
        uint32_t bestUse = UINT32_MAX;
        for (int j = 0; j < n; j++)
            if (owner[pool[j]] < 0 && !reserved[pool[j]]) {
                used |= 1u << pool[j];
                return pool[j];
            }
        for (int j = 0; j < n; j++) {
            int r = pool[j], k = owner[r];
            if (reserved[r] || k < 0) continue;
            if (!pinned(k) && loc[(size_t)k].used < bestUse) {
                bestUse = loc[(size_t)k].used;
                best = r;
            }
        }
        int victim = owner[best];
        writeBack(victim);
        dropReg(victim);
        used |= 1u << best;
        return best;
    }
    // an operand of the instruction being made (or the variable an operand shares)
    bool pinned(int k) const {
        for (int p : {pinA, pinB, pinC})
            if (p >= 0 && (p == k || loc[(size_t)p].alias == k)) return true;
        return false;
    }
    // ---- objects
    // The register that will hold the object in slot k (read from the variable it
    // copies): the variable's own when its class is known, else a new one.
    // Call before taking an exit snapshot; -1: can't.
    int objectIn(int k, uint64_t cls, size_t i) {
        (void)i;
        int v = memSlot(k);
        uint64_t want = cls == UINT64_MAX ? kAnyInstance : cls;
        bool var = v != k;
        pinA = v;
        if (var && instCls[(size_t)v] == want && loc[(size_t)v].ptr && loc[(size_t)v].reg >= 0) {
            loc[(size_t)v].used = ++clock;
            return loc[(size_t)v].reg;
        }
        if (var && loc[(size_t)v].reg >= 0) dropReg(v);  // an old pointer: checked again
        if (!var) return RAX;
        int r = allocReg(false);
        owner[r] = v;  // (taken now: later allocations must not hand it out again)
        return r;
    }
    // Checks (unless known) that slot k holds an instance of class cls (any class:
    // UINT64_MAX), loading its pointer into `obj`; remembers it for the variable.
    int checkObject(int k, uint64_t cls, int ex, int obj) {
        const Layout& L = layout();
        int v = memSlot(k);
        uint64_t want = cls == UINT64_MAX ? kAnyInstance : cls;
        bool var = v != k;
        if (var && instCls[(size_t)v] == want && loc[(size_t)v].ptr && loc[(size_t)v].reg == obj) return obj;
        if (var && instCls[(size_t)v] == want) {  // known: just the pointer
            a.load(obj, RBX, slot(v) + kValPayload);
        } else {
            a.cmpByte(RBX, slot(v) + kValTag, TAG_OBJ);
            a.jcc(CC_NE, ex);
            a.load(obj, RBX, slot(v) + kValPayload);
            a.cmpByte(obj, L.objType, (uint8_t)OT::Instance);
            a.jcc(CC_NE, ex);
            if (cls != UINT64_MAX) {
                a.load(RCX, obj, L.instCls);
                a.load(RCX, RCX, L.clsId);
                a.movImm(RDX, cls);
                a.cmpR(RCX, RDX);
                a.jcc(CC_NE, ex);
            }
        }
        if (var) {
            owner[obj] = v;
            Loc& l = loc[(size_t)v];
            l.reg = (int8_t)obj;
            l.dirty = false;
            l.ptr = true;
            l.used = ++clock;
            instCls[(size_t)v] = want;
        }
        return obj;
    }
    // ---- loop homes
    int homeOf(int k) const { return k >= 0 && k < (int)homeReg.size() ? homeReg[(size_t)k] : -1; }
    // the homes in force at join point j: slots whose type there is the home's
    std::vector<std::pair<int, int>> homesAt(size_t j) const {
        std::vector<std::pair<int, int>> out;
        int r = regionOf[j];
        if (r < 0) return out;
        for (auto& [k, reg] : regions[(size_t)r].map) {
            uint8_t t = ti->at[j][(size_t)k];
            uint8_t ht = ti->at[regions[(size_t)r].head][(size_t)k];
            if (k < an.ins[j].depth && t == ht) out.push_back({k, reg});
        }
        return out;
    }
    void loadHome(int k, int reg) {
        if (isX(reg)) a.movsdLoad(reg - 16, RBX, slot(k) + kValPayload);
        else if (T(k) == TY_BOOL) a.movzxByte(reg, RBX, slot(k) + kValPayload);
        else a.load(reg, RBX, slot(k) + kValPayload);
    }
    // Before going to instruction j (or falling into it): the state it expects.
    // Values stay where they are on this side too.
    void conformTo(size_t j, int depth) {
        int r = regionOf[j];
        if (r < 0 || r != curRegion) {
            flushBelow(depth);
            return;
        }
        auto hs = homesAt(j);
        for (int k = 0; k < depth && k < (int)loc.size(); k++) {
            bool home = false;
            for (auto& h : hs) home |= h.first == k;
            if (!home) writeBack(k);
        }
        for (auto& [k, reg] : hs)
            if (loc[(size_t)k].reg != reg) {
                if (loc[(size_t)k].reg >= 0) {  // (can't happen: a slot with a home only lives there)
                    writeBack(k);
                    unmap(k);
                }
                loadHome(k, reg);
                owner[reg] = k;
                loc[(size_t)k] = Loc{(int8_t)reg, false, ++clock};
            }
    }
    // the label code arriving with everything in memory uses for join point j
    int canonFor(size_t j) {
        if ((regionOf[j] < 0 || homesAt(j).empty()) && unprofiledLoop[j] < 0) return insLabel[j];
        if (canon[j] < 0) canon[j] = a.newLabel();
        return canon[j];
    }
    void enterRegion(int r) {
        curRegion = r;
        for (auto& h : homeReg) h = -1;
        for (bool& b : reserved) b = false;
        if (r < 0) return;
        for (auto& [k, reg] : regions[(size_t)r].map) {
            used |= 1u << reg;
            homeReg[(size_t)k] = reg;
            homeTy[(size_t)k] = ti->at[regions[(size_t)r].head][(size_t)k];
            reserved[reg] = true;
        }
    }
    void findRegions() {
        size_t n = an.ins.size();
        regionOf.assign(n, -1);
        canon.assign(n, -1);
        homeReg.assign((size_t)ti->slots, -1);
        homeTy.assign((size_t)ti->slots, TY_ANY);
        std::vector<std::pair<size_t, size_t>> loops;  // head, last LOOP to it
        for (size_t b = 0; b < n; b++) {
            const Ins& in = an.ins[b];
            if (in.op != OP_LOOP || in.target < 0 || in.depth < 0) continue;
            size_t h = an.at[(size_t)in.target];
            bool found = false;
            for (auto& l : loops)
                if (l.first == h) {
                    l.second = std::max(l.second, b);
                    found = true;
                }
            if (!found) loops.push_back({h, b});
        }
        for (auto& [h, b] : loops) {
            bool inner = true;  // no other loop inside
            for (auto& [h2, b2] : loops)
                if (h2 != h && h2 >= h && b2 <= b) inner = false;
            if (!inner || an.ins[h].depth < 0) continue;
            for (size_t i = h; i <= b; i++)
                if (regionOf[i] >= 0) inner = false;
            if (!inner) continue;
            Region rg;
            rg.head = h;
            rg.end = b;
            // the busiest typed variables there
            std::vector<std::pair<int, int>> uses;  // count, slot
            for (int k = 0; k < an.ins[h].depth && k < ti->slots; k++) {
                uint8_t t = ti->at[h][(size_t)k];
                if (!tyUnboxed(t) || ti->captured[(size_t)k]) continue;
                int c = 0;
                for (size_t i = h; i <= b; i++) {
                    const Ins& in = an.ins[i];
                    if (in.depth < 0) continue;
                    if ((in.op == OP_GET_LOCAL || in.op == OP_GET_LOCAL_CHK || in.op == OP_SET_LOCAL ||
                         in.op == OP_INC_LOCAL) && (int)in.a == k)
                        c++;
                    if (in.op == OP_FOR_RANGE && (k == (int)in.a || k == (int)in.a + 1 || k == (int)in.a + 2)) c += 2;
                }
                if (c > 0) uses.push_back({c, k});
            }
            std::sort(uses.begin(), uses.end(), [](auto& p, auto& q) { return p.first > q.first; });
            int gHomes[] = {R13, R14, R15}, xHomes[] = {16 + 10, 16 + 11, 16 + 12, 16 + 13};
            int ng = 0, nx = 0;
            for (auto& [c, k] : uses) {
                bool fp = ti->at[h][(size_t)k] == TY_FLOAT;
                if (fp && nx < 4) rg.map.push_back({k, xHomes[nx++]});
                else if (!fp && ng < 3) rg.map.push_back({k, gHomes[ng++]});
            }
            if (rg.map.empty()) continue;
            for (size_t i = h; i <= b; i++) regionOf[i] = (int)regions.size();
            regions.push_back(rg);
        }
    }
    // slot k receives the value in register r (free, type t): into its home if it has one
    void assignReg(int k, int r, uint8_t t) {
        int home = homeOf(k);
        if (home < 0) {
            define(k, r, t);
            return;
        }
        if (t == homeTy[(size_t)k]) {
            if (r != home) {
                if (isX(r)) a.movsdRR(home - 16, r - 16);
                else a.movR(home, r);
            }
            define(k, home, t);
            return;
        }
        // another type than its home's: through memory
        if (isX(r)) a.movsdStore(RBX, slot(k) + kValPayload, r - 16);
        else a.store(RBX, slot(k) + kValPayload, r);
        a.storeByte(RBX, slot(k) + kValTag, tagOf(t));
        unmap(k);
        ty[(size_t)k] = memTy[(size_t)k] = t;
    }

    // slot k (an int or bool) in a general register; a float in an xmm register (16 + k)
    int regOf(int k) {
        Loc& l = loc[(size_t)k];
        if (l.reg >= 0) {
            l.used = ++clock;
            return l.reg;
        }
        bool fp = T(k) == TY_FLOAT;
        int home = homeOf(k);
        int r = home >= 0 && isX(home) == fp ? home : allocReg(fp);
        used |= 1u << r;
        if (fp) a.movsdLoad(r - 16, RBX, slot(k) + kValPayload);
        else if (T(k) == TY_BOOL) a.movzxByte(r, RBX, slot(k) + kValPayload);
        else a.load(r, RBX, slot(k) + kValPayload);
        owner[r] = k;
        Loc& m = loc[(size_t)k];
        m.reg = (int8_t)r;
        m.dirty = false;
        m.used = ++clock;
        return r;
    }
    // slot k now holds register r (type t), not yet in memory
    void define(int k, int r, uint8_t t) {
        Loc& l = loc[(size_t)k];
        instCls[(size_t)k] = kUnknown;
        thawed[(size_t)k] = 0;
        listOk[(size_t)k] = 0;
        if (l.alias >= 0 || l.mem >= 0) {
            l = Loc{};
        } else {
            detachAliases(k);
            if (l.reg >= 0 && l.reg != r) owner[l.reg] = -1;
            l.ptr = false;
        }
        if (owner[r] >= 0 && owner[r] != k) loc[(size_t)owner[r]] = Loc{};
        l.reg = (int8_t)r;
        l.dirty = true;
        l.used = ++clock;
        owner[r] = k;
        ty[(size_t)k] = t;
    }
    // slot k as a double: its own register for a float; an int converted into xmm `scratch`
    int dblOf(int k, int scratch) {
        int r = regOf(k);
        if (isX(r)) return r - 16;
        a.cvtsi2sdRR(scratch, r);
        return scratch;
    }
    // Leaving for the interpreter at instruction `at`: the values only in
    // registers go to memory first (as they are right now), then the usual stop.
    int exitAt(size_t at, bool guess = true, int below = -1) {
        Cold c;
        c.label = a.newLabel();
        c.ins = at;
        c.guess = guess;
        int depth = below >= 0 ? below : an.ins[at].depth;
        for (int k = 0; k < depth && k < (int)loc.size(); k++) {
            const Loc& l = loc[(size_t)k];
            if (l.mem >= 0) c.spill.push_back({k, -2 - l.mem, ty[(size_t)k], memTy[(size_t)k]});
            else if (l.reg >= 0 && l.dirty) c.spill.push_back({k, l.reg, ty[(size_t)k], memTy[(size_t)k]});
        }
        colds.push_back(c);
        return c.label;
    }
    void emitColds() {
        const Layout& L = layout();
        (void)L;
        for (size_t n = 0; n < colds.size(); n++) {
            Cold c = colds[n];
            a.bind(c.label);
            for (auto& [k, r, t, mt] : c.spill) {
                if (r <= -2) {
                    copySlot(-2 - r, k);
                    continue;
                }
                if (isX(r)) a.movsdStore(RBX, slot(k) + kValPayload, r - 16);
                else a.store(RBX, slot(k) + kValPayload, r);
                if (mt != t) a.storeByte(RBX, slot(k) + kValTag, tagOf(t));
            }
            if (c.guess) {
                a.movImm(RAX, (uint64_t)(uintptr_t)&code->guessFails);
                a.incMem(RAX, 0);
            }
            a.jmp(bailAt(c.ins));
        }
    }
    // the state at the start of instruction i when everything is in memory
    void fromMemory(size_t i) {
        dropAll();
        const auto& at = ti->at[i];
        for (size_t k = 0; k < ty.size(); k++) {
            uint8_t t = k < at.size() && at[k] != TY_NONE ? at[k] : TY_ANY;
            ty[k] = memTy[k] = t;
        }
    }
    void loadConstInto(int k, const Value& v) {  // a constant that isn't unboxed, into memory
        if (v.t == VT::Float || v.t == VT::Obj) {
            uint64_t bits;
            std::memcpy(&bits, (const char*)&v + kValPayload, 8);
            a.movImm(RAX, bits);
            a.store(RBX, slot(k) + kValPayload, RAX);
            a.storeByte(RBX, slot(k) + kValTag, (uint8_t)v.t);
        } else {
            a.storeByte(RBX, slot(k) + kValTag, (uint8_t)v.t);
        }
    }
    // An instruction done with values in registers. False: do it the plain way.
    bool emitTyped(size_t i) {
        const Layout& L = layout();
        const Ins& in = an.ins[i];
        int d = in.depth;
        uint8_t x = d >= 2 ? T(d - 2) : TY_ANY, y = d >= 1 ? T(d - 1) : TY_ANY;
        pinA = pinB = pinC = -1;
        auto numOp = [&](uint8_t op) -> bool {  // + - * / on numbers
            if (!(tyNum(x) && tyNum(y))) return false;
            if (x == TY_INT && y == TY_INT && op != OP_DIV) {
                pinA = d - 2;
                pinB = d - 1;
                int r1 = ownReg(d - 2), r2 = regOf(d - 1);
                if (op == OP_ADD) a.addR(r1, r2);
                else if (op == OP_SUB) a.subR(r1, r2);
                else a.imulRR(r1, r2);
                define(d - 2, r1, TY_INT);
                unmap(d - 1);
                return true;
            }
            if (op == OP_DIV && x == TY_INT && y == TY_INT) {  // int / 0 panics: the interpreter says so
                pinA = d - 2;
                pinB = d - 1;
                int r2 = regOf(d - 1);
                regOf(d - 2);
                int ex = exitAt(i, false);
                a.testR(r2, r2);
                a.jcc(CC_E, ex);
            }
            pinA = d - 2;
            pinB = d - 1;
            int src = dblOf(d - 1, 1);  // first: the result's register must not be taken by it
            int dst;
            if (x == TY_FLOAT) {
                dst = ownReg(d - 2);
            } else {
                int ri = regOf(d - 2);
                dst = allocReg(true);
                a.cvtsi2sdRR(dst - 16, ri);
            }
            uint8_t sop = op == OP_ADD ? 0x58 : op == OP_SUB ? 0x5C : op == OP_MUL ? 0x59 : 0x5E;
            a.sdRR(sop, dst - 16, src);
            if (x != TY_FLOAT) unmap(d - 2);
            define(d - 2, dst, TY_FLOAT);
            unmap(d - 1);
            return true;
        };
        switch (in.op) {
            case OP_CONST: {
                Value v = fn->chunk.consts[in.a];
                fresh(d);
                if (v.t == VT::Int || v.t == VT::Bool) {
                    int r = allocReg(false);
                    a.movImm(r, v.t == VT::Int ? (uint64_t)v.i : (v.b ? 1 : 0));
                    define(d, r, v.t == VT::Int ? TY_INT : TY_BOOL);
                } else if (v.t == VT::Float) {
                    int r = allocReg(true);
                    uint64_t bits;
                    std::memcpy(&bits, &v.f, 8);
                    a.movImm(RAX, bits);
                    a.movqXG(r - 16, RAX);
                    define(d, r, TY_FLOAT);
                } else {
                    loadConstInto(d, v);
                    ty[(size_t)d] = memTy[(size_t)d] = v.t == VT::Nil ? TY_NIL : TY_DEF;
                }
                return true;
            }
            case OP_TRUE: case OP_FALSE: {
                fresh(d);
                int r = allocReg(false);
                a.movImm(r, in.op == OP_TRUE ? 1 : 0);
                define(d, r, TY_BOOL);
                return true;
            }
            case OP_NIL: case OP_UNDEF:
                fresh(d);
                a.storeByte(RBX, slot(d) + kValTag, in.op == OP_NIL ? TAG_NIL : TAG_UNDEF);
                ty[(size_t)d] = memTy[(size_t)d] = in.op == OP_NIL ? TY_NIL : TY_UNDEF;
                return true;
            case OP_POP: unmap(d - 1); return true;
            case OP_ENDSCOPE: killFrom(d - (int)in.a); return true;
            case OP_GET_LOCAL: case OP_GET_LOCAL_CHK: case OP_DUP: {
                int src = in.op == OP_DUP ? d - 1 : (int)in.a;
                uint8_t t = T(src);
                if (in.op == OP_GET_LOCAL_CHK && (t == TY_ANY || t == TY_UNDEF)) return false;  // maybe unset
                fresh(d);
                if (tyUnboxed(t)) {
                    pinA = src;
                    int r = regOf(src);
                    if (in.op != OP_DUP && loc[(size_t)src].alias < 0) {  // shares it until the variable changes
                        loc[(size_t)d] = Loc{(int8_t)r, true, ++clock, (int8_t)src};
                        ty[(size_t)d] = t;
                        return true;
                    }
                    int nr = allocReg(isX(r));
                    if (isX(r)) a.movsdRR(nr - 16, r - 16);
                    else a.movR(nr, r);
                    define(d, nr, t);
                } else if (in.op == OP_DUP && loc[(size_t)src].mem >= 0) {
                    loc[(size_t)d] = Loc{};
                    loc[(size_t)d].mem = loc[(size_t)src].mem;
                    ty[(size_t)d] = t;
                } else if (in.op != OP_DUP && !ti->captured[(size_t)src] && loc[(size_t)src].alias < 0 &&
                           loc[(size_t)src].mem < 0) {
                    loc[(size_t)d] = Loc{};  // no copy yet: its slot is read until the variable changes
                    loc[(size_t)d].mem = (int8_t)src;
                    ty[(size_t)d] = t;
                } else {
                    writeBack(src);
                    copySlot(src, d);
                    memTy[(size_t)d] = memTy[(size_t)src];
                    ty[(size_t)d] = t;
                }
                return true;
            }
            case OP_SET_LOCAL: case OP_ENDSCOPE_KEEP: {
                int k = in.op == OP_SET_LOCAL ? (int)in.a : d - 1 - (int)in.a;
                bool cap = in.op == OP_SET_LOCAL && ti->captured[(size_t)k];
                if (tyUnboxed(y) && !cap) {
                    int r = ownReg(d - 1);
                    loc[(size_t)d - 1] = Loc{};  // the value moves from the temporary to the variable
                    owner[r] = -1;
                    unmap(k);
                    assignReg(k, r, y);
                } else {
                    int from = memSlot(d - 1);
                    if (from == k) {  // x = x
                        unmap(d - 1);
                    } else {
                        if (from == d - 1) writeBack(d - 1);
                        uint64_t cls = from != d - 1 ? instCls[(size_t)from] : kUnknown;
                        uint8_t th = from != d - 1 ? thawed[(size_t)from] : 0;
                        uint8_t lk = from != d - 1 ? listOk[(size_t)from] : 0;
                        unmap(k);
                        copySlot(from, k);
                        memTy[(size_t)k] = memTy[(size_t)from];
                        ty[(size_t)k] = cap ? TY_ANY : y;
                        unmap(d - 1);
                        if (!cap) {
                            instCls[(size_t)k] = cls;
                            thawed[(size_t)k] = th;
                            listOk[(size_t)k] = lk;
                        }
                    }
                }
                if (in.op == OP_ENDSCOPE_KEEP) killFrom(d - (int)in.a);
                return true;
            }
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: return numOp(in.op);
            case OP_MOD: case OP_IDIV: {
                if (!(x == TY_INT && y == TY_INT)) return false;
                // by a positive constant (the instruction before): no division instruction
                const Ins* prev = i > 0 ? &an.ins[i - 1] : nullptr;
                if (prev && prev->op == OP_CONST && prev->depth == d - 1 && !ti->blockStart[i] &&
                    fn->chunk.consts[prev->a].t == VT::Int && fn->chunk.consts[prev->a].i > 0) {
                    int64_t c = fn->chunk.consts[prev->a].i;
                    unmap(d - 1);
                    pinA = d - 2;
                    int r1 = ownReg(d - 2);
                    bool mod = in.op == OP_MOD;
                    if (c == 1) {
                        if (mod) a.movImm(r1, 0);
                    } else if ((c & (c - 1)) == 0) {  // floor: a shift, and a mask
                        int k = 0;
                        while (((int64_t)1 << k) != c) k++;
                        if (mod) {
                            a.movImm(RAX, (uint64_t)(c - 1));
                            a.andR(r1, RAX);
                        } else {
                            a.sarImm(r1, (uint8_t)k);
                        }
                    } else {
                        DivMagic mg = divMagic(c);
                        a.movImm(RAX, (uint64_t)mg.m);
                        a.mulHigh(r1);  // rdx = high(m * x)
                        if (mg.m < 0) a.addR(RDX, r1);
                        if (mg.s) a.sarImm(RDX, (uint8_t)mg.s);
                        a.movR(RAX, r1);
                        a.shrImm(RAX, 63);
                        a.addR(RDX, RAX);  // x / c, toward zero
                        a.movImm(RCX, (uint64_t)c);
                        a.movR(RAX, RDX);
                        a.imulRR(RAX, RCX);
                        int ok = a.newLabel();
                        if (mod) {  // x - q*c, plus c when negative (floor)
                            a.subR(r1, RAX);
                            a.testR(r1, r1);
                            a.jcc(CC_NS, ok);
                            a.addR(r1, RCX);
                            a.bind(ok);
                        } else {
                            a.movR(R8, r1);
                            a.subR(R8, RAX);
                            a.testR(R8, R8);
                            a.jcc(CC_NS, ok);
                            a.subImm8(RDX, 1);
                            a.bind(ok);
                            a.movR(r1, RDX);
                        }
                    }
                    define(d - 2, r1, TY_INT);
                    return true;
                }
                pinA = d - 2;
                pinB = d - 1;
                int r1 = ownReg(d - 2), r2 = regOf(d - 1);
                int ex = exitAt(i, false);  // by zero: the interpreter reports it
                a.testR(r2, r2);
                a.jcc(CC_E, ex);
                int doDiv = a.newLabel(), store = a.newLabel(), done = a.newLabel();
                a.cmpImm32(r2, -1);
                a.jcc(CC_NE, doDiv);
                if (in.op == OP_MOD) a.movImm(r1, 0);
                else a.negR(r1);
                a.jmp(done);
                a.bind(doDiv);
                a.movR(RAX, r1);
                a.cqo();
                a.idivR(r2);  // rax = quotient, rdx = remainder (sign of the dividend)
                a.testR(RDX, RDX);
                a.jcc(CC_E, store);
                a.movR(R8, RDX);
                a.xorR(R8, r2);
                a.testR(R8, R8);
                a.jcc(CC_NS, store);
                if (in.op == OP_MOD) a.addR(RDX, r2);  // the remainder takes the divisor's sign
                else a.subImm8(RAX, 1);                // floor
                a.bind(store);
                a.movR(r1, in.op == OP_MOD ? RDX : RAX);
                a.bind(done);
                define(d - 2, r1, TY_INT);
                unmap(d - 1);
                return true;
            }
            case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
                if (!(tyNum(x) && tyNum(y))) return false;
                bool ints = x == TY_INT && y == TY_INT;
                if (!ints && (in.op == OP_EQ || in.op == OP_NE)) return false;  // NaN: the plain way
                pinA = d - 2;
                pinB = d - 1;
                if (ints) {
                    int r1 = ownReg(d - 2), r2 = regOf(d - 1);
                    int cc = in.op == OP_LT ? CC_L : in.op == OP_LE ? CC_LE : in.op == OP_GT ? CC_G
                           : in.op == OP_GE ? CC_GE : in.op == OP_EQ ? CC_E : CC_NE;
                    a.cmpR(r1, r2);
                    a.setccAl(cc);
                    a.movzxEaxAl();
                    a.movR(r1, RAX);
                    define(d - 2, r1, TY_BOOL);
                } else {
                    int p = dblOf(d - 2, 0), q = dblOf(d - 1, 1);
                    // a < b is b > a ("above": false when unordered), and so on
                    if (in.op == OP_LT || in.op == OP_LE) a.ucomisdRR(q, p);
                    else a.ucomisdRR(p, q);
                    a.setccAl(in.op == OP_LT || in.op == OP_GT ? CC_A : CC_AE);
                    a.movzxEaxAl();
                    unmap(d - 2);
                    int r = allocReg(false);
                    a.movR(r, RAX);
                    define(d - 2, r, TY_BOOL);
                }
                unmap(d - 1);
                return true;
            }
            case OP_NEG: {
                if (!tyNum(y)) return false;
                int r = ownReg(d - 1);
                if (isX(r)) {  // times -1.0: -0.0 and NaN come out as the interpreter's
                    a.movImm(RAX, 0xBFF0000000000000ull);
                    a.movqXG(1, RAX);
                    a.sdRR(0x59, r - 16, 1);
                } else {
                    a.negR(r);
                }
                define(d - 1, r, y);
                return true;
            }
            case OP_NOT: {
                if (y != TY_BOOL) return false;
                int r = ownReg(d - 1);
                a.negR(r);
                a.subImm8(r, -1);  // 1 - b
                define(d - 1, r, TY_BOOL);
                return true;
            }
            case OP_INC_LOCAL: {
                int k = (int)in.a;
                if (T(k) != TY_INT || ti->captured[(size_t)k] || (int8_t)(uint8_t)in.b == -128) return false;
                detachAliases(k);
                if (loc[(size_t)k].reg >= 0) {
                    int r = regOf(k);
                    a.subImm8(r, (int8_t)-(int8_t)(uint8_t)in.b);
                    define(k, r, TY_INT);
                } else {
                    a.addMemImm8(RBX, slot(k) + kValPayload, (int8_t)(uint8_t)in.b);
                }
                return true;
            }
            case OP_JUMP:
                conformTo(an.at[(size_t)in.target], d);
                a.jmp(insLabel[an.at[(size_t)in.target]]);
                live = 0;
                return true;
            case OP_LOOP: {
                conformTo(an.at[(size_t)in.target], d);
                int turn = exitAt(i, false);  // other tasks' turn: the interpreter switches
                a.decMem(R12, L.ctxBudget);
                int go = a.newLabel();
                a.jcc(CC_G, go);
                a.storeByte(R12, L.ctxBudgetExit, 1);
                a.jmp(turn);
                a.bind(go);
                a.jmp(insLabel[an.at[(size_t)in.target]]);
                live = 0;
                return true;
            }
            case OP_JUMP_IF_FALSE: case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP: {
                if (y != TY_BOOL) return false;
                int r = regOf(d - 1);
                int to = edgeTo(an.at[(size_t)in.target], in.op == OP_JUMP_IF_FALSE ? d - 1 : d);
                a.testR(r, r);
                a.jcc(in.op == OP_JUMP_IF_TRUE_KEEP ? CC_NE : CC_E, to);
                unmap(d - 1);
                return true;
            }
            case OP_JUMP_IF_NOT_CMP: {
                int cmp = in.a < 6 ? (int)in.a : 5;
                if (!(tyNum(x) && tyNum(y))) return false;
                bool ints = x == TY_INT && y == TY_INT;
                if (!ints && cmp >= 4) return false;
                pinA = d - 2;
                pinB = d - 1;
                if (ints) {
                    int r1 = regOf(d - 2), r2 = regOf(d - 1);
                    int target = edgeTo(an.at[(size_t)in.target], d - 2);
                    static const int inverse[] = {CC_GE, CC_G, CC_LE, CC_L, CC_NE, CC_E};
                    a.cmpR(r1, r2);
                    a.jcc(inverse[cmp], target);
                } else {
                    int p = dblOf(d - 2, 0), q = dblOf(d - 1, 1);
                    int target = edgeTo(an.at[(size_t)in.target], d - 2);
                    // jump unless a cmp b (unordered: never true, so it jumps)
                    if (cmp == 0 || cmp == 1) a.ucomisdRR(q, p);
                    else a.ucomisdRR(p, q);
                    a.jcc(cmp == 0 || cmp == 2 ? CC_BE : CC_B, target);
                }
                unmap(d - 2);
                unmap(d - 1);
                return true;
            }
            case OP_RETURN:
                writeBack(d - 1);
                copySlot(d - 1, 0);
                a.jmp(okLabel);
                live = 0;
                return true;
            case OP_STR_APPEND: {  // s += x on numbers: an addition
                int k = (int)in.a;
                uint8_t t = T(k);
                if (!(tyNum(t) && tyNum(y)) || ti->captured[(size_t)k]) return false;
                pinA = k;
                pinB = d - 1;
                detachAliases(k);
                if (t == TY_INT && y == TY_INT) {
                    int r = regOf(k), r2 = regOf(d - 1);
                    a.addR(r, r2);
                    define(k, r, TY_INT);
                } else if (t == TY_FLOAT) {
                    int r = regOf(k);
                    int src = dblOf(d - 1, 1);
                    a.sdRR(0x58, r - 16, src);
                    define(k, r, TY_FLOAT);
                } else {  // an int plus a float: a float now
                    int src = dblOf(d - 1, 1);
                    int ri = regOf(k);
                    int xr = allocReg(true);
                    a.cvtsi2sdRR(xr - 16, ri);
                    a.sdRR(0x58, xr - 16, src);
                    unmap(k);
                    assignReg(k, xr, TY_FLOAT);
                }
                unmap(d - 1);
                return true;
            }
            case OP_GET_UPVAL: {  // a captured variable: open (on a fiber's stack) or closed
                if (!L.vectorTriple || fn->upvalueCount == 0 || fn->methodStyle) return false;
                uint8_t t = i + 1 < an.ins.size() ? ti->at[i + 1][(size_t)d] : TY_ANY;
                fresh(d);
                int r = tyUnboxed(t) ? allocReg(t == TY_FLOAT) : -1;
                int ex = exitAt(i);
                int closed = a.newLabel(), have = a.newLabel();
                a.load(RAX, RBX, slot(0) + kValPayload);  // this closure
                a.load(RCX, RAX, L.closUpvals);
                a.load(RCX, RCX, (int32_t)(8 * in.a));     // its ObjUpvalue
                a.load(RDX, RCX, L.upFiber);
                a.testR(RDX, RDX);
                a.jcc(CC_E, closed);
                a.load(RAX, RDX, L.fiberStack);
                a.load(R8, RCX, L.upSlot);
                a.shlImm(R8, 4);
                a.addR(RAX, R8);
                a.jmp(have);
                a.bind(closed);
                a.lea(RAX, RCX, L.upClosed);
                a.bind(have);  // RAX = the value
                if (r >= 0) {
                    a.cmpByte(RAX, kValTag, tagOf(t));
                    guessCheck(ex);
                    if (isX(r)) a.movsdLoad(r - 16, RAX, kValPayload);
                    else if (t == TY_BOOL) a.movzxByte(r, RAX, kValPayload);
                    else a.load(r, RAX, kValPayload);
                    define(d, r, t);
                } else {
                    a.cmpByte(RAX, kValTag, TAG_UNDEF);
                    a.jcc(CC_E, ex);  // not set yet: the interpreter reports it
                    a.load(RCX, RAX, 0);
                    a.store(RBX, slot(d), RCX);
                    a.load(RCX, RAX, 8);
                    a.store(RBX, slot(d) + 8, RCX);
                    ty[(size_t)d] = memTy[(size_t)d] = TY_DEF;
                }
                return true;
            }
            case OP_SEQ_INIT: {  // a list: walked by position (slot d); anything else stops here
                int lst = memSlot(d - 1);
                int ex = exitAt(i);
                a.cmpByte(RBX, slot(lst) + kValTag, TAG_OBJ);
                guessCheck(ex);
                a.load(RAX, RBX, slot(lst) + kValPayload);
                a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                guessCheck(ex);
                fresh(d);
                int r = allocReg(false);
                a.movImm(r, 0);
                define(d, r, TY_INT);
                return true;
            }
            case OP_FOR_SEQ: {
                // the list in slot k, the position in k+1; the item (of the guessed type) goes on top
                int k = (int)in.a;
                uint8_t t = i + 1 < an.ins.size() ? ti->at[i + 1][(size_t)d] : TY_ANY;
                if (T(k + 1) != TY_INT || !tyUnboxed(t)) return false;
                ListKind want = t == TY_INT ? LK_INTS : t == TY_FLOAT ? LK_FLOATS : LK_BOOLS;
                int shift = t == TY_BOOL ? 0 : 3;
                pinA = k + 1;
                int rp = regOf(k + 1);
                fresh(d);
                int rv = allocReg(t == TY_FLOAT);
                int ex = exitAt(i);
                int end = edgeTo(an.at[(size_t)in.target], d);
                int lst = memSlot(k);
                a.cmpByte(RBX, slot(lst) + kValTag, TAG_OBJ);
                guessCheck(ex);
                a.load(RAX, RBX, slot(lst) + kValPayload);
                a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                guessCheck(ex);
                int values = a.newLabel(), done = a.newLabel();
                a.movzxByte(R9, RAX, L.objKind);
                a.cmpImm32(R9, want);
                a.jcc(CC_NE, values);
                a.movR(RCX, rp);
                listElement(shift, end);  // past the end: done
                if (isX(rv)) a.movsdLoad(rv - 16, RDX, 0);
                else if (t == TY_BOOL) a.movzxByte(rv, RDX, 0);
                else a.load(rv, RDX, 0);
                a.jmp(done);
                a.bind(values);
                a.cmpImm32(R9, LK_VALUES);
                guessCheck(ex);
                a.movR(RCX, rp);
                listElement(4, end);
                a.cmpByte(RDX, kValTag, tagOf(t));
                guessCheck(ex);
                if (isX(rv)) a.movsdLoad(rv - 16, RDX, kValPayload);
                else if (t == TY_BOOL) a.movzxByte(rv, RDX, kValPayload);
                else a.load(rv, RDX, kValPayload);
                a.bind(done);
                detachAliases(k + 1);
                a.subImm8(rp, -1);
                define(k + 1, rp, TY_INT);
                define(d, rv, t);
                return true;
            }
            case OP_INVOKE: {
                // xs.add(v) of an int, float or bool on a list with room (built-in methods come
                // first, so it's always the list's add); a full list goes to the interpreter
                if (in.a != 1 || !tyUnboxed(y) || !L.vectorTriple || !vm || VM::symName((int)in.sym) != "add") return false;
                int c = d - 2;
                if (tyUnboxed(T(c)) || T(c) == TY_NIL) return false;
                ListKind want = y == TY_INT ? LK_INTS : y == TY_FLOAT ? LK_FLOATS : LK_BOOLS;
                int bytes = y == TY_BOOL ? 1 : 8;
                pinA = d - 1;
                int rv = regOf(d - 1);
                int ex = exitAt(i, false);
                int lst = memSlot(c);
                a.cmpByte(RBX, slot(lst) + kValTag, TAG_OBJ);
                a.jcc(CC_NE, ex);
                a.load(RAX, RBX, slot(lst) + kValPayload);
                a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                a.jcc(CC_NE, ex);
                a.cmpByte(RAX, L.objFrozen, 0);
                a.jcc(CC_NE, ex);
                int values = a.newLabel(), counted = a.newLabel();
                a.movzxByte(R9, RAX, L.objKind);
                a.cmpImm32(R9, want);
                a.jcc(CC_NE, values);
                a.load(RDX, RAX, L.listItems + 8);   // end
                a.load(RCX, RAX, L.listItems + 16);  // end of capacity
                a.cmpR(RDX, RCX);
                a.jcc(CC_AE, ex);
                if (isX(rv)) {
                    a.movsdStore(RDX, 0, rv - 16);
                } else if (y == TY_BOOL) {
                    int zero = a.newLabel(), stored = a.newLabel();
                    a.testR(rv, rv);
                    a.jcc(CC_E, zero);
                    a.storeByte(RDX, 0, 1);
                    a.jmp(stored);
                    a.bind(zero);
                    a.storeByte(RDX, 0, 0);
                    a.bind(stored);
                } else {
                    a.store(RDX, 0, rv);
                }
                a.subImm8(RDX, (int8_t)-bytes);
                a.store(RAX, L.listItems + 8, RDX);
                a.movImm(R8, (uint64_t)bytes);
                a.jmp(counted);
                a.bind(values);  // a list of values: 16 bytes
                a.cmpImm32(R9, LK_VALUES);
                a.jcc(CC_NE, ex);
                a.load(RDX, RAX, L.listItems + 8);
                a.load(RCX, RAX, L.listItems + 16);
                a.cmpR(RDX, RCX);
                a.jcc(CC_AE, ex);
                if (isX(rv)) a.movsdStore(RDX, kValPayload, rv - 16);
                else a.store(RDX, kValPayload, rv);
                a.storeByte(RDX, kValTag, tagOf(y));
                a.subImm8(RDX, -16);
                a.store(RAX, L.listItems + 8, RDX);
                a.movImm(R8, 16);
                a.bind(counted);  // the list's size for the collector, as the helper does
                a.loadI32(RCX, RAX, L.objSize);
                a.addR(RCX, R8);
                a.store32(RAX, L.objSize, RCX);
                a.movImm(RDX, (uint64_t)(uintptr_t)&vm->bytesAllocated);
                a.load(RCX, RDX, 0);
                a.addR(RCX, R8);
                a.store(RDX, 0, RCX);
                a.movImm(RDX, (uint64_t)(uintptr_t)&vm->totalAllocated);
                a.load(RCX, RDX, 0);
                a.addR(RCX, R8);
                a.store(RDX, 0, RCX);
                unmap(d - 1);  // the result is the list, already in its slot
                return true;
            }
            case OP_FOR_RANGE: {
                // the counter, end and step (ints) in slots k, k+1, k+2
                int k = (int)in.a;
                bool incl = in.b & 1, one = in.b & 2;
                if (T(k) != TY_INT || T(k + 1) != TY_INT || (!one && T(k + 2) != TY_INT)) return false;
                pinA = k;
                pinB = k + 1;
                pinC = one ? -1 : k + 2;
                regOf(k);
                regOf(k + 1);
                if (!one) regOf(k + 2);
                size_t ex = an.at[(size_t)in.target];
                detachAliases(k);
                int rc = regOf(k), re = regOf(k + 1), rs = one ? -1 : regOf(k + 2);
                int exit = edgeTo(ex, d);
                if (one) {
                    a.cmpR(rc, re);
                    a.jcc(incl ? CC_G : CC_GE, exit);
                } else {
                    int neg = a.newLabel(), go = a.newLabel();
                    a.testR(rs, rs);
                    a.jcc(CC_S, neg);
                    a.cmpR(rc, re);
                    a.jcc(incl ? CC_G : CC_GE, exit);
                    a.jmp(go);
                    a.bind(neg);
                    a.cmpR(rc, re);
                    a.jcc(incl ? CC_L : CC_LE, exit);
                    a.bind(go);
                }
                fresh(d);
                int rv = allocReg(false);
                a.movR(rv, rc);
                define(d, rv, TY_INT);
                if (one) a.subImm8(rc, -1);
                else a.addR(rc, rs);
                define(k, rc, TY_INT);
                return true;
            }
            case OP_GET_MEMBER: case OP_GET_MEMBER_RAW: case OP_GET_FIELD: {
                int k = d - 1;
                if (in.op == OP_GET_MEMBER && tyNum(y) && VM::symName((int)in.sym) == "sqrt") {
                    pinA = k;
                    int src = dblOf(k, 1);
                    int r = allocReg(true);
                    a.sdRR(0x51, r - 16, src);
                    unmap(k);
                    define(k, r, TY_FLOAT);
                    return true;
                }
                if (tyUnboxed(y) || y == TY_NIL) return false;
                int32_t fo;
                uint64_t cls = UINT64_MAX;
                if (in.op == OP_GET_FIELD) {
                    fo = L.instFields + (int32_t)(16 * in.a);
                } else {
                    const InlineCache& ic = icOf(fn, in.b);
                    if (ic.clsId == UINT64_MAX || ic.index < 0) return false;
                    cls = ic.clsId;
                    fo = L.instFields + 16 * ic.index;
                }
                uint8_t t = i + 1 < an.ins.size() ? ti->at[i + 1][(size_t)k] : TY_ANY;
                int obj = objectIn(k, cls, i);
                if (obj < 0) return false;
                int r = tyUnboxed(t) ? allocReg(t == TY_FLOAT) : -1;
                int ex = exitAt(i);
                obj = checkObject(k, cls, ex, obj);
                if (r >= 0) {
                    a.cmpByte(obj, fo + kValTag, tagOf(t));
                    guessCheck(ex);
                    if (isX(r)) a.movsdLoad(r - 16, obj, fo + kValPayload);
                    else if (t == TY_BOOL) a.movzxByte(r, obj, fo + kValPayload);
                    else a.load(r, obj, fo + kValPayload);
                    unmap(k);
                    define(k, r, t);
                } else {
                    a.load(RCX, obj, fo);
                    a.load(RDX, obj, fo + 8);
                    unmap(k);
                    a.store(RBX, slot(k), RCX);
                    a.store(RBX, slot(k) + 8, RDX);
                    ty[(size_t)k] = memTy[(size_t)k] = TY_DEF;
                }
                pinA = pinB = pinC = -1;
                return true;
            }
            case OP_SET_MEMBER: case OP_SET_FIELD: {
                int k = d - 2;
                if (tyUnboxed(x) || x == TY_NIL) return false;
                int32_t fo;
                uint64_t cls = UINT64_MAX;
                if (in.op == OP_SET_FIELD) {
                    fo = L.instFields + (int32_t)(16 * in.a);
                } else {
                    const InlineCache& ic = icOf(fn, in.b);
                    if (ic.clsId == UINT64_MAX || ic.index < 0) return false;
                    cls = ic.clsId;
                    fo = L.instFields + 16 * ic.index;
                }
                pinB = d - 1;
                int rv = tyUnboxed(y) ? regOf(d - 1) : -1;
                if (rv < 0 && loc[(size_t)d - 1].mem < 0) writeBack(d - 1);
                int obj = objectIn(k, cls, i);
                if (obj < 0) return false;
                int ex = exitAt(i);
                obj = checkObject(k, cls, ex, obj);
                int v = memSlot(k);
                if (v == k || !thawed[(size_t)v]) {
                    a.cmpByte(obj, L.objFrozen, 0);
                    a.jcc(CC_NE, ex);
                    if (v != k) thawed[(size_t)v] = 1;
                }
                if (rv >= 0) {
                    if (isX(rv)) a.movsdStore(obj, fo + kValPayload, rv - 16);
                    else a.store(obj, fo + kValPayload, rv);
                    a.storeByte(obj, fo + kValTag, tagOf(y));
                } else {
                    int from = memSlot(d - 1);
                    a.load(RCX, RBX, slot(from));
                    a.store(obj, fo, RCX);
                    a.load(RCX, RBX, slot(from) + 8);
                    a.store(obj, fo + 8, RCX);
                }
                unmap(k);
                unmap(d - 1);
                pinA = pinB = pinC = -1;
                return true;
            }
            case OP_GET_INDEX: case OP_SET_INDEX: {
                bool get = in.op == OP_GET_INDEX;
                if (in.next < profileOf(fn).size() && (profileOf(fn)[in.next] & PB_NOTLIST)) return false;  // maps...
                int k = get ? d - 2 : d - 3;
                uint8_t t = get ? (i + 1 < an.ins.size() ? ti->at[i + 1][(size_t)k] : TY_ANY) : y;
                if (T(k + 1) != TY_INT || tyUnboxed(T(k)) || T(k) == TY_NIL) return false;
                if (!tyUnboxed(t)) {  // an item of a list of values (objects...): copied into the slot
                    if (!get || t == TY_NIL) return false;
                    pinA = k + 1;
                    int ri = regOf(k + 1);
                    int lst = memSlot(k);
                    int ex = exitAt(i);
                    if (lst != k && listOk[(size_t)lst]) {  // known to be a list: just its pointer
                        a.load(RAX, RBX, slot(lst) + kValPayload);
                    } else {
                        a.cmpByte(RBX, slot(lst) + kValTag, TAG_OBJ);
                        a.jcc(CC_NE, ex);
                        a.load(RAX, RBX, slot(lst) + kValPayload);
                        a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                        a.jcc(CC_NE, ex);
                        if (lst != k) listOk[(size_t)lst] = 1;
                    }
                    a.cmpByte(RAX, L.objKind, LK_VALUES);
                    a.jcc(CC_NE, ex);
                    a.movR(RCX, ri);
                    listElement(4, ex);
                    a.load(RCX, RDX, 0);
                    a.load(RAX, RDX, 8);
                    unmap(k + 1);
                    unmap(k);
                    a.store(RBX, slot(k), RCX);
                    a.store(RBX, slot(k) + 8, RAX);
                    ty[(size_t)k] = memTy[(size_t)k] = TY_DEF;
                    return true;
                }
                ListKind want = t == TY_INT ? LK_INTS : t == TY_FLOAT ? LK_FLOATS : LK_BOOLS;
                int shift = t == TY_BOOL ? 0 : 3;
                pinA = k + 1;
                pinB = get ? -1 : d - 1;
                int ri = regOf(k + 1);
                int rv = get ? allocReg(t == TY_FLOAT) : regOf(d - 1);
                int ex = exitAt(i);
                int lst = memSlot(k);
                bool var = lst != k;
                if (var && listOk[(size_t)lst]) {  // known to be a list: just its pointer
                    a.load(RAX, RBX, slot(lst) + kValPayload);
                } else {
                    a.cmpByte(RBX, slot(lst) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, ex);
                    a.load(RAX, RBX, slot(lst) + kValPayload);
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                    a.jcc(CC_NE, ex);
                    if (var) listOk[(size_t)lst] = 1;
                }
                if (!get && !(var && thawed[(size_t)lst])) {
                    a.cmpByte(RAX, L.objFrozen, 0);
                    a.jcc(CC_NE, ex);
                    if (var) thawed[(size_t)lst] = 1;
                }
                int values = a.newLabel(), done = a.newLabel();
                a.movzxByte(R9, RAX, L.objKind);
                a.cmpImm32(R9, want);
                a.jcc(CC_NE, values);
                a.movR(RCX, ri);
                listElement(shift, ex);  // RDX = the item
                if (get) {
                    if (isX(rv)) a.movsdLoad(rv - 16, RDX, 0);
                    else if (t == TY_BOOL) a.movzxByte(rv, RDX, 0);
                    else a.load(rv, RDX, 0);
                } else if (t == TY_BOOL) {
                    int zero = a.newLabel();
                    a.testR(rv, rv);
                    a.jcc(CC_E, zero);
                    a.storeByte(RDX, 0, 1);
                    a.jmp(done);
                    a.bind(zero);
                    a.storeByte(RDX, 0, 0);
                } else if (isX(rv)) {
                    a.movsdStore(RDX, 0, rv - 16);
                } else {
                    a.store(RDX, 0, rv);
                }
                a.jmp(done);
                a.bind(values);  // a list of values: the item must be of that type (when reading)
                a.cmpImm32(R9, LK_VALUES);
                a.jcc(CC_NE, ex);
                a.movR(RCX, ri);
                listElement(4, ex);
                if (get) {
                    a.cmpByte(RDX, kValTag, tagOf(t));
                    guessCheck(ex);
                    if (isX(rv)) a.movsdLoad(rv - 16, RDX, kValPayload);
                    else if (t == TY_BOOL) a.movzxByte(rv, RDX, kValPayload);
                    else a.load(rv, RDX, kValPayload);
                } else {
                    if (isX(rv)) a.movsdStore(RDX, kValPayload, rv - 16);
                    else a.store(RDX, kValPayload, rv);
                    a.storeByte(RDX, kValTag, tagOf(t));
                }
                a.bind(done);
                if (get) {
                    unmap(k + 1);
                    unmap(k);
                    define(k, rv, t);
                } else {
                    killFrom(k);
                }
                return true;
            }
            default: return false;
        }
    }

    bool run(JitCode& out, std::string* why) {
        const Layout& L = layout();
        code = &out;
        insLabel.resize(an.ins.size());
        for (auto& l : insLabel) l = a.newLabel();
        entryLabel = a.newLabel();
        okLabel = a.newLabel();
        bailCommon = a.newLabel();
        epilogue = a.newLabel();
        // frame: shadow space for helper calls, then saved call arguments; keep rsp 16-aligned
        frameSize = 32 + 16 * std::max(1, fn->arity);
        frameSize = (frameSize + 15) / 16 * 16 + 8;
        if (t2) {  // room to save the float registers the caller keeps
            xmmSave = frameSize - 8;
            frameSize += 128;
            for (auto& row : ti->at)
                for (uint8_t t : row) saveFp |= t == TY_FLOAT;
            findRegions();
            loc.assign((size_t)ti->slots, Loc{});
            instCls.assign((size_t)ti->slots, kUnknown);
            thawed.assign((size_t)ti->slots, 0);
            listOk.assign((size_t)ti->slots, 0);
            // loops whose values were never seen (compiled before the interpreter got there)
            unprofiledLoop.assign(an.ins.size(), -1);
            if (!profileOf(fn).empty()) {
                int count = 0;
                for (size_t b = 0; b < an.ins.size(); b++) {
                    const Ins& lp = an.ins[b];
                    if (lp.op != OP_LOOP || lp.target < 0 || lp.depth < 0) continue;
                    size_t h = an.at[(size_t)lp.target];
                    if (unprofiledLoop[h] >= 0) continue;
                    bool seed = false;
                    for (auto& sd : seedsOf(fn)) seed |= sd.first == (uint32_t)lp.target;
                    bool blind = false;
                    for (size_t j = h; j <= b && !seed && !blind; j++) {
                        const Ins& p = an.ins[j];
                        bool producer = p.op == OP_GET_FIELD || p.op == OP_GET_MEMBER || p.op == OP_GET_MEMBER_RAW ||
                                        p.op == OP_GET_INDEX || p.op == OP_CALL || p.op == OP_INVOKE ||
                                        p.op == OP_GET_GLOBAL || p.op == OP_FOR_SEQ;
                        if (producer && p.depth >= 0 && p.next < profileOf(fn).size() && profileOf(fn)[p.next] == 0)
                            blind = true;
                    }
                    if (blind) unprofiledLoop[h] = count++;
                }
                if (count) {
                    out.learned.reset(new uint8_t[(size_t)count]);
                    std::memset(out.learned.get(), 0, (size_t)count);
                }
            }
            ty.assign((size_t)ti->slots, TY_ANY);
            memTy.assign((size_t)ti->slots, TY_ANY);
            for (int& o : owner) o = -1;
            out.tier2 = true;
        }
        a.bind(entryLabel);
        a.enter();
        a.push(RBX);
        a.push(R12);
        // the typed tier's registers that belong to the caller (an even count keeps rsp aligned)
        std::vector<int> pushed;
        if (t2) {
            static const int keep[] = {R13, R14, R15, RSI, RDI, RBP};
            for (int r : keep) {
#if !defined(_WIN32) && defined(IXO_JIT_X64)
                if (r == RSI || r == RDI) continue;  // the caller's to save here
#endif
                if (saves(r)) pushed.push_back(r);
            }
            if (pushed.size() % 2) pushed.push_back(RCX);  // (padding)
            for (int r : pushed) a.push(r);
        }
        a.subRsp(frameSize);
        if (t2 && saveFp) {
#if defined(IXO_JIT_X64) && defined(_WIN32)
            for (int x = 6; x <= 13; x++)
                if (saves(16 + x)) a.movupsStore(RSP, xmmSave + 16 * (x - 6), x);
#elif defined(IXO_JIT_ARM64)
            for (int x = 8; x <= 13; x++)
                if (saves(16 + x)) a.movsdStore(RSP, xmmSave + 16 * (x - 8), x);
#endif
        }
        a.movR(RBX, ARG0);
        a.movR(R12, ARG1);
        // OSR entries
        for (size_t k = 0; k < an.loopHeaders.size(); k++) {
            a.movR(RAX, ARG2);
            a.cmpImm32(RAX, (int32_t)(k + 1));
            if (t2) {  // the types the code assumes there are checked first
                osrLabel.push_back(a.newLabel());
                a.jcc(CC_E, osrLabel.back());
            } else {
                a.jcc(CC_E, insLabel[an.at[an.loopHeaders[k]]]);
            }
        }
        if (t2)  // parameters of the guessed types
            for (int k = 1; k <= fn->arity; k++)
                if (tyUnboxed(ti->params[(size_t)k])) {
                    a.cmpByte(RBX, slot(k) + kValTag, tagOf(ti->params[(size_t)k]));
                    guessCheck(exitAt(0));
                }
        bool lastPlain = true;  // the previous instruction was done the plain way (all in memory)
        int32_t cnt = 0;
        auto& pcMap = out.pcMap;
        pcMap.push_back({0, 0});
        for (size_t i = 0; i < an.ins.size(); i++) {
            const Ins& in = an.ins[i];
            for (auto& [j, l] : relabeled) insLabel[j] = l;
            relabeled.clear();
            if (t2 && in.depth >= 0 && ti->blockStart[i]) {
                // control flow joins here: everything in memory, except a loop's homes
                int r = regionOf[i];
                if (unprofiledLoop[i] >= 0 && live > 0) {
                    // the first time into this loop: the interpreter runs it, learning its types
                    int ex = exitAt(i, false, live);
                    a.movImm(RAX, (uint64_t)(uintptr_t)(code->learned.get() + unprofiledLoop[i]));
                    int seen = a.newLabel();
                    a.cmpByte(RAX, 0, 0);
                    a.jcc(CC_NE, seen);
                    a.storeByte(RAX, 0, 1);
                    a.movImm(RAX, (uint64_t)(uintptr_t)&code->relearn);
                    a.storeByte(RAX, 0, 1);
                    a.jmp(ex);
                    a.bind(seen);
                }
                if (r >= 0 && r == curRegion) {
                    conformTo(i, live);
                } else {
                    flushBelow(live);
                    dropAll();
                    enterRegion(r);
                    if (r >= 0) conformTo(i, 0);
                }
            }
            a.bind(insLabel[i]);
            int d = in.depth;
            if (d < 0) continue;  // unreachable
            pcMap.push_back({(uint32_t)a.b.size(), (uint32_t)in.off});
            cnt++;
            if (t2) {
                if (ti->blockStart[i] || lastPlain) fromMemory(i);
                if (ti->blockStart[i])  // the homes hold the newest values (memory may be behind)
                    for (auto& [k, reg] : homesAt(i)) {
                        owner[reg] = k;
                        loc[(size_t)k] = Loc{(int8_t)reg, true, ++clock};
                        memTy[(size_t)k] = TY_ANY;
                    }
                if (lastPlain && i > 0 && ti->guess[i - 1] >= 0 && !ti->blockStart[i]) {
                    // the value the previous instruction made is of the type it had before
                    int g = ti->guess[i - 1];
                    uint8_t t = ty[(size_t)g];
                    if (tyUnboxed(t)) {
                        a.cmpByte(RBX, slot(g) + kValTag, tagOf(t));
                        guessCheck(exitAt(i));
                    }
                }
                live = in.fall >= 0 ? in.fall : 0;
                if (!in.exitOnly && (!in.step || in.op == OP_STR_APPEND || in.op == OP_GET_UPVAL) && emitTyped(i)) {
                    lastPlain = false;
                    continue;
                }
                // the plain way: every value in its slot, no registers kept
                flushBelow(d);
                dropAll();
                lastPlain = true;
                // its jumps to this loop's join points go through their loading stubs
                if (curRegion >= 0)
                    for (size_t j = regions[(size_t)curRegion].head; j <= regions[(size_t)curRegion].end; j++)
                        if (ti->blockStart[j] && an.ins[j].depth >= 0) {
                            int c = canonFor(j);
                            if (c != insLabel[j]) {
                                relabeled.push_back({j, insLabel[j]});
                                insLabel[j] = c;
                            }
                        }
            }
            if (in.exitOnly) {
                a.jmp(bailAt(i));
                continue;
            }
            if (in.step && in.op == OP_STR_APPEND) {  // ints add here; strings grow in the helper
                int k = (int)in.a;
                intOr(k, stepAt(i));
                intOr(d - 1, stepAt(i));
                a.load(RAX, RBX, slot(d - 1) + kValPayload);
                a.aluLoad(0x03, RAX, RBX, slot(k) + kValPayload);
                a.store(RBX, slot(k) + kValPayload, RAX);
                continue;
            }
            if (in.step) {
                a.jmp(stepAt(i));
                continue;
            }
            switch (in.op) {
                case OP_CONST: {
                    Value v = fn->chunk.consts[in.a];
                    if (v.t == VT::Int) {
                        a.movImm(RAX, (uint64_t)v.i);
                        storeInt(d, RAX);
                    } else if (v.t == VT::Float || v.t == VT::Obj) {
                        // constants live as long as the function: the pointer stays valid
                        uint64_t bits;
                        std::memcpy(&bits, (const char*)&v + kValPayload, 8);
                        a.movImm(RAX, bits);
                        a.store(RBX, slot(d) + kValPayload, RAX);
                        a.storeByte(RBX, slot(d) + kValTag, (uint8_t)v.t);
                    } else if (v.t == VT::Bool) {
                        a.movImm(RAX, v.b ? 1 : 0);
                        a.store(RBX, slot(d) + kValPayload, RAX);
                        a.storeByte(RBX, slot(d) + kValTag, TAG_BOOL);
                    } else {
                        a.storeByte(RBX, slot(d) + kValTag, TAG_NIL);
                    }
                    break;
                }
                case OP_NIL: a.storeByte(RBX, slot(d) + kValTag, TAG_NIL); break;
                case OP_UNDEF: a.storeByte(RBX, slot(d) + kValTag, TAG_UNDEF); break;
                case OP_TRUE:
                case OP_FALSE:
                    a.movImm(RAX, in.op == OP_TRUE ? 1 : 0);
                    a.store(RBX, slot(d) + kValPayload, RAX);
                    a.storeByte(RBX, slot(d) + kValTag, TAG_BOOL);
                    break;
                case OP_POP: break;
                case OP_DUP: copySlot(d - 1, d); break;
                case OP_GET_LOCAL: copySlot((int)in.a, d); break;
                case OP_GET_LOCAL_CHK:
                    a.cmpByte(RBX, slot((int)in.a) + kValTag, TAG_UNDEF);
                    a.jcc(CC_E, stepAt(i));  // it reports the unassigned variable
                    copySlot((int)in.a, d);
                    break;
                case OP_SET_LOCAL: copySlot(d - 1, (int)in.a); break;
                case OP_INC_LOCAL:
                    a.cmpByte(RBX, slot((int)in.a) + kValTag, TAG_INT);
                    a.jcc(CC_NE, stepAt(i));
                    a.addMemImm8(RBX, slot((int)in.a) + kValPayload, (int8_t)(uint8_t)in.b);
                    break;
                case OP_ADD:
                case OP_SUB:
                case OP_MUL:
                case OP_DIV: {
                    int cont = a.newLabel(), slow = slowFor(i, d, in.op, cont), floats = a.newLabel();
                    if (in.op != OP_DIV) {  // int op int stays an int
                        intOr(d - 2, floats);
                        intOr(d - 1, floats);
                        a.load(RAX, RBX, slot(d - 2) + kValPayload);
                        if (in.op == OP_ADD) a.aluLoad(0x03, RAX, RBX, slot(d - 1) + kValPayload);
                        else if (in.op == OP_SUB) a.aluLoad(0x2B, RAX, RBX, slot(d - 1) + kValPayload);
                        else a.imulLoad(RAX, RBX, slot(d - 1) + kValPayload);
                        a.store(RBX, slot(d - 2) + kValPayload, RAX);
                        a.jmp(cont);
                    } else {  // int / 0 panics: the interpreter does that
                        int notZeroInt = a.newLabel();
                        a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_INT);
                        a.jcc(CC_NE, notZeroInt);
                        a.cmpByte(RBX, slot(d - 2) + kValTag, TAG_INT);
                        a.jcc(CC_NE, notZeroInt);
                        a.load(RAX, RBX, slot(d - 1) + kValPayload);
                        a.testR(RAX, RAX);
                        a.jcc(CC_E, slow);
                        a.bind(notZeroInt);
                    }
                    a.bind(floats);  // numbers as doubles
                    loadNum(0, d - 2, slow);
                    loadNum(1, d - 1, slow);
                    a.sdRR(in.op == OP_ADD ? 0x58 : in.op == OP_SUB ? 0x5C : in.op == OP_MUL ? 0x59 : 0x5E, 0, 1);
                    a.movsdStore(RBX, slot(d - 2) + kValPayload, 0);
                    a.storeByte(RBX, slot(d - 2) + kValTag, TAG_FLOAT);
                    a.bind(cont);
                    break;
                }
                case OP_MOD:
                case OP_IDIV: {
                    int cont0 = a.newLabel(), slow0 = slowFor(i, d, in.op, cont0);
                    intOr(d - 2, slow0);
                    intOr(d - 1, slow0);
                    int doDiv = a.newLabel(), store = a.newLabel(), done = a.newLabel();
                    a.load(RCX, RBX, slot(d - 1) + kValPayload);
                    a.testR(RCX, RCX);
                    a.jcc(CC_E, stepAt(i));  // division by zero: the interpreter reports it
                    a.cmpImm32(RCX, -1);
                    a.jcc(CC_NE, doDiv);
                    if (in.op == OP_MOD) {
                        a.xorEaxEax();
                        a.store(RBX, slot(d - 2) + kValPayload, RAX);
                    } else {
                        a.negMem(RBX, slot(d - 2) + kValPayload);
                    }
                    a.jmp(done);
                    a.bind(doDiv);
                    a.load(RAX, RBX, slot(d - 2) + kValPayload);
                    a.cqo();
                    a.idivR(RCX);  // rax = quotient, rdx = remainder (sign of the dividend)
                    a.testR(RDX, RDX);
                    a.jcc(CC_E, store);
                    a.movR(R8, RDX);
                    a.xorR(R8, RCX);
                    a.testR(R8, R8);  // (AArch64: eor sets no flags)
                    a.jcc(CC_NS, store);
                    if (in.op == OP_MOD)
                        a.addR(RDX, RCX);  // remainder takes the divisor's sign
                    else
                        a.subImm8(RAX, 1);  // floor
                    a.bind(store);
                    a.store(RBX, slot(d - 2) + kValPayload, in.op == OP_MOD ? RDX : RAX);
                    a.bind(done);
                    a.bind(cont0);
                    break;
                }
                case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
                    int cont = a.newLabel(), slow = slowFor(i, d, in.op, cont);
                    bool ordered = in.op == OP_LT || in.op == OP_LE || in.op == OP_GT || in.op == OP_GE;
                    int floats = ordered ? a.newLabel() : slow;
                    intOr(d - 2, floats);
                    intOr(d - 1, floats);
                    int cc = in.op == OP_LT ? CC_L : in.op == OP_LE ? CC_LE : in.op == OP_GT ? CC_G
                           : in.op == OP_GE ? CC_GE : in.op == OP_EQ ? CC_E : CC_NE;
                    a.load(RAX, RBX, slot(d - 2) + kValPayload);
                    a.aluLoad(0x3B, RAX, RBX, slot(d - 1) + kValPayload);
                    a.setccAl(cc);
                    storeBoolFromAl(d - 2);
                    if (ordered) {
                        a.jmp(cont);
                        a.bind(floats);
                        loadNum(0, d - 2, slow);
                        loadNum(1, d - 1, slow);
                        // a < b is b > a ("above": false when unordered), and so on
                        if (in.op == OP_LT || in.op == OP_LE) a.ucomisdRR(1, 0);
                        else a.ucomisdRR(0, 1);
                        a.setccAl(in.op == OP_LT || in.op == OP_GT ? CC_A : CC_AE);
                        storeBoolFromAl(d - 2);
                    }
                    a.bind(cont);
                    break;
                }
                case OP_NEG: {
                    int cont = a.newLabel(), slow = slowFor(i, d, in.op, cont);
                    intOr(d - 1, slow);
                    a.negMem(RBX, slot(d - 1) + kValPayload);
                    a.bind(cont);
                    break;
                }
                case OP_GET_FIELD: case OP_SET_FIELD: {
                    // self.x in a method: a fixed slot of the instance
                    int k = in.op == OP_GET_FIELD ? d - 1 : d - 2;
                    a.cmpByte(RBX, slot(k) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, stepAt(i));
                    a.load(RAX, RBX, slot(k) + kValPayload);
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::Instance);
                    a.jcc(CC_NE, stepAt(i));
                    a.lea(RCX, RAX, L.instFields);  // the fields
                    int32_t f16 = (int32_t)(16 * in.a);
                    if (in.op == OP_GET_FIELD) {
                        a.load(RDX, RCX, f16);
                        a.store(RBX, slot(k), RDX);
                        a.load(RDX, RCX, f16 + 8);
                        a.store(RBX, slot(k) + 8, RDX);
                    } else {
                        a.cmpByte(RAX, L.objFrozen, 0);
                        a.jcc(CC_NE, stepAt(i));
                        a.load(RDX, RBX, slot(d - 1));
                        a.store(RCX, f16, RDX);
                        a.load(RDX, RBX, slot(d - 1) + 8);
                        a.store(RCX, f16 + 8, RDX);
                    }
                    break;
                }
                case OP_GET_MEMBER: case OP_GET_MEMBER_RAW: case OP_SET_MEMBER: {
                    // a field through the inline cache (filled by the interpreter); anything else stops here
                    int k = in.op == OP_SET_MEMBER ? d - 2 : d - 1;
                    if (in.op == OP_GET_MEMBER) {
                        // x.sqrt and x.abs on numbers (built-in methods come first, so these are always them)
                        const std::string& name = VM::symName((int)in.sym);
                        int other = a.newLabel();
                        if (name == "sqrt") {
                            loadNum(0, k, other);
                            a.sdRR(0x51, 0, 0);
                            a.movsdStore(RBX, slot(k) + kValPayload, 0);
                            a.storeByte(RBX, slot(k) + kValTag, TAG_FLOAT);
                            a.jmp(insLabel[i + 1]);
                        } else if (name == "len") {
                            // a list's or a map's count (strings count characters: the helper does those)
                            int notList = a.newLabel();
                            a.cmpByte(RBX, slot(k) + kValTag, TAG_OBJ);
                            a.jcc(CC_NE, other);
                            a.load(RAX, RBX, slot(k) + kValPayload);
                            a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                            a.jcc(CC_NE, notList);
                            a.load(RCX, RAX, L.listItems + 8);
                            a.load(RDX, RAX, L.listItems);
                            a.subR(RCX, RDX);  // bytes: shifted by the kind's item size
                            int lb = a.newLabel(), lw = a.newLabel(), lset = a.newLabel();
                            listKind(lb, lw, lw);
                            a.shrImm(RCX, 4);
                            a.jmp(lset);
                            a.bind(lw);
                            a.shrImm(RCX, 3);
                            a.bind(lb);
                            a.bind(lset);
                            storeInt(k, RCX);
                            a.jmp(insLabel[i + 1]);
                            a.bind(notList);
                            a.cmpByte(RAX, L.objType, (uint8_t)OT::Map);
                            a.jcc(CC_NE, other);
                            a.load(RCX, RAX, L.mapLive);
                            storeInt(k, RCX);
                            a.jmp(insLabel[i + 1]);
                        } else if (name == "abs") {
                            int pos = a.newLabel();
                            intOr(k, other);
                            a.load(RAX, RBX, slot(k) + kValPayload);
                            a.testR(RAX, RAX);
                            a.jcc(CC_NS, pos);
                            a.negMem(RBX, slot(k) + kValPayload);
                            a.bind(pos);
                            a.jmp(insLabel[i + 1]);
                        }
                        a.bind(other);
                    }
                    InlineCache* ic = &fn->ic[in.b];
                    int viaHelper = a.newLabel(), after = a.newLabel();
                    CallSite* site = in.op == OP_GET_MEMBER ? siteFor(in, 0) : nullptr;
                    int notInst = in.op == OP_GET_MEMBER ? viaHelper : stepAt(i);
                    a.cmpByte(RBX, slot(k) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, notInst);
                    a.load(RAX, RBX, slot(k) + kValPayload);
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::Instance);
                    a.jcc(CC_NE, notInst);
                    a.load(RCX, RAX, L.instCls);
                    a.load(RCX, RCX, L.clsId);
                    a.movImm(RDX, (uint64_t)(uintptr_t)ic);
                    a.aluLoad(0x3B, RCX, RDX, L.icClsId);
                    a.jcc(CC_NE, stepAt(i));
                    a.loadI32(RCX, RDX, L.icIndex);
                    a.testR(RCX, RCX);
                    if (in.op == OP_GET_MEMBER) {
                        // a field: read it; a cached method without parameters (obj.alive) or a
                        // function used as a method (x.f): the helper runs its machine code
                        int field = a.newLabel();
                        a.jcc(CC_NS, field);
                        a.bind(viaHelper);
                        a.movR(ARG0, R12);
                        a.lea(ARG1, RBX, slot(k));
                        a.movImm(ARG2, (uint64_t)(uintptr_t)site);
                        a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitInvoke);
                        a.callR(RAX);
                        afterCall(i, after, in);
                        a.bind(field);
                        a.shlImm(RCX, 4);
                        a.lea(RDX, RAX, L.instFields);
                        a.addR(RDX, RCX);
                        a.load(RCX, RDX, 0);
                        a.store(RBX, slot(k), RCX);
                        a.load(RCX, RDX, 8);
                        a.store(RBX, slot(k) + 8, RCX);
                        a.bind(after);
                        break;
                    }
                    (void)viaHelper;
                    a.bind(viaHelper);
                    a.bind(after);
                    a.jcc(CC_S, stepAt(i));  // a method: the interpreter calls it
                    a.shlImm(RCX, 4);
                    if (in.op == OP_SET_MEMBER) {
                        a.cmpByte(RAX, L.objFrozen, 0);
                        a.jcc(CC_NE, stepAt(i));
                    }
                    a.lea(RDX, RAX, L.instFields);
                    a.addR(RDX, RCX);
                    if (in.op != OP_SET_MEMBER) {  // GET_MEMBER_RAW: a field (a method stops above)
                        a.load(RCX, RDX, 0);
                        a.store(RBX, slot(k), RCX);
                        a.load(RCX, RDX, 8);
                        a.store(RBX, slot(k) + 8, RCX);
                    } else {
                        a.load(RCX, RBX, slot(d - 1));
                        a.store(RDX, 0, RCX);
                        a.load(RCX, RBX, slot(d - 1) + 8);
                        a.store(RDX, 8, RCX);
                    }
                    break;
                }
                case OP_GET_INDEX: case OP_SET_INDEX: {
                    // xs[i] on a list with an int index in range (negative counts from the end)
                    int k = in.op == OP_GET_INDEX ? d - 2 : d - 3;
                    a.cmpByte(RBX, slot(k) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, stepAt(i));
                    a.load(RAX, RBX, slot(k) + kValPayload);
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                    a.jcc(CC_NE, stepAt(i));
                    if (in.op == OP_SET_INDEX) {
                        a.cmpByte(RAX, L.objFrozen, 0);
                        a.jcc(CC_NE, stepAt(i));
                    }
                    a.cmpByte(RBX, slot(k + 1) + kValTag, TAG_INT);
                    a.jcc(CC_NE, stepAt(i));
                    // per kind: values (16 bytes), ints and floats (8), bools (1)
                    int kb = a.newLabel(), ki = a.newLabel(), kf = a.newLabel(), done = a.newLabel();
                    listKind(kb, ki, kf);
                    struct K {
                        int label;
                        ListKind lk;
                        int shift;
                    };
                    K kinds[] = {{-1, LK_VALUES, 4}, {ki, LK_INTS, 3}, {kf, LK_FLOATS, 3}, {kb, LK_BOOLS, 0}};
                    for (auto& kk : kinds) {
                        if (kk.label >= 0) a.bind(kk.label);
                        a.load(RCX, RBX, slot(k + 1) + kValPayload);  // the index
                        listElement(kk.shift, stepAt(i));
                        if (in.op == OP_GET_INDEX) readElement(k, kk.lk);
                        else writeElement(d - 1, kk.lk, stepAt(i));  // a value that doesn't fit: the interpreter widens
                        a.jmp(done);
                    }
                    a.bind(done);
                    break;
                }
                case OP_INVOKE: {
                    // obj.method(args): the cached method's machine code
                    int argc = (int)in.a;
                    int c = d - argc - 1;
                    int ok = a.newLabel();
                    if (argc == 1 && L.vectorTriple && vm && VM::symName((int)in.sym) == "add") {
                        // xs.add(v) on a list with room: store and count it here (built-in methods come first,
                        // so this is always the list's own add); the helper grows the list
                        int helper = a.newLabel();
                        a.cmpByte(RBX, slot(c) + kValTag, TAG_OBJ);
                        a.jcc(CC_NE, helper);
                        a.load(RAX, RBX, slot(c) + kValPayload);
                        a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                        a.jcc(CC_NE, helper);
                        a.cmpByte(RAX, L.objFrozen, 0);
                        a.jcc(CC_NE, helper);
                        int kb = a.newLabel(), ki = a.newLabel(), kf = a.newLabel();
                        a.movzxByte(R9, RAX, L.objKind);
                        a.cmpImm32(R9, LK_EMPTY);
                        a.jcc(CC_E, helper);  // the first item picks the kind: the helper does that
                        listKind(kb, ki, kf);
                        struct K {
                            int label;
                            ListKind lk;
                            int8_t bytes;
                        };
                        K kinds[] = {{-1, LK_VALUES, 16}, {ki, LK_INTS, 8}, {kf, LK_FLOATS, 8}, {kb, LK_BOOLS, 1}};
                        for (auto& kk : kinds) {
                            if (kk.label >= 0) a.bind(kk.label);
                            a.load(RDX, RAX, L.listItems + 8);   // end
                            a.load(RCX, RAX, L.listItems + 16);  // end of capacity
                            a.cmpR(RDX, RCX);
                            a.jcc(CC_AE, helper);
                            writeElement(c + 1, kk.lk, helper);  // a value that doesn't fit: the helper widens
                            a.load(RDX, RAX, L.listItems + 8);
                            a.subImm8(RDX, (int8_t)-kk.bytes);
                            a.store(RAX, L.listItems + 8, RDX);
                            a.loadI32(RCX, RAX, L.objSize);  // the list's size for the collector, as the helper does
                            a.subImm8(RCX, (int8_t)-kk.bytes);
                            a.store32(RAX, L.objSize, RCX);
                            a.movImm(RDX, (uint64_t)(uintptr_t)&vm->bytesAllocated);
                            a.addMemImm8(RDX, 0, kk.bytes);
                            a.movImm(RDX, (uint64_t)(uintptr_t)&vm->totalAllocated);
                            a.addMemImm8(RDX, 0, kk.bytes);
                            a.jmp(ok);  // the result is the list, already in its slot
                        }
                        a.bind(helper);
                    }
                    a.movR(ARG0, R12);
                    a.lea(ARG1, RBX, slot(c));
                    a.movImm(ARG2, (uint64_t)(uintptr_t)siteFor(in, argc));
                    a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitInvoke);
                    a.callR(RAX);
                    afterCall(i, ok, in);
                    a.bind(ok);
                    break;
                }
                case OP_NOT: {
                    int notBool = a.newLabel(), notNil = a.newLabel(), done = a.newLabel();
                    a.movzxByte(RAX, RBX, slot(d - 1) + kValTag);
                    a.cmpEaxImm8(TAG_BOOL);
                    a.jcc(CC_NE, notBool);
                    a.xorByte(RBX, slot(d - 1) + kValPayload, 1);
                    a.jmp(done);
                    a.bind(notBool);
                    a.testEaxEax();  // nil
                    a.jcc(CC_NE, notNil);
                    a.movImm(RAX, 1);
                    a.store(RBX, slot(d - 1) + kValPayload, RAX);
                    a.storeByte(RBX, slot(d - 1) + kValTag, TAG_BOOL);
                    a.jmp(done);
                    a.bind(notNil);
                    a.cmpEaxImm8(TAG_INT);
                    a.jcc(CC_NE, stepAt(i));
                    a.xorEaxEax();
                    a.store(RBX, slot(d - 1) + kValPayload, RAX);
                    a.storeByte(RBX, slot(d - 1) + kValTag, TAG_BOOL);
                    a.bind(done);
                    break;
                }
                case OP_AUTOCALL:
                    a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_OBJ);
                    a.jcc(CC_AE, stepAt(i));  // an object might be a function to call
                    break;
                case OP_JUMP: a.jmp(insLabel[an.at[(size_t)in.target]]); break;
                case OP_LOOP: {
                    // other tasks get a turn now and then (the interpreter switches tasks)
                    int turn = a.newLabel();
                    a.decMem(R12, L.ctxBudget);
                    a.jcc(CC_LE, turn);
                    a.jmp(insLabel[an.at[(size_t)in.target]]);
                    a.bind(turn);
                    a.storeByte(R12, L.ctxBudgetExit, 1);
                    a.jmp(bailAt(i));
                    break;
                }
                case OP_JUMP_IF_FALSE:
                case OP_JUMP_IF_FALSE_KEEP:
                case OP_JUMP_IF_TRUE_KEEP: {
                    int target = insLabel[an.at[(size_t)in.target]], next = a.newLabel();
                    if (in.op == OP_JUMP_IF_TRUE_KEEP) truthiness(d - 1, next, target);
                    else truthiness(d - 1, target, next);
                    a.bind(next);
                    break;
                }
                case OP_RANGE_INIT: {
                    // ints with a step other than 0 are ready; anything else the interpreter converts or reports
                    for (int k = d - 3; k < d; k++) {
                        a.cmpByte(RBX, slot(k) + kValTag, TAG_INT);
                        a.jcc(CC_NE, stepAt(i));
                    }
                    a.load(RAX, RBX, slot(d - 1) + kValPayload);
                    a.testR(RAX, RAX);
                    a.jcc(CC_E, stepAt(i));
                    break;
                }
                case OP_FOR_RANGE: {
                    // the counter, end and step live in slots k, k+1, k+2 (ints)
                    int k = (int)in.a;
                    bool incl = in.b & 1;
                    int exit = insLabel[an.at[(size_t)in.target]], push = a.newLabel();
                    a.load(R8, RBX, slot(k) + kValPayload);
                    a.load(R9, RBX, slot(k + 1) + kValPayload);
                    if (in.b & 2) {  // step 1
                        a.cmpR(R8, R9);
                        a.jcc(incl ? CC_G : CC_GE, exit);
                        storeInt(d, R8);
                        a.addMemImm8(RBX, slot(k) + kValPayload, 1);
                        break;
                    }
                    int neg = a.newLabel();
                    a.load(R10, RBX, slot(k + 2) + kValPayload);
                    a.testR(R10, R10);
                    a.jcc(CC_S, neg);
                    a.cmpR(R8, R9);
                    a.jcc(incl ? CC_G : CC_GE, exit);
                    a.jmp(push);
                    a.bind(neg);
                    a.cmpR(R8, R9);
                    a.jcc(incl ? CC_L : CC_LE, exit);
                    a.bind(push);
                    storeInt(d, R8);
                    a.addR(R8, R10);
                    a.store(RBX, slot(k) + kValPayload, R8);
                    break;
                }
                case OP_JUMP_IF_NOT_SEQ: {
                    // lists, tuples, strings, sets and closed ranges go on; anything else jumps
                    int target = insLabel[an.at[(size_t)in.target]], seq = a.newLabel();
                    a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, target);
                    a.load(RAX, RBX, slot(d - 1) + kValPayload);
                    for (OT t : {OT::List, OT::Tuple, OT::String, OT::Set}) {
                        a.cmpByte(RAX, L.objType, (uint8_t)t);
                        a.jcc(CC_E, seq);
                    }
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::Range);
                    a.jcc(CC_NE, target);
                    a.cmpByte(RAX, L.rangeOpenStart, 0);
                    a.jcc(CC_NE, target);
                    a.cmpByte(RAX, L.rangeOpenEnd, 0);
                    a.jcc(CC_NE, target);
                    a.bind(seq);
                    break;
                }
                case OP_JUMP_IF_SET:
                    a.cmpByte(RBX, slot((int)in.a) + kValTag, TAG_UNDEF);
                    a.jcc(CC_NE, insLabel[an.at[(size_t)in.target]]);
                    break;
                case OP_JUMP_IF_NOT_ERROR: {
                    int target = insLabel[an.at[(size_t)in.target]];
                    a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, target);
                    a.load(RAX, RBX, slot(d - 1) + kValPayload);
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::Error);
                    a.jcc(CC_NE, target);
                    break;
                }
                case OP_JUMP_IF_NOT_CMP: {
                    int target = insLabel[an.at[(size_t)in.target]];
                    int cmp = in.a < 6 ? (int)in.a : 5;
                    int cont = a.newLabel(), slow = slowFor(i, d, (uint8_t)cmp, cont, target);
                    slows.back().op = (uint8_t)(100 + cmp);  // marks the fused compare
                    int floats = cmp < 4 ? a.newLabel() : slow;
                    intOr(d - 2, floats);
                    intOr(d - 1, floats);
                    static const int inverse[] = {CC_GE, CC_G, CC_LE, CC_L, CC_NE, CC_E};
                    a.load(RAX, RBX, slot(d - 2) + kValPayload);
                    a.aluLoad(0x3B, RAX, RBX, slot(d - 1) + kValPayload);
                    a.jcc(inverse[cmp], target);
                    if (cmp < 4) {
                        a.jmp(cont);
                        a.bind(floats);
                        loadNum(0, d - 2, slow);
                        loadNum(1, d - 1, slow);
                        // jump unless a cmp b (unordered: never true, so it jumps)
                        if (cmp == 0 || cmp == 1) a.ucomisdRR(1, 0);
                        else a.ucomisdRR(0, 1);
                        a.jcc(cmp == 0 || cmp == 2 ? CC_BE : CC_B, target);
                    }
                    a.bind(cont);
                    break;
                }
                case OP_ENDSCOPE: break;
                case OP_ENDSCOPE_KEEP: copySlot(d - 1, d - 1 - (int)in.a); break;
                case OP_RETURN:
                    copySlot(d - 1, 0);
                    a.jmp(okLabel);
                    break;
                case OP_GET_GLOBAL:
                case OP_SET_GLOBAL: {
                    // module variables (the frame's module is the function's own: par tasks don't run jitted code)
                    ModuleRt* m = fn->module;
                    if (!m || in.a >= m->globals.size()) {
                        *why = "uses a module variable that doesn't exist yet";
                        return false;
                    }
                    a.movImm(RDX, (uint64_t)(uintptr_t)&m->globals);
                    a.load(RDX, RDX, 0);  // the vector's data pointer (read each time: the vector may grow)
                    int32_t g = (int32_t)(16 * in.a);
                    if (in.op == OP_GET_GLOBAL) {
                        a.cmpByte(RDX, g + kValTag, TAG_UNDEF);
                        a.jcc(CC_E, stepAt(i));  // not set yet: the interpreter reports it
                        a.load(RAX, RDX, g);
                        a.store(RBX, slot(d), RAX);
                        a.load(RAX, RDX, g + 8);
                        a.store(RBX, slot(d) + 8, RAX);
                        // a string grown in place is shared from now on
                        int done = a.newLabel();
                        a.cmpByte(RBX, slot(d) + kValTag, TAG_OBJ);
                        a.jcc(CC_NE, done);
                        a.cmpByte(RAX, L.objType, (uint8_t)OT::String);
                        a.jcc(CC_NE, done);
                        a.cmpByte(RAX, L.strAppendable, 0);
                        a.jcc(CC_E, done);
                        a.storeByte(RAX, L.strAppendable, 0);
                        a.bind(done);
                    } else {
                        a.load(RAX, RBX, slot(d - 1));
                        a.store(RDX, g, RAX);
                        a.load(RAX, RBX, slot(d - 1) + 8);
                        a.store(RDX, g + 8, RAX);
                    }
                    break;
                }
                case OP_CALL: {
                    int argc = (int)in.a;
                    int c = d - argc - 1;
                    int ok = a.newLabel(), generic = a.newLabel(), waits = a.newLabel();
                    if (argc == fn->arity) {
                        // a call to itself: straight into this code
                        a.cmpByte(RBX, slot(c) + kValTag, TAG_OBJ);
                        a.jcc(CC_NE, generic);
                        a.movImm(RCX, (uint64_t)(uintptr_t)self);
                        a.aluLoad(0x3B, RCX, RBX, slot(c) + kValPayload);
                        a.jcc(CC_NE, generic);
                        a.load(RAX, R12, L.ctxDepth);
                        a.aluLoad(0x3B, RAX, R12, L.ctxMax);
                        a.jcc(CC_GE, bailAt(i));
                        a.lea(RAX, RBX, slot(c + an.maxDepth + 1));
                        a.aluLoad(0x3B, RAX, R12, L.ctxStackEnd);
                        a.jcc(CC_A, bailAt(i));
                        a.incMem(R12, L.ctxDepth);
                        a.lea(ARG0, RBX, slot(c));
                        a.movR(ARG1, R12);
                        a.xorR(ARG2, ARG2);
                        a.call(entryLabel);
                        a.decMem(R12, L.ctxDepth);
                        a.testEaxEax();
                        a.jcc(CC_E, ok);
                        a.jmp(waits);
                    }
                    // another function with machine code (a callback, a closure): straight into it
                    a.bind(generic);
                    {
                        int helper = a.newLabel();
                        a.cmpByte(RBX, slot(c) + kValTag, TAG_OBJ);
                        a.jcc(CC_NE, helper);
                        a.load(RAX, RBX, slot(c) + kValPayload);
                        a.cmpByte(RAX, L.objType, (uint8_t)OT::Closure);
                        a.jcc(CC_NE, helper);
                        a.load(RCX, RAX, L.closFn);
                        a.cmpByte(RCX, L.fnJitDirect, 1);
                        a.jcc(CC_NE, helper);
                        a.loadI32(RDX, RCX, L.fnArity);
                        a.cmpImm32(RDX, argc);
                        a.jcc(CC_NE, helper);
                        a.load(RDX, R12, L.ctxDepth);
                        a.aluLoad(0x3B, RDX, R12, L.ctxMax);
                        a.jcc(CC_GE, helper);  // (the helper says "too deep" the usual way)
                        a.load(R8, RCX, L.fnJit);
                        a.loadI32(RDX, R8, L.jcMaxDepth);
                        a.shlImm(RDX, 4);
                        a.lea(R9, RBX, slot(c + 1));
                        a.addR(R9, RDX);
                        a.aluLoad(0x3B, R9, R12, L.ctxStackEnd);
                        a.jcc(CC_A, helper);
                        a.loadI32(RDX, RCX, L.fnJitCalls);
                        a.subImm8(RDX, -1);
                        a.store32(RCX, L.fnJitCalls, RDX);
                        a.store(RSP, saveArea, RCX);  // the callee, for after
                        a.incMem(R12, L.ctxDepth);
                        a.load(RAX, R8, L.jcFn);  // (before the arguments: R8 is one of them on Windows)
                        a.lea(ARG0, RBX, slot(c));
                        a.movR(ARG1, R12);
                        a.xorR(ARG2, ARG2);
                        a.callR(RAX);
                        a.decMem(R12, L.ctxDepth);
                        a.testEaxEax();
                        a.jcc(CC_E, ok);
                        a.movR(ARG0, R12);
                        a.load(ARG1, RSP, saveArea);
                        a.movImm(ARG2, 1);
                        a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitDirectBail);
                        a.callR(RAX);
                        a.jmp(waits);  // it stopped early: the interpreter finishes it
                        a.bind(helper);
                    }
                    // anything else (a built-in, a class, a function without machine code yet) through a helper
                    a.movR(ARG0, R12);
                    a.lea(ARG1, RBX, slot(c));
                    a.movImm(ARG2, (uint64_t)(uintptr_t)opSite(i));
                    a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitCall);
                    a.callR(RAX);
                    a.testR(RAX, RAX);
                    a.jcc(CC_E, ok);
                    a.cmpImm32(RAX, JIT_PAUSED);
                    a.jcc(CC_E, bailCommon);
                    a.cmpImm32(RAX, JIT_STEP);
                    a.jcc(CC_E, stepAt(i));
                    a.cmpImm32(RAX, 1);
                    a.jcc(CC_NE, bailAt(i));  // 2: the interpreter makes this call
                    // the call stopped early: wait for the interpreter to finish it
                    a.bind(waits);
                    loadSelf();
                    a.store(R12, L.ctxWaitingSelf, RAX);
                    a.movR(ARG0, R12);
                    a.movR(ARG1, RBX);
                    a.movImm(ARG2, (uint64_t)in.next);
                    a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitWait);
                    a.callR(RAX);
                    a.movEaxImm(1);
                    a.jmp(epilogue);
                    a.bind(ok);
                    break;
                }
                case OP_RANGE: {
                    // RANGE + ITER_INIT: an iterator object, like the interpreter makes
                    int first = d - ((in.a & 2) ? 3 : 2);
                    a.movR(ARG0, R12);
                    a.lea(ARG1, RBX, slot(first));
                    a.movImm(ARG2, in.a);
                    a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitMakeIter);
                    a.callR(RAX);
                    a.testEaxEax();
                    a.jcc(CC_NE, bailAt(i));
                    break;
                }
                case OP_ITER_INIT: {
                    if (i > 0 && an.ins[i - 1].op == OP_RANGE) break;  // RANGE made the iterator
                    // for x in list: the iterator (lists and ranges; anything else stops here)
                    a.movR(ARG0, R12);
                    a.lea(ARG1, RBX, slot(d - 1));
                    a.xorR(ARG2, ARG2);
                    a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitIterInit);
                    a.callR(RAX);
                    a.testR(RAX, RAX);
                    a.jcc(CC_NE, bailAt(i));
                    break;
                }
                case OP_CHECK_TYPE: {
                    // int / num / bool / any annotations; a mismatch stops here and the interpreter reports it
                    auto k = as<ObjTypeDesc>(fn->chunk.consts[in.a])->k;
                    if (k == ObjTypeDesc::Int || k == ObjTypeDesc::Num) {
                        a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_INT);
                        a.jcc(CC_NE, stepAt(i));
                    } else if (k == ObjTypeDesc::Bool) {
                        a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_BOOL);
                        a.jcc(CC_NE, stepAt(i));
                    }
                    break;
                }
                case OP_SEQ_INIT: {
                    // a list: position 0 in the next slot; anything else: the interpreter makes the iterator
                    a.cmpByte(RBX, slot(d - 1) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, stepAt(i));
                    a.load(RAX, RBX, slot(d - 1) + kValPayload);
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::List);
                    a.jcc(CC_NE, stepAt(i));
                    a.xorR(RAX, RAX);
                    storeInt(d, RAX);
                    break;
                }
                case OP_FOR_SEQ:
                case OP_FOR_NEXT: {
                    int k = (int)in.a;
                    int exit = insLabel[an.at[(size_t)in.target]];
                    int neg = a.newLabel(), push = a.newLabel();
                    if (in.op == OP_FOR_SEQ) {
                        // a list walked by the position in slot k+1; an iterator goes on below
                        int iter = a.newLabel();
                        a.cmpByte(RBX, slot(k + 1) + kValTag, TAG_INT);
                        a.jcc(CC_NE, iter);
                        a.load(RAX, RBX, slot(k) + kValPayload);  // ObjList*
                        int kb = a.newLabel(), ki = a.newLabel(), kf = a.newLabel();
                        listKind(kb, ki, kf);
                        struct K {
                            int label;
                            ListKind lk;
                            int shift;
                        };
                        K kinds[] = {{-1, LK_VALUES, 4}, {ki, LK_INTS, 3}, {kf, LK_FLOATS, 3}, {kb, LK_BOOLS, 0}};
                        for (auto& kk : kinds) {
                            if (kk.label >= 0) a.bind(kk.label);
                            a.load(RCX, RBX, slot(k + 1) + kValPayload);  // the position
                            listElement(kk.shift, exit);                    // past the end: done
                            readElement(d, kk.lk);
                            a.addMemImm8(RBX, slot(k + 1) + kValPayload, 1);
                            a.jmp(insLabel[i + 1]);
                        }
                        a.bind(iter);
                    }
                    a.cmpByte(RBX, slot(k) + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, bailAt(i));
                    a.load(RAX, RBX, slot(k) + kValPayload);  // ObjIterator*
                    a.cmpByte(RAX, L.objType, (uint8_t)OT::Iterator);
                    a.jcc(CC_NE, bailAt(i));
                    a.cmpByte(RAX, L.iterPending, 0);
                    a.jcc(CC_NE, bailAt(i));
                    a.cmpByte(RAX, L.iterPairs, 0);
                    a.jcc(CC_NE, bailAt(i));
                    a.cmpByte(RAX, L.iterSrc + kValTag, TAG_OBJ);
                    a.jcc(CC_NE, bailAt(i));
                    a.load(RCX, RAX, L.iterSrc + kValPayload);  // ObjRange* (or ObjList*)
                    int notList = a.newLabel();
                    a.cmpByte(RCX, L.objType, (uint8_t)OT::List);
                    a.jcc(CC_NE, notList);
                    {  // a list of values: the item at idx, while idx < size (typed lists: the interpreter)
                        a.movzxByte(R10, RCX, L.objKind);
                        a.subImm8(R10, LK_BOOLS);
                        a.cmpImm32(R10, LK_FLOATS - LK_BOOLS + 1);
                        a.jcc(CC_B, bailAt(i));
                        a.load(R8, RAX, L.iterIdx);
                        a.load(R9, RCX, L.listItems);
                        a.load(R10, RCX, L.listItems + 8);
                        a.subR(R10, R9);
                        a.shrImm(R10, 4);
                        a.cmpR(R8, R10);
                        a.jcc(CC_AE, exit);
                        a.movR(R11, R8);
                        a.shlImm(R11, 4);
                        a.addR(R11, R9);
                        a.load(R10, R11, 0);
                        a.store(RBX, slot(d), R10);
                        a.load(R10, R11, 8);
                        a.store(RBX, slot(d) + 8, R10);
                        a.incMem(RAX, L.iterIdx);
                        a.jmp(insLabel[i + 1]);
                    }
                    a.bind(notList);
                    a.cmpByte(RCX, L.objType, (uint8_t)OT::Range);
                    a.jcc(CC_NE, bailAt(i));
                    a.cmpByte(RCX, L.rangeOpenStart, 0);
                    a.jcc(CC_NE, bailAt(i));
                    a.cmpByte(RCX, L.rangeOpenEnd, 0);
                    a.jcc(CC_NE, bailAt(i));
                    a.load(R8, RAX, L.iterCur);
                    a.load(R9, RCX, L.rangeEnd);
                    a.load(R10, RCX, L.rangeStep);
                    a.movzxByte(R11, RCX, L.rangeIncl);
                    a.testR(R10, R10);
                    a.jcc(CC_S, neg);
                    {  // step > 0: more while cur < end (or <= when inclusive)
                        int incl = a.newLabel();
                        a.testR(R11, R11);
                        a.jcc(CC_NE, incl);
                        a.cmpR(R8, R9);
                        a.jcc(CC_GE, exit);
                        a.jmp(push);
                        a.bind(incl);
                        a.cmpR(R8, R9);
                        a.jcc(CC_G, exit);
                        a.jmp(push);
                    }
                    a.bind(neg);
                    {
                        int incl = a.newLabel();
                        a.testR(R11, R11);
                        a.jcc(CC_NE, incl);
                        a.cmpR(R8, R9);
                        a.jcc(CC_LE, exit);
                        a.jmp(push);
                        a.bind(incl);
                        a.cmpR(R8, R9);
                        a.jcc(CC_L, exit);
                    }
                    a.bind(push);
                    storeInt(d, R8);
                    a.addR(R8, R10);
                    a.store(RAX, L.iterCur, R8);
                    break;
                }
                default:
                    *why = std::string("uses ") + opName(in.op);
                    return false;
            }
        }
        for (auto& [j, l] : relabeled) insLabel[j] = l;
        relabeled.clear();
        // falling off the end can't happen (functions end with RETURN); stop safely if it does
        a.jmp(bailAt(an.ins.size() - 1));
        // return paths
        a.bind(okLabel);
        a.xorEaxEax();
        a.bind(epilogue);
        if (t2 && saveFp) {
#if defined(IXO_JIT_X64) && defined(_WIN32)
            for (int x = 6; x <= 13; x++)
                if (saves(16 + x)) a.movupsLoad(x, RSP, xmmSave + 16 * (x - 6));
#elif defined(IXO_JIT_ARM64)
            for (int x = 8; x <= 13; x++)
                if (saves(16 + x)) a.movsdLoad(x, RSP, xmmSave + 16 * (x - 8));
#endif
        }
        a.addRsp(frameSize);
        for (size_t k = pushed.size(); k-- > 0;) a.pop(pushed[k]);
        a.pop(R12);
        a.pop(RBX);
        a.leave();
        a.ret();
        if (t2) {
            // entering a hot loop from the interpreter: its slots must hold what the code assumes
            for (size_t k = 0; k < osrLabel.size(); k++) {
                a.bind(osrLabel[k]);
                size_t h = an.at[an.loopHeaders[k]];
                int depth = an.ins[h].depth;
                for (int j = 0; j < depth && j < ti->slots; j++) {
                    uint8_t t = ti->at[h][(size_t)j];
                    if (tyUnboxed(t)) {
                        a.cmpByte(RBX, slot(j) + kValTag, tagOf(t));
                        guessCheck(exitAt(h, true, 0));
                    }
                }
                a.jmp(canonFor(h));
            }
            emitColds();
            emitEdges();
        }
        for (auto& s : slows) {
            a.bind(s.label);
            pcMap.push_back({(uint32_t)a.b.size(), (uint32_t)an.ins[s.ins].off});
            a.movR(ARG0, R12);
            if (s.op >= 100) {
                // fused compare-and-branch: 1 true (go on), 0 false (jump), 2 not numbers
                a.lea(ARG1, RBX, slot(s.d - 2));
                a.movImm(ARG2, (uint64_t)(s.op - 100));
                a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitCmp);
                a.callR(RAX);
                a.cmpImm32(RAX, 2);
                a.jcc(CC_E, bailAt(s.ins));
                a.testR(RAX, RAX);
                a.jcc(CC_E, s.target);
                a.jmp(s.cont);
                continue;
            }
            a.lea(ARG1, RBX, slot(s.op == OP_NEG ? s.d - 1 : s.d - 2));
            a.movImm(ARG2, (uint64_t)s.op);
            a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitNum);
            a.callR(RAX);
            a.testR(RAX, RAX);
            a.jcc(CC_NE, stepAt(s.ins));  // not numbers (strings, lists...): the interpreter does it
            a.jmp(s.cont);
        }
        for (size_t k = 0; k < stepList.size(); k++) {
            size_t i = stepList[k];
            a.bind(stepFor[i]);
            pcMap.push_back({(uint32_t)a.b.size(), (uint32_t)an.ins[i].off});
            a.movR(ARG0, R12);
            a.movR(ARG1, RBX);
            a.movImm(ARG2, (uint64_t)(uintptr_t)opSite(i));
            a.movImm(RAX, (uint64_t)(uintptr_t)&ixoJitStep);
            a.callR(RAX);
            a.testR(RAX, RAX);
            a.jcc(CC_E, t2 && ti->blockStart[i + 1] ? canonFor(i + 1) : insLabel[i + 1]);
            a.cmpImm32(RAX, JIT_PAUSED);
            a.jcc(CC_E, bailCommon);
            a.jmp(bailAt(i));
        }
        if (t2)  // join points of loops, entered with everything in memory: load the homes
            for (size_t j = 0; j < canon.size(); j++) {
                if (canon[j] < 0) continue;
                a.bind(canon[j]);
                if (unprofiledLoop[j] >= 0) {  // (as on the way in from above)
                    a.movImm(RAX, (uint64_t)(uintptr_t)(code->learned.get() + unprofiledLoop[j]));
                    int seen = a.newLabel();
                    a.cmpByte(RAX, 0, 0);
                    a.jcc(CC_NE, seen);
                    a.storeByte(RAX, 0, 1);
                    a.movImm(RAX, (uint64_t)(uintptr_t)&code->relearn);
                    a.storeByte(RAX, 0, 1);
                    a.jmp(bailAt(j));
                    a.bind(seen);
                }
                for (auto& [k, reg] : homesAt(j)) {
                    if (isX(reg)) a.movsdLoad(reg - 16, RBX, slot(k) + kValPayload);
                    else if (ti->at[j][(size_t)k] == TY_BOOL) a.movzxByte(reg, RBX, slot(k) + kValPayload);
                    else a.load(reg, RBX, slot(k) + kValPayload);
                }
                a.jmp(insLabel[j]);
            }
        for (auto& s : stubs) {
            a.bind(s.label);
            pcMap.push_back({(uint32_t)a.b.size(), s.off});
            a.storeDword(R12, L.ctxBailOff, s.off);
            a.storeDword(R12, L.ctxBailDepth, s.depth);
            a.jmp(bailCommon);
        }
        a.bind(bailCommon);
        loadSelf();
        a.store(R12, L.ctxBailClosure, RAX);
        a.store(R12, L.ctxBailBase, RBX);
        a.movEaxImm(1);
        a.jmp(epilogue);
        if (!a.finish()) {
            *why = "internal: unresolved jump";
            return false;
        }
        if (dry) return true;
        if (const char* dump = std::getenv("IXO_JIT_DUMP")) {  // raw machine code, for a disassembler
            std::string path = std::string(dump) + "/" + (fn->name.empty() ? "script" : fn->name) + (t2 ? ".t2" : ".t1") + ".bin";
            if (FILE* fo = std::fopen(path.c_str(), "wb")) {
                std::fwrite(a.b.data(), 1, a.b.size(), fo);
                std::fclose(fo);
            }
        }
        void* mem = allocExec(a.b);
        if (!mem) {
            *why = "can't get executable memory";
            return false;
        }
        (void)cnt;
        out.fn = reinterpret_cast<JitFn>(mem);
        out.mem = mem;
        out.bytes = a.b.size();
        out.maxDepth = an.maxDepth + 1;
        out.loopHeaders = an.loopHeaders;
        registerCode((uintptr_t)mem, a.b.size(), fn, &out);
        return true;
    }
};

#endif  // IXO_JIT_ANY

}  // namespace

bool jitSupported() {
#ifdef IXO_JIT_ANY
    return true;
#else
    return false;
#endif
}

bool jitFindPc(uintptr_t pc, ObjFunction** fn, uint32_t* offset) {
#ifdef IXO_JIT_ANY
    std::lock_guard<std::mutex> g(codeRangesLock);
    auto it = std::upper_bound(codeRanges.begin(), codeRanges.end(), pc,
                               [](uintptr_t p, const CodeRange& c) { return p < c.start; });
    if (it == codeRanges.begin()) return false;
    --it;
    if (pc >= it->end) return false;
    const auto& map = it->code->pcMap;
    uint32_t rel = (uint32_t)(pc - it->start);
    auto m = std::upper_bound(map.begin(), map.end(), rel,
                              [](uint32_t r, const std::pair<uint32_t, uint32_t>& e) { return r < e.first; });
    *fn = it->fn;
    *offset = m == map.begin() ? 0 : std::prev(m)->second;
    return true;
#else
    (void)pc;
    (void)fn;
    (void)offset;
    return false;
#endif
}

void jitCodeBounds(uintptr_t* lo, uintptr_t* hi) {
#ifdef IXO_JIT_ANY
    *lo = codeLo.load(std::memory_order_relaxed);
    *hi = codeHi.load(std::memory_order_relaxed);
#else
    *lo = 1;
    *hi = 0;
#endif
}

std::string jitCheck(ObjFunction* fn) {
    Analysis an;
    std::string why;
    if (!analyze(fn, an, &why)) return why.empty() ? "isn't supported" : why;
    return "";
}

bool jitCompile(ObjFunction* fn, ObjClosure* self, JitCode& out, std::string* why,
                const std::vector<std::pair<uint32_t, int>>& seeds, VM* vm) {
#ifdef IXO_JIT_ANY
    Analysis an;
    std::string reason;
    if (!analyze(fn, an, &reason, seeds)) {
        if (why) *why = reason;
        return false;
    }
    // the typed tier first, where types are known; else (or if it can't) the plain one
    static const bool tier2On = [] {
        const char* e = std::getenv("IXO_JIT_TIER2");
        return !(e && e[0] == '0');
    }();
    if (tier2On && !(tlsNoTier2 >= 0 ? tlsNoTier2 != 0 : fn->noTier2)) {
        TypeInfo ti;
        inferTypes(fn, an, ti, seeds);
        if (ti.useful) {
            std::string r2;
            uint32_t used = 0;
            bool ok;
            {
                Gen probe(fn, self, an);  // which registers it uses
                probe.vm = vm;
                probe.t2 = true;
                probe.ti = &ti;
                probe.dry = true;
                ok = probe.run(out, &r2);
                used = probe.used;
            }
            out = JitCode{};
            if (ok) {
                Gen g2(fn, self, an);
                g2.vm = vm;
                g2.t2 = true;
                g2.ti = &ti;
                g2.saveMask = used;
                if (g2.run(out, &r2)) return true;
                out = JitCode{};
            }
        }
    }
    Gen g(fn, self, an);
    g.vm = vm;
    if (!g.run(out, &reason)) {
        if (why) *why = reason;
        return false;
    }
    return true;
#else
    (void)fn;
    (void)self;
    (void)out;
    if (why) *why = "the JIT only supports x86-64";
    return false;
#endif
}

bool jitCompileFrom(ObjFunction* fn, ObjClosure* self, JitCode& out, std::string* why,
                    const std::vector<std::pair<uint32_t, int>>& seeds, VM* vm, const std::vector<uint8_t>* profile,
                    const std::vector<InlineCache>* ics, bool noTier2) {
    tlsProfile = profile;
    tlsIcs = ics;
    tlsSeeds = &seeds;
    tlsNoTier2 = noTier2 ? 1 : 0;
    bool ok = jitCompile(fn, self, out, why, seeds, vm);
    tlsProfile = nullptr;
    tlsIcs = nullptr;
    tlsSeeds = nullptr;
    tlsNoTier2 = -1;
    return ok;
}

void* jitAllocExec(const std::vector<uint8_t>& code) {
#ifdef IXO_JIT_ANY
    return allocExec(code);
#else
    (void)code;
    return nullptr;
#endif
}

void jitFreeCode(JitCode* code) {
    if (!code) return;
#ifdef IXO_JIT_ANY
    if (code->mem) {
        unregisterCode((uintptr_t)code->mem);
        std::lock_guard<std::mutex> g(codeArenaLock);
        freeCode.push_back({static_cast<char*>(code->mem), (code->bytes + 15) & ~(size_t)15});
    }
#endif
    delete code;
}

}  // namespace ixo
