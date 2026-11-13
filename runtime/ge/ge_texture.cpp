// Texture and pixel-format decoding for the graphics engine.

#include "runtime/ge/ge_state.h"
#include "runtime/ge/renderer.h"
#include "runtime/mem/memory.h"

#include <algorithm>

namespace p2::ge {

namespace {

P2_INLINE u32 expand5(u32 v) { return (v << 3) | (v >> 2); }
P2_INLINE u32 expand6(u32 v) { return (v << 2) | (v >> 4); }
P2_INLINE u32 expand4(u32 v) { return (v << 4) | v; }

P2_INLINE u32 from_5650(u32 p) {
    return expand5(p & 31) | (expand6((p >> 5) & 63) << 8) | (expand5((p >> 11) & 31) << 16) | 0xFF000000u;
}
P2_INLINE u32 from_5551(u32 p) {
    return expand5(p & 31) | (expand5((p >> 5) & 31) << 8) | (expand5((p >> 10) & 31) << 16) | ((p & 0x8000) ? 0xFF000000u : 0);
}
P2_INLINE u32 from_4444(u32 p) {
    return expand4(p & 15) | (expand4((p >> 4) & 15) << 8) | (expand4((p >> 8) & 15) << 16) | (expand4((p >> 12) & 15) << 24);
}

u32 convert_color(u32 value, u32 format) {
    switch (format) {
    case 0: return from_5650(value);
    case 1: return from_5551(value);
    case 2: return from_4444(value);
    default: return value;
    }
}

u32 bits_per_pixel(u32 format) {
    switch (format) {
    case kTex5650:
    case kTex5551:
    case kTex4444:
    case kTexClut16: return 16;
    case kTex8888:
    case kTexClut32: return 32;
    case kTexClut4: return 4;
    case kTexClut8: return 8;
    default: return 0; // block-compressed
    }
}

// Swizzled textures store 16-byte-wide, 8-row-high blocks in raster order.
void unswizzle(const u8* src, u8* dst, u32 row_bytes, u32 height) {
    const u32 block_w = 16, block_h = 8;
    const u32 blocks_x = (row_bytes + block_w - 1) / block_w;
    const u32 blocks_y = (height + block_h - 1) / block_h;
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            for (u32 y = 0; y < block_h; ++y) {
                u32 row = by * block_h + y;
                u32 col = bx * block_w;
                if (row < height && col < row_bytes) {
                    u32 n = std::min(block_w, row_bytes - col);
                    std::memcpy(dst + size_t(row) * row_bytes + col, src, n);
                }
                src += block_w;
            }
        }
    }
}

