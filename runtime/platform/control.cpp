#include "runtime/platform/control.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/cpu/rt.h"
#include "runtime/kernel/kernel.h"
#include "runtime/platform/frame.h"
#include "runtime/platform/window.h"
#include "runtime/ui/overlay.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <sstream>
#include <string>
#include <thread>

namespace p2::host::control {

namespace {

struct Command {
    enum Kind { Hold, Wait, Shot, Quit, Trace, Untrace, Threads, Menu } kind = Wait;
    u32 buttons = 0;
    u64 frames = 0;
    std::string text; // Trace: the channels
    int tab = -1;          // Menu: the tab, or -1 to close the menu
    f32 scroll = -1.0f;    // Menu: where to scroll the tab, or -1
};

bool g_active = false;
std::filesystem::path g_dir;
std::deque<Command> g_queue;
u64 g_left = 0;         // frames left of the command at the front
bool g_started = false; // the front command has begun
bool g_shot_due = false;
u32 g_shots = 0;

u32 parse_buttons(const std::string& text) {
    static const struct {
        const char* name;
        u32 bit;
    } kNames[] = {{"up", kPadUp},       {"down", kPadDown},   {"left", kPadLeft},         {"right", kPadRight},
                  {"cross", kPadCross}, {"circle", kPadCircle}, {"square", kPadSquare}, {"triangle", kPadTriangle},
                  {"l", kPadL},         {"r", kPadR},         {"start", kPadStart},       {"select", kPadSelect}};
    u32 bits = 0;
    std::stringstream parts(text);
    std::string part;
    while (std::getline(parts, part, '+')) {
        bool known = false;
        for (const auto& n : kNames)
            if (part == n.name) {
                bits |= n.bit;
                known = true;
            }
        if (!known) LOG_WARN("control", "unknown button '%s'", part.c_str());
    }
    return bits;
}

void parse(const std::string& text) {
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::stringstream words(line);
        std::string verb, a, b;
        words >> verb >> a >> b;
        if (verb.empty() || verb[0] == '#') continue;
        if (verb == "hold") {
            g_queue.push_back({Command::Hold, parse_buttons(a), std::max<u64>(1, std::strtoull(b.c_str(), nullptr, 10))});
        } else if (verb == "tap") {
            const u32 bits = parse_buttons(a);
            const u64 count = b.empty() ? 1 : std::max<u64>(1, std::strtoull(b.c_str(), nullptr, 10));
            for (u64 k = 0; k < count; ++k) {
                g_queue.push_back({Command::Hold, bits, 4});
                g_queue.push_back({Command::Wait, 0, 12});
            }
        } else if (verb == "wait") {
            g_queue.push_back({Command::Wait, 0, std::max<u64>(1, std::strtoull(a.c_str(), nullptr, 10))});
        } else if (verb == "shot") {
            g_queue.push_back({Command::Shot});
        } else if (verb == "quit") {
            g_queue.push_back({Command::Quit});
        } else if (verb == "trace") { // trace <channels>: switch log channels on (e.g. hle,thread)
            g_queue.push_back({Command::Trace, 0, 0, a});
        } else if (verb == "untrace") {
            g_queue.push_back({Command::Untrace});
        } else if (verb == "threads") {
            g_queue.push_back({Command::Threads});
        } else if (verb == "menu") { // menu <tab> [scroll]: open the settings menu (needs --overlay); "menu close" closes it
            Command c{Command::Menu};
            c.tab = a == "close" ? -1 : int(std::strtol(a.c_str(), nullptr, 10));
            if (!b.empty()) c.scroll = std::strtof(b.c_str(), nullptr);
            g_queue.push_back(c);
        } else {
            LOG_WARN("control", "unknown command '%s'", line.c_str());
        }
    }
}

// Takes <dir>/commands.txt if it is there.
bool take_commands() {
    const std::filesystem::path file = g_dir / "commands.txt";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return false;
    std::string text;
    if (!read_text_file(file, text)) return false;
    std::filesystem::remove(file, ec);
    parse(text);
    return true;
}

} // namespace

void init(const std::filesystem::path& dir) {
    g_dir = dir;
    g_active = true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    LOG_INFO("control", "waiting for commands in %s", path_utf8(dir).c_str());
}

bool active() { return g_active; }

bool before_frame(u64 frame) {
    if (!g_active) return true;
    for (;;) {
        if (g_queue.empty()) {
            if (!take_commands()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            continue;
        }
        Command& c = g_queue.front();
        if (c.kind == Command::Quit) return false;
        if (c.kind == Command::Trace || c.kind == Command::Untrace) {
            if (c.kind == Command::Trace) log::enable_channels(c.text);
            else log::disable_channels();
            refresh_hle_trace();
            // A warning flushes the log file, so what was traced can be read straight away.
            LOG_WARN("control", "tracing %s at frame %llu", c.kind == Command::Trace ? c.text.c_str() : "off",
                     static_cast<unsigned long long>(frame));
            g_queue.pop_front();
            continue;
        }
        if (c.kind == Command::Threads) {
            LOG_WARN("control", "guest threads at frame %llu:", static_cast<unsigned long long>(frame));
            kernel::dump_state();
            g_queue.pop_front();
            continue;
        }
        if (c.kind == Command::Menu) {
            if (c.tab < 0) {
                if (ui::menu_open()) ui::toggle_menu();
            } else {
                ui::open_menu(c.tab, c.scroll);
            }
            g_queue.pop_front();
            continue;
        }
        if (c.kind == Command::Shot) {
            g_shot_due = true;
            g_queue.pop_front();
            set_control_buttons(0);
            return true; // this frame is shown, then written out
        }
        if (!g_started) {
            g_started = true;
            g_left = c.frames;
        }
        set_control_buttons(c.kind == Command::Hold ? c.buttons : 0);
        if (--g_left == 0) {
            g_queue.pop_front();
            g_started = false;
        }
        (void)frame;
        return true;
    }
}

void after_frame(u64 frame) {
    if (!g_active || !g_shot_due) return;
    g_shot_due = false;
    std::vector<u8> pixels;
    u32 w = 0, h = 0;
    if (!capture_picture(pixels, w, h)) {
        LOG_WARN("control", "no picture to write");
    } else {
        const std::filesystem::path partial = g_dir / "shot.png.partial";
        write_png(partial.string(), w, h, pixels.data());
        std::error_code ec;
        std::filesystem::rename(partial, g_dir / "shot.png", ec);
    }
    const std::string status = format("%u %llu\n", ++g_shots, static_cast<unsigned long long>(frame));
    write_file(g_dir / "status.txt", status.data(), status.size());
}

} // namespace p2::host::control
