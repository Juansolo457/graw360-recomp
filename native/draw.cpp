// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): guest draws -> host D3D12 draws. See draw.h.
//
// Ports (BSD-3, Xenia / ReXGlue): d3d12::PipelineCache::GetCurrentStateDescription and
// CreateD3D12Pipeline (pipeline state), D3D12CommandProcessor::IssueDraw,
// UpdateFixedFunctionState, UpdateSystemConstantValues and UpdateBindings (host render target
// path, bindless), draw_util::GetHostViewportInfo / GetScissor / GetNormalizedDepthControl /
// GetNormalizedColorMask / IsRasterizationPotentiallyDone / GetPreferredFacePolygonOffset,
// D3D12TextureCache::GetSamplerParameters / WriteSampler, and the DMA index path of
// PrimitiveProcessor::Process. Copyright Ben Vanik and Xenia developers; adapted by Tom Clay
// for ReXGlue; adapted here for the native renderer.
#include "draw.h"

#include "geometry_shader.h"
#include "guest_draw_state.h"
#include "guest_memory_mirror.h"
#include "log.h"
#include "pipeline_library.h"
#include "pipeline_store.h"
#include "profile.h"
#include "render_targets.h"
#include "d3d12_backend.h"
#include "renderer.h"
#include "resolve.h"
#include "shader_cache.h"
#include "shader_specialize.h"
#include "shader_translate.h"
#include "textures.h"
#include "upload_pool.h"

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <xxhash.h>
#include <simde/x86/sse2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <bit>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DECLARE(bool, graw_native_async_pipelines);
REXCVAR_DECLARE(int32_t, graw_native_pipeline_threads);
REXCVAR_DECLARE(bool, graw_native_pipeline_wait);
REXCVAR_DECLARE(bool, graw_native_pipeline_cache);
REXCVAR_DECLARE(std::string, graw_native_watch_trace);
REXCVAR_DECLARE(bool, graw_native_coherency_check);
REXCVAR_DECLARE(bool, graw_native_implicit_gradients);
REXCVAR_DEFINE_BOOL(graw_native_generic_pipelines, true, "GRAW native",
                    "With --graw_native_async_pipelines, a draw whose bool or texture-sign "
                    "specialization is still compiling uses the pipeline of its unspecialized "
                    "shaders when that one is ready (false: it waits for the specialization)");
REXCVAR_DEFINE_BOOL(graw_native_dynamic_depth_bias, true, "GRAW native",
                    "Set the polygon offset with RSSetDepthBias instead of baking it into the "
                    "pipeline state, where the driver supports it (the game animates it, which "
                    "made hundreds of pipelines per shader pair)");
REXCVAR_DEFINE_BOOL(graw_native_bool_specialize, true, "GRAW native",
                    "Translate shaders whose control flow tests bool constants for each draw's "
                    "values, without the branches (shader_specialize.h)");
REXCVAR_DEFINE_INT32(graw_native_draw_log_from, -1, "GRAW native",
                     "Debug: from this native frame on, log the draws of each frame to "
                     "<capture dir>/draws-f<frame>.txt, as bit 16 of <capture dir>/draw-debug.txt "
                     "does from when it is set (-1: off)");
REXCVAR_DEFINE_INT32(graw_native_draw_log_frames, 1, "GRAW native",
                     "Frames one draw log trigger (--graw_native_draw_log_from or draw-debug.txt "
                     "bit 16) covers");
REXCVAR_DEFINE_INT32(graw_native_anisotropy, 1, "GRAW native",
                     "Anisotropic filtering of the game's linearly filtered textures with mips, "
                     "at least this many samples per pixel (2, 4, 8 or 16; 1: the game's own)");
REXCVAR_DECLARE(std::string, graw_native_capture_dir);
namespace rex::graphics {
extern bool g_dxbc_implicit_gradients;  // dxbc_translator_fetch.cpp (local SDK experiment)
}

namespace graw::native {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::draw_util::Scissor;
using rex::graphics::draw_util::ViewportInfo;
namespace texture_util = rex::graphics::texture_util;

namespace {

constexpr uint32_t kSharedMemorySize = 512u << 20;  // guest physical address space
// Shared memory is a reserved buffer whose 4 MB chunks get memory when a range in them is first
// requested (the Xenos plugin's shared memory does the same): a committed 512 MB buffer took half
// of a Steam Deck's 1 GB of VRAM, of which a race touches a fraction.
constexpr uint32_t kSharedChunkShift = 22;
constexpr uint32_t kSharedChunkCount = kSharedMemorySize >> kSharedChunkShift;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageCount = kSharedMemorySize >> kPageShift;
// The memory unwatches at most the 4 MB-aligned block around a write fault
// (PhysicalHeap::TriggerCallbacks), so the invalidation callback looks no further.
constexpr uint32_t kUnwatchWindow = 4u << 20;
// The Xenos plugin's invalidation callbacks let the memory unwatch at most the 256 KB-aligned
// block around a fault: the pages of one 64-bit word of the valid bits.
constexpr uint32_t kPluginUnwatchBlock = 256u << 10;
static_assert(kPluginUnwatchBlock == 64u << kPageShift);
constexpr uint32_t kViewHeapSize = 65536;
constexpr uint32_t kSamplerHeapSize = 2048;
// View heap: 0 shared memory SRV, 1 shared memory UAV, then the bindless table (descriptor
// indices in b4 count from here): 0 null 2D array, 1 null 3D, 2 null cube, then textures.
constexpr uint32_t kViewTableStart = 2;
constexpr uint32_t kNull2DArray = 0, kNull3D = 1, kNullCube = 2, kFirstTexture = 3;
// The null view standing in for a view of dimension `dim`.
constexpr uint32_t NullView(D3D12_SRV_DIMENSION dim) {
  return dim == D3D12_SRV_DIMENSION_TEXTURECUBE ? kNullCube
         : dim == D3D12_SRV_DIMENSION_TEXTURE3D ? kNull3D
                                                : kNull2DArray;
}
// ... and for a texture binding of fetch dimension `dim`.
constexpr uint32_t NullView(xenos::FetchOpDimension dim) {
  return dim == xenos::FetchOpDimension::kCube          ? kNullCube
         : dim == xenos::FetchOpDimension::k3DOrStacked ? kNull3D
                                                        : kNull2DArray;
}
constexpr D3D12_RESOURCE_STATES kSharedReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_INDEX_BUFFER;

using enum rex::graphics::Register;  // XE_GPU_REG_* (clang-cl, C++20)

// ---- pipeline description ----

struct PipelineKey {
  const void* vs;  // translation objects (stable for the cache's lifetime)
  const void* ps;
  uint32_t gs_key;
  uint8_t topology_type;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE
  uint8_t strip_cut;      // 0 none, 1 0xFFFF, 2 0xFFFFFFFF
  uint8_t cull;           // D3D12_CULL_MODE
  uint8_t wireframe;
  uint8_t front_ccw;
  uint8_t depth_clip;
  uint8_t samples;
  uint8_t disable_raster;
  int32_t depth_bias;
  float slope_scaled_depth_bias;
  uint8_t depth_enable, depth_write, depth_func, stencil_enable;
  uint8_t stencil_read_mask, stencil_write_mask;
  uint8_t front[4], back[4];  // fail, depth fail, pass, func (D3D12 values)
  uint8_t num_rts, pad[5];
  DXGI_FORMAT dsv;
  DXGI_FORMAT rtv[4];
  struct Rt {
    uint8_t blend, src, dst, op, src_a, dst_a, op_a, mask;
  } rt[4];
  uint8_t pad_end[4];
};
// No implicit padding: keys are hashed and stored as bytes, and a copy need not carry padding.
static_assert(offsetof(PipelineKey, depth_bias) == 28 && offsetof(PipelineKey, dsv) == 56 &&
              offsetof(PipelineKey, rt) == 76 && sizeof(PipelineKey) == 112);
// Bump when PipelineKey's layout or the meaning of a field changes: keys are stored on disk
// (PipelineStore) and an old file is then discarded.
// 2: depth bias zero with dynamic depth bias; 3: pixel shaders specialized for unsigned textures
constexpr uint32_t kPipelineKeyVersion = 3;

constexpr D3D12_BLEND kD3DBlend[] = {
    D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
    D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
    D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
    D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
    D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
    D3D12_BLEND_SRC_ALPHA_SAT,
};
constexpr D3D12_BLEND_OP kD3DBlendOp[8] = {
    D3D12_BLEND_OP_ADD,          D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX,
    D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,      D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};

// ---- samplers ----

D3D12_SAMPLER_DESC SamplerDesc(const SamplerKey& k) {
  D3D12_SAMPLER_DESC d = {};
  if (xenos::AnisoFilter(k.aniso) != xenos::AnisoFilter::kDisabled) {
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << (uint32_t(k.aniso) - 1);
  } else {
    d.Filter =
        D3D12_ENCODE_BASIC_FILTER(k.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                  k.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                  k.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                  D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    d.MaxAnisotropy = 1;
  }
  static const D3D12_TEXTURE_ADDRESS_MODE kAddress[] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP,   D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
  };
  d.AddressU = kAddress[k.clamp_x];
  d.AddressV = kAddress[k.clamp_y];
  d.AddressW = kAddress[k.clamp_z];
  d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (xenos::BorderColor(k.border)) {
    case xenos::BorderColor::k_ABGR_White:
      d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      d.BorderColor[0] = 0.5f;
      d.BorderColor[2] = 0.5f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      d.BorderColor[1] = 0.5f;
      d.BorderColor[2] = 0.5f;
      break;
    default:
      break;
  }
  d.MinLOD = float(k.mip_min_level);
  if (k.mip_base_map) {
    d.MaxLOD = d.MinLOD;
    if (xenos::AnisoFilter(k.aniso) == xenos::AnisoFilter::kDisabled) d.MaxLOD += 0.25f;
  } else {
    d.MaxLOD = FLT_MAX;
  }
  return d;
}

// A constant buffer that is re-uploaded only when its bytes change within a frame. `last`
// holds the bytes of the current upload (the largest CBV is 256 float4 = 4 KB): new data is
// compared with it, and an upload streams it into upload memory that the CPU never reads back.
// gpu == 0: nothing current uploaded.
struct CachedCbv {
  static constexpr uint32_t kMaxSize = 256 * 16;
  alignas(16) uint8_t last[kMaxSize];
  uint32_t size = 0;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  D3D12_GPU_VIRTUAL_ADDRESS bound = 0;  // list state (ResetListState)
  // Forgets the upload, whose memory may be recycled: the next commit uploads `last` again.
  void Reset() {
    size = 0;
    gpu = 0;
  }
};

// Float constants packed by a shader's constant_register_map (UpdateBindings) straight into
// `cb.last`, compared with what was there on the way (one register = 16 bytes = one SSE
// load/store, no calls); 16 zero bytes for no shader or no constants (a CBV needs some data).
void PackFloats(CachedCbv& cb, const DxbcShader* shader, const uint32_t* regs, uint32_t reg_base) {
  const Shader::ConstantRegisterMap* map = shader ? &shader->constant_register_map() : nullptr;
  const uint32_t n = map ? map->float_count : 0;  // the bitmap's population count
  if (!n) {
    static constexpr uint8_t kZero[16] = {};
    if (cb.size != 16 || !Equal(cb.last, kZero, 16)) {
      std::memset(cb.last, 0, 16);
      cb.size = 16;
      cb.gpu = 0;
    }
    return;
  }
  const uint32_t size = n * 16;
  const bool resized = cb.size != size;
  cb.size = size;
  simde__m128i* out = reinterpret_cast<simde__m128i*>(cb.last);
  simde__m128i diff = simde_mm_setzero_si128();
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = map->float_bitmap[i];
    const uint32_t* base = regs + reg_base + (i << 8);
    while (bits) {
      const uint32_t b = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const simde__m128i v =
          simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(base + (b << 2)));
      diff = simde_mm_or_si128(diff, simde_mm_xor_si128(v, simde_mm_load_si128(out)));
      simde_mm_store_si128(out++, v);
    }
  }
  if (resized ||
      simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(diff, simde_mm_setzero_si128())) != 0xFFFF)
    cb.gpu = 0;
}

}  // namespace

struct DrawRenderer::Impl : GuestMemoryMirror::Sink {
  D3D12Backend& r;
  DrawRenderer::Stats& stats;
  ID3D12Device* device = nullptr;
  rex::memory::Memory* memory = nullptr;
  const uint8_t* membase = nullptr;
  ShaderCache shaders;
  BoolSpecializer vs_bools{shaders}, ps_bools{shaders};
  bool bool_specialize = REXCVAR_GET(graw_native_bool_specialize);
  ComPtr<ID3D12RootSignature> root_signature;
  uint64_t root_signature_hash = 0;  // names in the pipeline library depend on it

  // Guest physical memory in shared memory: which pages hold a current copy, the write watches
  // and the range copies (guest_memory_mirror.h). The D3D12 side (the buffer, its chunk
  // mapping, the vertex upload pool) is this Impl, as the mirror's Sink.
  GuestMemoryMirror mirror;
  // Shared memory: guest physical memory mirror (pages are current per `mirror`).
  ComPtr<ID3D12Resource> shared;
  // Its state in the list being recorded. Buffers are created in the common state and decay to
  // it when ExecuteCommandLists completes, so every new list starts from there (SyncListFrame,
  // and InvalidateListState after a submission).
  D3D12_RESOURCE_STATES shared_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_GPU_VIRTUAL_ADDRESS shared_gpu = 0;
  // Reserved shared buffer: which chunks have memory (set under shared_map_mutex, read without
  // it), and the tile heaps behind them. False: a committed buffer.
  bool shared_tiled = false;
  std::atomic<uint64_t> shared_mapped[kSharedChunkCount / 64] = {};
  std::mutex shared_map_mutex;
  std::vector<ComPtr<ID3D12Heap>> shared_heaps;
  uint32_t shared_mapped_chunks = 0;
  // Without the render thread: the copies of the draw being recorded. With it, Prepare
  // (guest thread) writes into the batch's vector instead: this one is the render thread's.
  std::vector<VertexCopy> copies;
  // Render thread mode: vertex_upload belongs to the guest thread (Prepare/EndFramePrepare).
  bool threaded = false;
  uint64_t prep_upload_bytes = 0, prep_uploads = 0;
  PrepareDiag prep_diag;  // the guest thread's, until EndFramePrepare hands it over
  UploadPool vertex_upload{16u << 20};
  // Write-back pages, written with streaming stores (CommitCbv).
  UploadPool constant_upload{4u << 20, /*cpu_cached=*/true};

  // Descriptors.
  ComPtr<ID3D12DescriptorHeap> view_heap, sampler_heap;
  uint32_t view_stride = 0, sampler_stride = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE view_cpu{}, sampler_cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE view_gpu{}, sampler_gpu{};
  uint32_t views_allocated = kViewTableStart + kFirstTexture;  // heap indices handed out so far
  std::vector<uint32_t> free_views;  // heap indices of evicted textures' views
  uint32_t samplers_allocated = 0;
  // Each texture resource's views: (XXH3 of the SRV desc, table index). The views of a texture
  // the cache evicts go back to free_views (ReleaseViews).
  std::unordered_map<ID3D12Resource*, std::vector<std::pair<uint64_t, uint32_t>>> srv_views;
  std::unordered_map<uint32_t, uint32_t> sampler_indices;  // SamplerKey::value()
  // Direct-mapped memo in front of srv_views: (resource, component mapping, signed, fetch
  // dimension) -> descriptor index; a hit skips TextureDescriptor's desc build, XXH3 and map
  // lookup. Every other field of the SRV desc (format, view dimension, mips, array size) is fixed
  // per resource, so the key determines the index while the resource lives; ReleaseViews drops
  // the entries of evicted ones, whose addresses new resources may reuse.
  struct SrvMemo {
    ID3D12Resource* resource = nullptr;
    uint32_t mapping = 0;
    uint8_t is_signed = 0, dimension = 0xFF;
    uint32_t index = 0;
  };
  static constexpr uint32_t kSrvMemoBits = 12;
  std::unique_ptr<SrvMemo[]> srv_memo = std::make_unique<SrvMemo[]>(size_t(1) << kSrvMemoBits);
  // Direct-mapped memo in front of GetSamplerKey + sampler_indices: (6 fetch dwords, binding
  // mag/min/mip/aniso) -> sampler index; the sampler key is a function of exactly those.
  struct SamplerMemo {
    uint32_t fetch[6] = {};
    uint8_t filters[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t index = 0;
  };
  static constexpr uint32_t kSamplerMemoBits = 10;
  std::unique_ptr<SamplerMemo[]> sampler_memo =
      std::make_unique<SamplerMemo[]>(size_t(1) << kSamplerMemoBits);
  xenos::AnisoFilter min_aniso = xenos::AnisoFilter::kDisabled;  // --graw_native_anisotropy
  // Fetch constants a shader reads (texture and sampler bindings), its descriptor table size and
  // its last BindMemo entry (checked before hashing), direct-mapped by shader.
  struct BindMemo;
  struct ShaderSlots {
    const DxbcShader* shader = nullptr;
    uint32_t mask = 0;   // fetch constants read
    uint32_t count = 0;  // bits in mask
    uint32_t n = 0;      // descriptor table entries (max bindless index + 1, at most 64)
    BindMemo* last = nullptr;
    // Takes the slot for `shader`. Cold and out of line: its loops vectorize with 256-bit
    // loads, and inlined they would make Draw run VZEROUPPER before its calls (build.sh).
    [[gnu::cold, gnu::noinline]] void Assign(const DxbcShader& shader);
  };
  ShaderSlots shader_slots[512];
  // Whole-shader binding memo, 2-way set-associative by (shader, the fetch constants it reads):
  // the descriptor table the shader's textures and samplers resolved to and the texture signs it
  // set. Valid while every texture it bound is unchanged (TextureCache::Unchanged: no guest
  // write, no resolve into it); `epoch` is the TextureCache::change_epoch at which that was last
  // known, so while the epoch stays the same the per-texture checks are skipped. A hit skips
  // TextureCache::Get and the descriptor memos for the whole shader. Keying on the fetch
  // constants keeps one entry per texture set a shader is drawn with (one entry per shader missed
  // ~900 times per frame on the race). Shaders with a missing texture are not memoized.
  struct BindMemo {
    static constexpr uint32_t kMaxSlots = 16, kMaxBound = 32, kMaxIndices = 64;
    // Fields a hit reads, packed so a hit reads one contiguous run (entry[] only when the epoch
    // moved, or at its first hit in a frame).
    const DxbcShader* shader = nullptr;
    uint64_t epoch = 0;
    uint64_t used = 0;  // bind_clock at the last fill or set lookup (the set's replacement order)
    uint64_t touched = 0;  // the frame fence its textures were last marked used with
    uint32_t bound = 0;    // textures bound, in order: their cache entries and signs[] writes
    uint32_t textures = 0;
    // The shader's fetch constants in ShaderSlots::mask order (count x 6 dwords), then its
    // descriptor table (n), then one signs[] write per bound texture (slot | value << 8).
    uint32_t data[kMaxSlots * 6 + kMaxIndices + kMaxBound];
    TextureHandle entry[kMaxBound];
  };
  static constexpr uint32_t kBindMemoSetBits = 11;
  std::unique_ptr<BindMemo[]> bind_memo =
      std::make_unique<BindMemo[]>(size_t(2) << kBindMemoSetBits);
  uint64_t bind_clock = 0;

