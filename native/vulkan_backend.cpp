// GRAW native renderer: the Vulkan backend (vulkan_backend.h).
#include "vulkan_backend.h"

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "game_window.h"
#include "guest_memory.h"  // GpuAddress
#include "pipeline_store.h"
#include "present.h"       // SwapCall
#include "resolve_params.h"  // ResolveParams
#include "vulkan_bindings.h"
#include "vulkan_memory.h"
#include "vulkan_pipelines.h"
#include "vulkan_present.h"
#include "vulkan_render_targets.h"
#include "vulkan_resolve.h"
#include "vulkan_shaders.h"
#include "vulkan_textures.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>

REXCVAR_DEFINE_BOOL(graw_native_vulkan_validation, false, "GRAW native",
                    "With --graw_native_api=vulkan: enable the Khronos validation layer when it "
                    "is installed (the Vulkan SDK); its errors and warnings go to the log");
REXCVAR_DEFINE_BOOL(graw_native_vulkan_show_target, false, "GRAW native",
                    "With --graw_native_api=vulkan: show the color render target most of the "
                    "frame's draws went to (the 3D scene) instead of the front buffer, to check "
                    "the geometry before textures exist (M3d debugging)");
REXCVAR_DEFINE_INT32(graw_native_vulkan_debug, 0, "GRAW native",
                     "With --graw_native_api=vulkan, debug toggles (bits): 1 samplers stay on "
                     "their first mip level, 2 alpha to mask off, 4 cube textures as "
                     "placeholders, 8 textures from guest memory as placeholders");
REXCVAR_DEFINE_STRING(graw_native_vulkan_resolve_dump, "", "GRAW native",
                      "With --graw_native_api=vulkan, debug: directory for dumps of the resolves "
                      "of frame --graw_native_vulkan_resolve_dump_frame, as the D3D12 "
                      "--graw_native_resolve_dump's native texels (f<F>-r<i>-native.bin, tight "
                      "rows; depth as D3D12's, (d24 << 8) | stencil) and f<F>-resolves.jsonl");
REXCVAR_DEFINE_INT32(graw_native_vulkan_debug_blank_fetch, 0, "GRAW native",
                     "With --graw_native_api=vulkan, debug: bit i set binds the placeholder (zero) "
                     "for texture fetch constant i");
REXCVAR_DEFINE_STRING(graw_native_vulkan_skip_draws, "", "GRAW native",
                      "With --graw_native_api=vulkan, debug: skip the draws first-last of every "
                      "frame (indices as the D3D12 draw log's), to find the draw behind a "
                      "difference");
REXCVAR_DEFINE_STRING(graw_native_vulkan_dump_target_draws, "", "GRAW native",
                      "With --graw_native_vulkan_resolve_dump, debug: also read back color target 0 "
                      "after each of the draws first-last of the dumped frame "
                      "(f<F>-d<draw>-rt.bin, in f<F>-targets.jsonl)");
REXCVAR_DEFINE_INT32(graw_native_vulkan_resolve_dump_frames, 1, "GRAW native",
                     "The frames --graw_native_vulkan_resolve_dump dumps from "
                     "--graw_native_vulkan_resolve_dump_frame on");
REXCVAR_DEFINE_INT32(graw_native_vulkan_resolve_dump_frame, -1, "GRAW native",
                     "The native frame --graw_native_vulkan_resolve_dump dumps (-1: none)");
REXCVAR_DECLARE(bool, graw_native_render_thread);
// The D3D12 renderer's (draw.cpp), for both backends.
REXCVAR_DECLARE(bool, graw_native_async_pipelines);
REXCVAR_DECLARE(int32_t, graw_native_pipeline_threads);
REXCVAR_DECLARE(bool, graw_native_pipeline_wait);
REXCVAR_DECLARE(bool, graw_native_pipeline_cache);
REXCVAR_DECLARE(bool, graw_native_implicit_gradients);
REXCVAR_DECLARE(std::string, graw_native_watch_trace);
REXCVAR_DECLARE(bool, graw_native_coherency_check);

