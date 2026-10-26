// GRAW 360 - render-to-texture probe for the black video panels (#2; docs/CROSSCOM.md, step 2).
//
// Each frame the campaign's video pass (sub_821D6838) calls sub_821BB8F0(node) per video panel:
//   push render target sub_8211E750 -> sub_8211E870 (D3D SetRenderTarget 0x824D1188)
//   video->vtable[1] = sub_823400A0 (the Bink draw, or a clear)
//   resolve sub_82194CC0 -> sub_82114588 (D3DDevice_Resolve 0x824D2CA0, or per rect 0x824D1960)
//   pop sub_8211E7E8
// and the HUD later samples the engine texture (video+68) through its D3DTexture. The probe shows
// which step the picture is lost in: the render target the draw goes to, the resolve's flags and
// destination, and whether any later SetTexture (0x824C6F88) binds that destination at all.
//
// Engine texture (52 bytes): +4 D3DTexture, +12/+14 width/height (u16), +33 cube flag, +40 a
// pointer to the render-target descriptor (rt+0 / rt+4 colour / depth wrappers whose +4 is the
// D3D surface, rt+8 mode byte, rt+172 viewport as four u16). D3DTexture+28 is its 24-byte
// GPUTEXTURE_FETCH_CONSTANT (decoded with xenos.h's xe_gpu_texture_fetch_t layout). All guest
// data is big-endian. Off unless --graw_debug_rtt; every hook's first check is that cvar.

#include <atomic>
#include <chrono>
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

#include "host_memory.h"

REXCVAR_DEFINE_BOOL(graw_debug_rtt, false, "GRAW",
                    "Log the campaign's video render-to-texture passes (#2): render target, "
                    "resolve, and the HUD's SetTexture of the result")
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
uint16_t Load16(uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Base() + a, 2);
  return rex::byte_swap(v);
}
uint8_t Load8(uint32_t a) { return Base()[a]; }
uint32_t Safe32(uint32_t a) { return Readable(a, 4) ? Load32(a) : 0; }

// The D3DTexture's fetch constant, decoded (xe_gpu_texture_fetch_t).
std::string FetchConstant(uint32_t d3d_texture) {
  const uint32_t fc = d3d_texture + 28;
  if (!Readable(fc, 24)) return "fetch constant unreadable";
  uint32_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = Load32(fc + 4 * i);
  return fmt::format(
      "fetch [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] type {} base {:08X} pitch {} tiled {} "
      "format {} endian {} size {}x{} exp_adjust {}",
      d[0], d[1], d[2], d[3], d[4], d[5], d[0] & 3, d[1] & 0xFFFFF000u, ((d[0] >> 22) & 0x1FF) * 32,
      d[0] >> 31, d[1] & 0x3F, (d[1] >> 6) & 3, (d[2] & 0x1FFF) + 1, ((d[2] >> 13) & 0x1FFF) + 1,
      static_cast<int32_t>(d[3] << 13) >> 26);
}

std::string Dwords(uint32_t a, int n) {
  if (!Readable(a, 4 * n)) return "unreadable";
  std::string s;
  for (int i = 0; i < n; ++i) s += fmt::format("{}{:08X}", i ? " " : "", Load32(a + 4 * i));
  return s;
}

struct PerTexture {
  uint64_t entries = 0, passes = 0, set_rt = 0, resolves = 0, resolve_rects = 0, ends = 0;
  uint64_t hud_binds = 0;
  uint32_t last_flags = 0, hud_sampler_mask = 0;
  int detailed = 0;  // full dumps so far (the first few passes of each texture)
  int hud_logged = 0;
};

std::mutex g_mutex;
std::map<uint32_t, PerTexture> g_textures;  // keyed by the engine texture (video+68)
std::map<uint32_t, uint32_t> g_d3d_to_tex;  // D3DTexture -> engine texture, for SetTexture
uint64_t g_entries = 0, g_gated = 0;        // sub_821BB8F0 calls; those that skipped the pass
auto g_window = std::chrono::steady_clock::now();
constexpr int kDetailed = 3;

