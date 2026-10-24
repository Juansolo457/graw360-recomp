// GRAW 360 - D3D blend render-state probe for the black HUD video panels (#2, docs/CROSSCOM.md).
//
// The renderer receives RB_BLENDCONTROL0 = 0x07060706 (SrcAlpha/InvSrcAlpha) for HUD overlay draws
// that Canary draws with One/InvSrcColor, Zero/InvSrcColor and One/One. The game's D3D runtime sets
// blend fields through small setters reached from a render-state table at 0x825F3930
// ({default, getter, setter} entries); each stores the field into the device's blend shadow
// (device+11448) and, when blending is enabled (device+11452 bit 31), the per-target copies that
// are flushed to the GPU. These hooks (port-yeti manifest, at each setter's entry) log the value
// passed and the engine code that led to the call, so the engine's blend-mode mapping can be found.
//
// The setters are leaf functions, and hooks can't read LR, so the caller is taken from the stack
// back chain: Load32(Load32(r1) - 8) is the return address saved by the function that called the
// setter's caller (the D3D dispatcher or the engine function). Off unless --graw_debug_blend.
//
// Per-draw part: SetTexture and the five D3D draw functions are hooked one instruction in, after
// `mflr r12`, so r12 is the exact caller. Binding a 160x120 texture (the resolved Cross-Com /
// narration video) arms the next 16 draws; each logs its caller, an 8-frame back chain, the blend
// shadow and per-target value, the textures on samplers 0-1, and who last set SrcBlend, DestBlend
// and AlphaBlendEnable. That ties the wrong overlay blend to the engine code that chose it.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

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

REXCVAR_DEFINE_BOOL(graw_debug_blend, false, "GRAW",
                    "Log the game's D3D blend render-state setter calls with their callers (#2)")
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

// Return address of the setter's caller's caller, and one frame further up.
std::pair<uint32_t, uint32_t> Callers(uint32_t sp) {
  uint32_t c1 = 0, c2 = 0;
  if (Readable(sp, 4)) {
    uint32_t back = Load32(sp);
    if (Readable(back - 8, 8)) {
      c1 = Load32(back - 8);
      if (Readable(back, 4)) {
        uint32_t back2 = Load32(back);
        if (Readable(back2 - 8, 8)) c2 = Load32(back2 - 8);
      }
    }
  }
  return {c1, c2};
}

std::string Chain(uint32_t sp, int depth) {
  std::string s;
  for (int i = 0; i < depth && Readable(sp, 4); ++i) {
    const uint32_t back = Load32(sp);
    if (back <= sp || !Readable(back - 8, 8)) break;
    s += fmt::format(" {:08X}", Load32(back - 8));
    sp = back;
  }
  return s;
}

