// GRAW native renderer: the Vulkan backend's host render targets for the guest's EDRAM surfaces
// (docs/VULKAN_RENDERER.md, M3c-3). The counterpart of render_targets.h, same model: no EDRAM
// emulation, one host image per guest surface (EDRAM base, pitch, MSAA, host format), at least as
// tall as the EDRAM holds from its base.
//
// Rendering uses dynamic rendering: a rendering instance stays open across draws while their
// targets don't change, and is ended before anything that can't run inside one (copies into
// shared memory, the frame's end). Clears are recorded when the guest makes them, as
// vkCmdClearAttachments in a rendering instance on the cleared target (rectangles or all of it).
// Not yet (M4): the EDRAM fills of render_targets.h (a clear's value read by another surface over
// the same tiles) and dropping clears a later one covers.
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

#include "render_targets.h"  // SurfaceRegs, ClearCall

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
  bool initialized = false;  // in its attachment layout (false: just created, UNDEFINED)
  uint64_t last_frame = 0;
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
  // The guest's Clear / ClearF.
  void Clear(VkCommandBuffer cmd, const ClearCall& c);
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
  // Begins rendering on these attachments (color[i] may be null) unless that's what's open.
  void BeginRendering(VkCommandBuffer cmd, VulkanRenderTarget* const* color, uint32_t color_count,
                      VulkanRenderTarget* depth);

  VulkanDevice& device_;
  VulkanMemory* memory_ = nullptr;
  VkFormat d24s8_ = VK_FORMAT_UNDEFINED;  // D24_UNORM_S8_UINT if supported, else D32_SFLOAT_S8
  std::unordered_map<uint64_t, std::unique_ptr<VulkanRenderTarget>> targets_;
  struct Retired {
    VulkanMemory::Image image;
    VkImageView view;
    uint64_t fence;
  };
  std::vector<Retired> retired_;
  uint64_t frame_ = 0, frame_fence_ = 0;
  // The open rendering instance's attachments.
  bool rendering_ = false;
  std::array<VulkanRenderTarget*, 4> open_color_{};
  uint32_t open_color_count_ = 0;
  VulkanRenderTarget* open_depth_ = nullptr;
};

}  // namespace graw::native
