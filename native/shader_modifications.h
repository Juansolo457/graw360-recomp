// GRAW native renderer: the API-free half of shader variant selection (docs/VULKAN_RENDERER.md).
//
// Each guest shader is translated once per "modification": a few bits the draw's registers
// select (interpolators, clip planes, depth mode, ...). The DXBC (shader_translate.h) and SPIR-V
// (spirv_translate.h) translators have their own modification layouts, but read the same
// registers and decide the same things first: whether the draw reaches the pipeline at all and
// whether it needs its pixel shader.
#pragma once

#include <cstdint>

namespace rex::graphics {
class Shader;
}

namespace graw::native {

// Raw guest register values that select the translation of a draw's shaders.
struct ShaderDrawRegs {
  uint32_t sq_program_cntl = 0;
  uint32_t sq_context_misc = 0;
  uint32_t sq_interpolator_cntl = 0;
  uint32_t pa_cl_clip_cntl = 0;
  uint32_t vgt_draw_initiator = 0;  // only prim_type is read
  uint32_t vgt_output_path_cntl = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_colorcontrol = 0;
  uint32_t rb_color_mask = 0;
  uint32_t rb_color_info[4] = {};  // RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO
};

struct ShaderModifications {
  // false: the plugin drops the draw (EDRAM copy mode, i.e. the D3D's own resolve rectangles, or
  // nothing can rasterize and the vertex shader does not memexport); nothing else is valid then.
  bool draw = false;
  bool pixel_shader_used = false;  // false: draw without a pixel shader (depth only)
  uint64_t vertex = 0;
  uint64_t pixel = 0;
};

// From a register file indexed by register number (DrawState::regs) and the draw's
// xenos::PrimitiveType (DrawState::primitive; the index count would defeat memos).
ShaderDrawRegs ReadShaderDrawRegs(const uint32_t* regs, uint32_t primitive);

// RB_COLORCONTROL: alpha test or alpha to coverage decide coverage.
bool CoverageDependsOnAlpha(uint32_t rb_colorcontrol);

// draw_util::IsPixelShaderNeededWithRasterization.
bool PixelShaderNeeded(const rex::graphics::Shader& ps, const ShaderDrawRegs& regs);

// Whether the draw reaches the pipeline (D3D12/VulkanCommandProcessor::IssueDraw, the host
// render target path), and with which of its shaders: ps becomes nullptr when the draw needs no
// pixel shader. False: the draw is dropped.
bool DrawReachesPipeline(const rex::graphics::Shader& vs, const rex::graphics::Shader*& ps,
                         const ShaderDrawRegs& regs);

}  // namespace graw::native
