// GRAW native renderer (ported from pgr4-native 8ea20d5, MIT; see native/README.md): owning Win32 handles.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <type_traits>

namespace graw::native {

// For objects whose creation returns null on failure (events, waitable timers, threads), not
// INVALID_HANDLE_VALUE (files).
struct HandleCloser {
  void operator()(HANDLE h) const noexcept { CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

// An unnamed auto-reset event, not set; null on failure.
inline UniqueHandle CreateAutoResetEvent() {
  return UniqueHandle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

}  // namespace graw::native
