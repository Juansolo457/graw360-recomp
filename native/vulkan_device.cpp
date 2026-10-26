// GRAW native renderer: the Vulkan device (vulkan_device.h).
#include "vulkan_device.h"

#include "clock.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace graw::native {

namespace {

// Validation messages (--graw_native_vulkan_validation): errors and warnings go to the log, the
// first few of each in full.
std::atomic<uint32_t> g_validation_errors{0}, g_validation_warnings{0};

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidationMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    if (g_validation_errors.fetch_add(1) < 32)
      REXLOG_ERROR("GRAW native Vulkan validation: {}", data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    if (g_validation_warnings.fetch_add(1) < 32)
      REXLOG_WARN("GRAW native Vulkan validation: {}", data->pMessage);
  }
  return VK_FALSE;
}

VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

}  // namespace

bool VulkanDevice::Check(VkResult r, const char* what) {
  if (r == VK_SUCCESS) return true;
  REXLOG_ERROR("GRAW native Vulkan: {} failed ({})", what, int(r));
  return false;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    fn_.vkDeviceWaitIdle(device_);
    DestroySwapchainObjects();
    for (Frame& f : frames_) {
      if (f.acquired) fn_.vkDestroySemaphore(device_, f.acquired, nullptr);
      if (f.fence) fn_.vkDestroyFence(device_, f.fence, nullptr);
      if (f.pool) fn_.vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    fn_.vkDestroyDevice(device_, nullptr);
  }
  if (instance_) {
    if (surface_) fn_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (g_messenger) {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          fn_.vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
      if (destroy) destroy(instance_, g_messenger, nullptr);
      g_messenger = VK_NULL_HANDLE;
    }
    fn_.vkDestroyInstance(instance_, nullptr);
  }
#if defined(_WIN32)
  if (library_) FreeLibrary(static_cast<HMODULE>(library_));
#else
  if (library_) dlclose(library_);
#endif
}

bool VulkanDevice::Initialize(void* game_window, bool vsync, bool validation) {
  game_window_ = game_window;
  vsync_ = vsync;
  if (!LoadLoader() || !CreateInstance(validation) || !PickDevice() || !CreateDevice())
    return false;
  for (Frame& f : frames_) {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = queue_family_;
    if (!Check(fn_.vkCreateCommandPool(device_, &pool, nullptr, &f.pool), "vkCreateCommandPool"))
      return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = f.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (!Check(fn_.vkAllocateCommandBuffers(device_, &alloc, &f.cmd), "vkAllocateCommandBuffers"))
      return false;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!Check(fn_.vkCreateFence(device_, &fence, nullptr, &f.fence), "vkCreateFence"))
      return false;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!Check(fn_.vkCreateSemaphore(device_, &sem, nullptr, &f.acquired), "vkCreateSemaphore"))
      return false;
  }
  return BeginSlot();
}

bool VulkanDevice::LoadLoader() {
#if defined(_WIN32)
  HMODULE dll = LoadLibraryW(L"vulkan-1.dll");
  library_ = dll;
  if (dll)
    fn_.vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll, "vkGetInstanceProcAddr"));
#else
  library_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (library_)
    fn_.vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
#endif
  if (!fn_.vkGetInstanceProcAddr) {
    REXLOG_ERROR("GRAW native Vulkan: no Vulkan loader (vulkan-1.dll / libvulkan.so.1)");
    return false;
  }
  auto global = [&](const char* name) { return fn_.vkGetInstanceProcAddr(nullptr, name); };
  fn_.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(global("vkCreateInstance"));
  fn_.vkEnumerateInstanceExtensionProperties =
      reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
          global("vkEnumerateInstanceExtensionProperties"));
  fn_.vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
      global("vkEnumerateInstanceLayerProperties"));
  return fn_.vkCreateInstance && fn_.vkEnumerateInstanceExtensionProperties &&
         fn_.vkEnumerateInstanceLayerProperties;
}

