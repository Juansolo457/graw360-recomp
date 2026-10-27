// GRAW native renderer: the GPU API side, behind an interface (docs/VULKAN_RENDERER.md, M1).
//
// NativeRenderer (renderer.h) owns everything that doesn't depend on the GPU API: the hooks'
// entry points, the render thread's queue, the frame statistics and CSV, the renderer switch
// and the frame pacing. It hands the frames to a Backend: D3D12Backend (d3d12_backend.h) today;
// a Vulkan one next.
//
// Threading follows renderer.h. The guest thread calls the Prepare group (with the render
// thread: shader lookup and vertex/index copies at the draw's call) and the capture queries;
// the recording thread (the render thread, else the guest thread) everything else.
#pragma once

#include "d3d_state.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rex::graphics {
class Shader;
}

namespace graw::native {

class FramePacer;
struct ClearCall;
struct CoherencyJob;
struct ResolveParams;
struct SwapCall;

// Guest memory copied into upload memory, to be copied into shared memory at `dst`. `src` is the
// backend's upload buffer (ID3D12Resource* for D3D12).
struct VertexCopy {
  void* src;
  uint64_t src_offset;
  uint32_t dst, size;
};

// --graw_native_mark_audit: a guest range a draw reads, with the XXH3 of its bytes when the draw
// was prepared, which shared memory must hold when the GPU reaches the draw.
struct AuditRange {
  uint32_t start, size;
  uint64_t hash;
};

// The guest-thread half of one draw (Backend::Prepare).
struct DrawPrep {
  // The backend's shader objects (rex::graphics::DxbcShader for D3D12). vs nullptr: no vertex
  // shader (the draw is dropped).
  rex::graphics::Shader* vs = nullptr;
  rex::graphics::Shader* ps = nullptr;
  bool upload_ok = true;               // every vertex/index range was copied
  const VertexCopy* copies = nullptr;  // set by the render side before Draw
  uint32_t copy_count = 0;
  bool copy_segment = false;  // Prepare: the draw's copies start a copy segment
  // Triangle fans: the converted host indices in upload memory (a GPU address).
  uint64_t fan_indices = 0;
  uint32_t fan_count = 0;
  bool fan_wide = false;
  uint64_t captured = ~0ull;  // TextureCache::MarkCapture stamp (kCapturedNow: none)
  // --graw_native_coherency_check: XXH3 of the register file's blocks as Prepare saw them
  // (render state, float constants VS and PS, fetch and bool/loop constants), 0: not taken.
  uint64_t regs_hash[5] = {};
  // --graw_native_mark_audit: the ranges to check, set by the render side like `copies`.
  const AuditRange* audit = nullptr;
  uint32_t audit_count = 0;
};

// The draw path's counters, per frame (the frame CSV's columns).
struct DrawStats {
  uint64_t draws = 0;        // issued host draws
  uint64_t skipped = 0;      // dropped like the plugin does: nothing to draw or write
  uint64_t unsupported = 0;  // see draw.h; logged once per reason
  uint64_t failed = 0;       // shader/PSO/RT/upload failures
  uint64_t pso_created = 0;
  uint64_t pso_pending = 0;   // draws skipped while their pipeline compiles (async)
  uint64_t pso_generic = 0;   // draws on unspecialized shaders while a specialization compiles
  uint64_t pso_wait_us = 0;   // the frame waiting for the pipelines of its skipped draws
  uint64_t upload_bytes = 0;  // guest memory copied into shared memory
  uint64_t uploads = 0;       // copy regions recorded
  uint64_t textures = 0, textures_missing = 0;  // bindings; missing = null SRV bound
  uint64_t textures_stale = 0;  // bindings for draws queued before a newer guest write
  uint64_t srv_lookups = 0, srv_created = 0, samplers_created = 0;
  uint64_t views = 0;  // texture views in the bindless heap (at the frame's end)
  uint64_t bind_memo_hits = 0, bind_memo_misses = 0;  // whole-shader texture binding memo
  uint64_t watch_faults = 0;  // guest writes caught by a page watch (any watcher), per frame
  uint64_t copy_batches = 0;  // groups of shared-memory copies, each between two barriers
  uint64_t watch_arms = 0, watch_arm_pages = 0;  // vertex/index watches armed (calls, pages)
  // ... pages found written between their copy and the arm, and the watch thread's time.
  uint64_t watch_arm_changed = 0, watch_arm_ns = 0;
  uint64_t hash_pages = 0, hash_copies = 0;  // pages hashed, and copied as they changed
  // --graw_native_coherency_check: pages whose guest memory differed from shared memory's
  // copy although the page watch said it was current, and pages rewritten after a draw of
  // the frame read them.
  uint64_t stale_pages = 0, late_pages = 0;
  // ... and pages whose shared memory copy, read back at the end of a frame, differs from
  // the bytes the frame's copies carried.
  uint64_t gpu_mismatch_pages = 0;
  uint64_t reg_mismatches = 0;  // draws whose register file differs from Prepare's
};

// Prepare's draw path counters (the draw diag of --graw_native_profile).
struct PrepareDiag {
  uint64_t shader_loads = 0, shader_loads_immediate = 0;
  uint64_t ranges = 0, range_pages = 0, range_runs = 0;  // watched ranges, their pages, copies
  uint64_t unwatched = 0;                                // unwatched ranges (copied whole)
};

// Prepare's numbers for one frame (EndFramePrepare), added to the draw stats and the draw diag
// by AddPrepareCounters.
struct PrepareCounters {
  uint64_t upload_bytes = 0, uploads = 0, stale_pages = 0, late_pages = 0;
  PrepareDiag diag;
  std::shared_ptr<CoherencyJob> job;  // the frame's copied ranges to read back, or null
};

// The texture cache's counters (cumulative).
struct TextureStats {
  uint64_t gets = 0, uploads = 0, upload_bytes = 0, resolved_copies = 0, resolved_aliases = 0;
  uint64_t untile_ns = 0;
  // Profile counters: memo misses, textures created, time in Create (resource creation), Upload
  // (buffer creation + untile + copies) and CopyResolved.
  uint64_t memo_misses = 0, created = 0, create_ns = 0, upload_ns = 0, copy_resolved_ns = 0;
  uint64_t evicted = 0, evict_ns = 0;  // textures evicted over the budget, time in Retire
  uint64_t deferred_uploads = 0;       // uploads on the upload lists (CloseUploadLists)
  uint64_t upload_record_ns = 0;       // recording them (worker CPU, as untile_ns)
};

// --graw_native_luma_probe: mean luma of a few blocks of the presented frame.
struct LumaProbe {
  static constexpr uint32_t kRows = 3, kSegments = 4;
  uint64_t frame = 0;  // the frame they belong to (0: none yet)
  float luma[kRows][kSegments] = {};
};

class Backend {
 public:
  struct Options {
    uint32_t width = 1280, height = 720;  // the debug window's size
    bool primary = false;  // present into the game window (`game_window`), else a debug window
    void* game_window = nullptr;  // the platform's window handle (HWND on Windows)
    bool vsync = false;           // present on the display's refresh (primary only)
    bool frame_latency_waitable = false;  // the frame pacer needs the present feedback
    bool gpu_timing = false, gpu_timing_shaders = false;
    bool texture_capture_order = true;
  };