  // Pipelines by PipelineKey hash. With --graw_native_async_pipelines new ones compile on
  // pso_threads and the draws that need them are skipped until they are ready
  // (stats.pso_pending). With pipeline_wait such a frame is not shown (FrameIncomplete): the
  // previous frame stays on screen while this one waits at its end for every pipeline its
  // draws skipped (WaitForPipelines, at most kPipelineWaitBudget per frame), compiled meanwhile
  // on all but two hardware threads, so no frame is shown with draws missing. A scene's first
  // frame needs many at once (180 at race start); a frame mid-race seldom needs more than one.
  // Waiting at each draw instead compiled one pipeline at a time, as the next draw requested
  // its own only once the previous one's was ready. Entries are never removed.
  // With --graw_native_pipeline_cache every new pipeline is recorded in `store`; the next run
  // translates and creates the recorded ones at start-up (Prewarm, a slice per frame) and the
  // workers load them from `library` instead of compiling them again.
  enum : uint8_t { kCompiling, kReady, kFailed };
  struct Pipeline {
    ComPtr<ID3D12PipelineState> pso;
    std::atomic<uint8_t> state{kCompiling};
    bool prewarm = false;  // its job was queued in prewarm_jobs (draw thread only)
    // A specialization whose draws use their unspecialized shaders' pipeline until it is ready
    // (draw thread only).
    bool behind_generic = false;
    uint64_t pending_frame = ~0ull;  // last frame it was added to frame_pending (draw thread)
  };
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines;
  struct PipelineJob {
    Pipeline* target;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;  // bytecode and root signature outlive the job
    bool logged = false;                      // a draw needed it new: its compile time is logged
  };
  std::vector<std::thread> pso_threads;
  std::mutex pso_mutex;
  std::condition_variable pso_cv;        // background workers: a job was queued
  std::condition_variable pso_wait_cv;   // wait workers: a frame began waiting
  std::condition_variable pso_ready_cv;  // a job finished (notified under pso_mutex)
  bool pso_waiting = false;              // WaitForPipelines is waiting (under pso_mutex)
  std::deque<PipelineJob> pso_jobs;      // a draw is waiting for these
  std::deque<PipelineJob> prewarm_jobs;  // taken only when pso_jobs is empty
  bool pso_stopping = false;
  static constexpr std::chrono::milliseconds kPipelineWaitBudget{500};
  bool pipeline_wait = false;
  bool generic_pipelines = false;        // --graw_native_generic_pipelines (with worker threads)
  std::vector<Pipeline*> frame_pending;  // pipelines of the frame's skipped draws, once each
  PipelineStore store;
  // What made the pipelines created while running new (logged, kNewPipelineLog lines): a
  // shader pair or a render state no pipeline had before, bool or texture-sign variants.
  // Counted only while those lines are logged: nothing reads them afterwards.
  std::unordered_map<uint64_t, uint32_t> pipeline_states, pipeline_pairs;  // digest -> pipelines
  uint32_t new_pipelines_logged = 0;
  static constexpr uint32_t kNewPipelineLog = 64;
  static uint64_t StateDigest(const PipelineKey& key) {
    PipelineKey k = key;
    k.vs = k.ps = nullptr;
    return XXH3_64bits(&k, sizeof(k));
  }
  static uint64_t PairDigest(const PipelineKey& key) {
    const void* pair[2] = {key.vs, key.ps};
    return XXH3_64bits(pair, sizeof(pair));
  }
  std::once_flag library_open;
  PipelineLibrary library;  // pipeline workers only
  // D3D12 OPTIONS16 dynamic depth bias: pipelines are created with
  // D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS and keyed with a zero bias, and draws set it
  // with RSSetDepthBias.
  bool dynamic_depth_bias = false;
  ComPtr<ID3D12GraphicsCommandList9> list9;
  ID3D12GraphicsCommandList* list9_of = nullptr;
  std::filesystem::path library_path;
  size_t prewarm_next = 0;  // into store.loaded_pipelines()
  uint32_t prewarm_failed = 0, prewarm_frames = 0;
  double prewarm_ms = 0;
  std::unordered_map<uint32_t, std::vector<uint32_t>> geometry_shaders;

  // Shader lookup memo, direct-mapped by ucode address and size (plus the immediate load
  // sequence and the vertex shader variant's binding): a hit skips ShaderCache::Load's XXH3
  // over the whole ucode. An entry also keeps the first and last dwords of the ucode it was
  // made from, so a different shader loaded at the same address with the same size misses.
  // A race binds a few hundred shaders a frame.
  struct ShaderMemo {
    static constexpr uint32_t kBits = 8, kEntries = 1u << kBits, kSample = 16;
    static uint32_t Slot(const ShaderRef& ref) {
      return ((ref.ucode ^ (ref.dwords << 16)) * 0x9E3779B1u) >> (32 - kBits);  // Fibonacci
    }
    uint32_t ucode = 0, dwords = 0, immediate_seq = 0;
    uint64_t binding = 0;
    uint32_t sample[kSample] = {};
    DxbcShader* shader = nullptr;
  };
  ShaderMemo vs_memo[ShaderMemo::kEntries], ps_memo[ShaderMemo::kEntries];
  ShaderMemo* vs_last = nullptr;  // the entry the previous draw used
  ShaderMemo* ps_last = nullptr;
  // SelectModifications is a function of the two shaders and these registers: the last call.
  struct ModsMemo {
    const DxbcShader* vs = nullptr;
    const DxbcShader* ps = nullptr;
    ShaderDrawRegs sdr;
    ShaderModifications mods;
    uint32_t ps_textures = 0;  // fetch constants the pixel shader reads (bit per constant)
  } mods_memo;
  const uint64_t signed_is_unsigned_formats = SignedViewIsUnsignedFormats();
  // Translation per shader, direct-mapped by shader: the last modification it was translated
  // with (valid translations only; they live as long as the shader cache).
  struct TranslationMemo {
    const DxbcShader* shader = nullptr;
    uint64_t modification = 0;
    DxbcShader::Translation* translation = nullptr;
  };
  TranslationMemo translation_memo[256];
  // Ready pipelines by PipelineKey hash, direct-mapped, in front of GetPipeline's map (ready
  // pipelines are never replaced or freed).
  struct PsoMemo {
    uint64_t hash = 0;
    ID3D12PipelineState* pso = nullptr;
  };
  PsoMemo pso_memo[1024];

  // Draw path counters, logged every 300 frames with --graw_native_profile.
  struct Diag {
    uint64_t frames = 0, draws = 0, mods_hits = 0, translation_hits = 0, pso_hits = 0, flushes = 0;
    PrepareDiag prep;  // with the render thread, added at each Swap (AddPrepareCounters)
    uint64_t cbv_uploads[16] = {}, cbv_bytes[16] = {};  // by root parameter
    // Textures bound, by the fetch constant state the shaders read at run time: swizzled signs
    // all unsigned, gamma RGB + unsigned A, all gamma, all signed, other; then exponent bias
    // set, LOD bias set (any signs).
    uint64_t fetch_classes[7] = {};
  } diag;
  // Where Prepare's counters go: with the render thread, which owns diag, the guest thread's own.
  PrepareDiag& PrepDiag() { return threaded ? prep_diag : diag.prep; }

  // List state.
  uint64_t list_frame = UINT64_MAX;
  bool list_dirty = true;
  ID3D12PipelineState* bound_pso = nullptr;
  D3D_PRIMITIVE_TOPOLOGY bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  D3D12_VIEWPORT bound_viewport{};
  D3D12_RECT bound_scissor{};
  float bound_blend[4] = {};
  uint32_t bound_stencil_ref = UINT32_MAX;
  float bound_depth_bias = NAN, bound_slope_bias = NAN;
  D3D12_CPU_DESCRIPTOR_HANDLE bound_rtv[4] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE bound_dsv{};
  uint32_t bound_rt_count = UINT32_MAX;
  D3D12_INDEX_BUFFER_VIEW bound_ibv{};
  CachedCbv cb_system, cb_float_vs, cb_float_ps, cb_bool_loop, cb_fetch, cb_desc_vs, cb_desc_ps;
  std::array<CachedCbv*, 7> Cbvs() {
    return {&cb_system, &cb_float_vs, &cb_float_ps, &cb_bool_loop,
            &cb_fetch,  &cb_desc_vs,  &cb_desc_ps};
  }

  // Oracle toggles from <capture dir>\draw-debug.txt (with --graw_native_capture_dir)
  // ("<mask> [max_draws] [skip_from] [skip_to] [shader hash]..."), read every 15 frames.
  // max_draws > 0 issues only the first N draws of a frame; draws whose vertex or pixel shader
  // ucode hash is listed are skipped (selected with kDebugOnlyListed). Hashes stay stable across
  // frames where draw indices do not.
  enum DebugBit : uint32_t {
    kDebugNoDepth = 1,  // depth and stencil off
    kDebugNoCull = 2,
    kDebugNoBlend = 4,
    kDebugNoAlphaTest = 8,
    kDebugLogDraws = 16,  // log the next frames' draws
    kDebugUploadEveryDraw = 32,
    kDebugNoDepthBias = 64,
    kDebugOnlyListed = 128,  // issue only the draws the hashes select
    kDebugSnapshots = 256,   // snap_pending (below)
  };
  uint32_t debug_mask = 0, debug_max_draws = 0, frame_draws = 0, debug_poll = 0;
  uint32_t debug_skip_from = 0, debug_skip_to = 0;
  std::string debug_path;  // empty without --graw_native_capture_dir: not polled
  std::array<uint64_t, 8> debug_hashes{};
  uint32_t debug_hash_count = 0;
  uint32_t draw_log_left = 0;  // frames still to log
  std::FILE* draw_log = nullptr;
  // 256: snapshots instead of skips. After the first draw of the next frame that uses each
  // listed hash (or, with none listed, after draw skip_from), the draw's bound single-sampled
  // targets are read back to <capture dir>\snap-f<frame>-d<draw>-{depth,c<i>}.bin, each with a
  // .txt holding "width height row_bytes dxgi_format".
  struct Snap {
    std::string stem;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    uint32_t rows = 0;
    uint64_t row_bytes = 0;
  };
  std::vector<Snap> snaps;
  uint64_t snap_fence = 0;
  uint32_t snap_pending = 0;  // bit i: listed hash i (bit 31: draw skip_from) still to snapshot
  void SnapshotTargets(ID3D12GraphicsCommandList* list, const BoundTargets& targets,
                       uint32_t draw_index);
  void Snapshot(ID3D12GraphicsCommandList* list, RenderTarget* rt, const std::string& stem);
  void WriteSnapshots(uint64_t completed_fence);

  Impl(D3D12Backend& renderer, DrawRenderer::Stats& s)
      : r(renderer), stats(s), shaders(renderer.device().vendor_id()) {}
  ~Impl() {
    if (!pso_threads.empty()) {
      {
        std::lock_guard<std::mutex> lock(pso_mutex);
        pso_stopping = true;
      }
      pso_cv.notify_all();
      pso_wait_cv.notify_all();
      for (std::thread& t : pso_threads) t.join();
      library.Save();
    }
    mirror.Shutdown();
    r.textures().SetEvictCallback(nullptr);
  }

  bool Initialize();
  void Draw(const DrawState& s, const DrawPrep* prep);
  // Draw's parts. Each has one call site, in Draw.
  //
  // At the first use of a frame's command list: forgets the previous list's state.
  void SyncListFrame();
  // The translation of `shader` with `modification` (translation_memo first), null on failure.
  DxbcShader::Translation* Translated(DxbcShader& shader, uint64_t modification);
  PipelineKey BuildPipelineKey(const RegisterFile& regs, xenos::PrimitiveType prim,
                               const ShaderModifications& mods, const DxbcShader::Translation* vst,
                               const DxbcShader::Translation* pst, uint8_t strip_cut,
                               bool rasterization, bool polygonal, reg::PA_SU_SC_MODE_CNTL sc,
                               reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask,
                               const BoundTargets& targets) const;
  // The draw's line in draw_log (open).
  void LogDraw(const DrawState& s, const RegisterFile& regs, uint32_t draw_index, uint32_t count,
               const DxbcShader& vs, const DxbcShader* ps, const ShaderModifications& mods,
               const PipelineKey& key, const ViewportInfo& vp, const Scissor& scissor);
  // The ready pipeline for `key` (hashed here), or null when the draw is skipped (compiling or
  // failed, counted). Drawing with the pipeline of the unspecialized shaders sets `vs` and `ps`
  // to those.
  ID3D12PipelineState* SelectPipeline(const PipelineKey& key, const DxbcShader::Translation& vst,
                                      const DxbcShader::Translation* pst, DxbcShader*& vs,
                                      DxbcShader*& ps, DxbcShader* vs_generic,
                                      DxbcShader* ps_generic, uint64_t pixel_mod,
                                      uint64_t generic_pixel_mod, const ShaderDrawRegs& sdr,
                                      xenos::PrimitiveType prim);
  // What BindTextures reads besides the shader, and where it writes the texture signs.
  struct TextureBindContext {
    const DrawState& s;
    const RegisterFile& regs;
    ID3D12GraphicsCommandList* list;
    TextureCache& texture_cache;
    uint64_t frame_fence;
    uint64_t captured;  // TextureCache::MarkCapture stamp
    uint8_t* signs;     // [32], by fetch constant: the bound textures' swizzled signs
  };
  // The shader's descriptor table into `indices`; returns its entry count.
  uint32_t BindTextures(const TextureBindContext& ctx, const DxbcShader& shader, uint32_t* indices);
  void FillSystemConstants(const DrawState& s, const RegisterFile& regs, xenos::PrimitiveType prim,
                           bool polygonal, xenos::Endian index_endian, const ViewportInfo& vp,
                           uint32_t used_textures, const uint8_t* signs,
                           DxbcShaderTranslator::SystemConstants& sys);
  DxbcShader* LoadShader(ShaderMemo* table, ShaderMemo*& last, const ShaderRef& ref,
                         xenos::ShaderType type);
  // Copies go to the end of `out`.
  bool RequestVertices(const DrawState& s, const DxbcShader& vs, std::vector<VertexCopy>& out);
  void Prepare(const DrawState& s, uint64_t tag, DrawPrep* prep, std::vector<VertexCopy>* out,
               std::vector<AuditRange>* audit);
  // A triangle fan as a host triangle list (v1, v2, v0), (v2, v3, v0)... like the plugin's
  // primitive processor; guest indices are copied raw (the VS still swaps them). The guest
  // indices are in physical memory (RequestVertices checked them); `count` >= 3. Out of line:
  // the vectorized interleaved stores are 256 bits wide, and inlined they would make Prepare
  // and Draw run VZEROUPPER before their calls (build.sh).
  [[gnu::noinline]] void WriteFanIndices(const DrawState& s, uint32_t count, bool wide,
                                         uint8_t* out) const;
  struct FanIndices {
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint32_t count = 0;  // host indices, 0: none (out of upload memory)
    bool wide = false;   // 32-bit
  };
  // The host indices of the triangle fan `s` (s.count >= 3) in upload memory tagged `tag`.
  FanIndices UploadFanIndices(const DrawState& s, uint64_t tag);
  void ResetListState();
  // Gives the shared buffer memory under [start, start + length) if it is reserved. Any thread;
  // the tile mappings are queued ahead of the copies the caller records afterwards.
  bool MapShared(uint32_t start, uint32_t length) override;
  // GuestMemoryMirror::Sink: vertex upload memory tagged `tag` (upload_fence).
  bool AllocateUpload(uint32_t size, uint64_t tag, GuestMemoryMirror::Upload* out) override {
    UploadPool::Alloc a;
    if (!vertex_upload.Allocate(size, 16, tag, &a)) return false;
    out->cpu = a.cpu;
    out->buffer = a.resource;
    out->offset = a.offset;
    return true;
  }
  void FlushCopies(ID3D12GraphicsCommandList* list);
  void TransitionShared(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to);
  // Uploads cb.size bytes of cb.last unless cb.gpu is current, and binds it at `root_index`.
  bool CommitCbv(CachedCbv& cb, uint32_t root_index, ID3D12GraphicsCommandList* list);
  bool UploadCbv(CachedCbv& cb, const void* data, uint32_t size, uint32_t root_index,
                 ID3D12GraphicsCommandList* list) {
    if (cb.size != size || !Equal(cb.last, data, size)) {
      std::memcpy(cb.last, data, size);
      cb.size = size;
      cb.gpu = 0;
    }
    return CommitCbv(cb, root_index, list);
  }
  uint32_t TextureDescriptor(const TextureBinding& b, const DxbcShader::TextureBinding& tb);
  // TextureCache::EvictCallback: frees the views of evicted textures.
  void ReleaseViews(const std::vector<ID3D12Resource*>& resources);
  uint32_t SamplerDescriptor(const SamplerKey& k);
  // The pipeline for `key` (`hash`: its XXH3), added and recorded in the store if new.
  // variant (for the log): 1 the vertex, 2 the pixel shader is a bool specialization, 4 the
  // pixel shader's texture signs are specialized. `background`: a new one compiles behind the
  // pipelines draws wait for (prewarm_jobs).
  Pipeline* GetPipeline(uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
                        const DxbcShader::Translation* ps, uint32_t variant,
                        bool background = false);
  // Moves a pipeline's prewarm job, if no worker has taken it yet, to the draw queue: a draw
  // waits for it now.
  void PromotePipeline(Pipeline* p);
  // At the end of a frame: waits until none of the pipelines its draws skipped is compiling,
  // within kPipelineWaitBudget.
  void WaitForPipelines();
  // Adds the pipeline under `hash` and creates it (queued on pso_threads when they run; a
  // prewarm job waits behind every draw's job).
  Pipeline* CreatePipeline(uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
                           const DxbcShader::Translation* ps, bool prewarm);
  // `saves`: this worker saves the library when the workers have been idle for a while.
  // `on_wait`: this worker compiles only while WaitForPipelines waits.
  void PipelineWorker(bool saves, bool on_wait);
  // Per frame: translates and queues stored pipelines from earlier runs for up to ~2 ms.
  void Prewarm();

