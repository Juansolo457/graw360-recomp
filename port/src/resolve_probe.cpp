// GRAW 360 - D3D Resolve / Clear probe for the HUD border noise (#1, docs/CROSSCOM.md).
//
// In the tutorial the margin around the 3D view shows coloured noise; no draw touches those
// pixels, so the front buffer there holds whatever was in memory. Canary shows it black. These
// hooks (port-yeti manifest, one instruction into D3DDevice_Resolve 0x824D2CA0 and
// D3DDevice_Clear 0x824CDCC8, after `mflr r12`, so r12 is the caller) log each distinct call:
// flags, source rectangle, destination texture (base address, size, format), destination point
// and clear colour. Comparing the tutorial with Mission 1 shows which write the margin misses.
// Off unless --graw_debug_resolve.

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

REXCVAR_DEFINE_BOOL(graw_debug_resolve, false, "GRAW",
                    "Log the game's distinct D3D Resolve and Clear calls with their callers (#1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

uint8_t* Base() { return rex::system::kernel_memory()->virtual_membase(); }

bool Readable(uint32_t a, size_t n) {
  if (a < 0x10000 || a >= 0xC0000000u) return false;
  const uint8_t* p = Base() + a;
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
      (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || info.Protect == 0) {
    return false;
  }
  return p + n <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
}

uint32_t Load32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Base() + a, 4);
  return rex::byte_swap(v);
}

float LoadF(uint32_t a) {
  uint32_t v = Load32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::string Rect(uint32_t a) {
  if (!a) return "full";
  if (!Readable(a, 16)) return fmt::format("{:08X}?", a);
  return fmt::format("({},{})-({},{})", int32_t(Load32(a)), int32_t(Load32(a + 4)),
                     int32_t(Load32(a + 8)), int32_t(Load32(a + 12)));
}

// The D3DTexture's fetch constant (at +28): base address, size, format.
std::string Texture(uint32_t t) {
  if (!t) return "none";
  if (!Readable(t + 28, 24)) return fmt::format("{:08X}?", t);
  const uint32_t d1 = Load32(t + 32), d2 = Load32(t + 36);
  return fmt::format("{:08X} base {:08X} {}x{} fmt {}", t, d1 & 0xFFFFF000u, (d2 & 0x1FFF) + 1,
                     ((d2 >> 13) & 0x1FFF) + 1, d1 & 0x3F);
}

std::mutex g_mutex;
std::map<std::string, uint64_t> g_seen;

void Note(const std::string& key, const std::string& line) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_seen.size() >= 2000 || g_seen[key]++) return;
  REXLOG_INFO("{}", line);
}

}  // namespace

// D3DDevice_Resolve(device, flags, source rect, dest texture, dest point, level, slice,
// clear colour, clear z (f1), ...)
void GrawResolveCall(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7,
                     PPCRegister& r10, PPCRegister& r12) {
  if (!REXCVAR_GET(graw_debug_resolve)) return;
  const std::string point =
      r7.u32 && Readable(r7.u32, 8)
          ? fmt::format("({},{})", int32_t(Load32(r7.u32)), int32_t(Load32(r7.u32 + 4)))
          : "-";
  const std::string clear = r10.u32 && Readable(r10.u32, 16)
                                ? fmt::format("({:.3f} {:.3f} {:.3f} {:.3f})", LoadF(r10.u32),
                                              LoadF(r10.u32 + 4), LoadF(r10.u32 + 8),
                                              LoadF(r10.u32 + 12))
                                : "-";
  const std::string rect = Rect(r5.u32), tex = Texture(r6.u32);
  Note(fmt::format("R {:08X} {:X} {} {} {}", r12.u32, r4.u32, rect, tex, point),
       fmt::format("GRAW resolve from {:08X}: flags {:08X} rect {} dest {} at {} clear {}",
                   r12.u32, r4.u32, rect, tex, point, clear));
}

// D3DDevice_Clear(device, count, rects, flags, colour, z (f1), stencil, ...)
void GrawClearCall(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7,
                   PPCRegister& r12) {
  if (!REXCVAR_GET(graw_debug_resolve)) return;
  const std::string rect = r4.u32 ? Rect(r5.u32) : "full";
  Note(fmt::format("C {:08X} {} {} {:X} {:X}", r12.u32, r4.u32, rect, r6.u32, r7.u32),
       fmt::format("GRAW clear from {:08X}: count {} first rect {} flags {:X} colour {:08X}",
                   r12.u32, r4.u32, rect, r6.u32, r7.u32));
}

// sub_82140938 (#1): computes a view's viewport rectangle from its fractions (+388 x, +392 y,
// +396 width, +400 height) times its render-target size (+384, +386) plus its offset (+380,
// +382), and stores it in the camera (+600..606). Logs each distinct input once with the caller.
void GrawViewRect(PPCRegister& r3, PPCRegister& r12) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r3.u32 + 380, 24)) return;
  const uint32_t v = r3.u32;
  auto h = [&](uint32_t o) { return Load32(v + o) >> 16; };  // the big-endian halfword at v+o
  const std::string line = fmt::format(
      "GRAW viewrect from {:08X}: view {:08X} offset ({},{}) size {}x{} fractions x {} y {} w {} h {}",
      r12.u32, v, h(380), h(382), h(384), h(386), LoadF(v + 388), LoadF(v + 392),
      LoadF(v + 396), LoadF(v + 400));
  Note(line, line);
}
