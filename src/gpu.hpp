// GPU compute (ROADMAP Phase 14): kernels written in Ixo.
//
//     gpu fn saxpy(a: float, x: buffer[float], y: buffer[float])
//         i = gpu.index
//         if i < y.len -> y[i] = a * x[i] + y[i]
//
//     x = gpu.floats(xs)
//     y = gpu.floats(ys)
//     saxpy(2.0, x, y).await      # one thread per item of the first buffer; a task
//     print(y.list)
//
// A `gpu fn` is checked against the GPU subset when it is compiled (gpuCompile):
// ints, floats and bools, buffers of ints or floats, locals, arithmetic, math,
// if/while/for over ranges. On the device, ints are 32-bit and floats are 32-bit
// (as in WGSL and on most GPUs). The kernel becomes a small typed tree (GNode),
// which three backends take: a CPU fallback that runs the threads on all cores
// (so every GPU program runs anywhere), SPIR-V for Vulkan (spirv.cpp, vk.cpp), and
// WGSL text for WebGPU (wgsl.cpp).
//
// A call of a `gpu fn` runs a launcher (compiled in its place) that calls the
// built-in __gpu_launch(kernel id, args...).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ixo {

struct FnDecl;
class VM;
struct ObjGpuBuffer;

enum class GTy : uint8_t { Int, Float, Bool, Void, Buffer };

// Math the kernels may call (sqrt(x), x.sqrt, min(a, b), ...).
enum GMath : uint8_t {
    GM_Sqrt, GM_Abs, GM_Min, GM_Max, GM_Floor, GM_Ceil, GM_Sin, GM_Cos, GM_Tan, GM_Exp, GM_Log, GM_Pow, GM_ToFloat,
    GM_ToInt
};

struct GNode {
    enum K : uint8_t {
        // expressions
        IntK, FloatK, BoolK, Var, Load /*buf[i]*/, Len /*buf.len*/, ThreadIdx, Neg, Not, Bin, Math,
        // statements
        Set /*var = e*/, Store /*buf[i] = e*/, If, While, Break, Continue, Return, Block, Nop
    } k;
    GTy t = GTy::Void;
    int op = 0;    // Bin: GBin; Math: GMath
    int slot = -1;  // Var/Set: the variable; Load/Len/Store: the buffer parameter
    int64_t ival = 0;
    double fval = 0;
    int line = 0, col = 0;
    std::vector<std::unique_ptr<GNode>> kids;  // operands; If: cond, then, else; While: cond, body
};
enum GBin : uint8_t {
    GB_Add, GB_Sub, GB_Mul, GB_Div /*float result*/, GB_IntDiv, GB_Mod, GB_Pow, GB_Lt, GB_Le, GB_Gt, GB_Ge, GB_Eq, GB_Ne,
    GB_And, GB_Or, GB_BitAnd, GB_BitOr, GB_BitXor, GB_Shl, GB_Shr
};

struct GParam {
    std::string name;
    GTy t;     // Int, Float, Bool or Buffer
    GTy elem;  // a buffer's items: Int or Float
};

struct GpuKernel {
    std::string name;
    std::vector<GParam> params;
    std::vector<GTy> vars;  // every variable: the scalar parameters first (copies), then locals
    std::unique_ptr<GNode> body;
    int buffers = 0;  // how many buffer parameters
    // filled by the backends
    std::vector<uint32_t> spirv;
    std::string wgsl;
    std::shared_ptr<void> device;  // the Vulkan pipeline (vk.cpp)
};

// A kernel from a `gpu fn`; its id, or -1 with an error (and where).
int gpuCompile(FnDecl* fnd, std::string& err, int& line, int& col);
GpuKernel* gpuKernel(int id);

// The `gpu` namespace and the launcher built-in.
void registerGpu(VM& vm);

// SPIR-V for Vulkan compute (spirv.cpp). Bindings 0..buffers-1: the buffer
// parameters in order; binding `buffers`: a status word (first bad index + 1);
// push constants: the scalar parameters (4 bytes each, bools as ints), then the
// thread count. Work groups of 64.
std::vector<uint32_t> spirvFor(const GpuKernel& k);
// WGSL for WebGPU (wgsl.cpp), with the same bindings (the scalars in a uniform).
std::string wgslFor(const GpuKernel& k);

// The Vulkan runtime (vk.cpp): loaded on first use; absent drivers just mean "no device".
struct VkBufferMem;
bool vkStart(std::string* why);  // true if a device is ready
std::string vkDeviceName();
std::vector<std::string> vkDeviceNames();
VkBufferMem* vkNewBuffer(size_t bytes, void** mapped);
void vkFreeBuffer(VkBufferMem* b);
// Runs kernel k over n threads with the buffers and scalar words; false + why on failure.
bool vkLaunch(GpuKernel& k, uint32_t n, const std::vector<VkBufferMem*>& bufs, const std::vector<uint32_t>& scalars,
              int64_t& badIndex, std::string* why);

}  // namespace ixo
