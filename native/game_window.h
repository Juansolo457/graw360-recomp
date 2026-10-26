// GRAW native renderer: the game window (the SDK's display window) and ReXGlue's presenter,
// which presents into it unless the native renderer does (Backend::Swap's own_window).
#pragma once

#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace graw::native {

inline rex::ui::Window* GameWindow() {
  rex::Runtime* runtime = rex::Runtime::instance();
  return runtime ? runtime->display_window() : nullptr;
}

// What the backend creates its swapchain on (Backend::Options::game_window): the window's HWND
// on Windows; elsewhere its SDL window (the SDK's windows are SDL's, game_window.cpp), whose
// properties give the X11 or Wayland surface. Null while the window isn't open.
void* GameWindowHandle();

inline rex::ui::Presenter* SdkPresenter() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->presenter() : nullptr;
}

// The window's presenter belongs to the UI thread. Connected again for a XAM dialog, it is asked
// to paint: it paints on guest frames (none while the Xenos plugin draws nothing) and on paint
// requests, and the dialog's own request came while the window was ours, so without this the
// window stayed black until something else repainted it (#24).
inline void ConnectSdkPresenter(bool connect) {
  rex::ui::Window* window = GameWindow();
  rex::ui::Presenter* presenter = SdkPresenter();
  if (window && presenter)
    window->app_context().CallInUIThreadSynchronous([&] {
      window->SetPresenter(connect ? presenter : nullptr);
      if (connect) presenter->RequestUIPaintFromUIThread();
    });
}

}  // namespace graw::native
