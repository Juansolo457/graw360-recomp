// GRAW native renderer: the Windows and DXGI types the API-free code shares with the D3D12
// renderer, defined on the other platforms too (the Linux build has the Vulkan backend only,
// docs/VULKAN_RENDERER.md):
// - Rect: a rectangle in pixels, exclusive right and bottom; RECT on Windows (D3D12_RECT), so the
//   clears' rectangles reach the D3D12 calls as they are;
// - DXGI_FORMAT: the texture cache's host format identifiers (texture_layout.h); elsewhere the
//   formats the shared code names, with dxgiformat.h's values.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef DrawState  // winuser.h maps it to DrawStateA/W; d3d_state.h's struct has that name
#include <dxgiformat.h>
#else
enum DXGI_FORMAT : uint32_t {
  DXGI_FORMAT_UNKNOWN = 0,
  DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
  DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
  DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
  DXGI_FORMAT_R16G16B16A16_UNORM = 11,
  DXGI_FORMAT_R16G16B16A16_SNORM = 13,
  DXGI_FORMAT_R32G32_FLOAT = 16,
  DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
  DXGI_FORMAT_R10G10B10A2_UNORM = 24,
  DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
  DXGI_FORMAT_R8G8B8A8_UNORM = 28,
  DXGI_FORMAT_R8G8B8A8_SNORM = 31,
  DXGI_FORMAT_R16G16_TYPELESS = 33,
  DXGI_FORMAT_R16G16_FLOAT = 34,
  DXGI_FORMAT_R16G16_UNORM = 35,
  DXGI_FORMAT_R16G16_SNORM = 37,
  DXGI_FORMAT_R32_FLOAT = 41,
  DXGI_FORMAT_R8G8_TYPELESS = 48,
  DXGI_FORMAT_R8G8_UNORM = 49,
  DXGI_FORMAT_R8G8_SNORM = 51,
  DXGI_FORMAT_R16_TYPELESS = 53,
  DXGI_FORMAT_R16_FLOAT = 54,
  DXGI_FORMAT_R16_UNORM = 56,
  DXGI_FORMAT_R16_SNORM = 58,
  DXGI_FORMAT_R8_TYPELESS = 60,
  DXGI_FORMAT_R8_UNORM = 61,
  DXGI_FORMAT_R8_SNORM = 63,
  DXGI_FORMAT_BC1_UNORM = 71,
  DXGI_FORMAT_BC2_UNORM = 74,
  DXGI_FORMAT_BC3_UNORM = 77,
  DXGI_FORMAT_BC5_UNORM = 83,
  DXGI_FORMAT_B5G6R5_UNORM = 85,
};
#endif

namespace graw::native {

#if defined(_WIN32)
using Rect = RECT;
#else
struct Rect {
  int32_t left, top, right, bottom;
};
#endif

}  // namespace graw::native
