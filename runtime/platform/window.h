#pragma once

#include "lib/common/types.h"

#include <string>

// The host window and the controller state derived from keyboard and gamepad.
namespace p2::host {

// PSP button bits as reported by sceCtrl.
enum : u32 {
    kPadSelect = 0x000001,
    kPadStart = 0x000008,
    kPadUp = 0x000010,
    kPadRight = 0x000020,
    kPadDown = 0x000040,
    kPadLeft = 0x000080,
    kPadL = 0x000100,
    kPadR = 0x000200,
    kPadTriangle = 0x001000,
    kPadCircle = 0x002000,
    kPadCross = 0x004000,
    kPadSquare = 0x008000,
};

struct PadState {
    u32 buttons = 0;
    u8 lx = 128;
    u8 ly = 128;
};

// Creates the window unless running headless. Safe to call once.
bool window_init(std::string* error);
void window_shutdown();
void* window_handle(); // SDL_Window*, or null when headless

// Processes pending window and input events. Returns false once the user asked to quit.
bool pump_events();
PadState pad_state();

// Makes the next pump_events() report that the user wants to quit.
void request_quit();
void set_fullscreen(bool on);

// True while the fast-forward key is held.
bool turbo_held();

// Scripted input for unattended runs: "frame:buttons" pairs set the pad from that frame on.
void set_input_script(const std::string& script);
void advance_input_script(u64 frame);

} // namespace p2::host
