// sceDisplay: the vertical-blank clock every game loop is paced by, and the framebuffer the game
// asks to have shown.

#include "runtime/hle/hle_display.h"

#include "runtime/hle/hle.h"

#include <vector>

using namespace p2;
using namespace p2::kernel;

namespace {

constexpr u64 kFramePeriodUs = 16683; // 59.94 Hz
constexpr u64 kVblankLengthUs = 731;  // the part of each frame spent in vertical blank

hle::DisplayState g_display;
u64 g_last_vblank_us = 0;
std::vector<std::pair<Uid, u32>> g_vblank_waiters;
void (*g_frame_hook)() = nullptr;

void vblank_event(u64) {
    g_last_vblank_us = now_us();
    ++g_display.vcount;

    for (auto [uid, serial] : g_vblank_waiters) {
        Thread* t = get<Thread>(uid);
        if (t && t->wait_type == WaitType::Vblank && t->wait_serial == serial) wake_thread(t, 0);
    }
    g_vblank_waiters.clear();

    trigger_interrupt(kIntrVblank);
    if (g_frame_hook) g_frame_hook();
    schedule_event(kFramePeriodUs, vblank_event, 0);
}

u32 wait_vblank(bool callbacks) {
    Thread* t = current_thread();
    if (!t) return kErrCanNotWait;
    g_vblank_waiters.push_back({t->uid, t->wait_serial + 1});
    wait_current(WaitType::Vblank, 0, 0, callbacks);
    return 0;
}

} // namespace

namespace p2::hle {

DisplayState& display() { return g_display; }

void display_init(void (*frame_hook)()) {
    g_frame_hook = frame_hook;
    schedule_event(kFramePeriodUs, vblank_event, 0);
}

void wait_for_vblank() { wait_vblank(false); }

} // namespace p2::hle

HLE(sceDisplaySetMode) {
    g_display.mode = ARG(0);
    g_display.width = ARG(1);
    g_display.height = ARG(2);
    RET(0);
}

HLE(sceDisplaySetFrameBuf) {
    u32 addr = ARG(0), stride = ARG(1), format = ARG(2);
    if (addr && (stride == 0 || format > 3)) RET(kErrIllegalArgument);
    g_display.fb_addr = addr;
    g_display.fb_stride = stride;
    g_display.fb_format = format;
    ++g_display.fb_serial;
    LOG_CH("display", "set framebuffer %08x stride %u format %u sync %u", addr, stride, format, ARG(3));
    RET(0);
}

HLE(sceDisplayWaitVblankStartCB) { RET(wait_vblank(true)); }

HLE(sceDisplayWaitVblankCB) {
    // Returns at once when the display is already in vertical blank.
    if (now_us() - g_last_vblank_us < kVblankLengthUs) RET(0);
    RET(wait_vblank(true));
}

HLE(sceDisplayIsVblank) { RET(now_us() - g_last_vblank_us < kVblankLengthUs ? 1 : 0); }
HLE(sceDisplayGetVcount) { RET(g_display.vcount); }

HLE(sceDisplayGetFramePerSec) {
    c->f[0] = 59.94f;
    RET(0);
}
