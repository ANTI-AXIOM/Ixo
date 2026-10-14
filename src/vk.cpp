// The Vulkan runtime for gpu fn kernels (see gpu.hpp).
//
// The Vulkan loader is opened at run time (vulkan-1.dll, libvulkan.so.1); without
// it, or without a device, kernels run on the CPU. Only the small part of the API
// compute needs is declared here, from the Vulkan 1.1 specification (no SDK
// needed to build). Buffers live in host-visible, coherent memory, kept mapped:
// the CPU reads and writes them in place, with no copies (on integrated GPUs
// that is the GPU's memory anyway). A launch records one dispatch, submits it and
// waits for it, so launches run in order.
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "gpu.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ixo {

namespace {

// ------------------------------------------------------------------ the API subset
typedef uint32_t VkFlags;
typedef uint64_t VkDeviceSize;
typedef int32_t VkResult;
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef uint64_t VkBuffer, VkDeviceMemory, VkShaderModule, VkDescriptorSetLayout, VkPipelineLayout, VkPipeline,
    VkDescriptorPool, VkDescriptorSet, VkCommandPool, VkFence, VkPipelineCache;
#if defined(_WIN32)
#define VKAPI __stdcall
#else
#define VKAPI
#endif
typedef void(VKAPI* PFN_vkVoidFunction)(void);

struct VkApplicationInfo {
    int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};
struct VkQueueFamilyProperties {
    VkFlags queueFlags;
    uint32_t queueCount;
    uint32_t timestampValidBits;
    uint32_t minImageTransferGranularity[3];
};
struct VkMemoryType {
    VkFlags propertyFlags;
    uint32_t heapIndex;
};
struct VkMemoryHeap {
    VkDeviceSize size;
    VkFlags flags;
};
struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};
struct VkDeviceQueueCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
};
struct VkDeviceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
};
struct VkBufferCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkDeviceSize size;
    VkFlags usage;
    int32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t* pQueueFamilyIndices;
};
struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
};
struct VkMemoryAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
};
struct VkShaderModuleCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    size_t codeSize;
    const uint32_t* pCode;
};
struct VkDescriptorSetLayoutBinding {
    uint32_t binding;
    int32_t descriptorType;
    uint32_t descriptorCount;
    VkFlags stageFlags;
    const void* pImmutableSamplers;
};
struct VkDescriptorSetLayoutCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t bindingCount;
    const VkDescriptorSetLayoutBinding* pBindings;
};
struct VkPushConstantRange {
    VkFlags stageFlags;
    uint32_t offset;
    uint32_t size;
};
struct VkPipelineLayoutCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t setLayoutCount;
    const VkDescriptorSetLayout* pSetLayouts;
    uint32_t pushConstantRangeCount;
    const VkPushConstantRange* pPushConstantRanges;
};
struct VkPipelineShaderStageCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkFlags stage;
    VkShaderModule module;
    const char* pName;
    const void* pSpecializationInfo;
};
struct VkComputePipelineCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    VkPipelineShaderStageCreateInfo stage;
    VkPipelineLayout layout;
    VkPipeline basePipelineHandle;
    int32_t basePipelineIndex;
};
struct VkDescriptorPoolSize {
    int32_t type;
    uint32_t descriptorCount;
};
struct VkDescriptorPoolCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t maxSets;
    uint32_t poolSizeCount;
    const VkDescriptorPoolSize* pPoolSizes;
};
struct VkDescriptorSetAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkDescriptorPool descriptorPool;
    uint32_t descriptorSetCount;
    const VkDescriptorSetLayout* pSetLayouts;
};
struct VkDescriptorBufferInfo {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
};
struct VkWriteDescriptorSet {
    int32_t sType;
    const void* pNext;
    VkDescriptorSet dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    int32_t descriptorType;
    const void* pImageInfo;
    const VkDescriptorBufferInfo* pBufferInfo;
    const void* pTexelBufferView;
};
struct VkCommandPoolCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
};
struct VkCommandBufferAllocateInfo {
    int32_t sType;
    const void* pNext;
    VkCommandPool commandPool;
    int32_t level;
    uint32_t commandBufferCount;
};
struct VkCommandBufferBeginInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const void* pInheritanceInfo;
};
struct VkSubmitInfo {
    int32_t sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const void* pWaitSemaphores;
    const VkFlags* pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer* pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const void* pSignalSemaphores;
};
struct VkFenceCreateInfo {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
};
struct VkMemoryBarrier {
    int32_t sType;
    const void* pNext;
    VkFlags srcAccessMask;
    VkFlags dstAccessMask;
};

