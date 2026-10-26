// GRAW 360 - D3D Resolve / Clear probe for the HUD border noise (#1, docs/CROSSCOM.md).
//
// In the tutorial the margin around the 3D view shows coloured noise; no draw touches those
// pixels, so the front buffer there holds whatever was in memory. Canary shows it black. These
// hooks (port-yeti manifest, one instruction into D3DDevice_Resolve 0x824D2CA0 and
// D3DDevice_Clear 0x824CDCC8, after `mflr r12`, so r12 is the caller) log each distinct call:
// flags, source rectangle, destination texture (base address, size, format), destination point
// and clear colour. Comparing the tutorial with Mission 1 shows which write the margin misses.
// Off unless --graw_debug_resolve.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "host_memory.h"

REXCVAR_DEFINE_BOOL(graw_debug_resolve, false, "GRAW",
                    "Log the game's distinct D3D Resolve and Clear calls with their callers (#1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(graw_dump_physical, "", "GRAW",
                      "Once, after --graw_dump_after seconds, write these guest physical ranges to "
                      "files: 'address:size,...' in hex, e.g. '1DB52000:E1000' (#1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(graw_dump_after, 60, "GRAW", "Seconds before --graw_dump_physical runs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(graw_debug_zero_physical, "", "GRAW",
                      "Experiment: keep this guest physical range zeroed, 'address:size' in hex (#1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

uint8_t* Base() { return rex::system::kernel_memory()->virtual_membase(); }

bool Readable(uint32_t a, size_t n) {
  if (a < 0x10000 || a >= 0xC0000000u) return false;
  return graw::HostReadable(Base() + a, n);
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

// --graw_dump_physical: guest memory as the CPU sees it (resolved data stays in the GPU's copy
// unless read back, so a range nothing on the CPU writes shows what the textures start from).
void DumpPhysicalOnce() {
  static const auto start = std::chrono::steady_clock::now();
  static bool done = false;
  const std::string spec = REXCVAR_GET(graw_dump_physical);
  if (done || spec.empty() ||
      std::chrono::steady_clock::now() - start < std::chrono::seconds(REXCVAR_GET(graw_dump_after))) {
    return;
  }
  done = true;
  std::stringstream ranges(spec);
  std::string range;
  while (std::getline(ranges, range, ',')) {
    const auto colon = range.find(':');
    if (colon == std::string::npos) continue;
    const uint32_t address = std::stoul(range.substr(0, colon), nullptr, 16);
    const uint32_t size = std::stoul(range.substr(colon + 1), nullptr, 16);
    const std::string path = fmt::format("graw-phys-{:08X}.bin", address);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(rex::system::kernel_memory()->TranslatePhysical<uint8_t*>(address), 1, size, f);
      std::fclose(f);
      REXLOG_INFO("GRAW dumped physical {:08X}+{:X} to {}", address, size, path);
    }
  }
}

// D3DDevice_Clear(device, count, rects, flags, colour, z (f1), stencil, ...)
void GrawClearCall(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, PPCRegister& r7,
                   PPCRegister& r12) {
  DumpPhysicalOnce();
  if (const std::string z = REXCVAR_GET(graw_debug_zero_physical); !z.empty()) {
    const auto colon = z.find(':');
    const uint32_t address = std::stoul(z.substr(0, colon), nullptr, 16);
    const uint32_t size = std::stoul(z.substr(colon + 1), nullptr, 16);
    std::memset(rex::system::kernel_memory()->TranslatePhysical<uint8_t*>(address), 0, size);
  }
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

// Before the game's VdSwap call (0x824CCB48, r4 = the front buffer's fetch constant): logs each
// distinct front buffer (base, size, format), to compare with the resolves that write it (#1).
void GrawSwapFetch(PPCRegister& r4) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r4.u32, 24)) return;
  uint32_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = Load32(r4.u32 + 4 * i);
  const std::string line = fmt::format(
      "GRAW swap front buffer base {:08X} {}x{} fmt {} fetch [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
      d[1] & 0xFFFFF000u, (d[2] & 0x1FFF) + 1, ((d[2] >> 13) & 0x1FFF) + 1, d[1] & 0x3F, d[0], d[1],
      d[2], d[3], d[4], d[5]);
  Note(line, line);
}

