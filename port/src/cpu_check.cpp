// GRAW 360 - stop with a message on a CPU without x86-64-v3 (AVX2, FMA, BMI1/2, F16C, LZCNT,
// MOVBE), instead of crashing on the first such instruction.
//
// All three programs are built for x86-64-v3 (tools/build.ps1, #11: the translated code's
// std::fma for every PowerPC multiply-add is one instruction there, a C runtime call otherwise).
// That is every Intel CPU since Haswell (2013) and AMD since Excavator (2015), and the Steam Deck;
// not older ones, nor Pentium/Celeron before 2017.
//
// This file is compiled for x86-64-v2 (CMakeLists.txt: per-file options, no precompiled header)
// and runs from the C runtime's .CRT$XCB initializers, before the program's own static
// initializers (.CRT$XCU) execute any v3 code. The SDK's DLL (rexruntimerd.dll) initializes
// earlier still; should its startup code use a v3 instruction, an old CPU stops there instead.

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cpuid.h>

namespace {

bool HasX86_64_V3() {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  const bool fma = c & (1u << 12), movbe = c & (1u << 22), osxsave = c & (1u << 27),
             avx = c & (1u << 28), f16c = c & (1u << 29);
  if (!(fma && movbe && osxsave && avx && f16c)) return false;
  // The OS saves the AVX registers (XCR0 bits 1 and 2).
  unsigned xcr0_low = 0, xcr0_high = 0;
  __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
  if ((xcr0_low & 6) != 6) return false;
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
  const bool bmi1 = b & (1u << 3), avx2 = b & (1u << 5), bmi2 = b & (1u << 8);
  if (!(bmi1 && avx2 && bmi2)) return false;
  if (!__get_cpuid(0x80000001, &a, &b, &c, &d)) return false;
  const bool lzcnt = c & (1u << 5);
  return lzcnt;
}

void CheckCpu() {
  if (!HasX86_64_V3()) {
    MessageBoxW(nullptr,
                L"This build needs a CPU with AVX2 and FMA (x86-64-v3): Intel Haswell (2013) or "
                L"AMD Excavator (2015) and newer, or a Steam Deck.\n\nThis CPU doesn't have them, "
                L"so the game can't run.",
                L"Ghost Recon Advanced Warfighter", MB_OK | MB_ICONERROR);
    ExitProcess(1);
  }
}

}  // namespace

// Before the C++ static initializers (.CRT$XCU): the C runtime calls every pointer in .CRT$XC*,
// in section-name order. Kept by the linker although nothing references it.
#pragma section(".CRT$XCB", read)
extern "C" __declspec(allocate(".CRT$XCB")) __attribute__((used)) void (*const graw_cpu_check)() =
    CheckCpu;
#pragma comment(linker, "/include:graw_cpu_check")

#endif