enum : int32_t {
    ST_APPLICATION_INFO = 0, ST_INSTANCE_CREATE_INFO = 1, ST_DEVICE_QUEUE_CREATE_INFO = 2, ST_DEVICE_CREATE_INFO = 3,
    ST_SUBMIT_INFO = 4, ST_MEMORY_ALLOCATE_INFO = 5, ST_FENCE_CREATE_INFO = 8, ST_BUFFER_CREATE_INFO = 12,
    ST_SHADER_MODULE_CREATE_INFO = 16, ST_PIPELINE_SHADER_STAGE_CREATE_INFO = 18, ST_COMPUTE_PIPELINE_CREATE_INFO = 29,
    ST_PIPELINE_LAYOUT_CREATE_INFO = 30, ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32, ST_DESCRIPTOR_POOL_CREATE_INFO = 33,
    ST_DESCRIPTOR_SET_ALLOCATE_INFO = 34, ST_WRITE_DESCRIPTOR_SET = 35, ST_COMMAND_POOL_CREATE_INFO = 39,
    ST_COMMAND_BUFFER_ALLOCATE_INFO = 40, ST_COMMAND_BUFFER_BEGIN_INFO = 42, ST_MEMORY_BARRIER = 46,
};
constexpr int32_t DESC_STORAGE_BUFFER = 7;
constexpr VkFlags STAGE_COMPUTE = 0x20, USAGE_STORAGE = 0x20, MEM_HOST_VISIBLE = 2, MEM_HOST_COHERENT = 4,
                  MEM_DEVICE_LOCAL = 1, QUEUE_COMPUTE = 2, PIPE_STAGE_COMPUTE = 0x800, PIPE_STAGE_HOST = 0x4000,
                  ACCESS_SHADER_WRITE = 0x40, ACCESS_HOST_READ = 0x2000, ACCESS_SHADER_READ = 0x20,
                  ACCESS_HOST_WRITE = 0x4000, CMD_ONE_TIME = 1, POOL_RESET_BUFFERS = 2;
constexpr int32_t BIND_COMPUTE = 1;
constexpr uint32_t API_1_1 = (1u << 22) | (1u << 12);

// ------------------------------------------------------------------ entry points
#define VK_FUNCS(X)                                                                                                    \
    X(vkCreateInstance, VkResult, const VkInstanceCreateInfo*, const void*, VkInstance*)                              \
    X(vkEnumeratePhysicalDevices, VkResult, VkInstance, uint32_t*, VkPhysicalDevice*)                                \
    X(vkGetPhysicalDeviceProperties, void, VkPhysicalDevice, void*)                                                  \
    X(vkGetPhysicalDeviceQueueFamilyProperties, void, VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*)       \
    X(vkGetPhysicalDeviceMemoryProperties, void, VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*)               \
    X(vkCreateDevice, VkResult, VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*)                 \
    X(vkGetDeviceProcAddr, PFN_vkVoidFunction, VkDevice, const char*)