  std::shared_ptr<CoherencyJob> MakeCoherencyJob();
  static constexpr uint32_t kRegisterBlocks[6] = {0,      0x4000, 0x4400,
                                                  0x4800, 0x4900, kGuestRegisterCount};
  static void HashRegisterBlocks(const uint32_t* regs, uint64_t out[5]) {
    for (uint32_t i = 0; i < 5; ++i)
      out[i] = XXH3_64bits(regs + kRegisterBlocks[i],
                           (kRegisterBlocks[i + 1] - kRegisterBlocks[i]) * 4) |
               1;
  }
  // The GPU half (render thread): jobs whose read back is recorded, waiting for their fence.
  static constexpr uint32_t kCoherencyReadback = 8u << 20;
  std::shared_ptr<CoherencyJob> coherency_next;
  std::deque<std::shared_ptr<CoherencyJob>> coherency_inflight;
  std::vector<ComPtr<ID3D12Resource>> coherency_readbacks;  // free, kCoherencyReadback each
  void CheckCoherencyJobs(uint64_t completed_fence);

  // --graw_native_mark_audit. Guest thread: the frames still to audit, the ranges taken this
  // frame (once each: key AuditKey), and the frame's counts.
  static constexpr uint32_t kAuditReadback = 64u << 20;  // bytes read back per audited frame
  uint32_t audit_left = 0;
  std::unordered_set<uint64_t> audit_seen;
  uint64_t audit_draws = 0, audit_ranges = 0, audit_bytes = 0, audit_over = 0, audit_shaders = 0;
  static uint64_t AuditKey(const AuditRange& a) { return XXH3_64bits(&a, sizeof(a)); }
  void AuditShader(const ShaderRef& ref, const DxbcShader* shader, const char* type);
  // Render thread: the frame's read back, recorded after each audited draw's copies, and the
  // frames waiting for their fence.
  struct AuditJob {
    struct Entry {
      AuditRange range;
      uint32_t offset;  // in the read back
      uint32_t draw;    // frame_draws when it was recorded (the draw log's index of the draw)
    };
    uint64_t frame = 0, fence = 0;
    ComPtr<ID3D12Resource> readback;
    std::vector<Entry> entries;
    uint32_t used = 0, over = 0;
  };
  std::unique_ptr<AuditJob> audit_job;
  std::deque<std::unique_ptr<AuditJob>> audit_inflight;
  std::vector<ComPtr<ID3D12Resource>> audit_readbacks;  // free, kAuditReadback each
  void AuditDraw(ID3D12GraphicsCommandList* list, const DrawPrep& prep);
  void CheckAuditJobs(uint64_t completed_fence);
};

// --graw_native_coherency_check: one frame's copied shared memory ranges, the bytes they should
// hold once the frame's copies are done (the shadow's at the frame's Swap), and their read back.
struct CoherencyJob {
  uint32_t frame = 0;
  std::vector<std::pair<uint32_t, uint32_t>> runs;  // (start, size), sorted and disjoint
  std::vector<uint8_t> expected;                    // the runs' bytes back to back
  uint64_t fence = 0;
  ComPtr<ID3D12Resource> readback;
};

bool DrawRenderer::Impl::Initialize() {
  device = r.d3d12();
  memory = rex::system::kernel_memory();
  if (!memory) {
    REXLOG_ERROR("GRAW native draw: no guest memory");
    return false;
  }
  membase = memory->physical_membase();
  vertex_upload.SetDevice(device);
  threaded = REXCVAR_GET(graw_native_render_thread);
  // kMax_N_1 is log2(N) + 1.
  if (const int32_t samples = std::clamp(REXCVAR_GET(graw_native_anisotropy), 1, 16); samples > 1)
    min_aniso = xenos::AnisoFilter(std::bit_width(uint32_t(samples)));
  if (const std::string dir = REXCVAR_GET(graw_native_capture_dir); !dir.empty())
    debug_path = (std::filesystem::path(dir) / "draw-debug.txt").string();
  constant_upload.SetDevice(device);
  // Race start keeps 3 vertex and 10 constant blocks in flight; creating one there takes up to
  // 16 ms.
  vertex_upload.Reserve(4);
  constant_upload.Reserve(12);
  root_signature = CreateBindlessRootSignature(device, &root_signature_hash);
  if (!root_signature) return false;

  const D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC rd =
      BufferDesc(kSharedMemorySize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // Tier 2: reads of chunks without memory return zero, as in a committed buffer never written.
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  shared_tiled = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                       sizeof(options))) &&
                 options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2 &&
                 SUCCEEDED(device->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&shared)));
  if (!shared_tiled) {
    if (Failed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&shared)),
               "shared memory buffer (512 MB)"))
      return false;
  }
  shared_gpu = shared->GetGPUVirtualAddress();
  // The mirror's page state, write watches and checks (guest_memory_mirror.h).
  if (!mirror.Initialize(memory, this, REXCVAR_GET(graw_native_watch_trace),
                         REXCVAR_GET(graw_native_coherency_check)))
    return false;
  mirror.SetCounters(threaded ? &prep_upload_bytes : &stats.upload_bytes,
                     threaded ? &prep_uploads : &stats.uploads, &PrepDiag());

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&view_heap)), "draw view heap"))
    return false;
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sampler_heap)), "draw sampler heap"))
    return false;
  view_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  view_cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
  view_gpu = view_heap->GetGPUDescriptorHandleForHeapStart();
  sampler_cpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
  sampler_gpu = sampler_heap->GetGPUDescriptorHandleForHeapStart();

  auto view_at = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
    h.ptr += size_t(i) * view_stride;
    return h;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_R32_TYPELESS;
  sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Buffer.NumElements = kSharedMemorySize / 4;
  sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(shared.Get(), &sd, view_at(0));
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = kSharedMemorySize / 4;
  ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(shared.Get(), nullptr, &ud, view_at(1));
  // Null textures for unbound or mismatching bindings.
  D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  nd.Texture2DArray.MipLevels = 1;
  nd.Texture2DArray.ArraySize = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNull2DArray));
  nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  nd.Texture3D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNull3D));
  nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  nd.TextureCube.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNullCube));
  r.textures().SetEvictCallback(
      [this](const std::vector<ID3D12Resource*>& resources) { ReleaseViews(resources); });
  REXLOG_INFO(
      "GRAW native draw: up (shared memory 512 MB {}, bindless heaps {} views / {} samplers)",
      shared_tiled ? "reserved" : "committed", kViewHeapSize, kSamplerHeapSize);
  rex::graphics::g_dxbc_implicit_gradients = REXCVAR_GET(graw_native_implicit_gradients);
  REXLOG_INFO("GRAW native draw: implicit gradients {}",
              rex::graphics::g_dxbc_implicit_gradients ? "on" : "off");
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
  dynamic_depth_bias = REXCVAR_GET(graw_native_dynamic_depth_bias) &&
                       SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16,
                                                             &options16, sizeof(options16))) &&
                       options16.DynamicDepthBiasSupported;
  REXLOG_INFO("GRAW native draw: dynamic depth bias {}", dynamic_depth_bias ? "on" : "off");
  if (REXCVAR_GET(graw_native_pipeline_cache)) {
    // The runtime's cache root (userdata\cache unless --cache_root): co_run keeps it.
    rex::Runtime* runtime =
        rex::system::kernel_state() ? rex::system::kernel_state()->emulator() : nullptr;
    std::filesystem::path dir = runtime ? runtime->cache_root() : std::filesystem::path();
    if (dir.empty()) {
      REXLOG_WARN("GRAW native pipelines: no cache root, no pipeline cache");
    } else {
      // Shaders translated the other way can't share the cache's pipelines.
      dir /= rex::graphics::g_dxbc_implicit_gradients ? "graw-native" : "graw-native-gradients";
      store.Open(dir, sizeof(PipelineKey), kPipelineKeyVersion);
      library_path = dir / "pipelines.d3d12lib";
    }
  }
  if (REXCVAR_GET(graw_native_async_pipelines)) {
    pipeline_wait = REXCVAR_GET(graw_native_pipeline_wait);
    generic_pipelines = REXCVAR_GET(graw_native_generic_pipelines);
    int threads = REXCVAR_GET(graw_native_pipeline_threads);
    if (threads <= 0) threads = int(std::thread::hardware_concurrency() / 4);
    threads = std::clamp(threads, 1, 4);
    // While a frame waits for its pipelines, the game threads wait as well.
    const int total = std::max(threads, std::min(int(std::thread::hardware_concurrency()) - 2, 8));
    for (int i = 0; i < total; ++i) {
      pso_threads.emplace_back([this, i, threads] { PipelineWorker(i == 0, i >= threads); });
      wchar_t name[32];
      std::swprintf(name, 32, L"GRAW native pipelines %d", i);
      SetThreadDescription(pso_threads.back().native_handle(), name);
    }
  }
  Prewarm();  // the first slice before the first frame, which needs some of them
  return true;
}

void DrawRenderer::Impl::PipelineWorker(bool saves, bool on_wait) {
  std::call_once(library_open, [this] { library.Open(device, library_path, root_signature_hash); });
  std::unique_lock<std::mutex> lock(pso_mutex);
  auto has_work = [this] { return pso_stopping || !pso_jobs.empty() || !prewarm_jobs.empty(); };
  for (;;) {
    if (on_wait) {
      pso_wait_cv.wait(lock, [this] { return pso_stopping || (pso_waiting && !pso_jobs.empty()); });
    } else if (saves && library.dirty()) {
      // Save once no new pipeline came for 3 s (the burst is over).
      if (!pso_cv.wait_for(lock, std::chrono::seconds(3), has_work)) {
        lock.unlock();
        library.Save();
        lock.lock();
        continue;
      }
    } else {
      pso_cv.wait(lock, has_work);
    }
    if (pso_stopping) return;  // pending pipelines never get used
    std::deque<PipelineJob>& queue = !pso_jobs.empty() ? pso_jobs : prewarm_jobs;
    const PipelineJob job = queue.front();
    queue.pop_front();
    lock.unlock();
    ComPtr<ID3D12PipelineState> pso;
    bool loaded;
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = library.Create(job.desc, &pso, &loaded);
    if (job.logged)
      GRAW_LOG_FIRST(
          kNewPipelineLog, INFO, "GRAW native pipelines: compiled in {:.1f} ms{}",
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
          loaded ? " (from the library)" : "");
    if (SUCCEEDED(hr)) {
      job.target->pso = std::move(pso);
      job.target->state.store(kReady, std::memory_order_release);
    } else {
      GRAW_WARN_ONCE("GRAW native draw: CreateGraphicsPipelineState failed {:08X}", uint32_t(hr));
      job.target->state.store(kFailed, std::memory_order_release);
    }
    lock.lock();
    pso_ready_cv.notify_all();
  }
}

void DrawRenderer::Impl::ResetListState() {
  list_dirty = true;
  bound_pso = nullptr;
  bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  bound_viewport = {};
  bound_scissor = {};
  std::memset(bound_blend, 0xFF, sizeof(bound_blend));  // NaN: never equal
  bound_stencil_ref = UINT32_MAX;
  bound_depth_bias = bound_slope_bias = NAN;
  for (auto& h : bound_rtv) h.ptr = 0;
  bound_dsv.ptr = 0;
  bound_rt_count = UINT32_MAX;
  bound_ibv = {};
  for (CachedCbv* cb : Cbvs()) cb->bound = 0;
}

void DrawRenderer::Impl::TransitionShared(ID3D12GraphicsCommandList* list,
                                          D3D12_RESOURCE_STATES to) {
  if (shared_state == to) return;
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(shared.Get(), shared_state, to);
  list->ResourceBarrier(1, &b);
  shared_state = to;
}

std::shared_ptr<CoherencyJob> DrawRenderer::Impl::MakeCoherencyJob() {
  GuestMemoryMirror::Coherency& c = *mirror.coherency;
  if (c.frame_copies.empty()) return nullptr;
  auto job = std::make_shared<CoherencyJob>();
  job->frame = mirror.watch_frame.load(std::memory_order_relaxed);
  std::sort(c.frame_copies.begin(), c.frame_copies.end());
  uint32_t total = 0;
  for (const auto& [start, size] : c.frame_copies) {
    uint32_t end = start + size;
    if (!job->runs.empty()) {
      auto& last = job->runs.back();
      const uint32_t last_end = last.first + last.second;
      if (start <= last_end) {
        if (end > last_end) {
          const uint32_t grow = std::min(end - last_end, kCoherencyReadback - total);
          last.second += grow;
          total += grow;
        }
        continue;
      }
    }
    if (total >= kCoherencyReadback) break;
    const uint32_t take = std::min(size, kCoherencyReadback - total);
    job->runs.push_back({start, take});
    total += take;
  }
  c.frame_copies.clear();
  job->expected.resize(total);
  uint8_t* out = job->expected.data();
  for (const auto& [start, size] : job->runs) {
    std::memcpy(out, c.shadow.get() + start, size);
    out += size;
  }
  return job;
}

void DrawRenderer::Impl::CheckCoherencyJobs(uint64_t completed_fence) {
  while (!coherency_inflight.empty() && coherency_inflight.front()->fence <= completed_fence) {
    std::shared_ptr<CoherencyJob> job = std::move(coherency_inflight.front());
    coherency_inflight.pop_front();
    void* data = nullptr;
    const D3D12_RANGE range = {0, job->expected.size()};
    if (SUCCEEDED(job->readback->Map(0, &range, &data))) {
      const uint8_t* got = static_cast<const uint8_t*>(data);
      const uint8_t* want = job->expected.data();
      for (const auto& [start, size] : job->runs) {
        for (uint32_t off = 0; off < size;) {
          const uint32_t addr = start + off;
          const uint32_t n =
              std::min(size - off, ((addr >> kPageShift) + 1) * (1u << kPageShift) - addr);
          if (std::memcmp(got + off, want + off, n)) {
            ++stats.gpu_mismatch_pages;
            uint32_t differ = 0, first = UINT32_MAX;
            for (uint32_t i = 0; i < n; ++i)
              if (got[off + i] != want[off + i]) {
                ++differ;
                first = std::min(first, addr + i);
              }
            GRAW_LOG_FIRST(200, WARN,
                           "GRAW native coherency: frame {} shared memory page {:05X} (copied run "
                           "{:08X}+{:X}) differs from its copies after the frame: {} of {} bytes "
                           "from {:08X}",
                           job->frame, addr >> kPageShift, start, size, differ, n, first);
          }
          off += n;
        }
        got += size;
        want += size;
      }
      const D3D12_RANGE none = {0, 0};
      job->readback->Unmap(0, &none);
    }
    coherency_readbacks.push_back(std::move(job->readback));
  }
}

void DrawRenderer::Impl::AuditShader(const ShaderRef& ref, const DxbcShader* shader,
                                     const char* type) {
  if (!shader) return;
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase + ref.ucode);
  const uint64_t hash = XXH3_64bits(ucode, size_t(ref.dwords) * sizeof(uint32_t));
  if (hash == shader->ucode_data_hash()) return;
  ++audit_shaders;
  GRAW_LOG_FIRST(100, WARN,
                 "GRAW native audit: frame {} {} shader {:08X} ({} dwords{}) hashes {:016X} in "
                 "guest memory, the shader memo gave {:016X}",
                 mirror.watch_frame.load(std::memory_order_relaxed), type, ref.ucode, ref.dwords,
                 ref.immediate ? ", immediate" : "", hash, shader->ucode_data_hash());
}

void DrawRenderer::Impl::AuditDraw(ID3D12GraphicsCommandList* list, const DrawPrep& prep) {
  if (!audit_job) {
    audit_job = std::make_unique<AuditJob>();
    audit_job->frame = r.frame_index();
    if (!audit_readbacks.empty()) {
      audit_job->readback = std::move(audit_readbacks.back());
      audit_readbacks.pop_back();
    } else {
      CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, kAuditReadback, D3D12_RESOURCE_STATE_COPY_DEST,
                   "audit readback", &audit_job->readback);
    }
  }
  AuditJob& job = *audit_job;
  if (!job.readback) return;
  TransitionShared(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (uint32_t i = 0; i < prep.audit_count; ++i) {
    const AuditRange& a = prep.audit[i];
    if (job.used + a.size > kAuditReadback) {
      ++job.over;
      continue;
    }
    list->CopyBufferRegion(job.readback.Get(), job.used, shared.Get(), a.start, a.size);
    job.entries.push_back({a, job.used, frame_draws});
    job.used += a.size;
  }
}

void DrawRenderer::Impl::CheckAuditJobs(uint64_t completed_fence) {
  while (!audit_inflight.empty() && audit_inflight.front()->fence <= completed_fence) {
    std::unique_ptr<AuditJob> job = std::move(audit_inflight.front());
    audit_inflight.pop_front();
    void* data = nullptr;
    const D3D12_RANGE range = {0, job->used};
    if (!job->readback || FAILED(job->readback->Map(0, &range, &data))) {
      REXLOG_WARN("GRAW native audit: frame {}: no read back", job->frame);
      continue;
    }
    const uint8_t* got = static_cast<const uint8_t*>(data);
    uint64_t differ = 0, newer = 0;
    for (const AuditJob::Entry& e : job->entries) {
      const uint8_t* bytes = got + e.offset;
      if (XXH3_64bits(bytes, e.range.size) == e.range.hash) continue;
      ++differ;
      // Shared memory may hold what the game wrote after the draw was queued (a copy recorded
      // too early) or something older (a write the page watch or the hashes missed).
      const bool now = !std::memcmp(bytes, membase + e.range.start, e.range.size);
      newer += now;
      uint32_t pages = 0, first = UINT32_MAX;
      for (uint32_t off = 0; off < e.range.size;) {
        const uint32_t addr = e.range.start + off;
        const uint32_t n =
            std::min(e.range.size - off, (((addr >> kPageShift) + 1) << kPageShift) - addr);
        if (std::memcmp(bytes + off, membase + addr, n)) {
          ++pages;
          first = std::min(first, addr >> kPageShift);
        }
        off += n;
      }
      const uint32_t p = first == UINT32_MAX ? e.range.start >> kPageShift : first;
      const uint64_t bit = 1ull << (p & 63);
      GRAW_LOG_FIRST(
          200, WARN,
          "GRAW native audit: frame {} draw {}: shared memory {:08X}+{:X} differs from guest "
          "memory when the draw was queued; {}: {} pages differ from it, first {:05X} (valid {}, "
          "hot {}, pending {}, armed at frame {}, written at frame {})",
          job->frame, e.draw, e.range.start, e.range.size,
          now ? "it holds guest memory as it is now" : "and from guest memory now", pages, p,
          (mirror.valid[p >> 6].load(std::memory_order_relaxed) & bit) != 0,
          (mirror.hot[p >> 6].load(std::memory_order_relaxed) & bit) != 0,
          (mirror.pending[p >> 6].load(std::memory_order_relaxed) & bit) != 0,
          mirror.armed_frame[p].load(std::memory_order_relaxed),
          int64_t(mirror.write_frame[p].load(std::memory_order_relaxed)) - 1);
    }
    const D3D12_RANGE none = {0, 0};
    job->readback->Unmap(0, &none);
    REXLOG_INFO(
        "GRAW native audit: frame {}: {} ranges ({:.1f} MB) read back{}, {} differ from guest "
        "memory when their draw was queued ({} hold it as it is now)",
        job->frame, job->entries.size(), double(job->used) / (1 << 20),
        job->over ? fmt::format(", {} not (read back full)", job->over) : std::string(), differ,
        newer);
    audit_readbacks.push_back(std::move(job->readback));
  }
}

