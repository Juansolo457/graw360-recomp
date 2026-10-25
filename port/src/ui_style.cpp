// GRAW 360 - a readable font and spacing for the runtime's dialogs. See ui_style.h.

#include "ui_style.h"

#include <algorithm>
#include <filesystem>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(graw_ui_font_size, 24.0, "GRAW",
                      "Font size of the runtime's own dialogs (save name, message boxes) at a "
                      "720-pixel-high window, scaled with the window's height; 0 = the SDK's 10 px "
                      "default")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace graw::ui {
namespace {

// The font is rasterized this large and drawn scaled down, so it stays sharp up to 4K.
constexpr float kRasterSize = 72.0f;

float Size() { return static_cast<float>(REXCVAR_GET(graw_ui_font_size)); }

bool g_font_loaded = false;
ImGuiStyle g_base_style;  // after ScaleStyle, at a 720-pixel-high window
bool g_have_base_style = false;
float g_applied_scale = 0.0f;

}  // namespace

void AddFonts(ImFontAtlas* atlas) {
  if (Size() <= 0.0f) return;
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0};  // Latin, Latin Extended-A
  ImFontConfig config;
  config.OversampleH = config.OversampleV = 2;
  for (const char* path : {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\bahnschrift.ttf",
                           "C:\\Windows\\Fonts\\arial.ttf"}) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) continue;
    if (ImFont* font = atlas->AddFontFromFileTTF(path, kRasterSize, &config, ranges)) {
      ImGui::GetIO().FontDefault = font;
      g_font_loaded = true;
      REXLOG_INFO("GRAW: dialog font {} at {} px per 720 window pixels", path, Size());
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
  g_base_style = style;
  g_have_base_style = true;
}

void ApplyWindowScale(ImGuiIO& io) {
  if (!g_font_loaded || !g_have_base_style || io.DisplaySize.y <= 0.0f) return;
  // The dialogs keep their size relative to the window, as the game's own menus do: on a 4K
  // window (or a 200% Windows display) they were a small box in the middle (#8).
  const float scale = std::max(0.5f, io.DisplaySize.y / 720.0f);
  io.FontGlobalScale = Size() * scale / kRasterSize;
  if (scale != g_applied_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base_style;
    style.ScaleAllSizes(scale);
    g_applied_scale = scale;
  }
}

}  // namespace graw::ui
