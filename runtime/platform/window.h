#pragma once

#include "lib/common/types.h"

#include <string>
#include <vector>

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

// The size of the window's picture area in pixels, when it is not full screen or maximised. A
// size asked for meanwhile is applied when the window is back to normal.
void set_window_size(u32 width, u32 height);

struct WindowSize {
    u32 width, height;
    u32 multiple; // of the console's 480x272, or 0
};
// Sizes worth offering for the display the window is on: those that fit it, smallest first.
std::vector<WindowSize> window_size_choices();

// Called with the UTF-8 path of each file dropped onto the window (null: drops are ignored).
void set_drop_handler(void (*handler)(const char* path));

// True while the fast-forward key is held.
bool turbo_held();

// Buttons the program itself holds down on the player's behalf (added to the real input).
void set_injected_buttons(u32 buttons);

// Scripted input for unattended runs: "frame:buttons" pairs set the pad from that frame on.
void set_input_script(const std::string& script);
void advance_input_script(u64 frame);

} // namespace p2::host