namespace graw::native {

struct VulkanBackend::ResolvedImage {
  VulkanMemory::Image image;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkFormat format_signed = VK_FORMAT_UNDEFINED;  // MUTABLE images: their signed views' format
  VkFormat storage_format = VK_FORMAT_UNDEFINED;  // resolve shaders: storage_view's format
  VkImageView storage_view = VK_NULL_HANDLE;
  // Depth resolves: `image` holds the texel bits; this, the depth as R32_SFLOAT (D3D12's
  // depth_r32f, what k_24_8 textures read), with its storage view.
  VulkanMemory::Image depth;
  VkImageView depth_storage_view = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  bool written = false;
  // Of the color resolves into it: the texels written from (0, 0) (the image the present pass
  // shows, as D3D12's ResolvedImage), the destination format (xenos::TextureFormat) and the
  // exponent bias of the last one (the front buffer's log).
  uint32_t written_width = 0, written_height = 0;
  uint32_t guest_format = 0;
  int32_t exp_bias = 0;
};

// The GPU memory the backend owns. Destroyed after the device went idle (~VulkanBackend).
struct VulkanBackend::Gpu {
  VulkanMemory memory;
  // Guest physical memory: what the translated shaders read vertices (and the index buffer is
  // bound) from, kept current by GuestMemoryMirror's copies.
  VulkanMemory::Buffer shared;
  // Vertex and index ranges copied into shared memory (the guest thread's with the render
  // thread: tagged by Prepare, recycled at EndFramePrepare).
  VulkanUploadPool vertex_upload{16u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
  // Descriptor sets, pipeline layouts, constant buffers, placeholder textures (the recording
  // thread's).
  VulkanBindings bindings;
  VulkanPipelines pipelines;
  // Host render targets for the guest's surfaces (destroyed before `memory`).
  VulkanRenderTargets targets;
  VulkanResolveShaders resolve_shaders;
  // M5: the back buffer's pass (the front buffer through the gamma ramp).
  VulkanPresenter presenter;
  // M4: guest textures (created once guest memory is known; destroyed before `memory`).
  std::unique_ptr<VulkanTextureCache> textures;
  // M3d: the images color resolves copied their source rectangles into, by destination address
  // (physical); the Swap's front buffer is one of them, and textures at those addresses bind
  // them (M4). Between uses in SHADER_READ_ONLY_OPTIMAL.
  using Resolved = ResolvedImage;
  std::unordered_map<uint32_t, Resolved> resolved;
  Resolved msaa_temp;  // MSAA targets resolved before a converting blit
  // Replaced images, destroyed once the GPU completed the frame that last used them.
  struct Retired {
    Resolved image;
    uint64_t fence;
  };
  std::vector<Retired> retired;
  // --graw_native_vulkan_resolve_dump: the dumped frame's resolves and their readbacks.
  struct DumpEntry {
    ResolveParams p;
    uint32_t width = 0, height = 0, bpp = 0;
    uint32_t after_draw = 0;  // the frame's draws before it
    uint64_t frame = 0, fence = 0;  // the frame it belongs to, and its fence
    int32_t target_draw = -1;  // a render target after this draw (DumpTarget), not a resolve
    uint32_t target_format = 0, target_base = 0, target_pitch = 0, samples = 1;
    VulkanMemory::Buffer readback;
  };
  std::vector<DumpEntry> dump;
  std::unordered_map<uint64_t, size_t> dump_resolves;  // per frame: resolves written
  VulkanDevice& device;
  explicit Gpu(VulkanDevice& d)
      : bindings(d), pipelines(d), targets(d), resolve_shaders(d), presenter(d, memory),
        device(d) {}
  ~Gpu() {
    textures.reset();
    for (auto& [address, r] : resolved) Destroy(r);
    for (Retired& r : retired) Destroy(r.image);
    for (DumpEntry& e : dump) memory.Destroy(e.readback);
    Destroy(msaa_temp);
    memory.Destroy(shared);
  }
  void Destroy(Resolved& r) {
    for (VkImageView* v : {&r.storage_view, &r.depth_storage_view}) {
      if (*v) device.fn().vkDestroyImageView(device.device(), *v, nullptr);
      *v = VK_NULL_HANDLE;
    }
    memory.Destroy(r.image);
    memory.Destroy(r.depth);
  }
};

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() {
  mirror_.Shutdown();
  if (device_) device_->WaitIdle();
}

bool VulkanBackend::Initialize(const Options& o) {
  if (!o.primary) {
    REXLOG_ERROR("GRAW native Vulkan: needs --graw_native_primary (no debug window yet)");
    return false;
  }
  device_ = std::make_unique<VulkanDevice>();
  if (!device_->Initialize(o.game_window, o.vsync, REXCVAR_GET(graw_native_vulkan_validation)))
    return false;
  shaders_ = std::make_unique<VulkanShaderCache>(*device_);
  rex::memory::Memory* memory = rex::system::kernel_memory();
  membase_ = memory->physical_membase();

  gpu_ = std::make_unique<Gpu>(*device_);
  if (!gpu_->memory.Initialize(*device_)) return false;
  gpu_->vertex_upload.SetMemory(&gpu_->memory);
  if (!gpu_->memory.CreateBuffer(GuestMemoryMirror::kSharedMemorySize,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 false, &gpu_->shared, "shared memory buffer (512 MB)"))
    return false;
  if (!gpu_->bindings.Initialize(gpu_->memory, gpu_->shared.buffer)) return false;
  if (!gpu_->targets.Initialize(gpu_->memory)) return false;
  if (!gpu_->resolve_shaders.Initialize()) return false;
  if (!gpu_->presenter.Initialize()) return false;
  gpu_->textures = std::make_unique<VulkanTextureCache>(*device_, gpu_->memory, memory);
  gpu_->textures->SetResolvedLookup([gpu = gpu_.get()](uint32_t address) {
    VulkanResolvedImage out;
    auto it = gpu->resolved.find(address);
    if (it != gpu->resolved.end() && it->second.written) {
      out.image = it->second.image.image;
      out.format = it->second.format;
      out.format_signed = it->second.format_signed;
      out.width = it->second.width;
      out.height = it->second.height;
      out.depth_image = it->second.depth.image;
    }
    return out;
  });
  // Pipelines: compiled on workers, kept across runs (M4d).
  std::filesystem::path cache_file;
  if (REXCVAR_GET(graw_native_pipeline_cache)) {
    rex::Runtime* runtime =
        rex::system::kernel_state() ? rex::system::kernel_state()->emulator() : nullptr;
    std::filesystem::path dir = runtime ? runtime->cache_root() : std::filesystem::path();
    if (dir.empty()) {
      REXLOG_WARN("GRAW native Vulkan pipelines: no cache root, no pipeline cache");
    } else {
      // Shaders translated the other way can't share the stored pipelines.
      dir /= REXCVAR_GET(graw_native_implicit_gradients) ? "graw-native-vulkan"
                                                         : "graw-native-vulkan-gradients";
      store_ = std::make_unique<PipelineStore>();
      store_->Open(dir, sizeof(VulkanPipelineKey), kVulkanPipelineKeyVersion);
      cache_file = dir / "pipelines.vkcache";
    }
  }
  int threads = 0, extra_threads = 0;
  if (REXCVAR_GET(graw_native_async_pipelines)) {
    const int hardware = int(std::thread::hardware_concurrency());
    threads = REXCVAR_GET(graw_native_pipeline_threads);
    if (threads <= 0) threads = hardware / 4;
    threads = std::clamp(threads, 1, 4);
    // While a frame waits for its pipelines, the game threads wait as well.
    extra_threads = std::max(threads, std::min(hardware - 2, 8)) - threads;
    pipeline_wait_ = REXCVAR_GET(graw_native_pipeline_wait);
  }
  gpu_->pipelines.Initialize(threads, extra_threads, cache_file);
  Prewarm();  // the first slice before the first frame, which needs some of them

  threaded_ = REXCVAR_GET(graw_native_render_thread);
  if (!mirror_.Initialize(memory, this, REXCVAR_GET(graw_native_watch_trace),
                          REXCVAR_GET(graw_native_coherency_check)))
    return false;
  mirror_.SetCounters(threaded_ ? &prep_upload_bytes_ : &stats_.upload_bytes,
                      threaded_ ? &prep_uploads_ : &stats_.uploads,
                      threaded_ ? &prep_diag_ : &diag_);
  REXLOG_INFO("GRAW native Vulkan: up (docs/VULKAN_RENDERER.md, M5)");
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

bool VulkanBackend::AllocateUpload(uint32_t size, uint64_t tag, GuestMemoryMirror::Upload* out) {
  VulkanUploadPool::Alloc a;
  if (!gpu_->vertex_upload.Allocate(size, 16, tag, &a)) return false;
  out->cpu = a.cpu;
  out->buffer = reinterpret_cast<void*>(a.buffer);
  out->offset = a.offset;
  return true;
}

// As DrawRenderer::Impl::Prepare (draw.cpp): shaders, then the draw's ranges into the batch's
// copies, with the copy segment bookkeeping.
void VulkanBackend::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                            std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) {
  // The render thread can't read the shaders later: the tracker's patched copies change.
  prep->vs = LoadShader(state.vs, rex::graphics::xenos::ShaderType::kVertex);
  prep->ps = LoadShader(state.ps, rex::graphics::xenos::ShaderType::kPixel);
  if (!prep->vs) return;
  mirror_.upload_fence = upload_tag;
  const bool indexed_ok = !state.indexed || state.index_address;
  // Straight into the batch: copies made before a failed range still go out (their pages are
  // marked valid already).
  mirror_.preparing = true;
  mirror_.segment_conflict = false;
  mirror_.draw_reads.clear();
  prep->upload_ok = indexed_ok && mirror_.RequestVertices(state, *prep->vs, *copies);
  mirror_.preparing = false;
  if (mirror_.segment_conflict ||
      mirror_.segment_exact.size() >= GuestMemoryMirror::kSegmentExactLimit) {
    mirror_.NewSegment();
    prep->copy_segment = true;
  }
  for (const GuestMemoryMirror::Read& read : mirror_.draw_reads) mirror_.SegmentAdd(read);
  if (mirror_.coherency)
    for (const GuestMemoryMirror::Read& read : mirror_.draw_reads)
      mirror_.coherency->segment_reads.push_back({read.begin, read.end});
}

void VulkanBackend::RecordCopies(const VertexCopy* copies, size_t count) {
  if (!count) return;
  ++stats_.copy_batches;
  const VulkanDevice::Functions& fn = device_->fn();
  VkCommandBuffer cmd = device_->command_buffer();
  // Copies can't be recorded inside a rendering instance.
  gpu_->targets.EndRendering(cmd);
  // Earlier draws' reads of shared memory (vertex fetches, the index buffer) before the copies'
  // writes, and the copies before later draws' reads.
  VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = gpu_->shared.buffer;
  b.size = VK_WHOLE_SIZE;
  constexpr VkPipelineStageFlags kReadStages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kReadAccess = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  b.srcAccessMask = kReadAccess;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fn.vkCmdPipelineBarrier(cmd, kReadStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &b,
                          0, nullptr);
  for (size_t i = 0; i < count; ++i) {
    const VertexCopy& c = copies[i];
    const VkBufferCopy region{c.src_offset, c.dst, c.size};
    fn.vkCmdCopyBuffer(cmd, reinterpret_cast<VkBuffer>(c.src), gpu_->shared.buffer, 1, &region);
  }
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = kReadAccess;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kReadStages, 0, 0, nullptr, 1, &b,
                          0, nullptr);
}

// Bytes per texel of the formats resolve images have.
static uint32_t FormatBytes(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UNORM:
      return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
      return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return 16;
    default:
      return 4;
  }
}