#define VK_DEVICE_FUNCS(X)                                                                                             \
    X(vkGetDeviceQueue, void, VkDevice, uint32_t, uint32_t, VkQueue*)                                                  \
    X(vkCreateBuffer, VkResult, VkDevice, const VkBufferCreateInfo*, const void*, VkBuffer*)                           \
    X(vkDestroyBuffer, void, VkDevice, VkBuffer, const void*)                                                          \
    X(vkGetBufferMemoryRequirements, void, VkDevice, VkBuffer, VkMemoryRequirements*)                                 \
    X(vkAllocateMemory, VkResult, VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*)                 \
    X(vkFreeMemory, void, VkDevice, VkDeviceMemory, const void*)                                                       \
    X(vkBindBufferMemory, VkResult, VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)                                 \
    X(vkMapMemory, VkResult, VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)                    \
    X(vkCreateShaderModule, VkResult, VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*)          \
    X(vkCreateDescriptorSetLayout, VkResult, VkDevice, const VkDescriptorSetLayoutCreateInfo*, const void*,            \
      VkDescriptorSetLayout*)                                                                                          \
    X(vkCreatePipelineLayout, VkResult, VkDevice, const VkPipelineLayoutCreateInfo*, const void*, VkPipelineLayout*)    \
    X(vkCreateComputePipelines, VkResult, VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*,     \
      const void*, VkPipeline*)                                                                                        \
    X(vkCreateDescriptorPool, VkResult, VkDevice, const VkDescriptorPoolCreateInfo*, const void*, VkDescriptorPool*)    \
    X(vkAllocateDescriptorSets, VkResult, VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*)               \
    X(vkUpdateDescriptorSets, void, VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const void*)            \
    X(vkCreateCommandPool, VkResult, VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*)             \
    X(vkAllocateCommandBuffers, VkResult, VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*)               \
    X(vkResetCommandBuffer, VkResult, VkCommandBuffer, VkFlags)                                                        \
    X(vkBeginCommandBuffer, VkResult, VkCommandBuffer, const VkCommandBufferBeginInfo*)                                \
    X(vkEndCommandBuffer, VkResult, VkCommandBuffer)                                                                   \
    X(vkCmdBindPipeline, void, VkCommandBuffer, int32_t, VkPipeline)                                                    \
    X(vkCmdBindDescriptorSets, void, VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t, uint32_t,                   \
      const VkDescriptorSet*, uint32_t, const uint32_t*)                                                               \
    X(vkCmdPushConstants, void, VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*)           \
    X(vkCmdDispatch, void, VkCommandBuffer, uint32_t, uint32_t, uint32_t)                                              \
    X(vkCmdPipelineBarrier, void, VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const VkMemoryBarrier*,        \
      uint32_t, const void*, uint32_t, const void*)                                                                    \
    X(vkQueueSubmit, VkResult, VkQueue, uint32_t, const VkSubmitInfo*, VkFence)                                         \
    X(vkCreateFence, VkResult, VkDevice, const VkFenceCreateInfo*, const void*, VkFence*)                               \
    X(vkWaitForFences, VkResult, VkDevice, uint32_t, const VkFence*, uint32_t, uint64_t)                               \
    X(vkResetFences, VkResult, VkDevice, uint32_t, const VkFence*)

#define DECLARE(name, ret, ...) typedef ret(VKAPI* PFN_##name)(__VA_ARGS__);
VK_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
#undef DECLARE
typedef PFN_vkVoidFunction(VKAPI* PFN_vkGetInstanceProcAddr)(VkInstance, const char*);

struct Vk {
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
#define MEMBER(name, ...) PFN_##name name = nullptr;
    VK_FUNCS(MEMBER)
    VK_DEVICE_FUNCS(MEMBER)
#undef MEMBER
    VkInstance instance = nullptr;
    VkPhysicalDevice phys = nullptr;
    VkDevice device = nullptr;
    VkQueue queue = nullptr;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t hostType = UINT32_MAX;  // host-visible, coherent (device-local too if there is one)
    VkCommandPool pool = 0;
    VkCommandBuffer cmd = nullptr;
    VkFence fence = 0;
    VkBufferMem* status = nullptr;
    int32_t* statusWords = nullptr;
    uint32_t maxGroups = 65535;
    std::string name;
    std::vector<std::string> names;
    bool ready = false;
    std::string why;
};
Vk vk;
std::once_flag vkOnce;
std::mutex vkLock;

