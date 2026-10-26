// GRAW native renderer: the Vulkan backend's host render targets for the guest's EDRAM surfaces
// (docs/VULKAN_RENDERER.md, M3c-3). The counterpart of render_targets.h, same model: no EDRAM
// emulation, one host image per guest surface (EDRAM base, pitch, MSAA, host format), at least as
// tall as the EDRAM holds from its base.
//
// Rendering uses dynamic rendering: a rendering instance stays open across draws while their
// targets don't change, and is ended before anything that can't run inside one (copies into
// shared memory, the frame's end). Clears are recorded when the guest makes them, as
// vkCmdClearAttachments in a rendering instance on the cleared target (rectangles or all of it),
// and as render_targets.h's EDRAM fills (a clear's value read by another surface over the same
// tiles), applied the same way. New targets start zeroed, as D3D12's committed resources do.
// Not yet: dropping clears a later one covers (render_targets.h defers them).
//
// Threading: the recording thread.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "vulkan_memory.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "surfaces.h"  // SurfaceRegs, ClearCall

namespace graw::native {

struct VulkanRenderTarget {
  bool depth = false;
  uint32_t edram_base = 0;  // tiles
  uint32_t pitch = 0;       // pixels
  uint32_t samples = 1;     // 1, 2 or 4
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VulkanMemory::Image image;
  VkImageView view = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;  // depth: the depth aspect alone, for sampling
  bool initialized = false;  // in its attachment layout (false: just created, UNDEFINED)
  uint64_t last_frame = 0;
  uint64_t fill_seq = 0;  // EDRAM fills up to this sequence are applied (SyncEdram)
};

struct VulkanBoundTargets {
  VulkanRenderTarget* color[4] = {};
  uint32_t color_count = 0;  // highest bound color index + 1
  VulkanRenderTarget* depth = nullptr;
  uint32_t samples = 1;
  uint32_t width = 0, height = 0;  // smallest bound target
};

class VulkanRenderTargets {
 public:
  explicit VulkanRenderTargets(VulkanDevice& device) : device_(device) {}
  ~VulkanRenderTargets();

  bool Initialize(VulkanMemory& memory);

  // Host formats of guest color / depth render target formats (UNDEFINED: unsupported).
  static VkFormat ColorFormat(uint32_t guest_format);
  VkFormat DepthFormat(uint32_t guest_format) const;

  // The targets of a draw (color_mask bit i: guest RT i written; depth_used: depth or stencil
  // test/write), created if missing, and a rendering instance on them (the open one if it has
  // the same attachments). height: rows the draw can touch.
  bool Bind(VkCommandBuffer cmd, const SurfaceRegs& regs, uint32_t color_mask, bool depth_used,
            uint32_t height, VulkanBoundTargets* out);
  // The guest's Clear / ClearF, and a Resolve's clears (bound_colors 0xF, depth_bound).
  void Clear(VkCommandBuffer cmd, const ClearCall& c);
  // Before reading rt outside a draw (resolve sources): zeroed if new, and the EDRAM fills
  // over its tiles since its last use applied. Ends the open rendering instance.
  void PrepareRead(VkCommandBuffer cmd, VulkanRenderTarget* rt);
  // A depth target's depth aspect view (created on first use), for sampling.
  VkImageView DepthView(VulkanRenderTarget* rt);
  // Forgets every recorded EDRAM fill.
  void DropAllFills() { fills_.Clear(); }
  // The host target of guest color RT `index` (0..3), created if missing (resolve sources);
  // nullptr for an unsupported format. Between draws its layout is COLOR_ATTACHMENT_OPTIMAL
  // (`initialized`) or UNDEFINED.
  VulkanRenderTarget* GetColor(VkCommandBuffer cmd, const SurfaceRegs& regs, uint32_t index,
                               uint32_t height) {
    return Get(cmd, false, regs.surface_info, regs.color_info[index & 3], height);
  }
  // The host target of the depth surface, created if missing (depth resolve sources).
  VulkanRenderTarget* GetDepth(VkCommandBuffer cmd, const SurfaceRegs& regs, uint32_t height) {
    return Get(cmd, true, regs.surface_info, regs.depth_info, height);
  }
  // Ends the open rendering instance, if any.
  void EndRendering(VkCommandBuffer cmd);
  bool rendering() const { return rendering_; }

  // Once per frame before recording it: frees replaced images the GPU is done with.
  void BeginFrame(uint64_t frame, uint64_t frame_fence, uint64_t completed_fence);

  size_t count() const { return targets_.size(); }

 private:
  // Growing a target ends the open rendering instance (recorded into cmd).
  VulkanRenderTarget* Get(VkCommandBuffer cmd, bool depth, uint32_t surface_info, uint32_t info,
                          uint32_t height);
  bool Create(VulkanRenderTarget* rt, uint32_t height);
  void Retire(VulkanRenderTarget& rt);
  // Zeroes a new target and moves it to its attachment layout (outside rendering instances).
  void Initialize(VkCommandBuffer cmd, VulkanRenderTarget* rt);
  // vkCmdClearAttachments of `att` over `rects` (none: all of rt) in a rendering instance on rt.
  void ClearRects(VkCommandBuffer cmd, VulkanRenderTarget* rt, const VkClearAttachment& att,
                  const Rect* rects, size_t count);
  // Clears color target rt to the EDRAM fills recorded over its tiles since its last use.
  void SyncEdram(VkCommandBuffer cmd, VulkanRenderTarget* rt);
  static EdramGrid GridOf(const VulkanRenderTarget& rt) {
    return GetEdramGrid(rt.depth, rt.edram_base, rt.pitch, rt.samples, rt.guest_format);
  }
  // Begins rendering on these attachments (color[i] may be null) unless that's what's open.
  void BeginRendering(VkCommandBuffer cmd, VulkanRenderTarget* const* color, uint32_t color_count,
                      VulkanRenderTarget* depth);

  VulkanDevice& device_;
  VulkanMemory* memory_ = nullptr;
  VkFormat d24s8_ = VK_FORMAT_UNDEFINED;  // D24_UNORM_S8_UINT if supported, else D32_SFLOAT_S8
  std::unordered_map<uint64_t, std::unique_ptr<VulkanRenderTarget>> targets_;
  struct Retired {
    VulkanMemory::Image image;
    VkImageView view, depth_view;
    uint64_t fence;
  };
  std::vector<Retired> retired_;
  uint64_t frame_ = 0, frame_fence_ = 0;
  // The open rendering instance's attachments.
  bool rendering_ = false;
  std::array<VulkanRenderTarget*, 4> open_color_{};
  uint32_t open_color_count_ = 0;
  VulkanRenderTarget* open_depth_ = nullptr;
  EdramFills fills_;
};

}  // namespace graw::native
