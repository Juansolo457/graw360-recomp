// GRAW 360 - a scripted virtual controller for unattended tests.
//
// With --graw_autopilot <script> the host adds one more controller to the input system. It plays
// a timetable of button presses and stick positions from the script, measured from when the
// program starts, and needs no window focus, so tests can drive the game (through menus into a
// mission) while the user keeps using the PC. With shared controllers on (the default) it drives
// player 1 alongside any real pad.
//
// Script: one event per line, '#' starts a comment.
//   <seconds> <button>[+<button>...] [hold <seconds>]       press, default hold 0.15 s
//   <seconds> stick <lx> <ly> <rx> <ry> hold <seconds>       -1.0 .. 1.0
//   <seconds> trigger <lt> <rt> hold <seconds>               0.0 .. 1.0
// Buttons: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT.
// Example (tools/autopilot/campaign-newgame.txt): from the campaign menu into the tutorial.

#pragma once

#include <memory>
#include <string>

#include <rex/input/input_driver.h>

namespace graw::autopilot {

// The driver for --graw_autopilot, or null when the option is empty or the script is unreadable.
std::unique_ptr<rex::input::InputDriver> Create();

}  // namespace graw::autopilot
