// GRAW native renderer: shader variant selection and system constants for the SPIR-V
// translator (Vulkan backend).
//
// Mirrors VulkanPipelineCache::GetCurrent{Vertex,Pixel}ShaderModification and
// VulkanCommandProcessor::IssueDraw / UpdateSystemConstantValues for the host render target
// path, as shader_translate.h and draw.cpp's FillSystemConstants do for D3D12. The SPIR-V translator is ReXGlue's (BSD-3, from Xenia), compiled into the app
// (shader_xlate.cmake, graw_shader_xlate_spirv).
#pragma once

#include "guest_draw_state.h"
#include "shader_modifications.h"

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>

namespace graw::native {

using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;

// Both shaders must have their ucode analyzed; ps may be null.
ShaderModifications SelectSpirvModifications(const SpirvShaderTranslator& translator,
                                             const SpirvShader& vs, const SpirvShader* ps,
                                             const ShaderDrawRegs& regs);

// The draw's system constants, with the D3D12 path's choices (draw.cpp, FillSystemConstants):
// indices come from a host index buffer (no shader index loading), no float24 depth flag, gamma
// render targets are unorm16 on the host (no conversion flag), snorm16 render targets use the
// full -32...32 range. `index_endian`: of the guest indices (kNone if not indexed). `signs`: the
// bound textures' swizzled signs by fetch constant (32), for the textures in `used_textures`.
// Host swizzles come from image views (texture_swizzles stays 0).
void FillSpirvSystemConstants(const RegisterFile& regs, xenos::PrimitiveType prim, bool polygonal,
                              xenos::Endian index_endian, const ViewportInfo& vp,
                              uint32_t used_textures, const uint8_t* signs,
                              SpirvShaderTranslator::SystemConstants& sys);

}  // namespace graw::native
