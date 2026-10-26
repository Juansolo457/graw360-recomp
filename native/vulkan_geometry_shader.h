// GRAW native renderer: the Vulkan backend's geometry shaders for the guest primitives Vulkan
// lacks (docs/VULKAN_RENDERER.md, M4c), ReXGlue's (its Vulkan pipeline cache's):
// - quad lists: the 4 vertices of a quad come in as a line list with adjacency and go out as a
//   strip of 2 triangles (GRAW's HUD, menus and text);
// - rectangle lists: 3 vertices of a rectangle as a triangle, the 4th made, a strip out;
// - point lists: a point to a strip of 2 triangles of the point size (reads the system
//   constants, set 1 binding 0).
// The vertex shader's interpolators, clip and cull distances pass through.
#pragma once

#include "vulkan_device.h"  // first: the Vulkan platform defines

#include <cstdint>
#include <vector>

#include "spirv_translate.h"

namespace graw::native {

enum class PipelineGeometryShader : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union VulkanGeometryShaderKey {
  uint32_t key;
  struct {
    PipelineGeometryShader type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
    uint32_t point_ps_ucp_mode : 2;
  };
  VulkanGeometryShaderKey() : key(0) {}
  bool operator==(const VulkanGeometryShaderKey& other) const { return key == other.key; }
};
static_assert(sizeof(VulkanGeometryShaderKey) == sizeof(uint32_t));

// The key for a draw of geometry shader `type` with these vertex and pixel shader modifications
// (SpirvShaderTranslator::Modification values).
VulkanGeometryShaderKey GetVulkanGeometryShaderKey(PipelineGeometryShader type,
                                                   uint64_t vertex_modification,
                                                   uint64_t pixel_modification);

// The SPIR-V of the geometry shader for `key` (float controls as `caps` has them).
std::vector<uint32_t> BuildVulkanGeometryShader(VulkanGeometryShaderKey key,
                                                const VulkanDevice::ShaderCaps& caps);

}  // namespace graw::native
