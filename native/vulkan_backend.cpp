// GRAW native renderer: the Vulkan backend (vulkan_backend.h).
#include "vulkan_backend.h"

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "game_window.h"
#include "guest_memory.h"  // GpuAddress
#include "present.h"       // SwapCall
#include "resolve.h"       // ResolveParams
#include "vulkan_bindings.h"
#include "vulkan_memory.h"
#include "vulkan_pipelines.h"
#include "vulkan_render_targets.h"
#include "vulkan_shaders.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

REXCVAR_DEFINE_BOOL(graw_native_vulkan_validation, false, "GRAW native",
                    "With --graw_native_api=vulkan: enable the Khronos validation layer when it "
                    "is installed (the Vulkan SDK); its errors and warnings go to the log");
REXCVAR_DEFINE_BOOL(graw_native_vulkan_show_target, false, "GRAW native",
                    "With --graw_native_api=vulkan: show the color render target most of the "
                    "frame's draws went to (the 3D scene) instead of the front buffer, to check "
                    "the geometry before textures exist (M3d debugging)");
REXCVAR_DECLARE(bool, graw_native_render_thread);
REXCVAR_DECLARE(std::string, graw_native_watch_trace);
REXCVAR_DECLARE(bool, graw_native_coherency_check);

namespace graw::native {

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
  // M3d: the images color resolves copied their source rectangles into, by destination address
  // (physical); the Swap's front buffer is one of them. Between uses in TRANSFER_SRC_OPTIMAL.
  struct Resolved {
    VulkanMemory::Image image;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool written = false;
  };
  std::unordered_map<uint32_t, Resolved> resolved;
  explicit Gpu(VulkanDevice& device) : bindings(device), pipelines(device), targets(device) {}
  ~Gpu() {
    for (auto& [address, r] : resolved) memory.Destroy(r.image);
    memory.Destroy(shared);
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
  threaded_ = REXCVAR_GET(graw_native_render_thread);
  if (!mirror_.Initialize(memory, this, REXCVAR_GET(graw_native_watch_trace),
                          REXCVAR_GET(graw_native_coherency_check)))
    return false;
  mirror_.SetCounters(threaded_ ? &prep_upload_bytes_ : &stats_.upload_bytes,
                      threaded_ ? &prep_uploads_ : &stats_.uploads,
                      threaded_ ? &prep_diag_ : &diag_);
  REXLOG_INFO(
      "GRAW native Vulkan: up (M3c-3: draws recorded into host render targets; the frame isn't "
      "shown yet, cleared frames)");
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

void VulkanBackend::Draw(const DrawState& state, const DrawPrep* prep) {
  using rex::graphics::xenos::ShaderType;
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

  // Host primitive. Points, rectangles and quads need the geometry shaders of
  // geometry_shader.cpp (next in M3); fans are native on Vulkan.
  VkPrimitiveTopology topology;
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
      return unsupported("point list (geometry shader)");
    case xenos::PrimitiveType::kRectangleList:
      return unsupported("rectangle list (geometry shader)");
    case xenos::PrimitiveType::kQuadList:
      return unsupported("quad list (geometry shader)");
    default:
      return unsupported("primitive type");
  }
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
  static constexpr uint8_t kUnsignedSigns[32] = {};  // placeholder textures (M4: real ones)
  SpirvShaderTranslator::SystemConstants sys;
  FillSpirvSystemConstants(regs, prim, polygonal, index_endian, vp, used_textures, kUnsignedSigns,
                           sys);

  VulkanBindings& bindings = gpu_->bindings;
  VulkanRenderTargets& targets = gpu_->targets;
  VkCommandBuffer cmd = device_->command_buffer();
  if (!bindings.placeholders_ready()) {
    targets.EndRendering(cmd);
    bindings.InitializePlaceholders(cmd);
  }
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
  if (!layout || !bindings.Bind(cmd, *layout, state.regs, *vs, ps, sys, device_->frame_fence())) {
    ++stats_.failed;
    return;
  }

  // The pipeline.
  VulkanPipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = shaders_->Module(*vs, mods.vertex);
  key.ps = ps ? shaders_->Module(*ps, mods.pixel) : VK_NULL_HANDLE;
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
  const VkPipeline pipeline = gpu_->pipelines.Get(key);
  if (!pipeline) {
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
}

void VulkanBackend::Clear(const ClearCall& clear) {
  gpu_->targets.Clear(device_->command_buffer(), clear);
}

void VulkanBackend::Resolve(const ResolveParams& p) {
  // Color copies only (depth resolves feed the game's own reads: M5).
  if (p.source > 3 || p.copy_command != 1) {
    ++resolves_skipped_;
    return;
  }
  VulkanRenderTargets& targets = gpu_->targets;
  VkCommandBuffer cmd = device_->command_buffer();
  targets.EndRendering(cmd);
  VulkanRenderTarget* rt =
      targets.GetColor(cmd, p.regs, p.source, uint32_t(std::clamp(p.y1, 1, 2048)));
  if (!rt || !rt->initialized) {
    ++resolves_skipped_;
    return;
  }
  // The source rectangle, clipped to the target and to the destination level.
  const int32_t x0 = std::max(p.x0, 0), y0 = std::max(p.y0, 0);
  const int32_t x1 = std::min(p.x1, int32_t(rt->width)), y1 = std::min(p.y1, int32_t(rt->height));
  const int32_t dx = std::max(p.dest_x, 0), dy = std::max(p.dest_y, 0);
  const int32_t w = std::min(x1 - x0, int32_t(p.dest_width) - dx);
  const int32_t h = std::min(y1 - y0, int32_t(p.dest_height) - dy);
  if (w <= 0 || h <= 0 || !p.dest_width || !p.dest_height ||
      !CopyTarget(rt, x0, y0, w, h, dx, dy, p.dest_width, p.dest_height, p.dest_base)) {
    ++resolves_skipped_;
    return;
  }
  ++resolves_;
}

bool VulkanBackend::CopyTarget(VulkanRenderTarget* rt, int32_t x0, int32_t y0, int32_t w,
                               int32_t h, int32_t dx, int32_t dy, uint32_t dest_w,
                               uint32_t dest_h, uint32_t key) {
  VkCommandBuffer cmd = device_->command_buffer();
  gpu_->targets.EndRendering(cmd);
  Gpu::Resolved& r = gpu_->resolved[key];
  if (!r.image.image || r.format != rt->format || r.width != dest_w || r.height != dest_h) {
    if (r.image.image) {
      // Rare (a destination reused for another size or format): wait out its users.
      device_->WaitIdle();
      gpu_->memory.Destroy(r.image);
    }
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = rt->format;
    ii.extent = {dest_w, dest_h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!gpu_->memory.CreateImage(ii, &r.image, "resolve destination image")) {
      gpu_->resolved.erase(key);
      return false;
    }
    r.format = rt->format;
    r.width = dest_w;
    r.height = dest_h;
    r.written = false;
  }
  const VulkanDevice::Functions& fn = device_->fn();
  VkImageMemoryBarrier b[2] = {};
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
  b[1].image = r.image.image;
  b[1].oldLayout = r.written ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fn.vkCmdPipelineBarrier(cmd,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
  const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  if (rt->samples > 1) {
    const VkImageResolve region{layers, {x0, y0, 0}, layers, {dx, dy, 0},
                                {uint32_t(w), uint32_t(h), 1}};
    fn.vkCmdResolveImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         r.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  } else {
    const VkImageCopy region{layers, {x0, y0, 0}, layers, {dx, dy, 0},
                             {uint32_t(w), uint32_t(h), 1}};
    fn.vkCmdCopyImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.image.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 2, b);
  r.written = true;
  return true;
}

void VulkanBackend::BeginFrame(uint64_t frame_index) {
  frame_index_ = frame_index;
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
    const VulkanBindingStats& b = gpu_->bindings.stats();
    REXLOG_INFO("GRAW native Vulkan bindings: {} descriptor sets, {} constant uploads ({} KB), "
                "{} draws kept the constants set",
                b.sets, b.constant_uploads, b.constant_bytes >> 10, b.constant_set_reuses);
    const VulkanPipelineStats& p = gpu_->pipelines.stats();
    REXLOG_INFO("GRAW native Vulkan pipelines: {} created ({:.0f} us total, max {:.0f} us), {} "
                "failed; {} render targets",
                p.created, p.create_us, p.max_create_us, p.failed, gpu_->targets.count());
    REXLOG_INFO("GRAW native Vulkan resolves: {} copied, {} skipped; {} destination images; "
                "front buffer {}",
                resolves_, resolves_skipped_, gpu_->resolved.size(),
                front_buffer_found_ ? "shown" : "not found");
  }
  // The front buffer: the image the frame's resolve into its address made.
  VulkanDevice::FrameSource source;
  const VulkanDevice::FrameSource* shown = nullptr;
  front_buffer_found_ = false;
  if (REXCVAR_GET(graw_native_vulkan_show_target)) {
    VulkanRenderTarget* best = nullptr;
    TargetUse best_use;
    for (const auto& [rt, use] : target_use_)
      if (use.draws > best_use.draws) {
        best = rt;
        best_use = use;
      }
    constexpr uint32_t kShowKey = 0xFFFFFFFFu;  // no guest address
    if (best && best->initialized && best_use.rows &&
        CopyTarget(best, 0, 0, int32_t(best->width), int32_t(best_use.rows), 0, 0, best->width,
                   best_use.rows, kShowKey)) {
      const Gpu::Resolved& r = gpu_->resolved[kShowKey];
      source.image = r.image.image;
      source.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      source.x1 = int32_t(r.width);
      source.y1 = int32_t(r.height);
      shown = &source;
    }
  } else if (swap.texture) {
    const uint32_t address = GpuAddress(swap.fetch[1] & 0xFFFFF000u);
    auto it = gpu_->resolved.find(address);
    if (it != gpu_->resolved.end() && it->second.written) {
      source.image = it->second.image.image;
      source.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      source.x1 = int32_t(it->second.width);
      source.y1 = int32_t(it->second.height);
      shown = &source;
      front_buffer_found_ = true;
    } else if (!front_buffer_logged_) {
      front_buffer_logged_ = true;
      REXLOG_WARN("GRAW native Vulkan: no resolved image for the front buffer {:08X} yet",
                  address);
    }
  }
  target_use_.clear();
  r.fence = device_->frame_fence();
  gpu_->targets.EndRendering(device_->command_buffer());
  if (!device_->EndFrame(window_taken_, color, shown)) return r;
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
