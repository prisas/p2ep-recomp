#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <string>

// User settings: what the overlay edits and what survives between runs in <user dir>/settings.ini.
namespace p2::host {

enum class Scaling : u32 { Fit, Integer, Stretch };

constexpr u32 kPadButtonCount = 12;
// Order of the bindable buttons, shared by the settings file, the overlay and the input code.
extern const u32 kPadButtonBits[kPadButtonCount];
extern const char* const kPadButtonNames[kPadButtonCount];

struct Settings {
    // general
    std::string image;          // the disc image last chosen in the launcher, as UTF-8 text
    bool show_launcher = true;  // started without an image, show the launcher (else play `image` at once)
    bool skip_intro = false;    // start at the main menu: notices, logos and the opening movie skipped

    // gameplay
    u32 encounter_rate = 100;   // percent of the game's own chance of random battles, 0-200 (see runtime/game)

    // video
    u32 scale = 0;              // internal resolution multiplier, 1-10; 0 follows the window size
    u32 window_width = 960;     // size of the window's picture area in pixels, when not full screen
    u32 window_height = 544;
    bool fullscreen = false;
    bool vsync = true;
    Scaling scaling = Scaling::Fit;
    bool smooth = true;         // bilinear filtering when the picture is scaled to the window
    bool show_fps = false;
    bool replace_textures = true; // draw with a texture pack's files where there are any
    bool dump_textures = false;   // write each texture the game uses to the dump folder

    // audio
    u32 volume = 100;           // percent
    bool reverb = false;        // the synthesizer's reverb on sound effects (an approximation; off by default)

    // input
    u32 turbo_speed = 0;        // fast-forward limit as a multiple of normal speed; 0 is unlimited
    u32 keys[kPadButtonCount];  // SDL scancodes, in kPadButtonBits order

    // For this run only, from the command line; never written to the settings file.
    u32 run_scale = 0;          // overrides `scale` when not zero
    bool run_unthrottled = false; // no waiting for the display
    bool run_skip_intro = false;  // skip the intro this time, whatever `skip_intro` says
    bool run_dump_textures = false; // dump textures this time, whatever `dump_textures` says
    u32 run_window_width = 0, run_window_height = 0; // window size this time (0: the setting)

    Settings();
    void reset_keys();
};

Settings& settings();

// Reads the settings file if there is one; unknown or damaged entries keep their defaults.
void load_settings(const std::filesystem::path& file);
// Writes the settings back to the file they were loaded from. Returns false if that fails.
bool save_settings();

} // namespace p2::host
