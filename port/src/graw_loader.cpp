// GRAW 360 - the hand-off between the disc's programs. See graw_loader.h.

#include "graw_loader.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "graw_process.h"

REXCVAR_DEFINE_STRING(graw_launch_data, "", "GRAW",
                      "Launch data handed over by the program that started this one (a file, "
                      "deleted after reading)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(graw_frontend_exe, "", "GRAW",
                      "The front end's program (default.xex's translation); empty = graw.exe in "
                      "port/out/build/<preset> next to this build")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(graw_campaign_exe, "", "GRAW",
                      "The campaign's program (gr3xenon_release.exe's translation); empty = "
                      "grawsp.exe in port-sp/out/build/<preset> next to this build")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace graw::loader {
namespace {

namespace fs = std::filesystem;

rex::kernel::xam::XamModule* Xam() {
  auto* state = rex::system::kernel_state();
  if (!state) return nullptr;
  auto xam = state->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  return xam.get();
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// The file name of a guest path ("game:\gr3xenon_release.exe" -> "gr3xenon_release.exe").
std::string GuestFileName(const std::string& path) {
  const size_t slash = path.find_last_of("\\/:");
  return Lower(slash == std::string::npos ? path : path.substr(slash + 1));
}

// A sibling build's program: this one runs from <repo>/<project>/out/build/<preset>/, so the
// other is <repo>/<other project>/out/build/<preset>/<name>.
fs::path SiblingProgram(const char* project, const char* name) {
  const fs::path own = graw::process::OwnExecutable();
  const fs::path preset_dir = own.parent_path();
  const fs::path repo = preset_dir.parent_path().parent_path().parent_path().parent_path();
  return repo / project / "out" / "build" / preset_dir.filename() / name;
}

fs::path ProgramFor(const std::string& executable) {
  if (executable == "default.xex") {
    const std::string set = REXCVAR_GET(graw_frontend_exe);
    return set.empty() ? SiblingProgram("port", "graw.exe") : fs::path(set);
  }
  if (executable == "gr3xenon_release.exe") {
    const std::string set = REXCVAR_GET(graw_campaign_exe);
    return set.empty() ? SiblingProgram("port-sp", "grawsp.exe") : fs::path(set);
  }
  return {};
}

std::atomic<bool> g_handed_off{false};

}  // namespace

void RestoreLaunchData() {
  const std::string file = REXCVAR_GET(graw_launch_data);
  if (file.empty()) return;
  std::ifstream in(fs::u8path(file), std::ios::binary);
  if (!in) {
    REXLOG_ERROR("GRAW: launch data file {} not readable", file);
    return;
  }
  uint32_t flags = 0;
  uint8_t present = 0;
  in.read(reinterpret_cast<char*>(&flags), 4);
  in.read(reinterpret_cast<char*>(&present), 1);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::error_code ignored;
  fs::remove(fs::u8path(file), ignored);
  auto* xam = Xam();
  if (!xam) {
    REXLOG_ERROR("GRAW: no XAM module to take the launch data");
    return;
  }
  auto& loader = xam->loader_data();
  loader.launch_flags = flags;
  loader.launch_data_present = present != 0;
  loader.launch_data = std::move(data);
  REXLOG_INFO("GRAW: launch data restored ({} bytes, flags {:#x})", loader.launch_data.size(),
              flags);
}

void HandOff() {
  if (g_handed_off.exchange(true)) return;
  auto* xam = Xam();
  if (!xam) return;
  const auto& loader = xam->loader_data();
  if (loader.launch_path.empty()) return;  // the game quit, or the window was closed

  const std::string executable = GuestFileName(loader.launch_path);
  fs::path program = ProgramFor(executable);
  if (program.empty()) {
    // Yeti_Final_Xenon.exe (multiplayer) has no translation yet: back to the front end.
    REXLOG_WARN("GRAW: {} requested; no translation of it yet, returning to the front end",
                loader.launch_path);
    program = ProgramFor("default.xex");
  }
  if (!fs::exists(program)) {
    REXLOG_ERROR("GRAW: {} requested, but {} is not built", loader.launch_path, program.string());
    return;
  }

  std::vector<std::string> arguments =
      graw::process::WithoutFlags(graw::process::OwnArguments(), {"graw_launch_data"});
  if (loader.launch_data_present || loader.launch_flags) {
    const fs::path file = fs::temp_directory_path() /
                          ("graw360-launch-" + std::to_string(std::rand()) + ".bin");
    std::ofstream out(file, std::ios::binary);
    const uint32_t flags = loader.launch_flags;
    const uint8_t present = loader.launch_data_present ? 1 : 0;
    out.write(reinterpret_cast<const char*>(&flags), 4);
    out.write(reinterpret_cast<const char*>(&present), 1);
    out.write(reinterpret_cast<const char*>(loader.launch_data.data()),
              static_cast<std::streamsize>(loader.launch_data.size()));
    out.close();
    arguments.push_back("--graw_launch_data");
    arguments.push_back(file.string());
  }
  REXLOG_INFO("GRAW: {} requested ({} bytes of launch data): starting {}", loader.launch_path,
              loader.launch_data.size(), program.string());
  graw::process::StartAfterThis(program, arguments, fs::current_path());
}

}  // namespace graw::loader
