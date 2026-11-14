// make_icon: turns a PNG or JPEG into a Windows icon with the sizes Explorer, the taskbar and the
// title bar ask for. The image is centre-cropped to a square first.
//
// usage: make_icon <image> <output.ico>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#pragma clang diagnostic pop

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kSizes[] = {256, 128, 64, 48, 32, 24, 16};

void put16(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

// The largest size is stored as PNG, which keeps the file small; the others as uncompressed
// bitmaps, which every part of Windows reads.
std::vector<uint8_t> encode_png(const std::vector<uint8_t>& rgba, int size) {
    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int length) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            const auto* bytes = static_cast<const uint8_t*>(data);
            out->insert(out->end(), bytes, bytes + length);
        },
        &png, size, size, 4, rgba.data(), size * 4);
    return png;
}

std::vector<uint8_t> encode_bitmap(const std::vector<uint8_t>& rgba, int size) {
    std::vector<uint8_t> dib;
    put32(dib, 40);              // header size
    put32(dib, uint32_t(size));  // width
    put32(dib, uint32_t(size * 2)); // height of colour plus mask
    put16(dib, 1);               // planes
    put16(dib, 32);              // bits per pixel
    put32(dib, 0);               // no compression
    put32(dib, uint32_t(size * size * 4));
    for (int i = 0; i < 4; ++i) put32(dib, 0);
    for (int y = size - 1; y >= 0; --y) { // bottom-up, BGRA
        for (int x = 0; x < size; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * size + x) * 4];
            dib.push_back(p[2]);
            dib.push_back(p[1]);
            dib.push_back(p[0]);
            dib.push_back(p[3]);
        }
    }
    const size_t mask_row = size_t((size + 31) / 32) * 4; // the alpha channel decides; the mask stays clear
    dib.insert(dib.end(), mask_row * size_t(size), 0);
    return dib;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: make_icon <image> <output.ico>\n");
        return 2;
    }
    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load(argv[1], &w, &h, &channels, 4);
    if (!pixels) {
        std::fprintf(stderr, "make_icon: cannot read %s: %s\n", argv[1], stbi_failure_reason());
        return 1;
    }
    const int side = w < h ? w : h;
    const uint8_t* square = pixels + (size_t((h - side) / 2) * w + size_t((w - side) / 2)) * 4;

    std::vector<std::vector<uint8_t>> images;
    for (int size : kSizes) {
        std::vector<uint8_t> rgba(size_t(size) * size * 4);
        if (!stbir_resize_uint8_srgb(square, side, side, w * 4, rgba.data(), size, size, size * 4, STBIR_RGBA)) {
            std::fprintf(stderr, "make_icon: cannot resize to %d pixels\n", size);
            stbi_image_free(pixels);
            return 1;
        }
        images.push_back(size >= 256 ? encode_png(rgba, size) : encode_bitmap(rgba, size));
    }
    stbi_image_free(pixels);

    std::vector<uint8_t> ico;
    put16(ico, 0);
    put16(ico, 1); // icon
    put16(ico, uint16_t(images.size()));
    uint32_t offset = uint32_t(6 + 16 * images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const int size = kSizes[i];
        ico.push_back(uint8_t(size >= 256 ? 0 : size));
        ico.push_back(uint8_t(size >= 256 ? 0 : size));
        ico.push_back(0); // no palette
        ico.push_back(0);
        put16(ico, 1);    // planes
        put16(ico, 32);   // bits per pixel
        put32(ico, uint32_t(images[i].size()));
        put32(ico, offset);
        offset += uint32_t(images[i].size());
    }
    for (const auto& image : images) ico.insert(ico.end(), image.begin(), image.end());

    FILE* f = std::fopen(argv[2], "wb");
    if (!f || std::fwrite(ico.data(), 1, ico.size(), f) != ico.size()) {
        std::fprintf(stderr, "make_icon: cannot write %s\n", argv[2]);
        if (f) std::fclose(f);
        return 1;
    }
    std::fclose(f);
    return 0;
}
