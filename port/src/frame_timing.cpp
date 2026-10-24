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

void GrawFrameMark() {
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
