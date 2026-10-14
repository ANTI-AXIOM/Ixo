// SPIR-V (1.3, for Vulkan 1.1+) for gpu fn kernels (see gpu.hpp).
//
// The kernel's tree becomes one compute entry point "main" with work groups of
// 64. Bindings 0..buffers-1 are the buffer parameters (runtime arrays of 32-bit
// floats or ints); binding `buffers` is a status buffer: [0] set when an index was
// out of range, [1] that index, [2] which parameter. Push constants: one 32-bit
// word per scalar parameter (bools as 0/1), then the thread count n, then the
// stride of the second dispatch dimension (thread = x + y * stride).
#include <cstdio>
#include <cstring>
#include <map>

#include "gpu.hpp"

namespace ixo {

namespace {

// opcodes and enums used here (SPIR-V 1.3 specification)
enum : uint32_t {
    OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41,
    OpConstantFalse = 42, OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61,
    OpStore = 62, OpAccessChain = 65, OpArrayLength = 68, OpDecorate = 71, OpMemberDecorate = 72,
    OpCompositeExtract = 81, OpConvertFToS = 110, OpConvertSToF = 111, OpBitcast = 124, OpSNegate = 126,
    OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpISub = 130, OpFSub = 131, OpIMul = 132, OpFMul = 133,
    OpSDiv = 135, OpFDiv = 136, OpSRem = 138, OpIsNan = 156, OpLogicalEqual = 164, OpLogicalNotEqual = 165,
    OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171,
    OpUGreaterThanEqual = 174, OpSGreaterThan = 173, OpSGreaterThanEqual = 175, OpULessThan = 176,
    OpSLessThan = 177, OpSLessThanEqual = 179, OpFOrdEqual = 180, OpFUnordNotEqual = 183, OpFOrdLessThan = 184,
    OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190,
    OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseXor = 198,
    OpBitwiseAnd = 199, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpReturn = 253, OpUnreachable = 255,
    // storage classes
    SC_Input = 1, SC_PushConstant = 9, SC_StorageBuffer = 12, SC_Function = 7,
    // decorations
    D_Block = 2, D_ArrayStride = 6, D_BuiltIn = 11, D_Binding = 33, D_DescriptorSet = 34, D_Offset = 35,
    BI_GlobalInvocationId = 28,
    // GLSL.std.450
    G_FAbs = 4, G_SAbs = 5, G_Floor = 8, G_Ceil = 9, G_Sin = 13, G_Cos = 14, G_Tan = 15, G_Pow = 26, G_Exp = 27,
    G_Log = 28, G_Sqrt = 31, G_FMin = 37, G_SMin = 39, G_FMax = 40, G_SMax = 42, G_FClamp = 43,
};

struct Spv {
    const GpuKernel& k;
    std::vector<uint32_t> head, decor, globals, fnVars, body;
    uint32_t next = 1;
    // types
    uint32_t tVoid, tBool, tInt, tUint, tFloat, tV3, tFnVoid, tRtaF, tRtaI, tSbF, tSbI, pSbF, pSbI, pElF, pElI, pIn,
        tPc, pPc, pPcI, pPcU, pPcF, pFnI, pFnF, pFnB, ext;
    uint32_t gid, pcVar, statusVar;
    std::vector<uint32_t> bufVar;  // by parameter index (0 for scalars)
    std::vector<uint32_t> varPtr;  // by kernel variable
    uint32_t tid = 0;
    std::map<uint64_t, uint32_t> consts;
    uint32_t cTrue = 0, cFalse = 0;
    // loops being emitted: (continue target, merge)
    std::vector<std::pair<uint32_t, uint32_t>> loopStack;
    bool ended = false;  // the current block has its terminator

    explicit Spv(const GpuKernel& kk) : k(kk) {}