bool DrawRenderer::Impl::MapShared(uint32_t start, uint32_t length) {
  if (!shared_tiled) return true;
  const uint32_t first = start >> kSharedChunkShift;
  const uint32_t last = (start + length - 1) >> kSharedChunkShift;
  auto mapped = [&](uint32_t c) {
    return (shared_mapped[c >> 6].load(std::memory_order_acquire) >> (c & 63)) & 1;
  };
  bool all = true;
  for (uint32_t c = first; c <= last && all; ++c) all = mapped(c);
  if (all) return true;

  std::lock_guard<std::mutex> lock(shared_map_mutex);
  for (uint32_t c = first; c <= last; ++c) {
    if (mapped(c)) continue;
    uint32_t end = c + 1;
    while (end <= last && !mapped(end)) ++end;
    const uint32_t chunks = end - c;
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = uint64_t(chunks) << kSharedChunkShift;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    if (Failed(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "shared memory tile heap"))
      return false;
    D3D12_TILED_RESOURCE_COORDINATE coord = {};
    coord.X = UINT(uint64_t(c) << kSharedChunkShift) / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    D3D12_TILE_REGION_SIZE size = {};
    size.NumTiles = UINT(hd.SizeInBytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT heap_offset = 0;
    r.command_queue()->UpdateTileMappings(shared.Get(), 1, &coord, &size, heap.Get(), 1, &flags,
                                          &heap_offset, &size.NumTiles,
                                          D3D12_TILE_MAPPING_FLAG_NONE);
    shared_heaps.push_back(std::move(heap));
    for (uint32_t m = c; m < end; ++m)
      shared_mapped[m >> 6].fetch_or(1ull << (m & 63), std::memory_order_release);
    shared_mapped_chunks += chunks;
    // At most 128 lines (4 MB chunks of 512 MB); the frame places a mapping next to a glitch.
    REXLOG_INFO("GRAW native draw: shared memory {:08X}+{} MB mapped at frame {}, {} MB in all",
                c << kSharedChunkShift, chunks << (kSharedChunkShift - 20),
                mirror.watch_frame.load(std::memory_order_relaxed),
                shared_mapped_chunks << (kSharedChunkShift - 20));
    c = end - 1;
  }
  return true;
}

void DrawRenderer::Impl::FlushCopies(ID3D12GraphicsCommandList* list) {
  if (copies.empty()) {
    TransitionShared(list, kSharedReadState);
    return;
  }
  ++diag.flushes;
  ++stats.copy_batches;
  TransitionShared(list, D3D12_RESOURCE_STATE_COPY_DEST);
  for (const VertexCopy& c : copies)
    list->CopyBufferRegion(shared.Get(), c.dst, static_cast<ID3D12Resource*>(c.src), c.src_offset,
                             c.size);
  copies.clear();
  TransitionShared(list, kSharedReadState);
}

bool DrawRenderer::Impl::CommitCbv(CachedCbv& cb, uint32_t root_index,
                                   ID3D12GraphicsCommandList* list) {
  if (!cb.gpu) {
    // Whole float4s (a CBV is read in them; `last` has room). Streaming stores: each upload
    // takes fresh lines that only the GPU reads, so they bypass the caches instead of being
    // read from memory for ownership and then pushing the game's code and data out.
    // D3D12Device::CloseAndExecute fences them before the GPU can read them.
    const uint32_t vectors = std::max<uint32_t>((cb.size + 15) / 16, 1);
    UploadPool::Alloc a;
    if (!constant_upload.Allocate(vectors * 16, 256, r.frame_fence(), &a)) return false;
    auto* dst = reinterpret_cast<simde__m128i*>(a.cpu);
    const auto* src = reinterpret_cast<const simde__m128i*>(cb.last);
    for (uint32_t i = 0; i < vectors; ++i)
      simde_mm_stream_si128(dst + i, simde_mm_load_si128(src + i));
    cb.gpu = a.gpu;
    ++diag.cbv_uploads[root_index & 15];
    diag.cbv_bytes[root_index & 15] += cb.size;
  }
  if (cb.bound != cb.gpu) {
    list->SetGraphicsRootConstantBufferView(root_index, cb.gpu);
    cb.bound = cb.gpu;
  }
  return true;
}

uint32_t DrawRenderer::Impl::TextureDescriptor(const TextureBinding& b,
                                               const DxbcShader::TextureBinding& tb) {
  ++stats.srv_lookups;
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = b.srv;
  // The translator's space for the binding: 2D array (1D/2D), cube, or 3D-or-stacked (either).
  const D3D12_SRV_DIMENSION dim = desc.ViewDimension;
  bool fits;
  switch (tb.dimension) {
    case xenos::FetchOpDimension::kCube:
      fits = dim == D3D12_SRV_DIMENSION_TEXTURECUBE;
      break;
    case xenos::FetchOpDimension::k3DOrStacked:
      fits = dim == D3D12_SRV_DIMENSION_TEXTURE3D || dim == D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      break;
    default:
      fits = dim == D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      break;
  }
  if (!fits) return NullView(tb.dimension);
  if (tb.is_signed) {
    if (b.srv_signed_format == DXGI_FORMAT_UNKNOWN) return NullView(dim);
    desc.Format = b.srv_signed_format;
  }
  const uint64_t key = XXH3_64bits(&desc, sizeof(desc));
  auto views = srv_views.find(b.resource);
  if (views != srv_views.end())
    for (const auto& [view_key, index] : views->second)
      if (view_key == key) return index;
  uint32_t heap_index;
  if (!free_views.empty()) {
    heap_index = free_views.back();
    free_views.pop_back();
  } else if (views_allocated < kViewHeapSize) {
    heap_index = views_allocated++;
  } else {
    GRAW_WARN_ONCE("GRAW native draw: view heap full ({} descriptors)", kViewHeapSize);
    return NullView(dim);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
  h.ptr += size_t(heap_index) * view_stride;
  device->CreateShaderResourceView(b.resource, &desc, h);
  const uint32_t index = heap_index - kViewTableStart;
  if (views == srv_views.end()) views = srv_views.try_emplace(b.resource).first;
  views->second.emplace_back(key, index);
  ++stats.srv_created;
  return index;
}

void DrawRenderer::Impl::ReleaseViews(const std::vector<ID3D12Resource*>& resources) {
  for (ID3D12Resource* resource : resources) {
    auto it = srv_views.find(resource);
    if (it == srv_views.end()) continue;
    for (const auto& view : it->second) free_views.push_back(view.second + kViewTableStart);
    srv_views.erase(it);
  }
  std::vector<ID3D12Resource*> sorted(resources);
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 0; i < (size_t(1) << kSrvMemoBits); ++i) {
    SrvMemo& m = srv_memo[i];
    if (m.resource && std::binary_search(sorted.begin(), sorted.end(), m.resource)) m = SrvMemo{};
  }
}

uint32_t DrawRenderer::Impl::SamplerDescriptor(const SamplerKey& k) {
  const uint32_t key = k.value();
  auto it = sampler_indices.find(key);
  if (it != sampler_indices.end()) return it->second;
  if (samplers_allocated >= kSamplerHeapSize) {
    GRAW_WARN_ONCE("GRAW native draw: sampler heap full");
    return 0;
  }
  const uint32_t index = samplers_allocated++;
  D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_cpu;
  h.ptr += size_t(index) * sampler_stride;
  D3D12_SAMPLER_DESC d = SamplerDesc(k);
  device->CreateSampler(&d, h);
  sampler_indices.emplace(key, index);
  ++stats.samplers_created;
  return index;
}

DrawRenderer::Impl::Pipeline* DrawRenderer::Impl::GetPipeline(uint64_t hash, const PipelineKey& key,
                                                              const DxbcShader::Translation& vs,
                                                              const DxbcShader::Translation* ps,
                                                              uint32_t variant, bool background) {
  auto it = pipelines.find(hash);
  Pipeline* p;
  if (it != pipelines.end()) {
    p = it->second.get();
  } else {
    if (new_pipelines_logged < kNewPipelineLog) {
      ++new_pipelines_logged;
      const auto st = pipeline_states.find(StateDigest(key));
      const auto pr = pipeline_pairs.find(PairDigest(key));
      REXLOG_INFO(
          "GRAW native pipelines: new at frame {}: vs {:016X}{} ps {:016X}{}{}, state {} "
          "({} pipelines), shader pair {} ({} pipelines)",
          r.frame_index(), vs.shader().ucode_data_hash(), variant & 1 ? " (bools)" : "",
          ps ? ps->shader().ucode_data_hash() : 0, variant & 2 ? " (bools)" : "",
          variant & 4 ? " (signs)" : "", st == pipeline_states.end() ? "new" : "known",
          st == pipeline_states.end() ? 0 : st->second,
          pr == pipeline_pairs.end() ? "new" : "known",
          pr == pipeline_pairs.end() ? 0 : pr->second);
    }
    p = CreatePipeline(hash, key, vs, ps, background);
    ++stats.pso_created;
    if (store.is_open()) {
      PipelineKey stored = key;
      stored.vs = stored.ps = nullptr;
      const Shader& vss = vs.shader();
      store.RecordShader(uint32_t(vss.type()), vss.ucode_data_hash(), vss.ucode_dwords(),
                         vss.ucode_dword_count());
      if (ps) {
        const Shader& pss = ps->shader();
        store.RecordShader(uint32_t(pss.type()), pss.ucode_data_hash(), pss.ucode_dwords(),
                           pss.ucode_dword_count());
      }
      store.RecordPipeline(vss.ucode_data_hash(), vs.modification(),
                           ps ? ps->shader().ucode_data_hash() : 0, ps ? ps->modification() : 0,
                           &stored);
    }
  }
  return p;
}

void DrawRenderer::Impl::PromotePipeline(Pipeline* p) {
  p->prewarm = false;
  std::lock_guard<std::mutex> lock(pso_mutex);
  auto it = std::find_if(prewarm_jobs.begin(), prewarm_jobs.end(),
                         [p](const PipelineJob& j) { return j.target == p; });
  if (it == prewarm_jobs.end()) return;  // being compiled
  pso_jobs.push_back(*it);
  prewarm_jobs.erase(it);
}

void DrawRenderer::Impl::WaitForPipelines() {
  if (pipeline_wait && !frame_pending.empty()) {
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lock(pso_mutex);
      pso_waiting = true;
      pso_wait_cv.notify_all();
      pso_ready_cv.wait_for(lock, kPipelineWaitBudget, [this] {
        return std::none_of(frame_pending.begin(), frame_pending.end(), [](const Pipeline* p) {
          return p->state.load(std::memory_order_acquire) == kCompiling;
        });
      });
      pso_waiting = false;
    }
    stats.pso_wait_us += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
  }
  frame_pending.clear();
}

DrawRenderer::Impl::Pipeline* DrawRenderer::Impl::CreatePipeline(uint64_t hash,
                                                                 const PipelineKey& key,
                                                                 const DxbcShader::Translation& vs,
                                                                 const DxbcShader::Translation* ps,
                                                                 bool prewarm) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
  std::memset(&d, 0, sizeof(d));  // padding too: PipelineLibrary hashes the bytes
  d.pRootSignature = root_signature.Get();
  if (dynamic_depth_bias) d.Flags = D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
  d.IBStripCutValue = key.strip_cut == 1   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                      : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                           : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  d.VS.pShaderBytecode = vs.translated_binary().data();
  d.VS.BytecodeLength = vs.translated_binary().size();
  if (ps && !key.disable_raster) {
    d.PS.pShaderBytecode = ps->translated_binary().data();
    d.PS.BytecodeLength = ps->translated_binary().size();
  }
  if (key.gs_key) {
    auto g = geometry_shaders.find(key.gs_key);
    if (g == geometry_shaders.end()) {
      GeometryShaderKey gk;
      gk.key = key.gs_key;
      g = geometry_shaders.emplace(key.gs_key, CreateGeometryShader(gk)).first;
    }
    d.GS.pShaderBytecode = g->second.data();
    d.GS.BytecodeLength = g->second.size() * sizeof(uint32_t);
  }
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
  d.RasterizerState.FillMode = key.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE(key.cull);
  d.RasterizerState.FrontCounterClockwise = key.front_ccw;
  d.RasterizerState.DepthBias = key.depth_bias;
  d.RasterizerState.SlopeScaledDepthBias = key.slope_scaled_depth_bias;
  d.RasterizerState.DepthClipEnable = key.depth_clip;
  d.SampleMask = UINT_MAX;
  d.SampleDesc.Count = key.samples;
  if (!key.disable_raster) {
    d.DepthStencilState.DepthEnable = key.depth_enable;
    d.DepthStencilState.DepthWriteMask =
        key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func);
    if (!key.depth_enable) d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.DepthStencilState.StencilEnable = key.stencil_enable;
    d.DepthStencilState.StencilReadMask = key.stencil_read_mask;
    d.DepthStencilState.StencilWriteMask = key.stencil_write_mask;
    auto face = [](const uint8_t f[4]) {
      D3D12_DEPTH_STENCILOP_DESC o;
      o.StencilFailOp = f[0] ? D3D12_STENCIL_OP(f[0]) : D3D12_STENCIL_OP_KEEP;
      o.StencilDepthFailOp = f[1] ? D3D12_STENCIL_OP(f[1]) : D3D12_STENCIL_OP_KEEP;
      o.StencilPassOp = f[2] ? D3D12_STENCIL_OP(f[2]) : D3D12_STENCIL_OP_KEEP;
      o.StencilFunc = f[3] ? D3D12_COMPARISON_FUNC(f[3]) : D3D12_COMPARISON_FUNC_ALWAYS;
      return o;
    };
    d.DepthStencilState.FrontFace = face(key.front);
    d.DepthStencilState.BackFace = face(key.back);
    d.DSVFormat = key.dsv;
  }
  d.BlendState.IndependentBlendEnable = TRUE;
  d.NumRenderTargets = key.num_rts;
  for (uint32_t i = 0; i < 4; ++i) {
    d.RTVFormats[i] = key.rtv[i];
    const PipelineKey::Rt& rt = key.rt[i];
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[i];
    b.BlendEnable = rt.blend;
    b.SrcBlend = kD3DBlend[rt.src];
    b.DestBlend = kD3DBlend[rt.dst];
    b.BlendOp = kD3DBlendOp[rt.op & 7];
    b.SrcBlendAlpha = kD3DBlend[rt.src_a];
    b.DestBlendAlpha = kD3DBlend[rt.dst_a];
    b.BlendOpAlpha = kD3DBlendOp[rt.op_a & 7];
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = rt.mask;
  }
  Pipeline* p = pipelines.emplace(hash, std::make_unique<Pipeline>()).first->second.get();
  if (new_pipelines_logged < kNewPipelineLog) {
    ++pipeline_states[StateDigest(key)];
    ++pipeline_pairs[PairDigest(key)];
  }
  if (!pso_threads.empty()) {
    p->prewarm = prewarm;
    {
      std::lock_guard<std::mutex> lock(pso_mutex);
      (prewarm ? prewarm_jobs : pso_jobs).push_back({p, d, !prewarm});
    }
    pso_cv.notify_one();
    return p;
  }
  HRESULT hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p->pso));
  if (FAILED(hr)) {
    GRAW_WARN_ONCE("GRAW native draw: CreateGraphicsPipelineState failed {:08X}", uint32_t(hr));
    p->state.store(kFailed, std::memory_order_relaxed);
    return p;
  }
  p->state.store(kReady, std::memory_order_relaxed);
  return p;
}

void DrawRenderer::Impl::Prewarm() {
  const std::vector<PipelineStore::Pipeline>& list = store.loaded_pipelines();
  if (prewarm_next >= list.size()) return;
  const auto t0 = std::chrono::steady_clock::now();
  auto load = [&](uint64_t hash, xenos::ShaderType type) -> DxbcShader* {
    const PipelineStore::Shader* s = store.loaded_shader(hash);
    if (!s || s->type != uint32_t(type) || s->ucode.empty()) return nullptr;
    return shaders.Load(type, s->ucode.data(), uint32_t(s->ucode.size()));
  };
  do {
    const PipelineStore::Pipeline& sp = list[prewarm_next++];
    DxbcShader* vs = load(sp.vs_hash, xenos::ShaderType::kVertex);
    DxbcShader* ps = sp.ps_hash ? load(sp.ps_hash, xenos::ShaderType::kPixel) : nullptr;
    DxbcShader::Translation* vst = vs ? shaders.Translate(*vs, sp.vs_mod) : nullptr;
    DxbcShader::Translation* pst = ps ? shaders.Translate(*ps, sp.ps_mod) : nullptr;
    if (!vst || (sp.ps_hash && !pst) || sp.key.size() != sizeof(PipelineKey)) {
      ++prewarm_failed;
      continue;
    }
    PipelineKey key;
    std::memcpy(&key, sp.key.data(), sizeof(key));
    // A stored key comes from disk: its blend factors index kD3DBlend in CreatePipeline, so a
    // corrupt or foreign one is dropped here (the other fields reach D3D12 as enum values, which
    // it validates).
    bool valid = true;
    for (const PipelineKey::Rt& rt : key.rt)
      for (uint8_t factor : {rt.src, rt.dst, rt.src_a, rt.dst_a})
        valid &= factor < std::size(kD3DBlend);
    if (!valid) {
      ++prewarm_failed;
      continue;
    }
    key.vs = vst;
    key.ps = pst;
    const uint64_t hash = XXH3_64bits(&key, sizeof(key));
    if (!pipelines.count(hash)) CreatePipeline(hash, key, *vst, pst, true);
  } while (prewarm_next < list.size() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2));
  prewarm_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ++prewarm_frames;
  if (prewarm_next == list.size())
    REXLOG_INFO(
        "GRAW native pipelines: prewarm queued {} pipelines ({} failed) in {:.1f} ms "
        "over {} frames",
        list.size() - prewarm_failed, prewarm_failed, prewarm_ms, prewarm_frames);
}

