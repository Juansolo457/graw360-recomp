// GRAW 360 - the controller drives the runtime's own dialogs. See ui_pad.h.

#include "ui_pad.h"

#include <cstdint>

#include <imgui.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>

namespace graw::ui {
namespace {

struct Mapping {
  uint16_t button;
  ImGuiKey key;
};

constexpr Mapping kMappings[] = {
    {0x1000, ImGuiKey_Enter},      // A
    {0x2000, ImGuiKey_Escape},     // B
    {0x0001, ImGuiKey_UpArrow},    // D-pad
    {0x0002, ImGuiKey_DownArrow},
    {0x0004, ImGuiKey_LeftArrow},
    {0x0008, ImGuiKey_RightArrow},
};

class PadBridge final : public rex::ui::ImGuiDialog {
 public:
  explicit PadBridge(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const uint16_t buttons = rex::kernel::xam::xeXamIsUIActive() ? Buttons() : 0;
    const uint16_t changed = static_cast<uint16_t>(buttons ^ previous_);
    for (const Mapping& m : kMappings) {
      if (changed & m.button) io.AddKeyEvent(m.key, (buttons & m.button) != 0);
    }
    if (buttons && !previous_) {
      REXLOG_DEBUG("GRAW: controller to dialog keys {:04X}", buttons);
    }
    previous_ = buttons;
  }

 private:
  // Player 1's controller as the game would see it, read past the dialog's input block.
  static uint16_t Buttons() {
    auto* state = rex::system::kernel_state();
    if (!state || !state->emulator()) return 0;
    auto* input = dynamic_cast<rex::input::InputSystem*>(state->emulator()->input_system());
    if (!input) return 0;
    rex::input::X_INPUT_STATE pad{};
#if defined(GRAW_NX)
    // fear-nx's SDK (port-yeti-nx) has no GetStateForUI; its GetState doesn't block while a
    // dialog is open, so it reads the same thing.
    if (input->GetState(0, &pad) != 0) return 0;
#else
    if (input->GetStateForUI(0, &pad) != 0) return 0;
#endif
    return pad.gamepad.buttons;
  }

  uint16_t previous_ = 0;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreatePadBridge(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer) return nullptr;
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  return std::make_unique<PadBridge>(drawer);
}

}  // namespace graw::ui
