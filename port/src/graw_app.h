// graw - Ghost Recon Advanced Warfighter (Xbox 360), ReXGlue application.
//
// Phase 1 (docs/PLAN.md): the stock SDK runtime and its Xenos GPU plugin, no game-specific
// host code yet. Everything we learn about this executable goes into docs/ first.

#pragma once

#include <rex/logging.h>
#include <rex/rex_app.h>

class GrawApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GrawApp>(new GrawApp(ctx, "graw",
        PPCImageConfig));
  }

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Without a GPU plugin the runtime ignores every Vd* call and the game never draws
    // (F.E.A.R. lesson). "xenos" is the SDK's Xenos -> D3D12/Vulkan translator; a
    // --gpu_plugin on the command line still takes precedence.
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    REXLOG_INFO("GRAW: GPU plugin '{}'", config.gpu_plugin);
  }
};