    uint32_t id() { return next++; }
    static void op(std::vector<uint32_t>& out, uint32_t code, std::initializer_list<uint32_t> words) {
        out.push_back(((uint32_t)(words.size() + 1) << 16) | code);
        out.insert(out.end(), words);
    }
    static void opv(std::vector<uint32_t>& out, uint32_t code, const std::vector<uint32_t>& words) {
        out.push_back(((uint32_t)(words.size() + 1) << 16) | code);
        out.insert(out.end(), words.begin(), words.end());
    }
    static std::vector<uint32_t> str(const char* s) {  // a literal string: words, nul-terminated
        std::vector<uint32_t> w;
        size_t n = std::strlen(s) + 1;
        w.resize((n + 3) / 4, 0);
        std::memcpy(w.data(), s, n);
        return w;
    }
    uint32_t cInt(int32_t v) {
        uint64_t key = (1ull << 32) | (uint32_t)v;
        auto it = consts.find(key);
        if (it != consts.end()) return it->second;
        uint32_t r = id();
        op(globals, OpConstant, {tInt, r, (uint32_t)v});
        return consts[key] = r;
    }
    uint32_t cUint(uint32_t v) {
        uint64_t key = (2ull << 32) | v;
        auto it = consts.find(key);
        if (it != consts.end()) return it->second;
        uint32_t r = id();
        op(globals, OpConstant, {tUint, r, v});
        return consts[key] = r;
    }
    uint32_t cFloat(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        uint64_t key = (3ull << 32) | bits;
        auto it = consts.find(key);
        if (it != consts.end()) return it->second;
        uint32_t r = id();
        op(globals, OpConstant, {tFloat, r, bits});
        return consts[key] = r;
    }
    uint32_t tyOf(GTy t) const { return t == GTy::Float ? tFloat : t == GTy::Bool ? tBool : tInt; }
    uint32_t un(uint32_t code, uint32_t type, uint32_t a) {
        uint32_t r = id();
        op(body, code, {type, r, a});
        return r;
    }
    uint32_t bin(uint32_t code, uint32_t type, uint32_t a, uint32_t b) {
        uint32_t r = id();
        op(body, code, {type, r, a, b});
        return r;
    }
    uint32_t sel(uint32_t type, uint32_t c, uint32_t a, uint32_t b) {
        uint32_t r = id();
        op(body, OpSelect, {type, r, c, a, b});
        return r;
    }
    uint32_t glsl(uint32_t type, uint32_t inst, std::initializer_list<uint32_t> args) {
        uint32_t r = id();
        std::vector<uint32_t> w = {type, r, ext, inst};
        w.insert(w.end(), args);
        opv(body, OpExtInst, w);
        return r;
    }
    void label(uint32_t l) {
        op(body, OpLabel, {l});
        ended = false;
    }
    void branch(uint32_t to) {
        if (ended) return;
        op(body, OpBranch, {to});
        ended = true;
    }

    void types() {
        tVoid = id();
        tBool = id();
        tInt = id();
        tUint = id();
        tFloat = id();
        tV3 = id();
        tFnVoid = id();
        tRtaF = id();
        tRtaI = id();
        tSbF = id();
        tSbI = id();
        pSbF = id();
        pSbI = id();
        pElF = id();
        pElI = id();
        pIn = id();
        tPc = id();
        pPc = id();
        pPcI = id();
        pPcU = id();
        pPcF = id();
        pFnI = id();
        pFnF = id();
        pFnB = id();
        op(globals, OpTypeVoid, {tVoid});
        op(globals, OpTypeBool, {tBool});
        op(globals, OpTypeInt, {tInt, 32, 1});
        op(globals, OpTypeInt, {tUint, 32, 0});
        op(globals, OpTypeFloat, {tFloat, 32});
        op(globals, OpTypeVector, {tV3, tUint, 3});
        op(globals, OpTypeFunction, {tFnVoid, tVoid});
        op(globals, OpTypeRuntimeArray, {tRtaF, tFloat});
        op(globals, OpTypeRuntimeArray, {tRtaI, tInt});
        op(globals, OpTypeStruct, {tSbF, tRtaF});
        op(globals, OpTypeStruct, {tSbI, tRtaI});
        op(globals, OpTypePointer, {pSbF, SC_StorageBuffer, tSbF});
        op(globals, OpTypePointer, {pSbI, SC_StorageBuffer, tSbI});
        op(globals, OpTypePointer, {pElF, SC_StorageBuffer, tFloat});
        op(globals, OpTypePointer, {pElI, SC_StorageBuffer, tInt});
        op(globals, OpTypePointer, {pIn, SC_Input, tV3});
        // push constants: the scalars, then n and the stride
        std::vector<uint32_t> members = {tPc};
        uint32_t m = 0;
        for (auto& p : k.params)
            if (p.t != GTy::Buffer) {
                members.push_back(p.t == GTy::Float ? tFloat : tInt);
                op(decor, OpMemberDecorate, {tPc, m, D_Offset, 4 * m});
                m++;
            }
        members.push_back(tUint);
        op(decor, OpMemberDecorate, {tPc, m, D_Offset, 4 * m});
        m++;
        members.push_back(tUint);
        op(decor, OpMemberDecorate, {tPc, m, D_Offset, 4 * m});
        opv(globals, OpTypeStruct, members);
        op(globals, OpTypePointer, {pPc, SC_PushConstant, tPc});
        op(globals, OpTypePointer, {pPcI, SC_PushConstant, tInt});
        op(globals, OpTypePointer, {pPcU, SC_PushConstant, tUint});
        op(globals, OpTypePointer, {pPcF, SC_PushConstant, tFloat});
        op(globals, OpTypePointer, {pFnI, SC_Function, tInt});
        op(globals, OpTypePointer, {pFnF, SC_Function, tFloat});
        op(globals, OpTypePointer, {pFnB, SC_Function, tBool});
        op(decor, OpDecorate, {tRtaF, D_ArrayStride, 4});
        op(decor, OpDecorate, {tRtaI, D_ArrayStride, 4});
        op(decor, OpDecorate, {tSbF, D_Block});
        op(decor, OpDecorate, {tSbI, D_Block});
        op(decor, OpMemberDecorate, {tSbF, 0, D_Offset, 0});
        op(decor, OpMemberDecorate, {tSbI, 0, D_Offset, 0});
        op(decor, OpDecorate, {tPc, D_Block});
        cTrue = id();
        cFalse = id();
        op(globals, OpConstantTrue, {tBool, cTrue});
        op(globals, OpConstantFalse, {tBool, cFalse});
        // variables
        gid = id();
        op(globals, OpVariable, {pIn, gid, SC_Input});
        op(decor, OpDecorate, {gid, D_BuiltIn, BI_GlobalInvocationId});
        uint32_t binding = 0;
        bufVar.assign(k.params.size(), 0);
        for (size_t i = 0; i < k.params.size(); i++)
            if (k.params[i].t == GTy::Buffer) {
                uint32_t v = id();
                op(globals, OpVariable, {k.params[i].elem == GTy::Float ? pSbF : pSbI, v, SC_StorageBuffer});
                op(decor, OpDecorate, {v, D_DescriptorSet, 0});
                op(decor, OpDecorate, {v, D_Binding, binding++});
                bufVar[i] = v;
            }
        statusVar = id();
        op(globals, OpVariable, {pSbI, statusVar, SC_StorageBuffer});
        op(decor, OpDecorate, {statusVar, D_DescriptorSet, 0});
        op(decor, OpDecorate, {statusVar, D_Binding, binding});
        pcVar = id();
        op(globals, OpVariable, {pPc, pcVar, SC_PushConstant});
    }

