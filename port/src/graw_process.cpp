// GRAW 360 - starting another of the disc's programs. See graw_process.h.

#include "graw_process.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <chrono>
#include <thread>

extern char** environ;
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
  std::error_code error;
  return std::filesystem::read_symlink("/proc/self/exe", error);
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
#else
  // NUL-separated, the program first.
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t start = all.find('\0');
  while (start != std::string::npos && start + 1 < all.size()) {
    const size_t end = all.find('\0', start + 1);
    out.push_back(all.substr(start + 1, end == std::string::npos ? end : end - start - 1));
    start = end;
  }
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
  arguments = WithoutFlags(arguments, {"graw_restart_wait_pid"});
  arguments.push_back("--graw_restart_wait_pid");
  arguments.push_back(std::to_string(getpid()));
  const std::string program = executable.string();
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program.c_str()));
  for (std::string& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  // The new run gets none of this one's descriptors (the log, the GPU device, the window's
  // connection) beyond stdin, stdout and stderr.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  const std::string directory = working_dir.string();
  if (!directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  pid_t pid = 0;
  const int result = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0) {
    REXLOG_ERROR("GRAW: couldn't start {} ({})", program, result);
    return false;
  }
  REXLOG_INFO("GRAW: started {} (pid {})", program, pid);
  return true;
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
#else
  const uint32_t pid = REXCVAR_GET(graw_restart_wait_pid);
  if (!pid) return;
  // Not this process's child: poll until it's gone (kill with signal 0 only checks).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (kill(pid_t(pid), 0) == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
#endif
}

}  // namespace graw::process
