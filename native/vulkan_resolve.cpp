// GRAW native renderer: the Vulkan backend's resolve shaders (vulkan_resolve.h).
#include "vulkan_resolve.h"

#include "shaders/bytecode/color_resolve_1x.h"
#include "shaders/bytecode/color_resolve_msaa.h"
#include "shaders/bytecode/depth_resolve_1x.h"
#include "shaders/bytecode/depth_resolve_msaa.h"

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>

namespace graw::native {

namespace xenos = rex::graphics::xenos;

VulkanResolveShaders::~VulkanResolveShaders() {
  VkDevice d = device_.device();
  if (!d) return;
  const VulkanDevice::Functions& fn = device_.fn();
  for (VkPipeline p : {depth_1x_, depth_msaa_, color_1x_, color_msaa_})
    if (p) fn.vkDestroyPipeline(d, p, nullptr);
  if (layout_) fn.vkDestroyPipelineLayout(d, layout_, nullptr);
  if (set_layout_) fn.vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
  if (sampler_) fn.vkDestroySampler(d, sampler_, nullptr);
}

bool VulkanResolveShaders::Initialize() {
  VkDevice d = device_.device();
  const VulkanDevice::Functions& fn = device_.fn();
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (fn.vkCreateSampler(d, &si, nullptr, &sampler_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: resolve sampler failed");
    return false;
  }
  VkDescriptorSetLayoutBinding b[2] = {};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[0].pImmutableSamplers = &sampler_;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = 2;
  li.pBindings = b;
  if (fn.vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: resolve set layout failed");
    return false;
  }
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         uint32_t(std::max(sizeof(DepthParams), sizeof(ColorParams)))};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &set_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pc;
  if (fn.vkCreatePipelineLayout(d, &pli, nullptr, &layout_) != VK_SUCCESS) {
    REXLOG_ERROR("GRAW native Vulkan: resolve pipeline layout failed");
    return false;
  }
  auto create = [&](const uint32_t* code, size_t size, VkPipeline* out) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size;
    mi.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (fn.vkCreateShaderModule(d, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout_;
    const VkResult r = fn.vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, out);
    fn.vkDestroyShaderModule(d, module, nullptr);
    return r == VK_SUCCESS;
  };
  if (!create(kDepthResolve1x, sizeof(kDepthResolve1x), &depth_1x_) ||
      !create(kDepthResolveMsaa, sizeof(kDepthResolveMsaa), &depth_msaa_)) {
    REXLOG_ERROR("GRAW native Vulkan: depth resolve pipelines failed");
    return false;
  }
  // The color shader writes without a format qualifier; without the feature color resolves
  // stay copies and blits.
  if (device_.shader_caps().storage_image_write_without_format &&
      (!create(kColorResolve1x, sizeof(kColorResolve1x), &color_1x_) ||
       !create(kColorResolveMsaa, sizeof(kColorResolveMsaa), &color_msaa_))) {
    REXLOG_ERROR("GRAW native Vulkan: color resolve pipelines failed");
    return false;
  }
  return true;
}

void VulkanResolveShaders::RecordDepth(VkCommandBuffer cmd, bool msaa, VkDescriptorSet set,
                                       const DepthParams& p) {
  const VulkanDevice::Functions& fn = device_.fn();
  fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, msaa ? depth_msaa_ : depth_1x_);
  fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
  fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
  fn.vkCmdDispatch(cmd, (p.width + 7) / 8, (p.height + 7) / 8, 1);
}

void VulkanResolveShaders::RecordColor(VkCommandBuffer cmd, bool msaa, VkDescriptorSet set,
                                       const ColorParams& p) {
  const VulkanDevice::Functions& fn = device_.fn();
  fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, msaa ? color_msaa_ : color_1x_);
  fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
  fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
  fn.vkCmdDispatch(cmd, (p.width + 7) / 8, (p.height + 7) / 8, 1);
}

VkFormat VulkanResolveShaders::ColorStorageFormat(uint32_t dest_format) {
  if (!color_1x_) return VK_FORMAT_UNDEFINED;
  // The formats PackDest writes (resolve.cpp's GetDestStorage), by texel size.
  VkFormat f;
  switch (xenos::TextureFormat(dest_format)) {
    case xenos::TextureFormat::k_8:
      f = VK_FORMAT_R8_UINT;
      break;
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
    case xenos::TextureFormat::k_16_FLOAT:
      f = VK_FORMAT_R16_UINT;
      break;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
      f = VK_FORMAT_R32_UINT;
      break;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
      f = VK_FORMAT_R32G32_UINT;
      break;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      f = VK_FORMAT_R32G32B32A32_UINT;
      break;
    default:
      return VK_FORMAT_UNDEFINED;
  }
  auto it = storage_support_.find(f);
  if (it == storage_support_.end()) {
    VkFormatProperties props{};
    device_.fn().vkGetPhysicalDeviceFormatProperties(device_.physical_device(), f, &props);
    const bool ok = props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (!ok)
      REXLOG_WARN("GRAW native Vulkan: no storage images of format {}: resolves into texture "
                  "format {} are blits",
                  int(f), dest_format);
    it = storage_support_.emplace(f, ok).first;
  }
  return it->second ? f : VK_FORMAT_UNDEFINED;
}

uint32_t VulkanResolveShaders::HostSample(uint32_t samples, uint32_t select) {
  // 4x k23 -> sample 2, other averages -> 0; 2x k2/k3 -> k0/k1 and the host's order swapped.
  const uint32_t s = select <= 3 ? select : (select == 5 && samples == 4 ? 2 : 0);
  return samples == 4 ? s : samples == 2 ? 1 - (s & 1) : 0;
}

}  // namespace graw::native