    uint32_t pcLoad(uint32_t member, uint32_t ptrType, uint32_t type) {
        uint32_t p = id();
        op(body, OpAccessChain, {ptrType, p, pcVar, cInt((int32_t)member)});
        uint32_t r = id();
        op(body, OpLoad, {type, r, p});
        return r;
    }
    // index i (int) of buffer parameter b: in range? (one unsigned compare covers negatives)
    uint32_t inRange(int b, uint32_t i) {
        uint32_t len = id();
        op(body, OpArrayLength, {tUint, len, bufVar[(size_t)b], 0});
        uint32_t ui = un(OpBitcast, tUint, i);
        return bin(OpULessThan, tBool, ui, len);
    }
    // status: the first bad index wins (races keep one of them: any is fine)
    void recordBad(int b, uint32_t i) {
        uint32_t p0 = id();
        op(body, OpAccessChain, {pElI, p0, statusVar, cInt(0), cInt(0)});
        uint32_t cur = id();
        op(body, OpLoad, {tInt, cur, p0});
        uint32_t first = bin(OpIEqual, tBool, cur, cInt(0));
        uint32_t yes = id(), done = id();
        op(body, OpSelectionMerge, {done, 0});
        op(body, OpBranchConditional, {first, yes, done});
        label(yes);
        op(body, OpStore, {p0, cInt(1)});
        uint32_t p1 = id();
        op(body, OpAccessChain, {pElI, p1, statusVar, cInt(0), cInt(1)});
        op(body, OpStore, {p1, i});
        uint32_t p2 = id();
        op(body, OpAccessChain, {pElI, p2, statusVar, cInt(0), cInt(2)});
        op(body, OpStore, {p2, cInt(b)});
        branch(done);
        label(done);
    }