// The pass in progress on this thread (the video pass runs on the game's render thread).
thread_local uint32_t t_tex = 0;
thread_local bool t_detail = false;  // this pass gets the full dump

// The D3DTextures to watch in SetTexture; read without the lock on the hot path.
constexpr int kWatchMax = 16;
std::atomic<uint32_t> g_watch[kWatchMax];
std::atomic<int> g_watch_count{0};

void Watch(uint32_t d3d) {
  const int n = g_watch_count.load();
  for (int i = 0; i < n; ++i) {
    if (g_watch[i].load() == d3d) return;
  }
  if (n < kWatchMax) {
    g_watch[n].store(d3d);
    g_watch_count.store(n + 1);
  }
}

void MaybeSummarise() {
  auto now = std::chrono::steady_clock::now();
  if (now - g_window < std::chrono::seconds(2)) return;
  g_window = now;
  REXLOG_INFO("GRAW rtt last 2 s: {} video passes entered, {} skipped by the +77 gate", g_entries,
              g_gated);
  g_entries = g_gated = 0;
  for (auto& [tex, t] : g_textures) {
    if (!t.passes && !t.hud_binds) continue;
    REXLOG_INFO(
        "GRAW rtt {:08X} last 2 s: {} passes, {} SetRenderTarget, {} resolves, {} per-rect "
        "resolves, {} ends; resolve flags {:08X}; HUD SetTexture {} (samplers mask {:04X})",
        tex, t.passes, t.set_rt, t.resolves, t.resolve_rects, t.ends, t.last_flags, t.hud_binds,
        t.hud_sampler_mask);
    const int detailed = t.detailed, hud_logged = t.hud_logged;
    t = PerTexture{};
    t.detailed = detailed;
    t.hud_logged = hud_logged;
  }
}

}  // namespace