// the device to use: IXO_GPU_DEVICE=n, else a discrete GPU, an integrated one, a virtual one, a CPU one
int deviceRank(int32_t type) { return type == 2 ? 0 : type == 1 ? 1 : type == 3 ? 2 : type == 4 ? 3 : 4; }

bool start() {
    std::string& why = vk.why;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) return why = "no Vulkan loader (vulkan-1.dll)", false;
    vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#ifdef __APPLE__
    if (!lib) lib = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib) return why = "no Vulkan loader (libvulkan.so.1)", false;
    vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
    if (!vk.getInstanceProcAddr) return why = "the Vulkan loader has no vkGetInstanceProcAddr", false;
    vk.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vk.getInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vk.vkCreateInstance) return why = "no vkCreateInstance", false;
    VkApplicationInfo app{ST_APPLICATION_INFO, nullptr, "ixo", 1, "ixo", 1, API_1_1};
    VkInstanceCreateInfo ici{ST_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    if (vk.vkCreateInstance(&ici, nullptr, &vk.instance) != 0) return why = "Vulkan 1.1 isn't available", false;
#define LOAD_I(name, ...)                                                                                              \
    vk.name = reinterpret_cast<PFN_##name>(vk.getInstanceProcAddr(vk.instance, #name));                              \
    if (!vk.name) return why = "Vulkan lacks " #name, false;
    VK_FUNCS(LOAD_I)
#undef LOAD_I
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
    if (count == 0) return why = "no Vulkan device", false;
    std::vector<VkPhysicalDevice> devs(count);
    vk.vkEnumeratePhysicalDevices(vk.instance, &count, devs.data());
    int best = -1, bestRank = 99;
    const char* pick = std::getenv("IXO_GPU_DEVICE");
    for (uint32_t i = 0; i < count; i++) {
        alignas(8) unsigned char props[4096] = {};
        vk.vkGetPhysicalDeviceProperties(devs[i], props);
        uint32_t api;
        int32_t type;
        std::memcpy(&api, props, 4);
        std::memcpy(&type, props + 16, 4);
        std::string nm(reinterpret_cast<const char*>(props + 20));
        vk.names.push_back(nm);
        if (api < API_1_1) continue;
        // a queue that does compute
        uint32_t nq = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qs.data());
        bool compute = false;
        for (auto& q : qs) compute |= (q.queueFlags & QUEUE_COMPUTE) != 0;
        if (!compute) continue;
        int rank = deviceRank(type);
        if (pick && std::atoi(pick) == (int)i) rank = -1;
        if (rank < bestRank) {
            bestRank = rank;
            best = (int)i;
        }
    }
    if (best < 0) return why = "no Vulkan 1.1 device with compute", false;
    vk.phys = devs[(size_t)best];
    vk.name = vk.names[(size_t)best];
    {
        alignas(8) unsigned char props[4096] = {};
        vk.vkGetPhysicalDeviceProperties(vk.phys, props);
        // limits start after deviceName[256] and pipelineCacheUUID[16] (offset 296), at 8-byte alignment;
        // maxComputeWorkGroupCount[0] is the 4-byte field at index 51 of the limits
        // (after 39 uint32/VkDeviceSize-sized fields...): read conservatively instead
        vk.maxGroups = 65535;  // the minimum every device supports
    }
    uint32_t nq = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, qs.data());
    vk.family = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qs[i].queueFlags & QUEUE_COMPUTE) {
            vk.family = i;
            break;
        }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ST_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, vk.family, 1, &prio};
    VkDeviceCreateInfo dci{ST_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr};
    if (vk.vkCreateDevice(vk.phys, &dci, nullptr, &vk.device) != 0) return why = "can't open the Vulkan device", false;
