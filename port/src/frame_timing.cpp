// GRAW 360 - frame timing measured in the host (#11).
//
// The prebuilt SDK has its performance counters compiled out (REXGLUE_ENABLE_PERF_COUNTERS), so
// --perf_log_csv writes nothing. GrawFrameMark runs just before the game's one call to VdSwap
// (present the frame; a [[entrypoint.midasm_hook]] in each manifest), records the time since the
// previous one, and:
//  - every 10 s logs "GRAW frames: ..." with fps, median and 1% low, and how many frames took
//    a 60 Hz or 30 Hz vblank's time (bunching there means the game paces itself on vblanks,
//    which --video_mode_refresh_rate lifts);
//  - with --graw_frame_log <file>, writes one CSV row per frame (frame_time_us), the format
//    tools/perf_report.py reads.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(graw_frame_log, "", "GRAW",
                      "Write one CSV row per presented frame (frame_time_us) to this file")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
Clock::time_point g_last{}, g_window_start{};
std::vector<uint32_t> g_window;  // frame times (us) in the current 10 s window
FILE* g_csv = nullptr;
bool g_csv_tried = false;
uint64_t g_frames = 0;

void Summarise() {
  if (g_window.empty()) return;
  std::vector<uint32_t> s = g_window;
  std::sort(s.begin(), s.end());
  double total = 0;
  for (uint32_t t : s) total += t;
  const double median = s[s.size() / 2] / 1000.0;
  const double low = s[std::min(s.size() - 1, s.size() * 99 / 100)] / 1000.0;
  size_t at60 = 0, at30 = 0;
  for (uint32_t t : s) {
    if (t >= 15500 && t < 17500) ++at60;
    if (t >= 31500 && t < 35500) ++at30;
  }
  REXLOG_INFO(
      "GRAW frames: {:.1f} fps average over {} frames, median {:.2f} ms ({:.1f} fps), 1% low "
      "{:.1f} fps; at 60 Hz vblank {:.0f}%, at 30 Hz {:.0f}%",
      s.size() / (total / 1e6), s.size(), median, 1000.0 / median, 1000.0 / low,
      100.0 * at60 / s.size(), 100.0 * at30 / s.size());
  g_window.clear();
}

}  // namespace

// The front end presents without waiting for a vblank (the campaign's Direct3D waits on them),
// so its menus ran uncapped: 4,700 fps average, a CPU core and the GPU flat out for nothing
// (#25). A frame limiter at the same spot as the frame mark holds every program to at most
// --graw_max_fps.
REXCVAR_DEFINE_INT32(graw_max_fps, 60, "GRAW",
                     "Present at most this many frames per second (0 = no limit); the campaign "
                     "paces itself at 30 below that")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

void GrawSleepUs(int64_t us);  // below

namespace {

// Waits until the next frame is due. Deadlines step by whole periods, so short sleeps' jitter
// doesn't drift the rate; after a frame longer than a period the schedule restarts from now
// instead of letting the next frames catch up.
void LimitFrameRate() {
  const int32_t max_fps = REXCVAR_GET(graw_max_fps);
  if (max_fps <= 0) return;
  const auto period = std::chrono::nanoseconds(1'000'000'000LL / max_fps);
  static Clock::time_point next{};
  Clock::time_point now = Clock::now();
  if (next != Clock::time_point{} && now < next) {
    // The timer wakes up to ~0.2 ms late: sleep to 0.5 ms before the deadline, then spin.
    const int64_t sleep_us =
        std::chrono::duration_cast<std::chrono::microseconds>(next - now).count() - 500;
    GrawSleepUs(sleep_us);
    while ((now = Clock::now()) < next) std::this_thread::yield();
  }
  next = next != Clock::time_point{} && now - next < period ? next + period : now + period;
}

}  // namespace

void GrawFrameMark() {
  LimitFrameRate();
  const Clock::time_point now = Clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_csv_tried) {
    g_csv_tried = true;
    const std::string path = REXCVAR_GET(graw_frame_log);
    if (!path.empty()) {
      g_csv = std::fopen(path.c_str(), "w");
      if (g_csv) std::fputs("frame_time_us\n", g_csv);
    }
  }
  if (g_last == Clock::time_point{}) {
    g_last = g_window_start = now;
    return;
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - g_last).count();
  g_last = now;
  const uint32_t t = static_cast<uint32_t>(std::min<int64_t>(us, 10'000'000));
  g_window.push_back(t);
  if (g_csv) {
    std::fprintf(g_csv, "%u\n", t);
    if (++g_frames % 120 == 0) std::fflush(g_csv);
  }
  if (now - g_window_start >= std::chrono::seconds(10)) {
    Summarise();
    g_window_start = now;
  }
}

