// Stress test of rex::thread waits (the SDK's POSIX WaitMultiple) against a built librexruntime:
//   clang++ -std=c++23 -O2 -I<SDK install>/include tools/linux/waittest.cpp -L<SDK install>/lib \n//     -lrexruntimerd -Wl,-rpath,<SDK install>/lib -o /tmp/waittest && /tmp/waittest
#include <rex/thread.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>
using namespace std::chrono;
using namespace rex::thread;
static double thread_cpu_ms() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

int main() {
  // 1. Ping-pong through two-handle alertable WaitAny (the audio worker's shape).
  {
    auto a = Event::CreateAutoResetEvent(false), b = Event::CreateAutoResetEvent(false);
    auto quit = Event::CreateManualResetEvent(false);
    const int n = 20000; std::atomic<int> timeouts{0};
    std::thread peer([&] {
      WaitHandle* hs[] = {a.get(), quit.get()};
      for (int i = 0; i < n; ++i) {
        auto r = WaitAny(hs, 2, true, milliseconds(500));
        if (r.first != WaitResult::kSuccess || r.second != 0) { timeouts++; }
        b->Set();
      }
    });
    WaitHandle* hs[] = {b.get(), quit.get()};
    auto t0 = steady_clock::now();
    for (int i = 0; i < n; ++i) {
      a->Set();
      auto r = WaitAny(hs, 2, true, milliseconds(500));
      if (r.first != WaitResult::kSuccess || r.second != 0) timeouts++;
    }
    double us = duration<double, std::micro>(steady_clock::now() - t0).count() / n;
    peer.join();
    std::printf("1. ping-pong: %.1f us per round trip, %d failed waits\n", us, timeouts.load());
    CHECK(timeouts == 0, "ping-pong lost a wake");
    CHECK(us < 100, "ping-pong too slow: %.1f us", us);
  }
  // 2. Idle alertable WaitAny on 9 semaphores for 2 s: CPU time.
  {
    std::vector<std::unique_ptr<Semaphore>> s; std::vector<WaitHandle*> hs;
    for (int i = 0; i < 9; ++i) { s.push_back(Semaphore::Create(0, 64)); hs.push_back(s.back().get()); }
    double c0 = thread_cpu_ms(); auto t0 = steady_clock::now(); int waits = 0;
    while (steady_clock::now() - t0 < seconds(2)) { WaitAny(hs.data(), hs.size(), true, milliseconds(500)); waits++; }
    double cpu = thread_cpu_ms() - c0;
    std::printf("2. idle 9-handle wait: %.1f ms CPU in 2 s (%d timeouts)\n", cpu, waits);
    CHECK(cpu < 300, "idle wait burns CPU: %.1f ms", cpu);
    // A release from another thread wakes it promptly.
    std::thread rel([&] { std::this_thread::sleep_for(milliseconds(50)); s[7]->Release(1, nullptr); });
    auto t1 = steady_clock::now();
    auto r = WaitAny(hs.data(), hs.size(), true, milliseconds(500));
    double ms = duration<double, std::milli>(steady_clock::now() - t1).count();
    rel.join();
    std::printf("   woken by handle %zu after %.2f ms (released at 50 ms)\n", r.second, ms);
    CHECK(r.first == WaitResult::kSuccess && r.second == 7 && ms < 55, "wake from release");
  }
  // 3. Four multi-waiters on one semaphore (plus a private event each): exact count of wakes.
  {
    auto sem = Semaphore::Create(0, 100000);
    std::atomic<int> got{0}; std::atomic<bool> stop{false};
    std::vector<std::thread> ws;
    std::vector<std::unique_ptr<Event>> priv;
    for (int i = 0; i < 4; ++i) priv.push_back(Event::CreateAutoResetEvent(false));
    for (int i = 0; i < 4; ++i) ws.emplace_back([&, i] {
      WaitHandle* hs[] = {priv[i].get(), sem.get()};
      while (!stop) { auto r = WaitAny(hs, 2, (i & 1) != 0, milliseconds(20)); if (r.first == WaitResult::kSuccess && r.second == 1) got++; }
    });
    const int total = 50000;
    for (int i = 0; i < total; ++i) { sem->Release(1, nullptr); if ((i & 255) == 0) std::this_thread::yield(); }
    auto t0 = steady_clock::now();
    while (got < total && steady_clock::now() - t0 < seconds(5)) std::this_thread::sleep_for(milliseconds(1));
    std::this_thread::sleep_for(milliseconds(50));
    stop = true; for (auto& w : ws) w.join();
    std::printf("3. semaphore: %d released, %d taken\n", total, got.load());
    CHECK(got == total, "semaphore count mismatch");
  }
  // 4. WaitAll on two events, set from another thread.
  {
    auto e1 = Event::CreateManualResetEvent(false), e2 = Event::CreateManualResetEvent(false);
    WaitHandle* hs[] = {e1.get(), e2.get()};
    std::thread t([&] { std::this_thread::sleep_for(milliseconds(10)); e1->Set(); std::this_thread::sleep_for(milliseconds(10)); e2->Set(); });
    auto t0 = steady_clock::now();
    auto r = WaitAll(hs, 2, false, milliseconds(1000));
    double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
    t.join();
    std::printf("4. wait-all: %s after %.2f ms\n", r == WaitResult::kSuccess ? "ok" : "FAILED", ms);
    CHECK(r == WaitResult::kSuccess && ms < 30, "wait-all");
  }
  // 5. Timeout accuracy, alertable and not.
  for (bool alertable : {false, true}) {
    auto e1 = Event::CreateAutoResetEvent(false), e2 = Event::CreateAutoResetEvent(false);
    WaitHandle* hs[] = {e1.get(), e2.get()};
    auto t0 = steady_clock::now();
    auto r = WaitAny(hs, 2, alertable, milliseconds(5));
    double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
    std::printf("5. timeout 5 ms (alertable %d): %.2f ms\n", alertable, ms);
    CHECK(r.first == WaitResult::kTimeout && ms >= 4.9 && ms < 8, "timeout");
  }
  std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
