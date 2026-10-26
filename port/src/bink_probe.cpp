// GRAW 360 - Bink video probe for the black Cross-Com box and narration panel (#2).
//
// The campaign plays Cross-Com clips through a small video object (Yeti_Final_Xenon.exe):
//  - sub_82340648 (update, each tick): decodes with BinkDoFrame (0x825B8CE0) / BinkNextFrame
//    (0x825B9280) until BinkWait (0x825B8348) says the next frame isn't due, then sets the
//    "has a frame" byte at +24. It also watches BinkGetRealtime's read buffer (0x825B86E8,
//    below 25 % full changes the state at +40).
//  - sub_823400A0 (draw): if +24 is set, draws the frame (sub_82340EE8: Bink's three-plane
//    YUV shader on a quad); otherwise it only clears the box, to black.
// So a box that stays black means +24 never sets, or the draw that follows lands nowhere.
// These [[entrypoint.midasm_hook]]s (port-yeti manifest) log the object's state when it changes,
// and every 2 s how often each path ran. Off unless --graw_debug_bink.
//
// Bink object fields read: +0 Width, +4 Height, +8 Frames, +12 FrameNum (Bink 1.x, big-endian).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "host_memory.h"

REXCVAR_DEFINE_BOOL(graw_debug_bink, false, "GRAW",
                    "Log the campaign's Bink video objects (Cross-Com, #2): ready flag, state, "
                    "frame number, BinkWait results, read buffer")
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
uint8_t Load8(uint32_t a) { return Base()[a]; }

struct ObjectState {
  uint32_t bink = 0, w = 0, h = 0, frames = 0;
  uint8_t ready = 0, f44 = 0, f46 = 0, f48 = 0;
  uint32_t s36 = 0, s40 = 0, n56 = 0, n60 = 0;
  bool operator==(const ObjectState&) const = default;
};

std::mutex g_mutex;
std::map<uint32_t, ObjectState> g_last;  // per video object
struct Counts {
  uint64_t updates = 0, draws = 0, draws_ready = 0, wait0 = 0, wait1 = 0, ready_set = 0;
  uint32_t last_frame = 0, buffer_used = 0, buffer_size = 0;
};
std::map<uint32_t, Counts> g_counts;
auto g_window = std::chrono::steady_clock::now();
uint32_t g_current = 0;  // the object sub_82340648 is updating (one guest thread runs it)

ObjectState Read(uint32_t obj) {
  ObjectState s;
  if (!Readable(obj, 72)) return s;
  s.bink = Load32(obj + 12);
  s.ready = Load8(obj + 24);
  s.s36 = Load32(obj + 36);
  s.s40 = Load32(obj + 40);
  s.f44 = Load8(obj + 44);
  s.f46 = Load8(obj + 46);
  s.f48 = Load8(obj + 48);
  s.n56 = Load32(obj + 56);
  s.n60 = Load32(obj + 60);
  if (s.bink && Readable(s.bink, 16)) {
    s.w = Load32(s.bink + 0);
    s.h = Load32(s.bink + 4);
    s.frames = Load32(s.bink + 8);
  }
  return s;
}

void Observe(uint32_t obj, const char* where) {
  ObjectState s = Read(obj);
  auto it = g_last.find(obj);
  if (it != g_last.end() && it->second == s) return;
  g_last[obj] = s;
  const uint32_t frame = (s.bink && Readable(s.bink, 16)) ? Load32(s.bink + 12) : 0;
  REXLOG_INFO(
      "GRAW bink {:08X} ({}): bink {:08X} {}x{} frame {}/{} ready {} state36 {} state40 {} "
      "b44 {} b46 {} b48 {} n56 {} n60 {}",
      obj, where, s.bink, s.w, s.h, frame, s.frames, s.ready, s.s36, s.s40, s.f44, s.f46, s.f48,
      s.n56, s.n60);
}

void MaybeSummarise() {
  auto now = std::chrono::steady_clock::now();
  if (now - g_window < std::chrono::seconds(2)) return;
  g_window = now;
  for (auto& [obj, c] : g_counts) {
    if (!c.updates && !c.draws) continue;
    REXLOG_INFO(
        "GRAW bink {:08X} last 2 s: {} updates, {} draws ({} with a frame), BinkWait 0:{} "
        "1+:{}, ready set {}, frame {}, read buffer {}/{}",
        obj, c.updates, c.draws, c.draws_ready, c.wait0, c.wait1, c.ready_set, c.last_frame,
        c.buffer_used, c.buffer_size);
    c = Counts{.last_frame = c.last_frame, .buffer_used = c.buffer_used,
               .buffer_size = c.buffer_size};
  }
}

}  // namespace

// 0x82340648, entry of the update.
void GrawBinkUpdate(PPCRegister& r3) {
  if (!REXCVAR_GET(graw_debug_bink)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current = r3.u32;
  ++g_counts[g_current].updates;
  Observe(g_current, "update");
  MaybeSummarise();
}

// 0x823407A0 and 0x823407BC, just after each bl BinkWait.
void GrawBinkWaitResult(PPCRegister& r3) {
  if (!REXCVAR_GET(graw_debug_bink)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& c = g_counts[g_current];
  (r3.u32 ? c.wait1 : c.wait0)++;
}

// 0x823407C8, the stb that sets the ready byte.
void GrawBinkReady(PPCRegister& r31) {
  if (!REXCVAR_GET(graw_debug_bink)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& c = g_counts[r31.u32];
  ++c.ready_set;
  const uint32_t bink = Readable(r31.u32 + 12, 4) ? Load32(r31.u32 + 12) : 0;
  if (bink && Readable(bink, 16)) c.last_frame = Load32(bink + 12);
}

// 0x8234080C, just after bl BinkGetRealtime(bink, r1+96, 0): ReadBufferSize +44, Used +48.
void GrawBinkRealtime(PPCRegister& r1, PPCRegister& r31) {
  if (!REXCVAR_GET(graw_debug_bink)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& c = g_counts[r31.u32];
  if (Readable(r1.u32 + 96, 64)) {
    c.buffer_size = Load32(r1.u32 + 140);
    c.buffer_used = Load32(r1.u32 + 144);
  }
}

// 0x823400A0, entry of the draw.
void GrawBinkDraw(PPCRegister& r3) {
  if (!REXCVAR_GET(graw_debug_bink)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& c = g_counts[r3.u32];
  ++c.draws;
  if (Readable(r3.u32 + 24, 1) && Load8(r3.u32 + 24)) ++c.draws_ready;
  Observe(r3.u32, "draw");
  MaybeSummarise();
}
