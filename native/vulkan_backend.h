// GRAW native renderer: the Vulkan backend (backend.h; docs/VULKAN_RENDERER.md).
//
// M2: the device, the swapchain on the game window and the frame loop.
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
// clear the source right after); the Swap's front buffer address picks the image.
// M4: guest textures and samplers (vulkan_textures.h), resolve images bound as textures, the
// resolve shaders (vulkan_resolve.h: exponent bias, 7e3, red/blue swap), geometry shaders,
// background pipeline compiles and the pipeline cache.
// M5: the front buffer shown through the present pass (vulkan_present.h): its fetch
// constant's swizzle and the display controller's gamma ramp, scaled to fit as D3D12 does.
// Primary mode only (--graw_native_primary), selected with --graw_native_api=vulkan.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include "backend.h"
#include "guest_memory_mirror.h"
#include "render_targets.h"  // SurfaceRegs

#include <rex/graphics/xenos.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace rex::graphics {
class SpirvShader;
}

namespace graw::native {

class VulkanDevice;
class PipelineStore;
struct VulkanPipelineKey;
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
  // M4d: pipelines that survive runs (pipeline_store.h, in <cache_root>/graw-native-vulkan):
  // recorded as first created, compiled at start-up in later runs (Prewarm, a slice per
  // frame). A frame whose draws waited for pipelines is held (pipeline_wait_).
  void RecordPipeline(const VulkanPipelineKey& key, const rex::graphics::SpirvShader& vs,
                      uint64_t vs_mod, const rex::graphics::SpirvShader* ps, uint64_t ps_mod,
                      uint32_t gs_key);
  void Prewarm();
  std::unique_ptr<PipelineStore> store_;
  size_t prewarm_next_ = 0;
  uint32_t prewarm_failed_ = 0, prewarm_frames_ = 0;
  double prewarm_ms_ = 0;
  bool pipeline_wait_ = false;
  // --graw_native_vulkan_skip_draws: this frame's draw count, and the range skipped.
  uint32_t frame_draw_ = 0;
  uint32_t skip_first_ = UINT32_MAX, skip_last_ = 0;
  DrawStats stats_;
  bool front_buffer_found_ = false;
  bool front_buffer_logged_ = false;
  uint64_t resolves_ = 0, resolves_skipped_ = 0, resolve_clears_ = 0, depth_resolves_ = 0;
  uint64_t shader_resolves_ = 0;  // color resolves through the shader (the rest: copies, blits)
  // Copies rt's (x0, y0, w, h) to (dx, dy) of the dest_w x dest_h image kept under `key` (a
  // resolve's destination address), resolving MSAA; the image is then ready to be shown.
  // dest_format: the image's format (VK_FORMAT_UNDEFINED: rt's); another one is blitted.
  // dest_format_signed: its signed views' format (a MUTABLE image), or UNDEFINED.
  bool CopyTarget(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w, int32_t h,
                  int32_t dx, int32_t dy, uint32_t dest_w, uint32_t dest_h, uint32_t key,
                  VkFormat dest_format = VK_FORMAT_UNDEFINED,
                  VkFormat dest_format_signed = VK_FORMAT_UNDEFINED);
  // Resolve shaders (vulkan_resolve.h): rt's (x0, y0, w, h) into the image at p.dest_base, at
  // (dx, dy): depth targets into R32_SFLOAT, color targets as the destination format's texel
  // bits. False: not done (a color format the shader can't write: CopyTarget instead).
  bool CopyDepth(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w, int32_t h,
                 int32_t dx, int32_t dy, const ResolveParams& p);
  bool CopyColor(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w, int32_t h,
                 int32_t dx, int32_t dy, const ResolveParams& p);
  // --graw_native_vulkan_resolve_dump: reads back the image of a resolve of the dumped frame,
  // texels (0, 0) to (width, height); AfterSwap writes them once the GPU finished the frame.
  void DumpRecord(const ResolveParams& p, uint32_t width, uint32_t height);
  void DumpWrite(uint64_t completed);
  bool DumpingFrame() const;  // the frame being recorded is dumped
  // --graw_native_vulkan_dump_target_draws: reads back color target rt after draw `index` of
  // the dumped frame.
  void DumpTarget(VulkanRenderTarget* rt, uint32_t index);
  uint32_t dump_target_first_ = UINT32_MAX, dump_target_last_ = 0;
  // Records a resolve shader reading rt (through `src_view`) and writing r's storage view:
  // `record` dispatches with the descriptor set.
  bool RunResolveShader(VulkanRenderTarget* rt, VkImageView src_view, ResolvedImage* r,
                        const std::function<void(VkDescriptorSet)>& record);
  // r, (re)created for the formats and size (the old image retired), with a storage view of
  // `storage_format` (UNDEFINED: none needed; a MUTABLE image when it is another format);
  // null: failed.
  ResolvedImage* EnsureImage(ResolvedImage& r, VkFormat format, VkFormat format_signed,
                             uint32_t width, uint32_t height, const char* what,
                             VkFormat storage_format = VK_FORMAT_UNDEFINED);
  void DestroyImage(ResolvedImage& r);
  // M5: the image Swap shows (the front buffer, or --graw_native_vulkan_show_target's), given
  // to the present pass; false: a black frame.
  bool SetPresentImage(const SwapCall& swap);
  bool front_format_logged_ = false;  // a front buffer format the present pass doesn't show
  uint32_t front_logged_ = 0;         // changes of the front buffer's format, swizzle, size...
  uint64_t front_seen_ = 0;           // ...last logged
  bool bias_dropped_logged_ = false;  // a resolve blit without its exponent bias
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