void VulkanBackend::Draw(const DrawState& state, const DrawPrep* prep) {
  using rex::graphics::xenos::ShaderType;
  const uint32_t draw_index = frame_draw_++;
  if (prep) {
    // The guest thread copied the ranges (and marked their pages valid): they go into shared
    // memory whether or not the draw is issued.
    RecordCopies(prep->copies, prep->copy_count);
    if (!prep->upload_ok) {
      ++stats_.failed;
      return;
    }
  }
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
  if (!prep) {
    mirror_.upload_fence = device_->frame_fence();
    copies_.clear();
    const bool ok = mirror_.RequestVertices(state, *vs, copies_);
    // The copies made before a failed range go out: their pages are marked valid already.
    RecordCopies(copies_.data(), copies_.size());
    if (!ok) {
      ++stats_.failed;
      return;
    }
  }
  if (!mods.pixel_shader_used) ps = nullptr;
  if (draw_index >= skip_first_ && draw_index <= skip_last_) {
    ++stats_.skipped;
    return;
  }
  if (!shaders_->Module(*vs, mods.vertex) || (ps && !shaders_->Module(*ps, mods.pixel))) {
    ++stats_.failed;
    return;
  }

  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(state.regs);
  const auto prim = xenos::PrimitiveType(state.primitive);
  auto unsupported = [&](const char* why) {
    if (unsupported_logged_ < 16 && !unsupported_seen_.count(why)) {
      unsupported_seen_.insert(why);
      ++unsupported_logged_;
      REXLOG_WARN("GRAW native Vulkan draw: {} unsupported (draw skipped)", why);
    }
    ++stats_.unsupported;
  };
  if (vs->memexport_eM_written() || (ps && ps->memexport_eM_written()))
    return unsupported("memexport");
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = 0;
  initiator.prim_type = prim;
  if (xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable)
    return unsupported("tessellation");
  const bool polygonal = rex::graphics::draw_util::IsPrimitivePolygonal(false, prim);
  const bool rasterization = IsRasterizationPotentiallyDone(regs, polygonal);

  // Host primitive. Points, rectangles and quads are expanded by geometry shaders
  // (vulkan_geometry_shader.h); fans are native on Vulkan.
  VkPrimitiveTopology topology;
  PipelineGeometryShader geometry = PipelineGeometryShader::kNone;
  switch (prim) {
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case xenos::PrimitiveType::kTriangleFan:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      break;
    case xenos::PrimitiveType::kPointList:
      topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      geometry = PipelineGeometryShader::kPointList;
      break;
    case xenos::PrimitiveType::kRectangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      geometry = PipelineGeometryShader::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      // 4 vertices per primitive, as a line list with adjacency.
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      geometry = PipelineGeometryShader::kQuadList;
      break;
    default:
      return unsupported("primitive type");
  }
  if (geometry != PipelineGeometryShader::kNone && !device_->shader_caps().geometry_shader)
    return unsupported("point, rectangle or quad list without geometry shaders");
  if (!state.count) {
    ++stats_.skipped;
    return;
  }

  // Index buffer (the guest's, in shared memory; the vertex shader swaps the indices) and
  // primitive reset, as draw.cpp.
  xenos::Endian index_endian = xenos::Endian::kNone;
  bool restart = false;
  if (state.indexed) {
    index_endian = xenos::Endian(state.index_endian);
    if (!state.index_32bit && index_endian != xenos::Endian::kNone &&
        index_endian != xenos::Endian::k8in16)
      index_endian =
          index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
        (prim == xenos::PrimitiveType::kLineStrip ||
         prim == xenos::PrimitiveType::kTriangleStrip)) {
      const uint32_t reset =
          xenos::GpuSwap(regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx, index_endian);
      if (!state.index_32bit && reset <= 0xFFFF) {
        if (reset != 0xFFFF) return unsupported("primitive reset index other than 0xFFFF");
        restart = true;
      } else if (state.index_32bit) {
        restart = true;  // all 32 bits compared on the host, the low 24 on the guest
      }
    }
    if (!state.index_address) {
      ++stats_.failed;
      return;
    }
  }

  // Render targets.
  const reg::RB_DEPTHCONTROL depth_control = GetNormalizedDepthControl(regs);
  const uint32_t color_mask = ps ? GetNormalizedColorMask(regs, ps->writes_color_targets()) : 0;
  const bool depth_used =
      rasterization && (depth_control.z_enable || depth_control.stencil_enable);
  uint32_t rt_mask = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if ((color_mask >> (4 * i)) & 0xF) rt_mask |= 1u << i;
  if (!rt_mask && !depth_used) {
    ++stats_.skipped;  // nothing to write
    return;
  }
  Scissor scissor;
  HostScissor(regs, scissor);
  ViewportInfo vp;
  HostViewportInfo(regs, ps && ps->writes_depth(), vp, 16384, /*origin_bottom_left=*/false);
  const uint32_t height = std::clamp(scissor.offset[1] + scissor.extent[1], 1u, 2048u);

  // System constants as UpdateSystemConstantValues sets them for the host render target path.
  const uint32_t used_textures = vs->GetUsedTextureMaskAfterTranslation() |
                                 (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  VulkanBindings& bindings = gpu_->bindings;
  VulkanRenderTargets& targets = gpu_->targets;
  VkCommandBuffer cmd = device_->command_buffer();
  if (!bindings.placeholders_ready()) {
    targets.EndRendering(cmd);
    bindings.InitializePlaceholders(cmd);
  }
  // Textures (M4) before the render targets: an upload ends the rendering instance.
  uint8_t signs[32] = {};
  texture_views_.clear();
  samplers_.clear();
  GetShaderTextures(*vs, state, signs);
  const size_t vs_views = texture_views_.size(), vs_samplers = samplers_.size();
  if (ps) GetShaderTextures(*ps, state, signs);
  const ShaderTextures vs_textures{texture_views_.data(), samplers_.data()};
  const ShaderTextures ps_textures{texture_views_.data() + vs_views,
                                   samplers_.data() + vs_samplers};
  SpirvShaderTranslator::SystemConstants sys;
  FillSpirvSystemConstants(regs, prim, polygonal, index_endian, vp, used_textures, signs, sys);
  if (REXCVAR_GET(graw_native_vulkan_debug) & 2) sys.alpha_to_mask = 0;
  VulkanBoundTargets bound;
  if (!targets.Bind(cmd, SurfaceRegsFromRegisters(state.regs), rt_mask, depth_used, height,
                    &bound)) {
    ++stats_.failed;
    return;
  }
  if (bound.color[0]) {
    TargetUse& use = target_use_[bound.color[0]];
    ++use.draws;
    use.rows = std::max(use.rows, std::min(height, bound.color[0]->height));
  }
  const VulkanBindings::Layout* layout = bindings.GetLayout(*vs, ps);
  if (!layout || !bindings.Bind(cmd, *layout, state.regs, *vs, ps, sys, device_->frame_fence(),
                                vs_textures, ps_textures)) {
    ++stats_.failed;
    return;
  }

  // The pipeline.
  VulkanPipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = shaders_->Module(*vs, mods.vertex);
  key.ps = ps ? shaders_->Module(*ps, mods.pixel) : VK_NULL_HANDLE;
  if (geometry != PipelineGeometryShader::kNone) {
    key.gs = shaders_->Geometry(GetVulkanGeometryShaderKey(geometry, mods.vertex, mods.pixel));
    if (!key.gs) {
      ++stats_.failed;
      return;
    }
  }
  key.layout = layout->layout;
  key.topology = uint8_t(topology);
  key.primitive_restart = restart;
  key.samples = uint8_t(bound.samples);
  const auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (!rasterization) {
    key.discard = 1;
  } else {
    if (polygonal) {
      key.front_cw = sc.face != 0;
      if (sc.cull_front) key.cull |= VK_CULL_MODE_FRONT_BIT;
      if (sc.cull_back) key.cull |= VK_CULL_MODE_BACK_BIT;
      if (sc.poly_mode == xenos::PolygonModeEnable::kDualMode &&
          ((!sc.cull_front && sc.polymode_front_ptype != xenos::PolygonType::kTriangles) ||
           (!sc.cull_back && sc.polymode_back_ptype != xenos::PolygonType::kTriangles)))
        key.polygon_line = 1;
    }
    key.depth_clamp = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  }
  if (bound.depth) {
    key.has_depth = 1;
    key.depth_format = bound.depth->format;
    uint32_t func = uint32_t(xenos::CompareFunction::kAlways);
    if (depth_control.z_enable) {
      func = uint32_t(depth_control.zfunc);
      key.depth_write = depth_control.z_write_enable;
    }
    key.depth_compare = uint8_t(VK_COMPARE_OP_NEVER + func);
    key.depth_test = func != uint32_t(xenos::CompareFunction::kAlways) || key.depth_write;
    if (depth_control.stencil_enable) {
      key.stencil_test = 1;
      key.front[0] = uint8_t(VK_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail));
      key.front[1] = uint8_t(VK_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass));
      key.front[2] = uint8_t(VK_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail));
      key.front[3] = uint8_t(VK_COMPARE_OP_NEVER + uint32_t(depth_control.stencilfunc));
      if (polygonal && depth_control.backface_enable) {
        key.back[0] = uint8_t(VK_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail_bf));
        key.back[1] = uint8_t(VK_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass_bf));
        key.back[2] = uint8_t(VK_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail_bf));
        key.back[3] = uint8_t(VK_COMPARE_OP_NEVER + uint32_t(depth_control.stencilfunc_bf));
      } else {
        std::memcpy(key.back, key.front, 4);
      }
    }
  }
  static constexpr VkBlendFactor kVkBlend[] = {
      VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_SRC_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  static constexpr VkBlendOp kVkBlendOp[8] = {
      VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT,         VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
      VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
  key.color_count = uint8_t(bound.color_count);
  for (uint32_t i = 0; i < bound.color_count; ++i) {
    if (!bound.color[i]) continue;
    key.color_format[i] = bound.color[i]->format;
    VulkanPipelineKey::Rt& rt = key.rt[i];
    rt.mask = uint8_t((color_mask >> (i * 4)) & 0xF);
    rt.src = rt.src_a = uint8_t(VK_BLEND_FACTOR_ONE);
    rt.dst = rt.dst_a = uint8_t(VK_BLEND_FACTOR_ZERO);
    if (!rt.mask) continue;
    auto bc = regs.Get<reg::RB_BLENDCONTROL>(kBlendControlRegs[i]);
    const BlendFactor src = kBlendFactorMap[uint32_t(bc.color_srcblend) & 31];
    const BlendFactor dst = kBlendFactorMap[uint32_t(bc.color_destblend) & 31];
    const BlendFactor src_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_srcblend) & 31];
    const BlendFactor dst_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_destblend) & 31];
    const uint32_t op = uint32_t(bc.color_comb_fcn) & 7, op_a = uint32_t(bc.alpha_comb_fcn) & 7;
    if (src != BlendFactor::kOne || dst != BlendFactor::kZero || op != 0 ||
        src_a != BlendFactor::kOne || dst_a != BlendFactor::kZero || op_a != 0) {
      rt.blend = 1;
      rt.src = uint8_t(kVkBlend[uint32_t(src)]);
      rt.dst = uint8_t(kVkBlend[uint32_t(dst)]);
      rt.op = uint8_t(kVkBlendOp[op]);
      rt.src_a = uint8_t(kVkBlend[uint32_t(src_a)]);
      rt.dst_a = uint8_t(kVkBlend[uint32_t(dst_a)]);
      rt.op_a = uint8_t(kVkBlendOp[op_a]);
    }
  }
  bool pending, is_new;
  const VkPipeline pipeline = gpu_->pipelines.Get(key, &pending, &is_new);
  if (is_new) {
    ++stats_.pso_created;
    if (store_ && store_->is_open())
      RecordPipeline(key, *vs, mods.vertex, ps, mods.pixel,
                     geometry != PipelineGeometryShader::kNone
                         ? GetVulkanGeometryShaderKey(geometry, mods.vertex, mods.pixel).key
                         : 0);
  }
  if (!pipeline) {
    // Still compiling: the draw is skipped and the frame held (Swap).
    if (pending)
      ++stats_.pso_pending;
    else
      ++stats_.failed;
    return;
  }

  // Dynamic state, then the draw.
  const VulkanDevice::Functions& fn = device_->fn();
  VkRect2D scissor_rect;
  scissor_rect.offset = {int32_t(std::min(scissor.offset[0], bound.width)),
                         int32_t(std::min(scissor.offset[1], bound.height))};
  scissor_rect.extent = {
      std::min(scissor.extent[0], bound.width - uint32_t(scissor_rect.offset.x)),
      std::min(scissor.extent[1], bound.height - uint32_t(scissor_rect.offset.y))};
  if (!vp.xy_extent[0] || !vp.xy_extent[1] || !scissor_rect.extent.width ||
      !scissor_rect.extent.height) {
    ++stats_.skipped;
    return;
  }
  fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const VkViewport viewport{float(vp.xy_offset[0]), float(vp.xy_offset[1]),
                            float(vp.xy_extent[0]), float(vp.xy_extent[1]), vp.z_min, vp.z_max};
  fn.vkCmdSetViewport(cmd, 0, 1, &viewport);
  fn.vkCmdSetScissor(cmd, 0, 1, &scissor_rect);
  float poly_offset = 0.0f, poly_offset_scale = 0.0f;
  if (rasterization) GetPreferredFacePolygonOffset(regs, polygonal, poly_offset_scale, poly_offset);
  fn.vkCmdSetDepthBias(cmd,
                       float(rex::graphics::draw_util::GetD3D10IntegerPolygonOffset(
                           regs.Get<reg::RB_DEPTH_INFO>().depth_format, poly_offset)),
                       0.0f, poly_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit);
  const float blend_constants[4] = {regs.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_RED),
                                    regs.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_GREEN),
                                    regs.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_BLUE),
                                    regs.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_ALPHA)};
  fn.vkCmdSetBlendConstants(cmd, blend_constants);
  const auto ref = regs.Get<reg::RB_STENCILREFMASK>();
  const auto ref_bf = polygonal && depth_control.backface_enable
                          ? regs.Get<reg::RB_STENCILREFMASK>(
                                rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF)
                          : ref;
  fn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, ref.stencilmask);
  fn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, ref_bf.stencilmask);
  fn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, ref.stencilwritemask);
  fn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, ref_bf.stencilwritemask);
  fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, ref.stencilref);
  fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, ref_bf.stencilref);
  if (state.indexed) {
    fn.vkCmdBindIndexBuffer(cmd, gpu_->shared.buffer, state.index_address,
                            state.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    fn.vkCmdDrawIndexed(cmd, state.count, 1, 0, 0, 0);
  } else {
    fn.vkCmdDraw(cmd, state.count, 1, 0, 0);
  }
  ++stats_.draws;
  if (draw_index >= dump_target_first_ && draw_index <= dump_target_last_ && bound.color[0] &&
      DumpingFrame())
    DumpTarget(bound.color[0], draw_index);
}

