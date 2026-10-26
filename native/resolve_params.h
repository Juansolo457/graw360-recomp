// GRAW native renderer: a guest Resolve call decoded, with no GPU API (both backends' resolves;
// resolve.h has the model and the D3D12 Resolver, vulkan_backend.cpp the Vulkan copies).
#pragma once

#include "surfaces.h"

#include <cstdint>

namespace graw::native {

// One decoded Resolve call.
struct ResolveParams {
  SurfaceRegs regs;                          // the source surfaces (register shadow at the call)
  uint32_t source = 0;                       // 0-3 color render target, 4 depth
  uint32_t sample_select = 0;                // xenos::CopySampleSelect
  uint32_t copy_command = 1;                 // xenos::CopyCommand: 0 raw (depth), 1 convert
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // source rectangle, surface pixels, exclusive end
  int32_t dest_x = 0, dest_y = 0;            // destination point
  uint32_t dest_base = 0;                    // physical address of the destination level/slice
  uint32_t dest_format = 0;                  // xenos::TextureFormat (copy destination format)
  uint32_t dest_width = 0, dest_height = 0;  // destination level size, texels
  uint32_t dest_endian = 0;  // xenos::Endian (dumps only; the host image is unswapped)
  uint32_t dest_number = 0;  // xenos::SurfaceNumberFormat: 0/1 unorm/snorm, 2/3 uint/sint, 7 float
  bool dest_swap = false;    // swap red and blue
  int32_t exp_bias = 0;      // color *= 2^exp_bias (convert only)
  // Guest layout of the destination (dumps only).
  bool dest_tiled = false;
  uint32_t dest_pitch_blocks = 0;  // row pitch in texels (tiled: 32-aligned)
  uint32_t dest_extent_bytes = 0;  // guest memory the level/slice occupies
  // Clears after the copy (Flags 0x100 color, 0x200 depth+stencil) over the source rect, all
  // of it on the target even where the copy is clipped to the destination level.
  // Inside EndTiling the rect is one tile, which the guest rendered with its origin (edram_x,
  // edram_y) at the surface's EDRAM base, so that is where the clear lands in the EDRAM.
  uint32_t clear_flags = 0;
  int32_t edram_x = 0, edram_y = 0;
  float clear_color[4] = {};
  float clear_depth = 1.0f;
  uint8_t clear_stencil = 0;
};

// Decodes a Resolve hook call (`args` = r3..r10, f1, sp at entry; guest memory at `base`), after
// the guest Resolve returned, and logs where its copy registers in the D3D's shadow disagree.
// Returns false (and logs once per reason) for what is not supported yet: packed mip tails,
// 3D destinations, copy commands other than raw/convert.
bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out);

// log2 of the bytes per texel of a copy destination format (xenos::TextureFormat); -1 if the
// resolves can't write it.
int DestBppLog2(uint32_t dest_format);
// Whether a copy destination format holds floats.
bool IsFloatFormat(uint32_t dest_format);

}  // namespace graw::native
