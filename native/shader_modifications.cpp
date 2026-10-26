// GRAW native renderer: the API-free half of shader variant selection (shader_modifications.h).
// Follows ReXGlue's src/graphics/*/command_processor.cpp IssueDraw and draw_util (Xenia,
// Copyright Ben Vanik, BSD-3; adapted for ReXGlue by Tom Clay).
#include "shader_modifications.h"

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

#include <bit>

namespace graw::native {

namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;
using namespace rex::graphics;  // XE_GPU_REG_*

namespace {

template <typename T>
T Reg(uint32_t value) {
  T r;
  r.value = value;
  return r;
}

constexpr uint32_t kColorInfoRegs[4] = {XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                                        XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO};

}  // namespace

ShaderDrawRegs ReadShaderDrawRegs(const uint32_t* regs, uint32_t primitive) {
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = 0;
  initiator.prim_type = xenos::PrimitiveType(primitive);
  ShaderDrawRegs sdr;
  sdr.sq_program_cntl = regs[XE_GPU_REG_SQ_PROGRAM_CNTL];
  sdr.sq_context_misc = regs[XE_GPU_REG_SQ_CONTEXT_MISC];
  sdr.sq_interpolator_cntl = regs[XE_GPU_REG_SQ_INTERPOLATOR_CNTL];
  sdr.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  sdr.vgt_draw_initiator = initiator.value;
  sdr.vgt_output_path_cntl = regs[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL];
  sdr.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  sdr.rb_modecontrol = regs[XE_GPU_REG_RB_MODECONTROL];
  sdr.rb_surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  sdr.rb_colorcontrol = regs[XE_GPU_REG_RB_COLORCONTROL];
  sdr.rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < 4; ++i) sdr.rb_color_info[i] = regs[kColorInfoRegs[i]];
  return sdr;
}

bool CoverageDependsOnAlpha(uint32_t rb_colorcontrol) {
  const auto c = Reg<reg::RB_COLORCONTROL>(rb_colorcontrol);
  return (c.alpha_test_enable && c.alpha_func != xenos::CompareFunction::kAlways) ||
         c.alpha_to_mask_enable;
}

bool PixelShaderNeeded(const rex::graphics::Shader& ps, const ShaderDrawRegs& regs) {
  if (Reg<reg::RB_MODECONTROL>(regs.rb_modecontrol).edram_mode != xenos::EdramMode::kColorDepth) {
    return false;
  }
  if (ps.kills_pixels() || ps.writes_depth() || ps.memexport_eM_written() ||
      (ps.writes_color_target(0) && CoverageDependsOnAlpha(regs.rb_colorcontrol))) {
    return true;
  }
  uint32_t rts = ps.writes_color_targets();
  while (rts) {
    uint32_t i = uint32_t(std::countr_zero(rts));
    rts &= rts - 1;
    uint32_t components = xenos::GetColorRenderTargetFormatComponentCount(
        Reg<reg::RB_COLOR_INFO>(regs.rb_color_info[i]).color_format);
    if ((regs.rb_color_mask >> (i * 4)) & ((1u << components) - 1)) return true;
  }
  return false;
}

bool DrawReachesPipeline(const rex::graphics::Shader& vs, const rex::graphics::Shader*& ps,
                         const ShaderDrawRegs& regs) {
  auto program_cntl = Reg<reg::SQ_PROGRAM_CNTL>(regs.sq_program_cntl);
  auto draw_initiator = Reg<reg::VGT_DRAW_INITIATOR>(regs.vgt_draw_initiator);
  auto mode = Reg<reg::RB_MODECONTROL>(regs.rb_modecontrol).edram_mode;
  if (mode == xenos::EdramMode::kCopy) return false;
  bool polygonal = rex::graphics::draw_util::IsPrimitivePolygonal(
      Reg<reg::VGT_OUTPUT_PATH_CNTL>(regs.vgt_output_path_cntl).path_select ==
          xenos::VGTOutputPath::kTessellationEnable,
      draw_initiator.prim_type);
  // draw_util::IsRasterizationPotentiallyDone.
  auto sc_mode = Reg<reg::PA_SU_SC_MODE_CNTL>(regs.pa_su_sc_mode_cntl);
  bool pitch_zero = !Reg<reg::RB_SURFACE_INFO>(regs.rb_surface_info).surface_pitch;
  bool rasterization =
      (mode == xenos::EdramMode::kColorDepth || mode == xenos::EdramMode::kDepthOnly) &&
      program_cntl.vs_export_mode != xenos::VertexShaderExportMode::kMultipass && !pitch_zero &&
      !(polygonal && sc_mode.cull_front && sc_mode.cull_back);
  if (rasterization) {
    if (ps && !PixelShaderNeeded(*ps, regs)) ps = nullptr;  // also drops it outside kColorDepth
  } else {
    if (!vs.memexport_eM_written()) return false;
    ps = nullptr;
  }
  return true;
}

}  // namespace graw::native
