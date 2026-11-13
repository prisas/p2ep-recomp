#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace p2::gfx {
class Renderer;
}

// Everything drawn on top of the game picture: the settings menu, the system dialogs the console
// would show (save-slot lists, messages), short notices and the performance display.
namespace p2::ui {

// Creates the overlay. `sdl_window` may be null (unattended runs), in which case the overlay is
// driven only by the pad state and drawn at `width` x `height`.
bool init(void* sdl_window, u32 width, u32 height);
void shutdown();
bool active();

// Hooks the overlay into the renderer and applies the current settings to it.
void attach(gfx::Renderer* renderer);
// Pushes the settings to the renderer, the window and the audio device.
void apply_settings();

// Input. `process_event` takes an SDL_Event; it returns true when the overlay used the event
// (for example while it waits for a key to bind).
bool process_event(const void* sdl_event);
// The console buttons currently held, after key and gamepad mapping. Drives the system dialogs.
void set_pad(u32 buttons);
// True while a system dialog or the menu owns the pad: the game must see no buttons.
bool captures_pad();

// The settings menu. While it is open the game is paused.
void toggle_menu();
void open_menu(int tab);
bool menu_open();

// Per-frame statistics for the performance display.
void frame_stats(f32 frame_ms, f32 speed_percent);

// A short message that fades after a few seconds.
void notify(const std::string& text);

// --- system dialogs -----------------------------------------------------------------------------------------

constexpr int kPending = -2;
constexpr int kCancelled = -1;

struct SaveSlot {
    std::string id;       // the game's name for the slot
    bool exists = false;
    std::string title;    // from the save's description
    std::string detail;
    std::string when;     // last written
    std::filesystem::path icon;
};

enum class SlotPurpose { Load, Save, Delete };

// False when nobody can answer a dialog (no overlay): callers then pick a default themselves.
bool interactive();

void open_slot_dialog(SlotPurpose purpose, std::vector<SaveSlot> slots, int focus);
// kPending while the list is open, then the chosen index or kCancelled.
int slot_dialog_result();

void open_message(const std::string& text, bool yes_no, bool default_no);
// kPending while the message is open; then 0 (acknowledged), 1 (yes), 2 (no) or 3 (dismissed).
int message_result();

} // namespace p2::ui
