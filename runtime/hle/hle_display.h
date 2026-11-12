#pragma once

#include "lib/common/types.h"

namespace p2::hle {

struct DisplayState {
    u32 mode = 0;
    u32 width = 480;
    u32 height = 272;
    u32 fb_addr = 0;   // guest address of the framebuffer the game wants displayed (0: none)
    u32 fb_stride = 512;
    u32 fb_format = 3; // 0: 565, 1: 5551, 2: 4444, 3: 8888
    u32 fb_serial = 0; // bumped on every sceDisplaySetFrameBuf
    u32 vcount = 0;
};

DisplayState& display();

// Starts the vertical-blank clock. `frame_hook` runs on the scheduler once per frame, after the
// guest's own vblank handlers.
void display_init(void (*frame_hook)());

// Blocks the calling guest thread until the next vertical blank.
void wait_for_vblank();

} // namespace p2::hle
