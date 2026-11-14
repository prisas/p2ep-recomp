// Per-frame host work: input, presentation, frame dumps and pacing to real time.

#include "runtime/platform/frame.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/audio/audio_out.h"
#include "runtime/cpu/rt.h"
#include "runtime/ge/ge.h"
#include "runtime/gfx/vk/vk_renderer.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/hle_display.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"
#include "runtime/platform/config.h"
#include "runtime/platform/host.h"
#include "runtime/platform/window.h"
#include "runtime/ui/overlay.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <stb_image_write.h>
#pragma clang diagnostic pop

#include <vector>

#include <windows.h>

namespace p2::host {

namespace {

u64 g_frames = 0;
gfx::Renderer* g_renderer = nullptr;
s64 g_qpc_frequency = 0;
s64 g_next_deadline = 0;
s64 g_last_present = 0;
u64 g_present_ticks = 0;
HANDLE g_timer = nullptr;

constexpr double kFrameSeconds = 1.001 / 60.0;

s64 qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void init_clock() {
    if (g_qpc_frequency) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_frequency = f.QuadPart;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    g_next_deadline = qpc_now();
}

// Sleeps until the next frame boundary, `speed` times faster than the console's frame rate;
// resynchronises after a long stall instead of racing to catch up.
void pace(u32 speed) {
    init_clock();
    const s64 frame_ticks = s64(kFrameSeconds * double(g_qpc_frequency)) / s64(speed);
    g_next_deadline += frame_ticks;
    s64 now = qpc_now();
    if (now > g_next_deadline + frame_ticks * 4) {
        g_next_deadline = now;
        return;
    }
    while (now < g_next_deadline) {
        s64 remaining_100ns = (g_next_deadline - now) * 10000000 / g_qpc_frequency;
        if (g_timer && remaining_100ns > 5000) {
            LARGE_INTEGER due;
            due.QuadPart = -(remaining_100ns - 3000); // wake slightly early, then spin
            SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(g_timer, 100);
        } else {
            YieldProcessor();
        }
        now = qpc_now();
    }
}

void dump_frame() {
    ge::Renderer* renderer = ge::renderer();
    if (!renderer) return;
    std::vector<u8> pixels;
    u32 w = 0, h = 0;
    // With the overlay on in an unattended run, dump the composed picture so the overlay shows.
    const bool composed = g_renderer && ui::active() && g_renderer->capture_output(pixels, w, h);
    if (!composed && !renderer->capture(pixels, w, h)) return;
    std::error_code ec;
    std::filesystem::create_directories(options().dump_dir, ec);
    std::string path = (options().dump_dir / format("frame_%06llu.png", static_cast<unsigned long long>(g_frames))).string();
    stbi_write_png(path.c_str(), int(w), int(h), 4, pixels.data(), int(w) * 4);
}

// --- skipping the intro ---------------------------------------------------------------------------------------
// The game lets the player skip each of its start-up screens (notices, logos, the opening movie)
// with Start, and shows the main menu by itself once the movie is gone. With the setting on, the
// program presses Start for the player and runs at full speed behind a blank window until the game
// shuts the opening movie down; the title then fades in at normal speed, with its music.

enum class Intro : u8 { Undecided, Skipping, Done };
Intro g_intro = Intro::Undecided;

constexpr u64 kIntroPressPeriod = 20; // frames between presses of Start ...
constexpr u64 kIntroPressLength = 4;  // ... each held this long
constexpr u64 kIntroGiveUp = 7200;    // two minutes of game time: show whatever there is

void set_intro_skipping(bool on) {
    if (g_renderer) g_renderer->set_output_visible(!on);
    audio::output_set_muted(on);
    if (!on) set_injected_buttons(0);
}

// Called before the frame's input is read.
void update_intro_skip() {
    if (g_intro == Intro::Undecided) {
        const Settings& s = settings();
        g_intro = s.skip_intro || s.run_skip_intro ? Intro::Skipping : Intro::Done;
        if (g_intro == Intro::Skipping) {
            set_intro_skipping(true);
            LOG_INFO("boot", "skipping the intro");
        }
    }
    if (g_intro != Intro::Skipping) return;

    const char* why = nullptr;
    if (hle::movies_finished() > 0) why = "the opening movie is over";
    else if (ui::captures_pad() || ui::menu_open()) why = "something needs the player"; // a dialog or the settings menu
    else if (g_frames >= kIntroGiveUp) why = "the main menu did not come in time";
    if (why) {
        g_intro = Intro::Done;
        set_intro_skipping(false);
        LOG_INFO("boot", "intro skipped at frame %llu: %s", static_cast<unsigned long long>(g_frames), why);
        return;
    }
    set_injected_buttons(g_frames % kIntroPressPeriod < kIntroPressLength ? kPadStart : 0);
}

} // namespace

u64 frames_shown() { return g_frames; }
u64 present_ticks() { return g_present_ticks; }

bool write_png(const std::string& path, u32 width, u32 height, const void* rgba) {
    return stbi_write_png(path.c_str(), int(width), int(height), 4, rgba, int(width) * 4) != 0;
}

void set_renderer(gfx::Renderer* renderer) { g_renderer = renderer; }

void on_frame() {
    const Options& opt = options();
    advance_input_script(g_frames);
    if (opt.menu_frame && g_frames == opt.menu_frame) ui::open_menu(int(opt.menu_tab));
    update_intro_skip();
    bool running = pump_events();

    // Frame time and speed for the performance display, measured presentation to presentation.
    init_clock();
    const s64 now = qpc_now();
    if (g_last_present) {
        const f32 ms = f32(double(now - g_last_present) * 1000.0 / double(g_qpc_frequency));
        ui::frame_stats(ms, ms > 0.0f ? f32(kFrameSeconds * 1000.0) / ms * 100.0f : 0.0f);
    }
    g_last_present = now;

    if (ge::Renderer* renderer = ge::renderer()) {
        const hle::DisplayState& display = hle::display();
        ge::DisplayTarget target{display.fb_addr, display.fb_stride, display.fb_format};
        const u64 started = ge::host_ticks();
        renderer->present(target);
        g_present_ticks += ge::host_ticks() - started;
        if (!opt.dump_dir.empty() && opt.dump_every && g_frames >= opt.dump_from && g_frames % opt.dump_every == 0) dump_frame();
        // While the menu is open the game stands still: keep showing the same picture under it.
        while (running && ui::menu_open() && !opt.headless) {
            pace(1);
            running = pump_events();
            renderer->present(target);
            g_last_present = 0;
        }
    }
    if (!running) kernel::request_stop(0);

    ++g_frames;
    if (!opt.late_channels.empty() && g_frames == opt.late_from) {
        log::enable_channels(opt.late_channels);
        refresh_hle_trace();
    }
    if (opt.watch_size && g_frames == opt.watch_from) mem::watch_writes(opt.watch_addr, opt.watch_size);
    if (!opt.ram_dump.empty() && g_frames == opt.ram_dump_frame)
        write_file(opt.ram_dump, mem::ptr(mem::kRamBase), mem::kRamSize);
    ge::set_draw_trace(g_frames == opt.trace_frame);
    if (g_frames == opt.trace_frame) {
        const hle::DisplayState& display = hle::display();
        LOG_INFO("ge", "--- frame %llu, display %08x/%u f%u", static_cast<unsigned long long>(g_frames), display.fb_addr,
                 display.fb_stride, display.fb_format);
    }
    if (opt.frame_limit && g_frames >= opt.frame_limit) kernel::request_stop(0);
    if (opt.unthrottled || opt.headless || g_intro == Intro::Skipping) return;
    // Fast-forward runs flat out, or at a chosen multiple of normal speed.
    if (!turbo_held()) pace(1);
    else if (settings().turbo_speed) pace(settings().turbo_speed);
}

} // namespace p2::host
