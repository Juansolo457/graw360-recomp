// GRAW 360 - the controller drives the runtime's own dialogs (#8).
//
// The dialogs the runtime draws for the game (the save-name keyboard after New game, message
// boxes) are ImGui windows that only take the keyboard and mouse: the keyboard dialog confirms on
// Enter or a click on OK, and while one is open the game gets no controller input. With only a
// pad (Steam Deck, a TV) New game could not be finished.
//
// PadBridge is an invisible ImGui dialog that runs every frame. While a XAM dialog is open
// (xeXamIsUIActive), it reads player 1's controller (every pad, and the scripted autopilot) and
// turns it into keys for ImGui: A = Enter, B = Escape, D-pad = arrows (keyboard navigation moves
// between OK and Cancel). Nothing changes while no dialog is open.

#pragma once

#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace graw::ui {

// For ReXApp::OnCreateDialogs; keep the result alive for the life of the drawer.
std::unique_ptr<rex::ui::ImGuiDialog> CreatePadBridge(rex::ui::ImGuiDrawer* drawer);

}  // namespace graw::ui
