#pragma once

#include "lib/common/types.h"

#include <vector>

// Pixel work of the texture upscaler that needs no FFmpeg.
namespace p2::texup {

// RGBA8, R first in memory, rows top to bottom with no padding.
struct Rgba {
    u32 width = 0, height = 0;
    std::vector<u8> pixels; // 4 bytes per pixel
};

bool has_transparency(const Rgba& image);

// Gives each fully transparent pixel a colour taken from its opaque surroundings; alpha is left as
// it is. Those pixels' colour is otherwise junk (often black) that a scaler, or the smooth filtering
// the game draws replacements with, would blend into the edges of what is visible: dark fringes.
// Pixels within `reach` of something visible take the mean of their filled neighbours, ring by ring;
// any further away take the mean colour of the visible pixels.
void fill_transparent(Rgba& image, u32 reach = 8);

// The colour, and the alpha as a grey picture, in the layout FFmpeg's xBR filter takes: four bytes
// per pixel, B, G, R and an unused one (bgr0).
std::vector<u8> color_bgr0(const Rgba& image);
std::vector<u8> alpha_bgr0(const Rgba& image);

// Puts a colour picture and, when there is one, an alpha picture (both bgr0, same size) back
// together. Without alpha, the result is opaque.
Rgba merge_bgr0(u32 width, u32 height, const std::vector<u8>& color, const std::vector<u8>* alpha);

} // namespace p2::texup
