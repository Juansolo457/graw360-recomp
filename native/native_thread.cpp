// GRAW native renderer: thread names and ids (native_thread.h).
#include "native_thread.h"

#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace graw::native {

void SetThreadName(const char* name) {
#if defined(_WIN32)
  std::wstring wide(name, name + std::strlen(name));
  SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
  char shortened[16] = {};
  std::strncpy(shortened, name, sizeof(shortened) - 1);
  pthread_setname_np(pthread_self(), shortened);
#endif
}

uint32_t CurrentThreadId() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return uint32_t(gettid());
#endif
}

}  // namespace graw::native