// D3D SetViewport(device, D3DVIEWPORT9*) at 0x824D0488, hooked after mflr r12: each distinct
// viewport (X, Y, width, height) with its caller (#1: is the tutorial's composition inset?).
void GrawSetViewport(PPCRegister& r3, PPCRegister& r4, PPCRegister& r12) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r4.u32, 24) || !Readable(r3.u32 + 13480, 8)) {
    return;
  }
  // The clamp: device+12536 is the colour surface (its +24 packs width-1 | height-1 << 13); when
  // device+10432 bit 4 and the surfaces match the saved set, device+13480/13484 is used instead.
  const uint32_t d = r3.u32, surface = Load32(d + 12536);
  const uint32_t packed = surface && Readable(surface + 24, 4) ? Load32(surface + 24) : 0;
  const std::string line = fmt::format(
      "GRAW viewport from {:08X}: ({},{}) {}x{} z {}..{} | surface {:08X} {}x{} alt {}x{} flag {:02X}",
      r12.u32, int32_t(Load32(r4.u32)), int32_t(Load32(r4.u32 + 4)), Load32(r4.u32 + 8),
      Load32(r4.u32 + 12), LoadF(r4.u32 + 16), LoadF(r4.u32 + 20), surface, (packed & 0x1FFF) + 1,
      ((packed >> 13) & 0x1FFF) + 1, Load32(d + 13480), Load32(d + 13484),
      Base()[d + 10432]);
  Note(line, line);
}

// D3D SetScissorRect(device, RECT*) at 0x824D03C0, hooked after mflr r12: each distinct rectangle
// with its caller and the device's scissor-test flag (device+11952) (#1).
void GrawSetScissor(PPCRegister& r3, PPCRegister& r4, PPCRegister& r12) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r4.u32, 16) || !Readable(r3.u32 + 11952, 4)) {
    return;
  }
  const std::string line = fmt::format(
      "GRAW scissor from {:08X}: ({},{})-({},{}) test {}", r12.u32, int32_t(Load32(r4.u32)),
      int32_t(Load32(r4.u32 + 4)), int32_t(Load32(r4.u32 + 8)), int32_t(Load32(r4.u32 + 12)),
      Load32(r3.u32 + 11952));
  Note(line, line);
}

// sub_821851D0 (the view effect, r3 = the view) entry (#1): logs the view's two 4x4 matrices
// (+12 and +140, uploaded as vertex shader constants for its camera-facing quad) and +588.
void GrawViewMatrices(PPCRegister& r3) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r3.u32 + 12, 600)) return;
  std::string m;
  for (uint32_t base : {12u, 140u}) {
    m += fmt::format(" +{}:", base);
    for (int i = 0; i < 16; ++i) m += fmt::format(" {:.4g}", LoadF(r3.u32 + base + 4 * i));
  }
  const std::string line = fmt::format("GRAW viewfx view {:08X} aspect {:.4g}{}", r3.u32,
                                       LoadF(r3.u32 + 588), m);
  static int logged = 0;
  if (logged++ < 6) REXLOG_INFO("{}", line);
}

// D3D SetVertexShaderConstantF(device, start, data, count) at 0x824C8788 (#1): each distinct
// upload (caller from the back chain, start register, first 8 floats) - to find the constants
// that map the post-process quads to the screen.
void GrawVsConstants(PPCRegister& r1, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r5.u32, 32) || !Readable(r1.u32, 4)) return;
  const uint32_t back = Load32(r1.u32);
  const uint32_t caller = Readable(back - 8, 4) ? Load32(back - 8) : 0;
  std::string v;
  for (uint32_t i = 0; i < std::min<uint32_t>(r6.u32 * 4, 8); ++i) v += fmt::format(" {:.5g}", LoadF(r5.u32 + 4 * i));
  const std::string line =
      fmt::format("GRAW vsconst from {:08X}: c{} x{}:{}", caller, r4.u32, r6.u32, v);
  Note(line, line);
}

// BinkDoFrame (0x825B8CE0), after mflr r12 (#1): every video the game decodes - handle, size,
// frame number and count (Bink 1.x header: +0 width, +4 height, +8 frames, +12 frame), caller.
void GrawBinkDoFrame(PPCRegister& r3, PPCRegister& r12) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r3.u32, 16)) return;
  const uint32_t b = r3.u32;
  const std::string key = fmt::format("{:08X} {:08X} {}x{}", r12.u32, b, Load32(b), Load32(b + 4));
  const std::string line =
      fmt::format("GRAW binkdoframe from {:08X}: bink {:08X} {}x{} frame {}/{}", r12.u32, b,
                  Load32(b), Load32(b + 4), Load32(b + 12), Load32(b + 8));
  Note(key, line);
}

