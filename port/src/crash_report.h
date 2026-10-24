// GRAW 360 - crash reports.
//
// Windows Error Reporting may be off on the player's machine, and the SDK's exception handler
// passes anything it doesn't handle on, so a crash used to end the process with nothing in the
// log. InstallCrashReporter() writes, into logs/ under the working directory:
//   crash-<time>.txt  the exception, the faulting module and offset, the thread's stack with
//                     symbols (recompiled functions are named sub_<360 address>), and a note
//                     in the game log;
//   crash-<time>.dmp  a small minidump (threads, stacks and the memory they point at).
// It also logs an exit() (the game's HalReturnToFirmware ends the process that way) and an
// abort(), so no way out of the process is silent.

#pragma once

namespace graw::crash_report {

void InstallCrashReporter();

}  // namespace graw::crash_report
