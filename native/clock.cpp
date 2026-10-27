// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): the CPU clock (clock.h).
#include "clock.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace graw::native {
namespace {

struct Calibration {
  uint64_t anchor_ns, anchor_ticks;
  double ns_per_tick;
};

#if !defined(_WIN32)
uint64_t MonotonicNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1'000'000'000u + uint64_t(t.tv_nsec);
}
#endif

const Calibration& Calibrated() {
  static const Calibration cal = [] {
#if defined(_WIN32)
    LARGE_INTEGER f, c0, c1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c0);
    const uint64_t t0 = Ticks();
    uint64_t t1;
    do {
      QueryPerformanceCounter(&c1);
      t1 = Ticks();
    } while (c1.QuadPart - c0.QuadPart < f.QuadPart / 50);
    const double ns_per_count = 1e9 / double(f.QuadPart);
    return Calibration{uint64_t(double(c1.QuadPart) * ns_per_count), t1,
                       double(c1.QuadPart - c0.QuadPart) * ns_per_count / double(t1 - t0)};
#else
    const uint64_t n0 = MonotonicNs();
    const uint64_t t0 = Ticks();
    uint64_t n1, t1;
    do {
      n1 = MonotonicNs();
      t1 = Ticks();
    } while (n1 - n0 < 20'000'000);
    return Calibration{n1, t1, double(n1 - n0) / double(t1 - t0)};
#endif
  }();
  return cal;
}

}  // namespace

uint64_t NowNs() {
  const Calibration& cal = Calibrated();
  // Signed: another core's counter may trail the anchor by a few ticks, and a negative double
  // converts to uint64_t undefined.
  const double ns = double(int64_t(Ticks() - cal.anchor_ticks)) * cal.ns_per_tick;
  return cal.anchor_ns + uint64_t(int64_t(ns));
}

uint64_t TicksToNs(uint64_t ticks) { return uint64_t(double(ticks) * Calibrated().ns_per_tick); }

}  // namespace graw::native