#define LOAD_D(name, ...)                                                                                              \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(vk.device, #name));                                \
    if (!vk.name) return why = "Vulkan lacks " #name, false;
    VK_DEVICE_FUNCS(LOAD_D)
#undef LOAD_D
    vk.vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    vk.vkGetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem);
    // host-visible and coherent; device-local as well when there is such memory (integrated GPUs, resizable BAR)
    for (int pass = 0; pass < 2 && vk.hostType == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < vk.mem.memoryTypeCount; i++) {
            VkFlags f = vk.mem.memoryTypes[i].propertyFlags;
            VkFlags want = MEM_HOST_VISIBLE | MEM_HOST_COHERENT | (pass == 0 ? MEM_DEVICE_LOCAL : 0);
            if ((f & want) == want) {
                vk.hostType = i;
                break;
            }
        }
    if (vk.hostType == UINT32_MAX) return why = "the device has no memory the CPU can map", false;
    VkCommandPoolCreateInfo cpi{ST_COMMAND_POOL_CREATE_INFO, nullptr, POOL_RESET_BUFFERS, vk.family};
    if (vk.vkCreateCommandPool(vk.device, &cpi, nullptr, &vk.pool) != 0) return why = "can't make a command pool", false;
    VkCommandBufferAllocateInfo cai{ST_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, vk.pool, 0, 1};
    if (vk.vkAllocateCommandBuffers(vk.device, &cai, &vk.cmd) != 0) return why = "can't make a command buffer", false;
    VkFenceCreateInfo fci{ST_FENCE_CREATE_INFO, nullptr, 0};
    if (vk.vkCreateFence(vk.device, &fci, nullptr, &vk.fence) != 0) return why = "can't make a fence", false;
    vk.ready = true;  // (vkNewBuffer needs it)
    void* st = nullptr;
    vk.status = vkNewBuffer(16, &st);
    if (!vk.status) {
        vk.ready = false;
        return why = "can't make a buffer", false;
    }
    vk.statusWords = static_cast<int32_t*>(st);
    return true;
}

// a kernel's pipeline on the device
struct Pipe {
    VkShaderModule module = 0;
    VkDescriptorSetLayout setLayout = 0;
    VkPipelineLayout layout = 0;
    VkPipeline pipeline = 0;
    VkDescriptorPool pool = 0;
    VkDescriptorSet set = 0;
    uint32_t pushWords = 0;
    bool ok = false;
};

std::shared_ptr<Pipe> makePipe(GpuKernel& k, std::string* why) {
    auto p = std::make_shared<Pipe>();
    if (k.spirv.empty()) k.spirv = spirvFor(k);
    VkShaderModuleCreateInfo smi{ST_SHADER_MODULE_CREATE_INFO, nullptr, 0, k.spirv.size() * 4, k.spirv.data()};
    if (vk.vkCreateShaderModule(vk.device, &smi, nullptr, &p->module) != 0) {
        if (why) *why = "the device rejected the kernel's SPIR-V";
        return nullptr;
    }
    uint32_t bindings = (uint32_t)k.buffers + 1;
    std::vector<VkDescriptorSetLayoutBinding> b(bindings);
    for (uint32_t i = 0; i < bindings; i++) b[i] = {i, DESC_STORAGE_BUFFER, 1, STAGE_COMPUTE, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, bindings, b.data()};
    if (vk.vkCreateDescriptorSetLayout(vk.device, &dli, nullptr, &p->setLayout) != 0) {
        if (why) *why = "can't make a descriptor set layout";
        return nullptr;
    }
    uint32_t scalars = 0;
    for (auto& prm : k.params) scalars += prm.t != GTy::Buffer;
    p->pushWords = scalars + 2;
    if (p->pushWords * 4 > 128) {
        if (why) *why = "more than 30 scalar parameters";
        return nullptr;
    }
    VkPushConstantRange pr{STAGE_COMPUTE, 0, p->pushWords * 4};
    VkPipelineLayoutCreateInfo pli{ST_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &p->setLayout, 1, &pr};
    if (vk.vkCreatePipelineLayout(vk.device, &pli, nullptr, &p->layout) != 0) {
        if (why) *why = "can't make a pipeline layout";
        return nullptr;
    }
    VkComputePipelineCreateInfo cpi{ST_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
                                    {ST_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, STAGE_COMPUTE, p->module, "main", nullptr},
                                    p->layout, 0, -1};
    if (vk.vkCreateComputePipelines(vk.device, 0, 1, &cpi, nullptr, &p->pipeline) != 0) {
        if (why) *why = "the device couldn't build the kernel";
        return nullptr;
    }
    VkDescriptorPoolSize ps{DESC_STORAGE_BUFFER, bindings};
    VkDescriptorPoolCreateInfo dpi{ST_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &ps};
    if (vk.vkCreateDescriptorPool(vk.device, &dpi, nullptr, &p->pool) != 0) {
        if (why) *why = "can't make a descriptor pool";
        return nullptr;
    }
    VkDescriptorSetAllocateInfo dai{ST_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, p->pool, 1, &p->setLayout};
    if (vk.vkAllocateDescriptorSets(vk.device, &dai, &p->set) != 0) {
        if (why) *why = "can't make a descriptor set";
        return nullptr;
    }
    p->ok = true;
    return p;
}

}  // namespace

