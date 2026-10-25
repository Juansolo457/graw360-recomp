// graw - Ghost Recon Advanced Warfighter (Xbox 360), ReXGlue application.
//
// Shared by the disc's programs (graw_loader.h): port/ (default.xex, the front end, graw.exe),
// port-mp/ (gr3xenon_release.exe: Instant Action, co-op, multiplayer, grawmp.exe) and port-yeti/
// (yeti_final_xenon.exe, the campaign, grawyeti.exe). Each main.cpp includes its own generated
// init header, defines GRAW_APP_NAME, then includes this file. The programs hand over to each
// other the way the console's loader would (graw_loader.h).

#pragma once

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

#include "autopilot.h"

void GrawRaiseTimerResolution();  // frame_timing.cpp
#include "crash_report.h"
#include "graw_loader.h"
#include "graw_process.h"
#include "ui_pad.h"
#include "ui_style.h"

#if defined(GRAW_NX)
// port-yeti-nx (fear-nx's patched SDK): fear-nx's native renderer and the statically linked
// stock Xenos plugin (plugin_main.cpp).
#include <rex/system/gpu_plugin.h>

#include "display_vblank.h"
#include "native/native_system.h"
extern "C" uint32_t rex_gpu_abi_version(void);
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                         const rex::system::GpuCreateInfo* info);
#endif

#ifndef GRAW_APP_NAME
#define GRAW_APP_NAME "graw"
#endif
// The executable on the disc this program is the translation of (the runtime loads
// game:\default.xex unless told otherwise).
// What the window title says this program is (the SDK's title is the app name and build stamp).
#ifndef GRAW_MODE_TITLE
#define GRAW_MODE_TITLE ""
#endif
#ifndef GRAW_XEX_IMAGE
#define GRAW_XEX_IMAGE "game:\\default.xex"
#endif

// The runtime signs in only player 1 (a local profile; Xbox Live is gone) and gives controllers
// player slots in connection order. On the user's PC a Logitech virtual XInput device takes slot
// 0, so the Xbox pad was player 2: unsigned, "PLAYER TWO" in Instant Action, and in F.E.A.R. no
// saves. As in fear360, every controller (and the keyboard) drives player 1 by default.
REXCVAR_DEFINE_BOOL(graw_shared_controllers, true, "GRAW",
                    "Every connected controller (and the keyboard) drives player 1, the "
                    "signed-in profile that can save; off = one controller per player slot")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

class GrawApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GrawApp>(new GrawApp(ctx, GRAW_APP_NAME,
        PPCImageConfig));
  }

 protected:
  // A program started by another (the hand-off) waits for that one to exit first, so the two
  // never hold the window, the saves or the log at the same time.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    graw::process::WaitForPreviousRun();
    // A packaged copy (a folder with the three programs, game\ = the disc's files and user\ =
    // profile and saves, e.g. for a Steam Deck under Proton) runs without command-line paths.
    const auto own_dir = graw::process::OwnExecutable().parent_path();
    std::error_code error;
    if (paths.game_data_root.empty() && std::filesystem::is_directory(own_dir / "game", error)) {
      paths.game_data_root = own_dir / "game";
    }
    if (REXCVAR_GET(user_data_root).empty() &&
        std::filesystem::is_directory(own_dir / "user", error)) {
      paths.user_data_root = own_dir / "user";
      paths.cache_root = own_dir / "user" / "cache";
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // 0.5 ms Windows timer resolution: the runtime's Sleep(1)s pace the guest's vblanks (#11).
    GrawRaiseTimerResolution();

    // logs/crash-<time>.txt + .dmp with the guest stack (sub_<360 address> names) on any fault.
    graw::crash_report::InstallCrashReporter();

    // Without a GPU plugin the runtime ignores every Vd* call and the game never draws
    // (F.E.A.R. lesson). "xenos" is the SDK's Xenos -> D3D12/Vulkan translator; a
    // --gpu_plugin on the command line still takes precedence.
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";

    // Shared controllers, and the scripted controller for unattended tests (autopilot.h).
    if (config.input_factory) {
      config.input_factory = [base = config.input_factory](bool tool_mode) {
        auto input = base(tool_mode);
        if (auto* system = dynamic_cast<rex::input::InputSystem*>(input.get())) {
          if (REXCVAR_GET(graw_shared_controllers)) {
            system->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
            REXLOG_INFO("GRAW: every controller drives player 1 (the signed-in profile)");
          }
          if (auto autopilot = graw::autopilot::Create()) system->AddDriver(std::move(autopilot));
        }
        return input;
      };
    }
#if defined(GRAW_NX)
    // port-yeti-nx: fear-nx's native Vulkan renderer (--nx_renderer native, its default), or the
    // stock Xenos plugin linked into this executable (--nx_renderer xenos) for comparison. No
    // plugin DLL either way (#2).
    config.gpu_plugin.clear();
    if (fear_nx::native::Enabled()) {
      fear::display_vblank::Configure();
      config.graphics = fear_nx::native::CreateGraphicsSystem();
      REXLOG_INFO("GRAW ({}): fear-nx's native Vulkan Xenos renderer", GRAW_APP_NAME);
      return;
    }
    if (rex_gpu_abi_version() != rex::system::kGpuPluginAbiVersion) {
      REXLOG_ERROR("GRAW: linked Xenos plugin has ABI {}, host expects {}", rex_gpu_abi_version(),
                   rex::system::kGpuPluginAbiVersion);
      return;
    }
    rex::system::GpuCreateInfo info{};
    info.struct_size = sizeof(info);
    info.backend = "any";
    config.graphics.reset(rex_gpu_create(rex::system::kGpuPluginAbiVersion, &info));
    REXLOG_INFO("GRAW ({}): the stock Xenos plugin, linked in", GRAW_APP_NAME);
#else
    REXLOG_INFO("GRAW ({}): GPU plugin '{}'", GRAW_APP_NAME, config.gpu_plugin);
#endif
  }

  void OnLoadXexImage(std::string& xex_image) override { xex_image = GRAW_XEX_IMAGE; }

  // "Ghost Recon Advanced Warfighter - Campaign" instead of "grawyeti [rexglue-v0.10...]".
  void OnPostSetup() override {
    if (auto* w = window()) {
      std::string title = "Ghost Recon Advanced Warfighter";
      if (*GRAW_MODE_TITLE) title += std::string(" - ") + GRAW_MODE_TITLE;
      w->SetTitle(title);
    }
  }

  // A readable font for the runtime's own dialogs (save name, message boxes; ui_style.h, #8).
  void OnConfigureFonts(ImFontAtlas* atlas) override { graw::ui::AddFonts(atlas); }
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    (void)ui_style;
    graw::ui::ScaleStyle(imgui_style);
  }

  // The controller confirms and navigates the runtime's dialogs (ui_pad.h, #8). Released in
  // OnShutdown, before the runtime destroys its ImGui drawer.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    pad_bridge_ = graw::ui::CreatePadBridge(drawer);
  }
  void OnShutdown() override { pad_bridge_.reset(); }

  // The launch data the previous program set (XamLoaderSetLaunchData), for this one to read.
  void OnPreLaunchModule() override { graw::loader::RestoreLaunchData(); }

  // XamLoaderLaunchTitle ends the title; start the program that holds the requested executable.
  void OnGuestThreadExit(rex::system::XThread* thread) override {
    if (thread && thread->main_thread()) graw::loader::HandOff();
  }

 private:
  std::unique_ptr<rex::ui::ImGuiDialog> pad_bridge_;
};
