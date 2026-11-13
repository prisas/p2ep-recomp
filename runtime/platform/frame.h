#pragma once

#include "lib/common/types.h"

#include <string>

namespace p2::host {

// Called once per guest vertical blank: polls input, presents, dumps frames and paces to real time.
void on_frame();
u64 frames_shown();

// Writes an RGBA8 image as PNG. Used by the frame and texture dumps.
bool write_png(const std::string& path, u32 width, u32 height, const void* rgba);

} // namespace p2::host
