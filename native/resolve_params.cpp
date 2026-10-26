// GRAW native renderer: guest Resolve calls decoded (resolve_params.h).
#include "resolve_params.h"

#include "guest_memory.h"
#include "hook_args.h"
#include "log.h"
#include "texture_layout.h"

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <algorithm>

namespace graw::native {
namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

// The guest D3D's mapping from the destination texture's format to RB_COPY_DEST_INFO's
// (sub_826CBEE0: the cmplwi chain on the fetch format).
uint32_t D3DCopyDestFormat(uint32_t f) {
  using enum xenos::TextureFormat;
  switch (xenos::TextureFormat(f)) {
    case k_2_10_10_10_AS_16_16_16_16:
      return uint32_t(k_2_10_10_10);
    case k_10_11_11_AS_16_16_16_16:
      return uint32_t(k_10_11_11);
    case k_11_11_10_AS_16_16_16_16:
      return uint32_t(k_11_11_10);
    case k_16_EXPAND:
      return uint32_t(k_16_FLOAT);
    case k_16_16_EXPAND:
      return uint32_t(k_16_16_FLOAT);
    case k_16_16_16_16_EXPAND:
      return uint32_t(k_16_16_16_16_FLOAT);
    case k_24_8:
    case k_24_8_FLOAT:
      return uint32_t(k_8_8_8_8);
    default:
      return f;
  }
}

// RB_COPY_DEST_BASE / RB_COPY_DEST_PITCH (0x2319, 0x231A) in the shadow block 0x2300
// (kDevShadow2300), as the guest Resolve that just ran left them, against the decoded copy. The
// Resolve zeroes RB_COPY_CONTROL and RB_COPY_DEST_INFO there afterwards, so those are checked
// offline against the trace (the dump's copy_control/dest_info vs the trace's RESOLVE lines,
// compare_resolves.py --trace).
void CheckAgainstShadow(const uint8_t* base, uint32_t device, const ResolveParams& p) {
  const uint32_t b = device + kDevShadow2300 + 0x19 * 4;
  const uint32_t dest = Load32(base, b), pitch = Load32(base, b + 4);
  // The D3D moves the base by the tiled offset of the rectangle origin minus the dest point;
  // only the unshifted case is compared.
  const bool origin = p.x0 == p.dest_x && p.y0 == p.dest_y;
  // RB_COPY_DEST_PITCH's height is the level height minus the dest point's y (r18 in the D3D).
  if ((!origin || dest == p.dest_base) &&
      ((pitch >> 16) & 0x3FFF) == p.dest_height - uint32_t(p.dest_y))
    return;
  GRAW_LOG_FIRST(16, WARN,
                 "GRAW native resolve: decode vs D3D shadow: dest {:08X}/{:08X} pitch {:08X} "
                 "(decoded {}x{}, rect {},{}-{},{} point {},{})",
                 dest, p.dest_base, pitch, p.dest_width, p.dest_height, p.x0, p.y0, p.x1, p.y1,
                 p.dest_x, p.dest_y);
}

}  // namespace

