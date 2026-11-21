#include "tests/test.h"
#include "tools/texup/realesrgan.h"
#include "tools/texup/texture_ops.h"
#include "tools/texup/upscaler.h"

#include <string>

using namespace p2;
using texup::Rgba;

namespace {

Rgba blank(u32 w, u32 h) {
    Rgba image;
    image.width = w;
    image.height = h;
    image.pixels.assign(size_t(w) * h * 4, 0);
    return image;
}

void put(Rgba& image, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
    u8* p = &image.pixels[(size_t(y) * image.width + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

const u8* at(const Rgba& image, u32 x, u32 y) { return &image.pixels[(size_t(y) * image.width + x) * 4]; }

// A 16x16 sprite: a green 8x8 square on a transparent background whose colour is black junk.
Rgba sprite() {
    Rgba image = blank(16, 16);
    for (u32 y = 4; y < 12; ++y)
        for (u32 x = 4; x < 12; ++x) put(image, x, y, 40, 200, 60, 255);
    return image;
}

} // namespace

TEST(texup_fill_takes_neighbour_colours) {
    Rgba image = blank(3, 3);
    put(image, 1, 1, 255, 0, 0, 255);
    texup::fill_transparent(image);
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 3; ++x) {
            CHECK_EQ(at(image, x, y)[0], 255);
            CHECK_EQ(at(image, x, y)[1], 0);
            CHECK_EQ(at(image, x, y)[3], (x == 1 && y == 1) ? 255 : 0); // alpha is never touched
        }
    }
}

TEST(texup_fill_beyond_reach_takes_the_mean) {
    Rgba image = blank(6, 1);
    put(image, 0, 0, 200, 100, 0, 255);
    put(image, 1, 0, 100, 50, 40, 128); // semi-transparent pixels keep their own colour
    texup::fill_transparent(image, 1);
    CHECK_EQ(at(image, 1, 0)[0], 100);
    CHECK_EQ(at(image, 2, 0)[0], 100); // the first ring copies its only filled neighbour
    CHECK_EQ(at(image, 5, 0)[0], 150); // out of reach: the mean of the visible pixels
    CHECK_EQ(at(image, 5, 0)[1], 75);
    CHECK_EQ(at(image, 5, 0)[2], 20);
}

TEST(texup_fill_leaves_opaque_and_empty_images_alone) {
    Rgba empty = blank(4, 4);
    put(empty, 2, 2, 9, 9, 9, 0);
    Rgba before = empty;
    texup::fill_transparent(empty);
    CHECK(empty.pixels == before.pixels);
}

TEST(texup_split_and_merge_round_trip) {
    Rgba image = sprite();
    put(image, 0, 0, 1, 2, 3, 77);
    const std::vector<u8> color = texup::color_bgr0(image), alpha = texup::alpha_bgr0(image);
    CHECK_EQ(color[0], 3); // bgr0: blue first
    CHECK_EQ(color[2], 1);
    CHECK_EQ(alpha[1], 77);
    const Rgba back = texup::merge_bgr0(image.width, image.height, color, &alpha);
    CHECK(back.pixels == image.pixels);
    const Rgba opaque = texup::merge_bgr0(image.width, image.height, color, nullptr);
    CHECK_EQ(at(opaque, 0, 0)[3], 255);
}

TEST(texup_upscale_sprite) {
    texup::Rgba out;
    std::string error;
    texup::Settings settings;
    settings.scale = 4;
    CHECK(texup::upscale(sprite(), settings, out, &error));
    CHECK_EQ(out.width, 64u);
    CHECK_EQ(out.height, 64u);
    CHECK_EQ(at(out, 0, 0)[3], 0);     // far outside the square: still transparent
    CHECK_EQ(at(out, 32, 32)[3], 255); // inside: opaque, and still green
    CHECK(at(out, 32, 32)[1] > 150 && at(out, 32, 32)[0] < 100);
    // Just inside the edge the colour must not have been darkened by the junk around the square.
    CHECK(at(out, 17, 32)[1] > 150);
}

TEST(texup_upscale_scales_and_keeps_opaque) {
    Rgba image = blank(5, 3);
    for (u32 y = 0; y < 3; ++y)
        for (u32 x = 0; x < 5; ++x) put(image, x, y, u8(x * 50), u8(y * 100), 7, 255);
    texup::Rgba out;
    std::string error;
    texup::Settings settings;
    settings.scale = 2;
    settings.sharpen = 0.0f;
    CHECK(texup::upscale(image, settings, out, &error));
    CHECK_EQ(out.width, 10u);
    CHECK_EQ(out.height, 6u);
    bool opaque = true;
    for (size_t i = 3; i < out.pixels.size(); i += 4) opaque = opaque && out.pixels[i] == 255;
    CHECK(opaque);
    settings.scale = 5;
    CHECK(!texup::upscale(image, settings, out, &error));
}

TEST(texup_method_names) {
    texup::Method m = texup::Method::Xbr;
    CHECK(texup::parse_method("realesrgan", &m));
    CHECK(m == texup::Method::RealEsrgan);
    CHECK_EQ(std::string(texup::method_name(m)), std::string("realesrgan"));
    CHECK(texup::parse_method("xbr", &m));
    CHECK(m == texup::Method::Xbr);
    CHECK(!texup::parse_method("bicubic", &m));
}

TEST(texup_resize_and_alpha) {
    Rgba image = blank(8, 4);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 8; ++x) put(image, x, y, 200, 100, 50, 255);
    texup::Rgba out;
    std::string error;
    CHECK(texup::resize(image, 16, 8, out, &error));
    CHECK_EQ(out.width, 16u);
    CHECK_EQ(out.height, 8u);
    CHECK_EQ(at(out, 7, 3)[3], 255); // resizing gives an opaque picture
    std::vector<u8> alpha;
    u32 w = 0, h = 0;
    CHECK(texup::upscale_alpha(sprite(), 4, alpha, w, h, &error));
    CHECK_EQ(w, 64u);
    CHECK_EQ(h, 64u);
    CHECK_EQ(alpha[(size_t(32) * 64 + 32) * 4], 255); // inside the square
    CHECK_EQ(alpha[0], 0);                            // outside
}

TEST(texup_realesrgan_helpers) {
    using namespace texup::realesrgan;
    CHECK_EQ(native_scale("realesr-animevideov3", 2), 2u);
    CHECK_EQ(native_scale("realesr-animevideov3", 3), 3u);
    CHECK_EQ(native_scale("realesrgan-x4plus-anime", 2), 4u);
    CHECK_EQ(native_scale("realesrgan-x4plus", 4), 4u);
    const std::wstring cmd = command_line(L"C:\\r\\realesrgan-ncnn-vulkan.exe", L"C:\\in", L"C:\\out", "realesrgan-x4plus-anime", 2);
    CHECK(cmd.find(L"-i \"C:\\in\"") != std::wstring::npos);
    CHECK(cmd.find(L"-o \"C:\\out\"") != std::wstring::npos);
    CHECK(cmd.find(L"-m \"C:\\r\\models\"") != std::wstring::npos);
    CHECK(cmd.find(L"-n realesrgan-x4plus-anime -s 4 -f png") != std::wstring::npos);

    // An all-black result is a failure only when the source had something other than black.
    Rgba green = sprite(), dark = blank(16, 16), black = blank(64, 64);
    for (size_t i = 3; i < dark.pixels.size(); i += 4) dark.pixels[i] = 255;
    CHECK(black_result(green, black));
    CHECK(!black_result(dark, black));
    Rgba lit = black;
    put(lit, 10, 10, 90, 90, 90, 255);
    CHECK(!black_result(green, lit));
}
