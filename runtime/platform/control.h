#pragma once

#include "lib/common/types.h"

#include <filesystem>

// Remote control for unattended play (--control <dir>): the game runs without a window, as fast as
// it can, through the commands it is given, then waits for more. Commands arrive as
// <dir>/commands.txt (write it under another name and rename it, so it is never read half-written),
// one per line:
//
//   hold <buttons> <frames>    hold buttons down for that many frames, then let go
//   tap <buttons> [<count>]    press and release, 4 frames down and 12 up, <count> times (default 1)
//   wait <frames>              let the game run
//   shot                       write <dir>/shot.png, then "<number> <frame>" to <dir>/status.txt
//   quit                       end the run
//   trace <channels>           switch log channels on (hle, thread, io, ...), untrace switches them off
//   threads                    log every guest thread: state, priority, what it waits on
//   menu <tab> [y] | menu close  open the settings menu at a tab (0 General, 1 Video, ...), scrolled
//                              to y if given, or close it; needs --overlay when headless
//
// Buttons are up, down, left, right, cross, circle, square, triangle, l, r, start and select,
// joined with '+' (for example up+right).
namespace p2::host::control {

void init(const std::filesystem::path& dir);
bool active();

// Before the frame's input is read: sets the buttons for this frame, waiting for commands while
// there are none. Returns false when told to quit.
bool before_frame(u64 frame);
// After the frame is shown: writes a screenshot asked for.
void after_frame(u64 frame);

} // namespace p2::host::control
