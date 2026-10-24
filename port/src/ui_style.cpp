// GRAW 360 - a readable font and spacing for the runtime's dialogs. See ui_style.h.

#include "ui_style.h"

#include <filesystem>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(graw_ui_font_size, 24.0, "GRAW",
                      "Font size of the runtime's own dialogs (save name, message boxes), in "
                      "logical pixels; 0 = the SDK's 10 px default")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace graw::ui {
namespace {

float Size() { return static_cast<float>(REXCVAR_GET(graw_ui_font_size)); }

}  // namespace

void AddFonts(ImFontAtlas* atlas) {
  const float size = Size();
  if (size <= 0.0f) return;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0};  // Latin, Latin Extended-A
  ImFontConfig config;
  config.OversampleH = config.OversampleV = 2;
  for (const char* path : {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\bahnschrift.ttf",
                           "C:\\Windows\\Fonts\\arial.ttf"}) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) continue;
    if (ImFont* font = atlas->AddFontFromFileTTF(path, size, &config, ranges)) {
      ImGui::GetIO().FontDefault = font;
      REXLOG_INFO("GRAW: dialog font {} at {} px", path, size);
      return;
    }
  }
  REXLOG_WARN("GRAW: no system font found; dialogs keep the SDK's 10 px font");
}

void ScaleStyle(ImGuiStyle& style) {
  const float size = Size();
  if (size <= 0.0f) return;
  // The SDK's spacing is tuned for its 10 px font; keep the proportions.
  style.ScaleAllSizes(size / 13.0f);
}

}  // namespace graw::ui