void VulkanBackend::DumpTarget(VulkanRenderTarget* rt, uint32_t index) {
  if (rt->samples != 1) return;  // MSAA images can't be copied to buffers
  Gpu::DumpEntry e;
  e.target_draw = int32_t(index);
  e.target_format = rt->guest_format;
  e.target_base = rt->edram_base;
  e.target_pitch = rt->pitch;
  e.width = rt->width;
  e.height = std::min(rt->height, 2048u);
  e.bpp = FormatBytes(rt->format);
  if (!gpu_->memory.CreateReadbackBuffer(uint64_t(e.width) * e.height * e.bpp, &e.readback,
                                         "render target dump readback"))
    return;
  VkCommandBuffer cmd = device_->command_buffer();
  gpu_->targets.EndRendering(cmd);
  const VulkanDevice::Functions& fn = device_->fn();
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = rt->image.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {e.width, e.height, 1};
  fn.vkCmdCopyImageToBuffer(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            e.readback.buffer, 1, &region);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                          1, &b);
  e.frame = frame_index_;
  e.fence = device_->frame_fence();
  gpu_->dump.push_back(std::move(e));
}

void VulkanBackend::Clear(const ClearCall& clear) {
  gpu_->targets.Clear(device_->command_buffer(), clear);
}

// The host formats of a resolve's destination texture format (VK_FORMAT_UNDEFINED: the
// target's own), unsigned and signed (UNDEFINED: none).
static void ResolveFormats(uint32_t dest_format, VkFormat* unorm, VkFormat* snorm) {
  const HostFormat* host =
      GetHostFormat(rex::graphics::xenos::TextureFormat(dest_format & 63));
  *unorm = host ? VulkanFormatOf(host->unorm) : VK_FORMAT_UNDEFINED;
  *snorm = host && *unorm != VK_FORMAT_UNDEFINED ? VulkanFormatOf(host->snorm)
                                                 : VK_FORMAT_UNDEFINED;
}

void VulkanBackend::Resolve(const ResolveParams& p) {
  const bool depth = p.source == 4;
  if (p.source > 4) {
    ++resolves_skipped_;
    return;
  }
  VulkanRenderTargets& targets = gpu_->targets;
  VkCommandBuffer cmd = device_->command_buffer();
  targets.EndRendering(cmd);
  const uint32_t rows = uint32_t(std::clamp(p.y1, 1, 2048));
  VulkanRenderTarget* rt = depth ? targets.GetDepth(cmd, p.regs, rows)
                                 : targets.GetColor(cmd, p.regs, p.source, rows);
  if (!rt) {
    ++resolves_skipped_;
    return;
  }
  // The source rectangle on the target. The clears cover all of it, the copy only what fits in
  // the destination level (as resolve.cpp).
  const int32_t x0 = std::max(p.x0, 0), y0 = std::max(p.y0, 0);
  const int32_t x1 = std::min(p.x1, int32_t(rt->width)), y1 = std::min(p.y1, int32_t(rt->height));
  if (x1 <= x0 || y1 <= y0) return;  // nothing to copy or clear
  const int32_t dx = p.dest_x + (x0 - p.x0), dy = p.dest_y + (y0 - p.y0);
  const int32_t w = std::min(x1 - x0, int32_t(p.dest_width) - dx);
  const int32_t h = std::min(y1 - y0, int32_t(p.dest_height) - dy);
  bool copied = false;
  if (w > 0 && h > 0 && dx >= 0 && dy >= 0) {
    targets.PrepareRead(cmd, rt);
    if (depth) {
      copied = CopyDepth(rt, x0, y0, w, h, dx, dy, p);
      depth_resolves_ += copied;
    } else if (p.copy_command == 1) {
      copied = CopyColor(rt, x0, y0, w, h, dx, dy, p);
      if (copied) {
        ++shader_resolves_;
      } else {
        VkFormat unorm, snorm;
        ResolveFormats(p.dest_format, &unorm, &snorm);
        copied = CopyTarget(rt, x0, y0, w, h, dx, dy, p.dest_width, p.dest_height, p.dest_base,
                            unorm, snorm);
        if (copied && p.exp_bias && !bias_dropped_logged_) {
          bias_dropped_logged_ = true;
          REXLOG_WARN("GRAW native Vulkan: a resolve into format {} with exponent bias {} is a "
                      "blit, without the bias",
                      p.dest_format, p.exp_bias);
        }
      }
      if (copied) {
        ResolvedImage& r = gpu_->resolved[p.dest_base];
        r.written_width = std::max(r.written_width, uint32_t(dx + w));
        r.written_height = std::max(r.written_height, uint32_t(dy + h));
        r.guest_format = p.dest_format;
        r.exp_bias = p.exp_bias;
      }
    }
  }
  if (copied) {
    ++resolves_;
    if (DumpingFrame())
      DumpRecord(p, uint32_t(dx + w), uint32_t(dy + h));
  } else {
    ++resolves_skipped_;
  }
  // Clears of the source surfaces over the source rectangle (after the copy).
  if (p.clear_flags) {
    ClearCall c;
    c.p.flags = ((p.clear_flags & 0x100) ? (1u << (depth ? 0 : p.source)) : 0) |
                ((p.clear_flags & 0x200) ? 0x30u : 0);
    std::memcpy(c.p.color, p.clear_color, sizeof(c.p.color));
    c.p.depth = p.clear_depth;
    c.p.stencil = p.clear_stencil;
    c.p.rects.push_back({int32_t(x0), int32_t(y0), int32_t(x1), int32_t(y1)});
    if (p.edram_x || p.edram_y)
      c.p.edram_rects.push_back(
          {int32_t(x0 - p.edram_x), int32_t(y0 - p.edram_y), int32_t(x1 - p.edram_x), int32_t(y1 - p.edram_y)});
    c.regs = p.regs;
    c.bound_colors = 0xF;
    c.depth_bound = true;
    c.height = uint32_t(std::max(p.y1, 0));
    targets.Clear(cmd, c);
    ++resolve_clears_;
  }
}