void decode_dxt_colors(const u8* block, u32 colors[4], bool dxt1) {
    u32 c0 = load_le<u16>(block), c1 = load_le<u16>(block + 2);
    auto rgb = [](u32 c, u32& r, u32& g, u32& b) {
        r = expand5((c >> 11) & 31);
        g = expand6((c >> 5) & 63);
        b = expand5(c & 31);
    };
    u32 r0, g0, b0, r1, g1, b1;
    rgb(c0, r0, g0, b0);
    rgb(c1, r1, g1, b1);
    auto pack = [](u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); };
    colors[0] = pack(r0, g0, b0, 255);
    colors[1] = pack(r1, g1, b1, 255);
    if (c0 > c1 || !dxt1) {
        colors[2] = pack((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        colors[3] = pack((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        colors[2] = pack((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        colors[3] = 0;
    }
}

// The GE stores each block's 32 bits of colour indices before its two endpoint colours, and for
// DXT3/DXT5 puts the colour part before the alpha part.
void decode_dxt(const TextureDesc& d, const u8* src, std::vector<u32>& out) {
    const u32 block_bytes = d.format == kTexDxt1 ? 8 : 16;
    const u32 blocks_per_row = std::max(d.buf_width, 4u) / 4;
    for (u32 by = 0; by < (d.height + 3) / 4; ++by) {
        for (u32 bx = 0; bx < (d.width + 3) / 4; ++bx) {
            const u8* block = src + (size_t(by) * blocks_per_row + bx) * block_bytes;
            u8 color_block[8];
            std::memcpy(color_block, block + 4, 4);
            std::memcpy(color_block + 4, block, 4);
            u32 colors[4];
            decode_dxt_colors(color_block, colors, d.format == kTexDxt1);
            u32 indices = load_le<u32>(color_block + 4);

            u8 alpha[16];
            if (d.format == kTexDxt3) {
                for (u32 i = 0; i < 16; ++i) alpha[i] = u8(expand4((load_le<u64>(block + 8) >> (i * 4)) & 15));
            } else if (d.format == kTexDxt5) {
                u32 a0 = block[14], a1 = block[15];
                u64 bits = load_le<u64>(block + 8) & 0xFFFFFFFFFFFFull;
                u32 table[8] = {a0, a1};
                if (a0 > a1) {
                    for (u32 i = 1; i < 7; ++i) table[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                } else {
                    for (u32 i = 1; i < 5; ++i) table[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    table[6] = 0;
                    table[7] = 255;
                }
                for (u32 i = 0; i < 16; ++i) alpha[i] = u8(table[(bits >> (i * 3)) & 7]);
            }

            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    u32 px = bx * 4 + x, py = by * 4 + y;
                    if (px >= d.width || py >= d.height) continue;
                    u32 i = y * 4 + x;
                    u32 color = colors[(indices >> (i * 2)) & 3];
                    if (d.format != kTexDxt1) color = (color & 0x00FFFFFFu) | (u32(alpha[i]) << 24);
                    out[size_t(py) * d.width + px] = color;
                }
            }
        }
    }
}

} // namespace

void convert_pixels(const u8* src, u32 format, u32 count, u32* dst) {
    if (format == 3) {
        std::memcpy(dst, src, size_t(count) * 4);
        return;
    }
    for (u32 i = 0; i < count; ++i) dst[i] = convert_color(load_le<u16>(src + i * 2), format);
}

u32 texture_byte_size(const TextureDesc& d) {
    u32 bpp = bits_per_pixel(d.format);
    if (bpp == 0) {
        u32 blocks = (std::max(d.buf_width, 4u) / 4) * ((d.height + 3) / 4);
        return blocks * (d.format == kTexDxt1 ? 8 : 16);
    }
    return (std::max(d.buf_width, 1u) * d.height * bpp + 7) / 8;
}

bool decode_texture(const TextureDesc& d, const u8* clut, std::vector<u32>& out) {
    if (d.width == 0 || d.height == 0 || d.width > 1024 || d.height > 1024) return false;
    const u32 bytes = texture_byte_size(d);
    if (!mem::valid(d.addr, bytes)) return false;
    out.assign(size_t(d.width) * d.height, 0);
    const u8* src = mem::ptr(d.addr);

    const u32 bpp = bits_per_pixel(d.format);
    if (bpp == 0) {
        decode_dxt(d, src, out);
        return true;
    }

    const u32 row_bytes = (std::max(d.buf_width, 1u) * bpp + 7) / 8;
    std::vector<u8> linear;
    if (d.swizzled) {
        // The swizzled image covers whole blocks; give the source and destination room for them.
        const u32 padded_row = align_up<u32>(row_bytes, 16), padded_h = align_up<u32>(d.height, 8);
        if (!mem::valid(d.addr, padded_row * padded_h)) return false;
        linear.resize(size_t(padded_row) * padded_h);
        unswizzle(src, linear.data(), padded_row, padded_h);
        // Re-pack to the unpadded row length the loops below expect.
        if (padded_row != row_bytes) {
            for (u32 y = 1; y < d.height; ++y)
                std::memmove(linear.data() + size_t(y) * row_bytes, linear.data() + size_t(y) * padded_row, row_bytes);
        }
        src = linear.data();
    }

    auto palette = [&](u32 index) {
        u32 entry = ((index >> d.clut_shift) & d.clut_mask) | d.clut_base;
        if (d.clut_format == 3) return load_le<u32>(clut + (entry & 0xFF) * 4);
        return convert_color(load_le<u16>(clut + (entry & 0x1FF) * 2), d.clut_format);
    };

    const u32 visible = std::min(d.width, std::max(d.buf_width, 1u));
    for (u32 y = 0; y < d.height; ++y) {
        const u8* row = src + size_t(y) * row_bytes;
        u32* dst = out.data() + size_t(y) * d.width;
        switch (d.format) {
        case kTex5650:
        case kTex5551:
        case kTex4444:
            for (u32 x = 0; x < visible; ++x) dst[x] = convert_color(load_le<u16>(row + x * 2), d.format);
            break;
        case kTex8888: std::memcpy(dst, row, size_t(visible) * 4); break;
        case kTexClut4:
            for (u32 x = 0; x < visible; ++x) dst[x] = palette((row[x / 2] >> ((x & 1) * 4)) & 15);
            break;
        case kTexClut8:
            for (u32 x = 0; x < visible; ++x) dst[x] = palette(row[x]);
            break;
        case kTexClut16:
            for (u32 x = 0; x < visible; ++x) dst[x] = palette(load_le<u16>(row + x * 2));
            break;
        case kTexClut32:
            for (u32 x = 0; x < visible; ++x) dst[x] = palette(load_le<u32>(row + x * 4));
            break;
        default: return false;
        }
    }
    return true;
}

} // namespace p2::ge
