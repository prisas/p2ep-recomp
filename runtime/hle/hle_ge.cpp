// sceGe_user and sceCtrl: the game's interface to the graphics engine and the controller.

#include "runtime/ge/ge.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/hle_display.h"
#include "runtime/platform/window.h"

using namespace p2;
using namespace p2::kernel;

// --- sceGe_user -----------------------------------------------------------------------------------------------

HLE(sceGeEdramGetAddr) { RET(mem::kVramBase); }

HLE(sceGeSetCallback) {
    u32 data = ARG(0);
    if (!mem::valid(data, 16)) RET(kErrIllegalAddr);
    RET(ge::set_callback(mem::r32(data), mem::r32(data + 4), mem::r32(data + 8), mem::r32(data + 12)));
}

HLE(sceGeUnsetCallback) { RET(ge::unset_callback(s32(ARG(0)))); }

HLE(sceGeListEnQueue) {
    c->r[2] = u32(ge::enqueue(ARG(0), ARG(1), s32(ARG(2)), false));
    reschedule();
}

HLE(sceGeListEnQueueHead) {
    c->r[2] = u32(ge::enqueue(ARG(0), ARG(1), s32(ARG(2)), true));
    reschedule();
}

HLE(sceGeListUpdateStallAddr) {
    c->r[2] = u32(ge::update_stall(s32(ARG(0)), ARG(1)));
    reschedule();
}

HLE(sceGeListSync) { RET(ge::list_sync(s32(ARG(0)), ARG(1))); }
HLE(sceGeDrawSync) { RET(ge::draw_sync(ARG(0))); }
HLE(sceGeBreak) { RET(0); }
HLE(sceGeContinue) { RET(0); }

// --- sceCtrl --------------------------------------------------------------------------------------------------

HLE(sceCtrlSetSamplingCycle) { RET(0); }
HLE(sceCtrlSetSamplingMode) { RET(0); }
HLE(sceCtrlSetIdleCancelThreshold) { RET(0); }

// Blocks until the next sample (one per vertical blank), then returns the pad state.
HLE(sceCtrlReadBufferPositive) {
    u32 out = ARG(0), count = ARG(1);
    hle::wait_for_vblank();
    if (count == 0 || !mem::valid(out, 16)) RET(0);
    host::PadState pad = host::pad_state();
    mem::w32(out + 0, u32(now_us()));
    mem::w32(out + 4, pad.buttons);
    mem::w8(out + 8, pad.lx);
    mem::w8(out + 9, pad.ly);
    std::memset(mem::ptr(out + 10), 0, 6);
    RET(1);
}
