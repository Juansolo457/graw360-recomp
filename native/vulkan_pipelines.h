// GRAW native renderer: the Vulkan backend's graphics pipelines (docs/VULKAN_RENDERER.md, M3c-3).
//
// A pipeline per key: the shader modules, the pipeline layout, and the fixed state the guest's
// registers select, as draw.cpp's PipelineKey with Vulkan values. Dynamic state (viewport,
// scissor, depth bias, blend constants, stencil masks and reference) as in the SDK's Vulkan
// pipeline cache, so pipelines don't multiply with values that change per draw. Pipelines are
// for dynamic rendering (the attachment formats are in the key). Created on first use, on the
// recording thread; not yet (later in M3): background compiles, a VkPipelineCache, and the
// geometry shaders that expand point, rectangle and quad lists.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include <cstdint>
#include <unordered_map>

namespace graw::native {

struct VulkanPipelineKey {
  VkShaderModule vs;
  VkShaderModule ps;  // VK_NULL_HANDLE: no fragment shader
  VkPipelineLayout layout;
  uint8_t topology;           // VkPrimitiveTopology
  uint8_t primitive_restart;  // all ones index restarts the strip
  uint8_t discard;            // rasterizer discard
  uint8_t cull;               // VkCullModeFlags
  uint8_t front_cw;
  uint8_t polygon_line;       // wireframe
  uint8_t depth_clamp;
  uint8_t samples;            // 1, 2, 4
  uint8_t depth_test, depth_write, depth_compare, stencil_test;
  uint8_t front[4], back[4];  // fail, pass, depth fail, compare (Vulkan values)
  uint8_t color_count;
  uint8_t has_depth;
  uint8_t pad[2];
  VkFormat color_format[4];
  VkFormat depth_format;
  struct Rt {
    uint8_t blend, src, dst, op, src_a, dst_a, op_a, mask;  // VkBlendFactor / VkBlendOp values
  } rt[4];
  uint8_t pad_end[4];
};
// No implicit padding: keys are hashed and compared as bytes.
static_assert(sizeof(VulkanPipelineKey) == 104);

struct VulkanPipelineStats {
  uint64_t created = 0, failed = 0;
  double create_us = 0, max_create_us = 0;
};

class VulkanPipelines {
 public:
  explicit VulkanPipelines(VulkanDevice& device) : device_(device) {}
  ~VulkanPipelines();

  // The pipeline for `key` (zeroed before filling: hashed as bytes), created if new;
  // VK_NULL_HANDLE if creation failed (logged once per key).
  VkPipeline Get(const VulkanPipelineKey& key);

  const VulkanPipelineStats& stats() const { return stats_; }
  size_t count() const { return pipelines_.size(); }

 private:
  VkPipeline Create(const VulkanPipelineKey& key);

  VulkanDevice& device_;
  struct Entry {
    VulkanPipelineKey key;
    VkPipeline pipeline;
  };
  std::unordered_multimap<uint64_t, Entry> pipelines_;
  VulkanPipelineStats stats_;
};

}  // namespace graw::native
