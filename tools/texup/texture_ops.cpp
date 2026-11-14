#include "tools/texup/texture_ops.h"

namespace p2::texup {

bool has_transparency(const Rgba& image) {
    for (size_t i = 3; i < image.pixels.size(); i += 4)
        if (image.pixels[i] != 255) return true;
    return false;
}

void fill_transparent(Rgba& image, u32 reach) {
    const u32 w = image.width, h = image.height;
    const size_t count = size_t(w) * h;
    if (count == 0 || image.pixels.size() < count * 4) return;
    u8* px = image.pixels.data();

    // Known pixels have a colour worth keeping: everything not fully transparent.
    std::vector<u8> known(count);
    u64 sum[3] = {};
    u64 visible = 0;
    for (size_t i = 0; i < count; ++i) {
        if (px[i * 4 + 3] == 0) continue;
        known[i] = 1;
        for (int c = 0; c < 3; ++c) sum[c] += px[i * 4 + size_t(c)];
        ++visible;
    }
    if (visible == 0 || visible == count) return; // nothing to take colours from, or nothing to fill

    // Grow the known area one ring at a time; a ring only sees the rings before it.
    std::vector<size_t> ring;
    std::vector<u8> colours;
    for (u32 pass = 0; pass < reach; ++pass) {
        ring.clear();
        colours.clear();
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (known[i]) continue;
                u32 total[3] = {}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const s64 nx = s64(x) + dx, ny = s64(y) + dy;
                        if ((dx | dy) == 0 || nx < 0 || ny < 0 || nx >= s64(w) || ny >= s64(h)) continue;
                        const size_t j = size_t(ny) * w + size_t(nx);
                        if (!known[j]) continue;
                        for (int c = 0; c < 3; ++c) total[c] += px[j * 4 + size_t(c)];
                        ++n;
                    }
                }
                if (n == 0) continue;
                ring.push_back(i);
                for (int c = 0; c < 3; ++c) colours.push_back(u8((total[c] + n / 2) / n));
            }
        }
        if (ring.empty()) break;
        for (size_t k = 0; k < ring.size(); ++k) {
            const size_t i = ring[k];
            for (int c = 0; c < 3; ++c) px[i * 4 + size_t(c)] = colours[k * 3 + size_t(c)];
            known[i] = 1;
        }
    }

    // Whatever is left lies far from anything visible: the mean colour keeps it neutral.
    u8 mean[3];
    for (int c = 0; c < 3; ++c) mean[c] = u8((sum[c] + visible / 2) / visible);
    for (size_t i = 0; i < count; ++i)
        if (!known[i])
            for (int c = 0; c < 3; ++c) px[i * 4 + size_t(c)] = mean[c];
}

std::vector<u8> color_bgr0(const Rgba& image) {
    std::vector<u8> out(image.pixels.size());
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        out[i + 0] = image.pixels[i + 2];
        out[i + 1] = image.pixels[i + 1];
        out[i + 2] = image.pixels[i + 0];
        out[i + 3] = 0;
    }
    return out;
}

std::vector<u8> alpha_bgr0(const Rgba& image) {
    std::vector<u8> out(image.pixels.size());
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        const u8 a = image.pixels[i + 3];
        out[i + 0] = a;
        out[i + 1] = a;
        out[i + 2] = a;
        out[i + 3] = 0;
    }
    return out;
}

Rgba merge_bgr0(u32 width, u32 height, const std::vector<u8>& color, const std::vector<u8>* alpha) {
    Rgba out;
    out.width = width;
    out.height = height;
    out.pixels.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        out.pixels[i + 0] = color[i + 2];
        out.pixels[i + 1] = color[i + 1];
        out.pixels[i + 2] = color[i + 0];
        // xBR blends the grey channels identically; green is the one luma weighs most.
        out.pixels[i + 3] = alpha ? (*alpha)[i + 1] : 255;
    }
    return out;
}

} // namespace p2::texup
