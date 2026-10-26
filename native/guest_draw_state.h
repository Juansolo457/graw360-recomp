// GRAW native renderer: the API-free part of a guest draw's state (docs/VULKAN_RENDERER.md,
// M3b), shared by the D3D12 (draw.cpp) and Vulkan backends: register-derived state (rasterization,
// depth control, polygon offset, color mask, viewport, scissor), blend factors and sampler keys.
// Ports of ReXGlue's draw_util and D3D12/Vulkan texture cache code (Xenia, Copyright Ben Vanik,
// BSD-3; adapted for ReXGlue by Tom Clay), moved out of draw.cpp unchanged.
#pragma once

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

#include <simde/x86/sse2.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>

namespace graw::native {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using rex::graphics::RegisterFile;
using rex::graphics::draw_util::Scissor;
using rex::graphics::draw_util::ViewportInfo;
namespace texture_util = rex::graphics::texture_util;

// reg::RB_COLOR_INFO / RB_BLENDCONTROL::rt_register_indices live in the plugin DLL (not exported).
inline constexpr rex::graphics::Register kColorInfoRegs[4] = {
    rex::graphics::XE_GPU_REG_RB_COLOR_INFO, rex::graphics::XE_GPU_REG_RB_COLOR1_INFO,
    rex::graphics::XE_GPU_REG_RB_COLOR2_INFO, rex::graphics::XE_GPU_REG_RB_COLOR3_INFO};
inline constexpr rex::graphics::Register kBlendControlRegs[4] = {
    rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL1,
    rex::graphics::XE_GPU_REG_RB_BLENDCONTROL2, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL3};

// ---- draw_util ports (host render targets, no resolution scale) ----

bool IsRasterizationPotentiallyDone(const RegisterFile& regs, bool primitive_polygonal);
reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const RegisterFile& regs);
void GetPreferredFacePolygonOffset(const RegisterFile& regs, bool primitive_polygonal,
                                   float& scale_out, float& offset_out);
uint32_t GetNormalizedColorMask(const RegisterFile& regs, uint32_t ps_writes_color_targets);
// GetHostViewportInfo for the host render target path (see guest_draw_state.cpp).
// viewport_bounds_max: the host's viewport bound (D3D12_VIEWPORT_BOUNDS_MAX, 32767, on D3D12).
// origin_bottom_left: NDC +Y up (D3D12); false for Vulkan, whose +Y points down.
void HostViewportInfo(const RegisterFile& regs, bool pixel_shader_writes_depth, ViewportInfo& out,
                      uint32_t viewport_bounds_max = 32767, bool origin_bottom_left = true);
void HostScissor(const RegisterFile& regs, Scissor& out);

// ---- blend factors ----

enum class BlendFactor : uint8_t {
  kZero,
  kOne,
  kSrcColor,
  kInvSrcColor,
  kSrcAlpha,
  kInvSrcAlpha,
  kDestColor,
  kInvDestColor,
  kDestAlpha,
  kInvDestAlpha,
  kBlendFactor,
  kInvBlendFactor,
  kSrcAlphaSat,
};
inline constexpr BlendFactor kBlendFactorMap[32] = {
    BlendFactor::kZero,        BlendFactor::kOne,
    BlendFactor::kZero,        BlendFactor::kZero,
    BlendFactor::kSrcColor,    BlendFactor::kInvSrcColor,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,
    BlendFactor::kDestColor,   BlendFactor::kInvDestColor,
    BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kSrcAlphaSat,
};
inline constexpr BlendFactor kBlendFactorAlphaMap[32] = {
    BlendFactor::kZero,        BlendFactor::kOne,
    BlendFactor::kZero,        BlendFactor::kZero,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,
    BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kSrcAlphaSat,
};

// ---- samplers ----

// Every sampler field the guest's fetch constant and shader binding select, packed losslessly.
struct SamplerKey {
  uint32_t clamp_x : 3 = 0, clamp_y : 3 = 0, clamp_z : 3 = 0;  // xenos::ClampMode
  uint32_t border : 2 = 0;                                     // xenos::BorderColor
  uint32_t aniso : 3 = 0;                                      // xenos::AnisoFilter
  uint32_t mag_linear : 1 = 0, min_linear : 1 = 0, mip_linear : 1 = 0, mip_base_map : 1 = 0;
  uint32_t mip_min_level : 4 = 0;  // below xenos::kTextureMaxMips
  uint32_t reserved : 10 = 0;
  uint32_t value() const { return std::bit_cast<uint32_t>(*this); }
};
static_assert(sizeof(SamplerKey) == sizeof(uint32_t));

