#include "runtime/platform/window.h"

#include "lib/common/log.h"
#include "runtime/platform/config.h"
#include "runtime/platform/host.h"
#include "runtime/ui/overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <windows.h>

namespace p2::host {

namespace {

SDL_Window* g_window = nullptr;
SDL_Gamepad* g_gamepad = nullptr;
PadState g_pad;
bool g_turbo = false;
bool g_sdl_ready = false;
std::vector<std::pair<u64, u32>> g_script; // (frame, buttons), sorted
u32 g_script_buttons = 0;
u32 g_injected = 0;

bool g_quit = false;
u32 g_held_over = 0; // buttons still down from before the overlay gave the pad back

struct PadBinding {
    SDL_GamepadButton button;
    u32 pad;
};

constexpr PadBinding kPadButtons[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
    {SDL_GAMEPAD_BUTTON_SOUTH, kPadCross},      {SDL_GAMEPAD_BUTTON_EAST, kPadCircle},
    {SDL_GAMEPAD_BUTTON_WEST, kPadSquare},      {SDL_GAMEPAD_BUTTON_NORTH, kPadTriangle},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadR},
    {SDL_GAMEPAD_BUTTON_START, kPadStart},      {SDL_GAMEPAD_BUTTON_BACK, kPadSelect},
};

u8 axis_to_byte(Sint16 value) {
    int v = (int(value) + 32768) >> 8;
    return u8(std::clamp(v, 0, 255));
}

void open_first_gamepad() {
    if (g_gamepad) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) {
        g_gamepad = SDL_OpenGamepad(ids[0]);
        if (g_gamepad) LOG_INFO("input", "using gamepad '%s'", SDL_GetGamepadName(g_gamepad));
    }
    SDL_free(ids);
}

void route_pad(PadState pad);

// --- window size ----------------------------------------------------------------------------------------------
// Sizes are the picture area in pixels. The size last asked for is applied as soon as the window is
// neither full screen nor maximised; a size the player sets by dragging the border becomes the
// setting, and is saved shortly after the dragging stops.

struct Size {
    u32 width = 0, height = 0;
    bool operator==(const Size&) const = default;
};
Size g_requested;    // the size last asked for, which may be larger than the display allows
Size g_applied;      // what the window was last given or last measured at
Size g_wanted;       // asked for while the window could not take it (0: nothing pending)
u64 g_save_due = 0;  // when to save a size the player dragged to (SDL ticks; 0: nothing to save)

bool window_is_normal() {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
    return !(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED));
}

f32 pixel_density() {
    const f32 density = SDL_GetWindowPixelDensity(g_window);
    return density > 0.0f ? density : 1.0f;
}

// The area a window's picture can have on its display, in pixels.
Size usable_area() {
    SDL_Rect bounds{};
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &bounds)) return {16384, 16384};
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(g_window, &top, &left, &bottom, &right);
    const f32 density = pixel_density();
    return {u32(std::max(0, bounds.w - left - right) * density), u32(std::max(0, bounds.h - top - bottom) * density)};
}

void apply_window_size(u32 width, u32 height) {
    if (!window_is_normal()) {
        g_wanted = {width, height};
        return;
    }
    g_wanted = {};
    // Never larger than the display can show; the console's shape is kept when shrinking.
    const Size area = usable_area();
    if (width > area.width || height > area.height) {
        const f32 fit = std::min(f32(area.width) / f32(width), f32(area.height) / f32(height));
        width = std::max<u32>(u32(f32(width) * fit), 320);
        height = std::max<u32>(u32(f32(height) * fit), 180);
    }
    const f32 density = pixel_density();
    const int w_units = int(std::lround(f32(width) / density)), h_units = int(std::lround(f32(height) / density));
    SDL_SetWindowSize(g_window, w_units, h_units);
    // Centred in the display's usable area with its frame included, title bar never above the top.
    SDL_Rect bounds{};
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_window), &bounds)) {
        int top = 0, left = 0, bottom = 0, right = 0;
        SDL_GetWindowBordersSize(g_window, &top, &left, &bottom, &right);
        const int x = bounds.x + left + (bounds.w - (w_units + left + right)) / 2;
        const int y = bounds.y + top + (bounds.h - (h_units + top + bottom)) / 2;
        SDL_SetWindowPosition(g_window, std::max(x, bounds.x + left), std::max(y, bounds.y + top));
    } else {
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_SyncWindow(g_window);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    g_applied = {u32(w), u32(h)};
}

// The window's size changed: remember it if the player set it.
void window_resized() {
    if (!window_is_normal()) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    const Size now{u32(w), u32(h)};
    if (now == g_applied || !w || !h) return;
    g_applied = g_requested = now;
    Settings& s = settings();
    s.window_width = now.width;
    s.window_height = now.height;
    s.run_window_width = s.run_window_height = 0;
    g_save_due = SDL_GetTicks() + 1000;
}

} // namespace

void set_window_size(u32 width, u32 height) {
    // Other settings changing must not move the window, nor resize one shrunk to fit the display.
    if (!g_window || Size{width, height} == g_requested) return;
    g_requested = {width, height};
    apply_window_size(width, height);
}

