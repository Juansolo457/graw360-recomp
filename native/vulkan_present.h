// GRAW native renderer: the Vulkan backend's present pass (docs/VULKAN_RENDERER.md, M5),
// present.cpp's blit without HDR (shaders/present.vert, present.frag): the front buffer drawn
// into the back buffer, scaled to fit with the aspect kept and black bars, through a view with
// its fetch constant's swizzle, every texel through the display controller's gamma ramp
// (gamma_ramp.h) before filtering; point sampled when shown at its size, else bilinear.
// The ramp goes into a uniform buffer slot per frame in flight; one descriptor set per slot.
//
// Threading: the recording thread.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include "gamma_ramp.h"
#include "vulkan_memory.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace graw::native {

class VulkanPresenter final : public VulkanDevice::PresentPass {
 public:
  VulkanPresenter(VulkanDevice& device, VulkanMemory& memory) : device_(device), memory_(memory) {}
  ~VulkanPresenter();

  bool Initialize();

  // The next frame's image: `view` of an image in SHADER_READ_ONLY_OPTIMAL (2D array, as the
  // texture cache's views of resolve images), shown from texel (0, 0), `width` x `height`;
  // `ramp`: the gamma ramp it goes through (kRampNone: as it is). Null view: a black frame.
  // `frame`: the frame index (the ramp's log).
  void SetImage(VkImageView view, uint32_t width, uint32_t height, RampMode ramp, uint64_t frame);

  void Record(VkCommandBuffer cmd, const VulkanDevice::BackBuffer& back, uint32_t slot) override;

 private:
  struct Constants {
    float image_size[2];
    uint32_t ramp_mode, point_sampled;
  };
  static constexpr uint32_t kRampBytes = kPwlWords * 4;  // the shader's uvec4 ramp[96]

  // The pipeline into back buffers of `format`, created on first use (kept until destroyed).
  VkPipeline Pipeline(VkFormat format);

  VulkanDevice& device_;
  VulkanMemory& memory_;
  GammaRamp gamma_;
  VulkanMemory::Buffer ramp_buffer_;  // kFramesInFlight slots of slot_bytes_
  uint32_t slot_bytes_ = 0;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, VulkanDevice::kFramesInFlight> sets_{};
  VkShaderModule vs_ = VK_NULL_HANDLE, ps_ = VK_NULL_HANDLE;
  std::vector<std::pair<VkFormat, VkPipeline>> pipelines_;

  VkImageView view_ = VK_NULL_HANDLE;
  uint32_t width_ = 0, height_ = 0;
  RampMode ramp_ = kRampNone;
  uint64_t frame_ = 0;
};

}  // namespace graw::native
