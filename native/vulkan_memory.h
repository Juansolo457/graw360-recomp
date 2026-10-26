// GRAW native renderer: memory for the Vulkan backend (docs/VULKAN_RENDERER.md, M3c).
//
// VulkanMemory: a Vulkan Memory Allocator (VMA, from the SDK source tree; implemented in
// vulkan_memory.cpp, as the prebuilt SDK has no Vulkan) and the buffers and images made with it.
// VulkanUploadPool: CPU-to-GPU upload memory in persistently mapped host-visible blocks,
// sub-allocated linearly and recycled once the GPU finished the frame that used them; the
// Vulkan counterpart of upload_pool.h, with the same tags (fence values: frame numbers).
// Threading: one thread per pool, as upload_pool.h.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include <cstdint>
#include <memory>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace graw::native {

class VulkanMemory {
 public:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* allocation = nullptr;
    uint8_t* mapped = nullptr;  // host-visible buffers: persistently mapped
    uint64_t size = 0;
  };

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation_T* allocation = nullptr;
  };

  ~VulkanMemory();
  bool Initialize(const VulkanDevice& device);

  // Device-local unless `host_visible` (then mapped, for sequential CPU writes). Logs `what` and
  // returns false on failure.
  bool CreateBuffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, Buffer* out,
                    const char* what);
  void Destroy(Buffer& buffer);
  // Device-local.
  bool CreateImage(const VkImageCreateInfo& info, Image* out, const char* what);
  void Destroy(Image& image);

 private:
  VmaAllocator_T* allocator_ = nullptr;
};

class VulkanUploadPool {
 public:
  struct Alloc {
    VkBuffer buffer;
    uint64_t offset;
    uint8_t* cpu;
  };
  // block_size: the size of the recycled blocks; a larger request gets a block of its own,
  // released (not recycled) when its frame completes.
  explicit VulkanUploadPool(uint64_t block_size, VkBufferUsageFlags usage)
      : block_size_(block_size), usage_(usage) {}
  ~VulkanUploadPool();
  void SetMemory(VulkanMemory* memory) { memory_ = memory; }

  // `fence`: the fence value (frame number) that completes the frame the memory is used in.
  bool Allocate(uint64_t size, uint64_t align, uint64_t fence, Alloc* out);
  // After the frame's submission: blocks used so far wait for their fence.
  void EndFrame(uint64_t completed);

 private:
  struct Block {
    VulkanMemory::Buffer buffer;
    uint64_t size = 0, used = 0, fence = 0;
  };
  std::unique_ptr<Block> CreateBlock(uint64_t size);
  void Release(std::unique_ptr<Block>& block);
  bool NewBlock(uint64_t size, uint64_t fence);

  VulkanMemory* memory_ = nullptr;
  uint64_t block_size_;
  VkBufferUsageFlags usage_;
  Block* current_ = nullptr;
  std::vector<std::unique_ptr<Block>> active_, retired_, free_;
};

}  // namespace graw::native
