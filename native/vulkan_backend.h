// GRAW native renderer: the Vulkan backend (backend.h; docs/VULKAN_RENDERER.md).
//
// M2: the device, the swapchain on the game window and the frame loop; each guest Swap presents
// a cleared frame whose color cycles slowly, so a live window is easy to tell from a stuck one.
// M3a: each draw's shaders are translated to SPIR-V for the variant its registers select, and
// get shader modules (vulkan_shaders.h).
// M3c-1: guest memory mirrored in a shared-memory storage buffer through GuestMemoryMirror (this
// backend is its Sink): each draw's vertex and index ranges are copied there; the draws aren't
// recorded yet.
// M3c-2: each draw's descriptor sets (vulkan_bindings.h): shared memory, the five constant
// buffers with the system constants, placeholder textures, bound for its pipeline layout.
// M3c-3: the draws recorded: host render targets (vulkan_render_targets.h, dynamic rendering),
// pipelines from the render state (vulkan_pipelines.h), the guest's clears. Points, rectangles
// and quads (geometry shaders) not yet.
// M3d: the frame shown at Swap: each color Resolve copies its source rectangle (MSAA resolved to
// 1x) into an image kept per destination address, as the D3D12 Resolver does (the guest may
// clear the source right after); the Swap's front buffer address picks the image, which is
// scaled into the back buffer. Not yet (M5): those images as the game's textures, the resolves'
// clears, the exponent bias and the gamma ramp.
// Primary mode only (--graw_native_primary), selected with --graw_native_api=vulkan.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include "backend.h"
#include "guest_memory_mirror.h"
#include "render_targets.h"  // SurfaceRegs

#include <rex/graphics/xenos.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace rex::graphics {
class SpirvShader;
}

namespace graw::native {

class VulkanDevice;
struct VulkanRenderTarget;
class VulkanShaderCache;
struct ShaderRef;

class VulkanBackend final : public Backend, public GuestMemoryMirror::Sink {
 public:
  VulkanBackend();
  ~VulkanBackend() override;

  bool Initialize(const Options& options) override;
  void SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) override;

  void BeginCopySegment() override { mirror_.NewSegment(); }
  void Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
               std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) override;
  void StartAudit(uint32_t frames) override {}
  PrepareCounters EndFramePrepare(uint64_t done) override;
  bool CaptureRequested(uint64_t frame) override { return false; }
  bool capture_holds_guest() const override { return false; }
  bool capture_holding_guest() const override { return false; }

  void AddPrepareCounters(const PrepareCounters& c) override;
  void LogFrames(uint32_t frames) override {}
  void BeginFrame(uint64_t frame_index) override;
  void Draw(const DrawState& state, const DrawPrep* prep) override;
  void Clear(const ClearCall& clear) override;
  void Resolve(const ResolveParams& params) override;
  SubmitResult SubmitEarly() override { return SubmitResult::kNotReady; }
  SwapResult Swap(const SwapCall& swap, std::optional<bool> own_window) override;
  void AfterSwap(uint64_t frame_index, uint64_t completed) override;
  void ReleaseGameWindow() override;

  const DrawStats& draw_stats() const override { return stats_; }
  void ResetDrawStats() override { stats_ = DrawStats{}; }
  const TextureStats& texture_stats() const override { return texture_stats_; }
  uint64_t texture_resident_bytes() const override { return 0; }
  uint64_t texture_count() const override { return 0; }
  bool front_buffer_found() const override { return front_buffer_found_; }
  const LumaProbe& luma_probe() const override { return luma_; }
  double gpu_last_frame_ms() const override { return 0.0; }
  void LogGpuTiming() override {}

  // GuestMemoryMirror::Sink. The shared buffer is allocated whole (no sparse binding yet).
  bool MapShared(uint32_t start, uint32_t length) override { return true; }
  bool AllocateUpload(uint32_t size, uint64_t tag, GuestMemoryMirror::Upload* out) override;

 private:
  bool OwnGameWindow(bool own);
  // Records count copies into shared memory, between the barriers that order them after the
  // draws before and ahead of the draws after.
  void RecordCopies(const VertexCopy* copies, size_t count);
  // The shader a draw binds, from guest memory (or the tracker's patched copy).
  rex::graphics::SpirvShader* LoadShader(const ShaderRef& ref,
                                         rex::graphics::xenos::ShaderType type);

  std::unique_ptr<VulkanDevice> device_;
  std::unique_ptr<VulkanShaderCache> shaders_;
  struct Gpu;  // memory, the shared buffer, upload pools (vulkan_backend.cpp)
  struct ResolvedImage;  // a resolve destination (vulkan_backend.cpp)
  std::unique_ptr<Gpu> gpu_;
  GuestMemoryMirror mirror_;
  bool threaded_ = false;  // --graw_native_render_thread: Prepare runs on the guest thread
  // Prepare's counters on the guest thread, handed over at EndFramePrepare (with the render
  // thread); diag_ is the recording thread's without it.
  uint64_t prep_upload_bytes_ = 0, prep_uploads_ = 0;
  PrepareDiag prep_diag_, diag_;
  std::vector<VertexCopy> copies_;  // without the render thread: the draw's copies
  const uint8_t* membase_ = nullptr;  // guest physical memory
  uint64_t frames_logged_ = 0;        // Swap: the shader summary every 150 frames
  bool window_taken_ = false;  // OwnGameWindow disconnected ReXGlue's presenter from the window
  std::atomic<bool>* paces_frames_ = nullptr;
  uint64_t frame_index_ = 0;
  DrawStats stats_;
  bool front_buffer_found_ = false;
  bool front_buffer_logged_ = false;
  uint64_t resolves_ = 0, resolves_skipped_ = 0;
  // Copies rt's (x0, y0, w, h) to (dx, dy) of the dest_w x dest_h image kept under `key` (a
  // resolve's destination address), resolving MSAA; the image is then ready to be shown.
  // dest_format: the image's format (VK_FORMAT_UNDEFINED: rt's); another one is blitted.
  bool CopyTarget(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w, int32_t h,
                  int32_t dx, int32_t dy, uint32_t dest_w, uint32_t dest_h, uint32_t key,
                  VkFormat dest_format = VK_FORMAT_UNDEFINED);
  // r, (re)created for the format and size (waiting for the GPU when it existed); null: failed.
  ResolvedImage* EnsureImage(ResolvedImage& r, VkFormat format, uint32_t width,
                             uint32_t height, const char* what);
  bool CanBlit(VkFormat src, VkFormat dst);
  std::unordered_map<uint64_t, bool> blit_support_;
  // --graw_native_vulkan_show_target: this frame's draws and lowest drawn row per color target.
  struct TargetUse {
    uint32_t draws = 0, rows = 0;
  };
  std::unordered_map<VulkanRenderTarget*, TargetUse> target_use_;
  // M4: the views, samplers and signs of a shader's texture bindings (uploading what changed,
  // which ends the rendering instance); appended to texture_views_ / samplers_.
  void GetShaderTextures(const rex::graphics::SpirvShader& shader, const DrawState& state,
                      uint8_t signs[32]);
  std::vector<VkImageView> texture_views_;
  std::vector<VkSampler> samplers_;
  // Reasons draws were unsupported, logged once each.
  std::unordered_set<const char*> unsupported_seen_;
  uint32_t unsupported_logged_ = 0;
  TextureStats texture_stats_;
  LumaProbe luma_;
};

}  // namespace graw::native
