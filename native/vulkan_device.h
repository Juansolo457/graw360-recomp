// GRAW native renderer: the Vulkan instance, device, queue, frames in flight and the swapchain on
// the game window (docs/VULKAN_RENDERER.md, M2). The Vulkan counterpart of d3d12_device.h.
//
// The loader is opened at run time (vulkan-1.dll, libvulkan.so.1) and every function used is
// loaded into Functions (VK_NO_PROTOTYPES): the prebuilt SDK the game links has no Vulkan.
// Requires Vulkan 1.2 with scalarBlockLayout, which the SDK's SPIR-V translator's shaders need
// (M0). --graw_native_vulkan_validation enables VK_LAYER_KHRONOS_validation when installed.
//
// Like D3D12Device in primary mode, the swapchain exists only between AttachSwapchain and
// DetachSwapchain, while the renderer owns the game window; the rest of the time ReXGlue's
// presenter has its own swapchain there. Debug window mode isn't supported.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#if defined(_WIN32)
#undef DrawState  // winuser.h maps it to DrawStateA/W; d3d_state.h's struct has that name
#endif

#include <array>
#include <cstdint>
#include <vector>

namespace graw::native {

#define GRAW_VK_INSTANCE_FUNCTIONS(X)        \
  X(vkDestroyInstance)                       \
  X(vkEnumeratePhysicalDevices)              \
  X(vkGetPhysicalDeviceProperties)           \
  X(vkGetPhysicalDeviceFeatures2)            \
  X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkEnumerateDeviceExtensionProperties)    \
  X(vkCreateDevice)                          \
  X(vkGetDeviceProcAddr)                     \
  X(vkDestroySurfaceKHR)                     \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)    \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)    \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define GRAW_VK_DEVICE_FUNCTIONS(X) \
  X(vkDestroyDevice)                \
  X(vkGetDeviceQueue)               \
  X(vkDeviceWaitIdle)               \
  X(vkCreateCommandPool)            \
  X(vkDestroyCommandPool)           \
  X(vkResetCommandPool)             \
  X(vkAllocateCommandBuffers)       \
  X(vkBeginCommandBuffer)           \
  X(vkEndCommandBuffer)             \
  X(vkCmdPipelineBarrier)           \
  X(vkCmdClearColorImage)           \
  X(vkCreateFence)                  \
  X(vkDestroyFence)                 \
  X(vkWaitForFences)                \
  X(vkResetFences)                  \
  X(vkCreateSemaphore)              \
  X(vkDestroySemaphore)             \
  X(vkQueueSubmit)                  \
  X(vkCreateSwapchainKHR)           \
  X(vkDestroySwapchainKHR)          \
  X(vkGetSwapchainImagesKHR)        \
  X(vkAcquireNextImageKHR)          \
  X(vkQueuePresentKHR)

class VulkanDevice {
 public:
  static constexpr uint32_t kFramesInFlight = 2;

  struct Functions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
#if defined(_WIN32)
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
#endif
#define GRAW_VK_DECLARE(name) PFN_##name name = nullptr;
    GRAW_VK_INSTANCE_FUNCTIONS(GRAW_VK_DECLARE)
    GRAW_VK_DEVICE_FUNCTIONS(GRAW_VK_DECLARE)
#undef GRAW_VK_DECLARE
  };

  // What the last EndFrame did (the frame CSV's swap columns).
  struct EndFrameTimes {
    bool presented = false;
    uint64_t presents = 0;  // presents so far
    uint64_t submit_ns = 0, present_ns = 0;
  };

  ~VulkanDevice();

  // Loader, instance, physical and logical device, queue, frame objects. `game_window`: the
  // window the swapchain is created on by AttachSwapchain (HWND on Windows). Logs and returns
  // false on failure.
  bool Initialize(void* game_window, bool vsync, bool validation);

  // Primary mode: create or release the swapchain on the game window.
  bool AttachSwapchain();
  void DetachSwapchain();
  bool has_swapchain() const { return swapchain_ != VK_NULL_HANDLE; }

  // The frame's command buffer, open from the previous EndFrame (BeginFrame waited for its
  // frame-in-flight slot).
  VkCommandBuffer command_buffer() const { return frames_[slot_].cmd; }
  // Frames submitted so far (= the number the current frame signals), and the frames the GPU
  // finished.
  uint64_t frame_fence() const { return submitted_ + 1; }
  uint64_t completed_fence() const { return completed_; }

  // M2: the back buffer cleared to `color`, then presented when `present` and a swapchain
  // exists; the frame's command buffer is submitted either way. False on a device failure.
  bool EndFrame(bool present, const float color[4]);
  const EndFrameTimes& end_frame_times() const { return times_; }

  const Functions& fn() const { return fn_; }
  VkDevice device() const { return device_; }

 private:
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    uint64_t number = 0;  // the frame it last carried (0: none)
  };
  bool LoadLoader();
  bool CreateInstance(bool validation);
  bool PickDevice();
  bool CreateDevice();
  bool CreateSwapchain();
  void DestroySwapchainObjects();
  bool BeginSlot();  // waits for the slot's previous frame and opens its command buffer
  bool Check(VkResult r, const char* what);

  void* library_ = nullptr;
  Functions fn_;
  void* game_window_ = nullptr;
  bool vsync_ = false;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::array<Frame, kFramesInFlight> frames_{};
  uint32_t slot_ = 0;
  uint64_t submitted_ = 0, completed_ = 0;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  std::vector<VkSemaphore> rendered_;  // per swapchain image: its clear and copies finished
  EndFrameTimes times_;
};

}  // namespace graw::native