std::string TexInfo(uint32_t tex) {
  if (!tex || !Readable(tex + 28, 24)) return fmt::format("{:08X}", tex);
  const uint32_t d1 = Load32(tex + 32), d2 = Load32(tex + 36);
  return fmt::format("{:08X} {}x{} fmt {}", tex, (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1,
                     d1 & 0x3F);
}

std::mutex g_mutex;
std::map<std::string, std::string> g_last_set;  // field -> "value from chain"
uint32_t g_textures[4] = {};
int g_armed = 0;
int g_arms = 0;
uint64_t g_draws = 0;
std::map<std::tuple<const char*, uint32_t, uint32_t, uint32_t>, uint64_t> g_counts;
auto g_window = std::chrono::steady_clock::now();
int g_windows = 0;

void Note(const char* field, uint32_t device, uint32_t value, uint32_t sp) {
  if (!REXCVAR_GET(graw_debug_blend)) return;
  auto [c1, c2] = Callers(sp);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_set[field] = fmt::format("{} from{}", value, Chain(sp, 6));
  auto key = std::make_tuple(field, value, c1, c2);
  if (g_counts[key]++ == 0) {
    uint32_t shadow = Readable(device + 11448, 8) ? Load32(device + 11448) : 0;
    uint32_t flags = Readable(device + 11452, 4) ? Load32(device + 11452) : 0;
    REXLOG_INFO("GRAW d3dblend new: {} = {} (shadow {:08X}, flags {:08X}) from {:08X} <- {:08X}",
                field, value, shadow, flags, c1, c2);
  }
  auto now = std::chrono::steady_clock::now();
  if (now - g_window >= std::chrono::seconds(5) && g_windows < 6) {
    g_window = now;
    ++g_windows;
    std::string line;
    for (const auto& [k, n] : g_counts) {
      line += fmt::format(" | {}={} from {:08X}<-{:08X} x{}", std::get<0>(k), std::get<1>(k),
                          std::get<2>(k), std::get<3>(k), n);
    }
    REXLOG_INFO("GRAW d3dblend last 5 s:{}", line);
    g_counts.clear();
  }
}

void SetTexture(uint32_t sampler, uint32_t tex) {
  if (!REXCVAR_GET(graw_debug_blend)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (sampler < 4) g_textures[sampler] = tex;
  if (!tex || !Readable(tex + 36, 4)) return;
  const uint32_t d2 = Load32(tex + 36);
  if ((d2 & 0x1FFF) + 1 == 160 && ((d2 >> 13) & 0x1FFF) + 1 == 120 && g_arms < 8) {
    ++g_arms;
    g_armed = 16;
    REXLOG_INFO("GRAW d3ddraw arm {}: sampler {} = {}", g_arms, sampler, TexInfo(tex));
  }
}

void Draw(const char* name, uint32_t device, uint32_t a4, uint32_t a5, uint32_t caller,
          uint32_t sp) {
  if (!REXCVAR_GET(graw_debug_blend)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_draws;
  if (g_armed <= 0) {
    // Not armed: catalogue each distinct draw site once (caller chain, sampler-0 texture size,
    // blend shadow), so every HUD overlay shows up even when no video plays.
    static std::map<std::string, int> sites;
    if (sites.size() >= 3000) return;
    const uint32_t tex = g_textures[0];
    const uint32_t size = tex && Readable(tex + 36, 4) ? Load32(tex + 36) & 0x3FFFFFF : 0;
    const uint32_t shadow = Readable(device + 11448, 4) ? Load32(device + 11448) : 0;
    const uint32_t flags = Readable(device + 11452, 4) ? Load32(device + 11452) : 0;
    const std::string chain = Chain(sp, 8);
    const std::string key = fmt::format("{} {:08X}{} {:X} {:X} {:X}", name, caller, chain, size,
                                        shadow, flags >> 30);
    if (sites[key]++) return;
    REXLOG_INFO("GRAW d3ddraw site #{} {}({:X},{:X}) from {:08X} <-{} | shadow {:08X} flags {:08X} "
                "rt0 {:08X} | s0 {} | s1 {} | last Src {} | Dest {}",
                g_draws, name, a4, a5, caller, chain, shadow, flags,
                Readable(device + 11640, 4) ? Load32(device + 11640) : 0, TexInfo(tex),
                TexInfo(g_textures[1]), g_last_set.count("SrcBlend") ? g_last_set["SrcBlend"] : "-",
                g_last_set.count("DestBlend") ? g_last_set["DestBlend"] : "-");
    return;
  }
  --g_armed;
  auto last = [](const char* f) {
    auto it = g_last_set.find(f);
    return it == g_last_set.end() ? std::string("-") : it->second;
  };
  REXLOG_INFO(
      "GRAW d3ddraw #{} {}({:X},{:X}) from {:08X} <-{} | shadow {:08X} flags {:08X} rt0 {:08X} | "
      "s0 {} | s1 {} | last Src {} | Dest {} | Enable {}",
      g_draws, name, a4, a5, caller, Chain(sp, 8),
      Readable(device + 11448, 4) ? Load32(device + 11448) : 0,
      Readable(device + 11452, 4) ? Load32(device + 11452) : 0,
      Readable(device + 11640, 4) ? Load32(device + 11640) : 0, TexInfo(g_textures[0]),
      TexInfo(g_textures[1]), last("SrcBlend"), last("DestBlend"), last("AlphaBlendEnable"));
}

}  // namespace

void GrawDrawSetTexture(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) { (void)r3; SetTexture(r4.u32, r5.u32); }
void GrawDraw4578(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r12) { Draw("draw4578", r3.u32, r4.u32, r5.u32, r12.u32, r1.u32); }
void GrawDraw4838(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r12) { Draw("drawUP4838", r3.u32, r4.u32, r5.u32, r12.u32, r1.u32); }
void GrawDraw48C0(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r12) { Draw("draw48C0", r3.u32, r4.u32, r5.u32, r12.u32, r1.u32); }
void GrawDraw4BF8(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r12) { Draw("draw4BF8", r3.u32, r4.u32, r5.u32, r12.u32, r1.u32); }
void GrawDraw4E38(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r12) { Draw("draw4E38", r3.u32, r4.u32, r5.u32, r12.u32, r1.u32); }

void GrawBlendSrc(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("SrcBlend", r3.u32, r4.u32, r1.u32); }
void GrawBlendOp(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("BlendOp", r3.u32, r4.u32, r1.u32); }
void GrawBlendDest(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("DestBlend", r3.u32, r4.u32, r1.u32); }
void GrawBlendSrcAlpha(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("SrcBlendAlpha", r3.u32, r4.u32, r1.u32); }
void GrawBlendOpAlpha(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("BlendOpAlpha", r3.u32, r4.u32, r1.u32); }
void GrawBlendDestAlpha(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("DestBlendAlpha", r3.u32, r4.u32, r1.u32); }
void GrawBlendEnable(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("AlphaBlendEnable", r3.u32, r4.u32, r1.u32); }
void GrawBlendSeparate(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("SeparateAlphaBlendEnable", r3.u32, r4.u32, r1.u32); }