    uint32_t expr(const GNode& n) {
        switch (n.k) {
            case GNode::IntK: return cInt((int32_t)n.ival);
            case GNode::FloatK: return cFloat((float)n.fval);
            case GNode::BoolK: return n.ival ? cTrue : cFalse;
            case GNode::Var: {
                uint32_t r = id();
                op(body, OpLoad, {tyOf(n.t), r, varPtr[(size_t)n.slot]});
                return r;
            }
            case GNode::ThreadIdx: return tid;
            case GNode::Len: {
                uint32_t len = id();
                op(body, OpArrayLength, {tUint, len, bufVar[(size_t)n.slot], 0});
                return un(OpBitcast, tInt, len);
            }
            case GNode::Load: {
                // through a variable: the value, or 0 (and the status) when out of range
                uint32_t i = expr(*n.kids[0]);
                bool f = k.params[(size_t)n.slot].elem == GTy::Float;
                uint32_t tmp = id();
                op(fnVars, OpVariable, {f ? pFnF : pFnI, tmp, SC_Function});
                uint32_t ok = inRange(n.slot, i);
                uint32_t yes = id(), no = id(), done = id();
                op(body, OpSelectionMerge, {done, 0});
                op(body, OpBranchConditional, {ok, yes, no});
                label(yes);
                uint32_t p = id();
                op(body, OpAccessChain, {f ? pElF : pElI, p, bufVar[(size_t)n.slot], cInt(0), i});
                uint32_t v = id();
                op(body, OpLoad, {f ? tFloat : tInt, v, p});
                op(body, OpStore, {tmp, v});
                branch(done);
                label(no);
                op(body, OpStore, {tmp, f ? cFloat(0) : cInt(0)});
                recordBad(n.slot, i);
                branch(done);
                label(done);
                uint32_t r = id();
                op(body, OpLoad, {f ? tFloat : tInt, r, tmp});
                return r;
            }
            case GNode::Neg: {
                uint32_t a = expr(*n.kids[0]);
                return un(n.t == GTy::Int ? OpSNegate : OpFNegate, tyOf(n.t), a);
            }
            case GNode::Not: return un(OpLogicalNot, tBool, expr(*n.kids[0]));
            case GNode::Math: return math(n);
            case GNode::Bin: return binary(n);
            default: return cInt(0);
        }
    }
    uint32_t math(const GNode& n) {
        uint32_t x = expr(*n.kids[0]);
        uint32_t y = n.kids.size() > 1 ? expr(*n.kids[1]) : 0;
        bool ints = n.kids[0]->t == GTy::Int;
        switch (n.op) {
            case GM_Sqrt: return glsl(tFloat, G_Sqrt, {x});
            case GM_Abs: return ints ? glsl(tInt, G_SAbs, {x}) : glsl(tFloat, G_FAbs, {x});
            case GM_Min: return ints ? glsl(tInt, G_SMin, {x, y}) : glsl(tFloat, G_FMin, {x, y});
            case GM_Max: return ints ? glsl(tInt, G_SMax, {x, y}) : glsl(tFloat, G_FMax, {x, y});
            case GM_Floor: return glsl(tFloat, G_Floor, {x});
            case GM_Ceil: return glsl(tFloat, G_Ceil, {x});
            case GM_Sin: return glsl(tFloat, G_Sin, {x});
            case GM_Cos: return glsl(tFloat, G_Cos, {x});
            case GM_Tan: return glsl(tFloat, G_Tan, {x});
            case GM_Exp: return glsl(tFloat, G_Exp, {x});
            case GM_Log: return glsl(tFloat, G_Log, {x});
            case GM_Pow: return glsl(tFloat, G_Pow, {x, y});
            case GM_ToFloat: return un(OpConvertSToF, tFloat, x);
            case GM_ToInt: {  // as the CPU fallback: toward zero, clamped, NaN 0
                uint32_t c = glsl(tFloat, G_FClamp, {x, cFloat(-2147483648.0f), cFloat(2147483520.0f)});
                uint32_t r = un(OpConvertFToS, tInt, c);
                uint32_t big = bin(OpFOrdGreaterThanEqual, tBool, x, cFloat(2147483648.0f));
                r = sel(tInt, big, cInt(INT32_MAX), r);
                uint32_t nan = un(OpIsNan, tBool, x);
                return sel(tInt, nan, cInt(0), r);
            }
        }
        return x;
    }
    uint32_t binary(const GNode& n) {
        GTy lt = n.kids[0]->t;
        uint32_t a = expr(*n.kids[0]);
        uint32_t b = expr(*n.kids[1]);
        bool ints = lt == GTy::Int, bools = lt == GTy::Bool;
        uint32_t ty = tyOf(n.t);
        switch (n.op) {
            case GB_Add: return bin(ints ? OpIAdd : OpFAdd, ty, a, b);
            case GB_Sub: return bin(ints ? OpISub : OpFSub, ty, a, b);
            case GB_Mul: return bin(ints ? OpIMul : OpFMul, ty, a, b);
            case GB_Div: return bin(OpFDiv, tFloat, a, b);
            case GB_Pow: return glsl(tFloat, G_Pow, {a, b});
            case GB_IntDiv: case GB_Mod: {
                if (!ints) {  // floats: floor(a / b), a - b * floor(a / b)
                    uint32_t q = glsl(tFloat, G_Floor, {bin(OpFDiv, tFloat, a, b)});
                    if (n.op == GB_IntDiv) return q;
                    return bin(OpFSub, tFloat, a, bin(OpFMul, tFloat, b, q));
                }
                // ints, floored, as the CPU fallback: by 0 gives 0; by -1 negates (or 0 for %)
                uint32_t bz = bin(OpIEqual, tBool, b, cInt(0));
                uint32_t bm1 = bin(OpIEqual, tBool, b, cInt(-1));
                uint32_t odd = bin(OpLogicalOr, tBool, bz, bm1);
                uint32_t safe = sel(tInt, odd, cInt(1), b);
                uint32_t r = bin(OpSRem, tInt, a, safe);
                uint32_t rnz = bin(OpINotEqual, tBool, r, cInt(0));
                uint32_t rneg = bin(OpSLessThan, tBool, r, cInt(0));
                uint32_t bneg = bin(OpSLessThan, tBool, safe, cInt(0));
                uint32_t diff = bin(OpLogicalNotEqual, tBool, rneg, bneg);
                uint32_t adj = bin(OpLogicalAnd, tBool, rnz, diff);
                if (n.op == GB_Mod) {
                    uint32_t m = sel(tInt, adj, bin(OpIAdd, tInt, r, safe), r);
                    return sel(tInt, odd, cInt(0), m);
                }
                uint32_t q = bin(OpSDiv, tInt, a, safe);
                q = sel(tInt, adj, bin(OpISub, tInt, q, cInt(1)), q);
                q = sel(tInt, bm1, un(OpSNegate, tInt, a), q);
                return sel(tInt, bz, cInt(0), q);
            }
            case GB_Lt: return bin(ints ? OpSLessThan : OpFOrdLessThan, tBool, a, b);
            case GB_Le: return bin(ints ? OpSLessThanEqual : OpFOrdLessThanEqual, tBool, a, b);
            case GB_Gt: return bin(ints ? OpSGreaterThan : OpFOrdGreaterThan, tBool, a, b);
            case GB_Ge: return bin(ints ? OpSGreaterThanEqual : OpFOrdGreaterThanEqual, tBool, a, b);
            case GB_Eq: return bin(bools ? OpLogicalEqual : ints ? OpIEqual : OpFOrdEqual, tBool, a, b);
            case GB_Ne: return bin(bools ? OpLogicalNotEqual : ints ? OpINotEqual : OpFUnordNotEqual, tBool, a, b);
            case GB_And: return bin(OpLogicalAnd, tBool, a, b);
            case GB_Or: return bin(OpLogicalOr, tBool, a, b);
            case GB_BitAnd: return bin(OpBitwiseAnd, tInt, a, b);
            case GB_BitOr: return bin(OpBitwiseOr, tInt, a, b);
            case GB_BitXor: return bin(OpBitwiseXor, tInt, a, b);
            case GB_Shl: return bin(OpShiftLeftLogical, tInt, a, bin(OpBitwiseAnd, tInt, b, cInt(31)));
            case GB_Shr: return bin(OpShiftRightArithmetic, tInt, a, bin(OpBitwiseAnd, tInt, b, cInt(31)));
        }
        return a;
    }

