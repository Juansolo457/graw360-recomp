// GRAW native renderer: the game window's handle for the backend (game_window.h).
#include "game_window.h"

#if !defined(_WIN32)
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#endif

namespace graw::native {

void* GameWindowHandle() {
  rex::ui::Window* window = GameWindow();
  if (!window) return nullptr;
#if defined(_WIN32)
  return window->GetNativeWindowHandle();
#else
  // The SDK's WindowSDL keeps its SDL_Window to itself: the game's is the SDL window with its
  // title (the only one in practice). SDL's window list is the UI thread's.
  void* found = nullptr;
  window->app_context().CallInUIThreadSynchronous([&] {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (!windows) return;
    for (int i = 0; i < count && !found; ++i) {
      const char* title = SDL_GetWindowTitle(windows[i]);
      if (title && window->GetTitle() == title) found = windows[i];
    }
    if (!found && count == 1) found = windows[0];
    SDL_free(windows);
  });
  return found;
#endif
}

}  // namespace graw::native
