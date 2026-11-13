// Per-frame host work: input, presentation, frame dumps and pacing to real time.

#include "runtime/platform/frame.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/ge/ge.h"
#include "runtime/hle/hle_display.h"
#include "runtime/kernel/kernel.h"
#include "runtime/platform/host.h"
#include "runtime/platform/window.h"

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
s64 g_qpc_frequency = 0;
s64 g_next_deadline = 0;
HANDLE g_timer = nullptr;

constexpr double kFrameSeconds = 1.001 / 60.0;

s64 qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Sleeps until the next frame boundary; resynchronises after a long stall instead of racing to catch up.
void pace() {
    if (!g_qpc_frequency) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_frequency = f.QuadPart;
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
        g_next_deadline = qpc_now();
    }
    const s64 frame_ticks = s64(kFrameSeconds * double(g_qpc_frequency));
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
    if (!renderer->capture(pixels, w, h)) return;
    std::error_code ec;
    std::filesystem::create_directories(options().dump_dir, ec);
    std::string path = (options().dump_dir / format("frame_%06llu.png", static_cast<unsigned long long>(g_frames))).string();
    stbi_write_png(path.c_str(), int(w), int(h), 4, pixels.data(), int(w) * 4);
}

} // namespace

u64 frames_shown() { return g_frames; }

bool write_png(const std::string& path, u32 width, u32 height, const void* rgba) {
    return stbi_write_png(path.c_str(), int(width), int(height), 4, rgba, int(width) * 4) != 0;
}

void on_frame() {
    const Options& opt = options();
    advance_input_script(g_frames);
    if (!pump_events()) kernel::request_stop(0);

    if (ge::Renderer* renderer = ge::renderer()) {
        const hle::DisplayState& display = hle::display();
        ge::DisplayTarget target{display.fb_addr, display.fb_stride, display.fb_format};
        renderer->present(target);
        if (!opt.dump_dir.empty() && opt.dump_every && g_frames % opt.dump_every == 0) dump_frame();
    }

    ++g_frames;
    if (opt.frame_limit && g_frames >= opt.frame_limit) kernel::request_stop(0);
    if (!opt.unthrottled && !opt.headless && !turbo_held()) pace();
}

} // namespace p2::host
