// GRAW native renderer: the Vulkan backend (vulkan_backend.h).
#include "vulkan_backend.h"

#include "game_window.h"
#include "vulkan_device.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cmath>

REXCVAR_DEFINE_BOOL(graw_native_vulkan_validation, false, "GRAW native",
                    "With --graw_native_api=vulkan: enable the Khronos validation layer when it "
                    "is installed (the Vulkan SDK); its errors and warnings go to the log");

namespace graw::native {

VulkanBackend::VulkanBackend() = default;
VulkanBackend::~VulkanBackend() = default;

bool VulkanBackend::Initialize(const Options& o) {
  if (!o.primary) {
    REXLOG_ERROR("GRAW native Vulkan: needs --graw_native_primary (no debug window yet)");
    return false;
  }
  device_ = std::make_unique<VulkanDevice>();
  if (!device_->Initialize(o.game_window, o.vsync, REXCVAR_GET(graw_native_vulkan_validation)))
    return false;
  REXLOG_INFO("GRAW native Vulkan: up; draws aren't rendered yet (M2: cleared frames)");
  return true;
}

void VulkanBackend::SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) {
  paces_frames_ = paces_frames;
  if (pacer) REXLOG_WARN("GRAW native Vulkan: frame pacing on the display isn't supported yet");
}

void VulkanBackend::ReleaseGameWindow() {
  if (paces_frames_) paces_frames_->store(false, std::memory_order_relaxed);
  if (device_ && device_->has_swapchain()) device_->DetachSwapchain();
  if (window_taken_) {
    ConnectSdkPresenter(true);
    window_taken_ = false;
    REXLOG_INFO("GRAW native: the game window shows ReXGlue's presenter");
  }
}

bool VulkanBackend::OwnGameWindow(bool own) {
  if (own == window_taken_) return true;
  if (!own) {
    ReleaseGameWindow();
    return true;
  }
  ConnectSdkPresenter(false);
  window_taken_ = true;
  return device_->AttachSwapchain();
}

Backend::SwapResult VulkanBackend::Swap(const SwapCall& swap, std::optional<bool> own_window) {
  SwapResult r;
  if (own_window && !OwnGameWindow(*own_window)) return r;
  // A slow hue cycle (about 8 s per turn at 60 fps), dark enough not to dazzle.
  const float t = float(frame_index_) * (6.2831853f / 480.0f);
  const float color[4] = {0.15f + 0.1f * std::sin(t), 0.15f + 0.1f * std::sin(t + 2.094f),
                          0.15f + 0.1f * std::sin(t + 4.189f), 1.0f};
  r.fence = device_->frame_fence();
  if (!device_->EndFrame(window_taken_, color)) return r;
  const VulkanDevice::EndFrameTimes& et = device_->end_frame_times();
  r.presented = et.presented;
  r.presents = et.presents;
  r.submit_ns = et.submit_ns;
  r.present_ns = et.present_ns;
  r.completed = device_->completed_fence();
  r.ok = true;
  return r;
}

}  // namespace graw::native
