// Portable batched ambiguity processing. Uses Vulkan compute, not graphics or
// vendor-specific CUDA/ROCm runtimes. VkFFT supplies the FFT kernels; the two
// small shaders below implement the same correlation/gather as CPU Ambiguity.
#include "GpuBackend.h"
#include "GpuDriverStatus.h"
#include "GpuMemory.h"
#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#if __has_include(<glslang/Public/resource_limits_c.h>)
#include <glslang/Public/resource_limits_c.h>
#else
// glslang 11 (Ubuntu 22.04) exports this C API but does not install its header.
extern "C" const glslang_resource_t* glslang_default_resource(void);
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// VkFFT is header-only. Route its own device allocations through the same
// budget as our buffers, including transient upload and prime-length scratch.
// The definitions below call the real Vulkan functions after these macros end.
static VkResult budgetAllocateMemory(VkDevice, const VkMemoryAllocateInfo*,
  const VkAllocationCallbacks*, VkDeviceMemory*);
static void budgetFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
#define vkAllocateMemory budgetAllocateMemory
#define vkFreeMemory budgetFreeMemory
#include <vkFFT.h>
#undef vkFreeMemory
#undef vkAllocateMemory

namespace {
struct DeviceAllocations {
  struct Entry { uint32_t type; uint64_t bytes; };
  blah2::gpu_memory::AllocationBudget budget;
  std::unordered_map<VkDeviceMemory, Entry> allocations;
  DeviceAllocations(uint64_t limit, blah2::gpu_memory::Properties properties)
    : budget(limit, std::move(properties)) {}
};
std::mutex allocationsMutex;
std::unordered_map<VkDevice, DeviceAllocations> deviceAllocations;
}