    void stmt(const GNode& n) {
        if (ended) return;  // after break/continue/return: nothing more in this block
        switch (n.k) {
            case GNode::Set: {
                uint32_t v = expr(*n.kids[0]);
                op(body, OpStore, {varPtr[(size_t)n.slot], v});
                return;
            }
            case GNode::Store: {
                uint32_t i = expr(*n.kids[0]);
                uint32_t v = expr(*n.kids[1]);
                bool f = k.params[(size_t)n.slot].elem == GTy::Float;
                uint32_t ok = inRange(n.slot, i);
                uint32_t yes = id(), no = id(), done = id();
                op(body, OpSelectionMerge, {done, 0});
                op(body, OpBranchConditional, {ok, yes, no});
                label(yes);
                uint32_t p = id();
                op(body, OpAccessChain, {f ? pElF : pElI, p, bufVar[(size_t)n.slot], cInt(0), i});
                op(body, OpStore, {p, v});
                branch(done);
                label(no);
                recordBad(n.slot, i);
                branch(done);
                label(done);
                return;
            }
            case GNode::Block:
                for (auto& c : n.kids) stmt(*c);
                return;
            case GNode::If: {
                uint32_t c = expr(*n.kids[0]);
                uint32_t then = id(), els = id(), done = id();
                op(body, OpSelectionMerge, {done, 0});
                op(body, OpBranchConditional, {c, then, els});
                label(then);
                stmt(*n.kids[1]);
                bool thenEnded = ended;
                branch(done);
                label(els);
                stmt(*n.kids[2]);
                bool elseEnded = ended;
                branch(done);
                label(done);
                if (thenEnded && elseEnded) {  // nothing reaches here
                    op(body, OpUnreachable, {});
                    ended = true;
                }
                return;
            }
            case GNode::While: {
                uint32_t header = id(), check = id(), loopBody = id(), cont = id(), merge = id();
                branch(header);
                label(header);
                op(body, OpLoopMerge, {merge, cont, 0});
                branch(check);
                label(check);
                uint32_t c = expr(*n.kids[0]);
                op(body, OpBranchConditional, {c, loopBody, merge});
                ended = true;
                label(loopBody);
                loopStack.push_back({cont, merge});
                stmt(*n.kids[1]);
                loopStack.pop_back();
                branch(cont);
                label(cont);
                if (n.kids.size() > 2) stmt(*n.kids[2]);
                branch(header);
                label(merge);
                return;
            }
            case GNode::Break: branch(loopStack.back().second); return;
            case GNode::Continue: branch(loopStack.back().first); return;
            case GNode::Return:
                op(body, OpReturn, {});
                ended = true;
                return;
            default: return;
        }
    }

