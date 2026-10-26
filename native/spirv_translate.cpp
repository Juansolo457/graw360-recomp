// GRAW native renderer: shader variant selection for the SPIR-V translator (spirv_translate.h).
// Follows ReXGlue's src/graphics/vulkan/pipeline_cache.cpp (Xenia, Copyright Ben Vanik, BSD-3;
// adapted for ReXGlue by Tom Clay), reduced to the host render target path with every device
// feature the translator asks about (clip and cull distances) available.
#include "spirv_translate.h"

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include <bit>

namespace graw::native {

namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;
using Modification = SpirvShaderTranslator::Modification;

namespace {

template <typename T>
T Reg(uint32_t value) {
  T r;
  r.value = value;
  return r;
}

}  // namespace

ShaderModifications SelectSpirvModifications(const SpirvShaderTranslator& translator,
                                             const SpirvShader& vs, const SpirvShader* ps,
                                             const ShaderDrawRegs& regs) {
  ShaderModifications out;
  const rex::graphics::Shader* pixel = ps;
  if (!DrawReachesPipeline(vs, pixel, regs)) return out;
  ps = static_cast<const SpirvShader*>(pixel);
  out.draw = true;
  out.pixel_shader_used = ps != nullptr;

  auto program_cntl = Reg<reg::SQ_PROGRAM_CNTL>(regs.sq_program_cntl);
  const bool points = Reg<reg::VGT_DRAW_INITIATOR>(regs.vgt_draw_initiator).prim_type ==
                      xenos::PrimitiveType::kPointList;
  uint32_t param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? vs.writes_interpolators() &
               ps->GetInterpolatorInputMask(
                   program_cntl, Reg<reg::SQ_CONTEXT_MISC>(regs.sq_context_misc), param_gen_pos)
         : 0;

  // Vertex shader (always a plain host vertex shader: GRAW doesn't tessellate).
  Modification vm(translator.GetDefaultVertexShaderModification(
      vs.GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg),
      rex::graphics::Shader::HostVertexShaderType::kVertex));
  vm.vertex.interpolator_mask = interpolator_mask;
  vm.vertex.tessellation_mode = 0;
  auto clip = Reg<reg::PA_CL_CLIP_CNTL>(regs.pa_cl_clip_cntl);
  const uint32_t ucp = clip.clip_disable ? 0 : clip.ucp_ena;
  vm.vertex.user_clip_plane_count = uint32_t(std::popcount(ucp));
  vm.vertex.user_clip_plane_cull = uint32_t(ucp && clip.ucp_cull_only_ena);
  vm.vertex.point_ps_ucp_mode = clip.ps_ucp_mode;
  vm.vertex.output_point_parameters =
      uint32_t((vs.writes_point_size_edge_flag_kill_vertex() & 0b001) && points);
  vm.vertex.vertex_kill_and =
      uint32_t((vs.writes_point_size_edge_flag_kill_vertex() & 0b100) && !clip.vtx_kill_or);
  out.vertex = vm.value;
  if (!ps) return out;

  // Pixel shader: host render targets, no float24 conversion in the shader.
  Modification pm(translator.GetDefaultPixelShaderModification(
      ps->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
  pm.pixel.interpolator_mask = interpolator_mask;
  pm.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          Reg<reg::RB_SURFACE_INFO>(regs.rb_surface_info).msaa_samples,
          Reg<reg::SQ_CONTEXT_MISC>(regs.sq_context_misc).sc_sample_cntl,
          Reg<reg::SQ_INTERPOLATOR_CNTL>(regs.sq_interpolator_cntl).sampling_pattern);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    pm.pixel.param_gen_enable = 1;
    pm.pixel.param_gen_interpolator = param_gen_pos;
    pm.pixel.param_gen_point = uint32_t(points);
  } else {
    pm.pixel.param_gen_enable = 0;
    pm.pixel.param_gen_interpolator = 0;
    pm.pixel.param_gen_point = 0;
  }
  using DepthStencilMode = Modification::DepthStencilMode;
  pm.pixel.depth_stencil_mode =
      ps->implicit_early_z_write_allowed() &&
              (!ps->writes_color_target(0) || !CoverageDependsOnAlpha(regs.rb_colorcontrol))
          ? DepthStencilMode::kEarlyHint
          : DepthStencilMode::kNoModifiers;
  out.pixel = pm.value;
  return out;
}

}  // namespace graw::native
