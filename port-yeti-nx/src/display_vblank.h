// F.E.A.R. 360 - the display's own vertical blank, for pacing the game.
//
// The game paces itself on the vblank interrupt: it presents every second one, so its frame rate
// is the guest refresh rate over two. fear-nx raises that interrupt from a software timer, which
// runs on its own clock: even at a rate that divides the display's refresh (80 fps on 240 Hz),
// the two drift apart and a frame now and then lands a refresh early or late. With
// fear_vblank_source=display (the default) the interrupt follows the display's real vblank
// instead (DXGI's WaitForVBlank on the primary output, any GPU vendor), and each frame is held to
// exactly N display refreshes, N the smallest that keeps it at or under fear_frame_rate_limit:
// 80 fps on 240 Hz (3), 72 on 144 Hz (2), 60 on 60 Hz (1, with two guest vblanks per refresh).
// Frames go out through mailbox presentation (no tearing), right after a vblank, so each one is
// shown at the next.
//
// A display whose vblanks stop coming at its refresh rate (variable refresh, or switched off)
// falls back to the timer at the same rate, which suits variable refresh anyway.
//
// Under Proton (Steam Deck) the same plan runs on the timer from the start: there DXGI's
// WaitForVBlank kept two threads spinning at ~60% of a core each (#28 profile), on a 4-core CPU
// whose other hyperthread is the game's.

#pragma once

#include <chrono>
#include <cstdint>

namespace fear::display_vblank {

// What Configure decided; inactive = fear-nx's timer at video_mode_refresh_rate.
struct Plan {
  bool active = false;
  double display_hz = 0;
  // Display refreshes per frame (N), and guest vblanks per display refresh (two when N is 1, so
  // the game's every-second-vblank cadence fits one refresh).
  uint32_t refreshes_per_frame = 1;
  uint32_t guest_vblanks_per_refresh = 1;
};

// Before the graphics system is created: reads the display, sets the guest refresh rate (display
// rate x guest_vblanks_per_refresh) and mailbox presentation. Native renderer only.
void Configure();
const Plan& GetPlan();

// Running under Wine (Proton): its ntdll exports wine_get_version.
bool UnderWine();

// A sleep of under a millisecond, for the renderer's short waits. Under Wine it is ntdll's
// NtDelayExecution, which (not alertable) sleeps in the process with no wineserver round trip;
// elsewhere rex::thread::SleepUntil's high-resolution waitable timer, which under Wine costs two
// round trips (setting the timer, then waiting on it). Windows' own NtDelayExecution keeps to the
// system timer's resolution, a millisecond or more.
void ShortSleep(std::chrono::microseconds duration);

class Waiter {
 public:
  Waiter() = default;
  ~Waiter();
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;

  // Finds the primary display's output. False where there is none (or off Windows).
  bool Open();
  // Blocks until the display's next vertical blank. False if the wait failed.
  bool Wait();
  // The display's refresh rate in Hz, 0 if unknown.
  double RefreshHz() const { return refresh_hz_; }

 private:
  void* output_ = nullptr;  // IDXGIOutput*
  double refresh_hz_ = 0;
};

}  // namespace fear::display_vblank
