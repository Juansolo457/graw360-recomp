// GRAW native renderer: the Vulkan backend's graphics pipelines (vulkan_pipelines.h). The state
// setup follows ReXGlue's src/graphics/vulkan/pipeline_cache.cpp (Xenia, Copyright Ben Vanik,
// BSD-3; adapted for ReXGlue by Tom Clay), host render target path.
#include "vulkan_pipelines.h"

#include <rex/logging.h>

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace graw::native {

VulkanPipelines::~VulkanPipelines() {
  if (!device_.device()) return;
  for (auto& [hash, e] : pipelines_)
    if (e.pipeline) device_.fn().vkDestroyPipeline(device_.device(), e.pipeline, nullptr);
}

VkPipeline VulkanPipelines::Get(const VulkanPipelineKey& key) {
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto [first, last] = pipelines_.equal_range(hash);
  for (auto it = first; it != last; ++it)
    if (!std::memcmp(&it->second.key, &key, sizeof(key))) return it->second.pipeline;
  const auto start = std::chrono::steady_clock::now();
  VkPipeline p = Create(key);
  const double us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  stats_.create_us += us;
  stats_.max_create_us = std::max(stats_.max_create_us, us);
  if (p)
    ++stats_.created;
  else
    ++stats_.failed;
  pipelines_.emplace(hash, Entry{key, p});
  return p;
}

VkPipeline VulkanPipelines::Create(const VulkanPipelineKey& k) {
  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t stage_count = 0;
  stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = k.vs;
  stages[stage_count++].pName = "main";
  if (k.ps) {
    stages[stage_count] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = k.ps;
    stages[stage_count++].pName = "main";
  }

  // Vertices come from shared memory (the shaders' vfetch): no vertex input.
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(k.topology);
  input_assembly.primitiveRestartEnable = k.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.rasterizerDiscardEnable = k.discard ? VK_TRUE : VK_FALSE;
  raster.depthClampEnable = k.depth_clamp ? VK_TRUE : VK_FALSE;
  raster.polygonMode = k.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = VkCullModeFlags(k.cull);
  raster.frontFace = k.front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Dynamic, and only meaningful with a depth attachment (as the SDK).
  raster.depthBiasEnable = k.has_depth ? VK_TRUE : VK_FALSE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(k.discard ? 1 : k.samples);

  VkPipelineDepthStencilStateCreateInfo depth_stencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = k.depth_test ? VK_TRUE : VK_FALSE;
  depth_stencil.depthWriteEnable = k.depth_write ? VK_TRUE : VK_FALSE;
  depth_stencil.depthCompareOp = VkCompareOp(k.depth_compare);
  depth_stencil.stencilTestEnable = k.stencil_test ? VK_TRUE : VK_FALSE;
  auto stencil = [](const uint8_t* s) {
    VkStencilOpState o{};
    o.failOp = VkStencilOp(s[0]);
    o.passOp = VkStencilOp(s[1]);
    o.depthFailOp = VkStencilOp(s[2]);
    o.compareOp = VkCompareOp(s[3]);
    return o;
  };
  depth_stencil.front = stencil(k.front);
  depth_stencil.back = stencil(k.back);

  VkPipelineColorBlendAttachmentState blend[4] = {};
  for (uint32_t i = 0; i < k.color_count; ++i) {
    const VulkanPipelineKey::Rt& rt = k.rt[i];
    blend[i].blendEnable = rt.blend ? VK_TRUE : VK_FALSE;
    blend[i].srcColorBlendFactor = VkBlendFactor(rt.src);
    blend[i].dstColorBlendFactor = VkBlendFactor(rt.dst);
    blend[i].colorBlendOp = VkBlendOp(rt.op);
    blend[i].srcAlphaBlendFactor = VkBlendFactor(rt.src_a);
    blend[i].dstAlphaBlendFactor = VkBlendFactor(rt.dst_a);
    blend[i].alphaBlendOp = VkBlendOp(rt.op_a);
    blend[i].colorWriteMask = VkColorComponentFlags(rt.mask);
  }
  VkPipelineColorBlendStateCreateInfo color_blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blend.attachmentCount = k.color_count;
  color_blend.pAttachments = blend;

  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = k.color_count;
  rendering.pColorAttachmentFormats = k.color_count ? k.color_format : nullptr;
  if (k.has_depth) {
    rendering.depthAttachmentFormat = k.depth_format;
    rendering.stencilAttachmentFormat = k.depth_format;
  }

  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rendering;
  ci.stageCount = stage_count;
  ci.pStages = stages;
  ci.pVertexInputState = &vertex_input;
  ci.pInputAssemblyState = &input_assembly;
  ci.pViewportState = &viewport;
  ci.pRasterizationState = &raster;
  ci.pMultisampleState = &multisample;
  ci.pDepthStencilState = &depth_stencil;
  ci.pColorBlendState = &color_blend;
  ci.pDynamicState = &dynamic;
  ci.layout = k.layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult r = device_.fn().vkCreateGraphicsPipelines(device_.device(), VK_NULL_HANDLE, 1,
                                                            &ci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: vkCreateGraphicsPipelines failed ({}), topology {} colors {}",
                 int(r), k.topology, k.color_count);
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

}  // namespace graw::native
