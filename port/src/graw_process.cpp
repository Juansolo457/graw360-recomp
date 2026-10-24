// GRAW 360 - starting another of the disc's programs. See graw_process.h.

#include "graw_process.h"

#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

REXCVAR_DEFINE_UINT32(graw_restart_wait_pid, 0, "GRAW",
                      "Set when one of the disc's programs starts another: wait for this "
                      "process to exit before starting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace graw::process {
namespace {

#if defined(_WIN32)
std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::string out(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr,
                      nullptr);
  return out;
}

// One argument as CommandLineToArgvW reads it back.
std::wstring Quote(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}
#endif

}  // namespace

std::filesystem::path OwnExecutable() {
#if defined(_WIN32)
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return length ? std::filesystem::path(std::wstring(path, length)) : std::filesystem::path();
#else
  return {};
#endif
}

std::vector<std::string> OwnArguments() {
  std::vector<std::string> out;
#if defined(_WIN32)
  int count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) return out;
  for (int i = 1; i < count; ++i) out.push_back(Narrow(arguments[i]));
  LocalFree(arguments);
#endif
  return out;
}

std::vector<std::string> WithoutFlags(const std::vector<std::string>& arguments,
                                      const std::vector<std::string>& flags) {
  std::vector<std::string> out;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument.rfind("--", 0) == 0) {
      std::string name = argument.substr(2);
      const size_t equals = name.find('=');
      const bool inline_value = equals != std::string::npos;
      if (inline_value) name.resize(equals);
      const bool negated = name.rfind("no-", 0) == 0;
      if (negated) name = name.substr(3);
      const auto* info = rex::cvar::GetFlagInfo(name);
      const bool takes_value = info && info->type != rex::cvar::FlagType::Boolean &&
                               !inline_value && !negated && i + 1 < arguments.size();
      if (std::find(flags.begin(), flags.end(), name) != flags.end()) {
        if (takes_value) ++i;
        continue;
      }
      if (name == "log_file" && takes_value) {
        std::filesystem::path log(arguments[++i]);
        std::string stem = log.stem().string();
        if (!stem.ends_with("-restart")) stem += "-restart";
        log.replace_filename(stem + log.extension().string());
        out.push_back(argument);
        out.push_back(log.string());
        continue;
      }
    }
    out.push_back(argument);
  }
  return out;
}

bool StartAfterThis(const std::filesystem::path& executable, std::vector<std::string> arguments,
                    const std::filesystem::path& working_dir) {
#if defined(_WIN32)
  arguments = WithoutFlags(arguments, {"graw_restart_wait_pid"});
  arguments.push_back("--graw_restart_wait_pid");
  arguments.push_back(std::to_string(GetCurrentProcessId()));
  std::wstring command = Quote(executable.wstring());
  for (const std::string& argument : arguments) command += L" " + Quote(Wide(argument));

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring directory = working_dir.wstring();
  if (!CreateProcessW(executable.wstring().c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, directory.empty() ? nullptr : directory.c_str(), &startup,
                      &process)) {
    REXLOG_ERROR("GRAW: couldn't start {} ({})", executable.string(), GetLastError());
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  REXLOG_INFO("GRAW: started {}", executable.string());
  return true;
#else
  (void)executable;
  (void)arguments;
  (void)working_dir;
  return false;
#endif
}

void WaitForPreviousRun() {
#if defined(_WIN32)
  const uint32_t pid = REXCVAR_GET(graw_restart_wait_pid);
  if (!pid) return;
  if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
    WaitForSingleObject(process, 30000);
    CloseHandle(process);
  }
#endif
}

}  // namespace graw::process