// Windows' default timer resolution is 15.6 ms, and nothing in the runtime raises it, so every
// Sleep(1) in it really sleeps up to 15.6 ms: the guest vblank thread (graphics_system.cpp sleeps
// 1 ms between checks and then fires all the vblanks that are due), the command processor's
// WAIT_REG_MEM sleeps and the game's own KeDelayExecutionThread. At 60 Hz that made vblanks
// arrive late and in bursts: ~half the frames 3 ms early or late and ~3% a missed refresh (33 ms),
// although the game needs only ~14 ms of CPU per frame (#11, play-test 2026-10-25). Xenia raises
// the resolution at start-up too. Also asks Windows 11 to keep honouring it when the window is
// in the background (power throttling ignores timer requests then).
REXCVAR_DEFINE_INT32(graw_timer_resolution_us, 500, "GRAW",
                     "Windows timer resolution to request at start-up, in microseconds (0 = leave "
                     "the system default of 15.6 ms)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void GrawRaiseTimerResolution() {
  const int32_t requested_us = REXCVAR_GET(graw_timer_resolution_us);
  if (requested_us <= 0) return;
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;  // don't ignore this process's timer resolution
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling,
                        sizeof(throttling));
  using SetResolution = LONG(WINAPI*)(ULONG desired, BOOLEAN set, ULONG* current);
  auto set_resolution = reinterpret_cast<SetResolution>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution"));
  ULONG current = 0;  // in 100 ns units
  if (set_resolution && set_resolution(ULONG(requested_us) * 10, TRUE, &current) >= 0) {
    REXLOG_INFO("GRAW: timer resolution {} us (requested {} us)", current / 10, requested_us);
  } else {
    REXLOG_WARN("GRAW: couldn't raise the timer resolution; frame pacing may be uneven");
  }
}
#else
void GrawRaiseTimerResolution() {}
#endif

// The campaign's Direct3D waits for room in the GPU command ring by polling the read pointer the
// GPU writes back (sub_824D7830's loop around sub_824E0A00): while the command processor is
// behind, the main thread spins a whole core. On a power-limited device (Steam Deck: CPU and GPU
// share ~15 W) that starves the GPU, whose clock dropped to 200-900 MHz with all CPU threads at
// 60-100% (play-test 2026-10-25). A hook at the loop's back-edge (only reached while waiting)
// sleeps briefly instead.
REXCVAR_DEFINE_INT32(graw_ring_wait_sleep_us, 200, "GRAW",
                     "Sleep this long (microseconds) each time the game's Direct3D waits for room "
                     "in the GPU command ring, instead of spinning; 0 = spin as on the 360")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
void GrawSleepUs(int64_t us) {
  if (us <= 0) return;
  // A high-resolution waitable timer sleeps for well under a millisecond (Sleep can't).
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) {
    Sleep(0);
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -us * 10;  // relative, in 100 ns units
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer, INFINITE);
  }
}

void GrawRingWaitYield() { GrawSleepUs(REXCVAR_GET(graw_ring_wait_sleep_us)); }
#else
void GrawSleepUs(int64_t us) {
  if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void GrawRingWaitYield() {}
#endif