bool VulkanDevice::CreateInstance(bool validation) {
  std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
#if defined(_WIN32)
  extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
  std::vector<const char*> layers;
  if (validation) {
    uint32_t n = 0;
    fn_.vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> available(n);
    fn_.vkEnumerateInstanceLayerProperties(&n, available.data());
    const bool found = std::any_of(available.begin(), available.end(), [](const auto& l) {
      return !std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
    });
    if (found) {
      layers.push_back("VK_LAYER_KHRONOS_validation");
      extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
      REXLOG_WARN("GRAW native Vulkan: validation requested, VK_LAYER_KHRONOS_validation not installed");
    }
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "GRAW 360 Recompiled";
  app.pEngineName = "graw native";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  ci.enabledLayerCount = uint32_t(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  if (!Check(fn_.vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance")) return false;
#define GRAW_VK_LOAD(name)                                                                  \
  fn_.name = reinterpret_cast<PFN_##name>(fn_.vkGetInstanceProcAddr(instance_, #name));    \
  if (!fn_.name) {                                                                          \
    REXLOG_ERROR("GRAW native Vulkan: {} missing", #name);                                 \
    return false;                                                                           \
  }
  GRAW_VK_INSTANCE_FUNCTIONS(GRAW_VK_LOAD)
#if defined(_WIN32)
  GRAW_VK_LOAD(vkCreateWin32SurfaceKHR)
#endif
#undef GRAW_VK_LOAD
  if (!layers.empty()) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        fn_.vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = OnValidationMessage;
    if (create) create(instance_, &mi, nullptr, &g_messenger);
    REXLOG_INFO("GRAW native Vulkan: validation layer on");
  }
  return true;
}

bool VulkanDevice::PickDevice() {
  uint32_t n = 0;
  fn_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  std::vector<VkPhysicalDevice> devices(n);
  fn_.vkEnumeratePhysicalDevices(instance_, &n, devices.data());
  int best = -1;
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceProperties p;
    fn_.vkGetPhysicalDeviceProperties(d, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) continue;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f12;
    fn_.vkGetPhysicalDeviceFeatures2(d, &f);
    if (!f12.scalarBlockLayout || !f13.dynamicRendering) {
      REXLOG_INFO("GRAW native Vulkan: {} lacks scalarBlockLayout or dynamicRendering, skipped",
                  p.deviceName);
      continue;
    }
    uint32_t qn = 0;
    fn_.vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qn);
    fn_.vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, queues.data());
    int family = -1;
    for (uint32_t i = 0; i < qn && family < 0; ++i)
      if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = int(i);
    if (family < 0) continue;
    const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                               : 1;
    if (score > best) {
      best = score;
      physical_ = d;
      queue_family_ = uint32_t(family);
    }
  }
  if (!physical_) {
    REXLOG_ERROR("GRAW native Vulkan: no Vulkan 1.2 GPU with scalarBlockLayout");
    return false;
  }
  VkPhysicalDeviceProperties p;
  fn_.vkGetPhysicalDeviceProperties(physical_, &p);
  REXLOG_INFO("GRAW native Vulkan: {} (Vulkan {}.{}.{}, driver {:08X})", p.deviceName,
              VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
              VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
  return true;
}

bool VulkanDevice::CreateDevice() {
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueFamilyIndex = queue_family_;
  q.queueCount = 1;
  q.pQueuePriorities = &priority;
  // What the SPIR-V translator can use (its Features, as SpirvShaderTranslator::Features reads
  // them from the SDK's VulkanDevice): the supported ones are enabled and passed to it.
  VkPhysicalDeviceProperties props;
  fn_.vkGetPhysicalDeviceProperties(physical_, &props);
  VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  supported.pNext = &s12;
  s12.pNext = &s13;
  fn_.vkGetPhysicalDeviceFeatures2(physical_, &supported);
  VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
  VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props2.pNext = &p12;
  auto get_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      fn_.vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceProperties2"));
  if (get_props2) get_props2(physical_, &props2);
  caps_.api_version = props.apiVersion;
  caps_.max_storage_buffer_range = props.limits.maxStorageBufferRange;
  caps_.min_uniform_buffer_offset_alignment = props.limits.minUniformBufferOffsetAlignment;
  const VkPhysicalDeviceFeatures& sf = supported.features;
  caps_.full_draw_index_uint32 = sf.fullDrawIndexUint32;
  caps_.geometry_shader = sf.geometryShader;
  caps_.sampler_anisotropy = sf.samplerAnisotropy;
  caps_.max_sampler_anisotropy = props.limits.maxSamplerAnisotropy;
  caps_.sampler_mirror_clamp_to_edge = s12.samplerMirrorClampToEdge;
  caps_.texture_compression_bc = sf.textureCompressionBC;
  caps_.storage_image_write_without_format = sf.shaderStorageImageWriteWithoutFormat;
  caps_.vertex_pipeline_stores_and_atomics = sf.vertexPipelineStoresAndAtomics;
  caps_.fragment_stores_and_atomics = sf.fragmentStoresAndAtomics;
  caps_.clip_distance = sf.shaderClipDistance;
  caps_.cull_distance = sf.shaderCullDistance;
  caps_.sample_rate_shading = sf.sampleRateShading;
  caps_.demote_to_helper_invocation = s13.shaderDemoteToHelperInvocation;
  caps_.signed_zero_inf_nan_preserve_float32 = p12.shaderSignedZeroInfNanPreserveFloat32;
  caps_.denorm_flush_to_zero_float32 = p12.shaderDenormFlushToZeroFloat32;
  caps_.rounding_mode_rte_float32 = p12.shaderRoundingModeRTEFloat32;
  REXLOG_INFO(
      "GRAW native Vulkan: shader caps: clip {} cull {} vertex stores {} fragment stores {} "
      "sample rate {} demote {} | float32 signed-zero/inf/nan {} denorm-ftz {} rte {} | max "
      "storage buffer {} MB",
      caps_.clip_distance, caps_.cull_distance, caps_.vertex_pipeline_stores_and_atomics,
      caps_.fragment_stores_and_atomics, caps_.sample_rate_shading,
      caps_.demote_to_helper_invocation, caps_.signed_zero_inf_nan_preserve_float32,
      caps_.denorm_flush_to_zero_float32, caps_.rounding_mode_rte_float32,
      caps_.max_storage_buffer_range >> 20);
  VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  enabled.features.fullDrawIndexUint32 = sf.fullDrawIndexUint32;
  enabled.features.vertexPipelineStoresAndAtomics = sf.vertexPipelineStoresAndAtomics;
  enabled.features.fragmentStoresAndAtomics = sf.fragmentStoresAndAtomics;
  enabled.features.shaderClipDistance = sf.shaderClipDistance;
  enabled.features.shaderCullDistance = sf.shaderCullDistance;
  enabled.features.sampleRateShading = sf.sampleRateShading;
  enabled.features.samplerAnisotropy = sf.samplerAnisotropy;
  enabled.features.independentBlend = sf.independentBlend;
  enabled.features.depthClamp = sf.depthClamp;
  enabled.features.depthBiasClamp = sf.depthBiasClamp;
  enabled.features.fillModeNonSolid = sf.fillModeNonSolid;
  enabled.features.geometryShader = sf.geometryShader;
  enabled.features.textureCompressionBC = sf.textureCompressionBC;
  enabled.features.shaderStorageImageWriteWithoutFormat = sf.shaderStorageImageWriteWithoutFormat;
  // M3 on: the SPIR-V translator's shaders use scalar block layout (docs/VULKAN_RENDERER.md, M0).
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.scalarBlockLayout = VK_TRUE;
  f12.samplerMirrorClampToEdge = caps_.sampler_mirror_clamp_to_edge;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.shaderDemoteToHelperInvocation = caps_.demote_to_helper_invocation;
  // M3c-3: render targets without render pass objects.
  f13.dynamicRendering = VK_TRUE;
  enabled.pNext = &f12;
  f12.pNext = &f13;
  const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.pNext = &enabled;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &q;
  ci.enabledExtensionCount = 1;
  ci.ppEnabledExtensionNames = extensions;
  if (!Check(fn_.vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice")) return false;
#define GRAW_VK_LOAD(name)                                                              \
  fn_.name = reinterpret_cast<PFN_##name>(fn_.vkGetDeviceProcAddr(device_, #name));    \
  if (!fn_.name) {                                                                      \
    REXLOG_ERROR("GRAW native Vulkan: {} missing", #name);                             \
    return false;                                                                       \
  }
  GRAW_VK_DEVICE_FUNCTIONS(GRAW_VK_LOAD)
#undef GRAW_VK_LOAD
  fn_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  return true;
}

bool VulkanDevice::AttachSwapchain() {
  if (swapchain_) return true;
  if (!surface_) {
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = static_cast<HWND>(game_window_);
    if (!Check(fn_.vkCreateWin32SurfaceKHR(instance_, &si, nullptr, &surface_),
               "vkCreateWin32SurfaceKHR"))
      return false;
#else
    REXLOG_ERROR("GRAW native Vulkan: no window surface on this platform yet");
    return false;
#endif
    VkBool32 supported = VK_FALSE;
    fn_.vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, surface_, &supported);
    if (!supported) {
      REXLOG_ERROR("GRAW native Vulkan: the graphics queue can't present to the game window");
      return false;
    }
  }
  return CreateSwapchain();
}

void VulkanDevice::DetachSwapchain() {
  if (!device_) return;
  fn_.vkDeviceWaitIdle(device_);
  DestroySwapchainObjects();
  if (surface_) {
    fn_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
}

void VulkanDevice::DestroySwapchainObjects() {
  for (VkSemaphore s : rendered_) fn_.vkDestroySemaphore(device_, s, nullptr);
  rendered_.clear();
  images_.clear();
  if (swapchain_) {
    fn_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

bool VulkanDevice::CreateSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  if (!Check(fn_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
    return false;
  VkExtent2D extent = caps.currentExtent;
#if defined(_WIN32)
  if (extent.width == UINT32_MAX) {
    RECT r{};
    GetClientRect(static_cast<HWND>(game_window_), &r);
    extent = {uint32_t(r.right - r.left), uint32_t(r.bottom - r.top)};
  }
#endif
  if (!extent.width || !extent.height) return true;  // minimized: no swapchain for now
  uint32_t n = 0;
  fn_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  fn_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
  if (formats.empty()) return Check(VK_ERROR_FORMAT_NOT_SUPPORTED, "surface formats");
  // The D3D12 path's back buffer format first (R8G8B8A8_UNORM), then BGRA.
  VkSurfaceFormatKHR format = formats[0];
  for (VkFormat want : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
    const auto it = std::find_if(formats.begin(), formats.end(), [&](const auto& f) {
      return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (it != formats.end()) {
      format = *it;
      break;
    }
  }
  fn_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  fn_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync_) {
    for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
      if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
        mode = want;
        break;
      }
    }
  }
  uint32_t count = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface_;
  ci.minImageCount = count;
  ci.imageFormat = format.format;
  ci.imageColorSpace = format.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = mode;
  ci.clipped = VK_TRUE;
  if (!Check(fn_.vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR"))
    return false;
  fn_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  images_.resize(n);
  fn_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
  rendered_.resize(n);
  for (VkSemaphore& s : rendered_) {
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!Check(fn_.vkCreateSemaphore(device_, &sem, nullptr, &s), "vkCreateSemaphore")) return false;
  }
  swapchain_format_ = format.format;
  extent_ = extent;
  static constexpr const char* kModeNames[] = {"immediate", "mailbox", "fifo", "fifo relaxed"};
  REXLOG_INFO("GRAW native Vulkan: swapchain {}x{}, {} images, format {}, {}", extent.width,
              extent.height, n, int(format.format), int(mode) < 4 ? kModeNames[mode] : "other");
  return true;
}

bool VulkanDevice::BeginSlot() {
  Frame& f = frames_[slot_];
  if (f.number) {
    if (!Check(fn_.vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
      return false;
    completed_ = std::max(completed_, f.number);
    fn_.vkResetFences(device_, 1, &f.fence);
  }
  fn_.vkResetCommandPool(device_, f.pool, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return Check(fn_.vkBeginCommandBuffer(f.cmd, &bi), "vkBeginCommandBuffer");
}

bool VulkanDevice::EndFrame(bool present, const float color[4]) {
  return EndFrame(present, color, nullptr);
}

bool VulkanDevice::EndFrame(bool present, const float color[4], const FrameSource* source) {
  times_.presented = false;
  times_.submit_ns = times_.present_ns = 0;
  Frame& f = frames_[slot_];
  uint32_t image = UINT32_MAX;
  if (present && swapchain_) {
    VkResult r = fn_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired,
                                           VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      // Resized: a new swapchain, shown from the next frame.
      fn_.vkDeviceWaitIdle(device_);
      DestroySwapchainObjects();
      if (!CreateSwapchain()) return false;
      image = UINT32_MAX;
    } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
      return Check(r, "vkAcquireNextImageKHR");
    }
  }
  if (image != UINT32_MAX) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = images_[image];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fn_.vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue clear;
    if (source) {
      clear = {};
      clear.float32[3] = 1.0f;  // black bars
    } else {
      std::memcpy(clear.float32, color, sizeof(clear.float32));
    }
    fn_.vkCmdClearColorImage(f.cmd, images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                             1, &b.subresourceRange);
    if (source) {
      VkImageMemoryBarrier sb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      sb.image = source->image;
      sb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      const bool transition = source->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      if (transition) {
        sb.oldLayout = source->layout;
        sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sb.srcAccessMask = source->access;
        sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        fn_.vkCmdPipelineBarrier(f.cmd, source->stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &sb);
      }
      // The back buffer after its clear.
      VkImageMemoryBarrier cb = b;
      cb.oldLayout = cb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      cb.srcAccessMask = cb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      fn_.vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cb);
      // Fit the region in the back buffer, aspect kept.
      const int32_t sw = std::max(1, source->x1 - source->x0);
      const int32_t sh = std::max(1, source->y1 - source->y0);
      const double scale = std::min(double(extent_.width) / sw, double(extent_.height) / sh);
      const int32_t dw = std::max(1, int32_t(sw * scale)), dh = std::max(1, int32_t(sh * scale));
      const int32_t dx = (int32_t(extent_.width) - dw) / 2, dy = (int32_t(extent_.height) - dh) / 2;
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[0] = {source->x0, source->y0, 0};
      blit.srcOffsets[1] = {source->x1, source->y1, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {dx, dy, 0};
      blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
      fn_.vkCmdBlitImage(f.cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         images_[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                         VK_FILTER_LINEAR);
      if (transition) {
        sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sb.newLayout = source->layout;
        sb.srcAccessMask = 0;
        sb.dstAccessMask = source->access;
        fn_.vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, source->stages, 0, 0,
                                 nullptr, 0, nullptr, 1, &sb);
      }
    }
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    fn_.vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
  }
  if (!Check(fn_.vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer")) return false;
  const uint64_t t0 = NowNs();
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  if (image != UINT32_MAX) {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[image];
  }
  if (!Check(fn_.vkQueueSubmit(queue_, 1, &si, f.fence), "vkQueueSubmit")) return false;
  f.number = ++submitted_;
  const uint64_t t1 = NowNs();
  times_.submit_ns = t1 - t0;
  if (image != UINT32_MAX) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image;
    const VkResult r = fn_.vkQueuePresentKHR(queue_, &pi);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
      times_.presented = true;
      ++times_.presents;
    } else if (r != VK_ERROR_OUT_OF_DATE_KHR) {
      return Check(r, "vkQueuePresentKHR");
    }
    times_.present_ns = NowNs() - t1;
  }
  slot_ = (slot_ + 1) % kFramesInFlight;
  return BeginSlot();
}

}  // namespace graw::native
