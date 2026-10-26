// GRAW native renderer: the Vulkan backend's host render targets (vulkan_render_targets.h).
#include "vulkan_render_targets.h"

#include "log.h"

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstring>

namespace graw::native {

namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint32_t kEdramTiles = 2048;  // 10 MiB of 80x16-sample tiles
constexpr uint32_t kMaxHeight = 2048;

// Rows of pixels the EDRAM holds from `base` for this surface, as render_targets.cpp.
uint32_t EdramRows(uint32_t base, uint32_t pitch, uint32_t samples, bool wide) {
  const uint32_t sx = samples >= 4 ? 2 : 1, sy = samples >= 2 ? 2 : 1;
  const uint32_t tiles_x = std::max(1u, (pitch * sx * (wide ? 2 : 1) + 79) / 80);
  const uint32_t free_tiles = base < kEdramTiles ? kEdramTiles - base : 0;
  return (free_tiles / tiles_x) * 16 / sy;
}

bool Is64bpp(uint32_t f) {
  return xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat(f));
}

}  // namespace

VkFormat VulkanRenderTargets::ColorFormat(uint32_t f) {
  switch (f) {
    case 0:  // k_8_8_8_8
    case 1:  // k_8_8_8_8_GAMMA (gamma not applied on the host yet, as on D3D12)
      return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3:   // k_2_10_10_10_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    case 7:   // k_16_16_16_16_FLOAT
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4:
      return VK_FORMAT_R16G16_SNORM;
    case 5:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case 6:
      return VK_FORMAT_R16G16_SFLOAT;
    case 14:
      return VK_FORMAT_R32_SFLOAT;
    case 15:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargets::DepthFormat(uint32_t guest_format) const {
  // kD24FS8 (20e4 float depth) as 32-bit float; kD24S8 as 24-bit unorm where the GPU has it
  // (not AMD's: there, 32-bit float too).
  return guest_format ? VK_FORMAT_D32_SFLOAT_S8_UINT : d24s8_;
}

VulkanRenderTargets::~VulkanRenderTargets() {
  VkDevice d = device_.device();
  if (!d || !memory_) return;
  for (auto& [key, rt] : targets_) Retire(*rt);
  for (Retired& r : retired_) {
    if (r.view) device_.fn().vkDestroyImageView(d, r.view, nullptr);
    if (r.depth_view) device_.fn().vkDestroyImageView(d, r.depth_view, nullptr);
    if (r.stencil_view) device_.fn().vkDestroyImageView(d, r.stencil_view, nullptr);
    memory_->Destroy(r.image);
  }
}

bool VulkanRenderTargets::Initialize(VulkanMemory& memory) {
  memory_ = &memory;
  VkFormatProperties p{};
  device_.fn().vkGetPhysicalDeviceFormatProperties(device_.physical_device(),
                                                   VK_FORMAT_D24_UNORM_S8_UINT, &p);
  d24s8_ = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
               ? VK_FORMAT_D24_UNORM_S8_UINT
               : VK_FORMAT_D32_SFLOAT_S8_UINT;
  REXLOG_INFO("GRAW native Vulkan: render targets: D24S8 depth as {}",
              d24s8_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
  return true;
}

void VulkanRenderTargets::Retire(VulkanRenderTarget& rt) {
  if (rt.image.image)
    retired_.push_back({rt.image, rt.view, rt.depth_view, rt.stencil_view, frame_fence_});
  rt.image = {};
  rt.view = VK_NULL_HANDLE;
  rt.depth_view = VK_NULL_HANDLE;
  rt.stencil_view = VK_NULL_HANDLE;
}

bool VulkanRenderTargets::Create(VulkanRenderTarget* rt, uint32_t height) {
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = rt->format;
  ii.extent = {std::max(1u, rt->pitch), height, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VkSampleCountFlagBits(rt->samples);
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = (rt->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VulkanMemory::Image image;
  if (!memory_->CreateImage(ii, &image, "render target")) return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = rt->format;
  vi.subresourceRange = {rt->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                         VK_IMAGE_ASPECT_STENCIL_BIT)
                                   : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                         0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  if (device_.fn().vkCreateImageView(device_.device(), &vi, nullptr, &view) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: render target view failed");
    memory_->Destroy(image);
    return false;
  }
  if (rt->image.image) {
    REXLOG_WARN("GRAW native Vulkan RT: growing {} base {} pitch {} x{} from {} to {} rows",
                rt->depth ? "depth" : "color", rt->edram_base, rt->pitch, rt->samples, rt->height,
                height);
    Retire(*rt);
  }
  rt->image = image;
  rt->view = view;
  rt->initialized = false;
  rt->width = ii.extent.width;
  rt->height = height;
  return true;
}

VulkanRenderTarget* VulkanRenderTargets::Get(VkCommandBuffer cmd, bool depth, uint32_t surface_info,
                                             uint32_t info, uint32_t height) {
  const uint32_t pitch = surface_info & 0x3FFF;
  const uint32_t samples = SurfaceSamples(surface_info);
  const uint32_t base = info & 0xFFF;
  if (pitch == 0) return nullptr;
  uint32_t guest_format;
  VkFormat format;
  bool wide = false;
  if (depth) {
    guest_format = (info >> 16) & 1;
    format = DepthFormat(guest_format);
  } else {
    guest_format = (info >> 16) & 0xF;
    format = ColorFormat(guest_format);
    if (format == VK_FORMAT_UNDEFINED) {
      static uint32_t warned = 0;
      if (warned++ < 8)
        REXLOG_WARN("GRAW native Vulkan RT: unsupported color format {}", guest_format);
      return nullptr;
    }
    wide = Is64bpp(guest_format);
  }
  const uint64_t key = (uint64_t(depth) << 63) | (uint64_t(samples >> 1) << 60) |
                       (uint64_t(base) << 44) | (uint64_t(pitch) << 30) | uint64_t(format);
  uint32_t want = std::max(std::min(height, kMaxHeight), EdramRows(base, pitch, samples, wide));
  want = std::min((std::max(want, 8u) + 7) & ~7u, kMaxHeight);
  auto it = targets_.find(key);
  VulkanRenderTarget* rt;
  if (it == targets_.end()) {
    auto owned = std::make_unique<VulkanRenderTarget>();
    rt = owned.get();
    rt->depth = depth;
    rt->edram_base = base;
    rt->pitch = pitch;
    rt->samples = samples;
    rt->format = format;
    if (!Create(rt, want)) return nullptr;
    targets_.emplace(key, std::move(owned));
  } else {
    rt = it->second.get();
    if (std::min(height, kMaxHeight) > rt->height) {
      // No rendering instance may stay open on the old image.
      EndRendering(cmd);
      if (!Create(rt, want)) return nullptr;
    }
  }
  rt->guest_format = guest_format;
  rt->last_frame = frame_;
  return rt;
}

void VulkanRenderTargets::EndRendering(VkCommandBuffer cmd) {
  if (!rendering_) return;
  device_.fn().vkCmdEndRendering(cmd);
  rendering_ = false;
}

void VulkanRenderTargets::BeginRendering(VkCommandBuffer cmd, VulkanRenderTarget* const* color,
                                         uint32_t color_count, VulkanRenderTarget* depth) {
  if (rendering_ && open_color_count_ == color_count && open_depth_ == depth &&
      std::equal(color, color + color_count, open_color_.begin()))
    return;
  EndRendering(cmd);
  const VulkanDevice::Functions& fn = device_.fn();
  for (uint32_t i = 0; i < color_count; ++i)
    if (color[i] && !color[i]->initialized) Initialize(cmd, color[i]);
  if (depth && !depth->initialized) Initialize(cmd, depth);
  // Earlier rendering instances' writes before this one's.
  VkImageMemoryBarrier barriers[5];
  uint32_t n = 0;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  auto add = [&](VulkanRenderTarget* rt) {
    VkImageMemoryBarrier& b = barriers[n++];
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = rt->image.image;
    if (rt->depth) {
      b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
      b.srcAccessMask = b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    } else {
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.srcAccessMask = b.dstAccessMask =
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    width = std::min(width, rt->width);
    height = std::min(height, rt->height);
  };
  for (uint32_t i = 0; i < color_count; ++i)
    if (color[i]) add(color[i]);
  if (depth) add(depth);
  if (!n) return;
  constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  fn.vkCmdPipelineBarrier(cmd, kStages, kStages, 0, 0, nullptr, 0, nullptr, n, barriers);

  VkRenderingAttachmentInfo colors[4] = {};
  for (uint32_t i = 0; i < color_count; ++i) {
    colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (!color[i]) continue;  // imageView VK_NULL_HANDLE: no attachment at this index
    colors[i].imageView = color[i]->view;
    colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingAttachmentInfo ds{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (depth) {
    ds.imageView = depth->view;
    ds.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    ds.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ds.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {width, height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = color_count;
  ri.pColorAttachments = color_count ? colors : nullptr;
  ri.pDepthAttachment = depth ? &ds : nullptr;
  ri.pStencilAttachment = depth ? &ds : nullptr;
  fn.vkCmdBeginRendering(cmd, &ri);
  rendering_ = true;
  open_color_ = {};
  std::copy(color, color + color_count, open_color_.begin());
  open_color_count_ = color_count;
  open_depth_ = depth;
}

bool VulkanRenderTargets::Bind(VkCommandBuffer cmd, const SurfaceRegs& regs, uint32_t color_mask,
                               bool depth_used, uint32_t height, VulkanBoundTargets* out) {
  *out = VulkanBoundTargets{};
  out->samples = SurfaceSamples(regs.surface_info);
  out->width = out->height = UINT32_MAX;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(color_mask >> i & 1)) continue;
    VulkanRenderTarget* rt = Get(cmd, false, regs.surface_info, regs.color_info[i], height);
    if (!rt) return false;
    if (rt->fill_seq != fills_.seq()) SyncEdram(cmd, rt);
    out->color[i] = rt;
    out->color_count = i + 1;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (depth_used) {
    VulkanRenderTarget* rt = Get(cmd, true, regs.surface_info, regs.depth_info, height);
    if (!rt) return false;
    out->depth = rt;
    out->width = std::min(out->width, rt->width);
    out->height = std::min(out->height, rt->height);
  }
  if (out->width == UINT32_MAX) {
    out->width = out->height = 0;
    return true;  // no attachments: rasterization only (e.g. memexport); nothing to begin
  }
  // The draw renders over these tiles (conservatively: its targets' rows above `height`).
  if (!fills_.empty()) {
    for (uint32_t i = 0; i < out->color_count; ++i)
      if (out->color[i]) fills_.Drop(GridOf(*out->color[i]), height);
    if (out->depth) fills_.Drop(GridOf(*out->depth), height);
  }
  BeginRendering(cmd, out->color, out->color_count, out->depth);
  return true;
}

void VulkanRenderTargets::Initialize(VkCommandBuffer cmd, VulkanRenderTarget* rt) {
  if (rt->initialized) return;
  EndRendering(cmd);
  const VulkanDevice::Functions& fn = device_.fn();
  const VkImageSubresourceRange range{
      rt->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
      0, 1, 0, 1};
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = rt->image.image;
  b.subresourceRange = range;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b);
  if (rt->depth) {
    const VkClearDepthStencilValue zero{0.0f, 0};
    fn.vkCmdClearDepthStencilImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &zero, 1, &range);
  } else {
    const VkClearColorValue zero{};
    fn.vkCmdClearColorImage(cmd, rt->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                            &range);
  }
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  VkPipelineStageFlags stages;
  if (rt->depth) {
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                      VK_ACCESS_SHADER_READ_BIT;
    stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  } else {
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT;
    stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }
  fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          stages | VK_PIPELINE_STAGE_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b);
  rt->initialized = true;
}

void VulkanRenderTargets::ClearRects(VkCommandBuffer cmd, VulkanRenderTarget* rt,
                                     const VkClearAttachment& att, const Rect* rects,
                                     size_t count) {
  VulkanRenderTarget* none[1] = {nullptr};
  if (rt->depth)
    BeginRendering(cmd, none, 0, rt);
  else
    BeginRendering(cmd, &rt, 1, nullptr);
  std::vector<VkClearRect> out;
  if (!count) {
    out.push_back({{{0, 0}, {rt->width, rt->height}}, 0, 1});
  } else {
    for (size_t i = 0; i < count; ++i) {
      const Rect& r = rects[i];
      const int32_t x0 = std::clamp<int32_t>(r.left, 0, int32_t(rt->width));
      const int32_t y0 = std::clamp<int32_t>(r.top, 0, int32_t(rt->height));
      const int32_t x1 = std::clamp<int32_t>(r.right, 0, int32_t(rt->width));
      const int32_t y1 = std::clamp<int32_t>(r.bottom, 0, int32_t(rt->height));
      if (x1 > x0 && y1 > y0)
        out.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
  }
  if (!out.empty())
    device_.fn().vkCmdClearAttachments(cmd, 1, &att, uint32_t(out.size()), out.data());
}

void VulkanRenderTargets::SyncEdram(VkCommandBuffer cmd, VulkanRenderTarget* rt) {
  const uint64_t since = rt->fill_seq;
  rt->fill_seq = fills_.seq();
  if (rt->depth || since == rt->fill_seq) return;
  fills_.ForEach(since, GridOf(*rt), rt->guest_format, rt->width, rt->height,
                 [&](const EdramFills::Fill& f, const Rect* rects, uint32_t n) {
    VkClearAttachment att{};
    att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    std::memcpy(att.clearValue.color.float32, f.color, sizeof(f.color));
    ClearRects(cmd, rt, att, rects, n);
    GRAW_LOG_FIRST(4, INFO,
                   "GRAW native Vulkan RT: EDRAM fill at tile {} pitch {} (alpha {}) applied to "
                   "base {} pitch {} x{} rows {}-{}",
                   f.base, f.pitch, f.color[3], rt->edram_base, rt->pitch, rt->samples,
                   rects[0].top, rects[n - 1].bottom);
  });
}

void VulkanRenderTargets::PrepareRead(VkCommandBuffer cmd, VulkanRenderTarget* rt) {
  Initialize(cmd, rt);
  if (!rt->depth && rt->fill_seq != fills_.seq()) SyncEdram(cmd, rt);
  EndRendering(cmd);
}

VkImageView VulkanRenderTargets::DepthView(VulkanRenderTarget* rt) {
  return AspectView(rt, VK_IMAGE_ASPECT_DEPTH_BIT, &rt->depth_view);
}

VkImageView VulkanRenderTargets::StencilView(VulkanRenderTarget* rt) {
  return AspectView(rt, VK_IMAGE_ASPECT_STENCIL_BIT, &rt->stencil_view);
}

VkImageView VulkanRenderTargets::AspectView(VulkanRenderTarget* rt, VkImageAspectFlags aspect,
                                            VkImageView* view) {
  if (!rt->depth) return VK_NULL_HANDLE;
  if (*view) return *view;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = rt->image.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = rt->format;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};
  if (device_.fn().vkCreateImageView(device_.device(), &vi, nullptr, view) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: {} sampling view failed",
                 aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? "depth" : "stencil");
    *view = VK_NULL_HANDLE;
  }
  return *view;
}

void VulkanRenderTargets::Clear(VkCommandBuffer cmd, const ClearCall& c) {
  const ClearParams& p = c.p;
  // What the clear leaves in the EDRAM, in pixels from the surface's base. A whole-surface
  // clear covers the rows of the call's height hint (the target itself may be taller).
  const std::vector<Rect>& edram = p.edram_rects.empty() ? p.rects : p.edram_rects;
  auto edram_rows = [&](const VulkanRenderTarget& rt) {
    if (edram.empty()) return c.height ? std::min(c.height, rt.height) : rt.height;
    uint32_t rows = 0;
    for (const Rect& r : edram) rows = std::max(rows, uint32_t(std::max<int32_t>(r.bottom, 0)));
    return rows;
  };
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(p.flags >> i & 1) || !(c.bound_colors >> i & 1)) continue;
    VulkanRenderTarget* rt = Get(cmd, false, c.regs.surface_info, c.regs.color_info[i], c.height);
    if (!rt) continue;
    // Outside the rectangles the target shows what the EDRAM holds there.
    if (!p.rects.empty() && rt->fill_seq != fills_.seq()) SyncEdram(cmd, rt);
    VkClearAttachment att{};
    att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    att.colorAttachment = 0;
    std::memcpy(att.clearValue.color.float32, p.color, sizeof(p.color));
    ClearRects(cmd, rt, att, p.rects.data(), p.rects.size());
    if (edram.empty()) {
      fills_.Record(GridOf(*rt), rt->guest_format, {0, 0, int32_t(rt->pitch), int32_t(edram_rows(*rt))},
                    p.color);
    } else {
      for (const Rect& r : edram) fills_.Record(GridOf(*rt), rt->guest_format, r, p.color);
    }
    rt->fill_seq = fills_.seq();
  }
  VkImageAspectFlags aspects = 0;
  if (p.flags & 0x10) aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if (p.flags & 0x20) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
  if (aspects && c.depth_bound) {
    VulkanRenderTarget* rt = Get(cmd, true, c.regs.surface_info, c.regs.depth_info, c.height);
    if (rt) {
      VkClearAttachment att{};
      att.aspectMask = aspects;
      att.clearValue.depthStencil = {std::clamp(p.depth, 0.0f, 1.0f), p.stencil};
      ClearRects(cmd, rt, att, p.rects.data(), p.rects.size());
      if (!fills_.empty()) fills_.Drop(GridOf(*rt), edram_rows(*rt));
    }
  }
}

void VulkanRenderTargets::BeginFrame(uint64_t frame, uint64_t frame_fence,
                                     uint64_t completed_fence) {
  frame_ = frame;
  frame_fence_ = frame_fence;
  for (size_t i = 0; i < retired_.size();) {
    if (retired_[i].fence <= completed_fence) {
      if (retired_[i].view) device_.fn().vkDestroyImageView(device_.device(), retired_[i].view, nullptr);
      if (retired_[i].depth_view)
        device_.fn().vkDestroyImageView(device_.device(), retired_[i].depth_view, nullptr);
      if (retired_[i].stencil_view)
        device_.fn().vkDestroyImageView(device_.device(), retired_[i].stencil_view, nullptr);
      memory_->Destroy(retired_[i].image);
      retired_[i] = retired_.back();
      retired_.pop_back();
    } else {
      ++i;
    }
  }
}

}  // namespace graw::native
