// GRAW native renderer: the Vulkan backend (vulkan_backend.h).
#include "vulkan_backend.h"

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "game_window.h"
#include "vulkan_shaders.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

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
  shaders_ = std::make_unique<VulkanShaderCache>(*device_);
  membase_ = rex::system::kernel_memory()->physical_membase();
  REXLOG_INFO("GRAW native Vulkan: up; draws aren't recorded yet (M3a: shaders and cleared frames)");
  return true;
}

rex::graphics::SpirvShader* VulkanBackend::LoadShader(const ShaderRef& ref,
                                                      rex::graphics::xenos::ShaderType type) {
  if (!ref.ucode || !ref.dwords) return nullptr;
  // Declaration-patched vertex shaders (IM_LOAD_IMMEDIATE) come from the tracker's copy.
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase_ + ref.ucode);
  return shaders_->Load(type, ucode, ref.dwords);
}

void VulkanBackend::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                            std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) {
  // The render thread can't read the shaders later: the tracker's patched copies change.
  prep->vs = LoadShader(state.vs, rex::graphics::xenos::ShaderType::kVertex);
  prep->ps = LoadShader(state.ps, rex::graphics::xenos::ShaderType::kPixel);
}

void VulkanBackend::Draw(const DrawState& state, const DrawPrep* prep) {
  using rex::graphics::xenos::ShaderType;
  auto* vs = static_cast<SpirvShader*>(prep ? prep->vs : LoadShader(state.vs, ShaderType::kVertex));
  auto* ps = static_cast<SpirvShader*>(prep ? prep->ps : LoadShader(state.ps, ShaderType::kPixel));
  if (!vs) {
    ++stats_.skipped;
    return;
  }
  const ShaderModifications mods = SelectSpirvModifications(
      shaders_->translator(), *vs, ps, ReadShaderDrawRegs(state.regs, state.primitive));
  if (!mods.draw) {
    ++stats_.skipped;
    return;
  }
  const bool ok = shaders_->Module(*vs, mods.vertex) &&
                  (!mods.pixel_shader_used || shaders_->Module(*ps, mods.pixel));
  // M3a: translated and turned into modules, not drawn yet.
  if (ok)
    ++stats_.unsupported;
  else
    ++stats_.failed;
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
  if (frame_index_ >= frames_logged_ + 150) {
    frames_logged_ = frame_index_;
    const VulkanShaderStats& s = shaders_->stats();
    REXLOG_INFO("GRAW native Vulkan shaders: {} shaders, {} translations ({:.0f} us total, max "
                "{:.0f} us), {} modules, {} failed",
                s.shaders, s.translations, s.translate_us, s.max_translate_us, s.modules,
                s.failures);
  }
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