    std::vector<uint32_t> build() {
        ext = id();
        types();
        uint32_t mainFn = id();
        // the function
        uint32_t entry = id();
        // variables: one per kernel variable
        varPtr.resize(k.vars.size());
        for (size_t s = 0; s < k.vars.size(); s++) {
            varPtr[s] = id();
            GTy t = k.vars[s];
            op(fnVars, OpVariable, {t == GTy::Float ? pFnF : t == GTy::Bool ? pFnB : pFnI, varPtr[s], SC_Function});
        }
        // thread = x + y * stride; past n: nothing
        uint32_t g = id();
        op(body, OpLoad, {tV3, g, gid});
        uint32_t x = id(), y = id();
        op(body, OpCompositeExtract, {tUint, x, g, 0});
        op(body, OpCompositeExtract, {tUint, y, g, 1});
        uint32_t scalars = 0;
        for (auto& p : k.params) scalars += p.t != GTy::Buffer;
        uint32_t nThreads = pcLoad(scalars, pPcU, tUint);
        uint32_t stride = pcLoad(scalars + 1, pPcU, tUint);
        uint32_t ut = bin(OpIAdd, tUint, x, bin(OpIMul, tUint, y, stride));
        tid = un(OpBitcast, tInt, ut);
        // the scalar parameters into their variables
        uint32_t m = 0, slot = 0;
        for (auto& p : k.params) {
            if (p.t == GTy::Buffer) continue;
            uint32_t v = pcLoad(m, p.t == GTy::Float ? pPcF : pPcI, p.t == GTy::Float ? tFloat : tInt);
            if (p.t == GTy::Bool) v = bin(OpINotEqual, tBool, v, cInt(0));
            op(body, OpStore, {varPtr[slot], v});
            m++;
            slot++;
        }
        uint32_t live = bin(OpULessThan, tBool, ut, nThreads);
        uint32_t run = id(), done = id();
        op(body, OpSelectionMerge, {done, 0});
        op(body, OpBranchConditional, {live, run, done});
        label(run);
        stmt(*k.body);
        branch(done);
        label(done);
        op(body, OpReturn, {});
        // the module
        std::vector<uint32_t> out = {0x07230203u, 0x00010300u, 0, 0 /* bound */, 0};
        op(out, OpCapability, {1 /* Shader */});
        std::vector<uint32_t> imp = {ext};
        auto gl = str("GLSL.std.450");
        imp.insert(imp.end(), gl.begin(), gl.end());
        opv(out, OpExtInstImport, imp);
        op(out, OpMemoryModel, {0 /* Logical */, 1 /* GLSL450 */});
        std::vector<uint32_t> ep = {5 /* GLCompute */, mainFn};
        auto nm = str("main");
        ep.insert(ep.end(), nm.begin(), nm.end());
        ep.push_back(gid);
        opv(out, OpEntryPoint, ep);
        op(out, OpExecutionMode, {mainFn, 17 /* LocalSize */, 64, 1, 1});
        out.insert(out.end(), decor.begin(), decor.end());
        out.insert(out.end(), globals.begin(), globals.end());
        op(out, OpFunction, {tVoid, mainFn, 0, tFnVoid});
        op(out, OpLabel, {entry});
        out.insert(out.end(), fnVars.begin(), fnVars.end());
        out.insert(out.end(), body.begin(), body.end());
        op(out, OpFunctionEnd, {});
        out[3] = next;
        return out;
    }
};

}  // namespace

std::vector<uint32_t> spirvFor(const GpuKernel& k) {
    Spv s(k);
    return s.build();
}

// ------------------------------------------------------------------ WGSL (WebGPU)
//
// The same kernel as text: bindings 0..buffers-1 the buffers, `buffers` the
// status words, `buffers + 1` a uniform with the scalars, n and the stride
// (WebGPU has no push constants). Helper functions keep the semantics of the
// SPIR-V and the CPU fallback: checked loads and stores, floored // and %.
namespace {

struct Wgsl {
    const GpuKernel& k;
    std::string out;
    int depth = 1;
    explicit Wgsl(const GpuKernel& kk) : k(kk) {}

