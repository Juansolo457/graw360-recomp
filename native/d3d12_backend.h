// GRAW native renderer: the D3D12 backend (backend.h). The device and swapchain
// (d3d12_device.h) and the modules that record the frames: render targets, resolves, textures,
// draws and the presenter. The modules reach each other and the device through it.
#pragma once

#include "backend.h"

#include <memory>
#include <vector>

struct ID3D12CommandList;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace graw::native {

class D3D12Device;
class DrawRenderer;
class GpuTimer;
class Presenter;
class RenderTargets;
class Resolver;
class TextureCache;

class D3D12Backend final : public Backend {
 public:
  D3D12Backend();
  ~D3D12Backend() override;

  bool Initialize(const Options& options) override;
  void SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) override;

  void BeginCopySegment() override;
  void Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
               std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) override;
  void StartAudit(uint32_t frames) override;
  PrepareCounters EndFramePrepare(uint64_t done) override;
  bool CaptureRequested(uint64_t frame) override;
  bool capture_holds_guest() const override;
  bool capture_holding_guest() const override;

  void AddPrepareCounters(const PrepareCounters& c) override;
  void LogFrames(uint32_t frames) override;
  void BeginFrame(uint64_t frame_index) override;
  void Draw(const DrawState& state, const DrawPrep* prep) override;
  void Clear(const ClearCall& clear) override;
  void Resolve(const ResolveParams& params) override;
  SubmitResult SubmitEarly() override;
  SwapResult Swap(const SwapCall& swap, std::optional<bool> own_window) override;
  void AfterSwap(uint64_t frame_index, uint64_t completed) override;
  void ReleaseGameWindow() override;

  const DrawStats& draw_stats() const override;
  void ResetDrawStats() override;
  const TextureStats& texture_stats() const override;
  uint64_t texture_resident_bytes() const override;
  uint64_t texture_count() const override;
  bool front_buffer_found() const override;
  const LumaProbe& luma_probe() const override;
  double gpu_last_frame_ms() const override;
  void LogGpuTiming() override;

  // For the modules.
  D3D12Device& device() { return *device_; }
  ID3D12Device* d3d12() const;
  ID3D12CommandQueue* command_queue() const;
  // The frame's direct command list: open from the previous Swap until Swap closes and submits
  // it. Record into it from the recording thread only.
  ID3D12GraphicsCommandList* command_list() const;
  // Swaps recorded so far (= index of the frame being recorded).
  uint64_t frame_index() const { return frame_index_; }
  // Fence value Swap will signal for the frame being recorded; a resource last used in this
  // frame may be freed once device().completed_fence() >= this.
  uint64_t frame_fence() const;
  TextureCache& textures() { return *textures_; }
  RenderTargets& render_targets() { return *rt_; }
  Resolver& resolver() { return *resolver_; }

 private:
  // Before each submission, once the untiles finished: the texture upload lists to submit first
  // (TextureCache::CloseUploadLists), into upload_lists_. False on failure (logged).
  bool CloseUploadLists();
  // Primary mode, at a Swap before anything is recorded into the back buffer: who presents into
  // the game window. The renderer while it draws the game and no XAM dialog is up; ReXGlue's
  // presenter otherwise (the dialogs are its ImGui overlay, and after the renderer switch it
  // shows the Xenos plugin's frames). A window has one swapchain at a time: the presenter's
  // surface is disconnected before ours is created, and ours released before it is connected
  // again. Returns false when the swapchain cannot be created (logged).
  bool OwnGameWindow(bool own);

  std::unique_ptr<D3D12Device> device_;
  bool window_taken_ = false;  // OwnGameWindow disconnected ReXGlue's presenter from the window
  std::unique_ptr<RenderTargets> rt_;
  std::unique_ptr<Resolver> resolver_;
  std::unique_ptr<TextureCache> textures_;
  std::unique_ptr<DrawRenderer> draw_;
  std::unique_ptr<Presenter> present_;
  GpuTimer* gpu_timer_ = nullptr;  // device_'s, with --graw_native_gpu_timing
  FramePacer* pacer_ = nullptr;
  std::atomic<bool>* paces_frames_ = nullptr;
  uint64_t frame_index_ = 0;
  std::vector<ID3D12CommandList*> upload_lists_;  // CloseUploadLists'
};

}  // namespace graw::native
