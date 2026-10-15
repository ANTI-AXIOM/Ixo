// gpu.ptx: kernels written for NVIDIA GPUs (PTX, as nvcc or nvrtc make it), run
// through the CUDA driver, which is loaded when first used (nvcuda.dll on
// Windows, libcuda.so.1 on Linux). Without the driver, gpu.ptx gives an error.
//
//     k = gpu.ptx(text, "saxpy")?          # a PtxKernel
//     k.launch(n, [2.0, xs, ys, n])         # n threads in blocks of 256; buffers copied in and back
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "builtins.hpp"
#include "gpu.hpp"

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

namespace {

using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
using CUfunction = void*;
using CUdeviceptr = unsigned long long;

struct Cuda {
    bool tried = false, ok = false;
    std::string why;
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*cuCtxCreate)(CUcontext*, unsigned, CUdevice) = nullptr;
    CUresult (*cuModuleLoadData)(CUmodule*, const void*) = nullptr;
    CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (*cuMemAlloc)(CUdeviceptr*, size_t) = nullptr;
    CUresult (*cuMemFree)(CUdeviceptr) = nullptr;
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void*, size_t) = nullptr;
    CUresult (*cuMemcpyDtoH)(void*, CUdeviceptr, size_t) = nullptr;
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void*, void**,
                               void**) = nullptr;
    CUresult (*cuCtxSynchronize)() = nullptr;
    CUresult (*cuGetErrorString)(CUresult, const char**) = nullptr;
    CUcontext ctx = nullptr;
};
Cuda cu;
std::mutex cuLock;

struct PtxKernel {
    CUmodule mod;
    CUfunction fn;
    std::string name;
};
std::vector<PtxKernel> ptxKernels;

std::string cuError(CUresult r) {
    const char* s = nullptr;
    if (cu.cuGetErrorString) cu.cuGetErrorString(r, &s);
    return s ? std::string(s) : "CUDA error " + std::to_string(r);
}

template <class F>
bool load(void* lib, F& f, const char* name) {
#ifdef _WIN32
    f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress((HMODULE)lib, name)));
#else
    f = reinterpret_cast<F>(dlsym(lib, name));
#endif
    return f != nullptr;
}

bool cudaStart() {
    if (cu.tried) return cu.ok;
    cu.tried = true;
#ifdef _WIN32
    void* lib = (void*)LoadLibraryA("nvcuda.dll");
#else
    void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib) {
        cu.why = "no NVIDIA driver here (gpu.ptx needs CUDA)";
        return false;
    }
    bool all = load(lib, cu.cuInit, "cuInit") && load(lib, cu.cuDeviceGet, "cuDeviceGet") &&
               load(lib, cu.cuCtxCreate, "cuCtxCreate_v2") && load(lib, cu.cuModuleLoadData, "cuModuleLoadData") &&
               load(lib, cu.cuModuleGetFunction, "cuModuleGetFunction") && load(lib, cu.cuMemAlloc, "cuMemAlloc_v2") &&
               load(lib, cu.cuMemFree, "cuMemFree_v2") && load(lib, cu.cuMemcpyHtoD, "cuMemcpyHtoD_v2") &&
               load(lib, cu.cuMemcpyDtoH, "cuMemcpyDtoH_v2") && load(lib, cu.cuLaunchKernel, "cuLaunchKernel") &&
               load(lib, cu.cuCtxSynchronize, "cuCtxSynchronize");
    load(lib, cu.cuGetErrorString, "cuGetErrorString");
    if (!all) {
        cu.why = "the CUDA driver is too old (some functions are missing)";
        return false;
    }
    CUdevice dev = 0;
    CUresult r = cu.cuInit(0);
    if (r == 0) r = cu.cuDeviceGet(&dev, 0);
    if (r == 0) r = cu.cuCtxCreate(&cu.ctx, 0, dev);
    if (r != 0) {
        cu.why = "CUDA didn't start: " + cuError(r);
        return false;
    }
    cu.ok = true;
    return true;
}

// gpu.ptx(text, name): the kernel `name` of a PTX module
NATIVE(gpu_ptx) {
    const std::string& text = vm.argStr(a, 0, "gpu.ptx")->s;
    const std::string& name = vm.argStr(a, 1, "gpu.ptx")->s;
    int handle;
    {
        std::lock_guard<std::mutex> g(cuLock);
        if (!cudaStart()) return vm.newError("NoCuda", cu.why);
        CUmodule mod = nullptr;
        CUresult r = cu.cuModuleLoadData(&mod, text.c_str());
        if (r != 0) return vm.newError("Ptx", "the PTX didn't load: " + cuError(r));
        CUfunction fn = nullptr;
        r = cu.cuModuleGetFunction(&fn, mod, name.c_str());
        if (r != 0) return vm.newError("Ptx", "no kernel '" + name + "' in the PTX: " + cuError(r));
        ptxKernels.push_back({mod, fn, name});
        handle = (int)ptxKernels.size() - 1;
    }
    int ci = vm.builtinIndex("PtxKernel");
    if (ci < 0 || !isObjType(vm.builtins[ci].value, OT::Class)) vm.panic("internal: the PtxKernel class is missing");
    auto* cls = as<ObjClass>(vm.builtins[ci].value);
    Value inst = Value::object(vm.newInstance(cls));
    auto* o = as<ObjInstance>(inst);
    auto set = [&](const char* field, Value v) {
        auto it = cls->fieldIndex.find(vm.sym(field));
        if (it != cls->fieldIndex.end()) o->fields[(size_t)it->second] = v;
    };
    set("handle", Value::integer(handle));
    set("name", vm.vstr(name));
    return inst;
}

