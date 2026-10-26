// GRAW native renderer: the guest's EDRAM surfaces, decoded clears and EDRAM fills (surfaces.h).
#include "surfaces.h"

#include "guest_memory.h"
#include "hook_args.h"
#include "log.h"

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace graw::native {
namespace {

namespace xenos = rex::graphics::xenos;

constexpr uint32_t kMaxHeight = 2048;

// Tiles [base, end) under a surface's first `rows` rows of pixels.
uint32_t GridEnd(const EdramGrid& g, uint32_t rows) {
  return g.base + (rows * g.dy + 15) / 16 * g.pitch;
}

// The EDRAM format a color format stores as (the _AS_ formats only change blending).
uint32_t StorageFormat(uint32_t color_format) {
  return color_format == 10 ? 2u : color_format == 12 ? 3u : color_format;
}

// Dword columns and sample rows on a tile grid (an EDRAM fill's rectangle, render_targets.h).
struct GridSpan {
  int64_t x0, x1, y0, y1;
};

// The spans of grid `g` that `s` on the grid at tile `base` with `pitch` covers: one on the
// same pitch at a whole number of rows from g's base, up to three (a row's tail, whole rows, a
// row's head) for whole tile rows on another pitch. 0 when it covers nothing of g, -1 when its
// shape does not map (part of a row on another pitch).
int32_t MapSpan(uint32_t base, uint32_t pitch, const GridSpan& s, const EdramGrid& g,
                GridSpan* out) {
  const int64_t d = int64_t(base) - int64_t(g.base);  // tiles
  const int64_t p = g.pitch;
  if (pitch == g.pitch && d % p == 0) {
    out[0] = {s.x0, s.x1, s.y0 + d / p * 16, s.y1 + d / p * 16};
    return 1;
  }
  if (s.x0 != 0 || s.x1 != int64_t(pitch) * 80 || s.y0 % 16 || s.y1 % 16) return -1;
  const int64_t a = std::max<int64_t>(d + s.y0 / 16 * pitch, 0);
  const int64_t b = d + s.y1 / 16 * pitch;
  if (b <= a) return 0;
  const int64_t ra = a / p, rb = b / p;
  if (ra == rb) {
    out[0] = {a % p * 80, b % p * 80, ra * 16, ra * 16 + 16};
    return 1;
  }
  int32_t n = 0;
  int64_t full = ra;
  if (a % p) {
    out[n++] = {a % p * 80, p * 80, ra * 16, ra * 16 + 16};
    ++full;
  }
  if (rb > full) out[n++] = {0, p * 80, full * 16, rb * 16};
  if (b % p) out[n++] = {0, b % p * 80, rb * 16, rb * 16 + 16};
  return n;
}

}  // namespace

bool IsColor64bpp(uint32_t color_format) {
  return xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat(color_format));
}

EdramGrid GetEdramGrid(bool depth, uint32_t edram_base, uint32_t pitch, uint32_t samples,
                       uint32_t guest_format) {
  EdramGrid g;
  g.base = edram_base;
  g.dx = (samples >= 4 ? 2 : 1) * (!depth && IsColor64bpp(guest_format) ? 2 : 1);
  g.dy = samples >= 2 ? 2 : 1;
  g.pitch = std::max(1u, (pitch * g.dx + 79) / 80);
  return g;
}