// The clamp modes the host has (halfway clamps become edge clamps).
xenos::ClampMode NormalizeClampMode(xenos::ClampMode m);

// `min_aniso`: --graw_native_anisotropy (DrawRenderer::Impl::min_aniso). `Binding`: DxbcShader's
// or SpirvShader's SamplerBinding (the same fields).
template <typename Binding>
SamplerKey GetSamplerKey(const RegisterFile& regs, const Binding& binding,
                         xenos::AnisoFilter min_aniso) {
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  SamplerKey k{};
  xenos::ClampMode cx, cy, cz;
  texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
  cx = NormalizeClampMode(cx);
  cy = NormalizeClampMode(cy);
  cz = NormalizeClampMode(cz);
  k.clamp_x = uint32_t(cx);
  k.clamp_y = uint32_t(cy);
  k.clamp_z = uint32_t(cz);
  k.border = (xenos::ClampModeUsesBorder(cx) || xenos::ClampModeUsesBorder(cy) ||
              xenos::ClampModeUsesBorder(cz))
                 ? uint32_t(fetch.border_color)
                 : uint32_t(xenos::BorderColor::k_ABGR_Black);
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  k.mip_min_level = mip_min_level;
  auto mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter
                                                                        : binding.mag_filter;
  auto min = binding.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter
                                                                        : binding.min_filter;
  auto mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                                        : binding.mip_filter;
  auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter
                                                                          : binding.aniso_filter;
  aniso = std::min(aniso, xenos::AnisoFilter::kMax_16_1);
  // Linearly filtered textures with mips get at least `min_aniso`; render targets, lookup
  // tables and other point-sampled or single-level textures keep the game's filtering.
  if (mag == xenos::TextureFilter::kLinear && min == xenos::TextureFilter::kLinear &&
      mip != xenos::TextureFilter::kBaseMap && mip_max_level > mip_min_level)
    aniso = std::max(aniso, min_aniso);
  k.aniso = uint32_t(aniso);
  if (aniso != xenos::AnisoFilter::kDisabled) {
    k.mag_linear = k.min_linear = k.mip_linear = 1;
  } else {
    k.mag_linear = mag == xenos::TextureFilter::kLinear;
    k.min_linear = min == xenos::TextureFilter::kLinear;
    k.mip_linear = mip == xenos::TextureFilter::kLinear;
  }
  k.mip_base_map = mip == xenos::TextureFilter::kBaseMap;
  return k;
}


// ---- byte comparison ----

// Whether `size` bytes at `a` and `b` are equal: 16 bytes at a time, four per round, no
// ordering (cheaper than memcmp).
inline bool Equal(const void* a, const void* b, uint32_t size) {
  auto* x = static_cast<const uint8_t*>(a);
  auto* y = static_cast<const uint8_t*>(b);
  auto load = [](const uint8_t* p) {
    return simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p));
  };
  auto zero = [](simde__m128i v) {
    return simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(v, simde_mm_setzero_si128())) == 0xFFFF;
  };
  uint32_t i = 0;
  for (; i + 64 <= size; i += 64) {
    const simde__m128i d = simde_mm_or_si128(
        simde_mm_or_si128(simde_mm_xor_si128(load(x + i), load(y + i)),
                          simde_mm_xor_si128(load(x + i + 16), load(y + i + 16))),
        simde_mm_or_si128(simde_mm_xor_si128(load(x + i + 32), load(y + i + 32)),
                          simde_mm_xor_si128(load(x + i + 48), load(y + i + 48))));
    if (!zero(d)) return false;
  }
  for (; i + 16 <= size; i += 16)
    if (!zero(simde_mm_xor_si128(load(x + i), load(y + i)))) return false;
  return i == size || std::memcmp(x + i, y + i, size - i) == 0;
}


}  // namespace graw::native