namespace {

// Fan vertex i is guest index i of `guest`, or i itself without one.
template <typename T>
void FanAsList(const uint8_t* guest, uint32_t count, T* out) {
  auto at = [guest](uint32_t i) {
    if (!guest) return T(i);
    T v;
    std::memcpy(&v, guest + size_t(i) * sizeof(T), sizeof(T));
    return v;
  };
  const T first = at(0);
  for (uint32_t t = 0; t + 2 < count; ++t, out += 3) {
    out[0] = at(t + 1);
    out[1] = at(t + 2);
    out[2] = first;
  }
}

}  // namespace

void DrawRenderer::Impl::WriteFanIndices(const DrawState& s, uint32_t count, bool wide,
                                         uint8_t* out) const {
  const uint8_t* guest = s.indexed ? membase + s.index_address : nullptr;
  if (wide)
    FanAsList(guest, count, reinterpret_cast<uint32_t*>(out));
  else
    FanAsList(guest, count, reinterpret_cast<uint16_t*>(out));
}

DrawRenderer::Impl::FanIndices DrawRenderer::Impl::UploadFanIndices(const DrawState& s,
                                                                    uint64_t tag) {
  const uint32_t host_count = (s.count - 2) * 3;
  const bool wide = s.indexed ? s.index_32bit : s.count > 0xFFFF;
  UploadPool::Alloc a;
  if (!vertex_upload.Allocate(uint64_t(host_count) * (wide ? 4 : 2), 16, tag, &a)) return {};
  WriteFanIndices(s, s.count, wide, a.cpu);
  return {a.gpu, host_count, wide};
}

DxbcShader* DrawRenderer::Impl::LoadShader(ShaderMemo* table, ShaderMemo*& last,
                                           const ShaderRef& ref, xenos::ShaderType type) {
  if (!ref.ucode || !ref.dwords) return nullptr;
  // The previous draw's shader again (most draws): no guest memory read, as before the table.
  if (last && last->shader && last->ucode == ref.ucode && last->dwords == ref.dwords &&
      last->immediate_seq == ref.immediate_seq && last->binding == ref.binding)
    return last->shader;
  // Declaration-patched vertex shaders (IM_LOAD_IMMEDIATE) come from the tracker's copy.
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase + ref.ucode);
  uint32_t sample[ShaderMemo::kSample];
  for (uint32_t i = 0; i < ShaderMemo::kSample / 2; ++i) {
    sample[i] = ucode[std::min(i, ref.dwords - 1)];
    sample[ShaderMemo::kSample / 2 + i] = ucode[ref.dwords - 1 - std::min(i, ref.dwords - 1)];
  }
  ShaderMemo& memo = table[ShaderMemo::Slot(ref)];
  last = &memo;
  if (memo.shader && memo.ucode == ref.ucode && memo.dwords == ref.dwords &&
      memo.immediate_seq == ref.immediate_seq &&
      std::memcmp(memo.sample, sample, sizeof(sample)) == 0) {
    if (memo.binding == ref.binding) return memo.shader;
    GRAW_LOG_FIRST(
        8, INFO, "GRAW native draw: vertex shader {:08X} ({} dwords) rebound in place at frame {}",
        ref.ucode, ref.dwords, r.frame_index());
  }
  memo.ucode = ref.ucode;
  memo.dwords = ref.dwords;
  memo.immediate_seq = ref.immediate_seq;
  memo.binding = ref.binding;
  std::memcpy(memo.sample, sample, sizeof(sample));
  PrepareDiag& pd = PrepDiag();
  ++pd.shader_loads;
  if (ref.immediate) ++pd.shader_loads_immediate;
  memo.shader = shaders.Load(type, ucode, ref.dwords);
  return memo.shader;
}

// Vertex ranges the VS fetches and the index range into `out`. Fan indices are only checked:
// WriteFanIndices reads them from guest memory.
bool DrawRenderer::Impl::RequestVertices(const DrawState& s, const DxbcShader& vs,
                                         std::vector<VertexCopy>& out) {
  return mirror.RequestVertices(s, vs, out);
}

void DrawRenderer::Impl::Prepare(const DrawState& s, uint64_t tag, DrawPrep* prep,
                                 std::vector<VertexCopy>* out, std::vector<AuditRange>* audit) {
  prep->captured = r.textures().MarkCapture();
  if (!s.vs.ucode || !s.vs.dwords) return;
  prep->vs = LoadShader(vs_memo, vs_last, s.vs, xenos::ShaderType::kVertex);
  prep->ps = LoadShader(ps_memo, ps_last, s.ps, xenos::ShaderType::kPixel);
  profile::Mark(profile::kCapShaders);
  if (audit_left) {
    ++audit_draws;
    AuditShader(s.vs, static_cast<DxbcShader*>(prep->vs), "vertex");
    AuditShader(s.ps, static_cast<DxbcShader*>(prep->ps), "pixel");
  }
  if (!prep->vs) return;
  if (mirror.coherency || audit_left) HashRegisterBlocks(s.regs, prep->regs_hash);
  mirror.upload_fence = tag;
  const bool indexed_ok = !s.indexed || s.index_address;
  // Straight into the batch: copies made before a failed range still go out (their pages
  // are marked valid already).
  mirror.preparing = true;
  mirror.segment_conflict = false;
  mirror.draw_reads.clear();
  prep->upload_ok = indexed_ok && RequestVertices(s, *static_cast<DxbcShader*>(prep->vs), *out);
  mirror.preparing = false;
  if (mirror.segment_conflict ||
      mirror.segment_exact.size() >= GuestMemoryMirror::kSegmentExactLimit) {
    mirror.NewSegment();
    prep->copy_segment = true;
  }
  for (const GuestMemoryMirror::Read& read : mirror.draw_reads) mirror.SegmentAdd(read);
  if (audit_left) {
    for (const GuestMemoryMirror::Read& read : mirror.draw_reads) {
      const AuditRange a = {read.begin, read.end - read.begin,
                            XXH3_64bits(membase + read.begin, read.end - read.begin)};
      if (!audit_seen.insert(AuditKey(a)).second) continue;
      if (audit_bytes + a.size > kAuditReadback) {
        ++audit_over;
        continue;
      }
      audit->push_back(a);
      ++audit_ranges;
      audit_bytes += a.size;
    }
  }
  if (mirror.coherency)
    for (const GuestMemoryMirror::Read& read : mirror.draw_reads)
      mirror.coherency->segment_reads.push_back({read.begin, read.end});
  // Triangle fans: host indices from the guest's now (Draw reads them from here).
  if (prep->upload_ok && xenos::PrimitiveType(s.primitive) == xenos::PrimitiveType::kTriangleFan &&
      s.count >= 3) {
    const FanIndices fan = UploadFanIndices(s, tag);
    prep->fan_indices = fan.gpu;
    prep->fan_count = fan.count;
    prep->fan_wide = fan.wide;
  }
  profile::Mark(profile::kCapVertices);
}

void DrawRenderer::Impl::ShaderSlots::Assign(const DxbcShader& s) {
  // The translator numbers bindless descriptors from 1 (it counts the new binding before
  // assigning its index), so the table needs max index + 1 entries, not texture + sampler
  // count: the last binding (often a fetch's signed SRV) was left out of the upload.
  uint32_t entries = 1;
  mask = 0;
  for (const auto& tb : s.GetTextureBindingsAfterTranslation()) {
    entries = std::max(entries, tb.bindless_descriptor_index + 1);
    mask |= 1u << (tb.fetch_constant & 31);
  }
  for (const auto& sb : s.GetSamplerBindingsAfterTranslation()) {
    entries = std::max(entries, sb.bindless_descriptor_index + 1);
    mask |= 1u << (sb.fetch_constant & 31);
  }
  shader = &s;
  count = uint32_t(std::popcount(mask));
  n = std::min(entries, 64u);
  last = nullptr;
}

void DrawRenderer::Impl::SyncListFrame() {
  if (list_frame == r.frame_index()) return;
  // A new frame's list: nothing is bound, and shared memory decayed to the common state when the
  // previous frame's lists completed. Every CBV is uploaded again: the upload memory holding older
  // frames' constants may have been recycled.
  list_frame = r.frame_index();
  ResetListState();
  shared_state = D3D12_RESOURCE_STATE_COMMON;
  for (CachedCbv* cb : Cbvs()) cb->Reset();
}

DxbcShader::Translation* DrawRenderer::Impl::Translated(DxbcShader& shader, uint64_t modification) {
  TranslationMemo& m = translation_memo[(reinterpret_cast<uintptr_t>(&shader) >> 6) & 255];
  if (m.shader == &shader && m.modification == modification) {
    ++diag.translation_hits;
    return m.translation;
  }
  DxbcShader::Translation* t = shaders.Translate(shader, modification);
  if (t) m = {&shader, modification, t};
  return t;
}

// GetCurrentStateDescription, host render targets.
PipelineKey DrawRenderer::Impl::BuildPipelineKey(
    const RegisterFile& regs, xenos::PrimitiveType prim, const ShaderModifications& mods,
    const DxbcShader::Translation* vst, const DxbcShader::Translation* pst, uint8_t strip_cut,
    bool rasterization, bool polygonal, reg::PA_SU_SC_MODE_CNTL sc,
    reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask, const BoundTargets& targets) const {
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = vst;
  key.ps = pst;
  key.gs_key = GeometryShaderKeyFor(prim, mods.vertex, pst ? mods.pixel : 0);
  key.topology_type = uint8_t(TopologyTypeForPrimitive(prim));
  key.strip_cut = strip_cut;
  key.samples = 1;
  if (!rasterization) {
    key.disable_raster = 1;
    key.cull = uint8_t(D3D12_CULL_MODE_NONE);
    key.depth_clip = 1;
  } else {
    bool cull_front = false, cull_back = false;
    key.cull = uint8_t(D3D12_CULL_MODE_NONE);
    if (polygonal) {
      key.front_ccw = sc.face == 0;
      cull_front = sc.cull_front != 0;
      cull_back = sc.cull_back != 0;
      if (cull_front)
        key.cull = uint8_t(D3D12_CULL_MODE_FRONT);
      else if (cull_back)
        key.cull = uint8_t(D3D12_CULL_MODE_BACK);
      if (!cull_front && sc.polymode_front_ptype != xenos::PolygonType::kTriangles)
        key.wireframe = 1;
      if (!cull_back && sc.polymode_back_ptype != xenos::PolygonType::kTriangles) key.wireframe = 1;
      if (sc.poly_mode != xenos::PolygonModeEnable::kDualMode) key.wireframe = 0;
    }
    float poly_offset, poly_offset_scale;
    GetPreferredFacePolygonOffset(regs, polygonal, poly_offset_scale, poly_offset);
    key.depth_bias = rex::graphics::draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, poly_offset);
    key.slope_scaled_depth_bias = poly_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
    key.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
    if (targets.depth) {
      uint8_t func = uint8_t(xenos::CompareFunction::kAlways);
      if (depth_control.z_enable) {
        func = uint8_t(depth_control.zfunc);
        key.depth_write = depth_control.z_write_enable;
      }
      key.depth_func = uint8_t(D3D12_COMPARISON_FUNC_NEVER + func);
      key.depth_enable = func != uint8_t(xenos::CompareFunction::kAlways) || key.depth_write;
      if (depth_control.stencil_enable) {
        key.stencil_enable = 1;
        const bool bf = polygonal && depth_control.backface_enable;
        auto ref = regs.Get<reg::RB_STENCILREFMASK>(
            bf && cull_front ? XE_GPU_REG_RB_STENCILREFMASK_BF : XE_GPU_REG_RB_STENCILREFMASK);
        key.stencil_read_mask = uint8_t(ref.stencilmask);
        key.stencil_write_mask = uint8_t(ref.stencilwritemask);
        key.front[0] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail));
        key.front[1] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail));
        key.front[2] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass));
        key.front[3] = uint8_t(D3D12_COMPARISON_FUNC_NEVER + uint32_t(depth_control.stencilfunc));
        if (bf) {
          key.back[0] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail_bf));
          key.back[1] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail_bf));
          key.back[2] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass_bf));
          key.back[3] =
              uint8_t(D3D12_COMPARISON_FUNC_NEVER + uint32_t(depth_control.stencilfunc_bf));
        } else {
          std::memcpy(key.back, key.front, 4);
        }
      }
      if (key.depth_enable || key.stencil_enable) key.dsv = targets.depth->view_format;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (!targets.color[i]) continue;
      key.num_rts = uint8_t(i + 1);
      key.rtv[i] = targets.color[i]->view_format;
      PipelineKey::Rt& rt = key.rt[i];
      rt.mask = uint8_t((color_mask >> (i * 4)) & 0xF);
      BlendFactor src = BlendFactor::kOne, dst = BlendFactor::kZero, src_a = BlendFactor::kOne,
                  dst_a = BlendFactor::kZero;
      uint32_t op = 0, op_a = 0;
      if (rt.mask) {
        auto bc = regs.Get<reg::RB_BLENDCONTROL>(kBlendControlRegs[i]);
        src = kBlendFactorMap[uint32_t(bc.color_srcblend) & 31];
        dst = kBlendFactorMap[uint32_t(bc.color_destblend) & 31];
        op = uint32_t(bc.color_comb_fcn);
        src_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_srcblend) & 31];
        dst_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_destblend) & 31];
        op_a = uint32_t(bc.alpha_comb_fcn);
      }
      if (src != BlendFactor::kOne || dst != BlendFactor::kZero || op != 0 ||
          src_a != BlendFactor::kOne || dst_a != BlendFactor::kZero || op_a != 0) {
        rt.blend = 1;
        rt.src = uint8_t(src);
        rt.dst = uint8_t(dst);
        rt.op = uint8_t(op);
        rt.src_a = uint8_t(src_a);
        rt.dst_a = uint8_t(dst_a);
        rt.op_a = uint8_t(op_a);
      } else {
        rt.src = uint8_t(BlendFactor::kOne);
        rt.src_a = uint8_t(BlendFactor::kOne);
      }
    }
    if (key.num_rts || key.dsv != DXGI_FORMAT_UNKNOWN) key.samples = uint8_t(targets.samples);
  }
  if (debug_mask & kDebugNoDepth) {
    key.depth_enable = key.depth_write = key.stencil_enable = 0;
    key.dsv = DXGI_FORMAT_UNKNOWN;
    if (!key.num_rts) key.samples = 1;
  }
  if (debug_mask & kDebugNoCull) key.cull = uint8_t(D3D12_CULL_MODE_NONE);
  if (debug_mask & kDebugNoBlend)
    for (auto& rt : key.rt) rt.blend = 0;
  if (debug_mask & kDebugNoDepthBias) {
    key.depth_bias = 0;
    key.slope_scaled_depth_bias = 0;
  }
  return key;
}

