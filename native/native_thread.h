// GRAW native renderer: thread names and ids on each platform.
#pragma once

#include <cstdint>

namespace graw::native {

// Names the calling thread for debuggers and profilers (Linux keeps the first 15 characters).
void SetThreadName(const char* name);
// The calling thread's id from the OS, for logs.
uint32_t CurrentThreadId();

}  // namespace graw::native