void EdramFills::Record(const EdramGrid& g, uint32_t format, const Rect& r,
                        const float color[4]) {
  Fill f;
  f.base = g.base;
  f.pitch = g.pitch;
  f.x0 = uint32_t(std::max<int32_t>(r.left, 0)) * g.dx;
  f.x1 = std::min(uint32_t(std::max<int32_t>(r.right, 0)) * g.dx, g.pitch * 80);
  f.y0 = uint32_t(std::max<int32_t>(r.top, 0)) * g.dy;
  f.y1 = uint32_t(std::max<int32_t>(r.bottom, 0)) * g.dy;
  if (f.x1 <= f.x0 || f.y1 <= f.y0) return;
  f.format = StorageFormat(format);
  std::memcpy(f.color, color, sizeof(f.color));
  // Older fills inside this one are gone from the EDRAM: on the same grid, or anywhere in its
  // run of tiles when it covers whole tile rows.
  const bool whole_rows = f.x0 == 0 && f.x1 == f.pitch * 80 && f.y0 % 16 == 0 && f.y1 % 16 == 0;
  auto covered = [&](const Fill& o) {
    if (o.base == f.base && o.pitch == f.pitch)
      return o.x0 >= f.x0 && o.x1 <= f.x1 && o.y0 >= f.y0 && o.y1 <= f.y1;
    return whole_rows && o.first_tile() >= f.first_tile() && o.end_tile() <= f.end_tile();
  };
  fills_.erase(std::remove_if(fills_.begin(), fills_.end(), covered), fills_.end());
  if (fills_.size() >= kMaxFills) fills_.erase(fills_.begin());
  f.seq = ++seq_;
  fills_.push_back(f);
}

void EdramFills::Drop(const EdramGrid& g, uint32_t rows) {
  const uint32_t begin = g.base, end = GridEnd(g, rows);
  fills_.erase(std::remove_if(fills_.begin(), fills_.end(),
                              [&](const Fill& f) {
                                return f.first_tile() < end && f.end_tile() > begin;
                              }),
               fills_.end());
}

void EdramFills::ForEach(
    uint64_t since, const EdramGrid& g, uint32_t format, uint32_t width, uint32_t height,
    const std::function<void(const Fill&, const Rect*, uint32_t)>& apply) const {
  const uint32_t end = GridEnd(g, height);
  format = StorageFormat(format);
  for (const Fill& f : fills_) {
    if (f.seq <= since || f.first_tile() >= end || f.end_tile() <= g.base) continue;
    if (f.format != format) {
      GRAW_LOG_FIRST(
          4, INFO,
          "GRAW native RT: EDRAM fill of format {} at tile {} not applied to format {} at base {}",
          f.format, f.base, format, g.base);
      continue;
    }
    GridSpan spans[3];
    const int32_t n = MapSpan(f.base, f.pitch, {f.x0, f.x1, f.y0, f.y1}, g, spans);
    if (n < 0) {
      GRAW_LOG_FIRST(
          4, INFO,
          "GRAW native RT: EDRAM fill at tile {} pitch {} ({}x{} dwords x rows) does not map onto "
          "base {} pitch {}",
          f.base, f.pitch, f.x1 - f.x0, f.y1 - f.y0, g.base, g.pitch);
      continue;
    }
    // The whole pixels inside each span, clipped to the surface.
    Rect rects[3];
    uint32_t nr = 0;
    for (int32_t i = 0; i < n; ++i) {
      const GridSpan& s = spans[i];
      const int64_t x0 = (s.x0 + g.dx - 1) / g.dx, x1 = std::min<int64_t>(s.x1 / g.dx, width);
      const int64_t y0 = (std::max<int64_t>(s.y0, 0) + g.dy - 1) / g.dy;
      const int64_t y1 = std::min<int64_t>(s.y1 / g.dy, height);
      if (x1 > x0 && y1 > y0) rects[nr++] = {int32_t(x0), int32_t(y0), int32_t(x1), int32_t(y1)};
    }
    if (nr) apply(f, rects, nr);
  }
}

SurfaceRegs SurfaceRegsFromRegisters(const uint32_t* regs) {
  SurfaceRegs s;
  s.surface_info = regs[0x2000];
  s.color_info[0] = regs[0x2001];
  s.depth_info = regs[0x2002];
  s.color_info[1] = regs[0x2003];
  s.color_info[2] = regs[0x2004];
  s.color_info[3] = regs[0x2005];
  return s;
}

SurfaceRegs SurfaceRegsFromDevice(const uint8_t* base, uint32_t device) {
  const uint32_t b = device + kDevShadow2000;
  SurfaceRegs s;
  s.surface_info = Load32(base, b + 0 * 4);
  s.color_info[0] = Load32(base, b + 1 * 4);
  s.depth_info = Load32(base, b + 2 * 4);
  s.color_info[1] = Load32(base, b + 3 * 4);
  s.color_info[2] = Load32(base, b + 4 * 4);
  s.color_info[3] = Load32(base, b + 5 * 4);
  return s;
}

