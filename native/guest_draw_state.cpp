// GRAW native renderer: the API-free part of a guest draw's state (guest_draw_state.h).
#include "guest_draw_state.h"

#include <cmath>

namespace graw::native {

using enum rex::graphics::Register;  // XE_GPU_REG_* (clang-cl, C++20)

bool IsRasterizationPotentiallyDone(const RegisterFile& regs, bool primitive_polygonal) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return false;
  }
  if (regs.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode ==
          xenos::VertexShaderExportMode::kMultipass ||
      !regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) {
    return false;
  }
  if (primitive_polygonal) {
    auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (sc.cull_front && sc.cull_back) return false;
  }
  return true;
}

reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const RegisterFile& regs) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    reg::RB_DEPTHCONTROL disabled;
    disabled.value = 0;
    return disabled;
  }
  reg::RB_DEPTHCONTROL depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
  if (depthcontrol.z_enable && !depthcontrol.z_write_enable &&
      depthcontrol.zfunc == xenos::CompareFunction::kAlways) {
    depthcontrol.z_enable = 0;
  }
  return depthcontrol;
}

void GetPreferredFacePolygonOffset(const RegisterFile& regs, bool primitive_polygonal,
                                   float& scale_out, float& offset_out) {
  float scale = 0.0f, offset = 0.0f;
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    if (sc.poly_offset_front_enable && !sc.cull_front) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (sc.poly_offset_back_enable && !sc.cull_back && !scale && !offset) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else if (sc.poly_offset_para_enable) {
    scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
    offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  scale_out = scale;
  offset_out = offset;
}

uint32_t GetNormalizedColorMask(const RegisterFile& regs, uint32_t ps_writes_color_targets) {
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) return 0;
  uint32_t normalized = 0;
  uint32_t rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(ps_writes_color_targets & (1u << i))) continue;
    uint32_t format_mask = (1u << xenos::GetColorRenderTargetFormatComponentCount(
                                regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]).color_format)) -
                           1;
    uint32_t rt_mask = (rb_color_mask >> (4 * i)) & format_mask;
    if (!rt_mask) continue;
    rt_mask |= 0b1111 & ~format_mask;
    normalized |= rt_mask << (4 * i);
  }
  return normalized;
}