// sub_82119858 at 0x8211AE10, just before the edge grid's draw (sub_821154B0) (#1): the vertex
// shader constants it wrote inline into the device shadow (device+2048..2092, +6032..6044).
void GrawGridConstants(PPCRegister& r31) {
  if (!REXCVAR_GET(graw_debug_resolve) || !Readable(r31.u32 + 4, 4)) return;
  const uint32_t device = Load32(r31.u32 + 4);
  if (!Readable(device + 2048, 48) || !Readable(device + 6032, 16)) return;
  std::string c;
  for (uint32_t o : {2048u, 2064u, 2080u, 6032u}) {
    c += fmt::format(" +{}:", o);
    for (int i = 0; i < 4; ++i) c += fmt::format(" {:.5g}", LoadF(device + o + 4 * i));
  }
  const std::string line = "GRAW grid constants" + c;
  Note(line, line);
  // The grid's vertex buffer (global 0x828F12C4; header fetch constant at +24: address | 3,
  // size in words): min/max of the 16-bit positions (each vertex: x, y, u, v as int16).
  const uint32_t vb = Load32(0x828F12C4);
  if (!vb || !Readable(vb, 32)) return;
  {
    std::string raw;
    for (int i = 0; i < 16; ++i) raw += fmt::format(" {:08X}", Load32(vb + 4 * i));
    const std::string rline = fmt::format("GRAW grid vb object {:08X}:{}", vb, raw);
    Note(rline, rline);
  }
  // Engine wrapper: +4 = vertex data (virtual), +8 = size in bytes; 8 bytes per vertex:
  // int16 x, y (screen pixels), then two more int16 (texture coordinates).
  const uint32_t header = Load32(vb + 4);
  if (!Readable(header, 32)) return;
  const uint32_t data_va = Load32(header + 28), bytes = Load32(vb + 8);
  if (!bytes || bytes > 0x40000 || !Readable(data_va, bytes)) return;
  int minx = 99999, maxx = -99999, miny = 99999, maxy = -99999;
  std::string first;
  for (uint32_t o = 0; o + 8 <= bytes; o += 8) {
    const uint32_t v = Load32(data_va + o);
    const int x = int16_t(v >> 16), y = int16_t(v & 0xFFFF);
    minx = std::min(minx, x), maxx = std::max(maxx, x), miny = std::min(miny, y), maxy = std::max(maxy, y);
    if (o < 64) first += fmt::format(" ({},{})", x, y);
  }
  const std::string vline = fmt::format("GRAW grid vb data {:08X} bytes {} x {}..{} y {}..{} first{}",
                                        data_va, bytes, minx, maxx, miny, maxy, first);
  Note(vline, vline);
}

// sub_821820C8 (#3, tactical map): the camera object's matrices once they're built: view (+12),
// sub_8212AE48's result (+76), projection (+140), product (+204) and the transposed constant
// copy (+268). Logged on each distinct content.
REXCVAR_DEFINE_BOOL(graw_debug_map, false, "GRAW",
                    "Log the tactical map's camera matrices and panel rects (#3)");

void GrawMapMatrices(PPCRegister& r31) {
  if (!REXCVAR_GET(graw_debug_map) || !Readable(r31.u32 + 8, 324)) return;
  if (Load32(r31.u32 + 8) != 1) return;  // only the orthographic (2D panel) camera
  std::string line = fmt::format("GRAW map matrices {:08X}", r31.u32);
  for (uint32_t o : {12u, 76u, 140u, 204u, 268u}) {
    line += fmt::format(" | +{}:", o);
    for (int i = 0; i < 16; ++i) line += fmt::format(" {:.5g}", LoadF(r31.u32 + o + 4 * i));
  }
  Note(line, line);
}

// sub_821823A0 (#3): a map panel entry (24 bytes: x, z, width, height, flags byte, pointer)
// before the draw scales x and width by the aspect at +588 when the flags byte is set.
void GrawMapRect(PPCRegister& r25, PPCRegister& r27) {
  if (!REXCVAR_GET(graw_debug_map) || !Readable(r25.u32, 24) || !Readable(r27.u32 + 588, 4)) {
    return;
  }
  const uint32_t e = r25.u32;
  const std::string line = fmt::format(
      "GRAW map rect {:08X}: x {:.5g} z {:.5g} w {:.5g} h {:.5g} flags {:08X} ptr {:08X} aspect {:.5g}",
      e, LoadF(e), LoadF(e + 4), LoadF(e + 8), LoadF(e + 12), Load32(e + 16), Load32(e + 20),
      LoadF(r27.u32 + 588));
  Note(line, line);
}

// sub_821F3D98 (#3): the message that adds a map panel. Its x, z, width, height are half floats
// at +12, +14, +16, +18; logs the raw message words and the caller chain.
void GrawMapPanelMessage(PPCRegister& r1, PPCRegister& r4, PPCRegister& r5) {
  if (!REXCVAR_GET(graw_debug_map) || !Readable(r4.u32, 24)) return;
  std::string words;
  for (uint32_t o = 0; o < 24; o += 4) words += fmt::format(" {:08X}", Load32(r4.u32 + o));
  uint32_t chain[4] = {};
  uint32_t sp = r1.u32;
  for (int i = 0; i < 4 && Readable(sp, 4); ++i) {
    sp = Load32(sp);  // back chain
    if (!sp || !Readable(sp - 8, 4)) break;
    chain[i] = Load32(sp - 8);  // saved LR (mflr r12; stw r12,-8(r1))
  }
  const std::string line =
      fmt::format("GRAW map panel message {:08X} r5 {:X}:{} <- {:08X} {:08X} {:08X} {:08X}",
                  r4.u32, r5.u32, words, chain[0], chain[1], chain[2], chain[3]);
  Note(line, line);
}