bool VulkanBackend::CopyDepth(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w,
                              int32_t h, int32_t dx, int32_t dy, const ResolveParams& p) {
  const VkImageView src = gpu_->targets.DepthView(rt);
  const VkImageView stencil = gpu_->targets.StencilView(rt);
  if (!src || !stencil) return false;
  // The texel bits, as D3D12's resolver writes them: a 32-bit destination (the guest copies
  // k_24_8 depth as k_8_8_8_8).
  VkFormat unorm, snorm;
  ResolveFormats(p.dest_format, &unorm, &snorm);
  if (DestBppLog2(p.dest_format) != 2 || unorm == VK_FORMAT_UNDEFINED) {
    if (!depth_format_logged_) {
      depth_format_logged_ = true;
      REXLOG_WARN("GRAW native Vulkan: depth resolves into format {} not supported",
                  p.dest_format);
    }
    return false;
  }
  if (snorm == unorm) snorm = VK_FORMAT_UNDEFINED;
  ResolvedImage* r =
      EnsureImage(gpu_->resolved[p.dest_base], unorm, snorm, p.dest_width, p.dest_height,
                  "depth resolve image", VK_FORMAT_R32_UINT, true);
  if (!r) {
    gpu_->resolved.erase(p.dest_base);
    return false;
  }
  VulkanResolveShaders::DepthParams params;
  params.src_x = x0;
  params.src_y = y0;
  params.width = uint32_t(w);
  params.height = uint32_t(h);
  params.dest_x = dx;
  params.dest_y = dy;
  params.depth_format = rt->guest_format;
  params.host_sample = VulkanResolveShaders::HostSample(rt->samples, p.sample_select);
  return RunResolveShader(rt, src, stencil, r, [&](VkDescriptorSet set) {
    gpu_->resolve_shaders.RecordDepth(device_->command_buffer(), rt->samples > 1, set, params);
  });
}

bool VulkanBackend::CopyColor(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w,
                              int32_t h, int32_t dx, int32_t dy, const ResolveParams& p) {
  const VkFormat storage = gpu_->resolve_shaders.ColorStorageFormat(p.dest_format);
  if (storage == VK_FORMAT_UNDEFINED) return false;
  VkFormat unorm, snorm;
  ResolveFormats(p.dest_format, &unorm, &snorm);
  if (unorm == VK_FORMAT_UNDEFINED) return false;
  if (snorm == unorm) snorm = VK_FORMAT_UNDEFINED;
  ResolvedImage* r = EnsureImage(gpu_->resolved[p.dest_base], unorm, snorm, p.dest_width,
                                 p.dest_height, "resolve destination image", storage);
  if (!r) {
    gpu_->resolved.erase(p.dest_base);
    return false;
  }
  VulkanResolveShaders::ColorParams params;
  params.src_x = x0;
  params.src_y = y0;
  params.width = uint32_t(w);
  params.height = uint32_t(h);
  params.dest_x = uint32_t(dx);
  params.dest_y = uint32_t(dy);
  params.dest_format = p.dest_format;
  params.src_format = rt->guest_format;
  params.flags = (p.dest_swap ? 2u : 0u) | (p.sample_select << 4) | (rt->samples << 8) |
                 (p.dest_number << 12);
  params.exp_bias = p.exp_bias;
  return RunResolveShader(rt, rt->view, VK_NULL_HANDLE, r, [&](VkDescriptorSet set) {
    gpu_->resolve_shaders.RecordColor(device_->command_buffer(), rt->samples > 1, set, params);
  });
}

void VulkanBackend::DumpRecord(const ResolveParams& p, uint32_t width, uint32_t height) {
  auto it = gpu_->resolved.find(p.dest_base);
  if (it == gpu_->resolved.end() || !it->second.written) return;
  ResolvedImage& r = it->second;
  width = std::min(width, r.width);
  height = std::min(height, r.height);
  if (!width || !height) return;
  Gpu::DumpEntry e;
  e.p = p;
  e.width = width;
  e.height = height;
  e.bpp = FormatBytes(r.format);
  e.after_draw = frame_draw_;
  if (!gpu_->memory.CreateReadbackBuffer(uint64_t(width) * height * e.bpp, &e.readback,
                                         "resolve dump readback"))
    return;
  VkCommandBuffer cmd = device_->command_buffer();
  gpu_->targets.EndRendering(cmd);
  const VulkanDevice::Functions& fn = device_->fn();
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = r.image.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  fn.vkCmdCopyImageToBuffer(cmd, r.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            e.readback.buffer, 1, &region);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b);
  e.frame = frame_index_;
  e.fence = device_->frame_fence();
  gpu_->dump.push_back(std::move(e));
}

bool VulkanBackend::DumpingFrame() const {
  const int32_t at = REXCVAR_GET(graw_native_vulkan_resolve_dump_frame);
  return at >= 0 && frame_index_ >= uint64_t(at) &&
         frame_index_ < uint64_t(at) + uint64_t(std::max(1, REXCVAR_GET(graw_native_vulkan_resolve_dump_frames)));
}

void VulkanBackend::DumpWrite(uint64_t completed) {
  const std::filesystem::path dir = REXCVAR_GET(graw_native_vulkan_resolve_dump);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  size_t written = 0;
  for (Gpu::DumpEntry& e : gpu_->dump) {
    if (e.fence > completed || !e.readback.buffer) continue;
    const ResolveParams& p = e.p;
    const std::string prefix = "f" + std::to_string(e.frame);
    gpu_->memory.Invalidate(e.readback);
    if (e.target_draw >= 0) {
      const std::string stem = prefix + "-d" + std::to_string(e.target_draw);
      if (std::FILE* f = std::fopen((dir / (stem + "-rt.bin")).string().c_str(), "wb")) {
        std::fwrite(e.readback.mapped, 1, size_t(e.width) * e.height * e.bpp, f);
        std::fclose(f);
      }
      if (std::FILE* m = std::fopen((dir / (prefix + "-targets.jsonl")).string().c_str(), "ab")) {
        std::fprintf(m,
                     "{\"draw\":%d,\"format\":%u,\"base\":%u,\"pitch\":%u,\"width\":%u,"
                     "\"height\":%u,\"bpp\":%u}\n",
                     e.target_draw, e.target_format, e.target_base, e.target_pitch, e.width,
                     e.height, e.bpp);
        std::fclose(m);
      }
    } else {
      const size_t i = gpu_->dump_resolves[e.frame]++;
      const std::string stem = prefix + "-r" + std::to_string(i);
      if (std::FILE* f = std::fopen((dir / (stem + "-native.bin")).string().c_str(), "wb")) {
        std::fwrite(e.readback.mapped, 1, size_t(e.width) * e.height * e.bpp, f);
        std::fclose(f);
      }
      if (std::FILE* m = std::fopen((dir / (prefix + "-resolves.jsonl")).string().c_str(), "ab")) {
        std::fprintf(m,
                     "{\"index\":%zu,\"source\":%u,\"surface_info\":%u,\"source_info\":%u,"
                     "\"sample_select\":%u,\"copy_command\":%u,\"rect\":[%d,%d,%d,%d],"
                     "\"dest_point\":[%d,%d],\"dest_base\":%u,\"dest_format\":%u,"
                     "\"dest_width\":%u,\"dest_height\":%u,\"number\":%u,\"swap\":%d,"
                     "\"exp_bias\":%d,\"bpp\":%u,\"native_width\":%u,\"native_height\":%u,"
                     "\"clear_flags\":%u,\"after_draw\":%u,\"depth_bits\":%d}\n",
                     i, p.source, p.regs.surface_info,
                     p.source == 4 ? p.regs.depth_info : p.regs.color_info[p.source & 3],
                     p.sample_select, p.copy_command, p.x0, p.y0, p.x1, p.y1, p.dest_x, p.dest_y,
                     p.dest_base, p.dest_format, p.dest_width, p.dest_height, p.dest_number,
                     p.dest_swap ? 1 : 0, p.exp_bias, e.bpp, e.width, e.height, p.clear_flags,
                     e.after_draw, p.source == 4 ? 1 : 0);
        std::fclose(m);
      }
    }
    gpu_->memory.Destroy(e.readback);
    ++written;
  }
  gpu_->dump.erase(std::remove_if(gpu_->dump.begin(), gpu_->dump.end(),
                                  [](const Gpu::DumpEntry& e) { return !e.readback.buffer; }),
                   gpu_->dump.end());
  if (written)
    REXLOG_INFO("GRAW native Vulkan: dumped {} resolves and targets to {}", written, dir.string());
}