std::vector<WindowSize> window_size_choices() {
    std::vector<WindowSize> all;
    for (u32 k = 1; k <= 8; ++k) all.push_back({480 * k, 272 * k, k});
    for (const auto& [w, h] : {std::pair{1280u, 720u}, {1600u, 900u}, {1920u, 1080u}, {2560u, 1440u}, {3840u, 2160u}})
        all.push_back({w, h, 0});
    std::sort(all.begin(), all.end(), [](const WindowSize& a, const WindowSize& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    if (!g_window) return all;
    const Size area = usable_area();
    std::erase_if(all, [&](const WindowSize& s) { return s.width > area.width || s.height > area.height; });
    return all;
}

bool window_init(std::string* error) {
    if (options().headless) return true;
    // When the program carries an icon (resource 1, see runtime/CMakeLists.txt), the window's title
    // bar and taskbar button use it too.
    if (FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */)) {
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        if (error) *error = std::string("cannot initialise SDL: ") + SDL_GetError();
        return false;
    }
    g_sdl_ready = true;
    const Settings& s = settings();
    const u32 width = s.run_window_width ? s.run_window_width : s.window_width;
    const u32 height = s.run_window_width ? s.run_window_height : s.window_height;
    g_window = SDL_CreateWindow("Persona 2: Eternal Punishment", int(width), int(height),
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        if (error) *error = std::string("cannot create the window: ") + SDL_GetError();
        return false;
    }
    // The size is in pixels; on a display that scales the desktop, the window's own units differ.
    g_applied = {0, 0};
    g_requested = {width, height};
    apply_window_size(width, height);
    open_first_gamepad();
    return true;
}

void window_shutdown() {
    if (g_save_due) save_settings(); // a size dragged to just before quitting
    g_save_due = 0;
    if (g_gamepad) SDL_CloseGamepad(g_gamepad);
    if (g_window) SDL_DestroyWindow(g_window);
    if (g_sdl_ready) SDL_Quit();
    g_gamepad = nullptr;
    g_window = nullptr;
    g_sdl_ready = false;
}

void* window_handle() { return g_window; }

namespace {

// Hands the pad to the overlay while it has a dialog or the menu up; the game then sees nothing,
// and afterwards not the buttons that were used to close it until they are released.
void route_pad(PadState pad) {
    ui::set_pad(pad.buttons);
    if (ui::captures_pad()) {
        g_held_over = pad.buttons;
        pad = PadState{};
    } else {
        g_held_over &= pad.buttons;
        pad.buttons &= ~g_held_over;
    }
    g_pad = pad;
}

} // namespace

void request_quit() { g_quit = true; }

void set_fullscreen(bool on) {
    if (!g_window) return;
    const bool now = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (now != on) SDL_SetWindowFullscreen(g_window, on);
}

void set_injected_buttons(u32 buttons) { g_injected = buttons; }

bool pump_events() {
    PadState pad;
    pad.buttons = g_script_buttons | g_injected;
    if (!g_sdl_ready) {
        route_pad(pad);
        return !g_quit;
    }

    bool keep_running = !g_quit;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (ui::process_event(&event)) continue;
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: keep_running = false; break;
        case SDL_EVENT_GAMEPAD_ADDED: open_first_gamepad(); break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_gamepad && SDL_GetGamepadID(g_gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(g_gamepad);
                g_gamepad = nullptr;
                open_first_gamepad();
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat) break;
            if (event.key.scancode == SDL_SCANCODE_F11 || (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT))) {
                settings().fullscreen = !settings().fullscreen;
                set_fullscreen(settings().fullscreen);
                save_settings();
            } else if (event.key.scancode == SDL_SCANCODE_ESCAPE || event.key.scancode == SDL_SCANCODE_F1) {
                ui::toggle_menu();
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) ui::toggle_menu();
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: window_resized(); break;
        default: break;
        }
    }
    if (g_wanted.width && window_is_normal()) apply_window_size(g_wanted.width, g_wanted.height);
    if (g_save_due && SDL_GetTicks() >= g_save_due) {
        g_save_due = 0;
        save_settings();
    }

    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    const Settings& s = settings();
    for (u32 i = 0; i < kPadButtonCount; ++i)
        if (keys[s.keys[i]] && !(alt && s.keys[i] == SDL_SCANCODE_RETURN)) pad.buttons |= kPadButtonBits[i];
    g_turbo = keys[SDL_SCANCODE_TAB];

    if (g_gamepad) {
        for (const PadBinding& b : kPadButtons)
            if (SDL_GetGamepadButton(g_gamepad, b.button)) pad.buttons |= b.pad;
        pad.lx = axis_to_byte(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        pad.ly = axis_to_byte(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY));
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) g_turbo = true;
    }
    route_pad(pad);
    return keep_running && !g_quit;
}

PadState pad_state() { return g_pad; }
bool turbo_held() { return g_turbo; }

void set_input_script(const std::string& script) {
    g_script.clear();
    size_t pos = 0;
    while (pos < script.size()) {
        size_t end = script.find(',', pos);
        if (end == std::string::npos) end = script.size();
        std::string item = script.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            g_script.push_back({std::strtoull(item.c_str(), nullptr, 10), u32(std::strtoul(item.c_str() + colon + 1, nullptr, 16))});
        pos = end + 1;
    }
    std::sort(g_script.begin(), g_script.end());
}

void advance_input_script(u64 frame) {
    for (const auto& [at, buttons] : g_script)
        if (at <= frame) g_script_buttons = buttons;
}

} // namespace p2::host
