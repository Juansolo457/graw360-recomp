// GRAW native renderer: shader variant selection for the SPIR-V translator (Vulkan backend).
//
// Mirrors VulkanPipelineCache::GetCurrent{Vertex,Pixel}ShaderModification and
// VulkanCommandProcessor::IssueDraw for the host render target path, as shader_translate.h does
// for D3D12. The SPIR-V translator is ReXGlue's (BSD-3, from Xenia), compiled into the app
// (shader_xlate.cmake, graw_shader_xlate_spirv).
#pragma once

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

}  // namespace graw::native
