// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): the
// arguments of a hooked guest D3D call.
//
// What the hook dispatch (graw_d3d_hooks.cpp) hands the native path for each call: which guest
// function it is, its PPC arguments at entry and the tiling call it is nested in.
#pragma once

#include <cstdint>

#include "graw_d3d.h"

namespace graw::native {

// Guest D3D functions that reach the renderer's On* calls (graw_d3d.h has the rest).
enum class HookFn : uint32_t {
  kBeginVertices = fn::kBeginVertices,                // draw; queued at EndVertices
  kBeginIndexedVertices = fn::kBeginIndexedVertices,  // draw; queued at EndVertices
  kDrawVerticesUP = fn::kDrawVerticesUP,              // draw (BeginVertices + copy + End)
  kDrawVertices = fn::kDrawVertices,                  // draw
  kDrawIndexedVertices = fn::kDrawIndexedVertices,    // draw
  kClearF = fn::kClearF,
  kClear = fn::kClear,
  kResolve = fn::kResolve,
  kSwap = fn::kSwap,
};

// The guest D3D's predicated tiling calls a hooked call can be nested in. BeginTiling
// (sub_824D2868) clears through ClearF with the largest tile's extent at the EDRAM base, meaning
// every tile; EndTiling (sub_824D2CA0) resolves each tile, its rectangle in surface pixels and the
// EDRAM holding the tile from its origin (the tile list at device+12996, graw_d3d.h).
enum class TilingCall : uint8_t { kNone, kBegin, kEnd };

// Raw PPC arguments of one hooked call. Integer arguments in r3..r10 and float arguments in
// f1..f4 are captured at entry, before the guest function runs (it clobbers them). The XDK
// D3DDevice_* signatures (graw_d3d.h fn::), as GRAW's D3D reads them:
//   DrawIndexedVertices(dev, prim, base_vertex, start_index, index_count)
//   DrawVertices(dev, prim, start_vertex, vertex_count)
//   DrawVerticesUP(dev, prim, vertex_count, data_ptr, stride)
//   BeginVertices(dev, prim, vertex_count, stride) -> r3 = pointer the game writes into
//   BeginIndexedVertices(dev, prim, base_vertex, vertex_count, index_count, index_format, stride,
//                        ...) -> pointers the game writes into
//   Clear(dev, count, rects_ptr, flags, color; z in f1, stencil in r9)
//   ClearF(dev, flags, rect_ptr, color_ptr; z in f1, stencil in r8)
//   (a float argument takes the next integer register's slot)
//   Resolve(dev, flags, src_rect_ptr, dest_texture, dest_point_ptr, level, slice,
//           clear_color_ptr; clear_z in f1, clear_stencil (r1 + 92), params_ptr (r1 + 100))
//   Swap(dev, front_buffer_texture, params_ptr)
struct HookArgs {
  HookFn fn;
  uint32_t device;  // r3 at entry: the guest D3D device (a guest virtual address)
  uint32_t r[8];    // r3..r10 at entry; r[0] == device
  uint32_t sp;      // r1 at entry: arguments past r10 are on the guest stack
  double f[4];      // f1..f4 at entry
  uint64_t ret;     // r3 after the guest function returned
  TilingCall tiling = TilingCall::kNone;  // the tiling call this one is nested in
};

}  // namespace graw::native