struct VkBufferMem {
    VkBuffer buffer = 0;
    VkDeviceMemory memory = 0;
    VkDeviceSize size = 0;
};

bool vkStart(std::string* why) {
    std::call_once(vkOnce, [] {
        if (!start()) vk.ready = false;
    });
    if (!vk.ready && why) *why = vk.why;
    return vk.ready;
}
std::string vkDeviceName() { return vk.ready ? vk.name : ""; }
std::vector<std::string> vkDeviceNames() { return vk.names; }

VkBufferMem* vkNewBuffer(size_t bytes, void** mapped) {
    if (!vk.ready) return nullptr;
    std::lock_guard<std::mutex> g(vkLock);
    auto* b = new VkBufferMem;
    b->size = bytes;
    VkBufferCreateInfo bci{ST_BUFFER_CREATE_INFO, nullptr, 0, bytes, USAGE_STORAGE, 0, 0, nullptr};
    if (vk.vkCreateBuffer(vk.device, &bci, nullptr, &b->buffer) != 0) {
        delete b;
        return nullptr;
    }
    VkMemoryRequirements req{};
    vk.vkGetBufferMemoryRequirements(vk.device, b->buffer, &req);
    if (!(req.memoryTypeBits & (1u << vk.hostType))) {
        vk.vkDestroyBuffer(vk.device, b->buffer, nullptr);
        delete b;
        return nullptr;
    }
    VkMemoryAllocateInfo mai{ST_MEMORY_ALLOCATE_INFO, nullptr, req.size, vk.hostType};
    if (vk.vkAllocateMemory(vk.device, &mai, nullptr, &b->memory) != 0) {
        vk.vkDestroyBuffer(vk.device, b->buffer, nullptr);
        delete b;
        return nullptr;
    }
    vk.vkBindBufferMemory(vk.device, b->buffer, b->memory, 0);
    if (vk.vkMapMemory(vk.device, b->memory, 0, bytes, 0, mapped) != 0) {
        vk.vkDestroyBuffer(vk.device, b->buffer, nullptr);
        vk.vkFreeMemory(vk.device, b->memory, nullptr);
        delete b;
        return nullptr;
    }
    return b;
}

void vkFreeBuffer(VkBufferMem* b) {
    if (!b || !vk.ready) return;
    std::lock_guard<std::mutex> g(vkLock);
    vk.vkDestroyBuffer(vk.device, b->buffer, nullptr);
    vk.vkFreeMemory(vk.device, b->memory, nullptr);  // (unmaps it)
    delete b;
}

