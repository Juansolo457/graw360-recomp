// GRAW native renderer: the Vulkan backend (backend.h; docs/VULKAN_RENDERER.md).
//
// M2: the device, the swapchain on the game window and the frame loop. The game's draws, clears
// and resolves are counted, not drawn yet (M3-M5); each guest Swap presents a cleared frame
// whose color cycles slowly, so a live window is easy to tell from a stuck one. Primary mode
// only (--graw_native_primary), selected with --graw_native_api=vulkan.
#pragma once

#include "backend.h"

#include <memory>

namespace graw::native {

class VulkanDevice;

class VulkanBackend final : public Backend {
 public:
  VulkanBackend();
  ~VulkanBackend() override;

  bool Initialize(const Options& options) override;
  void SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) override;

  void BeginCopySegment() override {}
  void Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
               std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) override {}
  void StartAudit(uint32_t frames) override {}
  PrepareCounters EndFramePrepare(uint64_t done) override { return {}; }
  bool CaptureRequested(uint64_t frame) override { return false; }
  bool capture_holds_guest() const override { return false; }
  bool capture_holding_guest() const override { return false; }

  void AddPrepareCounters(const PrepareCounters& c) override {}
  void LogFrames(uint32_t frames) override {}
  void BeginFrame(uint64_t frame_index) override { frame_index_ = frame_index; }
  void Draw(const DrawState& state, const DrawPrep* prep) override { ++stats_.skipped; }
  void Clear(const ClearCall& clear) override {}
  void Resolve(const ResolveParams& params) override {}
  SubmitResult SubmitEarly() override { return SubmitResult::kNotReady; }
  SwapResult Swap(const SwapCall& swap, std::optional<bool> own_window) override;
  void AfterSwap(uint64_t frame_index, uint64_t completed) override { frame_index_ = frame_index; }
  void ReleaseGameWindow() override;

  const DrawStats& draw_stats() const override { return stats_; }
  void ResetDrawStats() override { stats_ = DrawStats{}; }
  const TextureStats& texture_stats() const override { return texture_stats_; }
  uint64_t texture_resident_bytes() const override { return 0; }
  uint64_t texture_count() const override { return 0; }
  bool front_buffer_found() const override { return false; }
  const LumaProbe& luma_probe() const override { return luma_; }
  double gpu_last_frame_ms() const override { return 0.0; }
  void LogGpuTiming() override {}

 private:
  bool OwnGameWindow(bool own);

  std::unique_ptr<VulkanDevice> device_;
  bool window_taken_ = false;  // OwnGameWindow disconnected ReXGlue's presenter from the window
  std::atomic<bool>* paces_frames_ = nullptr;
  uint64_t frame_index_ = 0;
  DrawStats stats_;  // draws counted as skipped until M3 draws them
  TextureStats texture_stats_;
  LumaProbe luma_;
};

}  // namespace graw::native
