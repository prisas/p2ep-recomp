#pragma once

#include "lib/common/types.h"

#include <string>
#include <vector>

namespace p2::gfx {
class Renderer;
}

namespace p2::host {

// The renderer whose composed output frame dumps may use. Optional.
void set_renderer(gfx::Renderer* renderer);

// Called once per guest vertical blank: polls input, presents, dumps frames and paces to real time.
void on_frame();
u64 frames_shown();
// Host time spent presenting (composition and waiting for the GPU), in ge::host_ticks() units.
u64 present_ticks();

// Writes an RGBA8 image as PNG. Used by the frame and texture dumps.
bool write_png(const std::string& path, u32 width, u32 height, const void* rgba);

// The picture last shown, as RGBA8: with the overlay in an unattended run, the composed picture.
bool capture_picture(std::vector<u8>& rgba, u32& width, u32& height);

} // namespace p2::host