bool vkLaunch(GpuKernel& k, uint32_t n, const std::vector<VkBufferMem*>& bufs, const std::vector<uint32_t>& scalars,
              int64_t& badIndex, std::string* why) {
    if (!vk.ready) {
        if (why) *why = vk.why;
        return false;
    }
    std::lock_guard<std::mutex> g(vkLock);
    auto pipe = std::static_pointer_cast<Pipe>(k.device);
    if (!pipe) {
        pipe = makePipe(k, why);
        if (!pipe) {
            k.device = std::make_shared<Pipe>();  // (failed: not again)
            return false;
        }
        k.device = pipe;
    }
    if (!pipe->ok) {
        if (why && why->empty()) *why = "the kernel couldn't be built for this device";
        return false;
    }
    // the buffers, then the status
    std::vector<VkDescriptorBufferInfo> infos;
    for (auto* b : bufs) infos.push_back({b->buffer, 0, b->size});
    infos.push_back({vk.status->buffer, 0, vk.status->size});
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < infos.size(); i++)
        writes.push_back({ST_WRITE_DESCRIPTOR_SET, nullptr, pipe->set, i, 0, 1, DESC_STORAGE_BUFFER, nullptr, &infos[i], nullptr});
    vk.vkUpdateDescriptorSets(vk.device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    std::memset(vk.statusWords, 0, 16);
    // groups of 64; past 65535 groups, a second dimension (thread = x + y * stride)
    uint32_t groups = (n + 63) / 64;
    uint32_t gx = groups, gy = 1;
    if (gx > vk.maxGroups) {
        gx = vk.maxGroups;
        gy = (groups + gx - 1) / gx;
    }
    std::vector<uint32_t> push = scalars;
    push.push_back(n);
    push.push_back(gx * 64);
    vk.vkResetCommandBuffer(vk.cmd, 0);
    VkCommandBufferBeginInfo bi{ST_COMMAND_BUFFER_BEGIN_INFO, nullptr, CMD_ONE_TIME, nullptr};
    vk.vkBeginCommandBuffer(vk.cmd, &bi);
    VkMemoryBarrier before{ST_MEMORY_BARRIER, nullptr, ACCESS_HOST_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE};
    vk.vkCmdPipelineBarrier(vk.cmd, PIPE_STAGE_HOST, PIPE_STAGE_COMPUTE, 0, 1, &before, 0, nullptr, 0, nullptr);
    vk.vkCmdBindPipeline(vk.cmd, BIND_COMPUTE, pipe->pipeline);
    vk.vkCmdBindDescriptorSets(vk.cmd, BIND_COMPUTE, pipe->layout, 0, 1, &pipe->set, 0, nullptr);
    vk.vkCmdPushConstants(vk.cmd, pipe->layout, STAGE_COMPUTE, 0, (uint32_t)push.size() * 4, push.data());
    vk.vkCmdDispatch(vk.cmd, gx, gy, 1);
    VkMemoryBarrier after{ST_MEMORY_BARRIER, nullptr, ACCESS_SHADER_WRITE, ACCESS_HOST_READ};
    vk.vkCmdPipelineBarrier(vk.cmd, PIPE_STAGE_COMPUTE, PIPE_STAGE_HOST, 0, 1, &after, 0, nullptr, 0, nullptr);
    vk.vkEndCommandBuffer(vk.cmd);
    VkSubmitInfo si{ST_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &vk.cmd, 0, nullptr};
    vk.vkResetFences(vk.device, 1, &vk.fence);
    if (vk.vkQueueSubmit(vk.queue, 1, &si, vk.fence) != 0) {
        if (why) *why = "the device didn't take the work";
        return false;
    }
    if (vk.vkWaitForFences(vk.device, 1, &vk.fence, 1, UINT64_MAX) != 0) {
        if (why) *why = "the device didn't finish (lost?)";
        return false;
    }
    badIndex = -1;
    if (vk.statusWords[0]) badIndex = (int64_t)(uint32_t)vk.statusWords[1] | ((int64_t)vk.statusWords[2] << 32);
    return true;
}

}  // namespace ixo
