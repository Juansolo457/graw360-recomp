// GRAW native renderer: the Vulkan backend's present pass (vulkan_present.h).
#include "vulkan_present.h"

#include "shaders/bytecode/present_ps.h"
#include "shaders/bytecode/present_vs.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>

namespace graw::native {

VulkanPresenter::~VulkanPresenter() {
  VkDevice d = device_.device();
  if (!d) return;
  const VulkanDevice::Functions& fn = device_.fn();
  for (auto& [format, p] : pipelines_)
    if (p) fn.vkDestroyPipeline(d, p, nullptr);
  for (VkShaderModule m : {vs_, ps_})
    if (m) fn.vkDestroyShaderModule(d, m, nullptr);
  if (pool_) fn.vkDestroyDescriptorPool(d, pool_, nullptr);
  if (layout_) fn.vkDestroyPipelineLayout(d, layout_, nullptr);
  if (set_layout_) fn.vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
  if (sampler_) fn.vkDestroySampler(d, sampler_, nullptr);
  memory_.Destroy(ramp_buffer_);
}

bool VulkanPresenter::Initialize() {
  VkDevice d = device_.device();
  const VulkanDevice::Functions& fn = device_.fn();
  const uint64_t align = std::max<uint64_t>(device_.shader_caps().min_uniform_buffer_offset_alignment, 16);
  slot_bytes_ = uint32_t((kRampBytes + align - 1) / align * align);
  if (!memory_.CreateBuffer(uint64_t(slot_bytes_) * VulkanDevice::kFramesInFlight,
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, &ramp_buffer_,
                            "present gamma ramp buffer"))
    return false;
  // The shader loads texels (texelFetch); the sampler is only there for the combined descriptor.
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (fn.vkCreateSampler(d, &si, nullptr, &sampler_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: present sampler failed");
    return false;
  }
  VkDescriptorSetLayoutBinding b[2] = {};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b[0].pImmutableSamplers = &sampler_;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = 2;
  li.pBindings = b;
  if (fn.vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: present set layout failed");
    return false;
  }
  const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(Constants)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &set_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pc;
  if (fn.vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: present pipeline layout failed");
    return false;
  }
  constexpr uint32_t kSets = VulkanDevice::kFramesInFlight;
  const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets}};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = kSets;
  pi.poolSizeCount = 2;
  pi.pPoolSizes = sizes;
  if (fn.vkCreateDescriptorPool(d, &pi, nullptr, &pool_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: present descriptor pool failed");
    return false;
  }
  std::array<VkDescriptorSetLayout, kSets> layouts;
  layouts.fill(set_layout_);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = kSets;
  ai.pSetLayouts = layouts.data();
  if (fn.vkAllocateDescriptorSets(d, &ai, sets_.data()) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: present descriptor sets failed");
    return false;
  }
  // Each slot's set reads that slot's ramp; the image is written per frame (Record).
  for (uint32_t i = 0; i < kSets; ++i) {
    const VkDescriptorBufferInfo bi{ramp_buffer_.buffer, VkDeviceSize(i) * slot_bytes_,
                                    kRampBytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[i];
    w.dstBinding = 1;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi;
    fn.vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
  }
  auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size;
    mi.pCode = code;
    return fn.vkCreateShaderModule(d, &mi, nullptr, out) == VK_SUCCESS;
  };
  if (!module(kPresentVs, sizeof(kPresentVs), &vs_) ||
      !module(kPresentPs, sizeof(kPresentPs), &ps_)) {
    REXLOG_ERROR("GRAW native Vulkan: present shaders failed");
    return false;
  }
  return true;
}

VkPipeline VulkanPresenter::Pipeline(VkFormat format) {
  for (const auto& [f, p] : pipelines_)
    if (f == format) return p;
  const VulkanDevice::Functions& fn = device_.fn();
  VkPipelineShaderStageCreateInfo stages[2] = {};
  for (VkPipelineShaderStageCreateInfo& s : stages) {
    s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.pName = "main";
  }
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps_;
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dynamic;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &ri;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &ds;
  ci.layout = layout_;
  VkPipeline p = VK_NULL_HANDLE;
  if (fn.vkCreateGraphicsPipelines(device_.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p) !=
      VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: present pipeline for format {} failed", int(format));
    p = VK_NULL_HANDLE;
  }
  pipelines_.emplace_back(format, p);  // a failure is not retried
  return p;
}

void VulkanPresenter::SetImage(VkImageView view, uint32_t width, uint32_t height, RampMode ramp,
                               uint64_t frame) {
  view_ = width && height ? view : VK_NULL_HANDLE;
  width_ = width;
  height_ = height;
  ramp_ = ramp;
  frame_ = frame;
}

void VulkanPresenter::Record(VkCommandBuffer cmd, const VulkanDevice::BackBuffer& back,
                             uint32_t slot) {
  const VulkanDevice::Functions& fn = device_.fn();
  const VkPipeline pipeline = view_ ? Pipeline(back.format) : VK_NULL_HANDLE;
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = back.view;
  ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  // Without an image, all black; with one, the bars are cleared (the draw writes the rest).
  ca.loadOp = pipeline ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
  ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ca.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, back.extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &ca;
  fn.vkCmdBeginRendering(cmd, &ri);
  if (pipeline) {
    // The image scaled to fit the window, centred, aspect kept (as present.cpp).
    const float iw = float(width_), ih = float(height_);
    const float ww = float(back.extent.width), wh = float(back.extent.height);
    const float s = std::min(ww / iw, wh / ih);
    const float vw = std::round(iw * s), vh = std::round(ih * s);
    const int32_t vx = int32_t(std::floor((ww - vw) * 0.5f));
    const int32_t vy = int32_t(std::floor((wh - vh) * 0.5f));
    const int32_t w = int32_t(back.extent.width), h = int32_t(back.extent.height);
    const int32_t rx = vx + int32_t(vw), ry = vy + int32_t(vh);
    const int32_t bars[4][4] = {{0, 0, w, vy}, {0, ry, w, h}, {0, vy, vx, ry}, {rx, vy, w, ry}};
    VkClearRect clear[4];
    uint32_t clears = 0;
    for (const auto& bar : bars)
      if (bar[2] > bar[0] && bar[3] > bar[1])
        clear[clears++] = {{{bar[0], bar[1]}, {uint32_t(bar[2] - bar[0]), uint32_t(bar[3] - bar[1])}},
                           0, 1};
    if (clears) {
      VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, ca.clearValue};
      fn.vkCmdClearAttachments(cmd, 1, &a, clears, clear);
    }
    Constants c{{iw, ih}, kRampNone, vw == iw && vh == ih};
    if (ramp_ != kRampNone &&
        gamma_.Read(ramp_, frame_,
                    reinterpret_cast<uint32_t*>(ramp_buffer_.mapped + size_t(slot) * slot_bytes_)))
      c.ramp_mode = ramp_;
    const VkDescriptorImageInfo ii{VK_NULL_HANDLE, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = sets_[slot];
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    fn.vkUpdateDescriptorSets(device_.device(), 1, &wr, 0, nullptr);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &sets_[slot],
                               0, nullptr);
    fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(c), &c);
    const VkViewport viewport{float(vx), float(vy), vw, vh, 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, back.extent};
    fn.vkCmdSetViewport(cmd, 0, 1, &viewport);
    fn.vkCmdSetScissor(cmd, 0, 1, &scissor);
    fn.vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  fn.vkCmdEndRendering(cmd);
}

}  // namespace graw::native
