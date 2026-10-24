// GRAW 360 - starting another of the disc's programs after this one ends (graw_loader.h: the
// front end handing over to the other programs, and back). From fear360-recomp (MIT, same author).
//
// A new run is told the old one's process id (graw_restart_wait_pid) and waits for it to exit
// before it starts (WaitForPreviousRun, from ReXApp::OnConfigurePaths), so the two never hold the
// display, the save files or the log at the same time.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace graw::process {

// This program and the arguments it was started with (UTF-8, without the program).
std::filesystem::path OwnExecutable();
std::vector<std::string> OwnArguments();

// The arguments without the given flags (as --name value, --name=value, --name or --no-name).
// The caller names the next run's log (graw_loader.cpp, RenameLog), so it doesn't write over this
// one's.
std::vector<std::string> WithoutFlags(const std::vector<std::string>& arguments,
                                      const std::vector<std::string>& flags);

// Starts a program that waits for this run to exit (the arguments get --graw_restart_wait_pid).
// False if it couldn't be started.
bool StartAfterThis(const std::filesystem::path& executable, std::vector<std::string> arguments,
                    const std::filesystem::path& working_dir);

// Blocks until the run named by graw_restart_wait_pid has exited (at most 30 s).
void WaitForPreviousRun();

}  // namespace graw::process