// gpu.ptx_launch(handle, n, args): n threads; buffers are copied to the device and back,
// ints and bools passed as 32-bit ints, floats as 32-bit floats
NATIVE(gpu_ptx_launch) {
    int64_t h = vm.argInt(a, 0, "gpu.ptx_launch"), threads = vm.argInt(a, 1, "gpu.ptx_launch");
    if (!isObjType(a[2], OT::List)) vm.panic("PtxKernel.launch(n, args): args is a list");
    std::vector<Value> args;
    for (size_t i = 0; i < as<ObjList>(a[2])->len(); i++) args.push_back(as<ObjList>(a[2])->get(i));
    std::lock_guard<std::mutex> g(cuLock);
    if (!cu.ok || h < 0 || h >= (int64_t)ptxKernels.size()) vm.panic("PtxKernel.launch: not a loaded kernel");
    if (threads < 0 || threads > INT32_MAX) vm.panic("PtxKernel.launch: the thread count is from 0 to 2147483647");
    std::vector<CUdeviceptr> dev(args.size(), 0);
    std::vector<uint32_t> words(args.size(), 0);
    std::vector<void*> params(args.size(), nullptr);
    std::string failure;
    auto cleanup = [&]() {
        for (auto p : dev)
            if (p) cu.cuMemFree(p);
    };
    for (size_t i = 0; i < args.size() && failure.empty(); i++) {
        Value v = args[i];
        if (isObjType(v, OT::GpuBuffer)) {
            auto* b = static_cast<ObjGpuBuffer*>(v.o);
            if (!b->data) vm.panic("PtxKernel.launch: argument " + std::to_string(i + 1) + " was freed");
            size_t bytes = (size_t)std::max<uint32_t>(b->n, 1) * 4;
            CUresult r = cu.cuMemAlloc(&dev[i], bytes);
            if (r == 0) r = cu.cuMemcpyHtoD(dev[i], b->data, bytes);
            if (r != 0) failure = cuError(r);
            params[i] = &dev[i];
        } else if (v.isInt() || v.isBool()) {
            int32_t x = v.isBool() ? (v.b ? 1 : 0) : (int32_t)v.i;
            std::memcpy(&words[i], &x, 4);
            params[i] = &words[i];
        } else if (v.isNum()) {
            float f = (float)v.num();
            std::memcpy(&words[i], &f, 4);
            params[i] = &words[i];
        } else {
            cleanup();
            vm.panic("PtxKernel.launch: argument " + std::to_string(i + 1) + " is a " + vm.typeName(v) +
                     " (gpu buffers, ints, floats and bools go to a kernel)");
        }
    }
    if (failure.empty() && threads > 0) {
        unsigned blocks = (unsigned)((threads + 255) / 256);
        CUresult r = cu.cuLaunchKernel(ptxKernels[(size_t)h].fn, blocks, 1, 1, 256, 1, 1, 0, nullptr, params.data(), nullptr);
        if (r == 0) r = cu.cuCtxSynchronize();
        if (r != 0) failure = cuError(r);
    }
    for (size_t i = 0; i < args.size() && failure.empty(); i++)
        if (dev[i]) {
            auto* b = static_cast<ObjGpuBuffer*>(args[i].o);
            CUresult r = cu.cuMemcpyDtoH(b->data, dev[i], (size_t)std::max<uint32_t>(b->n, 1) * 4);
            if (r != 0) failure = cuError(r);
        }
    cleanup();
    if (!failure.empty()) return vm.newError("Cuda", "the kernel " + ptxKernels[(size_t)h].name + " failed: " + failure);
    return Value::object(vm.finishedTask());
}

}  // namespace

void registerCuda(VM& vm, ObjNamespace* g) {
    vm.nsFunc(g, "ptx", gpu_ptx, 2, 2, "fn(str, str) -> PtxKernel ! Error",
              "A kernel from PTX for NVIDIA GPUs (through the CUDA driver): gpu.ptx(text, name).");
    vm.nsFunc(g, "ptx_launch", gpu_ptx_launch, 3, 3, "fn(int, int, list) -> task ! Error", "Used by PtxKernel.launch.");
}

}  // namespace ixo
