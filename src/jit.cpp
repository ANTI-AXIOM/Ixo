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
    void sdRR(uint8_t op, int dst, int src) {  // addsd 58, mulsd 59, subsd 5C, divsd 5E, sqrtsd 51
        u8(0xF2);
        u8(0x0F);
        u8(op);
        u8((uint8_t)(0xC0 | (dst << 3) | src));
    }
    void ucomisdRR(int a2, int b2) {  // flags from a2 vs b2 (unordered: ZF PF CF all set)
        u8(0x66);
        u8(0x0F);
        u8(0x2E);
        u8((uint8_t)(0xC0 | (a2 << 3) | b2));
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
        static const int map[] = {9, 10, 11, 19, 31, 29, 12, 13, 12, 13, 14, 15, 20, 21, 22, 23, 0, 1, 2};
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

void* allocExec(const std::vector<uint8_t>& code) {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    std::memcpy(p, code.data(), code.size());
    DWORD old;
    if (!VirtualProtect(p, code.size(), PAGE_EXECUTE_READ, &old)) return nullptr;
    FlushInstructionCache(GetCurrentProcess(), p, code.size());
    return p;
#elif defined(__APPLE__) && defined(IXO_JIT_ARM64)
    // Apple silicon: JIT pages are written with the thread's write permission on
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t n = (code.size() + page - 1) / page * page;
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    pthread_jit_write_protect_np(0);
    std::memcpy(p, code.data(), code.size());
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, code.size());
    return p;
#else
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t n = (code.size() + page - 1) / page * page;
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::memcpy(p, code.data(), code.size());
    if (mprotect(p, n, PROT_READ | PROT_EXEC) != 0) return nullptr;
    __builtin___clear_cache((char*)p, (char*)p + code.size());
    return p;
#endif
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
        strAppendable, objSize, mapLive;
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
        listItems = d(&lst, &lst.items);
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
        InlineCache ic;
        icClsId = d(&ic, &ic.clsId);
        icIndex = d(&ic, &ic.index);
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
    if (ctx->depth >= ctx->maxDepth || base + fn->jit->maxDepth + 1 > ctx->stackEnd) return 2;
    fn->jitCalls++;
    ctx->depth++;
    int r = fn->jit->fn(base, ctx, 0);
    ctx->depth--;
    if (r == 1 && (ctx->vm->jitPanic || ctx->pausedExit || ctx->budgetExit)) return r;  // not a failure
    if (r != 0 && ++fn->jitBails > 50 && fn->jitBails * 2 > fn->jitCalls) {
        fn->jitState = 4;
        if (ctx->vm->jitStats) ctx->vm->noteJitGiveUp(fn, "calls");
    }
    return r;
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
    if (ctx->depth >= ctx->maxDepth || base + fn->jit->maxDepth + 2 > ctx->stackEnd) return 2;
    fn->jitCalls++;
    ctx->depth++;
    int res = fn->jit->fn(base, ctx, 0);
    ctx->depth--;
    if (res == 1 && (ctx->vm->jitPanic || ctx->pausedExit || ctx->budgetExit)) return res;
    if (res != 0 && ++fn->jitBails > 50 && fn->jitBails * 2 > fn->jitCalls) {
        fn->jitState = 4;
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
        a.bind(entryLabel);
        a.enter();
        a.push(RBX);
        a.push(R12);
        a.subRsp(frameSize);
        a.movR(RBX, ARG0);
        a.movR(R12, ARG1);
        // OSR entries
        for (size_t k = 0; k < an.loopHeaders.size(); k++) {
            a.movR(RAX, ARG2);
            a.cmpImm32(RAX, (int32_t)(k + 1));
            a.jcc(CC_E, insLabel[an.at[an.loopHeaders[k]]]);
        }
        int32_t cnt = 0;
        auto& pcMap = out.pcMap;
        pcMap.push_back({0, 0});
        for (size_t i = 0; i < an.ins.size(); i++) {
            const Ins& in = an.ins[i];
            a.bind(insLabel[i]);
            int d = in.depth;
            if (d < 0) continue;  // unreachable
            pcMap.push_back({(uint32_t)a.b.size(), (uint32_t)in.off});
            cnt++;
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
                            a.subR(RCX, RDX);
                            a.shrImm(RCX, 4);
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
                    needInt(i, k + 1);
                    a.load(RCX, RBX, slot(k + 1) + kValPayload);  // the index
                    a.load(RDX, RAX, L.listItems);                // begin
                    a.load(R8, RAX, L.listItems + 8);             // end
                    a.subR(R8, RDX);
                    a.shrImm(R8, 4);  // the length
                    int pos = a.newLabel();
                    a.testR(RCX, RCX);
                    a.jcc(CC_NS, pos);
                    a.addR(RCX, R8);
                    a.bind(pos);
                    a.cmpR(RCX, R8);
                    a.jcc(CC_AE, stepAt(i));  // out of range (or still negative): the interpreter reports it
                    a.shlImm(RCX, 4);
                    a.addR(RDX, RCX);
                    if (in.op == OP_GET_INDEX) {
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
                        a.load(RCX, RAX, L.listItems + 8);   // end
                        a.load(RDX, RAX, L.listItems + 16);  // end of capacity
                        a.cmpR(RCX, RDX);
                        a.jcc(CC_AE, helper);
                        a.load(RDX, RBX, slot(c + 1));
                        a.store(RCX, 0, RDX);
                        a.load(RDX, RBX, slot(c + 1) + 8);
                        a.store(RCX, 8, RDX);
                        a.subImm8(RCX, -16);
                        a.store(RAX, L.listItems + 8, RCX);
                        a.loadI32(RCX, RAX, L.objSize);  // the list's size for the collector, as the helper does
                        a.subImm8(RCX, -16);
                        a.store32(RAX, L.objSize, RCX);
                        a.movImm(RDX, (uint64_t)(uintptr_t)&vm->bytesAllocated);
                        a.addMemImm8(RDX, 0, 16);
                        a.movImm(RDX, (uint64_t)(uintptr_t)&vm->totalAllocated);
                        a.addMemImm8(RDX, 0, 16);
                        a.jmp(ok);  // the result is the list, already in its slot
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
                    // anything else (another function, a built-in, a class) through a helper
                    a.bind(generic);
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
                        a.load(RCX, RBX, slot(k) + kValPayload);  // ObjList*
                        a.load(R8, RBX, slot(k + 1) + kValPayload);
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
                        a.addMemImm8(RBX, slot(k + 1) + kValPayload, 1);
                        a.jmp(insLabel[i + 1]);
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
                    {  // a list: the item at idx, while idx < size
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
        // falling off the end can't happen (functions end with RETURN); stop safely if it does
        a.jmp(bailAt(an.ins.size() - 1));
        // return paths
        a.bind(okLabel);
        a.xorEaxEax();
        a.bind(epilogue);
        a.addRsp(frameSize);
        a.pop(R12);
        a.pop(RBX);
        a.leave();
        a.ret();
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
            a.jcc(CC_E, insLabel[i + 1]);
            a.cmpImm32(RAX, JIT_PAUSED);
            a.jcc(CC_E, bailCommon);
            a.jmp(bailAt(i));
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
        void* mem = allocExec(a.b);
        if (!mem) {
            *why = "can't get executable memory";
            return false;
        }
        (void)cnt;
        out.fn = reinterpret_cast<JitFn>(mem);
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

}  // namespace ixo
