// GRAW native renderer: the Vulkan backend's resolve shaders (docs/VULKAN_RENDERER.md, M4),
// resolve.cpp's (the D3D12 Resolver's) in GLSL (shaders/*.comp, built into
// shaders/bytecode/ by tools/vulkan_shaders.py):
// - color: a rectangle of a color render target into a resolve destination image as the Xenos
//   texel bits of the destination format, through a uint view of the image's texel size
//   (sample selection and averaging, the 7e3 EDRAM quantization, exponent bias, red/blue swap);
// - depth: a rectangle of a depth render target into an R32_SFLOAT image as the Resolver's
//   depth_r32f, the 20e4 float the EDRAM holds for kD24FS8 (unorm24 for kD24S8), from one
//   sample (ReXGlue's SanitizeCopySampleSelect never averages depth). Textures of k_24_8 /
//   k_24_8_FLOAT at the destination bind that image.
// Both take one descriptor set: binding 0 the source (its own sampler), binding 1 the
// destination's storage view (GENERAL).
//
// Threading: the recording thread.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include <cstdint>
#include <unordered_map>

namespace graw::native {

class VulkanResolveShaders {
 public:
  explicit VulkanResolveShaders(VulkanDevice& device) : device_(device) {}
  ~VulkanResolveShaders();

  bool Initialize();

  VkDescriptorSetLayout set_layout() const { return set_layout_; }
  VkSampler sampler() const { return sampler_; }

  struct DepthParams {
    int32_t src_x = 0, src_y = 0;
    uint32_t width = 0, height = 0;
    int32_t dest_x = 0, dest_y = 0;
    uint32_t depth_format = 0;  // xenos::DepthRenderTargetFormat
    uint32_t host_sample = 0;   // MSAA sources
  };
  struct ColorParams {
    int32_t src_x = 0, src_y = 0;
    uint32_t width = 0, height = 0;
    uint32_t dest_x = 0, dest_y = 0;
    uint32_t dest_format = 0;  // xenos::TextureFormat
    uint32_t src_format = 0;   // xenos::ColorRenderTargetFormat
    uint32_t flags = 0;  // 2: red/blue swap, 4-6: sample select, 8-10: samples, 12-14: number
    int32_t exp_bias = 0;
  };
  // Record a copy with `set` (set_layout()) bound.
  void RecordDepth(VkCommandBuffer cmd, bool msaa, VkDescriptorSet set, const DepthParams& p);
  void RecordColor(VkCommandBuffer cmd, bool msaa, VkDescriptorSet set, const ColorParams& p);

  // The uint format the color shader writes destination format `dest_format`
  // (xenos::TextureFormat) through, by its texel size; UNDEFINED when the shader can't write
  // it (an unsupported format, or a GPU without the storage support it needs).
  VkFormat ColorStorageFormat(uint32_t dest_format);

  // The host sample a resolve copies from a depth target with `samples` samples, for guest
  // sample select `select` (xenos::CopySampleSelect), as resolve.cpp's shader picks it.
  static uint32_t HostSample(uint32_t samples, uint32_t select);

 private:
  VulkanDevice& device_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline depth_1x_ = VK_NULL_HANDLE, depth_msaa_ = VK_NULL_HANDLE;
  VkPipeline color_1x_ = VK_NULL_HANDLE, color_msaa_ = VK_NULL_HANDLE;
  std::unordered_map<VkFormat, bool> storage_support_;
};

}  // namespace graw::native
