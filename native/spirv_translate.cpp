// GRAW native renderer: shader variant selection for the SPIR-V translator (spirv_translate.h).
// Follows ReXGlue's src/graphics/vulkan/pipeline_cache.cpp (Xenia, Copyright Ben Vanik, BSD-3;
// adapted for ReXGlue by Tom Clay), reduced to the host render target path with every device
// feature the translator asks about (clip and cull distances) available.
#include "spirv_translate.h"

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include <algorithm>
#include <bit>
#include <cstring>

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

void FillSpirvSystemConstants(const RegisterFile& regs, xenos::PrimitiveType prim, bool polygonal,
                              xenos::Endian index_endian, const ViewportInfo& vp,
                              uint32_t used_textures, const uint8_t* signs,
                              SpirvShaderTranslator::SystemConstants& sys) {
  using T = SpirvShaderTranslator;
  std::memset(&sys, 0, sizeof(sys));
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t flags = 0;
  if (vte.vtx_xy_fmt) flags |= T::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) flags |= T::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) flags |= T::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= T::kSysFlag_PrimitivePolygonal;
  if (rex::graphics::draw_util::IsPrimitiveLine(false, prim)) flags |= T::kSysFlag_PrimitiveLine;
  flags |= uint32_t(surface_info.msaa_samples) << T::kSysFlag_MsaaSamples_Shift;
  const xenos::CompareFunction alpha_func = colorcontrol.alpha_test_enable
                                                ? colorcontrol.alpha_func
                                                : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << T::kSysFlag_AlphaPassIfLess_Shift;
  sys.flags = flags;
  sys.vertex_index_endian = index_endian;
  sys.vertex_base_index = regs.Get<int32_t>(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET);
  sys.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  sys.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  if (!clip.clip_disable) {
    float* ucp = sys.user_clip_planes[0];
    uint32_t remaining = clip.ucp_ena;
    while (remaining) {
      uint32_t i = uint32_t(std::countr_zero(remaining));
      remaining &= remaining - 1;
      std::memcpy(ucp, &regs[rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 4 * sizeof(float));
      ucp += 4;
    }
  }
  sys.tessellation_factor_range_min =
      regs.Get<float>(rex::graphics::XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sys.tessellation_factor_range_max =
      regs.Get<float>(rex::graphics::XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  for (uint32_t i = 0; i < 3; ++i) {
    sys.ndc_scale[i] = vp.ndc_scale[i];
    sys.ndc_offset[i] = vp.ndc_offset[i];
  }
  if (prim == xenos::PrimitiveType::kPointList) {
    auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sys.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sys.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sys.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sys.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sys.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(std::max(vp.xy_extent[0], 1u));
    sys.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(std::max(vp.xy_extent[1], 1u));
  }
  uint32_t remaining = used_textures;
  while (remaining) {
    uint32_t i = uint32_t(std::countr_zero(remaining));
    remaining &= remaining - 1;
    sys.texture_swizzled_signs[i >> 2] |= uint32_t(signs[i]) << ((i & 3) * 8);
  }
  sys.alpha_test_reference = regs.Get<float>(rex::graphics::XE_GPU_REG_RB_ALPHA_REF);
  sys.alpha_to_mask = colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1 << 8) : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    auto ci = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]);
    int32_t bias = ci.color_exp_bias;
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        ci.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)
      bias -= 5;
    uint32_t bits = uint32_t(0x3F800000 + (bias << 23));
    std::memcpy(&sys.color_exp_bias[i], &bits, 4);
  }
}

}  // namespace graw::native