void DrawRenderer::Impl::LogDraw(const DrawState& s, const RegisterFile& regs, uint32_t draw_index,
                                 uint32_t count, const DxbcShader& vs, const DxbcShader* ps,
                                 const ShaderModifications& mods, const PipelineKey& key,
                                 const ViewportInfo& vp, const Scissor& scissor) {
  std::fprintf(
      draw_log,
      "%u fn=%08X prim=%u n=%u idx=%u vs=%016llX:%llX ps=%016llX:%llX rt=%u:%d,%d dsv=%d "
      "z=%u/%u/%u st=%u cull=%u blend=%u:%u/%u/%u,%u/%u/%u mask=%X bc=%08X cc=%08X "
      "f=%.2f,%.2f,%.2f,%.2f samples=%u vp=%u,%u,%u,%u z=%.3f-%.3f sc=%u,%u,%u,%u "
      "tex=%zu ci=%08X si=%08X di=%08X su=%08X dc=%08X po=%g/%g,%g/%g bias=%d/%g\n",
      draw_index, uint32_t(s.fn), s.primitive, count, uint32_t(s.indexed),
      (unsigned long long)vs.ucode_data_hash(), (unsigned long long)mods.vertex,
      ps ? (unsigned long long)ps->ucode_data_hash() : 0ull, (unsigned long long)mods.pixel,
      key.num_rts, int(key.rtv[0]), int(key.rtv[1]), int(key.dsv), key.depth_enable,
      key.depth_write, key.depth_func, key.stencil_enable, key.cull, key.rt[0].blend, key.rt[0].src,
      key.rt[0].dst, key.rt[0].op, key.rt[0].src_a, key.rt[0].dst_a, key.rt[0].op_a, key.rt[0].mask,
      regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
      key.samples, vp.xy_offset[0], vp.xy_offset[1], vp.xy_extent[0], vp.xy_extent[1], vp.z_min,
      vp.z_max, scissor.offset[0], scissor.offset[1], scissor.extent[0], scissor.extent[1],
      ps ? ps->GetTextureBindingsAfterTranslation().size() : size_t(0),
      regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
      regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
      regs[XE_GPU_REG_RB_DEPTHCONTROL], regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
      regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
      regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
      regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET), key.depth_bias,
      key.slope_scaled_depth_bias);
  // Where the draw's data comes from and the float constants each shader reads (hashed).
  std::fprintf(draw_log, "  ia=%08X", s.indexed ? s.index_address : 0u);
  const Shader::ConstantRegisterMap& vmap = vs.constant_register_map();
  for (uint32_t w = 0; w < 3; ++w)
    for (uint32_t bits = vmap.vertex_fetch_bitmap[w]; bits; bits &= bits - 1) {
      const uint32_t index = w * 32 + uint32_t(std::countr_zero(bits));
      const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(index);
      std::fprintf(draw_log, " vf%u=%08X+%X", index, vf.address << 2, vf.size << 2);
    }
  // Each shader's ucode disassembly, once per log file.
  {
    static std::FILE* logged_for = nullptr;
    static std::unordered_set<uint64_t> logged;
    if (logged_for != draw_log) {
      logged_for = draw_log;
      logged.clear();
    }
    for (const Shader* sh : {static_cast<const Shader*>(&vs), static_cast<const Shader*>(ps)})
      if (sh && logged.insert(sh->ucode_data_hash()).second && !sh->ucode_disassembly().empty())
        std::fprintf(draw_log, "\n  ucode %016llX:\n%s", (unsigned long long)sh->ucode_data_hash(),
                     sh->ucode_disassembly().c_str());
  }
  // The vertex index offset (DrawVertices' start vertex, DrawIndexedVertices' base vertex) and,
  // from it, the first 64 bytes of each fetched vertex buffer (guest byte order), for vertex
  // format and position checks.
  const uint32_t indx_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
  std::fprintf(draw_log, " indx_offset=%u base_vertex=%u", indx_offset, s.base_vertex);
  for (uint32_t w = 0; w < 3; ++w)
    for (uint32_t bits = vmap.vertex_fetch_bitmap[w]; bits; bits &= bits - 1) {
      const uint32_t index = w * 32 + uint32_t(std::countr_zero(bits));
      const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(index);
      std::fprintf(draw_log, "\n  vdata%u (endian %u):", index, uint32_t(vf.endian));
      for (uint32_t i = 0; i < 32 && i < (vf.size << 2); i += 4) {
        uint32_t v;
        std::memcpy(&v, membase + (vf.address << 2) + i, 4);
        std::fprintf(draw_log, " %08X", __builtin_bswap32(v));
      }
      // At the offset, assuming the shader's stride is 32 bytes for the dump (the stride is in the
      // vfetch instruction, not the fetch constant); 64 bytes.
      std::fprintf(draw_log, "\n  vdata%u@offset*32:", index);
      for (uint32_t i = 0; i < 64; i += 4) {
        const uint64_t o = uint64_t(indx_offset) * 32 + i;
        if (o + 4 > (uint64_t(vf.size) << 2)) break;
        uint32_t v;
        std::memcpy(&v, membase + (vf.address << 2) + o, 4);
        std::fprintf(draw_log, " %08X", __builtin_bswap32(v));
      }
    }
  {
    std::fprintf(draw_log, "\n  vc");
    for (uint32_t w = 0; w < 4; ++w)
      for (uint64_t bits = vmap.float_bitmap[w]; bits; bits &= bits - 1) {
        const uint32_t c = w * 64 + uint32_t(std::countr_zero(bits));
        const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4;
        std::fprintf(draw_log, " %u=%g,%g,%g,%g", c, regs.Get<float>(r), regs.Get<float>(r + 1),
                     regs.Get<float>(r + 2), regs.Get<float>(r + 3));
      }
  }
  auto float_hash = [&](const Shader* shader, uint32_t base) {
    uint64_t h = 0;
    if (!shader) return h;
    const Shader::ConstantRegisterMap& map = shader->constant_register_map();
    for (uint32_t w = 0; w < 4; ++w)
      for (uint64_t bits = map.float_bitmap[w]; bits; bits &= bits - 1)
        h = XXH3_64bits_withSeed(&regs[base + (w * 64 + uint32_t(std::countr_zero(bits))) * 4], 16,
                                 h);
    return h;
  };
  std::fprintf(draw_log, " cvs=%016llX cps=%016llX\n",
               (unsigned long long)float_hash(&vs, XE_GPU_REG_SHADER_CONSTANT_000_X),
               (unsigned long long)float_hash(ps, XE_GPU_REG_SHADER_CONSTANT_256_X));
  if (ps) {
    std::fprintf(draw_log, "  pc");
    const Shader::ConstantRegisterMap& map = ps->constant_register_map();
    for (uint32_t w = 0; w < 4; ++w)
      for (uint64_t bits = map.float_bitmap[w]; bits; bits &= bits - 1) {
        const uint32_t c = w * 64 + uint32_t(std::countr_zero(bits));
        const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_256_X + c * 4;
        std::fprintf(draw_log, " %u=%g,%g,%g,%g", c, regs.Get<float>(r), regs.Get<float>(r + 1),
                     regs.Get<float>(r + 2), regs.Get<float>(r + 3));
      }
    std::fputc('\n', draw_log);
  }
}

ID3D12PipelineState* DrawRenderer::Impl::SelectPipeline(
    const PipelineKey& key, const DxbcShader::Translation& vst, const DxbcShader::Translation* pst,
    DxbcShader*& vs, DxbcShader*& ps, DxbcShader* vs_generic, DxbcShader* ps_generic,
    uint64_t pixel_mod, uint64_t generic_pixel_mod, const ShaderDrawRegs& sdr,
    xenos::PrimitiveType prim) {
  const uint64_t key_hash = XXH3_64bits(&key, sizeof(key));
  PsoMemo& pso_slot = pso_memo[key_hash & 1023];
  if (pso_slot.pso && pso_slot.hash == key_hash) {
    ++diag.pso_hits;
    return pso_slot.pso;
  }
  const uint32_t variant = (vs != vs_generic ? 1u : 0u) | (ps && ps != ps_generic ? 2u : 0u) |
                           (pst && pixel_mod != generic_pixel_mod ? 4u : 0u);
  const auto found = pipelines.find(key_hash);
  Pipeline* p = found != pipelines.end() ? found->second.get() : nullptr;
  // A new specialization (bool constants, texture signs) of shaders and state whose
  // unspecialized pipeline is ready compiles in the background while its draws use that
  // pipeline: the same image for a little more GPU time. A race keeps bringing new
  // combinations of known shaders for minutes, and each would otherwise hold a frame for its
  // compile. The first draw of the shaders and state waits for its specialization, which has
  // less code and compiles faster, and queues the unspecialized pipeline behind it.
  Pipeline* use = nullptr;
  DxbcShader* generic_ps = nullptr;
  if (variant && generic_pipelines &&
      (!p || (p->behind_generic && p->state.load(std::memory_order_acquire) == kCompiling))) {
    const ShaderModifications gm =
        SelectModifications(shaders.translator(), *vs_generic, ps_generic, sdr);
    DxbcShader* gps = gm.pixel_shader_used ? ps_generic : nullptr;
    // The draw's setup so far read the specialized pixel shader's depth output.
    if (gm.draw && !gps == !ps && (!gps || gps->writes_depth() == ps->writes_depth())) {
      DxbcShader::Translation* gvst = Translated(*vs_generic, gm.vertex);
      DxbcShader::Translation* gpst = gps ? Translated(*gps, gm.pixel) : nullptr;
      if (gvst && (!gps || gpst)) {
        PipelineKey gkey = key;
        gkey.vs = gvst;
        gkey.ps = gpst;
        gkey.gs_key = GeometryShaderKeyFor(prim, gm.vertex, gpst ? gm.pixel : 0);
        const uint64_t ghash = XXH3_64bits(&gkey, sizeof(gkey));
        const auto gf = pipelines.find(ghash);
        if (gf == pipelines.end()) {
          GetPipeline(ghash, gkey, *gvst, gpst, 0, /*background=*/true);
        } else if (gf->second->state.load(std::memory_order_acquire) == kReady) {
          use = gf->second.get();
          generic_ps = gps;
          if (!p) {
            p = GetPipeline(key_hash, key, vst, pst, variant, /*background=*/true);
            p->behind_generic = true;
          }
        }
      }
    }
  }
  if (!use) use = p ? p : GetPipeline(key_hash, key, vst, pst, variant);
  const uint8_t state = use->state.load(std::memory_order_acquire);
  if (state == kCompiling) {
    if (use->prewarm) PromotePipeline(use);
    if (use->pending_frame != r.frame_index()) {
      use->pending_frame = r.frame_index();
      frame_pending.push_back(use);
    }
  }
  if (state != kReady) {
    if (state == kCompiling) {
      ++stats.pso_pending;
    } else {
      ++stats.failed;
    }
    return nullptr;
  }
  ID3D12PipelineState* const pso = use->pso.Get();
  if (use == p) {
    pso_slot = {key_hash, pso};
  } else {
    // Bindings and constants follow the shaders the pipeline was built from.
    vs = vs_generic;
    ps = generic_ps;
    ++stats.pso_generic;
  }
  return pso;
}

uint32_t DrawRenderer::Impl::BindTextures(const TextureBindContext& ctx, const DxbcShader& shader,
                                          uint32_t* indices) {
  const DrawState& s = ctx.s;
  const RegisterFile& regs = ctx.regs;
  TextureCache& texture_cache = ctx.texture_cache;
  const uint64_t frame_fence = ctx.frame_fence;
  uint8_t* const signs = ctx.signs;
  const auto& tex = shader.GetTextureBindingsAfterTranslation();
  const auto& smp = shader.GetSamplerBindingsAfterTranslation();
  ShaderSlots& ss = shader_slots[(reinterpret_cast<uintptr_t>(&shader) >> 6) & 511];
  if (ss.shader != &shader) ss.Assign(shader);
  const uint32_t n = ss.n;
  // The memo entry for (shader, the fetch constants it reads); null if not memoizable.
  BindMemo* bm = nullptr;
  const uint64_t epoch = texture_cache.change_epoch();
  // While the draw log records, every binding goes through Get so each one is logged.
  if (!draw_log && ss.count <= BindMemo::kMaxSlots && tex.size() <= BindMemo::kMaxBound) {
    auto same_fetch = [&](const BindMemo& e) {
      if (e.shader != &shader) return false;
      const uint32_t* stored = e.data;
      for (uint32_t m = ss.mask; m; m &= m - 1, stored += 6)
        if (std::memcmp(stored, s.fetch(uint32_t(std::countr_zero(m))), 24) != 0) return false;
      return true;
    };
    profile::Counter miss_kind = profile::kBindMissEmpty;
    bool found = false;
    if (ss.last && same_fetch(*ss.last)) {
      bm = ss.last;
      found = true;
      profile::Count(profile::kBindMru);
    } else {
      uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(&shader)) * 0x9E3779B97F4A7C15ull;
      for (uint32_t m = ss.mask; m; m &= m - 1) {
        const uint32_t* f = s.fetch(uint32_t(std::countr_zero(m)));
        uint64_t a, b, c;
        std::memcpy(&a, f, 8);
        std::memcpy(&b, f + 2, 8);
        std::memcpy(&c, f + 4, 8);
        h = (h ^ a ^ std::rotl(b, 21) ^ std::rotl(c, 42)) * 0x9E3779B97F4A7C15ull;
      }
      BindMemo* set = &bind_memo[size_t(h >> (64 - kBindMemoSetBits)) << 1];
      for (uint32_t way = 0; way < 2 && !found; ++way) {
        if (same_fetch(set[way])) {
          bm = &set[way];
          found = true;
        } else if (set[way].shader == &shader) {
          miss_kind = profile::kBindMissFetch;
        }
      }
      // Not found: fill the least recently used way.
      if (!found) bm = set[0].used <= set[1].used ? &set[0] : &set[1];
      bm->used = ++bind_clock;
      ss.last = bm;
    }
    if (found) {
      bool same = true;
      if (bm->epoch != epoch) {
        for (uint32_t i = 0; i < bm->bound && same; ++i)
          same = texture_cache.Unchanged(bm->entry[i]);
        if (same) bm->epoch = epoch;
      }
      if (same) {
        if (bm->touched != frame_fence) {
          for (uint32_t i = 0; i < bm->bound; ++i) texture_cache.Touch(bm->entry[i], frame_fence);
          bm->touched = frame_fence;
        }
        const uint32_t* stored = bm->data + ss.count * 6;
        std::memcpy(indices, stored, n * sizeof(uint32_t));
        stored += n;
        for (uint32_t i = 0; i < bm->bound; ++i) signs[stored[i] & 0xFF] = uint8_t(stored[i] >> 8);
        stats.textures += bm->textures;
        ++stats.bind_memo_hits;
        profile::Count(profile::kBindHit);
        return n;
      }
      miss_kind = profile::kBindMissTexture;
    }
    profile::Count(miss_kind);
  }
  profile::Scope miss_scope(profile::kTexMiss);
  ++stats.bind_memo_misses;
  std::fill(indices, indices + n, 0u);
  uint32_t bound = 0, textures = 0, missing = 0, stale = 0;
  for (const auto& tb : tex) {
    ++textures;
    TextureBinding b;
    uint32_t index;
    const uint32_t* fetch = s.fetch(tb.fetch_constant);
    bool got;
    {
      profile::Scope get_scope(profile::kTexGet);
      got = texture_cache.Get(fetch, ctx.list, frame_fence, &b, ctx.captured) && b.resource;
    }
    if (draw_log)
      std::fprintf(draw_log,
                   "  tf%u %08X fmt=%u %ux%ux%u dim=%u tiled=%u mips=%u%s%s signed=%u fetch=%08X "
                   "%08X %08X %08X %08X %08X\n",
                   tb.fetch_constant, b.key.base_page << 12, uint32_t(b.key.format), b.key.width,
                   b.key.height, b.key.depth_or_array_size, uint32_t(b.key.dimension),
                   uint32_t(b.key.tiled), b.key.mip_levels(), got ? "" : " missing",
                   got && b.aliased ? " alias" : "", uint32_t(tb.is_signed), fetch[0], fetch[1],
                   fetch[2], fetch[3], fetch[4], fetch[5]);
    if (got) {
      profile::Scope srv_scope(profile::kTexSrv);
      stale += b.stale;
      const uint32_t mapping = b.srv.Shader4ComponentMapping;
      const uint64_t sh =
          (uint64_t(reinterpret_cast<uintptr_t>(b.resource)) ^ (uint64_t(mapping) << 32) ^
           (uint64_t(tb.is_signed) << 3) ^ (uint64_t(tb.dimension) << 4)) *
          0x9E3779B97F4A7C15ull;
      SrvMemo& m = srv_memo[sh >> (64 - kSrvMemoBits)];
      if (m.resource == b.resource && m.mapping == mapping &&
          m.is_signed == uint8_t(tb.is_signed) && m.dimension == uint8_t(tb.dimension)) {
        index = m.index;
        profile::Count(profile::kSrvMemoHit);
      } else {
        index = TextureDescriptor(b, tb);
        m.resource = b.resource;
        m.mapping = mapping;
        m.is_signed = uint8_t(tb.is_signed);
        m.dimension = uint8_t(tb.dimension);
        m.index = index;
      }
      signs[tb.fetch_constant] = b.swizzled_signs;
      if (bm) {
        bm->entry[bound] = b.entry;
        bm->data[ss.count * 6 + n + bound] =
            uint32_t(uint8_t(tb.fetch_constant)) | uint32_t(b.swizzled_signs) << 8;
      }
      ++bound;
    } else {
      ++missing;
      index = NullView(tb.dimension);
    }
    if (tb.bindless_descriptor_index < 64) indices[tb.bindless_descriptor_index] = index;
  }
  for (const auto& sb : smp) {
    const uint32_t* fetch = s.fetch(sb.fetch_constant);
    const uint8_t filters[4] = {uint8_t(sb.mag_filter), uint8_t(sb.min_filter),
                                uint8_t(sb.mip_filter), uint8_t(sb.aniso_filter)};
    uint32_t sh = 0;
    for (int i = 0; i < 6; ++i) sh = (sh ^ fetch[i]) * 0x9E3779B1u;
    uint32_t fw;
    std::memcpy(&fw, filters, 4);
    sh = (sh ^ fw) * 0x9E3779B1u;
    SamplerMemo& m = sampler_memo[sh >> (32 - kSamplerMemoBits)];
    uint32_t index;
    if (std::memcmp(m.filters, filters, 4) == 0 &&
        std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0) {
      index = m.index;
    } else {
      index = SamplerDescriptor(GetSamplerKey(regs, sb, min_aniso));
      std::memcpy(m.filters, filters, 4);
      std::memcpy(m.fetch, fetch, sizeof(m.fetch));
      m.index = index;
    }
    if (sb.bindless_descriptor_index < 64) indices[sb.bindless_descriptor_index] = index;
  }
  stats.textures += textures;
  stats.textures_missing += missing;
  stats.textures_stale += stale;
  if (bm && !missing && !stale) {
    bm->shader = &shader;
    bm->epoch = epoch;
    bm->touched = frame_fence;  // Get marked them
    uint32_t* stored = bm->data;
    for (uint32_t m = ss.mask; m; m &= m - 1, stored += 6)
      std::memcpy(stored, s.fetch(uint32_t(std::countr_zero(m))), 24);
    std::memcpy(stored, indices, n * sizeof(uint32_t));
    bm->bound = bound;
    bm->textures = textures;
  } else {
    if (bm) bm->shader = nullptr;
    profile::Count(profile::kBindNoMemo);
  }
  return n;
}

// UpdateSystemConstantValues, host render targets, no ROV.
void DrawRenderer::Impl::FillSystemConstants(const DrawState& s, const RegisterFile& regs,
                                             xenos::PrimitiveType prim, bool polygonal,
                                             xenos::Endian index_endian, const ViewportInfo& vp,
                                             uint32_t used_textures, const uint8_t* signs,
                                             DxbcShaderTranslator::SystemConstants& sys) {
  std::memset(&sys, 0, sizeof(sys));
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t flags = 0;
  if (vte.vtx_xy_fmt) flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (rex::graphics::draw_util::IsPrimitiveLine(false, prim))
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  // No kSysFlag_DepthFloat24: without float24 conversion in the shader, its only effect is to
  // scale oDepth into the plugin's host 0...0.5 float24 range, while viewports, clears and
  // resolves here keep D24FS8 in 0...1 (HostViewportInfo). Setting it halves the depth that
  // depth-restoring passes write.
  const xenos::CompareFunction alpha_func =
      colorcontrol.alpha_test_enable && !(debug_mask & kDebugNoAlphaTest)
          ? colorcontrol.alpha_func
          : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma render targets are unorm16 on the host (the plugin's setting): no conversion flag.
  sys.flags = flags;
  sys.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sys.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  sys.vertex_index_endian = index_endian;
  sys.vertex_index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  sys.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  sys.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  if (!clip.clip_disable) {
    float* ucp = sys.user_clip_planes[0];
    uint32_t remaining = clip.ucp_ena;
    while (remaining) {
      uint32_t i = uint32_t(std::countr_zero(remaining));
      remaining &= remaining - 1;
      std::memcpy(ucp, &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 4 * sizeof(float));
      ucp += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sys.ndc_scale[i] = vp.ndc_scale[i];
    sys.ndc_offset[i] = vp.ndc_offset[i];
  }
  if (prim == xenos::PrimitiveType::kPointList) {
    auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sys.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sys.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sys.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sys.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sys.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(std::max(vp.xy_extent[0], 1u));
    sys.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(std::max(vp.xy_extent[1], 1u));
  }
  uint32_t remaining = used_textures;
  while (remaining) {
    uint32_t i = uint32_t(std::countr_zero(remaining));
    remaining &= remaining - 1;
    sys.texture_swizzled_signs[i >> 2] |= uint32_t(signs[i]) << ((i & 3) * 8);
    if (profile::On()) {
      const uint32_t* f = s.fetch(i);
      const uint8_t sg = signs[i];
      ++diag.fetch_classes[sg == 0 ? 0 : sg == 0x3F ? 1 : sg == 0xFF ? 2 : sg == 0x55 ? 3 : 4];
      if ((f[3] >> 13) & 0x3F) ++diag.fetch_classes[5];
      if ((f[4] >> 12) & 0x3FF) ++diag.fetch_classes[6];
    }
  }
  sys.sample_count_log2[0] = surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  sys.sample_count_log2[1] = surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  sys.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  sys.alpha_to_mask = colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1 << 8) : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    auto ci = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]);
    int32_t bias = ci.color_exp_bias;
    // snorm16 render targets use the full -32...32 range (the plugin's default).
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        ci.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)
      bias -= 5;
    uint32_t bits = uint32_t(0x3F800000 + (bias << 23));
    std::memcpy(&sys.color_exp_bias[i], &bits, 4);
  }
}

