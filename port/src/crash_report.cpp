// GRAW 360 - crash reports. See crash_report.h.

#include "crash_report.h"

#include <rex/logging.h>

#if defined(_WIN32)

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// After windows.h.
#include <dbghelp.h>

namespace graw::crash_report {
namespace {

std::wstring report_dir;
volatile LONG reporting = 0;

std::wstring Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t text[32];
  swprintf(text, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
           t.wSecond);
  return text;
}

// "graw.exe+0x1234" for an address in a loaded module.
std::string ModuleOffset(DWORD64 address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module)) {
    return "?";
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* name = std::strrchr(path, '\\');
  char text[MAX_PATH + 32];
  std::snprintf(text, sizeof(text), "%s+0x%llX", name ? name + 1 : path,
                static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
  return text;
}

void WriteStack(FILE* out, CONTEXT context) {
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset) {
      break;
    }
    const DWORD64 pc = frame.AddrPC.Offset;
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    const bool named = SymFromAddr(process, pc, &displacement, symbol);
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    const bool has_line = SymGetLineFromAddr64(process, pc, &line_displacement, &line);
    std::fprintf(out, "  #%02d %016llX %s %s+0x%llX%s%s:%lu\n", i,
                 static_cast<unsigned long long>(pc), ModuleOffset(pc).c_str(),
                 named ? symbol->Name : "?", static_cast<unsigned long long>(displacement),
                 has_line ? "  " : "", has_line ? line.FileName : "",
                 has_line ? line.LineNumber : 0ul);
  }
  SymCleanup(process);
}

void WriteDump(const std::wstring& path, EXCEPTION_POINTERS* pointers) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), pointers, FALSE};
  // Not the full memory: the guest's 4 GB address space would make it enormous.
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                  MiniDumpWithUnloadedModules),
                    pointers ? &info : nullptr, nullptr, nullptr);
  CloseHandle(file);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  if (InterlockedExchange(&reporting, 1)) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
  const DWORD64 address = reinterpret_cast<DWORD64>(record.ExceptionAddress);
  const std::wstring base = report_dir + L"\\crash-" + Stamp();
  REXLOG_CRITICAL("GRAW: crash: exception {:08X} at {} on thread {}; report in {}.txt",
                  static_cast<unsigned>(record.ExceptionCode), ModuleOffset(address),
                  GetCurrentThreadId(), std::filesystem::path(base).string());
  rex::FlushLogging();
  WriteDump(base + L".dmp", pointers);
  if (FILE* out = _wfopen((base + L".txt").c_str(), L"w")) {
    std::fprintf(out, "Exception %08X at %016llX (%s), thread %lu\n",
                 static_cast<unsigned>(record.ExceptionCode),
                 static_cast<unsigned long long>(address), ModuleOffset(address).c_str(),
                 GetCurrentThreadId());
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
      std::fprintf(out, "Access violation: %s %016llX\n",
                   record.ExceptionInformation[0] == 0   ? "reading"
                   : record.ExceptionInformation[0] == 1 ? "writing"
                                                         : "executing",
                   static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }
    if (record.ExceptionCode == EXCEPTION_BREAKPOINT) {
      std::fprintf(out, "A breakpoint: the SDK's rex::debug::Break(), e.g. the game called "
                        "KeBugCheck, or an assertion.\n");
    }
    const CONTEXT& c = *pointers->ContextRecord;
    std::fprintf(out,
                 "rax %016llX rbx %016llX rcx %016llX rdx %016llX\n"
                 "rsi %016llX rdi %016llX rbp %016llX rsp %016llX\n"
                 "r8  %016llX r9  %016llX r10 %016llX r11 %016llX\n"
                 "r12 %016llX r13 %016llX r14 %016llX r15 %016llX\n",
                 c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10,
                 c.R11, c.R12, c.R13, c.R14, c.R15);
    // The callee-saved vector registers: the generated code keeps values in them across its host
    // calls (docs/HANDOFF-REGOPTS.md, section 7), and under Wine the dump viewer is not at hand.
    const M128A* xmm = &c.Xmm6;
    for (int i = 0; i < 10; i += 2) {
      std::fprintf(out, "xmm%-2d %016llX_%016llX xmm%-2d %016llX_%016llX\n", 6 + i,
                   static_cast<unsigned long long>(xmm[i].High),
                   static_cast<unsigned long long>(xmm[i].Low), 7 + i,
                   static_cast<unsigned long long>(xmm[i + 1].High),
                   static_cast<unsigned long long>(xmm[i + 1].Low));
    }
    std::fprintf(out, "mxcsr %08lX\n", static_cast<unsigned long>(c.MxCsr));
    // Wine (Proton) names itself through ntdll; the logs don't say which build ran.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
      using GetVersion = const char*(__cdecl*)();
      auto version = reinterpret_cast<GetVersion>(GetProcAddress(ntdll, "wine_get_version"));
      auto build = reinterpret_cast<GetVersion>(GetProcAddress(ntdll, "wine_get_build_id"));
      if (version) {
        std::fprintf(out, "Wine %s (%s)\n", version(), build ? build() : "no build id");
      }
    }
    std::fprintf(out, "Stack:\n");
    WriteStack(out, c);
    std::fclose(out);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void OnExit() {
  REXLOG_WARN("GRAW: the process is ending through exit() (e.g. the game's "
              "HalReturnToFirmware)");
  rex::FlushLogging();
}

void OnAbort(int) {
  REXLOG_CRITICAL("GRAW: abort() called");
  rex::FlushLogging();
  // Make it an exception, so the report and dump are written.
  RaiseException(0xE0000ABD, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

}  // namespace

void InstallCrashReporter() {
  std::error_code error;
  const auto dir = std::filesystem::current_path(error) / "logs";
  std::filesystem::create_directories(dir, error);
  report_dir = dir.wstring();
  SetUnhandledExceptionFilter(OnUnhandledException);
  std::atexit(OnExit);
  std::signal(SIGABRT, OnAbort);
  REXLOG_INFO("GRAW: crash reports go to {}", dir.string());
}

}  // namespace graw::crash_report

#else

namespace graw::crash_report {
void InstallCrashReporter() {}
}  // namespace graw::crash_report

#endif
