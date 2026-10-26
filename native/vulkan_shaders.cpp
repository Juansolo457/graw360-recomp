// GRAW native renderer: the Vulkan backend's shader cache (vulkan_shaders.h).
#include "vulkan_shaders.h"

#include "log.h"

#include <rex/logging.h>

#include <xxhash.h>

#include <algorithm>
#include <chrono>

namespace graw::native {

namespace {

double Us(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

namespace {

// SpirvShaderTranslator::Features(const VulkanDevice*) for our device: what the GPU supports
// (and VulkanDevice enabled). Image view format swizzle is a portability-subset limitation only.
SpirvShaderTranslator::Features TranslatorFeatures(const VulkanDevice::ShaderCaps& c) {
  SpirvShaderTranslator::Features f(false);
  f.spirv_version = c.api_version >= VK_API_VERSION_1_2 ? spv::Spv_1_5 : spv::Spv_1_0;
  f.max_storage_buffer_range = c.max_storage_buffer_range;
  f.full_draw_index_uint32 = c.full_draw_index_uint32;
  f.vertex_pipeline_stores_and_atomics = c.vertex_pipeline_stores_and_atomics;
  f.fragment_stores_and_atomics = c.fragment_stores_and_atomics;
  f.clip_distance = c.clip_distance;
  f.cull_distance = c.cull_distance;
  f.image_view_format_swizzle = true;
  f.signed_zero_inf_nan_preserve_float32 = c.signed_zero_inf_nan_preserve_float32;
  f.denorm_flush_to_zero_float32 = c.denorm_flush_to_zero_float32;
  f.rounding_mode_rte_float32 = c.rounding_mode_rte_float32;
  f.fragment_shader_sample_interlock = false;  // host render targets, no interlock path
  f.demote_to_helper_invocation = c.demote_to_helper_invocation;
  f.sample_rate_shading = c.sample_rate_shading;
  return f;
}

}  // namespace

// Native 2x MSAA with and without attachments, no fragment shader interlock (host render
// targets).
VulkanShaderCache::VulkanShaderCache(VulkanDevice& device)
    : device_(device),
      translator_(TranslatorFeatures(device.shader_caps()), true, true, false) {}

VulkanShaderCache::~VulkanShaderCache() {
  for (const auto& [key, module] : modules_)
    if (module) device_.fn().vkDestroyShaderModule(device_.device(), module, nullptr);
}

SpirvShader* VulkanShaderCache::Load(rex::graphics::xenos::ShaderType type,
                                     const uint32_t* guest_ucode, uint32_t dword_count) {
  const uint64_t hash = XXH3_64bits(guest_ucode, size_t(dword_count) * sizeof(uint32_t));
  std::lock_guard<std::mutex> lock(load_mutex_);
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second.get();
  auto shader = std::make_unique<SpirvShader>(type, hash, guest_ucode, dword_count);
  shader->AnalyzeUcode(disasm_buffer_);
  ++stats_.shaders;
  return shaders_.emplace(hash, std::move(shader)).first->second.get();
}

VkShaderModule VulkanShaderCache::Module(SpirvShader& shader, uint64_t modification) {
  const ModuleKey key{shader.ucode_data_hash(), modification};
  auto it = modules_.find(key);
  if (it != modules_.end()) return it->second;
  bool is_new = false;
  auto* translation = shader.GetOrCreateTranslation(modification, &is_new);
  if (is_new) {
    const auto t = std::chrono::steady_clock::now();
    translator_.TranslateAnalyzedShader(*translation);
    const double us = Us(t);
    stats_.translate_us += us;
    stats_.max_translate_us = std::max(stats_.max_translate_us, us);
    ++stats_.translations;
  }
  VkShaderModule module = VK_NULL_HANDLE;
  const auto& binary = translation->translated_binary();
  if (translation->is_valid() && !binary.empty()) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = binary.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(binary.data());
    if (device_.fn().vkCreateShaderModule(device_.device(), &ci, nullptr, &module) == VK_SUCCESS)
      ++stats_.modules;
    else
      module = VK_NULL_HANDLE;
  }
  if (!module) {
    ++stats_.failures;
    GRAW_LOG_FIRST(16, WARN,
                   "GRAW native Vulkan: shader {:016X} modification {:016X} ({}) has no module "
                   "({})",
                   shader.ucode_data_hash(), modification,
                   shader.type() == rex::graphics::xenos::ShaderType::kVertex ? "vertex"
                                                                               : "pixel",
                   translation->is_valid() ? "vkCreateShaderModule failed" : "invalid translation");
  }
  modules_.emplace(key, module);
  return module;
}

}  // namespace graw::native
