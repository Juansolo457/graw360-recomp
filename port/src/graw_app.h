// graw - Ghost Recon Advanced Warfighter (Xbox 360), ReXGlue application.
//
// Shared by the disc's programs (graw_loader.h): port/ (default.xex, the front end, graw.exe),
// port-mp/ (gr3xenon_release.exe: Instant Action, co-op, multiplayer, grawmp.exe) and port-yeti/
// (yeti_final_xenon.exe, the campaign, grawyeti.exe). Each main.cpp includes its own generated
// init header, defines GRAW_APP_NAME, then includes this file. The programs hand over to each
// other the way the console's loader would (graw_loader.h).

#pragma once

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/xthread.h>
#include <rex/ui/imgui_dialog.h>

#include "autopilot.h"
#include "crash_report.h"
#include "graw_loader.h"
#include "graw_process.h"
#include "ui_pad.h"
#include "ui_style.h"

#ifndef GRAW_APP_NAME
#define GRAW_APP_NAME "graw"
#endif
// The executable on the disc this program is the translation of (the runtime loads
// game:\default.xex unless told otherwise).
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
    (void)paths;
    graw::process::WaitForPreviousRun();
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
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
    REXLOG_INFO("GRAW ({}): GPU plugin '{}'", GRAW_APP_NAME, config.gpu_plugin);
  }

  void OnLoadXexImage(std::string& xex_image) override { xex_image = GRAW_XEX_IMAGE; }

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