bool DecodeClear(const uint8_t* base, const HookArgs& args, ClearCall* out) {
  ClearParams& p = out->p;
  auto read_rect = [&](uint32_t a) {
    // D3DRECT {x1, y1, x2, y2}, big-endian LONGs.
    Rect r;
    r.left = int32_t(Load32(base, a));
    r.top = int32_t(Load32(base, a + 4));
    r.right = int32_t(Load32(base, a + 8));
    r.bottom = int32_t(Load32(base, a + 12));
    if (r.right > r.left && r.bottom > r.top) p.rects.push_back(r);
  };
  if (args.fn == HookFn::kClear) {
    // Clear(dev, Count r4, pRects r5, Flags r6, Color r7, Z f1, Stencil r9): Z takes r8's
    // argument slot (sub_826D1A08 passes r9 on to ClearF as its stencil).
    const uint32_t count = args.r[1], rects = args.r[2], color = args.r[4];
    p.flags = args.r[3];
    p.color[0] = float((color >> 16) & 0xFF) / 255.0f;
    p.color[1] = float((color >> 8) & 0xFF) / 255.0f;
    p.color[2] = float(color & 0xFF) / 255.0f;
    p.color[3] = float(color >> 24) / 255.0f;
    p.depth = float(args.f[0]);
    p.stencil = uint8_t(args.r[6]);
    if (rects)
      for (uint32_t i = 0; i < count && i < 64; ++i) read_rect(rects + i * 16);
    if (rects && count && p.rects.empty()) return false;  // only empty rectangles
  } else if (args.fn == HookFn::kClearF) {
    // ClearF(dev, Flags r4, pRect r5, pColor r6 (D3DVECTOR4), Z f1, Stencil r8): Z takes r7's
    // argument slot (sub_826D1250 reads the stencil from r8).
    p.flags = args.r[1];
    if (args.r[3])
      for (int i = 0; i < 4; ++i) p.color[i] = LoadFloat(base, args.r[3] + 4 * i);
    p.depth = float(args.f[0]);
    p.stencil = uint8_t(args.r[5]);
    if (args.r[2]) {
      read_rect(args.r[2]);
      if (p.rects.empty()) return false;
    }
    // Inside BeginTiling (fn_826D1C38) the rectangle is the largest tile's extent at the EDRAM
    // base. Every tile starts from these values: the first from this clear, each later one
    // from the previous tile's resolve, which EndTiling clears with BeginTiling's color, Z and
    // stencil. The host surface holds all tiles, so it clears each of them.
    if (args.tiling == TilingCall::kBegin && !p.rects.empty()) {
      const uint32_t tiles = std::min(Load32(base, args.device + kDevTileCount), kMaxTiles);
      if (tiles) {
        p.edram_rects = std::move(p.rects);
        p.rects.clear();
        for (uint32_t i = 0; i < tiles; ++i) read_rect(args.device + kDevTileRects + 16 * i);
      }
    }
  } else {
    return false;
  }
  const uint32_t device = args.device;
  out->regs = SurfaceRegsFromDevice(base, device);
  uint32_t bound = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if (Load32(base, device + kDevColorSurfaces + 4 * i)) bound |= 1u << i;
  out->bound_colors = bound;
  out->depth_bound = Load32(base, device + kDevDepthSurface) != 0;
  // The surfaces' height is not in the registers; the viewport bottom is the best hint
  // (PA_CL_VPORT_YSCALE 0x2111, YOFFSET 0x2112). Targets are at least their EDRAM height.
  const float ys = LoadFloat(base, device + kDevShadow2100 + 0x11 * 4);
  const float yo = LoadFloat(base, device + kDevShadow2100 + 0x12 * 4);
  float bottom = std::fabs(ys) + yo;
  uint32_t height =
      (bottom > 0.0f && bottom <= float(kMaxHeight)) ? uint32_t(std::ceil(bottom)) : 0;
  for (const Rect& r : p.rects)
    height = std::max(height, uint32_t(std::max<int32_t>(r.bottom, 0)));
  out->height = height;
  return true;
}

}  // namespace graw::native
