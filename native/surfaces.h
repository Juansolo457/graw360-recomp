// GRAW native renderer: the guest's EDRAM surfaces as both backends see them, with no GPU API
// (render_targets.h is the D3D12 renderer's targets, vulkan_render_targets.h the Vulkan one's):
// the surface registers, the guest Clear calls decoded, and the EDRAM fills clears leave
// (render_targets.h has the model: one host target per guest surface, no EDRAM emulation).
//
// Threading: DecodeClear runs on the guest thread; EdramFills on the thread that records the
// frames.
#pragma once

#include "host_types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace graw::native {

struct HookArgs;

// Raw EDRAM surface registers of the current render targets.
struct SurfaceRegs {
  uint32_t surface_info = 0;    // RB_SURFACE_INFO (0x2000)
  uint32_t color_info[4] = {};  // RB_COLOR_INFO (0x2001), RB_COLOR1..3_INFO (0x2003..0x2005)
  uint32_t depth_info = 0;      // RB_DEPTH_INFO (0x2002)
};

// From a register file indexed by register number (DrawState::regs).
SurfaceRegs SurfaceRegsFromRegisters(const uint32_t* regs);
// From the guest device's register shadow (block 0x2000 at device+11456).
SurfaceRegs SurfaceRegsFromDevice(const uint8_t* base, uint32_t device);

// Samples per pixel of RB_SURFACE_INFO's msaa_samples (bits 16-17): 1, 2 or 4.
inline uint32_t SurfaceSamples(uint32_t surface_info) {
  const uint32_t msaa = (surface_info >> 16) & 3;
  return msaa >= 2 ? 4 : msaa == 1 ? 2 : 1;
}

// GRAW's guest D3D device fields this module reads (docs/NATIVE_RENDERER.md). The register
// shadow blocks are the ones the draw flush sub_824D9E98 sends; the surfaces are what
// SetRenderTarget / SetDepthStencilSurface (sub_824D0FB8) bind and Resolve (sub_824D1960) reads.
constexpr uint32_t kDevShadow2000 = 11456;     // registers 0x2000..
constexpr uint32_t kDevShadow2100 = 11532;     // registers 0x2100..
constexpr uint32_t kDevShadow2300 = 11768;     // registers from 0x2300 (the RB_COPY_* block)
constexpr uint32_t kDevColorSurfaces = 12536;  // 4 x guest D3D surface object
constexpr uint32_t kDevDepthSurface = 12552;
// Predicated tiling, as BeginTiling (sub_824D2868) stores it: the tile count, each tile's
// rectangle in surface pixels {x1, y1, x2, y2} (16 bytes), and each tile's rectangle rounded
// out to 32 pixels {x1, y1, x2, y2} (16 bytes; PGR4's D3D keeps only the origin, 8 bytes): its
// top left is the surface pixel the guest renders at the EDRAM base.
constexpr uint32_t kDevTileCount = 12996;
constexpr uint32_t kDevTileRects = 13000;
constexpr uint32_t kDevTileOrigins = 13240;
constexpr uint32_t kDevTileOriginStride = 16;
constexpr uint32_t kMaxTiles = 15;

// Decoded Clear / ClearF arguments.
struct ClearParams {
  uint32_t flags = 0;   // D3DCLEAR_TARGET0..3 = 1..8, ZBUFFER = 0x10, STENCIL = 0x20
  float color[4] = {};  // RGBA
  float depth = 1.0f;
  uint8_t stencil = 0;
  std::vector<Rect> rects;  // surface pixels; empty: whole surface
  // The EDRAM the guest clears, in pixels from the surface's EDRAM base, where it is not
  // `rects`: under predicated tiling every tile is rendered at the base, so BeginTiling's
  // ClearF clears the largest tile's extent there once (and `rects` holds every tile), and
  // EndTiling's resolve clears land at the base whatever the tile's position.
  std::vector<Rect> edram_rects;
};

// One Clear / ClearF call, decoded from its arguments and the device (guest memory).
struct ClearCall {
  ClearParams p;
  SurfaceRegs regs;
  uint32_t bound_colors = 0;  // bit i: a surface is bound as render target i
  bool depth_bound = false;
  uint32_t height = 0;
};

// A surface's EDRAM layout: tiles of 80 dwords x 16 sample rows; a pixel takes 2 x 2 samples
// with 4x MSAA and 1 x 2 with 2x, and a 64bpp sample takes 2 dwords.
struct EdramGrid {
  uint32_t base;   // tile
  uint32_t pitch;  // tiles per row
  uint32_t dx;     // dwords per pixel
  uint32_t dy;     // sample rows per pixel
};
// guest_format: xenos::ColorRenderTargetFormat (ignored for depth).
EdramGrid GetEdramGrid(bool depth, uint32_t edram_base, uint32_t pitch, uint32_t samples,
                       uint32_t guest_format);

// EDRAM residue, for the D3D12 and Vulkan render targets. A color clear leaves one value in
// the EDRAM tiles it covers, and another surface of the same storage format over those tiles
// reads it until something renders there. A fill is one cleared rectangle in EDRAM terms: dword
// columns and sample rows on the cleared surface's tile grid (tiles of 80 dwords x 16 sample
// rows, `pitch` tiles per row from tile `base`). A draw or a depth clear over any of its tiles
// drops a fill whole.
class EdramFills {
 public:
  struct Fill {
    uint64_t seq = 0;
    uint32_t base = 0, pitch = 1;  // tiles
    uint32_t x0 = 0, x1 = 0;       // dword columns within a tile row
    uint32_t y0 = 0, y1 = 0;       // sample rows from base's tile row
    uint32_t format = 0;           // xenos color format as stored (the _AS_ variants folded)
    float color[4] = {};
    uint32_t first_tile() const { return base + y0 / 16 * pitch + x0 / 80; }
    uint32_t end_tile() const { return base + (y1 - 1) / 16 * pitch + (x1 - 1) / 80 + 1; }
  };

  // A clear of `r` (pixels from the grid's base) on a surface of color format `format`.
  void Record(const EdramGrid& g, uint32_t format, const Rect& r, const float color[4]);
  // Drops the fills over the grid's first `rows` rows of pixels.
  void Drop(const EdramGrid& g, uint32_t rows);
  void Clear() { fills_.clear(); }
  bool empty() const { return fills_.empty(); }
  // The sequence number of the last fill recorded (a surface that applied fills up to it is
  // current).
  uint64_t seq() const { return seq_; }
  // The fills recorded after `since` over a color surface (grid g, color format `format`,
  // width x height pixels), oldest first: `apply` gets each with its whole pixels as up to 3
  // rectangles clipped to the surface. Fills of another storage format, or of a shape that
  // does not map onto g, are skipped (logged).
  void ForEach(uint64_t since, const EdramGrid& g, uint32_t format, uint32_t width,
               uint32_t height,
               const std::function<void(const Fill&, const Rect*, uint32_t)>& apply) const;

 private:
  static constexpr size_t kMaxFills = 32;
  std::vector<Fill> fills_;  // oldest first
  uint64_t seq_ = 0;
};

// Guest Clear / ClearF (HookFn kClear / kClearF), after the guest call returned: the surfaces
// bound in the device's register shadow and the clear values (reads guest memory only); false:
// nothing to clear.
bool DecodeClear(const uint8_t* base, const HookArgs& args, ClearCall* out);

// Whether a color render target format (xenos::ColorRenderTargetFormat) has 64 bits per sample.
bool IsColor64bpp(uint32_t color_format);

}  // namespace graw::native