bool IsFloatFormat(uint32_t f) {
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

int DestBppLog2(uint32_t f) {
  // The formats resolve.cpp's GetDestStorage and vulkan_resolve.cpp's ColorStorageFormat write.
  switch (xenos::TextureFormat(f)) {
    case xenos::TextureFormat::k_8:
      return 0;
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
    case xenos::TextureFormat::k_16_FLOAT:
      return 1;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
      return 2;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return 3;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return 4;
    default:
      return -1;
  }
}

bool DecodeResolve(const uint8_t* base, const HookArgs& args, ResolveParams* out) {
  ResolveParams p;
  const uint32_t device = args.device;
  const uint32_t flags = args.r[1];
  const uint32_t rect_ptr = args.r[2], tex = args.r[3], point_ptr = args.r[4];
  const uint32_t level = args.r[5], slice = args.r[6], color_ptr = args.r[7];
  if (level >= xenos::kTextureMaxMips) {
    GRAW_LOG_FIRST(4, WARN, "GRAW native resolve: destination level {} out of range", level);
    return false;
  }
  p.regs = SurfaceRegsFromDevice(base, device);
  p.source = flags & 7;
  if (p.source > 4) {
    GRAW_LOG_FIRST(4, WARN, "GRAW native resolve: source select {} unsupported", p.source);
    return false;
  }
  // Sample select: Flags bits 4-6 hold it plus one; 0 means "by the source's MSAA".
  uint32_t field = (flags >> 4) & 7;
  if (!field) {
    const uint32_t samples = SurfaceSamples(p.regs.surface_info);
    field = samples == 1 ? 1 : samples == 2 ? 5 : 7;
  }
  p.sample_select = field - 1;
  if (flags & 0x2000) {
    GRAW_LOG_FIRST(4, WARN, "GRAW native resolve: null copy (Flags {:08X}) unsupported", flags);
    return false;
  }
  p.copy_command = p.source == 4 ? 0 : 1;
  p.exp_bias = int32_t(flags) >> 26;  // bits 26-31, signed

  // The destination texture's fetch constant: D3DBaseTexture + 16 in GRAW's XDK (its resource
  // header is 12 bytes shorter than PGR4's, where it is at + 28; sub_824D1960 reads +16..+36),
  // 6 big-endian dwords.
  if (!tex) {
    GRAW_LOG_FIRST(4, WARN, "GRAW native resolve: no destination texture");
    return false;
  }
  uint32_t fetch[6];
  for (int i = 0; i < 6; ++i) fetch[i] = Load32(base, tex + kTextureFetchOffset + 4 * i);
  TextureKey key;
  if (!TextureKeyFromFetch(fetch, &key)) {
    GRAW_LOG_FIRST(4, WARN, "GRAW native resolve: invalid destination fetch constant {:08X} {:08X}",
                   fetch[0], fetch[1]);
    return false;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    GRAW_LOG_FIRST(4, WARN, "GRAW native resolve: 3D destination unsupported");
    return false;
  }
  const uint32_t fetch_format = fetch[1] & 0x3F;
  p.dest_format = D3DCopyDestFormat(fetch_format);
  p.dest_endian = (fetch[1] >> 6) & 3;
  p.dest_swap = ((fetch[3] >> 1) & 7) != 0;  // swizzle X is not R
  {
    const uint32_t sign_x = (fetch[0] >> 2) & 3;
    const bool integer = fetch[3] & 1;
    p.dest_number = IsFloatFormat(p.dest_format) ? 7
                    : integer                    ? (sign_x == 1 ? 3 : 2)
                                                 : (sign_x == 1 ? 1 : 0);
  }
  if (DestBppLog2(p.dest_format) < 0) {
    GRAW_LOG_FIRST(
        4, WARN,
        "GRAW native resolve: destination format {} (fetch format {}) unsupported, source {}, "
        "surface {:08X} {:08X} {:08X}",
        p.dest_format, fetch_format, p.source, p.regs.surface_info, p.regs.color_info[0],
        p.regs.depth_info);
    return false;
  }
  p.dest_width = std::max(1u, key.width >> level);
  p.dest_height = std::max(1u, key.height >> level);
  // Destination address of the level and slice. The texture object holds virtual addresses;
  // GpuAddress is also the physical address the GPU-side fetch constant (and so the texture
  // cache) sees.
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dimension, key.pitch, key.width, key.height, key.depth_or_array_size, key.tiled,
      rex::graphics::GetBaseFormat(xenos::TextureFormat(fetch_format)), key.packed_mips, true,
      std::max(level, key.mip_max_level));
  const texture_util::TextureGuestLayout::Level* lv;
  if (level == 0) {
    p.dest_base = GpuAddress(fetch[1] & 0xFFFFF000u);
    lv = &layout.base;
  } else {
    if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
      GRAW_LOG_FIRST(4, WARN,
                     "GRAW native resolve: packed mip tail destination (level {}) unsupported",
                     level);
      return false;
    }
    p.dest_base = GpuAddress(fetch[5] & 0xFFFFF000u) + layout.mip_offsets_bytes[level];
    lv = &layout.mips[level];
  }
  p.dest_base += lv->array_slice_stride_bytes * slice;
  p.dest_tiled = key.tiled;
  p.dest_pitch_blocks = lv->row_pitch_bytes >> DestBppLog2(p.dest_format);
  p.dest_extent_bytes = lv->array_slice_data_extent_bytes;

  // Rectangle: pSourceRect or the destination level's size; right/bottom rounded up to 8.
  if (rect_ptr) {
    p.x0 = int32_t(Load32(base, rect_ptr));
    p.y0 = int32_t(Load32(base, rect_ptr + 4));
    p.x1 = int32_t(Load32(base, rect_ptr + 8));
    p.y1 = int32_t(Load32(base, rect_ptr + 12));
  } else {
    p.x1 = int32_t(p.dest_width);
    p.y1 = int32_t(p.dest_height);
  }
  p.x1 = (p.x1 + 7) & ~7;
  p.y1 = (p.y1 + 7) & ~7;
  if (point_ptr) {
    p.dest_x = int32_t(Load32(base, point_ptr));
    p.dest_y = int32_t(Load32(base, point_ptr + 4));
  }
  // Clears: color from pClearColor (default: the D3D's float at 0x82000744 in all four
  // components, sub_824D1960), depth from ClearZ (f1), stencil from the stack argument (caller
  // r1 + 92; EndTiling sub_824D2CA0 stores it there).
  p.clear_flags = flags & 0x300;
  if (p.clear_flags) {
    if (color_ptr) {
      for (int i = 0; i < 4; ++i) p.clear_color[i] = LoadFloat(base, color_ptr + 4 * i);
    } else {
      const float c = LoadFloat(base, 0x82000744u);
      for (float& v : p.clear_color) v = c;
    }
    p.clear_depth = float(args.f[0]);
    p.clear_stencil = uint8_t(Load32(base, args.sp + 92));
    if (args.tiling == TilingCall::kEnd) {
      const uint32_t tiles = std::min(Load32(base, device + kDevTileCount), kMaxTiles);
      for (uint32_t i = 0; i < tiles; ++i) {
        const uint32_t r = device + kDevTileRects + 16 * i;
        if (p.x0 >= int32_t(Load32(base, r)) && p.y0 >= int32_t(Load32(base, r + 4)) &&
            p.x0 < int32_t(Load32(base, r + 8)) && p.y0 < int32_t(Load32(base, r + 12))) {
          p.edram_x = int32_t(Load32(base, device + kDevTileOrigins + kDevTileOriginStride * i));
          p.edram_y =
              int32_t(Load32(base, device + kDevTileOrigins + kDevTileOriginStride * i + 4));
          break;
        }
      }
    }
  }
  CheckAgainstShadow(base, device, p);
  *out = p;
  return true;
}

}  // namespace graw::native
