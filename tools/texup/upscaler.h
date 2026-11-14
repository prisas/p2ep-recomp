#pragma once

#include "tools/texup/texture_ops.h"

#include <string>
#include <vector>

// Upscaling with FFmpeg's filters: xBR, an edge-aware scaler made for pixel art, then optional
// contrast-adaptive sharpening. xBR ignores alpha, so transparency goes through it separately.
namespace p2::texup {

struct Settings {
    u32 scale = 4;        // 2, 3 or 4 (what xBR offers)
    f32 sharpen = 0.25f;  // 0 (off) to 1
};

// Runs one bgr0 picture through an FFmpeg filter chain such as "xbr=n=4". The chain must leave the
// picture in bgr0. Returns false with `error` set on failure.
bool run_filters(const std::vector<u8>& bgr0, u32 width, u32 height, const std::string& chain,
                 std::vector<u8>& out, u32& out_width, u32& out_height, std::string* error);

// The whole treatment: transparent pixels coloured from their surroundings, colour through xBR
// (and sharpening), alpha through xBR, back together. `out` is `scale` times the size of `in`.
bool upscale(const Rgba& in, const Settings& settings, Rgba& out, std::string* error);

} // namespace p2::texup
