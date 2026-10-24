// GRAW 360 - a readable font and spacing for the runtime's own dialogs (#8).
//
// The SDK's ImGui uses a 10 px bitmap font (ProggyTiny) by default, in logical pixels: on a 200%
// Windows display the 2560x1440 window is 1280x720 logical, so the dialogs the runtime draws for
// the game (the save-name keyboard after New game, message boxes, sign-in) came out tiny. These
// make a system TrueType font the default at graw_ui_font_size and scale the spacing to match.
// Only the runtime's ImGui is affected, not the game's own graphics.

#pragma once

struct ImFontAtlas;
struct ImGuiStyle;

namespace graw::ui {

void AddFonts(ImFontAtlas* atlas);   // ReXApp::OnConfigureFonts
void ScaleStyle(ImGuiStyle& style);  // ReXApp::OnConfigureStyle

}  // namespace graw::ui
