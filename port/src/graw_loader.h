// GRAW 360 - the disc's three programs and the hand-off between them.
//
// default.xex is the front end (start screen, menus). Campaign calls XamLoaderLaunchTitle with
// "gr3xenon_release.exe" and launch data (sub_82170EE0 -> sub_822012A8), Multiplayer launches
// "Yeti_Final_Xenon.exe", and the campaign program launches "default.xex" to go back. On a
// console the loader replaces the running title. ReXGlue ends the process instead: it records the
// request in XamModule::LoaderData and nothing reads it. Here each program is its own
// translation (port/ -> graw.exe, port-sp/ -> grawsp.exe), so:
//  - HandOff(), when the main guest thread has ended: if a launch was requested, save the launch
//    data and flags to a file and start the program holding that executable's translation, with
//    this run's arguments, after this process exits (graw_process.h).
//  - RestoreLaunchData(), before the new program's module starts: put the saved launch data back
//    into XamModule::LoaderData so XamLoaderGetLaunchData returns what the front end set.

#pragma once

namespace graw::loader {

// OnPreLaunchModule: load --graw_launch_data (if given) into the XAM loader data, then delete it.
void RestoreLaunchData();

// OnGuestThreadExit for the main thread: start the requested program, if any.
void HandOff();

}  // namespace graw::loader
