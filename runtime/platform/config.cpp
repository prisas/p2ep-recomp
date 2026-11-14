#include "runtime/platform/config.h"

#include "lib/common/file.h"
#include "runtime/platform/window.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace p2::host {

const u32 kPadButtonBits[kPadButtonCount] = {kPadUp,     kPadDown,     kPadLeft, kPadRight, kPadCross, kPadCircle,
                                             kPadSquare, kPadTriangle, kPadL,    kPadR,     kPadStart, kPadSelect};
const char* const kPadButtonNames[kPadButtonCount] = {"Up",     "Down",     "Left", "Right", "Cross", "Circle",
                                                      "Square", "Triangle", "L",    "R",     "Start", "Select"};

namespace {

// Keys in the settings file for the bindings, in kPadButtonBits order.
constexpr const char* kKeyNames[kPadButtonCount] = {"key_up",     "key_down",     "key_left", "key_right",
                                                    "key_cross",  "key_circle",   "key_square", "key_triangle",
                                                    "key_l",      "key_r",        "key_start", "key_select"};

Settings g_settings;
std::filesystem::path g_file;

} // namespace

Settings::Settings() { reset_keys(); }

void Settings::reset_keys() {
    // Arrows move; Z/X/A/S are cross/circle/square/triangle.
    static constexpr u32 defaults[kPadButtonCount] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_Z,      SDL_SCANCODE_X,
        SDL_SCANCODE_A,  SDL_SCANCODE_S,    SDL_SCANCODE_Q,    SDL_SCANCODE_W,     SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE};
    std::copy(std::begin(defaults), std::end(defaults), keys);
}

Settings& settings() { return g_settings; }

void load_settings(const std::filesystem::path& file) {
    g_file = file;
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return;
    const std::string text(bytes.begin(), bytes.end());

    Settings& s = g_settings;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        auto trim = [](std::string v) {
            const size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
        };
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (value.empty()) continue;
        const u32 n = u32(std::strtoul(value.c_str(), nullptr, 10));

        if (key == "skip_intro") s.skip_intro = n != 0;
        else if (key == "scale") s.scale = std::min<u32>(n, 10);
        else if (key == "fullscreen") s.fullscreen = n != 0;
        else if (key == "vsync") s.vsync = n != 0;
        else if (key == "scaling") s.scaling = Scaling(std::min<u32>(n, 2));
        else if (key == "smooth") s.smooth = n != 0;
        else if (key == "show_fps") s.show_fps = n != 0;
        else if (key == "volume") s.volume = std::min<u32>(n, 100);
        else if (key == "reverb") s.reverb = n != 0;
        else if (key == "turbo_speed") s.turbo_speed = std::min<u32>(n, 16);
        else {
            for (u32 i = 0; i < kPadButtonCount; ++i)
                if (key == kKeyNames[i] && n > 0 && n < SDL_SCANCODE_COUNT) s.keys[i] = n;
        }
    }
}

bool save_settings() {
    if (g_file.empty()) return false;
    const Settings& s = g_settings;
    std::string out = "# Persona 2: Eternal Punishment - settings. Edited by the in-game overlay.\n";
    append_format(out, "skip_intro = %u\n", u32(s.skip_intro));
    append_format(out, "scale = %u\n", s.scale);
    append_format(out, "fullscreen = %u\n", u32(s.fullscreen));
    append_format(out, "vsync = %u\n", u32(s.vsync));
    append_format(out, "scaling = %u\n", u32(s.scaling));
    append_format(out, "smooth = %u\n", u32(s.smooth));
    append_format(out, "show_fps = %u\n", u32(s.show_fps));
    append_format(out, "volume = %u\n", s.volume);
    append_format(out, "reverb = %u\n", u32(s.reverb));
    append_format(out, "turbo_speed = %u\n", s.turbo_speed);
    for (u32 i = 0; i < kPadButtonCount; ++i) append_format(out, "%s = %u\n", kKeyNames[i], s.keys[i]);
    std::error_code ec;
    std::filesystem::create_directories(g_file.parent_path(), ec);
    return write_file(g_file, out.data(), out.size());
}

} // namespace p2::host