void DrawRenderer::Impl::Draw(const DrawState& s, const DrawPrep* prep) {
  ID3D12GraphicsCommandList* list = r.command_list();
  SyncListFrame();
  if (prep && prep->regs_hash[0]) {
    uint64_t now[5];
    HashRegisterBlocks(s.regs, now);
    if (std::memcmp(now, prep->regs_hash, sizeof(now))) {
      ++stats.reg_mismatches;
      GRAW_LOG_FIRST(100, WARN,
                     "GRAW native coherency: frame {} draw {} registers differ from Prepare's in "
                     "blocks{}{}{}{}{}",
                     r.frame_index(), frame_draws, now[0] != prep->regs_hash[0] ? " state" : "",
                     now[1] != prep->regs_hash[1] ? " vs-float" : "",
                     now[2] != prep->regs_hash[2] ? " ps-float" : "",
                     now[3] != prep->regs_hash[3] ? " fetch" : "",
                     now[4] != prep->regs_hash[4] ? " bool-loop" : "");
    }
  }
  if (prep) {
    // The guest thread copied the ranges (and marked their pages valid): they go into shared
    // memory whether or not the draw is issued.
    if (prep->copy_count) {
      ++stats.copy_batches;
      TransitionShared(list, D3D12_RESOURCE_STATE_COPY_DEST);
      for (uint32_t i = 0; i < prep->copy_count; ++i) {
        const VertexCopy& c = prep->copies[i];
        list->CopyBufferRegion(shared.Get(), c.dst, static_cast<ID3D12Resource*>(c.src), c.src_offset,
                             c.size);
      }
    }
    if (prep->audit_count) AuditDraw(list, *prep);
    if (!prep->upload_ok) {
      ++stats.failed;
      return;
    }
  }
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(s.regs);
  const xenos::PrimitiveType prim = xenos::PrimitiveType(s.primitive);

  // Shaders.
  if (!s.vs.ucode || !s.vs.dwords) {
    ++stats.skipped;
    return;
  }
  DxbcShader* vs = prep ? static_cast<DxbcShader*>(prep->vs) : LoadShader(vs_memo, vs_last, s.vs, xenos::ShaderType::kVertex);
  DxbcShader* ps = prep ? static_cast<DxbcShader*>(prep->ps) : LoadShader(ps_memo, ps_last, s.ps, xenos::ShaderType::kPixel);
  if (!vs) {
    ++stats.failed;
    return;
  }
  DxbcShader* const vs_generic = vs;
  DxbcShader* const ps_generic = ps;
  if (bool_specialize) {
    const uint32_t* bools = &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
    vs = vs_bools.Get(vs, bools);
    ps = ps_bools.Get(ps, bools);
  }

  // SelectModifications reads only prim_type; the index count would defeat mods_memo.
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = 0;
  initiator.prim_type = prim;

  ShaderDrawRegs sdr;
  sdr.sq_program_cntl = regs[XE_GPU_REG_SQ_PROGRAM_CNTL];
  sdr.sq_context_misc = regs[XE_GPU_REG_SQ_CONTEXT_MISC];
  sdr.sq_interpolator_cntl = regs[XE_GPU_REG_SQ_INTERPOLATOR_CNTL];
  sdr.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  sdr.vgt_draw_initiator = initiator.value;
  sdr.vgt_output_path_cntl = regs[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL];
  sdr.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  sdr.rb_modecontrol = regs[XE_GPU_REG_RB_MODECONTROL];
  sdr.rb_surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  sdr.rb_colorcontrol = regs[XE_GPU_REG_RB_COLORCONTROL];
  sdr.rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < 4; ++i) sdr.rb_color_info[i] = regs[kColorInfoRegs[i]];
  if (mods_memo.vs == vs && mods_memo.ps == ps &&
      std::memcmp(&mods_memo.sdr, &sdr, sizeof(sdr)) == 0) {
    ++diag.mods_hits;
  } else {
    mods_memo.vs = vs;
    mods_memo.ps = ps;
    mods_memo.sdr = sdr;
    mods_memo.mods = SelectModifications(shaders.translator(), *vs, ps, sdr);
    mods_memo.ps_textures = 0;
    if (mods_memo.mods.pixel_shader_used)
      for (const auto& tb : ps->texture_bindings())
        mods_memo.ps_textures |= 1u << tb.fetch_constant;
  }
  ShaderModifications mods = mods_memo.mods;
  const uint64_t generic_pixel_mod = mods.pixel;
  mods.pixel = SpecializePixelTextures(mods.pixel, s.fetch(0), mods_memo.ps_textures,
                                       signed_is_unsigned_formats);

  if (!mods.draw) {
    ++stats.skipped;
    return;
  }
  if (!mods.pixel_shader_used) ps = nullptr;
  if (vs->memexport_eM_written() || (ps && ps->memexport_eM_written())) {
    GRAW_WARN_ONCE("GRAW native draw: memexport unsupported (draw skipped)");
    ++stats.unsupported;
    return;
  }
  const bool tess = xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
                    regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
                        xenos::VGTOutputPath::kTessellationEnable;
  if (tess) {
    GRAW_WARN_ONCE("GRAW native draw: tessellation unsupported");
    ++stats.unsupported;
    return;
  }
  profile::Mark(profile::kShaders);
  const bool polygonal = rex::graphics::draw_util::IsPrimitivePolygonal(false, prim);
  const bool rasterization = IsRasterizationPotentiallyDone(regs, polygonal);

  // Host primitive (the plugin's primitive processor on D3D12: points, rectangles and quads go
  // through a geometry shader, triangle fans through converted indices).
  D3D_PRIMITIVE_TOPOLOGY topology;
  switch (prim) {
    case xenos::PrimitiveType::kPointList:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
      break;
    case xenos::PrimitiveType::kTriangleFan:
      // As a triangle list with host indices (v1, v2, v0), (v2, v3, v0)... like the plugin's
      // primitive processor; guest indices are copied raw (the VS still swaps them).
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    default:
      GRAW_WARN_ONCE("GRAW native draw: primitive type {} unsupported", s.primitive);
      ++stats.unsupported;
      return;
  }
  uint32_t count = s.count;
  if (!count) {
    ++stats.skipped;
    return;
  }

  // Index buffer (kGuestDMA) and primitive reset.
  xenos::Endian index_endian = xenos::Endian::kNone;
  uint8_t strip_cut = 0;
  if (s.indexed) {
    index_endian = xenos::Endian(s.index_endian);
    if (!s.index_32bit && index_endian != xenos::Endian::kNone &&
        index_endian != xenos::Endian::k8in16)
      index_endian =
          index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
        (prim == xenos::PrimitiveType::kLineStrip ||
         prim == xenos::PrimitiveType::kTriangleStrip)) {
      uint32_t reset =
          xenos::GpuSwap(regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx, index_endian);
      if (!s.index_32bit) {
        if (reset <= 0xFFFF) {
          if (reset != 0xFFFF) {
            GRAW_WARN_ONCE("GRAW native draw: primitive reset index {:X} unsupported", reset);
            ++stats.unsupported;
            return;
          }
          strip_cut = 1;
        }
      } else {
        // The host compares all 32 bits; the guest only the low 24. Fine when the guest uses
        // 0xFFFFFFFF (or the index never appears).
        strip_cut = 2;
      }
    }
    if (!s.index_address) {
      ++stats.failed;
      return;
    }
  }

  // Render targets.
  const reg::RB_DEPTHCONTROL depth_control = GetNormalizedDepthControl(regs);
  const uint32_t color_mask = ps ? GetNormalizedColorMask(regs, ps->writes_color_targets()) : 0;
  const bool depth_used = rasterization && (depth_control.z_enable || depth_control.stencil_enable);
  uint32_t rt_mask = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if ((color_mask >> (4 * i)) & 0xF) rt_mask |= 1u << i;
  if (!rt_mask && !depth_used) {
    ++stats.skipped;  // nothing to write (no occlusion query emulation yet)
    return;
  }
  Scissor scissor;
  HostScissor(regs, scissor);
  ViewportInfo vp;
  HostViewportInfo(regs, ps && ps->writes_depth(), vp);
  const uint32_t height = std::clamp(scissor.offset[1] + scissor.extent[1], 1u, 2048u);
  profile::Mark(profile::kSetup);
  BoundTargets targets;
  if (!r.render_targets().Bind(list, SurfaceRegsFromRegisters(s.regs), rt_mask, depth_used, height,
                               &targets)) {
    GRAW_WARN_ONCE("GRAW native draw: RenderTargets::Bind failed (surface {:08X})",
                   regs[XE_GPU_REG_RB_SURFACE_INFO]);
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kRtBind);

  // Translations.
  DxbcShader::Translation* vst = Translated(*vs, mods.vertex);
  DxbcShader::Translation* pst = ps ? Translated(*ps, mods.pixel) : nullptr;
  if (!vst || (ps && !pst)) {
    GRAW_WARN_ONCE("GRAW native draw: shader translation failed");
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kTranslate);

  const auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  PipelineKey key = BuildPipelineKey(regs, prim, mods, vst, pst, strip_cut, rasterization,
                                     polygonal, sc, depth_control, color_mask, targets);
  const uint32_t draw_index = frame_draws++;
  bool debug_skip = (debug_max_draws && draw_index >= debug_max_draws) ||
                    (draw_index >= debug_skip_from && draw_index < debug_skip_to);
  uint32_t snap_now = 0;
  if (debug_hash_count) {
    const uint64_t vs_hash = vs->ucode_data_hash(), ps_hash = ps ? ps->ucode_data_hash() : 0;
    uint32_t listed = 0;
    for (uint32_t i = 0; i < debug_hash_count; ++i)
      if (debug_hashes[i] == vs_hash || (ps && debug_hashes[i] == ps_hash)) listed |= 1u << i;
    if (debug_mask & kDebugSnapshots)
      snap_now = listed & snap_pending;
    else
      debug_skip |= bool(listed) != bool(debug_mask & kDebugOnlyListed);
  } else if ((snap_pending >> 31) && draw_index == debug_skip_from) {
    snap_now = 1u << 31;
  }
  if (debug_skip) {
    ++stats.skipped;
    return;
  }
  if (draw_log) LogDraw(s, regs, draw_index, count, *vs, ps, mods, key, vp, scissor);
  // With dynamic depth bias the polygon offset is not part of the pipeline.
  const float depth_bias = float(key.depth_bias);
  const float slope_bias = key.slope_scaled_depth_bias;
  if (dynamic_depth_bias) {
    key.depth_bias = 0;
    key.slope_scaled_depth_bias = 0;
  }
  const DxbcShader* const vs_selected = vs;
  ID3D12PipelineState* const pso = SelectPipeline(key, *vst, pst, vs, ps, vs_generic, ps_generic,
                                                  mods.pixel, generic_pixel_mod, sdr, prim);
  if (draw_log)
    std::fprintf(draw_log, "  pso=%s\n",
                 !pso                ? "pending"
                 : vs != vs_selected ? "generic"
                                     : "ready");
  if (!pso) return;
  profile::Mark(profile::kPipeline);

  // Textures and samplers (bindless descriptor indices, b4).
  const uint32_t used_textures = vs->GetUsedTextureMaskAfterTranslation() |
                                 (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  uint32_t desc_vs[64], desc_ps[64];
  uint8_t signs[32] = {};
  const TextureBindContext bind = {s,
                                   regs,
                                   list,
                                   r.textures(),
                                   r.frame_fence(),
                                   prep ? prep->captured : TextureCache::kCapturedNow,
                                   signs};
  const uint32_t n_desc_vs = BindTextures(bind, *vs, desc_vs);
  const uint32_t n_desc_ps = ps ? BindTextures(bind, *ps, desc_ps) : 1;
  if (!ps) desc_ps[0] = 0;
  profile::Mark(profile::kTextures);

  DxbcShaderTranslator::SystemConstants sys;
  FillSystemConstants(s, regs, prim, polygonal, index_endian, vp, used_textures, signs, sys);
  profile::Mark(profile::kSysConst);

  // Float constants packed by the shaders' maps (UpdateBindings), compared and staged in place.
  PackFloats(cb_float_vs, vs, s.regs, XE_GPU_REG_SHADER_CONSTANT_000_X);
  PackFloats(cb_float_ps, ps, s.regs, XE_GPU_REG_SHADER_CONSTANT_256_X);
  profile::Mark(profile::kPack);

  // Vertex data and indices into shared memory (made by Prepare with `prep`).
  const uint32_t index_bytes = count * (s.index_32bit ? 4u : 2u);
  const bool fan = prim == xenos::PrimitiveType::kTriangleFan;
  if (!prep) {
    mirror.upload_fence = r.frame_fence();
    if (!RequestVertices(s, *vs, copies)) {
      // The copies made before the failed range go out: their pages are marked valid already.
      FlushCopies(list);
      ++stats.failed;
      return;
    }
  }
  FlushCopies(list);
  profile::Mark(profile::kVertices);

  // Fixed-function state.
  D3D12_VIEWPORT viewport = {float(vp.xy_offset[0]),
                             float(vp.xy_offset[1]),
                             float(vp.xy_extent[0]),
                             float(vp.xy_extent[1]),
                             vp.z_min,
                             vp.z_max};
  D3D12_RECT scissor_rect = {LONG(scissor.offset[0]), LONG(scissor.offset[1]),
                             LONG(scissor.offset[0] + scissor.extent[0]),
                             LONG(scissor.offset[1] + scissor.extent[1])};

  // Root state.
  if (list_dirty) {
    ID3D12DescriptorHeap* heaps[] = {view_heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_SharedMemory, view_gpu);
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_SamplerHeap, sampler_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE table = view_gpu;
    table.ptr += uint64_t(kViewTableStart) * view_stride;
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_ViewHeap, table);
    list_dirty = false;
  }
  static_assert(sizeof(sys) <= CachedCbv::kMaxSize);
  bool ok =
      UploadCbv(cb_system, &sys, sizeof(sys), kBindlessRoot_SystemConstants, list) &&
      CommitCbv(cb_float_vs, kBindlessRoot_FloatConstantsVertex, list) &&
      CommitCbv(cb_float_ps, kBindlessRoot_FloatConstantsPixel, list) &&
      UploadCbv(cb_bool_loop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], (8 + 32) * 4,
                kBindlessRoot_BoolLoopConstants, list) &&
      UploadCbv(cb_fetch, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 32 * 6 * 4,
                kBindlessRoot_FetchConstants, list) &&
      UploadCbv(cb_desc_vs, desc_vs, n_desc_vs * 4, kBindlessRoot_DescriptorIndicesVertex, list) &&
      UploadCbv(cb_desc_ps, desc_ps, n_desc_ps * 4, kBindlessRoot_DescriptorIndicesPixel, list);
  if (!ok) {
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kCbv);
  if (bound_pso != pso) {
    list->SetPipelineState(pso);
    bound_pso = pso;
    // SetPipelineState loads the pipeline's depth bias, dynamic or not (DirectX-Specs
    // VulkanOn12, "initial dynamic depth bias state"). These pipelines are keyed with zero bias.
    if (dynamic_depth_bias) bound_depth_bias = bound_slope_bias = 0.0f;
  }
  if (bound_topology != topology) {
    list->IASetPrimitiveTopology(topology);
    bound_topology = topology;
  }
  if (std::memcmp(&bound_viewport, &viewport, sizeof(viewport))) {
    list->RSSetViewports(1, &viewport);
    bound_viewport = viewport;
  }
  if (std::memcmp(&bound_scissor, &scissor_rect, sizeof(scissor_rect))) {
    list->RSSetScissorRects(1, &scissor_rect);
    bound_scissor = scissor_rect;
  }
  float blend[4] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (std::memcmp(bound_blend, blend, sizeof(blend))) {
    list->OMSetBlendFactor(blend);
    std::memcpy(bound_blend, blend, sizeof(blend));
  }
  {
    const bool bf = polygonal && depth_control.backface_enable && sc.cull_front && !sc.cull_back;
    uint32_t ref = regs.Get<reg::RB_STENCILREFMASK>(bf ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                                       : XE_GPU_REG_RB_STENCILREFMASK)
                       .stencilref;
    if (ref != bound_stencil_ref) {
      list->OMSetStencilRef(ref);
      bound_stencil_ref = ref;
    }
  }
  if (dynamic_depth_bias && (depth_bias != bound_depth_bias || slope_bias != bound_slope_bias)) {
    if (list9_of != list) {
      list9.Reset();
      list->QueryInterface(IID_PPV_ARGS(&list9));
      list9_of = list;
    }
    if (list9) list9->RSSetDepthBias(depth_bias, 0.0f, slope_bias);
    bound_depth_bias = depth_bias;
    bound_slope_bias = slope_bias;
  }
  // Render targets: only the ones the pipeline declares, the DSV only when it is used.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[4] = {};
  for (uint32_t i = 0; i < key.num_rts; ++i) rtv[i] = targets.rtv[i];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  if (key.dsv != DXGI_FORMAT_UNKNOWN) dsv = targets.dsv;
  if (bound_rt_count != key.num_rts || std::memcmp(bound_rtv, rtv, sizeof(rtv)) ||
      bound_dsv.ptr != dsv.ptr) {
    list->OMSetRenderTargets(key.num_rts, key.num_rts ? rtv : nullptr, FALSE,
                             dsv.ptr ? &dsv : nullptr);
    std::memcpy(bound_rtv, rtv, sizeof(rtv));
    bound_dsv = dsv;
    bound_rt_count = key.num_rts;
  }

  if (fan) {
    if (count < 3) {
      ++stats.skipped;
      return;
    }
    const FanIndices f = prep ? FanIndices{prep->fan_indices, prep->fan_count, prep->fan_wide}
                              : UploadFanIndices(s, r.frame_fence());
    if (!f.count) {
      ++stats.failed;
      return;
    }
    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = f.gpu;
    ibv.SizeInBytes = f.count * (f.wide ? 4 : 2);
    ibv.Format = f.wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    list->IASetIndexBuffer(&ibv);
    bound_ibv = ibv;
    list->DrawIndexedInstanced(f.count, 1, 0, 0, 0);
  } else if (s.indexed) {
    // The whole shared memory buffer is the index buffer and the draw starts at the guest
    // address, so consecutive draws only rebind it when the index format changes.
    const uint32_t index_size = s.index_32bit ? 4u : 2u;
    const bool aligned = (s.index_address & (index_size - 1)) == 0;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = aligned ? shared_gpu : shared_gpu + s.index_address;
    ibv.SizeInBytes = aligned ? kSharedMemorySize : index_bytes;
    ibv.Format = s.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (std::memcmp(&bound_ibv, &ibv, sizeof(ibv))) {
      list->IASetIndexBuffer(&ibv);
      bound_ibv = ibv;
    }
    list->DrawIndexedInstanced(count, 1, aligned ? s.index_address / index_size : 0, 0, 0);
  } else {
    list->DrawInstanced(count, 1, 0, 0);
  }
  ++stats.draws;
  ++diag.draws;
  if (snap_now) {
    snap_pending &= ~snap_now;
    SnapshotTargets(list, targets, draw_index);
  }
  profile::Mark(profile::kRecord);
}

