#pragma once

#include "tools/texup/texture_ops.h"

#include <string>
#include <vector>

// Upscaling with FFmpeg's filters: xBR, an edge-aware scaler made for pixel art, then optional
// contrast-adaptive sharpening. xBR ignores alpha, so transparency goes through it separately.
// (The other method, Real-ESRGAN, runs as a separate program: see realesrgan.h. It reuses the alpha
// and resizing steps here.)
namespace p2::texup {

enum class Method { Xbr, RealEsrgan };

struct Settings {
    Method method = Method::Xbr;
    u32 scale = 4;        // 2, 3 or 4
    f32 sharpen = 0.25f;  // xBR only: 0 (off) to 1
    std::string model = "realesrgan-x4plus-anime"; // Real-ESRGAN only
};

// The method's name, as the output folder and the settings use it: "xbr" or "realesrgan".
const char* method_name(Method method);
bool parse_method(const std::string& name, Method* method);

// Runs one bgr0 picture through an FFmpeg filter chain such as "xbr=n=4". The chain must leave the
// picture in bgr0. Returns false with `error` set on failure.
bool run_filters(const std::vector<u8>& bgr0, u32 width, u32 height, const std::string& chain,
                 std::vector<u8>& out, u32& out_width, u32& out_height, std::string* error);

// The whole xBR treatment: transparent pixels coloured from their surroundings, colour through xBR
// (and sharpening), alpha through xBR, back together. `out` is `scale` times the size of `in`.
bool upscale(const Rgba& in, const Settings& settings, Rgba& out, std::string* error);

// The alpha of `in`, upscaled `scale` times with xBR (as a bgr0 grey picture), for merge_bgr0.
bool upscale_alpha(const Rgba& in, u32 scale, std::vector<u8>& alpha, u32& width, u32& height, std::string* error);

// Resizes an opaque picture to width x height with Lanczos filtering (alpha is dropped).
bool resize(const Rgba& in, u32 width, u32 height, Rgba& out, std::string* error);

} // namespace p2::texup
