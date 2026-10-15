// Machine code for the register code of CPU kernels (gpukern.hpp).
//
// Each register of the code is a slot in memory (4 bytes narrow, 8 wide); each
// instruction becomes a few machine instructions working on them. Buffer loads
// and stores check the index inline; the rarer operations (floored division,
// math, launch dimensions) and an index out of range call back into C++. x86-64
// and ARM64; elsewhere the register code runs as it is.
#include <cstring>

#include "gpukern.hpp"
#include "jit.hpp"

namespace ixo {

namespace {

constexpr int32_t kLenOff = 8 * kKernelBuffers;  // KCtx::len

#if defined(__x86_64__) || defined(_M_X64)
#define IXO_KCODE 1
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RSI = 6, RDI = 7, R8 = 8, R12 = 12, R13 = 13, R14 = 14 };

struct Emit {
    std::vector<uint8_t> b;
    std::vector<size_t> at;  // instruction -> code offset
    struct Fix {
        size_t pos;  // where the rel32 goes
        int target;  // instruction index (-1: the epilogue)
    };
    std::vector<Fix> fixes;
    void u8(uint8_t x) { b.push_back(x); }
    void u32(uint32_t x) {
        for (int i = 0; i < 4; i++) b.push_back((uint8_t)(x >> (8 * i)));
    }
    void u64(uint64_t x) {
        for (int i = 0; i < 8; i++) b.push_back((uint8_t)(x >> (8 * i)));
    }
    void rex(bool w, int reg, int base) {
        uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((base & 8) ? 1 : 0));
        if (r != 0x40) u8(r);
    }
    void mem(int reg, int base, int32_t disp) {
        u8((uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
        if ((base & 7) == 4) u8(0x24);
        u32((uint32_t)disp);
    }
    void rm(bool w, std::initializer_list<uint8_t> ops, int reg, int base, int32_t disp, uint8_t prefix = 0) {
        if (prefix) u8(prefix);
        rex(w, reg, base);
        for (uint8_t o : ops) u8(o);
        mem(reg, base, disp);
    }
    void load(bool w, int reg, int base, int32_t disp) { rm(w, {0x8B}, reg, base, disp); }
    void store(bool w, int base, int32_t disp, int reg) { rm(w, {0x89}, reg, base, disp); }
    void movRR(int dst, int src) {  // 64-bit
        rex(true, src, dst);
        u8(0x89);
        u8((uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
    }
    void movImm(int r, uint64_t v) {
        rex(true, 0, r);
        u8((uint8_t)(0xB8 + (r & 7)));
        u64(v);
    }
    void push(int r) {
        if (r & 8) u8(0x41);
        u8((uint8_t)(0x50 + (r & 7)));
    }
    void pop(int r) {
        if (r & 8) u8(0x41);
        u8((uint8_t)(0x58 + (r & 7)));
    }
    void jmpTo(int target) {
        u8(0xE9);
        fixes.push_back({b.size(), target});
        u32(0);
    }
    void jccTo(uint8_t cc, int target) {  // cc: the second byte of 0F 8x
        u8(0x0F);
        u8(cc);
        fixes.push_back({b.size(), target});
        u32(0);
    }
    // local forward jumps (within one instruction's code)
    size_t jccFwd(uint8_t cc) {
        u8(0x0F);
        u8(cc);
        u32(0);
        return b.size();
    }
    size_t jmpFwd() {
        u8(0xE9);
        u32(0);
        return b.size();
    }
    void land(size_t after) {
        uint32_t rel = (uint32_t)(b.size() - after);
        std::memcpy(&b[after - 4], &rel, 4);
    }
};

#ifdef _WIN32
constexpr int ARG0 = RCX, ARG1 = RDX, ARG2 = R8;
#else
constexpr int ARG0 = RDI, ARG1 = RSI, ARG2 = RDX;
#endif

KernelFn compileX64(const CpuCode& c, KernelHelper helper) {
    Emit e;
    const bool w = c.wide;
    const int S = w ? 8 : 4;
    if ((int64_t)c.regs * S > INT32_MAX / 2) return nullptr;
    auto R = [&](int32_t r) { return r * S; };
    // prologue: rbx = registers, r12 = the context, r13 = the thread
    e.push(RBX);
    e.push(R12);
    e.push(R13);
    e.push(R14);
    e.u8(0x48), e.u8(0x83), e.u8(0xEC), e.u8(40);  // sub rsp, 40 (shadow space; aligned)
    e.movRR(RBX, ARG0);
    e.movRR(R12, ARG1);
    e.movRR(R13, ARG2);
    auto call = [&](const CInst* in) {
        e.movRR(ARG0, RBX);
        e.movImm(ARG1, (uint64_t)(uintptr_t)in);
        e.movRR(ARG2, R12);
        e.movImm(RAX, (uint64_t)(uintptr_t)helper);
        e.u8(0xFF), e.u8(0xD0);  // call rax
    };
    auto setccStore = [&](uint8_t cc, int32_t d) {
        e.u8(0x0F), e.u8(cc), e.u8(0xC0);  // setcc al
        e.u8(0x0F), e.u8(0xB6), e.u8(0xC0);  // movzx eax, al
        e.store(w, RBX, R(d), RAX);
    };
    const uint8_t pre = w ? 0xF2 : 0xF3;  // sd / ss
    e.at.resize(c.code.size() + 1);
    for (size_t pc = 0; pc < c.code.size(); pc++) {
        e.at[pc] = e.b.size();
        const CInst& in = c.code[pc];
        switch (in.op) {
            case C_CONST:
                if (w) {
                    e.movImm(RAX, (uint64_t)(int64_t)in.a);
                    e.store(true, RBX, R(in.d), RAX);
                } else {
                    e.rex(false, 0, RBX);
                    e.u8(0xC7);
                    e.mem(0, RBX, R(in.d));
                    e.u32((uint32_t)in.a);
                }
                break;
            case C_CONSTW:
                e.movImm(RAX, in.a >= 0 && (size_t)in.a < c.consts.size() ? c.consts[(size_t)in.a] : 0);
                e.store(true, RBX, R(in.d), RAX);
                break;
            case C_MOV:
                e.load(w, RAX, RBX, R(in.a));
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_TID: e.store(w, RBX, R(in.d), R13); break;
            case C_LEN:
                if (in.a < 0 || in.a >= kKernelBuffers) return nullptr;
                e.load(true, RAX, R12, kLenOff + 8 * in.a);
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_LOAD: case C_STORE: {
                bool ld = in.op == C_LOAD;
                int buf = ld ? in.a : in.d, idx = ld ? in.b : in.a;
                if (buf < 0 || buf >= kKernelBuffers) return nullptr;
                if (w) e.load(true, RAX, RBX, R(idx));
                else e.rm(true, {0x63}, RAX, RBX, R(idx));  // movsxd rax, [index]
                e.rm(true, {0x3B}, RAX, R12, kLenOff + 8 * buf);  // cmp rax, len
                size_t bad = e.jccFwd(0x83);  // jae: out of range (negatives too)
                e.load(true, RDX, R12, 8 * buf);
                if (ld) {
                    if (w) e.u8(0x48);
                    e.u8(0x8B), e.u8(0x04), e.u8(w ? 0xC2 : 0x82);  // mov (r/e)ax, [rdx + rax*S]
                    e.store(w, RBX, R(in.d), RAX);
                } else {
                    e.load(w, RCX, RBX, R(in.b));
                    if (w) e.u8(0x48);
                    e.u8(0x89), e.u8(0x0C), e.u8(w ? 0xC2 : 0x82);  // mov [rdx + rax*S], (r/e)cx
                }
                size_t done = e.jmpFwd();
                e.land(bad);
                call(&in);
                e.land(done);
                break;
            }
            case C_IADD: case C_ISUB: case C_AND: case C_OR: case C_XOR: {
                uint8_t op = in.op == C_IADD ? 0x03 : in.op == C_ISUB ? 0x2B : in.op == C_AND ? 0x23 : in.op == C_OR ? 0x0B : 0x33;
                e.load(w, RAX, RBX, R(in.a));
                e.rm(w, {op}, RAX, RBX, R(in.b));
                e.store(w, RBX, R(in.d), RAX);
                break;
            }
            case C_IMUL:
                e.load(w, RAX, RBX, R(in.a));
                e.rm(w, {0x0F, 0xAF}, RAX, RBX, R(in.b));
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_FADD: case C_FSUB: case C_FMUL: case C_FDIV: {
                if (in.op == C_FDIV && in.m2) {  // ints divided (wide): stops at 0, as the loop would
                    call(&in);
                    break;
                }
                uint8_t op = in.op == C_FADD ? 0x58 : in.op == C_FSUB ? 0x5C : in.op == C_FMUL ? 0x59 : 0x5E;
                e.rm(false, {0x0F, 0x10}, 0, RBX, R(in.a), pre);
                e.rm(false, {0x0F, op}, 0, RBX, R(in.b), pre);
                e.rm(false, {0x0F, 0x11}, 0, RBX, R(in.d), pre);
                break;
            }
            case C_ILT: case C_ILE: case C_IGT: case C_IGE: case C_IEQ: case C_INE: {
                static const uint8_t cc[] = {0x9C, 0x9E, 0x9F, 0x9D, 0x94, 0x95};
                e.load(w, RAX, RBX, R(in.a));
                e.rm(w, {0x3B}, RAX, RBX, R(in.b));
                setccStore(cc[in.op - C_ILT], in.d);
                break;
            }
            case C_FLT: case C_FLE: case C_FGT: case C_FGE: {
                // a < b is b > a: unordered (NaN) sets CF, so "above" is false then
                bool swap = in.op == C_FLT || in.op == C_FLE;
                e.rm(false, {0x0F, 0x10}, 0, RBX, R(swap ? in.b : in.a), pre);
                e.rm(false, {0x0F, 0x2E}, 0, RBX, R(swap ? in.a : in.b), w ? 0x66 : 0);  // ucomis
                setccStore(in.op == C_FLT || in.op == C_FGT ? 0x97 : 0x93, in.d);  // seta / setae
                break;
            }
            case C_FEQ: case C_FNE:
                e.rm(false, {0x0F, 0x10}, 0, RBX, R(in.a), pre);
                e.rm(false, {0x0F, 0x2E}, 0, RBX, R(in.b), w ? 0x66 : 0);
                if (in.op == C_FEQ) {
                    e.u8(0x0F), e.u8(0x94), e.u8(0xC0);  // sete al
                    e.u8(0x0F), e.u8(0x9B), e.u8(0xC1);  // setnp cl
                    e.u8(0x20), e.u8(0xC8);              // and al, cl
                } else {
                    e.u8(0x0F), e.u8(0x95), e.u8(0xC0);  // setne al
                    e.u8(0x0F), e.u8(0x9A), e.u8(0xC1);  // setp cl
                    e.u8(0x08), e.u8(0xC8);              // or al, cl
                }
                e.u8(0x0F), e.u8(0xB6), e.u8(0xC0);
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_SHL: case C_SHR:
                e.load(w, RCX, RBX, R(in.b));
                e.load(w, RAX, RBX, R(in.a));
                if (w) e.u8(0x48);
                e.u8(0xD3), e.u8(in.op == C_SHL ? 0xE0 : 0xF8);  // shl / sar (r/e)ax, cl
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_NOT:
                e.load(w, RAX, RBX, R(in.a));
                if (w) e.u8(0x48);
                e.u8(0x85), e.u8(0xC0);  // test
                setccStore(0x94, in.d);
                break;
            case C_INEG:
                e.load(w, RAX, RBX, R(in.a));
                if (w) e.u8(0x48);
                e.u8(0xF7), e.u8(0xD8);  // neg
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_FNEG:
                e.load(w, RAX, RBX, R(in.a));
                if (w) {
                    e.movImm(RCX, 0x8000000000000000ull);
                    e.u8(0x48), e.u8(0x31), e.u8(0xC8);  // xor rax, rcx
                } else {
                    e.u8(0x35);
                    e.u32(0x80000000u);  // xor eax, sign
                }
                e.store(w, RBX, R(in.d), RAX);
                break;
            case C_JMP: e.jmpTo(in.a); break;
            case C_JZ:
                e.load(w, RAX, RBX, R(in.a));
                if (w) e.u8(0x48);
                e.u8(0x85), e.u8(0xC0);
                e.jccTo(0x84, in.b);
                break;
            case C_RET: e.jmpTo(-1); break;
            case C_IDIV: case C_IMOD: case C_FIDIV: case C_FMOD: case C_FPOW: case C_MATH: case C_DIM: case C_LID:
            case C_GRP:
                call(&in);
                break;
            default: return nullptr;  // groups (barriers, shared memory): the register code runs them
        }
    }
    e.at[c.code.size()] = e.b.size();
    size_t epilogue = e.b.size();
    e.u8(0x48), e.u8(0x83), e.u8(0xC4), e.u8(40);  // add rsp, 40
    e.pop(R14);
    e.pop(R13);
    e.pop(R12);
    e.pop(RBX);
    e.u8(0xC3);
    for (auto& f : e.fixes) {
        size_t to = f.target < 0 ? epilogue : (size_t)f.target <= c.code.size() ? e.at[(size_t)f.target] : epilogue;
        int32_t rel = (int32_t)((int64_t)to - (int64_t)(f.pos + 4));
        std::memcpy(&e.b[f.pos], &rel, 4);
    }
    return (KernelFn)jitAllocExec(e.b);
}

#elif defined(__aarch64__) || defined(_M_ARM64)
#define IXO_KCODE 1
// x19 = registers, x20 = the context, x21 = the thread; x0..x3, x9 and s0/s1 (d0/d1) for work
struct Emit {
    std::vector<uint32_t> w;
    std::vector<size_t> at;
    struct Fix {
        size_t pos;
        int target;  // instruction (-1: epilogue)
        int kind;    // 0 B (imm26), 1 B.cond / CBZ (imm19)
    };
    std::vector<Fix> fixes;
    bool ok = true;
    void ins(uint32_t x) { w.push_back(x); }
    // LDR/STR with an unsigned scaled offset: base opcode for the size, off in bytes
    void ldst(uint32_t op, int t, int n, int32_t off, int size) {
        if (off < 0 || off % size || off / size > 4095) {
            ok = false;
            return;
        }
        ins(op | ((uint32_t)(off / size) << 10) | ((uint32_t)n << 5) | (uint32_t)t);
    }
    void ldr(bool x, int t, int n, int32_t off) { ldst(x ? 0xF9400000u : 0xB9400000u, t, n, off, x ? 8 : 4); }
    void str(bool x, int t, int n, int32_t off) { ldst(x ? 0xF9000000u : 0xB9000000u, t, n, off, x ? 8 : 4); }
    void ldrF(bool d, int t, int n, int32_t off) { ldst(d ? 0xFD400000u : 0xBD400000u, t, n, off, d ? 8 : 4); }
    void strF(bool d, int t, int n, int32_t off) { ldst(d ? 0xFD000000u : 0xBD000000u, t, n, off, d ? 8 : 4); }
    void movImm(int r, uint64_t v) {  // movz + movk
        ins(0xD2800000u | ((uint32_t)(v & 0xFFFF) << 5) | (uint32_t)r);
        for (uint32_t hw = 1; hw < 4; hw++) {
            uint32_t part = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
            if (part) ins(0xF2800000u | (hw << 21) | (part << 5) | (uint32_t)r);
        }
    }
    void movRR(int d, int m) { ins(0xAA0003E0u | ((uint32_t)m << 16) | (uint32_t)d); }
    void cset(bool x, int d, int cond) {  // csinc d, zr, zr, !cond
        ins((x ? 0x9A9F07E0u : 0x1A9F07E0u) | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)d);
    }
    void bTo(int target) {
        fixes.push_back({w.size(), target, 0});
        ins(0x14000000u);
    }
    void bcondTo(int cond, int target) {
        fixes.push_back({w.size(), target, 1});
        ins(0x54000000u | (uint32_t)cond);
    }
    void cbzTo(bool x, int t, int target) {
        fixes.push_back({w.size(), target, 1});
        ins((x ? 0xB4000000u : 0x34000000u) | (uint32_t)t);
    }
    size_t bcondFwd(int cond) {
        ins(0x54000000u | (uint32_t)cond);
        return w.size() - 1;
    }
    size_t bFwd() {
        ins(0x14000000u);
        return w.size() - 1;
    }
    void land(size_t at2, bool imm19) {
        int32_t rel = (int32_t)(w.size() - at2);
        if (imm19) w[at2] |= ((uint32_t)rel & 0x7FFFF) << 5;
        else w[at2] |= (uint32_t)rel & 0x3FFFFFF;
    }
};
enum { EQ = 0, NE = 1, HS = 2, MI = 4, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };

KernelFn compileA64(const CpuCode& c, KernelHelper helper) {
    Emit e;
    const bool x = c.wide;
    const int S = x ? 8 : 4;
    auto R = [&](int32_t r) { return r * S; };
    // stp x29, x30, [sp, #-48]!; stp x19, x20, [sp, #16]; str x21, [sp, #32]
    e.ins(0xA9800000u | ((uint32_t)(-6 & 0x7F) << 15) | (30u << 10) | (31u << 5) | 29u);
    e.ins(0xA9000000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);
    e.ins(0xF9000000u | (4u << 10) | (31u << 5) | 21u);
    e.movRR(19, 0);
    e.movRR(20, 1);
    e.movRR(21, 2);
    auto call = [&](const CInst* in) {
        e.movRR(0, 19);
        e.movImm(1, (uint64_t)(uintptr_t)in);
        e.movRR(2, 20);
        e.movImm(16, (uint64_t)(uintptr_t)helper);
        e.ins(0xD63F0200u);  // blr x16
    };
    uint32_t sf = x ? 0x80000000u : 0;
    e.at.resize(c.code.size() + 1);
    for (size_t pc = 0; pc < c.code.size(); pc++) {
        e.at[pc] = e.w.size();
        const CInst& in = c.code[pc];
        switch (in.op) {
            case C_CONST:
                e.movImm(0, x ? (uint64_t)(int64_t)in.a : (uint64_t)(uint32_t)in.a);
                e.str(x, 0, 19, R(in.d));
                break;
            case C_CONSTW:
                e.movImm(0, in.a >= 0 && (size_t)in.a < c.consts.size() ? c.consts[(size_t)in.a] : 0);
                e.str(true, 0, 19, R(in.d));
                break;
            case C_MOV:
                e.ldr(x, 0, 19, R(in.a));
                e.str(x, 0, 19, R(in.d));
                break;
            case C_TID: e.str(x, 21, 19, R(in.d)); break;
            case C_LEN:
                if (in.a < 0 || in.a >= kKernelBuffers) return nullptr;
                e.ldr(true, 0, 20, kLenOff + 8 * in.a);
                e.str(x, 0, 19, R(in.d));
                break;
            case C_LOAD: case C_STORE: {
                bool ld = in.op == C_LOAD;
                int buf = ld ? in.a : in.d, idx = ld ? in.b : in.a;
                if (buf < 0 || buf >= kKernelBuffers) return nullptr;
                if (x) {
                    e.ldr(true, 0, 19, R(idx));
                } else {
                    e.ldr(false, 0, 19, R(idx));
                    e.ins(0x93407C00u | (0u << 5) | 0u);  // sxtw x0, w0
                }
                e.ldr(true, 1, 20, kLenOff + 8 * buf);
                e.ins(0xEB00001Fu | (1u << 16) | (0u << 5));  // cmp x0, x1
                size_t bad = e.bcondFwd(HS);
                e.ldr(true, 2, 20, 8 * buf);
                if (ld) {
                    e.ins((x ? 0xF8607800u : 0xB8607800u) | (0u << 16) | (2u << 5) | 3u);  // ldr r3, [x2, x0, lsl #s]
                    e.str(x, 3, 19, R(in.d));
                } else {
                    e.ldr(x, 3, 19, R(in.b));
                    e.ins((x ? 0xF8207800u : 0xB8207800u) | (0u << 16) | (2u << 5) | 3u);  // str r3, [x2, x0, lsl #s]
                }
                size_t done = e.bFwd();
                e.land(bad, true);
                call(&in);
                e.land(done, false);
                break;
            }
            case C_IADD: case C_ISUB: case C_IMUL: case C_AND: case C_OR: case C_XOR: {
                e.ldr(x, 0, 19, R(in.a));
                e.ldr(x, 1, 19, R(in.b));
                uint32_t op = in.op == C_IADD ? 0x0B000000u : in.op == C_ISUB ? 0x4B000000u
                              : in.op == C_IMUL ? 0x1B007C00u : in.op == C_AND ? 0x0A000000u
                              : in.op == C_OR ? 0x2A000000u : 0x4A000000u;
                e.ins(op | sf | (1u << 16) | (0u << 5) | 0u);
                e.str(x, 0, 19, R(in.d));
                break;
            }
            case C_FADD: case C_FSUB: case C_FMUL: case C_FDIV: {
                if (in.op == C_FDIV && in.m2) {
                    call(&in);
                    break;
                }
                e.ldrF(x, 0, 19, R(in.a));
                e.ldrF(x, 1, 19, R(in.b));
                uint32_t op = in.op == C_FADD ? 0x1E202800u : in.op == C_FSUB ? 0x1E203800u
                              : in.op == C_FMUL ? 0x1E200800u : 0x1E201800u;
                e.ins(op | (x ? 0x00400000u : 0) | (1u << 16) | (0u << 5) | 0u);
                e.strF(x, 0, 19, R(in.d));
                break;
            }
            case C_ILT: case C_ILE: case C_IGT: case C_IGE: case C_IEQ: case C_INE: {
                static const int cc[] = {LT, LE, GT, GE, EQ, NE};
                e.ldr(x, 0, 19, R(in.a));
                e.ldr(x, 1, 19, R(in.b));
                e.ins((x ? 0xEB00001Fu : 0x6B00001Fu) | (1u << 16) | (0u << 5));  // cmp
                e.cset(x, 0, cc[in.op - C_ILT]);
                e.str(x, 0, 19, R(in.d));
                break;
            }
            case C_FLT: case C_FLE: case C_FGT: case C_FGE: case C_FEQ: case C_FNE: {
                // after fcmp, unordered (NaN) sets C and V: mi, ls, gt, ge and eq are false then, ne true
                static const int cc[] = {MI, LS, GT, GE, EQ, NE};
                e.ldrF(x, 0, 19, R(in.a));
                e.ldrF(x, 1, 19, R(in.b));
                e.ins((x ? 0x1E602000u : 0x1E202000u) | (1u << 16) | (0u << 5));  // fcmp s0, s1
                e.cset(x, 0, cc[in.op - C_FLT]);
                e.str(x, 0, 19, R(in.d));
                break;
            }
            case C_SHL: case C_SHR:
                e.ldr(x, 0, 19, R(in.a));
                e.ldr(x, 1, 19, R(in.b));
                e.ins((in.op == C_SHL ? 0x1AC02000u : 0x1AC02800u) | sf | (1u << 16) | (0u << 5) | 0u);  // lslv / asrv
                e.str(x, 0, 19, R(in.d));
                break;
            case C_NOT:
                e.ldr(x, 0, 19, R(in.a));
                e.ins((x ? 0xF100001Fu : 0x7100001Fu) | (0u << 5));  // cmp r0, #0
                e.cset(x, 0, EQ);
                e.str(x, 0, 19, R(in.d));
                break;
            case C_INEG:
                e.ldr(x, 0, 19, R(in.a));
                e.ins((x ? 0xCB0003E0u : 0x4B0003E0u) | (0u << 16) | 0u);  // neg
                e.str(x, 0, 19, R(in.d));
                break;
            case C_FNEG:
                e.ldrF(x, 0, 19, R(in.a));
                e.ins((x ? 0x1E614000u : 0x1E214000u) | (0u << 5) | 0u);
                e.strF(x, 0, 19, R(in.d));
                break;
            case C_JMP: e.bTo(in.a); break;
            case C_JZ:
                e.ldr(x, 0, 19, R(in.a));
                e.cbzTo(x, 0, in.b);
                break;
            case C_RET: e.bTo(-1); break;
            case C_IDIV: case C_IMOD: case C_FIDIV: case C_FMOD: case C_FPOW: case C_MATH: case C_DIM: case C_LID:
            case C_GRP:
                call(&in);
                break;
            default: return nullptr;
        }
        if (!e.ok) return nullptr;
    }
    e.at[c.code.size()] = e.w.size();
    size_t epilogue = e.w.size();
    e.ins(0xF9400000u | (4u << 10) | (31u << 5) | 21u);              // ldr x21, [sp, #32]
    e.ins(0xA9400000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);  // ldp x19, x20, [sp, #16]
    e.ins(0xA8C00000u | (6u << 15) | (30u << 10) | (31u << 5) | 29u);  // ldp x29, x30, [sp], #48
    e.ins(0xD65F03C0u);                                                // ret
    for (auto& f : e.fixes) {
        size_t to = f.target < 0 ? epilogue : (size_t)f.target <= c.code.size() ? e.at[(size_t)f.target] : epilogue;
        int32_t rel = (int32_t)((int64_t)to - (int64_t)f.pos);
        if (f.kind == 0) e.w[f.pos] |= (uint32_t)rel & 0x3FFFFFF;
        else e.w[f.pos] |= ((uint32_t)rel & 0x7FFFF) << 5;
    }
    std::vector<uint8_t> bytes(e.w.size() * 4);
    std::memcpy(bytes.data(), e.w.data(), bytes.size());
    return (KernelFn)jitAllocExec(bytes);
}
#endif

}  // namespace

KernelFn kernelMachineCode(const CpuCode& c, KernelHelper helper) {
#ifdef IXO_KCODE
    if (!jitSupported() || c.groups || c.code.empty()) return nullptr;
#if defined(__x86_64__) || defined(_M_X64)
    return compileX64(c, helper);
#else
    return compileA64(c, helper);
#endif
#else
    (void)c;
    (void)helper;
    return nullptr;
#endif
}

}  // namespace ixo
