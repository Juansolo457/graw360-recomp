// GRAW host: whether host memory can be read without faulting, for the diagnostic hooks that
// follow guest pointers (a register can hold a float or a stale value that points at unmapped
// guest memory). Windows asks VirtualQuery; Linux reads one byte of each page through
// process_vm_readv, which fails with EFAULT instead of faulting.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace graw {

inline bool HostReadable(const void* start, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(start);
  const uint8_t* end = p + n;
#if defined(_WIN32)
  while (p < end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || info.Protect == 0) {
      return false;
    }
    p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
#else
  static const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
  static const pid_t self = getpid();
  while (p < end) {
    uint8_t byte;
    iovec local{&byte, 1}, remote{const_cast<uint8_t*>(p), 1};
    if (process_vm_readv(self, &local, 1, &remote, 1, 0) != 1) return false;
    p = reinterpret_cast<const uint8_t*>((uintptr_t(p) | (page - 1)) + 1);
  }
  return true;
#endif
}

}  // namespace graw
