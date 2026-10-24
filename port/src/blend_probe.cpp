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

std::mutex g_mutex;
std::map<std::tuple<const char*, uint32_t, uint32_t, uint32_t>, uint64_t> g_counts;
auto g_window = std::chrono::steady_clock::now();
int g_windows = 0;

void Note(const char* field, uint32_t device, uint32_t value, uint32_t sp) {
  if (!REXCVAR_GET(graw_debug_blend)) return;
  auto [c1, c2] = Callers(sp);
  std::lock_guard<std::mutex> lock(g_mutex);
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

}  // namespace

void GrawBlendSrc(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("SrcBlend", r3.u32, r4.u32, r1.u32); }
void GrawBlendOp(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("BlendOp", r3.u32, r4.u32, r1.u32); }
void GrawBlendDest(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("DestBlend", r3.u32, r4.u32, r1.u32); }
void GrawBlendSrcAlpha(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("SrcBlendAlpha", r3.u32, r4.u32, r1.u32); }
void GrawBlendOpAlpha(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("BlendOpAlpha", r3.u32, r4.u32, r1.u32); }
void GrawBlendDestAlpha(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("DestBlendAlpha", r3.u32, r4.u32, r1.u32); }
void GrawBlendEnable(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("AlphaBlendEnable", r3.u32, r4.u32, r1.u32); }
void GrawBlendSeparate(PPCRegister& r3, PPCRegister& r4, PPCRegister& r1) { Note("SeparateAlphaBlendEnable", r3.u32, r4.u32, r1.u32); }
