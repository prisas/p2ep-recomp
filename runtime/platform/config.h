#pragma once

#include "lib/common/types.h"

#include <filesystem>

// User settings: what the overlay edits and what survives between runs in <user dir>/settings.ini.
namespace p2::host {

enum class Scaling : u32 { Fit, Integer, Stretch };

constexpr u32 kPadButtonCount = 12;
// Order of the bindable buttons, shared by the settings file, the overlay and the input code.
extern const u32 kPadButtonBits[kPadButtonCount];
extern const char* const kPadButtonNames[kPadButtonCount];

struct Settings {
    // video
    u32 scale = 0;              // internal resolution multiplier, 1-10; 0 follows the window size
    bool fullscreen = false;
    bool vsync = true;
    Scaling scaling = Scaling::Fit;
    bool smooth = true;         // bilinear filtering when the picture is scaled to the window
    bool show_fps = false;

    // audio
    u32 volume = 100;           // percent

    // input
    u32 turbo_speed = 0;        // fast-forward limit as a multiple of normal speed; 0 is unlimited
    u32 keys[kPadButtonCount];  // SDL scancodes, in kPadButtonBits order

    Settings();
    void reset_keys();
};

Settings& settings();

// Reads the settings file if there is one; unknown or damaged entries keep their defaults.
void load_settings(const std::filesystem::path& file);
// Writes the settings back to the file they were loaded from. Returns false if that fails.
bool save_settings();

} // namespace p2::host