void DrawRenderer::Impl::SnapshotTargets(ID3D12GraphicsCommandList* list,
                                         const BoundTargets& targets, uint32_t draw_index) {
  const std::string stem =
      (std::filesystem::path(REXCVAR_GET(graw_native_capture_dir)) / "snap-f").string() +
      std::to_string(r.frame_index()) + "-d" + std::to_string(draw_index);
  for (uint32_t i = 0; i < targets.color_count; ++i)
    Snapshot(list, targets.color[i], stem + "-c" + std::to_string(i));
  Snapshot(list, targets.depth, stem + "-depth");
  snap_fence = r.frame_fence();
  REXLOG_INFO("GRAW native draw: snapshot of draw {} ({} targets) to {}-*", draw_index,
              snaps.size(), stem);
  ResetListState();  // the barriers moved the targets out of their bound states
}

void DrawRenderer::Impl::Snapshot(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                                  const std::string& stem) {
  if (!rt || rt->samples != 1) return;
  const D3D12_RESOURCE_DESC desc = rt->resource->GetDesc();
  Snap s;
  s.stem = stem;
  uint64_t total = 0;
  // Subresource 0: the color target, or a depth target's depth plane.
  device->GetCopyableFootprints(&desc, 0, 1, 0, &s.layout, &s.rows, &s.row_bytes, &total);
  if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST,
                    "snapshot readback", &s.readback))
    return;
  const D3D12_RESOURCE_STATES state = rt->state;
  RenderTargets::Transition(list, rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = rt->resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dst.pResource = s.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = s.layout;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  RenderTargets::Transition(list, rt, state);
  snaps.push_back(std::move(s));
}

void DrawRenderer::Impl::WriteSnapshots(uint64_t completed_fence) {
  if (snaps.empty() || completed_fence < snap_fence) return;
  for (Snap& s : snaps) {
    void* data = nullptr;
    const D3D12_RANGE range = {0, size_t(s.layout.Footprint.RowPitch) * s.rows};
    if (FAILED(s.readback->Map(0, &range, &data))) continue;
    if (std::FILE* f = std::fopen((s.stem + ".bin").c_str(), "wb")) {
      for (uint32_t y = 0; y < s.rows; ++y)
        std::fwrite(static_cast<const uint8_t*>(data) + size_t(y) * s.layout.Footprint.RowPitch, 1,
                    size_t(s.row_bytes), f);
      std::fclose(f);
    }
    const D3D12_RANGE none = {0, 0};
    s.readback->Unmap(0, &none);
    if (std::FILE* f = std::fopen((s.stem + ".txt").c_str(), "w")) {
      std::fprintf(f, "%u %u %llu %u\n", s.layout.Footprint.Width, s.rows,
                   (unsigned long long)s.row_bytes, uint32_t(s.layout.Footprint.Format));
      std::fclose(f);
    }
  }
  REXLOG_INFO("GRAW native draw: wrote {} snapshot targets", snaps.size());
  snaps.clear();
}

DrawRenderer::DrawRenderer(D3D12Backend& renderer)
    : renderer_(renderer), impl_(std::make_unique<Impl>(renderer, stats_)) {}
DrawRenderer::~DrawRenderer() = default;

bool DrawRenderer::Initialize() { return impl_->Initialize(); }

void DrawRenderer::OnDraw(const DrawState& state, const DrawPrep* prep) {
  impl_->Draw(state, prep);
}

void DrawRenderer::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                           std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) {
  impl_->Prepare(state, upload_tag, prep, copies, audit);
}

void DrawRenderer::StartAudit(uint32_t frames) { impl_->audit_left = frames; }

void DrawRenderer::LogFrames(uint32_t frames) {
  impl_->draw_log_left = std::max(impl_->draw_log_left, frames);
}

void DrawRenderer::BeginCopySegment() { impl_->mirror.NewSegment(); }

DrawRenderer::PrepareCounters DrawRenderer::EndFramePrepare(uint64_t done) {
  Impl& m = *impl_;
  PrepareCounters c;
  if (m.mirror.coherency) {
    m.mirror.CheckLateWrites();
    c.stale_pages = std::exchange(m.mirror.coherency->stale, 0);
    c.late_pages = std::exchange(m.mirror.coherency->late, 0);
    c.job = m.MakeCoherencyJob();
  }
  if (m.audit_left) {
    --m.audit_left;
    REXLOG_INFO(
        "GRAW native audit: frame {} queued: {} draws, {} ranges ({:.1f} MB) to read back{}, {} "
        "shaders differ from the guest's ucode",
        m.mirror.watch_frame.load(std::memory_order_relaxed), m.audit_draws, m.audit_ranges,
        double(m.audit_bytes) / (1 << 20),
        m.audit_over ? fmt::format(", {} not (read back full)", m.audit_over) : std::string(),
        m.audit_shaders);
    m.audit_seen.clear();
    m.audit_draws = m.audit_ranges = m.audit_bytes = m.audit_over = m.audit_shaders = 0;
  }
  m.mirror.watch_frame.fetch_add(1, std::memory_order_relaxed);
  m.vertex_upload.EndFrame(done);
  c.upload_bytes = std::exchange(m.prep_upload_bytes, 0);
  c.uploads = std::exchange(m.prep_uploads, 0);
  c.diag = std::exchange(m.prep_diag, {});
  return c;
}

void DrawRenderer::AddPrepareCounters(const PrepareCounters& c) {
  stats_.upload_bytes += c.upload_bytes;
  stats_.uploads += c.uploads;
  stats_.stale_pages += c.stale_pages;
  stats_.late_pages += c.late_pages;
  PrepareDiag& d = impl_->diag.prep;
  d.shader_loads += c.diag.shader_loads;
  d.shader_loads_immediate += c.diag.shader_loads_immediate;
  d.ranges += c.diag.ranges;
  d.range_pages += c.diag.range_pages;
  d.range_runs += c.diag.range_runs;
  d.unwatched += c.diag.unwatched;
  if (c.job) impl_->coherency_next = c.job;
}

void DrawRenderer::BeforeSubmit() {
  Impl& m = *impl_;
  if (m.audit_job) {
    m.audit_job->fence = m.r.frame_fence();
    m.audit_inflight.push_back(std::move(m.audit_job));
  }
  if (!m.coherency_next) return;
  std::shared_ptr<CoherencyJob> job = std::move(m.coherency_next);
  if (!m.coherency_readbacks.empty()) {
    job->readback = std::move(m.coherency_readbacks.back());
    m.coherency_readbacks.pop_back();
  } else {
    if (!CreateBuffer(m.device, D3D12_HEAP_TYPE_READBACK, Impl::kCoherencyReadback,
                      D3D12_RESOURCE_STATE_COPY_DEST, "coherency readback", &job->readback))
      return;
  }
  ID3D12GraphicsCommandList* list = m.r.command_list();
  m.SyncListFrame();  // a frame without draws
  m.TransitionShared(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
  uint64_t offset = 0;
  for (const auto& [start, size] : job->runs) {
    list->CopyBufferRegion(job->readback.Get(), offset, m.shared.Get(), start, size);
    offset += size;
  }
  job->fence = m.r.frame_fence();
  m.coherency_inflight.push_back(std::move(job));
}

void DrawRenderer::InvalidateListState(ListBreak why) {
  impl_->ResetListState();
  // Recording goes on in a new list, and the submitted one left shared memory in the common
  // state once it completed.
  const bool submit = why == ListBreak::kSubmit;
  if (submit) impl_->shared_state = D3D12_RESOURCE_STATE_COMMON;
  if (impl_->draw_log)
    std::fprintf(impl_->draw_log, "%s (after draw %u)\n", submit ? "submit" : "resolve",
                 impl_->frame_draws);
}

std::pair<uint64_t, uint64_t> DrawRenderer::ShaderHashes(const DrawPrep& prep) {
  return {prep.vs ? prep.vs->ucode_data_hash() : 0, prep.ps ? prep.ps->ucode_data_hash() : 0};
}

std::string DrawRenderer::ShaderDisassembly(uint64_t hash) const {
  const DxbcShader* shader = impl_->shaders.Find(hash);
  return shader ? shader->ucode_disassembly() : std::string();
}

void DrawRenderer::LogClear(const ClearCall& c) {
  std::FILE* f = impl_->draw_log;
  if (!f) return;
  std::fprintf(
      f,
      "clear (after draw %u) flags=%X colors=%X depth_bound=%d z=%g stencil=%u height=%u si=%08X "
      "ci0=%08X di=%08X rects",
      impl_->frame_draws, c.p.flags, c.bound_colors, int(c.depth_bound), c.p.depth,
      uint32_t(c.p.stencil), c.height, c.regs.surface_info, c.regs.color_info[0],
      c.regs.depth_info);
  for (const D3D12_RECT& r : c.p.rects)
    std::fprintf(f, " %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
  std::fprintf(f, " edram");
  for (const D3D12_RECT& r : c.p.edram_rects)
    std::fprintf(f, " %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
  std::fprintf(f, "\n");
}

void DrawRenderer::LogResolve(const ResolveParams& p) {
  std::FILE* f = impl_->draw_log;
  if (!f) return;
  std::fprintf(f,
               "resolve params (after draw %u) src=%u si=%08X ci0=%08X di=%08X rect=%d,%d-%d,%d "
               "dest=%08X fmt=%u %ux%u at %d,%d clear=%X z=%g stencil=%u edram=%d,%d\n",
               impl_->frame_draws, p.source, p.regs.surface_info, p.regs.color_info[0],
               p.regs.depth_info, p.x0, p.y0, p.x1, p.y1, p.dest_base, p.dest_format, p.dest_width,
               p.dest_height, p.dest_x, p.dest_y, p.clear_flags, p.clear_depth,
               uint32_t(p.clear_stencil), p.edram_x, p.edram_y);
}

bool DrawRenderer::FrameIncomplete() const {
  const Impl& m = *impl_;
  return m.pipeline_wait && !m.frame_pending.empty();
}

void DrawRenderer::EndFrame(uint64_t completed_fence) {
  impl_->WaitForPipelines();  // after the frame's submission: the GPU runs it meanwhile
  stats_.views =
      impl_->views_allocated - (kViewTableStart + kFirstTexture) - impl_->free_views.size();
  stats_.watch_faults = impl_->mirror.watch_faults.exchange(0, std::memory_order_relaxed);
  stats_.watch_arms = impl_->mirror.watch_arms.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_pages = impl_->mirror.watch_arm_pages.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_changed = impl_->mirror.watch_arm_changed.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_ns = impl_->mirror.watch_arm_ns.exchange(0, std::memory_order_relaxed);
  stats_.hash_pages = impl_->mirror.hash_pages.exchange(0, std::memory_order_relaxed);
  stats_.hash_copies = impl_->mirror.hash_copies.exchange(0, std::memory_order_relaxed);
  if (!impl_->threaded) impl_->mirror.watch_frame.fetch_add(1, std::memory_order_relaxed);
  if (impl_->mirror.trace.file) {
    {
      std::lock_guard<std::mutex> lock(impl_->mirror.trace.mutex);
      ++impl_->mirror.trace.frame;
    }
    if ((impl_->mirror.trace.frame & 255) == 0) impl_->mirror.trace.Flush();
  }
  if (!impl_->threaded) impl_->vertex_upload.EndFrame(completed_fence);
  impl_->constant_upload.EndFrame(completed_fence);
  if (!impl_->coherency_inflight.empty()) impl_->CheckCoherencyJobs(completed_fence);
  if (!impl_->audit_inflight.empty()) impl_->CheckAuditJobs(completed_fence);
  impl_->Prewarm();
  impl_->store.Flush();
  impl_->frame_draws = 0;
  impl_->WriteSnapshots(completed_fence);
  if (profile::On() && ++impl_->diag.frames >= 300) {
    Impl::Diag& d = impl_->diag;
    const double n = double(std::max<uint64_t>(d.draws, 1));
    auto per = [&](uint64_t v) { return double(v) / n; };
    REXLOG_INFO(
        "GRAW native draw diag: {} draws/frame; per draw: shader loads {:.3f} ({:.3f} immediate), "
        "mods hits {:.3f}, translation hits {:.3f}, pso hits {:.3f}, ranges {:.3f} (pages "
        "{:.1f}, copied runs {:.3f}), unwatched copies {:.3f}, copy flushes {:.3f}",
        d.draws / d.frames, per(d.prep.shader_loads), per(d.prep.shader_loads_immediate),
        per(d.mods_hits), per(d.translation_hits), per(d.pso_hits), per(d.prep.ranges),
        per(d.prep.range_pages), per(d.prep.range_runs), per(d.prep.unwatched), per(d.flushes));
    std::string cbv;
    for (uint32_t i = 0; i < 16; ++i) {
      if (!d.cbv_uploads[i]) continue;
      char part[64];
      std::snprintf(part, sizeof(part), " r%u=%.3f/%.0fB", i, per(d.cbv_uploads[i]),
                    double(d.cbv_bytes[i]) / double(d.cbv_uploads[i]));
      cbv += part;
    }
    REXLOG_INFO("GRAW native draw diag: cbv uploads per draw / bytes per upload:{}", cbv);
    const uint64_t* fc = d.fetch_classes;
    REXLOG_INFO(
        "GRAW native draw diag: textures per draw by signs: unsigned {:.3f}, gamma rgb "
        "{:.3f}, gamma {:.3f}, signed {:.3f}, other {:.3f}; exp bias {:.3f}, lod bias {:.3f}",
        per(fc[0]), per(fc[1]), per(fc[2]), per(fc[3]), per(fc[4]), per(fc[5]), per(fc[6]));
    d = Impl::Diag{};
  }
  if (!impl_->debug_path.empty() && ++impl_->debug_poll >= 15) {
    impl_->debug_poll = 0;
    uint32_t mask = 0, max_draws = 0, skip_from = 0, skip_to = 0, hash_count = 0;
    std::array<uint64_t, 8> hashes{};
    if (std::FILE* f = std::fopen(impl_->debug_path.c_str(), "r")) {
      if (std::fscanf(f, "%u %u %u %u", &mask, &max_draws, &skip_from, &skip_to) < 1) mask = 0;
      unsigned long long h;
      while (hash_count < hashes.size() && std::fscanf(f, "%llx", &h) == 1)
        hashes[hash_count++] = h;
      std::fclose(f);
    }
    if (mask != impl_->debug_mask || max_draws != impl_->debug_max_draws ||
        skip_from != impl_->debug_skip_from || skip_to != impl_->debug_skip_to ||
        hash_count != impl_->debug_hash_count || hashes != impl_->debug_hashes)
      REXLOG_INFO("GRAW native draw: debug mask {} max draws {} skip [{}, {}), {} shader hashes",
                  mask, max_draws, skip_from, skip_to, hash_count);
    impl_->debug_hashes = hashes;
    impl_->debug_hash_count = hash_count;
    // Set, not held: the next frames' draws are logged once (to <capture dir>\draws-f<frame>.txt)
    // and the snapshots taken once.
    const auto set = [&](uint32_t bit) { return (mask & bit) && !(impl_->debug_mask & bit); };
    if (set(Impl::kDebugLogDraws))
      impl_->draw_log_left = uint32_t(std::max(REXCVAR_GET(graw_native_draw_log_frames), 1));
    if (set(Impl::kDebugSnapshots))
      impl_->snap_pending = hash_count ? (1u << hash_count) - 1 : 1u << 31;
    impl_->debug_mask = mask;
    impl_->mirror.upload_every_draw.store(mask & Impl::kDebugUploadEveryDraw, std::memory_order_relaxed);
    impl_->debug_max_draws = max_draws;
    impl_->debug_skip_from = skip_from;
    impl_->debug_skip_to = skip_to;
  }
  if (impl_->draw_log) {
    std::fclose(impl_->draw_log);
    impl_->draw_log = nullptr;
    renderer_.render_targets().SetLog(nullptr);
  }
  if (const int32_t from = REXCVAR_GET(graw_native_draw_log_from);
      from >= 0 && renderer_.frame_index() == uint64_t(from))
    impl_->draw_log_left = uint32_t(std::max(REXCVAR_GET(graw_native_draw_log_frames), 1));
  if (impl_->draw_log_left) {
    --impl_->draw_log_left;
    const std::filesystem::path dir(REXCVAR_GET(graw_native_capture_dir));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path =
        dir / ("draws-f" + std::to_string(renderer_.frame_index()) + ".txt");
    impl_->draw_log = std::fopen(path.string().c_str(), "w");
    renderer_.render_targets().SetLog(impl_->draw_log);
  }
  // The next frame's list finds the shared buffer in the common state, to which buffers decay
  // when ExecuteCommandLists completes: SyncListFrame resets shared_state at its first use.
}

}  // namespace graw::native
