// GRAW 360 - the disc's three programs and the hand-off between them.
//
//   default.xex           the front end (start screen, menus)             port/      graw.exe
//   gr3xenon_release.exe  Red Storm's GR3 engine: Instant Action, co-op   port-mp/   grawmp.exe
//                         and multiplayer
//   yeti_final_xenon.exe  Ubisoft Paris's Yeti engine: the campaign       port-yeti/ grawyeti.exe
//
// The front end starts the others with XamLoaderLaunchTitle (sub_822012A8) after setting launch
// data, and they return with "default.xex". On a console the loader replaces the running title.
// ReXGlue ends the process instead: it records the request in XamModule::LoaderData and nothing
// reads it. Here each program is its own translation, so:
//  - HandOff(), when the main guest thread has ended: if a launch was requested, save the launch
//    data and flags to a file and start the program holding that executable's translation, with
//    this run's arguments and its own log file, after this process exits (graw_process.h).
//  - RestoreLaunchData(), before the new program's module starts: put the saved launch data back
//    into XamModule::LoaderData so XamLoaderGetLaunchData returns what the previous program set.
// A program that isn't built yet sends the player back to the front end.

#pragma once

namespace graw::loader {

// OnPreLaunchModule: load --graw_launch_data (if given) into the XAM loader data, then delete it.
void RestoreLaunchData();

// OnGuestThreadExit for the main thread: start the requested program, if any.
void HandOff();

}  // namespace graw::loader
