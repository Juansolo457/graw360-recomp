// F.E.A.R. 360 - the display's own vertical blank. See display_vblank.h.

#include "display_vblank.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

REXCVAR_DECLARE(double, video_mode_refresh_rate);
REXCVAR_DECLARE(bool, vulkan_allow_present_mode_immediate);

REXCVAR_DEFINE_STRING(fear_vblank_source, "display", "F.E.A.R.",
                      "What drives the game's vblank (and so its frame pacing): display = the "
                      "display's own refresh, each frame shown for a whole number of refreshes; "
                      "timer = a software timer at video_mode_refresh_rate")
    .allowed({"display", "timer"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(fear_frame_rate_limit, 120.0, "F.E.A.R.",
                      "Highest frame rate with fear_vblank_source=display; the rate used is the "
                      "display's refresh over the smallest whole number that keeps it at or "
                      "under this. Played at 120; the tracers that broke above ~90 were the "
                      "renderer's (fear_vertex_exact)")
    .range(20.0, 240.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

namespace fear::display_vblank {

Waiter::~Waiter() {
  if (output_) static_cast<IDXGIOutput*>(output_)->Release();
}

bool Waiter::Open() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return false;
  }
  const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; !output_ && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; !output_ && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == primary) {
        output_ = output;
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        // 0 and 1 mean the hardware's default rate: unknown.
        if (GetMonitorInfoW(primary, &info) &&
            EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1) {
          refresh_hz_ = double(mode.dmDisplayFrequency);
        }
      } else {
        output->Release();
      }
    }
    adapter->Release();
  }
  factory->Release();
  return output_ != nullptr;
}

bool Waiter::Wait() {
  return output_ && SUCCEEDED(static_cast<IDXGIOutput*>(output_)->WaitForVBlank());
}

bool UnderWine() {
  static const bool wine =
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
  return wine;
}

void ShortSleep(std::chrono::microseconds duration) {
  using DelayExecution = LONG(NTAPI*)(BOOLEAN alertable, PLARGE_INTEGER interval);
  static const DelayExecution delay =
      UnderWine() ? reinterpret_cast<DelayExecution>(reinterpret_cast<void*>(
                        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution")))
                  : nullptr;
  if (delay) {
    LARGE_INTEGER interval;
    interval.QuadPart = -std::max<LONGLONG>(1, duration.count() * 10);  // relative, 100 ns units
    delay(FALSE, &interval);
    return;
  }
  rex::thread::SleepUntil(std::chrono::steady_clock::now() + duration);
}

}  // namespace fear::display_vblank

#else

namespace fear::display_vblank {
Waiter::~Waiter() = default;
bool Waiter::Open() { return false; }
bool Waiter::Wait() { return false; }
bool UnderWine() { return false; }
void ShortSleep(std::chrono::microseconds duration) {
  rex::thread::SleepUntil(std::chrono::steady_clock::now() + duration);
}
}  // namespace fear::display_vblank

#endif

namespace fear::display_vblank {
namespace {
Plan g_plan;
}  // namespace

const Plan& GetPlan() { return g_plan; }

void Configure() {
  g_plan = {};
  if (REXCVAR_GET(fear_vblank_source) != "display") {
    REXLOG_INFO("F.E.A.R.: vblank from a timer at {} Hz", REXCVAR_GET(video_mode_refresh_rate));
    return;
  }
  Waiter waiter;
  const bool opened = waiter.Open();
  const double hz = waiter.RefreshHz();
  if (!opened || hz < 24.0) {
    REXLOG_WARN("F.E.A.R.: no display vblank to follow (output {}, {} Hz); vblank from a timer "
                "at {} Hz", opened ? "found" : "not found", hz,
                REXCVAR_GET(video_mode_refresh_rate));
    return;
  }
  const double limit = REXCVAR_GET(fear_frame_rate_limit);
  g_plan.active = true;
  g_plan.display_hz = hz;
  g_plan.refreshes_per_frame = std::max(1u, uint32_t(std::ceil(hz / limit - 1e-6)));
  g_plan.guest_vblanks_per_refresh = g_plan.refreshes_per_frame == 1 ? 2 : 1;
  // The guest's refresh rate is what it gets; at most 480 Hz (SDK patch 0003).
  REXCVAR_SET(video_mode_refresh_rate,
              std::min(480.0, hz * double(g_plan.guest_vblanks_per_refresh)));
  // Presents come right after a vblank: immediate presentation would tear them a little way
  // down the screen where it flips at once, mailbox shows each at the next vblank.
  const bool mailbox = !rex::cvar::HasNonDefaultValue("vulkan_allow_present_mode_immediate");
  if (mailbox) REXCVAR_SET(vulkan_allow_present_mode_immediate, false);
  REXLOG_INFO("F.E.A.R.: vblank from the display, {} Hz: each frame shown for {} refreshes ({:.1f} "
              "fps), guest refresh {} Hz{}",
              hz, g_plan.refreshes_per_frame, hz / g_plan.refreshes_per_frame,
              REXCVAR_GET(video_mode_refresh_rate), mailbox ? ", mailbox presentation" : "");
}

}  // namespace fear::display_vblank
