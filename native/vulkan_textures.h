// GRAW native renderer: the Vulkan backend's texture cache (docs/VULKAN_RENDERER.md, M4). The
// counterpart of textures.h, simpler for now: one image per TextureKey (texture_layout.h), untiled
// on the CPU into upload memory and copied on the frame's command buffer when new or written
// since (the same physical memory invalidation callbacks as textures.h mark textures dirty),
// with views per signedness and swizzle as the SPIR-V translator's bindings want them (2D array,
// cube, 3D). A texture whose base address is a resolve destination (vulkan_backend.cpp's images)
// binds that image instead, when its format and size match.
// Not yet: untiling on worker threads and upload lists ahead of the frame, the memory budget's
// eviction (textures unused for a while are dropped), capture order with the render thread.
//
// Threading: the recording thread, except Invalidate (any guest thread that writes a watched
// page).
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "vulkan_memory.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "texture_layout.h"

namespace rex::memory {
class Memory;
}

namespace graw::native {

// A resolve destination image at a guest physical address (VK_NULL_HANDLE: none), single
// level and layer, in SHADER_READ_ONLY_OPTIMAL between uses. format_signed: the format of its
// signed views (a MUTABLE image's), or UNDEFINED.
struct VulkanResolvedImage {
  VkImage image = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED, format_signed = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
};
using VulkanResolvedLookup = std::function<VulkanResolvedImage(uint32_t guest_physical)>;

struct VulkanTextureStats {
  uint64_t uploads = 0, upload_bytes = 0, created = 0, resolved_binds = 0;
  uint64_t unsupported = 0, failed = 0, resolved_mismatches = 0;
};

// What a draw binds for one texture fetch constant.
struct VulkanTextureBinding {
  VkImageView view_unsigned = VK_NULL_HANDLE;
  VkImageView view_signed = VK_NULL_HANDLE;  // null: the format has no signed version
  rex::graphics::xenos::DataDimension dimension{};
  uint8_t swizzled_signs = 0;  // texture_util::SwizzleSigns
  bool resolved = false;       // a resolve's image (not from guest memory)
};

class VulkanTextureCache {
 public:
  VulkanTextureCache(VulkanDevice& device, VulkanMemory& memory, rex::memory::Memory* guest);
  ~VulkanTextureCache();

  void SetResolvedLookup(VulkanResolvedLookup lookup) { resolved_ = std::move(lookup); }

  // The texture of a fetch constant (6 dwords), uploaded first if new or written since: the
  // upload ends the open rendering instance (`end_rendering`) and records the copy into cmd.
  // frame: the frame being recorded (upload memory is kept until it completes). False: no data
  // or unsupported (logged once per format).
  bool Get(const uint32_t fetch[6], VkCommandBuffer cmd, uint64_t frame,
           const std::function<void()>& end_rendering, VulkanTextureBinding* out);

  // The backend replaces a resolve image: its views go once the GPU finished `frame` (the
  // fence of the frame being recorded, which may still use them).
  void ForgetResolvedImage(VkImage image, uint64_t frame);

  // A view of a resolve image (2D array) in `format` with host component swizzle `swizzle` (3
  // bits per component, 4 zero, 5 one), kept until ForgetResolvedImage; null if it failed. The
  // texture bindings' and the present pass's.
  VkImageView ResolvedView(VkImage image, VkFormat format, uint32_t swizzle);

  // After the frame's submission: upload memory of completed frames is recycled.
  void EndFrame(uint64_t completed);

  const VulkanTextureStats& stats() const { return stats_; }
  size_t count() const { return textures_.size(); }

 private:
  struct Texture {
    TextureKey key;
    const HostFormat* host = nullptr;
    VkFormat format_unsigned = VK_FORMAT_UNDEFINED, format_signed = VK_FORMAT_UNDEFINED;
    VulkanMemory::Image image;
    bool initialized = false;  // in SHADER_READ_ONLY_OPTIMAL (false: UNDEFINED)
    std::atomic<bool> dirty{true};
    // Views by (signed << 12 | host component swizzle).
    std::unordered_map<uint32_t, VkImageView> views;
  };
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start,
                                                         uint32_t length, bool exact);
  std::pair<uint32_t, uint32_t> Invalidate(uint32_t start, uint32_t length);
  Texture* Create(const TextureKey& key, const HostFormat* host);
  bool Upload(Texture& t, VkCommandBuffer cmd, uint64_t frame);
  VkImageView View(Texture& t, bool is_signed, uint32_t swizzle);

  VulkanDevice& device_;
  VulkanMemory& memory_;
  rex::memory::Memory* guest_;
  const uint8_t* membase_ = nullptr;
  void* invalidation_handle_ = nullptr;
  VulkanResolvedLookup resolved_;
  std::unordered_map<TextureKey, std::unique_ptr<Texture>, TextureKeyHasher> textures_;
  // Watched ranges (physical [start, end)) per texture; Invalidate scans them.
  struct Watch {
    uint32_t start, end;
    Texture* texture;
  };
  std::mutex watch_mutex_;
  std::vector<Watch> watches_;
  // Views of resolved images, by (image, view format, swizzle).
  struct ResolvedViewKey {
    VkImage image;
    VkFormat format;
    uint32_t swizzle;
    bool operator==(const ResolvedViewKey&) const = default;
  };
  struct ResolvedViewHash {
    size_t operator()(const ResolvedViewKey& k) const {
      return std::hash<const void*>()(k.image) ^
             ((size_t(k.swizzle) | size_t(k.format) << 16) * 0x9E3779B97F4A7C15ull);
    }
  };
  std::unordered_map<ResolvedViewKey, VkImageView, ResolvedViewHash> resolved_views_;
  struct RetiredView {
    VkImageView view;
    uint64_t fence;
  };
  std::vector<RetiredView> retired_views_;
  VulkanUploadPool upload_{16u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
  std::vector<uint8_t> scratch_;
  std::vector<HostSubresource> subresources_;
  uint64_t unsupported_formats_logged_ = 0;  // bit per format
  uint32_t resolved_mismatches_logged_ = 0;
  VulkanTextureStats stats_;
};

// The Vulkan format of a host format's DXGI format (texture_layout.h's table).
VkFormat VulkanFormatOf(DXGI_FORMAT format);

}  // namespace graw::native
