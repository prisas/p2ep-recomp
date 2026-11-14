#pragma once

#include "lib/common/types.h"

#include <vector>

// The boundary between the display-list processor and a rendering backend. The processor decodes,
// transforms and lights vertices on the CPU and hands over ready-to-rasterise primitives together
// with the pixel pipeline state they need.
namespace p2::ge {

// Position is homogeneous with x/w and y/w in framebuffer pixels and z/w in [0, 1].
struct Vertex {
    f32 x, y, z, w;
    f32 u, v, q;  // texture coordinates, normalised; divide by q when projecting
    u32 color;    // R | G << 8 | B << 16 | A << 24
    u32 specular; // added after texturing (RGB only)
    f32 fog;      // 1 = no fog, 0 = fully fogged
};

struct TextureDesc {
    u32 addr = 0;
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    u32 buf_width = 0;
    bool swizzled = false;
    u32 clut_format = 0;
    u32 clut_shift = 0;
    u32 clut_mask = 0xFF;
    u32 clut_base = 0;
    u64 clut_hash = 0; // identifies the palette contents for indexed formats
    // The game touched the texture state since the last draw: its pixels may have changed even if
    // they were already checked this frame.
    bool revalidate = false;
    // Advances whenever the game's code has had a chance to run. Pixels compared in the current
    // epoch cannot have changed since.
    u64 memory_epoch = 0;
    bool linear_min = false;
    bool linear_mag = false;
    bool clamp_s = false;
    bool clamp_t = false;
};

struct DrawState {
    // render target
    u32 fb_addr = 0;
    u32 fb_stride = 0;
    u32 fb_format = 0;
    u32 zb_addr = 0;

    // clear mode: write the vertex colour / alpha / depth directly, no tests
    bool clear_mode = false;
    bool clear_color = false;
    bool clear_alpha = false;
    bool clear_depth = false;

    s32 scissor_x1 = 0, scissor_y1 = 0, scissor_x2 = 480, scissor_y2 = 272; // x2/y2 exclusive

    bool depth_test = false;
    bool depth_write = false;
    u8 depth_func = 1;

    bool blend = false;
    u8 blend_src = 0, blend_dst = 0, blend_eq = 0;
    u32 blend_fix_a = 0, blend_fix_b = 0;

    bool alpha_test = false;
    u8 alpha_func = 1, alpha_ref = 0, alpha_mask = 0xFF;
    bool color_test = false;
    u8 color_func = 1;
    u32 color_ref = 0, color_mask = 0;

    bool stencil_test = false;
    u8 stencil_func = 1, stencil_ref = 0, stencil_mask = 0xFF;
    u8 stencil_sfail = 0, stencil_zfail = 0, stencil_zpass = 0;

    u32 write_mask_rgb = 0; // bits set are NOT written
    u32 write_mask_alpha = 0;

    bool fog = false;
    u32 fog_color = 0;

    bool texture = false;
    u8 tex_func = 0;
    bool tex_alpha = false;
    bool color_double = false;
    u32 tex_env_color = 0;
    TextureDesc tex;

    bool flat_shade = false;
};

// What sceDisplaySetFrameBuf last selected for scan-out.
struct DisplayTarget {
    u32 addr = 0;
    u32 stride = 512;
    u32 format = 3;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    // Indexed triangles: the processor turns every primitive type into triangles first.
    virtual void draw(const DrawState& state, const Vertex* vertices, u32 vertex_count, const u16* indices,
                      u32 index_count) = 0;

    // Copies a rectangle of pixels between two guest buffers (either may be a render target).
    virtual void block_transfer(u32 src_addr, u32 src_stride, u32 src_x, u32 src_y, u32 dst_addr, u32 dst_stride,
                                u32 dst_x, u32 dst_y, u32 width, u32 height, u32 bytes_per_pixel) = 0;

    // Guest memory in [addr, addr + size) may have changed in a way the cache cannot see.
    virtual void invalidate_textures() = 0;

    // End of a displayed frame: show `target` and do per-frame housekeeping.
    virtual void present(const DisplayTarget& target) = 0;

    // Reads back what present() last showed, at 480x272, as RGBA8.
    virtual bool capture(std::vector<u8>& rgba, u32& width, u32& height) = 0;
};

// Decodes a guest texture to RGBA8 (R in the low byte). `clut` is the palette as loaded by the GE.
bool decode_texture(const TextureDesc& desc, const u8* clut, std::vector<u32>& out);
// Number of guest bytes the texture occupies (used for change detection).
u32 texture_byte_size(const TextureDesc& desc);
// Converts one framebuffer-format pixel run to RGBA8.
void convert_pixels(const u8* src, u32 format, u32 count, u32* dst);

} // namespace p2::ge
