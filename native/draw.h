// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): one guest draw -> one host D3D12 draw.
//
// Per draw (DrawRenderer::OnDraw, after ReadDrawState):
//   - shaders: ShaderCache::Load of the bound VS/PS ucode, SelectModifications (the Xenos
//     plugin's variant selection), translations with the DXBC translator (host RT path);
//   - pipeline: blend, depth/stencil, rasterizer, primitive topology, RT/DSV formats from the
//     register file, the geometry shader for point/rectangle/quad lists; PSOs cached by a hash
//     of that description (ports of d3d12::PipelineCache::GetCurrentStateDescription and
//     CreateD3D12Pipeline, BSD);
//   - vertices: the translator reads them in the vertex shader from "shared memory", a GPU
//     buffer mirroring guest physical memory (t0). Ranges named by the vertex fetch constants
//     the VS uses, and the index buffer, are uploaded at their guest offsets; uploaded pages
//     stay valid until a CPU write hits them (physical memory access watches, like the
//     plugin's shared memory), except hot pages, which the game rewrites every few frames:
//     those are not watched but hashed on each request, and copied when their content changed.
//     The inline vertices of BeginVertices/DrawVerticesUP live in the push buffer and are
//     uploaded on every draw, never watched;
//   - indices: DMA index buffers are bound straight from shared memory (big-endian, the VS
//     swaps them by SystemConstants::vertex_index_endian); auto-indexed draws are non-indexed;
//   - constants: the translator's b0 system constants (port of
//     D3D12CommandProcessor::UpdateSystemConstantValues, host RT path), b1 the VS/PS float
//     constants packed by the shaders' constant_register_map, b2 bool/loop, b3 fetch, b4
//     bindless descriptor indices; uploaded to per-frame ring buffers, reused while unchanged;
//   - textures from TextureCache (TextureMod), samplers from the sampler bindings (port of
//     D3D12TextureCache::GetSamplerParameters/WriteSampler), bindless heaps;
//   - render targets from RenderTargets::Bind (RTResolve); viewport, scissor, blend factor
//     and stencil reference (port of draw_util::GetHostViewportInfo / GetScissor).
//
// Not handled (counted in Stats::unsupported and dropped): memexport, tessellation, line loops
// and other primitive types needing converted indices, and primitive reset with a 16-bit index
// other than 0xFFFF.
//
// Threading: the renderer's On* calls only (one guest thread). With the render thread
// (renderer.h, --graw_native_render_thread) the work is split: Prepare and EndFramePrepare run
// on the guest thread at the draw's call (shader lookup, vertex/index copies out of guest
// memory, which the game may rewrite right after the draw); everything else runs on the
// render thread, later, from the DrawPrep the hook side made.
#pragma once

#include "backend.h"
#include "d3d12_device.h"
#include "d3d_state.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace graw::native {

class D3D12Backend;

// DrawPrep::vs/ps hold rex::graphics::DxbcShader objects here (shader_cache.h).
class DrawRenderer {
 public:
  // backend.h: the types the renderer host queues and reports.
  using Stats = DrawStats;
  using PrepareDiag = graw::native::PrepareDiag;
  using PrepareCounters = graw::native::PrepareCounters;

  explicit DrawRenderer(D3D12Backend& renderer);
  ~DrawRenderer();
  DrawRenderer(const DrawRenderer&) = delete;
  DrawRenderer& operator=(const DrawRenderer&) = delete;

  // Root signature, descriptor heaps, shared memory buffer, the memory watch. Logs and
  // returns false on failure.
  bool Initialize();

  // After a successful ReadDrawState for the draw. Records into renderer.command_list().
  // `prep` (render thread): what Prepare made for this draw on the guest thread.
  void OnDraw(const DrawState& state, const DrawPrep* prep = nullptr);

  // Guest thread, render thread mode: shader lookup and vertex/index copies for `state` (whose
  // regs are the tracker's, current), into a default-constructed `prep`. Copies go to the end
  // of `copies`; upload memory is tagged with `upload_tag` (EndFramePrepare's units).
  //
  // Copy segments: the render thread records all copies of a segment ahead of the segment's
  // first draw that needs them, so a segment costs one pair of shared-memory barriers instead
  // of one pair per uploading draw. A draw starts a new segment (prep->copy_segment) when one
  // of its copies would overwrite a page an earlier draw of the segment reads: that draw must
  // still see the old bytes. BeginCopySegment starts one unconditionally (a new batch).
  //
  // While a mark audit runs (StartAudit), Prepare also checks the loaded shaders against the
  // whole guest ucode, hashes the register blocks for Draw to compare, and appends the ranges
  // the draw reads to `audit` (each range once per frame).
  void Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
               std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit);
  void BeginCopySegment();
  // Guest thread, at a marked Swap: audit the draws of the next `frames` frames. The render
  // thread logs those frames (LogFrames), reads back the shared memory each audited draw reads
  // and logs every range that differs from guest memory as the draw was prepared.
  void StartAudit(uint32_t frames);
  // Render thread, at a Swap: log the next `frames` frames' draws (<capture
  // dir>/draws-f<frame>.txt).
  void LogFrames(uint32_t frames);
  // Guest thread, once per Swap: recycles vertex upload memory with tags <= `done`. Returns
  // Prepare's counters since the previous call.
  PrepareCounters EndFramePrepare(uint64_t done);
  // Render thread, at the frame's Swap.
  void AddPrepareCounters(const PrepareCounters& c);
  // Render thread, the last thing recorded into a frame's command list.
  void BeforeSubmit();

  // Why the next draw must set everything on the command list again. A new frame's list is
  // detected without InvalidateListState.
  enum class ListBreak : uint8_t {
    kResolve,  // the Resolver set its own state and descriptor heaps
    kSubmit,   // the list was submitted and reset mid-frame
  };
  void InvalidateListState(ListBreak why = ListBreak::kResolve);
  // Write the clear or resolve into the draw log while one is open (draw-debug.txt bit 16).
  void LogClear(const ClearCall& c);
  void LogResolve(const ResolveParams& p);

  // At Swap, before EndFrame: the frame skipped draws in a burst of new pipelines (a scene's
  // first frame, before its draws began waiting for them), so it is not worth showing.
  bool FrameIncomplete() const;
  // Per frame, with the fence values of D3D12Backend (once per Swap, after EndFrame).
  void EndFrame(uint64_t completed_fence);

  const Stats& stats() const { return stats_; }
  void ResetStats() { stats_ = Stats{}; }

  // Ucode hashes of a prepared draw's (vertex, pixel) shaders, 0 where there is none.
  static std::pair<uint64_t, uint64_t> ShaderHashes(const DrawPrep& prep);
  // The ucode disassembly of a loaded shader; empty when no loaded shader has the hash.
  std::string ShaderDisassembly(uint64_t hash) const;

 private:
  struct Impl;
  D3D12Backend& renderer_;
  Stats stats_;  // before impl_, which keeps a reference to it
  std::unique_ptr<Impl> impl_;
};

}  // namespace graw::native
