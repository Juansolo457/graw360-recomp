// GRAW 360 - profile-guided optimization, the training run (from fear360-recomp).
//
// tools/build-linux.sh with GRAW_PGO=generate builds the game instrumented (-fprofile-generate)
// and defines GRAW_PGO_GENERATE. The profile runtime writes its counters at exit, but the game
// never exits normally: ReXGlue's window close ends in std::_Exit (rex_app.cpp, OnClosing), and
// a hand-over (XamLoaderLaunchTitle) starts the other program and leaves the same way. So a thread writes them every
// 20 s instead, each time the whole of this process's counts so far, into its own file
// (pgo-raw/<pid>.profraw beside the executable). The runtime opens the file for appending, so
// each snapshot goes to an emptied temporary file first and then replaces the last one: the file
// always holds one snapshot, the session's total so far. A session loses at most its last 20 s.
// The folder is made here: the runtime doesn't make it for a name set this way (the first Deck
// training in F.E.A.R. lost its counts to that).
// tools/pgo-merge.sh merges the files into pgo/<project>/default.profdata, which the next build
// uses.
#if defined(GRAW_PGO_GENERATE)

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <unistd.h>

extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char* name);

namespace {

struct ProfileWriter {
  ProfileWriter() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::read_symlink("/proc/self/exe", ec);
    dir = ec ? std::filesystem::path("pgo-raw") : dir.parent_path() / "pgo-raw";
    std::filesystem::create_directories(dir, ec);
    const std::string pid = std::to_string(getpid());
    // The runtime keeps the pointer: strings that live as long as the process.
    static const std::filesystem::path temp = dir / (pid + ".profraw.tmp");
    static const std::filesystem::path done = dir / (pid + ".profraw");
    static const std::string temp_name = temp.string();
    __llvm_profile_set_filename(temp_name.c_str());
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::error_code e;
        std::filesystem::remove(temp, e);
        if (__llvm_profile_write_file() == 0) std::filesystem::rename(temp, done, e);
      }
    }).detach();
  }
};

const ProfileWriter g_writer;

}  // namespace

#endif