    static const char* ty(GTy t) { return t == GTy::Float ? "f32" : t == GTy::Bool ? "bool" : "i32"; }
    static std::string flt(double v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", (double)(float)v);
        std::string t = buf;
        if (t.find_first_of(".eE") == std::string::npos) t += ".0";
        return t;
    }
    void line(const std::string& s) { out += std::string((size_t)depth * 4, ' ') + s + "\n"; }
    std::string expr(const GNode& n) {
        switch (n.k) {
            case GNode::IntK: return n.ival < 0 ? "(" + std::to_string(n.ival) + ")" : std::to_string(n.ival);
            case GNode::FloatK: return n.fval < 0 ? "(" + flt(n.fval) + ")" : flt(n.fval);
            case GNode::BoolK: return n.ival ? "true" : "false";
            case GNode::Var: return "v" + std::to_string(n.slot);
            case GNode::ThreadIdx: return "tid";
            case GNode::Len: return "i32(arrayLength(&b" + std::to_string(n.slot) + "))";
            case GNode::Load: return "ld" + std::to_string(n.slot) + "(" + expr(*n.kids[0]) + ")";
            case GNode::Neg: return "(-" + expr(*n.kids[0]) + ")";
            case GNode::Not: return "(!" + expr(*n.kids[0]) + ")";
            case GNode::Math: {
                std::string x = expr(*n.kids[0]);
                std::string y = n.kids.size() > 1 ? expr(*n.kids[1]) : "";
                switch (n.op) {
                    case GM_Sqrt: return "sqrt(" + x + ")";
                    case GM_Abs: return "abs(" + x + ")";
                    case GM_Min: return "min(" + x + ", " + y + ")";
                    case GM_Max: return "max(" + x + ", " + y + ")";
                    case GM_Floor: return "floor(" + x + ")";
                    case GM_Ceil: return "ceil(" + x + ")";
                    case GM_Sin: return "sin(" + x + ")";
                    case GM_Cos: return "cos(" + x + ")";
                    case GM_Tan: return "tan(" + x + ")";
                    case GM_Exp: return "exp(" + x + ")";
                    case GM_Log: return "log(" + x + ")";
                    case GM_Pow: return "pow(" + x + ", " + y + ")";
                    case GM_ToFloat: return "f32(" + x + ")";
                    case GM_ToInt: return "toint(" + x + ")";
                }
                return x;
            }
            case GNode::Bin: {
                std::string a = expr(*n.kids[0]), b = expr(*n.kids[1]);
                bool ints = n.kids[0]->t == GTy::Int;
                switch (n.op) {
                    case GB_Add: return "(" + a + " + " + b + ")";
                    case GB_Sub: return "(" + a + " - " + b + ")";
                    case GB_Mul: return "(" + a + " * " + b + ")";
                    case GB_Div: return "(" + a + " / " + b + ")";
                    case GB_Pow: return "pow(" + a + ", " + b + ")";
                    case GB_IntDiv: return ints ? "idiv(" + a + ", " + b + ")" : "floor(" + a + " / " + b + ")";
                    case GB_Mod: return ints ? "imod(" + a + ", " + b + ")" : "fmodf(" + a + ", " + b + ")";
                    case GB_Lt: return "(" + a + " < " + b + ")";
                    case GB_Le: return "(" + a + " <= " + b + ")";
                    case GB_Gt: return "(" + a + " > " + b + ")";
                    case GB_Ge: return "(" + a + " >= " + b + ")";
                    case GB_Eq: return "(" + a + " == " + b + ")";
                    case GB_Ne: return "(" + a + " != " + b + ")";
                    case GB_And: return "(" + a + " && " + b + ")";
                    case GB_Or: return "(" + a + " || " + b + ")";
                    case GB_BitAnd: return "(" + a + " & " + b + ")";
                    case GB_BitOr: return "(" + a + " | " + b + ")";
                    case GB_BitXor: return "(" + a + " ^ " + b + ")";
                    case GB_Shl: return "(" + a + " << u32(" + b + " & 31))";
                    case GB_Shr: return "(" + a + " >> u32(" + b + " & 31))";
                }
                return a;
            }
            default: return "0";
        }
    }
    void stmt(const GNode& n) {
        switch (n.k) {
            case GNode::Set: line("v" + std::to_string(n.slot) + " = " + expr(*n.kids[0]) + ";"); return;
            case GNode::Store:
                line("st" + std::to_string(n.slot) + "(" + expr(*n.kids[0]) + ", " + expr(*n.kids[1]) + ");");
                return;
            case GNode::Block:
                for (auto& c : n.kids) stmt(*c);
                return;
            case GNode::If:
                line("if (" + expr(*n.kids[0]) + ") {");
                depth++;
                stmt(*n.kids[1]);
                depth--;
                if (!n.kids[2]->kids.empty()) {
                    line("} else {");
                    depth++;
                    stmt(*n.kids[2]);
                    depth--;
                }
                line("}");
                return;
            case GNode::While:
                line("loop {");
                depth++;
                line("if (!(" + expr(*n.kids[0]) + ")) { break; }");
                stmt(*n.kids[1]);
                if (n.kids.size() > 2) {
                    line("continuing {");
                    depth++;
                    stmt(*n.kids[2]);
                    depth--;
                    line("}");
                }
                depth--;
                line("}");
                return;
            case GNode::Break: line("break;"); return;
            case GNode::Continue: line("continue;"); return;
            case GNode::Return: line("return;"); return;
            default: return;
        }
    }
    std::string build() {
        std::string h;
        h += "// " + k.name + ": generated by ixo from a gpu fn\n";
        h += "struct Params {\n";
        int scalars = 0;
        for (size_t i = 0; i < k.params.size(); i++)
            if (k.params[i].t != GTy::Buffer) {
                h += "    s" + std::to_string(scalars) + ": " + (k.params[i].t == GTy::Float ? "f32" : "i32") + ",\n";
                scalars++;
            }
        h += "    n: u32,\n    stride: u32,\n};\n";
        int binding = 0;
        for (size_t i = 0; i < k.params.size(); i++)
            if (k.params[i].t == GTy::Buffer) {
                const char* t = k.params[i].elem == GTy::Float ? "f32" : "i32";
                std::string b = std::to_string(i);
                h += "@group(0) @binding(" + std::to_string(binding++) + ") var<storage, read_write> b" + b + ": array<" + t + ">;\n";
            }
        h += "@group(0) @binding(" + std::to_string(binding) + ") var<storage, read_write> status: array<i32>;\n";
        h += "@group(0) @binding(" + std::to_string(binding + 1) + ") var<uniform> P: Params;\n\n";
        h += "fn bad(i: i32, b: i32) {\n    if (status[0] == 0) { status[0] = 1; status[1] = i; status[2] = b; }\n}\n";
        for (size_t i = 0; i < k.params.size(); i++)
            if (k.params[i].t == GTy::Buffer) {
                const char* t = k.params[i].elem == GTy::Float ? "f32" : "i32";
                std::string b = std::to_string(i), zero = k.params[i].elem == GTy::Float ? "0.0" : "0";
                h += "fn ld" + b + "(i: i32) -> " + t + " {\n    if (u32(i) < arrayLength(&b" + b + ")) { return b" + b +
                     "[i]; }\n    bad(i, " + b + ");\n    return " + zero + ";\n}\n";
                h += "fn st" + b + "(i: i32, v: " + t + ") {\n    if (u32(i) < arrayLength(&b" + b + ")) { b" + b +
                     "[i] = v; } else { bad(i, " + b + "); }\n}\n";
            }
        h += "fn idiv(a: i32, b: i32) -> i32 {\n"
             "    if (b == 0) { return 0; }\n"
             "    if (b == -1) { return -a; }\n"
             "    var q = a / b;\n"
             "    if ((a % b != 0) && ((a < 0) != (b < 0))) { q = q - 1; }\n"
             "    return q;\n}\n";
        h += "fn imod(a: i32, b: i32) -> i32 {\n"
             "    if (b == 0 || b == -1) { return 0; }\n"
             "    var m = a % b;\n"
             "    if (m != 0 && ((m < 0) != (b < 0))) { m = m + b; }\n"
             "    return m;\n}\n";
        h += "fn fmodf(a: f32, b: f32) -> f32 { return a - b * floor(a / b); }\n";
        h += "fn toint(x: f32) -> i32 {\n"
             "    if (x != x) { return 0; }\n"
             "    if (x >= 2147483648.0) { return 2147483647; }\n"
             "    return i32(clamp(x, -2147483648.0, 2147483520.0));\n}\n\n";
        h += "@compute @workgroup_size(64)\nfn main(@builtin(global_invocation_id) g: vec3<u32>) {\n";
        h += "    let ut = g.x + g.y * P.stride;\n    if (ut >= P.n) { return; }\n    let tid = i32(ut);\n";
        for (size_t s2 = 0; s2 < k.vars.size(); s2++) h += "    var v" + std::to_string(s2) + ": " + ty(k.vars[s2]) + ";\n";
        int m = 0, slot = 0;
        for (auto& p : k.params) {
            if (p.t == GTy::Buffer) continue;
            std::string src = "P.s" + std::to_string(m);
            if (p.t == GTy::Bool) src = "(" + src + " != 0)";
            h += "    v" + std::to_string(slot) + " = " + src + ";\n";
            m++;
            slot++;
        }
        stmt(*k.body);
        return h + out + "}\n";
    }
};

}  // namespace

std::string wgslFor(const GpuKernel& k) {
    Wgsl w(k);
    return w.build();
}

}  // namespace ixo