bool VulkanBackend::RunResolveShader(VulkanRenderTarget* rt, VkImageView src_view,
                                     VkImageView stencil_view, ResolvedImage* r,
                                     const std::function<void(VkDescriptorSet)>& record) {
  VkCommandBuffer cmd = device_->command_buffer();
  gpu_->targets.EndRendering(cmd);
  VulkanResolveShaders& rs = gpu_->resolve_shaders;
  const bool depth = stencil_view != VK_NULL_HANDLE;
  VkDescriptorSet set = gpu_->bindings.AllocateFrameSet(
      depth ? rs.depth_set_layout() : rs.set_layout(), device_->frame_fence());
  if (!set) return false;
  const VulkanDevice::Functions& fn = device_->fn();
  const VkImageLayout src_layout = rt->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // vulkan_resolve.h's bindings: color {source, destination}, depth {depth, R32_SFLOAT image,
  // bits, stencil}.
  const VkDescriptorImageInfo images[4] = {
      {rs.sampler(), src_view, src_layout},
      {VK_NULL_HANDLE, depth ? r->depth_storage_view : r->storage_view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, r->storage_view, VK_IMAGE_LAYOUT_GENERAL},
      {rs.sampler(), stencil_view, src_layout}};
  const uint32_t bindings = depth ? 4 : 2;
  VkWriteDescriptorSet wr[4] = {};
  for (uint32_t i = 0; i < bindings; ++i) {
    wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[i].dstSet = set;
    wr[i].dstBinding = i;
    wr[i].descriptorCount = 1;
    wr[i].descriptorType = i == 0 || i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[i].pImageInfo = &images[i];
  }
  fn.vkUpdateDescriptorSets(device_->device(), bindings, wr, 0, nullptr);

  // The source from its attachment layout, the destination from shader reads, to the compute
  // shader; and back.
  const VkPipelineStageFlags rt_stages =
      rt->depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkImageLayout rt_layout = rt->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  const VkAccessFlags rt_access = rt->depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                            : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                            VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkImageMemoryBarrier b[3] = {};
  for (VkImageMemoryBarrier& x : b) {
    x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  }
  b[0].image = rt->image.image;
  b[0].subresourceRange = {
      rt->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
      0, 1, 0, 1};
  b[0].oldLayout = rt_layout;
  b[0].newLayout = src_layout;
  b[0].srcAccessMask = rt_access;
  b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b[1].image = r->image.image;
  b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b[1].oldLayout =
      r->written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  b[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  // The depth image: as the bits' image (written at the same resolves).
  b[2] = b[1];
  b[2].image = r->depth.image;
  const uint32_t barriers = depth ? 3 : 2;
  fn.vkCmdPipelineBarrier(cmd, rt_stages | kReaders, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                          nullptr, 0, nullptr, barriers, b);
  record(set);
  b[0].oldLayout = src_layout;
  b[0].newLayout = rt_layout;
  b[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b[0].dstAccessMask = rt_access;
  b[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  b[2].oldLayout = b[1].oldLayout;
  b[2].newLayout = b[1].newLayout;
  b[2].srcAccessMask = b[1].srcAccessMask;
  b[2].dstAccessMask = b[1].dstAccessMask;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, rt_stages | kReaders, 0, 0,
                          nullptr, 0, nullptr, barriers, b);
  r->written = true;
  return true;
}

bool VulkanBackend::CopyTarget(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w,
                               int32_t h, int32_t dx, int32_t dy, uint32_t dest_w,
                               uint32_t dest_h, uint32_t key, VkFormat dest_format,
                               VkFormat dest_format_signed) {
  VkCommandBuffer cmd = device_->command_buffer();
  gpu_->targets.EndRendering(cmd);
  if (dest_format == VK_FORMAT_UNDEFINED) dest_format = rt->format;
  // Another format (an HDR target resolved into k_2_10_10_10, say): a blit converts, from a
  // single-sample copy of the target when it has MSAA. Without the exponent bias (Resolve logs a
  // resolve that had one): the resolve shader does them where the GPU can.
  const bool convert = dest_format != rt->format;
  if (convert && !CanBlit(rt->format, dest_format)) return false;
  Gpu::Resolved* temp = nullptr;
  if (convert && rt->samples > 1) {
    temp = EnsureImage(gpu_->msaa_temp, rt->format, VK_FORMAT_UNDEFINED,
                       std::max(rt->width, gpu_->msaa_temp.width),
                       std::max(rt->height, gpu_->msaa_temp.height), "MSAA resolve image");
    if (!temp) return false;
  }
  // A signed view only of another format than the unsigned one (float formats: the same).
  if (dest_format_signed == dest_format) dest_format_signed = VK_FORMAT_UNDEFINED;
  Gpu::Resolved* r = EnsureImage(gpu_->resolved[key], dest_format, dest_format_signed, dest_w,
                                 dest_h, "resolve destination image");
  if (!r) {
    gpu_->resolved.erase(key);
    return false;
  }
  const VulkanDevice::Functions& fn = device_->fn();
  constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                            VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkImageMemoryBarrier b[3] = {};
  for (VkImageMemoryBarrier& x : b) {
    x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  b[0].image = rt->image.image;
  b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[1].image = r->image.image;
  b[1].oldLayout =
      r->written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  uint32_t barriers = 2;
  if (temp) {
    // Its last use was a blit source; its contents don't matter.
    b[2].image = temp->image.image;
    b[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[2].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[2].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers = 3;
  }
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kReaders,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, barriers, b);
  const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  if (convert) {
    VkImage source = rt->image.image;
    int32_t sx = x0, sy = y0;
    if (temp) {
      const VkImageResolve region{layers, {x0, y0, 0}, layers, {0, 0, 0},
                                  {uint32_t(w), uint32_t(h), 1}};
      fn.vkCmdResolveImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           temp->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      b[2].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b[2].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &b[2]);
      source = temp->image.image;
      sx = sy = 0;
    }
    const VkImageBlit region{layers,
                             {{sx, sy, 0}, {sx + w, sy + h, 1}},
                             layers,
                             {{dx, dy, 0}, {dx + w, dy + h, 1}}};
    fn.vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->image.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
  } else if (rt->samples > 1) {
    const VkImageResolve region{layers, {x0, y0, 0}, layers, {dx, dy, 0},
                                {uint32_t(w), uint32_t(h), 1}};
    fn.vkCmdResolveImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         r->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  } else {
    const VkImageCopy region{layers, {x0, y0, 0}, layers, {dx, dy, 0},
                             {uint32_t(w), uint32_t(h), 1}};
    fn.vkCmdCopyImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->image.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kReaders, 0, 0,
                          nullptr, 0, nullptr, 2, b);
  r->written = true;
  return true;
}

VulkanBackend::ResolvedImage* VulkanBackend::EnsureImage(ResolvedImage& r, VkFormat format,
                                                        VkFormat format_signed, uint32_t width,
                                                        uint32_t height, const char* what,
                                                        VkFormat storage_format, bool depth) {
  if (r.image.image && r.format == format && r.format_signed == format_signed &&
      r.width == width && r.height == height &&
      (storage_format == VK_FORMAT_UNDEFINED || storage_format == r.storage_format) &&
      (r.depth.image != VK_NULL_HANDLE) == depth)
    return &r;
  if (r.image.image) {
    // A destination reused for another size or format: the frame being recorded may still use
    // the old image.
    gpu_->textures->ForgetResolvedImage(r.image.image, device_->frame_fence());
    if (r.depth.image) gpu_->textures->ForgetResolvedImage(r.depth.image, device_->frame_fence());
    gpu_->retired.push_back({r, device_->frame_fence()});
  }
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  const bool storage = storage_format != VK_FORMAT_UNDEFINED;
  ii.flags = format_signed != VK_FORMAT_UNDEFINED || (storage && storage_format != format)
                 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                 : 0;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {width, height, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  r = {};
  if (!gpu_->memory.CreateImage(ii, &r.image, what)) return nullptr;
  r.format = format;
  r.format_signed = format_signed;
  r.storage_format = storage_format;
  r.width = width;
  r.height = height;
  if (storage) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = storage_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (device_->fn().vkCreateImageView(device_->device(), &vi, nullptr, &r.storage_view) !=
        VK_SUCCESS) {
      r.storage_view = VK_NULL_HANDLE;
      gpu_->Destroy(r);
      return nullptr;
    }
  }
  if (depth) {
    ii.flags = 0;
    ii.format = VK_FORMAT_R32_SFLOAT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_STORAGE_BIT;
    if (!gpu_->memory.CreateImage(ii, &r.depth, "depth resolve image (R32_SFLOAT)")) {
      gpu_->Destroy(r);
      return nullptr;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.depth.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (device_->fn().vkCreateImageView(device_->device(), &vi, nullptr, &r.depth_storage_view) !=
        VK_SUCCESS) {
      r.depth_storage_view = VK_NULL_HANDLE;
      gpu_->Destroy(r);
      return nullptr;
    }
  }
  return &r;
}

bool VulkanBackend::CanBlit(VkFormat src, VkFormat dst) {
  const uint64_t key = uint64_t(src) << 32 | uint32_t(dst);
  auto it = blit_support_.find(key);
  if (it != blit_support_.end()) return it->second;
  VkFormatProperties ps, pd;
  device_->fn().vkGetPhysicalDeviceFormatProperties(device_->physical_device(), src, &ps);
  device_->fn().vkGetPhysicalDeviceFormatProperties(device_->physical_device(), dst, &pd);
  const bool ok = (ps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                  (pd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
  if (!ok)
    REXLOG_WARN("GRAW native Vulkan: no blit from format {} to {}: those resolves are skipped",
                int(src), int(dst));
  blit_support_.emplace(key, ok);
  return ok;
}

void VulkanBackend::GetShaderTextures(const SpirvShader& shader, const DrawState& state,
                                      uint8_t signs[32]) {
  using rex::graphics::xenos::DataDimension;
  using rex::graphics::xenos::FetchOpDimension;
  VkCommandBuffer cmd = device_->command_buffer();
  VulkanRenderTargets& targets = gpu_->targets;
  const auto end_rendering = [&] { targets.EndRendering(cmd); };
  for (const SpirvShader::TextureBinding& tb : shader.GetTextureBindingsAfterTranslation()) {
    VkImageView view = VK_NULL_HANDLE;
    VulkanTextureBinding b;
    ++texture_stats_.gets;
    if (gpu_->textures->Get(state.fetch(tb.fetch_constant), cmd, device_->frame_fence(),
                            end_rendering, &b)) {
      // The translator's image type: 2D array (1D, 2D, stacked), cube, or 3D.
      bool fits = tb.dimension == FetchOpDimension::kCube
                            ? b.dimension == DataDimension::kCube
                        : tb.dimension == FetchOpDimension::k3DOrStacked
                            ? b.dimension == DataDimension::k3D
                            : b.dimension != DataDimension::k3D &&
                                  b.dimension != DataDimension::kCube;
      const int32_t debug = REXCVAR_GET(graw_native_vulkan_debug);
      if ((debug & 4) && b.dimension == DataDimension::kCube) fits = false;
      if ((debug & 8) && !b.resolved) fits = false;
      if (REXCVAR_GET(graw_native_vulkan_debug_blank_fetch) >> tb.fetch_constant & 1) fits = false;
      if (fits) view = tb.is_signed ? b.view_signed : b.view_unsigned;
      signs[tb.fetch_constant] = b.swizzled_signs;
    }
    texture_views_.push_back(view);  // VK_NULL_HANDLE: the placeholder
  }
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(state.regs);
  for (const SpirvShader::SamplerBinding& sb : shader.GetSamplerBindingsAfterTranslation())
    samplers_.push_back(gpu_->bindings.Sampler(
        GetSamplerKey(regs, sb, rex::graphics::xenos::AnisoFilter::kDisabled)));
}

void VulkanBackend::RecordPipeline(const VulkanPipelineKey& key, const SpirvShader& vs,
                                   uint64_t vs_mod, const SpirvShader* ps, uint64_t ps_mod,
                                   uint32_t gs_key) {
  // The key without its handles; the geometry shader's key where its module was.
  VulkanPipelineKey stored = key;
  stored.vs = stored.ps = stored.gs = VK_NULL_HANDLE;
  stored.layout = VK_NULL_HANDLE;
  const uint64_t gs = gs_key;
  std::memcpy(&stored.gs, &gs, sizeof(gs));
  store_->RecordShader(uint32_t(vs.type()), vs.ucode_data_hash(), vs.ucode_dwords(),
                       vs.ucode_dword_count());
  if (ps)
    store_->RecordShader(uint32_t(ps->type()), ps->ucode_data_hash(), ps->ucode_dwords(),
                         ps->ucode_dword_count());
  store_->RecordPipeline(vs.ucode_data_hash(), vs_mod, ps ? ps->ucode_data_hash() : 0,
                         ps ? ps_mod : 0, &stored);
}

void VulkanBackend::Prewarm() {
  if (!store_) return;
  const std::vector<PipelineStore::Pipeline>& list = store_->loaded_pipelines();
  if (prewarm_next_ >= list.size()) return;
  namespace xenos = rex::graphics::xenos;
  const auto t0 = std::chrono::steady_clock::now();
  auto load = [&](uint64_t hash, xenos::ShaderType type) -> SpirvShader* {
    const PipelineStore::Shader* s = store_->loaded_shader(hash);
    if (!s || s->type != uint32_t(type) || s->ucode.empty()) return nullptr;
    return shaders_->Load(type, s->ucode.data(), uint32_t(s->ucode.size()));
  };
  do {
    const PipelineStore::Pipeline& sp = list[prewarm_next_++];
    VulkanPipelineKey key;
    if (sp.key.size() != sizeof(key)) {
      ++prewarm_failed_;
      continue;
    }
    std::memcpy(&key, sp.key.data(), sizeof(key));
    uint64_t gs_key = 0;
    std::memcpy(&gs_key, &key.gs, sizeof(gs_key));
    SpirvShader* vs = load(sp.vs_hash, xenos::ShaderType::kVertex);
    SpirvShader* ps = sp.ps_hash ? load(sp.ps_hash, xenos::ShaderType::kPixel) : nullptr;
    // A stored key comes from disk: values Vulkan would reject (a corrupt or foreign file)
    // drop it.
    if (!vs || (sp.ps_hash && !ps) || !IsValidStoredPipelineKey(key) || gs_key > UINT32_MAX) {
      ++prewarm_failed_;
      continue;
    }
    key.vs = shaders_->Module(*vs, sp.vs_mod);
    key.ps = ps ? shaders_->Module(*ps, sp.ps_mod) : VK_NULL_HANDLE;
    key.gs = VK_NULL_HANDLE;
    if (gs_key) {
      VulkanGeometryShaderKey g;
      g.key = uint32_t(gs_key);
      key.gs = shaders_->Geometry(g);
    }
    const VulkanBindings::Layout* layout = gpu_->bindings.GetLayout(*vs, ps);
    if (!key.vs || (ps && !key.ps) || (gs_key && !key.gs) || !layout) {
      ++prewarm_failed_;
      continue;
    }
    key.layout = layout->layout;
    gpu_->pipelines.Prewarm(key);
  } while (prewarm_next_ < list.size() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2));
  prewarm_ms_ +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ++prewarm_frames_;
  if (prewarm_next_ == list.size())
    REXLOG_INFO(
        "GRAW native Vulkan pipelines: prewarm queued {} pipelines ({} failed) in {:.1f} ms over "
        "{} frames",
        list.size() - prewarm_failed_, prewarm_failed_, prewarm_ms_, prewarm_frames_);
}

void VulkanBackend::BeginFrame(uint64_t frame_index) {
  frame_index_ = frame_index;
  frame_draw_ = 0;
  Prewarm();
  const std::string dump_targets = REXCVAR_GET(graw_native_vulkan_dump_target_draws);
  if (!dump_targets.empty() &&
      std::sscanf(dump_targets.c_str(), "%u-%u", &dump_target_first_, &dump_target_last_) != 2)
    dump_target_first_ = UINT32_MAX;
  const std::string skip = REXCVAR_GET(graw_native_vulkan_skip_draws);
  if (!skip.empty() && std::sscanf(skip.c_str(), "%u-%u", &skip_first_, &skip_last_) != 2)
    skip_first_ = UINT32_MAX;
  gpu_->targets.BeginFrame(frame_index, device_->frame_fence(), device_->completed_fence());
}

PrepareCounters VulkanBackend::EndFramePrepare(uint64_t done) {
  PrepareCounters c;
  if (mirror_.coherency) {
    mirror_.CheckLateWrites();
    c.stale_pages = std::exchange(mirror_.coherency->stale, 0);
    c.late_pages = std::exchange(mirror_.coherency->late, 0);
  }
  mirror_.watch_frame.fetch_add(1, std::memory_order_relaxed);
  gpu_->vertex_upload.EndFrame(done);
  c.upload_bytes = std::exchange(prep_upload_bytes_, 0);
  c.uploads = std::exchange(prep_uploads_, 0);
  c.diag = std::exchange(prep_diag_, {});
  return c;
}

void VulkanBackend::AddPrepareCounters(const PrepareCounters& c) {
  stats_.upload_bytes += c.upload_bytes;
  stats_.uploads += c.uploads;
  stats_.stale_pages += c.stale_pages;
  stats_.late_pages += c.late_pages;
}

void VulkanBackend::AfterSwap(uint64_t frame_index, uint64_t completed) {
  frame_index_ = frame_index;
  stats_.watch_faults = mirror_.watch_faults.exchange(0, std::memory_order_relaxed);
  stats_.watch_arms = mirror_.watch_arms.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_pages = mirror_.watch_arm_pages.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_changed = mirror_.watch_arm_changed.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_ns = mirror_.watch_arm_ns.exchange(0, std::memory_order_relaxed);
  stats_.hash_pages = mirror_.hash_pages.exchange(0, std::memory_order_relaxed);
  stats_.hash_copies = mirror_.hash_copies.exchange(0, std::memory_order_relaxed);
  gpu_->bindings.EndFrame(completed);
  gpu_->textures->EndFrame(completed);
  // After the frame's submission (the GPU runs it meanwhile): a held frame waits for the
  // pipelines its draws skipped.
  stats_.pso_wait_us +=
      gpu_->pipelines.WaitForFrame(pipeline_wait_ ? std::chrono::milliseconds(500)
                                                  : std::chrono::milliseconds(0));
  if (store_) store_->Flush();
  if (!gpu_->dump.empty()) DumpWrite(completed);
  for (size_t i = 0; i < gpu_->retired.size();) {
    if (gpu_->retired[i].fence <= completed) {
      gpu_->Destroy(gpu_->retired[i].image);
      gpu_->retired[i] = gpu_->retired.back();
      gpu_->retired.pop_back();
    } else {
      ++i;
    }
  }
  if (!threaded_) {
    mirror_.watch_frame.fetch_add(1, std::memory_order_relaxed);
    gpu_->vertex_upload.EndFrame(completed);
  }
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

bool VulkanBackend::SetPresentImage(const SwapCall& swap) {
  namespace xenos = rex::graphics::xenos;
  VulkanPresenter& presenter = gpu_->presenter;
  presenter.SetImage(VK_NULL_HANDLE, 0, 0, kRampNone, frame_index_);
  constexpr uint32_t kIdentity = 0 | 1 << 3 | 2 << 6 | 3 << 9;
  if (REXCVAR_GET(graw_native_vulkan_show_target)) {
    // The color target most of the frame's draws went to, as it is (no ramp).
    VulkanRenderTarget* best = nullptr;
    TargetUse best_use;
    for (const auto& [rt, use] : target_use_)
      if (use.draws > best_use.draws) {
        best = rt;
        best_use = use;
      }
    constexpr uint32_t kShowKey = 0xFFFFFFFFu;  // no guest address
    if (!best || !best->initialized || !best_use.rows ||
        !CopyTarget(best, 0, 0, int32_t(best->width), int32_t(best_use.rows), 0, 0, best->width,
                    best_use.rows, kShowKey))
      return false;
    const Gpu::Resolved& r = gpu_->resolved[kShowKey];
    presenter.SetImage(gpu_->textures->ResolvedView(r.image.image, r.format, kIdentity), r.width,
                       r.height, kRampNone, frame_index_);
    return true;
  }
  if (!swap.texture) return false;
  // The front buffer: the image the frame's resolve into its address made.
  const uint32_t address = GpuAddress(swap.fetch[1] & 0xFFFFF000u);
  auto it = gpu_->resolved.find(address);
  if (it == gpu_->resolved.end() || !it->second.written) {
    if (!front_buffer_logged_) {
      front_buffer_logged_ = true;
      REXLOG_WARN("GRAW native Vulkan: no resolved image for the front buffer {:08X} yet",
                  address);
    }
    return false;
  }
  const ResolvedImage& front = it->second;
  // As D3D12's present: the copy's destination format picks the ramp (PWL for 2_10_10_10, the
  // table for 8_8_8_8) and its host format the swizzle; other formats aren't shown.
  const xenos::TextureFormat format = xenos::TextureFormat(front.guest_format);
  const HostFormat* host = GetHostFormat(format);
  if (!host || (format != xenos::TextureFormat::k_8_8_8_8 &&
                format != xenos::TextureFormat::k_2_10_10_10)) {
    if (!front_format_logged_) {
      front_format_logged_ = true;
      REXLOG_WARN("GRAW native Vulkan: front buffer format {} not handled", uint32_t(format));
    }
    return false;
  }
  const uint32_t swizzle =
      GuestToHostComponentMapping((swap.fetch[3] >> 1) & 0xFFF, host->host_swizzle) & 0xFFF;
  const uint32_t width = std::min(front.written_width, front.width);
  const uint32_t height = std::min(front.written_height, front.height);
  const RampMode ramp = format == xenos::TextureFormat::k_2_10_10_10 ? kRampPwl : kRampTable;
  // The fetch constant's exp_adjust (dword 3, bits 13-18) isn't applied: the display controller
  // scans the front buffer out without the texture unit. The exponent bias the front buffer
  // has is its resolve's (the color resolve shader applies it).
  const int32_t exp_adjust = int32_t(swap.fetch[3] << 13) >> 26;
  const uint64_t seen = uint64_t(format) | uint64_t(swizzle) << 6 |
                        uint64_t(front.exp_bias & 63) << 18 | uint64_t(exp_adjust & 63) << 24 |
                        uint64_t(width) << 32 | uint64_t(height) << 48;
  if (seen != front_seen_ && front_logged_ < 8) {
    front_seen_ = seen;
    ++front_logged_;
    REXLOG_INFO("GRAW native Vulkan: front buffer {:08X} {}x{} format {}, swizzle {:04o}, "
                "{} ramp; resolve exponent bias {}, fetch exp_adjust {}",
                address, width, height, uint32_t(format), swizzle,
                ramp == kRampPwl ? "PWL" : "table", front.exp_bias, exp_adjust);
  }
  presenter.SetImage(gpu_->textures->ResolvedView(front.image.image, front.format, swizzle), width,
                     height, ramp, frame_index_);
  return true;
}

Backend::SwapResult VulkanBackend::Swap(const SwapCall& swap, std::optional<bool> own_window) {
  SwapResult r;
  if (own_window && !OwnGameWindow(*own_window)) return r;
  if (frame_index_ >= frames_logged_ + 150) {
    frames_logged_ = frame_index_;
    const VulkanShaderStats& s = shaders_->stats();
    REXLOG_INFO("GRAW native Vulkan shaders: {} shaders, {} translations ({:.0f} us total, max "
                "{:.0f} us), {} modules, {} failed",
                s.shaders, s.translations, s.translate_us, s.max_translate_us, s.modules,
                s.failures);
    const VulkanBindingStats& b = gpu_->bindings.stats();
    REXLOG_INFO("GRAW native Vulkan bindings: {} descriptor sets, {} constant uploads ({} KB), "
                "{} draws kept the constants set",
                b.sets, b.constant_uploads, b.constant_bytes >> 10, b.constant_set_reuses);
    const VulkanPipelineStats p = gpu_->pipelines.stats();
    REXLOG_INFO("GRAW native Vulkan pipelines: {} created ({} from the cache, {} prewarmed; "
                "{:.0f} us total, max {:.0f} us), {} failed, {} queued; {} render targets",
                p.created, p.cache_hits, p.prewarmed, p.create_us, p.max_create_us, p.failed,
                gpu_->pipelines.queued(), gpu_->targets.count());
    REXLOG_INFO("GRAW native Vulkan resolves: {} copied ({} depth, {} color by the shader), {} "
                "skipped, {} with clears; {} destination images; front buffer {}",
                resolves_, depth_resolves_, shader_resolves_, resolves_skipped_, resolve_clears_,
                gpu_->resolved.size(),
                front_buffer_found_ ? "shown" : "not found");
    const VulkanTextureStats& t = gpu_->textures->stats();
    REXLOG_INFO("GRAW native Vulkan textures: {} textures, {} created ({} with mips), {} "
                "uploads ({} KB), {} resolved binds ({} mismatched), {} unsupported, {} failed; "
                "{} samplers",
                gpu_->textures->count(), t.created, t.with_mips, t.uploads, t.upload_bytes >> 10,
                t.resolved_binds, t.resolved_mismatches, t.unsupported, t.failed, gpu_->bindings.sampler_count());
  }
  front_buffer_found_ = SetPresentImage(swap) && !REXCVAR_GET(graw_native_vulkan_show_target);
  target_use_.clear();
  // A frame without the draws whose pipelines were compiling: the previous frame stays on
  // screen while this one waits for them (AfterSwap).
  r.incomplete = pipeline_wait_ && gpu_->pipelines.frame_pending();
  r.fence = device_->frame_fence();
  gpu_->targets.EndRendering(device_->command_buffer());
  if (!device_->EndFrame(window_taken_ && !r.incomplete, gpu_->presenter)) return r;
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