  // What the frame's Swap did (Swap).
  struct SwapResult {
    bool ok = false;          // false: the submission or present failed (the caller fails)
    bool incomplete = false;  // held back: a scene's first frame without its new pipelines
    bool presented = false;
    uint64_t presents = 0;    // the swapchain's present count, when presented
    uint64_t fence = 0;       // the fence value the frame signals
    uint64_t completed = 0;   // the fence value the GPU completed, after the submission
    uint64_t untile_ns = 0, submit_ns = 0, present_ns = 0, fence_ns = 0;
  };
  enum class SubmitResult : uint8_t { kNotReady, kSubmitted, kFailed };

  virtual ~Backend() = default;

  // Device, window or swapchain, and the modules. Logs and returns false on failure.
  virtual bool Initialize(const Options& options) = 0;
  // The frame pacer (renderer.h) that presents attach to, and the flag the backend sets while
  // the frames are paced on the display. Primary mode with a pacer only.
  virtual void SetPacer(FramePacer* pacer, std::atomic<bool>* paces_frames) = 0;

  // Guest thread (Prepare group; see draw.h's DrawRenderer::Prepare).
  virtual void BeginCopySegment() = 0;
  virtual void Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                       std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) = 0;
  virtual void StartAudit(uint32_t frames) = 0;
  virtual PrepareCounters EndFramePrepare(uint64_t done) = 0;
  // Captures (present.h).
  virtual bool CaptureRequested(uint64_t frame) = 0;
  virtual bool capture_holds_guest() const = 0;
  virtual bool capture_holding_guest() const = 0;

  // Recording thread.
  virtual void AddPrepareCounters(const PrepareCounters& c) = 0;
  virtual void LogFrames(uint32_t frames) = 0;
  // The frame's first recorded call: per-frame module state.
  virtual void BeginFrame(uint64_t frame_index) = 0;
  virtual void Draw(const DrawState& state, const DrawPrep* prep) = 0;
  virtual void Clear(const ClearCall& clear) = 0;
  virtual void Resolve(const ResolveParams& params) = 0;
  // Submits the frame so far (--graw_native_submit_draws): not ready while texture uploads run.
  virtual SubmitResult SubmitEarly() = 0;
  // The guest Swap: the front buffer, the submission and the present. `own_window` in primary
  // mode: whether the renderer presents into the game window this frame (else ReXGlue's
  // presenter does); nullopt in the debug window mode.
  virtual SwapResult Swap(const SwapCall& swap, std::optional<bool> own_window) = 0;
  // After a Swap's bookkeeping: the frames up to `completed` are done.
  virtual void AfterSwap(uint64_t frame_index, uint64_t completed) = 0;
  // A failed frame: gives the game window back to ReXGlue's presenter.
  virtual void ReleaseGameWindow() = 0;

  // Statistics (recording thread, at the Swap).
  virtual const DrawStats& draw_stats() const = 0;
  virtual void ResetDrawStats() = 0;
  virtual const TextureStats& texture_stats() const = 0;
  virtual uint64_t texture_resident_bytes() const = 0;
  virtual uint64_t texture_count() const = 0;
  virtual bool front_buffer_found() const = 0;
  virtual const LumaProbe& luma_probe() const = 0;
  // --graw_native_gpu_timing: the previous frame's GPU time (0: off), and its periodic log.
  virtual double gpu_last_frame_ms() const = 0;
  virtual void LogGpuTiming() = 0;
};

}  // namespace graw::native