static VkResult budgetAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto found = deviceAllocations.find(device);
  if (found == deviceAllocations.end()) return VK_ERROR_INITIALIZATION_FAILED;
  auto& state = found->second;
  *memory = VK_NULL_HANDLE;
  if (!state.budget.reserve(info->memoryTypeIndex, info->allocationSize)) {
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
      std::cerr << "GPU allocation budget rejected bytes=" << info->allocationSize
        << " type=" << info->memoryTypeIndex << " used=" << state.budget.used()
        << " limit=" << state.budget.limit() << '\n';
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  const VkResult result = vkAllocateMemory(device, info, callbacks, memory);
  if (result == VK_SUCCESS) {
    try {
      state.allocations.emplace(*memory,
        DeviceAllocations::Entry{info->memoryTypeIndex, info->allocationSize});
      return result;
    } catch (...) {
      vkFreeMemory(device, *memory, callbacks); *memory = VK_NULL_HANDLE;
      state.budget.release(info->memoryTypeIndex, info->allocationSize);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  state.budget.release(info->memoryTypeIndex, info->allocationSize);
  return result;
}
static void budgetFreeMemory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto found = deviceAllocations.find(device);
  if (found != deviceAllocations.end()) {
    auto& state = found->second;
    const auto allocation = state.allocations.find(memory);
    if (allocation != state.allocations.end()) {
      state.budget.release(allocation->second.type, allocation->second.bytes);
      state.allocations.erase(allocation);
    }
  }
  vkFreeMemory(device, memory, callbacks);
}

namespace blah2 {
namespace {
gpu_memory::Properties memoryProperties(VkPhysicalDevice physical);
class ClutterRejected final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
static_assert(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT == gpu_memory::deviceLocal);
static_assert(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT == gpu_memory::hostVisible);
static_assert(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT == gpu_memory::hostCoherent);
static_assert(VK_MEMORY_PROPERTY_HOST_CACHED_BIT == gpu_memory::hostCached);
void startupTrace(const char* stage) {
  if (!std::getenv("BLAH2_GPU_DIAGNOSTICS")) return;
  std::cerr << "GPU startup " << std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count() << " ms: " << stage << std::endl;
}
void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string("GPU ") + operation + " failed (" +
      std::to_string(result) + "); using CPU");
}
void checkFft(VkFFTResult result) {
  if (result != VKFFT_SUCCESS)
    throw std::runtime_error("GPU FFT failed (" + std::to_string(result) + "); using CPU");
}
struct Instance {
  VkInstance handle = VK_NULL_HANDLE;
  bool diagnosticProperties = false;
  explicit Instance(bool diagnostic = false) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "blah2"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    const char* extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    if (diagnostic) {
      uint32_t count = 0;
      check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "extension discovery");
      std::vector<VkExtensionProperties> extensions(count);
      check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()), "extension discovery");
      diagnosticProperties = std::any_of(extensions.begin(), extensions.end(), [&](const auto& value) {
        return std::strcmp(value.extensionName, extension) == 0;
      });
      if (diagnosticProperties) { info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = &extension; }
    }
    startupTrace("vkCreateInstance begin");
    check(vkCreateInstance(&info, nullptr, &handle), "driver initialization");
    startupTrace("vkCreateInstance complete");
  }
  ~Instance() { if (handle) vkDestroyInstance(handle, nullptr); }
};
struct Candidate {
  VkPhysicalDevice physical;
  VkPhysicalDeviceProperties properties;
  GpuDevice info;
  uint32_t queue;
};
std::vector<Candidate> enumerate(Instance& instance) {
  startupTrace("device enumeration begin");
  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "device discovery");
  std::vector<VkPhysicalDevice> devices(count);
  check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "device discovery");
  std::vector<Candidate> result;
  for (size_t index = 0; index < count; ++index) {
    Candidate item{}; item.physical = devices[index];
    vkGetPhysicalDeviceProperties(item.physical, &item.properties);
    // llvmpipe/lavapipe and other software drivers must never be called GPU acceleration.
    const auto type = item.properties.deviceType;
    if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
        type != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        type != VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) continue;
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(item.physical, &memory);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
      if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        bytes = std::max(bytes, uint64_t(memory.memoryHeaps[i].size));
    item.info = {std::to_string(item.properties.vendorID) + ":" +
      std::to_string(item.properties.deviceID) + ":" + std::to_string(index),
      item.properties.deviceName, bytes};
    uint32_t queues = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(item.physical, &queues, nullptr);
    std::vector<VkQueueFamilyProperties> families(queues);
    vkGetPhysicalDeviceQueueFamilyProperties(item.physical, &queues, families.data());
    item.queue = UINT32_MAX;
    for (uint32_t q = 0; q < queues; ++q)
      if (families[q].queueCount && (families[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        item.queue = q;
        if (!(families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;
      }
    if (item.queue != UINT32_MAX) result.push_back(item);
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    const auto rank = [](VkPhysicalDeviceType type) {
      return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 :
        type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 2;
    };
    if (a.properties.deviceType != b.properties.deviceType)
      return rank(a.properties.deviceType) < rank(b.properties.deviceType);
    return a.info.memoryBytes > b.info.memoryBytes;
  });
  startupTrace("device enumeration complete");
  return result;
}
struct Context {
  std::shared_ptr<Instance> instance;
  Candidate candidate;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool compiler = false;
  Context(std::shared_ptr<Instance> owner, Candidate selected)
    : instance(std::move(owner)), candidate(std::move(selected)) {
    try {
      const float priority = 1;
      VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = candidate.queue;
      queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queueInfo;
      startupTrace(candidate.info.name.c_str());
      startupTrace("vkCreateDevice begin");
      check(vkCreateDevice(candidate.physical, &info, nullptr, &device), "device initialization");
      {
        std::lock_guard<std::mutex> lock(allocationsMutex);
        deviceAllocations.try_emplace(device,
          gpu_memory::heapBudget(candidate.info.memoryBytes),
          memoryProperties(candidate.physical));
      }
      startupTrace("vkCreateDevice complete");
      vkGetDeviceQueue(device, candidate.queue, 0, &queue);
      VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      poolInfo.queueFamilyIndex = candidate.queue;
      check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command allocation");
      VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence allocation");
      startupTrace("glslang initialization begin");
      compiler = glslang_initialize_process();
      startupTrace("glslang initialization complete");
      if (!compiler) throw std::runtime_error("GPU shader compiler is unavailable");
    } catch (...) { release(); throw; }
  }
  void release() {
    if (compiler) glslang_finalize_process();
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (device) {
      vkDestroyDevice(device, nullptr);
      std::lock_guard<std::mutex> lock(allocationsMutex);
      const auto found = deviceAllocations.find(device);
      if (found != deviceAllocations.end()) {
        if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
          std::cerr << "GPU allocation budget peak=" << found->second.budget.peak()
            << " limit=" << found->second.budget.limit()
            << " remaining=" << found->second.budget.used() << '\n';
        deviceAllocations.erase(found);
      }
    }
  }
  ~Context() { release(); }
};
gpu_memory::Properties memoryProperties(VkPhysicalDevice physical) {
  VkPhysicalDeviceMemoryProperties source{};
  vkGetPhysicalDeviceMemoryProperties(physical, &source);
  gpu_memory::Properties result;
  for (uint32_t i = 0; i < source.memoryHeapCount; ++i)
    result.heaps.push_back({source.memoryHeaps[i].size,
      bool(source.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)});
  for (uint32_t i = 0; i < source.memoryTypeCount; ++i)
    result.types.push_back({source.memoryTypes[i].propertyFlags,
      source.memoryTypes[i].heapIndex});
  return result;
}
struct Buffer {
  Context& context;
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  uint64_t bytes, allocationBytes = 0;
  VkMemoryPropertyFlags properties = 0;
  uint32_t heap = UINT32_MAX;
  Buffer(Context& ctx, uint64_t size, VkMemoryPropertyFlags required,
      VkMemoryPropertyFlags preferred = 0) : context(ctx), bytes(size) {
    try {
      VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      info.size = size;
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      check(vkCreateBuffer(ctx.device, &info, nullptr, &handle), "buffer allocation");
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(ctx.device, handle, &requirements);
      VkPhysicalDeviceMemoryProperties properties{};
      vkGetPhysicalDeviceMemoryProperties(ctx.candidate.physical, &properties);
      gpu_memory::Properties view;
      for (uint32_t i = 0; i < properties.memoryHeapCount; ++i)
        view.heaps.push_back({properties.memoryHeaps[i].size,
          bool(properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)});
      for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        view.types.push_back({properties.memoryTypes[i].propertyFlags,
          properties.memoryTypes[i].heapIndex});
      const uint32_t type = gpu_memory::chooseType(requirements.memoryTypeBits,
        required, preferred, view);
      if (type == gpu_memory::noType)
        throw std::runtime_error("GPU has no suitable memory; using CPU");
      this->properties = properties.memoryTypes[type].propertyFlags;
      heap = properties.memoryTypes[type].heapIndex;
      allocationBytes = requirements.size;
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
      check(budgetAllocateMemory(ctx.device, &allocation, nullptr, &memory), "memory allocation");
      check(vkBindBufferMemory(ctx.device, handle, memory, 0), "memory binding");
      if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        check(vkMapMemory(ctx.device, memory, 0, allocationBytes, 0, &mapped), "host mapping");
    } catch (...) { release(); throw; }
  }
  void synchronize(uint64_t offset, uint64_t size, bool flush) {
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    const auto range = gpu_memory::alignedRange(offset, size, allocationBytes,
      context.candidate.properties.limits.nonCoherentAtomSize);
    if ((!range.bytes && !range.whole) || !mapped)
      throw std::runtime_error("GPU mapped-memory range is invalid; using CPU");
    VkMappedMemoryRange memoryRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    memoryRange.memory = memory; memoryRange.offset = range.offset;
    memoryRange.size = range.whole ? VK_WHOLE_SIZE : range.bytes;
    check(flush ? vkFlushMappedMemoryRanges(context.device, 1, &memoryRange) :
      vkInvalidateMappedMemoryRanges(context.device, 1, &memoryRange),
      flush ? "host-memory flush" : "host-memory invalidate");
  }
  void flush(uint64_t offset, uint64_t size) { synchronize(offset, size, true); }
  void invalidate(uint64_t offset, uint64_t size) { synchronize(offset, size, false); }
  void release() {
    if (mapped) vkUnmapMemory(context.device, memory);
    if (handle) vkDestroyBuffer(context.device, handle, nullptr);
    if (memory) budgetFreeMemory(context.device, memory, nullptr);
  }
  ~Buffer() { release(); }
};
struct Plan {
  VkFFTApplication app{};
  Buffer& buffer_;
  Plan(Context& context, Buffer& buffer, uint32_t length, uint32_t batches,
      bool twiddleLut = false) : buffer_(buffer) {
    VkFFTConfiguration config{};
    config.FFTdim = 1; config.size[0] = length; config.numberBatches = batches;
    // VkFFT's FP32 defaults calculate twiddles on NVIDIA/AMD but use a LUT on
    // Intel. Clutter's repeated cancellation needs the same bounded LUT path on
    // every backend; ambiguity retains the library's native tuning.
    if (twiddleLut) config.useLUT = 1;
    config.device = &context.device; config.queue = &context.queue;
    config.fence = &context.fence; config.commandPool = &context.pool;
    config.physicalDevice = &context.candidate.physical;
    // Bind at append time, after VkFFT has allocated prime-length FFT tables.
    // Binding during initialization can leave descriptors pointing at null LUTs.
    config.bufferSize = &buffer.bytes;
    config.isCompilerInitialized = 1; config.normalize = 1;
    config.warpSize = 32;
    config.coalescedMemory = 64;
    startupTrace(("VkFFT plan begin length=" + std::to_string(length) + " batches=" + std::to_string(batches)).c_str());
    const auto result = initializeVkFFT(&app, config);
    startupTrace(("VkFFT plan complete result=" + std::to_string(result)).c_str());
    if (result != VKFFT_SUCCESS) { deleteVkFFT(&app); checkFft(result); }
  }
  ~Plan() { deleteVkFFT(&app); }
  void append(VkCommandBuffer command, int direction) {
    VkFFTLaunchParams params{}; params.commandBuffer = &command;
    params.buffer = &buffer_.handle;
    checkFft(VkFFTAppend(&app, direction, &params));
  }
};
struct Push { uint32_t count, range, doppler, delays; int32_t delayMin; };
struct Kernel {
  Context& context;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  Kernel(Context& ctx, const char* source, const std::vector<Buffer*>& buffers) : context(ctx) {
    try {
      glslang_input_t input{};
      input.language = GLSLANG_SOURCE_GLSL; input.stage = GLSLANG_STAGE_COMPUTE;
      input.client = GLSLANG_CLIENT_VULKAN; input.client_version = GLSLANG_TARGET_VULKAN_1_0;
      input.target_language = GLSLANG_TARGET_SPV; input.target_language_version = GLSLANG_TARGET_SPV_1_0;
      input.code = source; input.default_version = 450; input.default_profile = GLSLANG_NO_PROFILE;
      input.messages = static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
      input.resource = glslang_default_resource();
      auto shaderSource = std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)>(
        glslang_shader_create(&input), glslang_shader_delete);
      if (!shaderSource || !glslang_shader_preprocess(shaderSource.get(), &input) ||
          !glslang_shader_parse(shaderSource.get(), &input)) {
        const char* info = shaderSource ? glslang_shader_get_info_log(shaderSource.get()) : nullptr;
        const char* debug = shaderSource ? glslang_shader_get_info_debug_log(shaderSource.get()) : nullptr;
        std::string reason = "GPU shader compilation failed";
        if (info && *info) reason += std::string(": ") + info;
        if (debug && *debug) reason += std::string("; ") + debug;
        throw std::runtime_error(reason);
      }
      auto program = std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)>(
        glslang_program_create(), glslang_program_delete);
      if (!program) throw std::runtime_error("GPU shader compiler allocation failed");
      glslang_program_add_shader(program.get(), shaderSource.get());
      if (!glslang_program_link(program.get(), input.messages)) {
        const char* info = glslang_program_get_info_log(program.get());
        const char* debug = glslang_program_get_info_debug_log(program.get());
        std::string reason = "GPU shader link failed";
        if (info && *info) reason += std::string(": ") + info;
        if (debug && *debug) reason += std::string("; ") + debug;
        throw std::runtime_error(reason);
      }
      glslang_program_SPIRV_generate(program.get(), input.stage);
      const auto words = glslang_program_SPIRV_get_size(program.get());
      if (!words) throw std::runtime_error("GPU shader generation failed");
      VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shaderInfo.codeSize = words * sizeof(uint32_t);
      shaderInfo.pCode = glslang_program_SPIRV_get_ptr(program.get());
      check(vkCreateShaderModule(ctx.device, &shaderInfo, nullptr, &shader), "shader creation");
      std::vector<VkDescriptorSetLayoutBinding> bindings(buffers.size());
      for (uint32_t i = 0; i < buffers.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      setInfo.bindingCount = bindings.size(); setInfo.pBindings = bindings.data();
      check(vkCreateDescriptorSetLayout(ctx.device, &setInfo, nullptr, &setLayout), "descriptor layout");
      VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
      VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout;
      layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
      check(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layout), "pipeline layout");
      VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipelineInfo.layout = layout;
      pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipelineInfo.stage.module = shader; pipelineInfo.stage.pName = "main";
      check(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "compute pipeline");
      VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(buffers.size())};
      VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      poolInfo.maxSets = 1; poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
      check(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &pool), "descriptor pool");
      VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocation.descriptorPool = pool; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &setLayout;
      check(vkAllocateDescriptorSets(ctx.device, &allocation, &set), "descriptor allocation");
      for (uint32_t i = 0; i < buffers.size(); ++i) {
        VkDescriptorBufferInfo bufferInfo{buffers[i]->handle, 0, buffers[i]->bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set; write.dstBinding = i; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
      }
    } catch (...) { release(); throw; }
  }
  void release() {
    if (pipeline) vkDestroyPipeline(context.device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(context.device, shader, nullptr);
    if (layout) vkDestroyPipelineLayout(context.device, layout, nullptr);
    if (pool) vkDestroyDescriptorPool(context.device, pool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(context.device, setLayout, nullptr);
  }
  ~Kernel() { release(); }
  void append(VkCommandBuffer command, Push push) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t groups = (push.count + 127) / 128;
    const uint32_t x = std::min(groups, 65535u);
    vkCmdDispatch(command, x, (groups + x - 1) / x, 1);
  }
};

} // anonymous
} // blah2
