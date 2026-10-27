// GRAW native renderer: ucode -> SPIR-V -> VkShaderModule cache of the Vulkan backend
// (docs/VULKAN_RENDERER.md, M3a). The counterpart of shader_cache.h.
//
// Shaders are keyed by the XXH3 hash of their guest (big-endian) ucode, translations by (shader,
// modification), as in the DXBC cache; each valid translation gets one shader module.
// Threading: Load takes a lock (the guest thread loads in Prepare, the recording thread in Draw
// without the render thread); Module runs on the recording thread only.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/string/buffer.h>

#include "spirv_translate.h"
#include "vulkan_geometry_shader.h"

namespace graw::native {

struct VulkanShaderStats {
  uint64_t shaders = 0;       // distinct ucode loaded
  uint64_t translations = 0;  // distinct (ucode, modification) translated
  uint64_t failures = 0;      // translations that came out invalid, or modules not created
  uint64_t modules = 0;       // shader modules created
  double translate_us = 0, max_translate_us = 0;
};

class VulkanShaderCache {
 public:
  explicit VulkanShaderCache(VulkanDevice& device);
  ~VulkanShaderCache();

  const SpirvShaderTranslator& translator() const { return translator_; }

  // ucode: dword_count dwords as they are in guest memory (big-endian). Analyzes new ucode.
  SpirvShader* Load(rex::graphics::xenos::ShaderType type, const uint32_t* guest_ucode,
                    uint32_t dword_count);

  // The shader module of the modification, translating and creating it on first use;
  // VK_NULL_HANDLE if the translation is invalid or the module can't be created (counted in
  // stats().failures, logged once per shader).
  VkShaderModule Module(SpirvShader& shader, uint64_t modification);

  // The geometry shader module for `key` (vulkan_geometry_shader.h), created on first use;
  // VK_NULL_HANDLE if creation failed (logged once).
  VkShaderModule Geometry(VulkanGeometryShaderKey key);

  const VulkanShaderStats& stats() const { return stats_; }

 private:
  VulkanDevice& device_;
  SpirvShaderTranslator translator_;
  rex::string::StringBuffer disasm_buffer_;
  std::mutex load_mutex_;  // shaders_, disasm_buffer_, the Load half of stats_
  std::unordered_map<uint64_t, std::unique_ptr<SpirvShader>> shaders_;
  // (ucode hash ^ modification) -> module; VK_NULL_HANDLE for a failed one.
  struct ModuleKey {
    uint64_t hash, modification;
    bool operator==(const ModuleKey&) const = default;
  };
  struct ModuleKeyHash {
    size_t operator()(const ModuleKey& k) const { return size_t(k.hash ^ (k.modification * 31)); }
  };
  std::unordered_map<ModuleKey, VkShaderModule, ModuleKeyHash> modules_;
  std::unordered_map<uint32_t, VkShaderModule> geometry_modules_;  // by key
  VulkanShaderStats stats_;
};

}  // namespace graw::native
