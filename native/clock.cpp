// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): the CPU clock (clock.h).
#include "clock.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
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

ThreadUsage ThreadUsageNow() {
  ThreadUsage u;
#if defined(_WIN32)
  // Cycle counts run on the time-stamp counter here: GetThreadTimes only moves in 15.6 ms steps.
  ULONG64 cycles = 0;
  if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) u.cpu_ns = TicksToNs(cycles);
  if (QueryProcessCycleTime(GetCurrentProcess(), &cycles)) u.process_cpu_ns = TicksToNs(cycles);
#else
  timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  u.cpu_ns = uint64_t(t.tv_sec) * 1'000'000'000u + uint64_t(t.tv_nsec);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  u.process_cpu_ns = uint64_t(t.tv_sec) * 1'000'000'000u + uint64_t(t.tv_nsec);
  rusage r;
  if (getrusage(RUSAGE_THREAD, &r) == 0) {
    u.blocks = uint64_t(r.ru_nvcsw);
    u.preempts = uint64_t(r.ru_nivcsw);
  }
  // "<time on the CPU> <time waiting for it> <timeslices>", per thread: kept open for the thread.
  thread_local const int schedstat = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
  char text[96];
  if (schedstat >= 0) {
    const ssize_t n = pread(schedstat, text, sizeof(text) - 1, 0);
    if (n > 0) {
      text[n] = 0;
      unsigned long long on_cpu = 0, waiting = 0;
      if (std::sscanf(text, "%llu %llu", &on_cpu, &waiting) == 2) u.runq_ns = waiting;
    }
  }
#endif
  return u;
}

ThreadUsage ThreadUsageSince(const ThreadUsage& begin, const ThreadUsage& now) {
  const auto since = [](uint64_t b, uint64_t n) { return n >= b ? n - b : 0; };
  return {since(begin.cpu_ns, now.cpu_ns), since(begin.runq_ns, now.runq_ns),
          since(begin.blocks, now.blocks), since(begin.preempts, now.preempts),
          since(begin.process_cpu_ns, now.process_cpu_ns)};
}

}  // namespace graw::native
