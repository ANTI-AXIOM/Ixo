// The CPU side of gpu fn kernels (gpu.cpp): register code that runs a kernel's
// threads on every core, and its machine code (kcode.cpp).
//
// Kernels run in one of two widths:
// - narrow (the GPU's semantics): 32-bit ints and floats, as on the device;
// - wide (Ixo's semantics): 64-bit ints and doubles, for maps the compiler moves
//   to all cores by itself (they must give exactly what the plain loop gives).
#pragma once
#include <cstdint>
#include <vector>

namespace ixo {

union GWord {
    int32_t i;
    float f;
    uint32_t u;
};
union GWide {
    int64_t i;
    double f;
    uint64_t u;
};

// The kernel as register code: each variable is a register, temporaries follow.
enum COp : uint8_t {
    C_CONST, C_MOV, C_TID, C_LEN, C_LOAD, C_STORE, C_IADD, C_ISUB, C_IMUL, C_FADD, C_FSUB, C_FMUL, C_FDIV, C_IDIV,
    C_IMOD, C_FIDIV, C_FMOD, C_FPOW, C_ILT, C_ILE, C_IGT, C_IGE, C_IEQ, C_INE, C_FLT, C_FLE, C_FGT, C_FGE, C_FEQ,
    C_FNE, C_AND, C_OR, C_XOR, C_SHL, C_SHR, C_NOT, C_INEG, C_FNEG, C_MATH, C_JMP, C_JZ, C_RET,
    // added with groups: the thread's place in its group of 64, the group, a launch dimension (m: 0 x, 1 y, 2 z)
    C_LID, C_GRP, C_DIM,
    C_BAR,     // wait for the group's other threads (gpu.barrier())
    C_SLOAD,   // d = shared array a [index register b]
    C_SSTORE,  // shared array d [index register a] = register b
    C_CONSTW,  // wide: d = consts[a] (64 bits)
};
struct CInst {
    COp op;
    uint8_t m;  // C_MATH: GMath; C_DIM: the dimension
    uint8_t m2;  // C_MATH: 1 when the operand is an int
    int32_t d, a, b;
};
struct CpuCode {
    std::vector<CInst> code;
    int regs = 0;
    bool wide = false;
    bool groups = false;               // uses barriers or shared memory: runs a group at a time
    std::vector<uint64_t> consts;      // wide constants (C_CONSTW)
};

// What machine code for a kernel sees: buffer storage and lengths, then the run's
// own data for the helpers it calls.
constexpr int kKernelBuffers = 16;
struct KCtx {
    void* data[kKernelBuffers];
    uint64_t len[kKernelBuffers];
    void* run;  // the CPU run (helpers report bad indexes there)
};
// One thread: regs (GWord or GWide registers), its index.
using KernelFn = void (*)(void* regs, KCtx* ctx, int64_t tid);
// The helpers machine code calls for the rarer operations (division, math,
// dimensions) and for an index out of range: they do the instruction `in`.
using KernelHelper = void (*)(void* regs, const CInst* in, KCtx* ctx);

// Machine code for register code (null: this machine or this code can't; the
// register code runs as it is). The code lives as long as the program.
KernelFn kernelMachineCode(const CpuCode& c, KernelHelper helper);

}  // namespace ixo