// 0x821BB8F0, entry of the per-video pass (r3 = node).
void GrawRttEnter(PPCRegister& r3) {
  (void)r3;
  if (!REXCVAR_GET(graw_debug_rtt)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_entries;
  ++g_gated;  // undone at GrawRttBegin when the pass goes ahead
  MaybeSummarise();
}

// 0x821BB954, the gate passed and r31 = tex loaded, before the render target is pushed
// (r29 = node).
void GrawRttBegin(PPCRegister& r29, PPCRegister& r31) {
  if (!REXCVAR_GET(graw_debug_rtt)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_gated) --g_gated;
  const uint32_t tex = r31.u32;
  t_tex = tex;
  t_detail = false;
  auto& t = g_textures[tex];
  ++t.passes;
  if (!Readable(tex, 52)) return;
  const uint32_t d3d = Load32(tex + 4);
  if (d3d) {
    g_d3d_to_tex[d3d] = tex;
    Watch(d3d);
  }
  if (t.detailed >= kDetailed) return;
  ++t.detailed;
  t_detail = true;
  const uint32_t video = Safe32(r29.u32 + 12);
  const uint32_t rt = Load32(tex + 40);
  uint32_t color = 0, depth = 0, color_d3d = 0, depth_d3d = 0, mode = 0;
  uint16_t vp[4] = {};
  if (Readable(rt, 180)) {
    color = Load32(rt + 0);
    depth = Load32(rt + 4);
    mode = Load8(rt + 8);
    for (int i = 0; i < 4; ++i) vp[i] = Load16(rt + 172 + 2 * i);
    color_d3d = color ? Safe32(color + 4) : 0;
    depth_d3d = depth ? Safe32(depth + 4) : 0;
  }
  REXLOG_INFO(
      "GRAW rtt {:08X} pass {}: node {:08X} video {:08X}, engine texture {}x{} cube {} d3d "
      "{:08X}; rt {:08X} mode {} colour {:08X} (d3d surface {:08X}) depth {:08X} (d3d {:08X}) "
      "viewport {},{} {}x{}",
      tex, t.detailed, r29.u32, video, Load16(tex + 12), Load16(tex + 14), Load8(tex + 33), d3d,
      rt, mode, color, color_d3d, depth, depth_d3d, vp[0], vp[1], vp[2], vp[3]);
  if (d3d) REXLOG_INFO("GRAW rtt {:08X}   destination texture {}", tex, FetchConstant(d3d));
  if (color_d3d)
    REXLOG_INFO("GRAW rtt {:08X}   colour surface dwords {}", tex, Dwords(color_d3d, 16));
}

// 0x8211E8A8, sub_8211E870's bl SetRenderTarget (r3 = device, r5 = D3D colour surface,
// r31 = the descriptor). Also runs for the pop and for every other render target the engine
// binds; only the ones inside a video pass are logged.
void GrawRttSetRenderTarget(PPCRegister& r3, PPCRegister& r5, PPCRegister& r31) {
  (void)r3;
  if (!REXCVAR_GET(graw_debug_rtt) || !t_tex) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& t = g_textures[t_tex];
  ++t.set_rt;
  if (!t_detail) return;
  REXLOG_INFO("GRAW rtt {:08X}   SetRenderTarget surface {:08X} from descriptor {:08X}", t_tex,
              r5.u32, r31.u32);
}

// 0x821146DC, sub_82114588's bl D3DDevice_Resolve (r4 = flags, r6 = destination D3DTexture).
void GrawRttResolve(PPCRegister& r4, PPCRegister& r6) {
  if (!REXCVAR_GET(graw_debug_rtt) || !t_tex) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& t = g_textures[t_tex];
  ++t.resolves;
  t.last_flags = r4.u32;
  if (!t_detail) return;
  const auto expected = Readable(t_tex + 4, 4) ? Load32(t_tex + 4) : 0;
  REXLOG_INFO("GRAW rtt {:08X}   Resolve flags {:08X} to {:08X} ({}): {}", t_tex, r4.u32, r6.u32,
              r6.u32 == expected ? "the engine texture's own" : "NOT the engine texture's",
              FetchConstant(r6.u32));
}

// 0x821146A8, the per-rect resolve (sub_824D1960), for render targets in mode 1.
void GrawRttResolveRect(PPCRegister& r3) {
  (void)r3;
  if (!REXCVAR_GET(graw_debug_rtt) || !t_tex) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_textures[t_tex].resolve_rects;
}

// 0x821BB9B4, after the resolve, before the pop (r31 = tex).
void GrawRttEnd(PPCRegister& r31) {
  if (!REXCVAR_GET(graw_debug_rtt)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_textures[r31.u32].ends;
  t_tex = 0;
  t_detail = false;
}

// 0x824C6F88, D3D SetTexture(device, sampler, texture): counts binds of a video's resolved
// texture outside its own pass (the HUD's draw, or anything else that samples it).
void GrawRttSetTexture(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  (void)r3;
  if (!REXCVAR_GET(graw_debug_rtt) || !r5.u32 || t_tex) return;
  const int n = g_watch_count.load();
  bool hit = false;
  for (int i = 0; i < n && !hit; ++i) hit = g_watch[i].load() == r5.u32;
  if (!hit) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_d3d_to_tex.find(r5.u32);
  if (it == g_d3d_to_tex.end()) return;
  auto& t = g_textures[it->second];
  if (t.hud_logged < 2) {
    ++t.hud_logged;
    REXLOG_INFO("GRAW rtt {:08X}   first SetTexture of the result: sampler {}, {}", it->second,
                r4.u32, FetchConstant(r5.u32));
  }
  ++t.hud_binds;
  t.hud_sampler_mask |= 1u << (r4.u32 & 15);
}