// GetHostViewportInfo with origin_bottom_left = true, x/y max = viewport_bounds_max, no
// reverse Z, no float24 conversion, resolution scale 1 and half_pixel_offset on (the plugin's
// settings), and full_float24_in_0_to_1 = false: the host depth buffers here are not shared
// with EDRAM ownership transfers, so depth stays in 0...1 (the Clear values RenderTargets uses,
// and the shader-written depth: the system flags never carry kSysFlag_DepthFloat24).
void HostViewportInfo(const RegisterFile& regs, bool pixel_shader_writes_depth, ViewportInfo& out,
                      uint32_t viewport_bounds_max) {
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto vtx = regs.Get<reg::PA_SU_VTX_CNTL>();
  float scale_xy[] = {
      vte.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
      vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f,
  };
  float scale_z = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  float offset_base_xy[] = {
      vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
      vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f,
  };
  float offset_z = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  float offset_add_xy[2] = {};
  if (sc.vtx_window_offset_enable) {
    auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_add_xy[0] += float(wo.window_x_offset);
    offset_add_xy[1] += float(wo.window_y_offset);
  }
  if (vtx.pix_center == xenos::PixelCenter::kD3DZero) {
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }
  const uint32_t xy_max[] = {viewport_bounds_max, viewport_bounds_max};
  float z_min, z_max, ndc_scale[3], ndc_offset[3];
  if (clip.clip_disable) {
    for (uint32_t i = 0; i < 2; ++i) {
      out.xy_offset[i] = 0;
      uint32_t extent = std::min(xenos::kTexture2DCubeMaxWidthHeight, xy_max[i]);
      out.xy_extent[i] = extent;
      float extent_f = float(extent);
      float px_to_ndc = 2.0f / extent_f;
      ndc_scale[i] = scale_xy[i] * px_to_ndc;
      ndc_offset[i] = (offset_base_xy[i] - extent_f * 0.5f + offset_add_xy[i]) * px_to_ndc;
    }
    z_min = 0.0f;
    z_max = 1.0f;
    ndc_scale[2] = scale_z;
    ndc_offset[2] = offset_z;
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      float offset_axis = offset_base_xy[i] + offset_add_xy[i];
      float scale_axis = scale_xy[i];
      float scale_abs = std::abs(scale_axis);
      float max_f = float(xy_max[i]);
      uint32_t a0 = uint32_t(std::clamp(offset_axis - scale_abs, 0.0f, max_f));
      uint32_t a1 = uint32_t(std::clamp(offset_axis + scale_abs, 0.0f, max_f));
      uint32_t extent = a1 - a0;
      out.xy_offset[i] = a0;
      out.xy_extent[i] = extent;
      if (extent) {
        float extent_f = float(extent);
        ndc_scale[i] = scale_axis * 2.0f / extent_f;
        ndc_offset[i] = (offset_axis - (float(a0) + extent_f * 0.5f)) * 2.0f / extent_f;
      } else {
        ndc_scale[i] = 1.0f;
        ndc_offset[i] = 0.0f;
      }
    }
    float host_offset_z, host_scale_z;
    if (clip.dx_clip_space_def) {
      host_offset_z = offset_z;
      host_scale_z = scale_z;
      ndc_scale[2] = 1.0f;
      ndc_offset[2] = 0.0f;
    } else {
      host_offset_z = offset_z - scale_z;
      host_scale_z = scale_z * 2.0f;
      ndc_scale[2] = 0.5f;
      ndc_offset[2] = 0.5f;
    }
    if (pixel_shader_writes_depth) {
      z_min = 0.0f;
      z_max = 1.0f;
    } else {
      z_min = std::clamp(host_offset_z, 0.0f, 1.0f);
      z_max = std::clamp(host_offset_z + host_scale_z, 0.0f, 1.0f);
      if (z_min > z_max) {
        std::swap(z_min, z_max);
        ndc_scale[2] = -ndc_scale[2];
        ndc_offset[2] = 1.0f - ndc_offset[2];
      }
    }
  }
  out.z_min = z_min;
  out.z_max = z_max;
  ndc_scale[1] = -ndc_scale[1];  // origin_bottom_left
  ndc_offset[1] = -ndc_offset[1];
  for (uint32_t i = 0; i < 3; ++i) {
    out.ndc_scale[i] = ndc_scale[i];
    out.ndc_offset[i] = ndc_offset[i];
  }
}

void HostScissor(const RegisterFile& regs, Scissor& out) {
  auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t tl_x = int32_t(tl.tl_x), tl_y = int32_t(tl.tl_y);
  int32_t br_x = int32_t(br.br_x), br_y = int32_t(br.br_y);
  if (!tl.window_offset_disable) {
    auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    tl_x += wo.window_x_offset;
    tl_y += wo.window_y_offset;
    br_x += wo.window_x_offset;
    br_y += wo.window_y_offset;
  }
  auto stl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  tl_x = std::max(tl_x, int32_t(stl.tl_x));
  tl_y = std::max(tl_y, int32_t(stl.tl_y));
  br_x = std::min(br_x, int32_t(sbr.br_x));
  br_y = std::min(br_y, int32_t(sbr.br_y));
  uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  tl_x = std::min(tl_x, int32_t(pitch));
  br_x = std::min(br_x, int32_t(pitch));
  tl_x = std::max(tl_x, 0);
  tl_y = std::max(tl_y, 0);
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  out.offset[0] = uint32_t(tl_x);
  out.offset[1] = uint32_t(tl_y);
  out.extent[0] = uint32_t(br_x - tl_x);
  out.extent[1] = uint32_t(br_y - tl_y);
}


xenos::ClampMode NormalizeClampMode(xenos::ClampMode m) {
  if (m == xenos::ClampMode::kClampToHalfway) return xenos::ClampMode::kClampToEdge;
  if (m == xenos::ClampMode::kMirrorClampToHalfway || m == xenos::ClampMode::kMirrorClampToBorder)
    return xenos::ClampMode::kMirrorClampToEdge;
  return m;
}


}  // namespace graw::native
