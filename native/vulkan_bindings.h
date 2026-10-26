// GRAW native renderer: the Vulkan backend's descriptor sets and pipeline layouts
// (docs/VULKAN_RENDERER.md, M3c-2).
//
// The binding model is the SDK's SPIR-V translator's (VulkanCommandProcessor's, host render
// target path): four descriptor sets and no push constants.
//   set 0  shared memory: storage buffer binding 0, an array of
//          1 << GetSharedMemoryStorageBufferCountLog2 elements covering the 512 MB together
//   set 1  the five constant buffers, uniform buffer bindings 0-4 in kConstantBuffer order
//          (system, float VS, float PS, bool/loop, fetch)
//   set 2  vertex shader textures: the shader's sampled images (0..N-1), then its samplers
//   set 3  pixel shader textures, the same; an empty layout for a shader without any
// Pipeline layouts are made per (vertex, pixel) texture and sampler counts.
//
// M3c-2: textures are placeholders, a 1x1 opaque mid-grey image per view type (2D array,
// cube, 3D, as the SDK's null images) and one sampler; texture_swizzled_signs stays unsigned.
// Constant buffers go to a per-frame uniform upload pool, uploaded again only when their
// contents change within the frame. Descriptor sets come from per-frame pools, reset when the
// frame in flight slot comes round again.
// Threading: the recording thread only.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines
#include "vulkan_memory.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "spirv_translate.h"

namespace graw::native {

struct VulkanBindingStats {
  uint64_t sets = 0;              // descriptor sets allocated
  uint64_t constant_uploads = 0;  // constant buffers uploaded (not reused)
  uint64_t constant_bytes = 0;
  uint64_t constant_set_reuses = 0;  // draws that kept the previous constants set
};

class VulkanBindings {
 public:
  struct Layout {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE, textures_pixel = VK_NULL_HANDLE;
  };

  explicit VulkanBindings(VulkanDevice& device) : device_(device) {}
  ~VulkanBindings();

  // `shared`: the shared memory buffer, kSharedMemorySize bytes.
  bool Initialize(VulkanMemory& memory, VkBuffer shared);

  // The pipeline layout for the shaders' texture and sampler counts (after translation); null
  // if it can't be created (logged).
  const Layout* GetLayout(uint32_t textures_vertex, uint32_t samplers_vertex,
                          uint32_t textures_pixel, uint32_t samplers_pixel);
  const Layout* GetLayout(const SpirvShader& vs, const SpirvShader* ps);

  // Once, outside rendering, before the first Bind: the placeholder images' clear.
  void InitializePlaceholders(VkCommandBuffer cmd);
  bool placeholders_ready() const { return placeholders_ready_; }

  // A draw's descriptor sets: uploads the constant buffers (`regs`: the guest register file,
  // `sys`: the system constants), writes the texture sets and binds sets 0-3 for `layout`.
  // `frame`: the frame being recorded (VulkanDevice::frame_fence). Both shaders translated
  // (their bindings are known); ps null without a pixel shader. False on failure (logged once).
  bool Bind(VkCommandBuffer cmd, const Layout& layout, const uint32_t* regs, const SpirvShader& vs,
            const SpirvShader* ps, const SpirvShaderTranslator::SystemConstants& sys,
            uint64_t frame);

  // After the frame's submission: uniform memory of completed frames is recycled.
  void EndFrame(uint64_t completed) { uniforms_.EndFrame(completed); }

  const VulkanBindingStats& stats() const { return stats_; }

 private:
  // A constant buffer's last contents and where they were uploaded this frame.
  struct Constants {
    std::vector<uint8_t> last;
    VkDescriptorBufferInfo info{};
    uint64_t frame = 0;  // the frame `info` belongs to (0: none)
  };
  // Uploads data[size] unless the frame's last upload has the same contents; `changed`: it
  // uploaded.
  bool Upload(Constants& c, const void* data, uint32_t size, uint64_t frame, bool* changed);
  VkDescriptorSetLayout TextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  // A descriptor set of `layout` from the frame's pools (another pool when they're full).
  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
  VkDescriptorPool NewPool();
  void BeginFrame(uint64_t frame);
  // Writes a texture set for the shader's bindings with the placeholders.
  VkDescriptorSet TextureSet(const Layout& layout, bool vertex, const SpirvShader& shader);
  bool Check(VkResult r, const char* what);

  VulkanDevice& device_;
  VulkanMemory* memory_ = nullptr;
  VkShaderStageFlags vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayout empty_layout_ = VK_NULL_HANDLE, shared_layout_ = VK_NULL_HANDLE,
                        constants_layout_ = VK_NULL_HANDLE;
  // (vertex << 16 | samplers << 8 | textures) -> layout.
  std::unordered_map<uint32_t, VkDescriptorSetLayout> texture_layouts_;
  // (texture and sampler counts, 16 bits each) -> layout; references stay valid.
  std::unordered_map<uint64_t, Layout> layouts_;

  // Set 0, written once from its own pool.
  VkDescriptorPool shared_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE;
  // Per frame in flight slot: the pools sets were allocated from, the next to use.
  struct FramePools {
    std::vector<VkDescriptorPool> pools;
    size_t current = 0;
  };
  std::array<FramePools, VulkanDevice::kFramesInFlight> frame_pools_;
  uint64_t frame_ = 0;

  VulkanUploadPool uniforms_{4u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
  uint64_t uniform_align_ = 256;
  std::array<Constants, SpirvShaderTranslator::kConstantBufferCount> constants_;
  VkDescriptorSet constants_set_ = VK_NULL_HANDLE;  // this frame's last, for constants_
  std::vector<uint8_t> float_scratch_;              // packed float constants

  // Placeholder textures: [0] 2D array (6 layers, cube compatible), [1] 3D.
  std::array<VulkanMemory::Image, 2> placeholder_images_{};
  VkImageView view_2d_array_ = VK_NULL_HANDLE, view_cube_ = VK_NULL_HANDLE,
              view_3d_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  bool placeholders_ready_ = false;

  std::vector<VkDescriptorImageInfo> image_infos_;
  bool bind_failed_logged_ = false;
  VulkanBindingStats stats_;
};

}  // namespace graw::native
