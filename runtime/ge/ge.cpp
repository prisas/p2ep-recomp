#include "runtime/ge/ge.h"

#include "lib/common/hash.h"
#include "lib/common/log.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace p2::ge {

namespace {

constexpr u32 kErrInvalidId = 0x80000100;
constexpr u32 kErrInvalidAddr = 0x80000103;
constexpr u32 kErrOutOfMemory = 0x80000022;
constexpr int kMaxLists = 64;
constexpr int kMaxCallbacks = 16;

enum class ListState : u8 { Free, Queued, Running, Stalled, Done };

struct List {
    ListState state = ListState::Free;
    u32 pc = 0;
    u32 stall = 0;
    s32 callback = -1;
    u32 call_stack[32];
    u32 offset_stack[32];
    u32 call_depth = 0;
    bool finished = false;  // FINISH seen: the list ends at the next END
    u32 finish_arg = 0;
    bool signalled = false; // SIGNAL seen: the next END carries its second half
    u32 signal_arg = 0;
};

struct Callbacks {
    bool used = false;
    u32 signal_fn = 0, signal_arg = 0, finish_fn = 0, finish_arg = 0;
};

State g_state;
Renderer* g_renderer = nullptr;
Stats g_stats;
List g_lists[kMaxLists];
Callbacks g_callbacks[kMaxCallbacks];
std::vector<int> g_queue;
bool g_executing = false;
u32 g_vertex_addr = 0, g_index_addr = 0, g_offset_addr = 0;
bool g_texture_touched = true; // a texture command ran since the last textured draw

std::vector<Vertex> g_vertices;
std::vector<u16> g_indices;
std::vector<Vertex> g_out_vertices;

// --- vertex formats ---------------------------------------------------------------------------------------------

struct Layout {
    u32 size = 0;
    u32 weight_off = 0, tc_off = 0, color_off = 0, normal_off = 0, pos_off = 0;
    u32 tc_fmt = 0, color_fmt = 0, normal_fmt = 0, pos_fmt = 0, weight_fmt = 0, weight_count = 0, index_fmt = 0;
    u32 morph_count = 1;
    bool through = false;
};

Layout make_layout(u32 vtype) {
    static constexpr u32 sizes[4] = {0, 1, 2, 4};
    static constexpr u32 color_sizes[8] = {0, 0, 0, 0, 2, 2, 2, 4};
    Layout l;
    l.tc_fmt = vtype & 3;
    l.color_fmt = (vtype >> 2) & 7;
    l.normal_fmt = (vtype >> 5) & 3;
    l.pos_fmt = (vtype >> 7) & 3;
    l.weight_fmt = (vtype >> 9) & 3;
    l.index_fmt = (vtype >> 11) & 3;
    l.weight_count = l.weight_fmt ? ((vtype >> 14) & 7) + 1 : 0;
    l.morph_count = ((vtype >> 18) & 7) + 1;
    l.through = (vtype >> 23) & 1;

    u32 offset = 0, biggest = 1;
    auto place = [&](u32 element, u32 count) {
        if (!element) return 0u;
        offset = align_up(offset, element);
        u32 at = offset;
        offset += element * count;
        biggest = std::max(biggest, element);
        return at;
    };
    l.weight_off = place(sizes[l.weight_fmt], l.weight_count);
    l.tc_off = place(sizes[l.tc_fmt], 2);
    l.color_off = place(color_sizes[l.color_fmt], 1);
    l.normal_off = place(sizes[l.normal_fmt], 3);
    l.pos_off = place(sizes[l.pos_fmt], 3);
    l.size = align_up(offset, biggest) * l.morph_count;
    return l;
}

struct RawVertex {
    f32 pos[3] = {};
    f32 normal[3] = {0, 0, 1};
    f32 uv[2] = {};
    u32 color = 0xFFFFFFFFu;
    f32 weights[8] = {};
    bool has_color = false;
};

u32 read_color(const u8* p, u32 fmt) {
    switch (fmt) {
    case 4: {
        u32 c = load_le<u16>(p);
        u32 r = c & 31, g = (c >> 5) & 63, b = (c >> 11) & 31;
        return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
    }
    case 5: {
        u32 c = load_le<u16>(p);
        u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        return ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16) | ((c & 0x8000) ? 0xFF000000u : 0);
    }
    case 6: {
        u32 c = load_le<u16>(p);
        u32 r = c & 15, g = (c >> 4) & 15, b = (c >> 8) & 15, a = c >> 12;
        return (r * 17) | ((g * 17) << 8) | ((b * 17) << 16) | ((a * 17) << 24);
    }
    case 7: return load_le<u32>(p);
    default: return 0xFFFFFFFFu;
    }
}

void read_vertex(const Layout& l, const u8* p, RawVertex& v) {
    for (u32 i = 0; i < l.weight_count; ++i) {
        const u8* w = p + l.weight_off;
        switch (l.weight_fmt) {
        case 1: v.weights[i] = f32(w[i]) / 128.0f; break;
        case 2: v.weights[i] = f32(load_le<u16>(w + i * 2)) / 32768.0f; break;
        default: v.weights[i] = load_le<f32>(w + i * 4); break;
        }
    }
    if (l.tc_fmt) {
        const u8* t = p + l.tc_off;
        switch (l.tc_fmt) {
        case 1:
            v.uv[0] = f32(t[0]);
            v.uv[1] = f32(t[1]);
            if (!l.through) {
                v.uv[0] /= 128.0f;
                v.uv[1] /= 128.0f;
            }
            break;
        case 2:
            v.uv[0] = f32(load_le<u16>(t));
            v.uv[1] = f32(load_le<u16>(t + 2));
            if (!l.through) {
                v.uv[0] /= 32768.0f;
                v.uv[1] /= 32768.0f;
            }
            break;
        default:
            v.uv[0] = load_le<f32>(t);
            v.uv[1] = load_le<f32>(t + 4);
            break;
        }
    }
    if (l.color_fmt >= 4) {
        v.color = read_color(p + l.color_off, l.color_fmt);
        v.has_color = true;
    }
    if (l.normal_fmt) {
        const u8* n = p + l.normal_off;
        for (u32 i = 0; i < 3; ++i) {
            switch (l.normal_fmt) {
            case 1: v.normal[i] = f32(s8(n[i])) / 127.0f; break;
            case 2: v.normal[i] = f32(load_le<s16>(n + i * 2)) / 32767.0f; break;
            default: v.normal[i] = load_le<f32>(n + i * 4); break;
            }
        }
    }
    const u8* q = p + l.pos_off;
    if (l.through) {
        switch (l.pos_fmt) {
        case 1:
            v.pos[0] = f32(s8(q[0]));
            v.pos[1] = f32(s8(q[1]));
            v.pos[2] = f32(q[2]);
            break;
        case 2:
            v.pos[0] = f32(load_le<s16>(q));
            v.pos[1] = f32(load_le<s16>(q + 2));
            v.pos[2] = f32(load_le<u16>(q + 4));
            break;
        default:
            v.pos[0] = load_le<f32>(q);
            v.pos[1] = load_le<f32>(q + 4);
            v.pos[2] = load_le<f32>(q + 8);
            break;
        }
    } else {
        for (u32 i = 0; i < 3; ++i) {
            switch (l.pos_fmt) {
            case 1: v.pos[i] = f32(s8(q[i])) / 128.0f; break;
            case 2: v.pos[i] = f32(load_le<s16>(q + i * 2)) / 32768.0f; break;
            default: v.pos[i] = load_le<f32>(q + i * 4); break;
            }
        }
    }
}

// --- transform and lighting -------------------------------------------------------------------------------------

void mul43(const f32* m, const f32* in, f32* out) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6] + m[9];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7] + m[10];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8] + m[11];
}

void rot33(const f32* m, const f32* in, f32* out) {
    out[0] = in[0] * m[0] + in[1] * m[3] + in[2] * m[6];
    out[1] = in[0] * m[1] + in[1] * m[4] + in[2] * m[7];
    out[2] = in[0] * m[2] + in[1] * m[5] + in[2] * m[8];
}

void normalize(f32* v) {
    f32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.0f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

f32 dot3(const f32* a, const f32* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void unpack_rgb(u32 c, f32* out) {
    out[0] = f32(c & 0xFF) / 255.0f;
    out[1] = f32((c >> 8) & 0xFF) / 255.0f;
    out[2] = f32((c >> 16) & 0xFF) / 255.0f;
}

u32 pack_color(const f32* rgb, f32 alpha) {
    auto ch = [](f32 v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(rgb[0]) | (ch(rgb[1]) << 8) | (ch(rgb[2]) << 16) | (ch(alpha) << 24);
}

// Per-vertex lighting in world space with up to four lights.
void light_vertex(const State& s, const f32* world_pos, const f32* normal, const RawVertex& raw, u32* color, u32* specular) {
    const u32 update = s.arg[kMaterialUpdate] & 7;
    f32 vcol[3];
    unpack_rgb(raw.color, vcol);
    f32 ambient[3], diffuse[3], spec[3], emissive[3], global[3];
    unpack_rgb(s.arg[kMaterialAmbient], ambient);
    unpack_rgb(s.arg[kMaterialDiffuse], diffuse);
    unpack_rgb(s.arg[kMaterialSpecular], spec);
    unpack_rgb(s.arg[kMaterialEmissive], emissive);
    unpack_rgb(s.arg[kAmbientColor], global);
    f32 alpha = f32(s.arg[kMaterialAlpha] & 0xFF) / 255.0f;
    if (raw.has_color) {
        if (update & 1) {
            std::memcpy(ambient, vcol, sizeof(vcol));
            alpha = f32(raw.color >> 24) / 255.0f;
        }
        if (update & 2) std::memcpy(diffuse, vcol, sizeof(vcol));
        if (update & 4) std::memcpy(spec, vcol, sizeof(vcol));
    }

    f32 out[3], out_spec[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) out[i] = emissive[i] + global[i] * ambient[i];
    const f32 shininess = float24(s.arg[kMaterialSpecularCoef]);

    for (u32 light = 0; light < 4; ++light) {
        if (!s.light_on(light)) continue;
        const u32 type = (s.arg[kLightType0 + light] >> 8) & 3; // 0 directional, 1 point, 2 spot
        const u32 kind = s.arg[kLightType0 + light] & 3;        // 0 diffuse, 1 diffuse + specular, 2 powered diffuse
        f32 lpos[3], ldir[3], att[3], la[3], ld[3], ls[3];
        for (int i = 0; i < 3; ++i) {
            lpos[i] = float24(s.arg[kLightPos0 + light * 3 + i]);
            ldir[i] = float24(s.arg[kLightDir0 + light * 3 + i]);
            att[i] = float24(s.arg[kLightAtt0 + light * 3 + i]);
        }
        unpack_rgb(s.arg[kLightAmbient0 + light * 3 + 0], la);
        unpack_rgb(s.arg[kLightAmbient0 + light * 3 + 1], ld);
        unpack_rgb(s.arg[kLightAmbient0 + light * 3 + 2], ls);

        f32 to_light[3];
        f32 attenuation = 1.0f;
        if (type == 0) {
            std::memcpy(to_light, lpos, sizeof(lpos));
            normalize(to_light);
        } else {
            for (int i = 0; i < 3; ++i) to_light[i] = lpos[i] - world_pos[i];
            f32 dist = std::sqrt(dot3(to_light, to_light));
            normalize(to_light);
            f32 denom = att[0] + att[1] * dist + att[2] * dist * dist;
            attenuation = denom > 0.0f ? std::clamp(1.0f / denom, 0.0f, 1.0f) : 1.0f;
            if (type == 2) {
                normalize(ldir);
                f32 cone = -dot3(to_light, ldir);
                f32 cutoff = float24(s.arg[kLightSpotCutoff0 + light]);
                if (cone < cutoff) attenuation = 0.0f;
                else attenuation *= std::pow(std::max(cone, 0.0f), float24(s.arg[kLightSpotExp0 + light]));
            }
        }

        f32 ndotl = std::max(dot3(normal, to_light), 0.0f);
        f32 diffuse_factor = kind == 2 ? std::pow(ndotl, shininess) : ndotl;
        for (int i = 0; i < 3; ++i) out[i] += attenuation * (la[i] * ambient[i] + ld[i] * diffuse[i] * diffuse_factor);

        if (kind == 1 && ndotl > 0.0f) {
            f32 half[3] = {to_light[0], to_light[1], to_light[2] + 1.0f};
            normalize(half);
            f32 ndoth = std::max(dot3(normal, half), 0.0f);
            f32 factor = std::pow(ndoth, shininess) * attenuation;
            for (int i = 0; i < 3; ++i) out_spec[i] += ls[i] * spec[i] * factor;
        }
    }

    if (s.arg[kLightMode] & 1) { // separate specular colour
        *specular = pack_color(out_spec, 0.0f);
    } else {
        for (int i = 0; i < 3; ++i) out[i] += out_spec[i];
        *specular = 0;
    }
    *color = pack_color(out, alpha);
}

void transform_vertex(const State& s, const Layout& l, const RawVertex& raw, f32 tex_w, f32 tex_h, Vertex& out) {
    out.specular = 0;
    out.fog = 1.0f;
    out.q = 1.0f;

    if (l.through) {
        out.x = raw.pos[0];
        out.y = raw.pos[1];
        out.z = std::clamp(raw.pos[2], 0.0f, 65535.0f) / 65535.0f;
        out.w = 1.0f;
        out.u = raw.uv[0] / tex_w;
        out.v = raw.uv[1] / tex_h;
        out.color = raw.has_color ? raw.color : ((s.arg[kMaterialAmbient] & 0xFFFFFF) | ((s.arg[kMaterialAlpha] & 0xFF) << 24));
        return;
    }

    // Skinning blends the bone transforms before the world transform.
    f32 model[3], normal[3];
    if (l.weight_count) {
        model[0] = model[1] = model[2] = 0.0f;
        normal[0] = normal[1] = normal[2] = 0.0f;
        for (u32 i = 0; i < l.weight_count; ++i) {
            if (raw.weights[i] == 0.0f) continue;
            f32 p[3], n[3];
            mul43(s.bone + i * 12, raw.pos, p);
            rot33(s.bone + i * 12, raw.normal, n);
            for (int k = 0; k < 3; ++k) {
                model[k] += p[k] * raw.weights[i];
                normal[k] += n[k] * raw.weights[i];
            }
        }
    } else {
        std::memcpy(model, raw.pos, sizeof(model));
        std::memcpy(normal, raw.normal, sizeof(normal));
    }
    if (s.arg[kReverseNormal] & 1) {
        for (f32& n : normal) n = -n;
    }

    f32 world[3], view[3];
    mul43(s.world, model, world);
    mul43(s.view, world, view);
    const f32* p = s.proj;
    f32 cx = view[0] * p[0] + view[1] * p[4] + view[2] * p[8] + p[12];
    f32 cy = view[0] * p[1] + view[1] * p[5] + view[2] * p[9] + p[13];
    f32 cz = view[0] * p[2] + view[1] * p[6] + view[2] * p[10] + p[14];
    f32 cw = view[0] * p[3] + view[1] * p[7] + view[2] * p[11] + p[15];

    // Viewport transform folded into the homogeneous coordinates.
    out.x = cx * s.vp_xscale() + (s.vp_xcenter() - s.offset_x()) * cw;
    out.y = cy * s.vp_yscale() + (s.vp_ycenter() - s.offset_y()) * cw;
    out.z = (cz * s.vp_zscale() + s.vp_zcenter() * cw) / 65535.0f;
    out.w = cw;

    f32 world_normal[3];
    rot33(s.world, normal, world_normal);
    normalize(world_normal);

    if (s.lighting()) {
        light_vertex(s, world, world_normal, raw, &out.color, &out.specular);
    } else {
        out.color = raw.has_color ? raw.color : ((s.arg[kMaterialAmbient] & 0xFFFFFF) | ((s.arg[kMaterialAlpha] & 0xFF) << 24));
    }

    switch (s.tex_map_mode()) {
    case 1: { // projection through the texture matrix
        f32 src[3];
        switch (s.tex_proj_source()) {
        case 0: std::memcpy(src, model, sizeof(src)); break;
        case 1:
            src[0] = raw.uv[0];
            src[1] = raw.uv[1];
            src[2] = 0.0f;
            break;
        case 2:
            std::memcpy(src, normal, sizeof(src));
            normalize(src);
            break;
        default: std::memcpy(src, normal, sizeof(src)); break;
        }
        f32 t[3];
        mul43(s.tgen, src, t);
        out.u = t[0];
        out.v = t[1];
        out.q = t[2];
        break;
    }
    case 2: { // environment map from two light directions
        u32 l0 = s.arg[kTexShadeLs] & 3, l1 = (s.arg[kTexShadeLs] >> 8) & 3;
        f32 a[3], b[3];
        for (int i = 0; i < 3; ++i) {
            a[i] = float24(s.arg[kLightPos0 + l0 * 3 + i]);
            b[i] = float24(s.arg[kLightPos0 + l1 * 3 + i]);
        }
        normalize(a);
        normalize(b);
        out.u = (1.0f + dot3(a, world_normal)) * 0.5f;
        out.v = (1.0f + dot3(b, world_normal)) * 0.5f;
        break;
    }
    default:
        out.u = raw.uv[0] * s.tex_scale_u() + s.tex_offset_u();
        out.v = raw.uv[1] * s.tex_scale_v() + s.tex_offset_v();
        break;
    }

    if (s.fog_enabled()) {
        f32 fog_end = float24(s.arg[kFog1]), fog_slope = float24(s.arg[kFog2]);
        out.fog = std::clamp((view[2] + fog_end) * fog_slope, 0.0f, 1.0f);
    }
}

// --- draw state -------------------------------------------------------------------------------------------------

void fill_draw_state(const State& s, DrawState& d) {
    d.fb_addr = s.fb_addr();
    d.fb_stride = s.fb_stride();
    d.fb_format = s.fb_format();
    d.zb_addr = s.zb_addr();

    d.clear_mode = s.clear_mode();
    d.clear_color = (s.arg[kClearMode] >> 8) & 1;
    d.clear_alpha = (s.arg[kClearMode] >> 9) & 1;
    d.clear_depth = (s.arg[kClearMode] >> 10) & 1;

    d.scissor_x1 = s32(s.scissor_x1());
    d.scissor_y1 = s32(s.scissor_y1());
    d.scissor_x2 = s32(s.scissor_x2()) + 1;
    d.scissor_y2 = s32(s.scissor_y2()) + 1;

    d.depth_test = s.depth_test_enabled();
    d.depth_func = u8(s.depth_func());
    d.depth_write = s.depth_write();

    d.blend = s.blend_enabled();
    d.blend_src = u8(s.blend_src());
    d.blend_dst = u8(s.blend_dst());
    d.blend_eq = u8(s.blend_eq());
    d.blend_fix_a = s.arg[kBlendFixedA];
    d.blend_fix_b = s.arg[kBlendFixedB];

    d.alpha_test = s.alpha_test_enabled();
    d.alpha_func = u8(s.alpha_func());
    d.alpha_ref = u8(s.alpha_ref());
    d.alpha_mask = u8(s.alpha_mask());
    d.color_test = s.color_test_enabled();
    d.color_func = u8(s.color_func());
    d.color_ref = s.arg[kColorRef];
    d.color_mask = s.arg[kColorTestMask];

    d.stencil_test = s.stencil_test_enabled();
    d.stencil_func = u8(s.stencil_func());
    d.stencil_ref = u8(s.stencil_ref());
    d.stencil_mask = u8(s.stencil_mask());
    d.stencil_sfail = u8(s.stencil_sfail());
    d.stencil_zfail = u8(s.stencil_zfail());
    d.stencil_zpass = u8(s.stencil_zpass());

    d.write_mask_rgb = s.arg[kMaskRgb];
    d.write_mask_alpha = s.arg[kMaskAlpha] & 0xFF;

    d.fog = s.fog_enabled() && !s.through();
    d.fog_color = s.arg[kFogColor];

    d.texture = s.texture_enabled() && !d.clear_mode;
    d.tex_func = u8(s.tex_func());
    d.tex_alpha = s.tex_uses_alpha();
    d.color_double = s.tex_color_double();
    d.tex_env_color = s.arg[kTexEnvColor];
    d.flat_shade = s.flat_shading();

    if (d.texture) {
        TextureDesc& t = d.tex;
        t.revalidate = g_texture_touched;
        g_texture_touched = false;
        t.addr = s.tex_addr(0);
        t.format = s.tex_format();
        t.width = s.tex_width(0);
        t.height = s.tex_height(0);
        t.buf_width = s.tex_buf_width(0);
        t.swizzled = s.tex_swizzled();
        t.linear_min = s.tex_min_linear();
        t.linear_mag = s.tex_mag_linear();
        t.clamp_s = s.tex_clamp_s();
        t.clamp_t = s.tex_clamp_t();
        if (t.format >= kTexClut4 && t.format <= kTexClut32) {
            t.clut_format = s.clut_format();
            t.clut_shift = s.clut_shift();
            t.clut_mask = s.clut_mask();
            t.clut_base = s.clut_base();
            // Only the entries this texture can reach matter for its identity.
            u32 entries = t.format == kTexClut4 ? 16 : 256;
            u32 bytes = std::min<u32>((t.clut_base + entries) * (t.clut_format == 3 ? 4 : 2), sizeof(s.clut));
            t.clut_hash = fnv1a64(s.clut, bytes);
        }
    }
}

// --- primitives -------------------------------------------------------------------------------------------------

void draw_primitive(u32 prim_type, u32 count) {
    const State& s = g_state;
    const Layout layout = make_layout(s.vtype());
    static constexpr u32 index_sizes[4] = {0, 1, 2, 4};
    const u32 index_size = index_sizes[layout.index_fmt];

    const u32 vertex_base = g_vertex_addr, index_base = g_index_addr;
    // Whatever happens, the next primitive continues after this one's data.
    if (index_size) g_index_addr += count * index_size;
    else g_vertex_addr += count * layout.size;

    if (count == 0 || !g_renderer || layout.pos_fmt == 0) return;
    if (prim_type > kSprites) return;
    if (index_size && !mem::valid(index_base, count * index_size)) return;

    DrawState draw;
    fill_draw_state(s, draw);
    const f32 tex_w = draw.texture ? f32(draw.tex.width) : 1.0f;
    const f32 tex_h = draw.texture ? f32(draw.tex.height) : 1.0f;

    g_vertices.resize(count);
    for (u32 i = 0; i < count; ++i) {
        u32 index = i;
        if (index_size == 1) index = mem::r8(index_base + i);
        else if (index_size == 2) index = mem::r16(index_base + i * 2);
        else if (index_size == 4) index = mem::r32(index_base + i * 4);
        u32 addr = vertex_base + index * layout.size;
        if (!mem::valid(addr, layout.size)) return;
        RawVertex raw;
        read_vertex(layout, mem::ptr(addr), raw);
        transform_vertex(s, layout, raw, tex_w, tex_h, g_vertices[i]);
    }
    g_stats.vertices += count;
    ++g_stats.draws;

    g_indices.clear();
    g_out_vertices.clear();
    Topology topology = Topology::Triangles;
    const bool cull = s.cull_enabled() && !layout.through && !draw.clear_mode;
    const bool keep_clockwise = s.cull_ccw();

    auto triangle = [&](u32 a, u32 b, u32 c) {
        if (cull) {
            const Vertex &va = g_vertices[a], &vb = g_vertices[b], &vc = g_vertices[c];
            if (va.w > 0.0f && vb.w > 0.0f && vc.w > 0.0f) {
                f32 ax = va.x / va.w, ay = va.y / va.w;
                f32 area = (vb.x / vb.w - ax) * (vc.y / vc.w - ay) - (vc.x / vc.w - ax) * (vb.y / vb.w - ay);
                if (keep_clockwise ? area < 0.0f : area > 0.0f) return;
            }
        }
        g_indices.push_back(u16(a));
        g_indices.push_back(u16(b));
        g_indices.push_back(u16(c));
    };

    switch (prim_type) {
    case kPoints:
        topology = Topology::Points;
        for (u32 i = 0; i < count; ++i) g_indices.push_back(u16(i));
        break;
    case kLines:
        topology = Topology::Lines;
        for (u32 i = 0; i + 1 < count; i += 2) {
            g_indices.push_back(u16(i));
            g_indices.push_back(u16(i + 1));
        }
        break;
    case kLineStrip:
        topology = Topology::Lines;
        for (u32 i = 0; i + 1 < count; ++i) {
            g_indices.push_back(u16(i));
            g_indices.push_back(u16(i + 1));
        }
        break;
    case kTriangles:
        for (u32 i = 0; i + 2 < count; i += 3) triangle(i, i + 1, i + 2);
        break;
    case kTriangleStrip:
        for (u32 i = 0; i + 2 < count; ++i) {
            if (i & 1) triangle(i + 1, i, i + 2);
            else triangle(i, i + 1, i + 2);
        }
        break;
    case kTriangleFan:
        for (u32 i = 1; i + 1 < count; ++i) triangle(0, i, i + 1);
        break;
    case kSprites: {
        // Two corners make an axis-aligned rectangle; colour and depth come from the second corner.
        for (u32 i = 0; i + 1 < count; i += 2) {
            Vertex a = g_vertices[i], b = g_vertices[i + 1];
            if (a.w != 1.0f || b.w != 1.0f) { // transformed sprites: work in screen space
                if (a.w <= 0.0f || b.w <= 0.0f) continue;
                a.x /= a.w, a.y /= a.w, b.x /= b.w, b.y /= b.w, b.z /= b.w;
                a.w = b.w = 1.0f;
            }
            Vertex quad[4] = {b, b, b, b};
            quad[0].x = a.x, quad[0].y = a.y, quad[0].u = a.u, quad[0].v = a.v;
            quad[1].x = b.x, quad[1].y = a.y, quad[1].u = b.u, quad[1].v = a.v;
            quad[2].x = a.x, quad[2].y = b.y, quad[2].u = a.u, quad[2].v = b.v;
            // A rectangle flipped on exactly one axis has its texture rotated a quarter turn.
            if ((a.x < b.x) != (a.y < b.y)) {
                std::swap(quad[1].u, quad[2].u);
                std::swap(quad[1].v, quad[2].v);
            }
            u32 base = u32(g_out_vertices.size());
            if (base + 4 > 65535) break;
            g_out_vertices.insert(g_out_vertices.end(), quad, quad + 4);
            const u16 idx[6] = {u16(base), u16(base + 1), u16(base + 2), u16(base + 1), u16(base + 3), u16(base + 2)};
            g_indices.insert(g_indices.end(), idx, idx + 6);
        }
        break;
    }
    }

    if (g_indices.empty()) return;
    if (prim_type == kSprites)
        g_renderer->draw(draw, topology, g_out_vertices.data(), u32(g_out_vertices.size()), g_indices.data(), u32(g_indices.size()));
    else
        g_renderer->draw(draw, topology, g_vertices.data(), count, g_indices.data(), u32(g_indices.size()));
}

void block_transfer() {
    const State& s = g_state;
    u32 src = (s.arg[kTransferSrc] & 0xFFFFF0) | ((s.arg[kTransferSrcW] & 0xFF0000) << 8);
    u32 dst = (s.arg[kTransferDst] & 0xFFFFF0) | ((s.arg[kTransferDstW] & 0xFF0000) << 8);
    u32 src_stride = s.arg[kTransferSrcW] & 0x7F8, dst_stride = s.arg[kTransferDstW] & 0x7F8;
    u32 sx = s.arg[kTransferSrcPos] & 0x3FF, sy = (s.arg[kTransferSrcPos] >> 10) & 0x3FF;
    u32 dx = s.arg[kTransferDstPos] & 0x3FF, dy = (s.arg[kTransferDstPos] >> 10) & 0x3FF;
    u32 w = (s.arg[kTransferSize] & 0x3FF) + 1, h = ((s.arg[kTransferSize] >> 10) & 0x3FF) + 1;
    u32 bpp = (s.arg[kTransferStart] & 1) ? 4 : 2;
    LOG_CH("ge", "block transfer %08x(%u,%u)/%u -> %08x(%u,%u)/%u  %ux%u  %u bpp", src, sx, sy, src_stride, dst, dx, dy,
           dst_stride, w, h, bpp * 8);
    if (g_renderer) g_renderer->block_transfer(src, src_stride, sx, sy, dst, dst_stride, dx, dy, w, h, bpp);
}

void load_clut() {
    State& s = g_state;
    u32 addr = s.clut_addr();
    u32 bytes = std::min<u32>((s.arg[kLoadClut] & 0x3F) * 32, sizeof(s.clut));
    if (bytes && mem::valid(addr, bytes)) std::memcpy(s.clut, mem::ptr(addr), bytes);
    ++s.clut_serial;
}

// --- display lists ----------------------------------------------------------------------------------------------

u32 relative_address(u32 arg) { return ((((g_state.arg[kBase] & 0x000F0000) << 8) | arg) + g_offset_addr) & 0x0FFFFFFF; }

void matrix_data(u8 number_cmd, f32* matrix, u32 size, u32 arg) {
    u32& index = g_state.arg[number_cmd];
    if ((index & 0xFF) < size) matrix[index & 0xFF] = float24(arg);
    ++index;
}

void call_guest_handler(u32 fn, u32 id, u32 arg, u32 pc) {
    if (fn) kernel::call_guest_interrupt(fn, id, arg, pc);
}

void end_of_list(List& list, u32 end_arg) {
    if (list.signalled) {
        list.signalled = false;
        const u32 behaviour = (list.signal_arg >> 16) & 0xFF, id = list.signal_arg & 0xFFFF;
        switch (behaviour) {
        case 0x01: // the handler runs and the list carries on
        case 0x02:
        case 0x03:
            if (list.callback >= 0 && g_callbacks[list.callback].used)
                call_guest_handler(g_callbacks[list.callback].signal_fn, id, g_callbacks[list.callback].signal_arg, list.pc);
            break;
        case 0x10: // jump
            list.pc = ((list.signal_arg & 0xFFFF) << 16) | (end_arg & 0xFFFF);
            break;
        case 0x11: // call
            if (list.call_depth < 32) {
                list.call_stack[list.call_depth] = list.pc;
                list.offset_stack[list.call_depth] = g_offset_addr;
                ++list.call_depth;
            }
            list.pc = ((list.signal_arg & 0xFFFF) << 16) | (end_arg & 0xFFFF);
            break;
        case 0x12: // return
            if (list.call_depth > 0) {
                --list.call_depth;
                list.pc = list.call_stack[list.call_depth];
                g_offset_addr = list.offset_stack[list.call_depth];
            }
            break;
        default: LOG_WARN("ge", "unhandled display-list signal %02x", behaviour); break;
        }
        return;
    }
    if (list.finished) {
        list.state = ListState::Done;
        if (list.callback >= 0 && g_callbacks[list.callback].used)
            call_guest_handler(g_callbacks[list.callback].finish_fn, list.finish_arg & 0xFFFF,
                               g_callbacks[list.callback].finish_arg, list.pc);
        return;
    }
    // A bare END without FINISH also stops the list.
    list.state = ListState::Done;
}

void execute(List& list, u32 cmd, u32 arg) {
    State& s = g_state;
    switch (cmd) {
    case kVaddr: g_vertex_addr = relative_address(arg); break;
    case kIaddr: g_index_addr = relative_address(arg); break;
    case kPrim: draw_primitive((arg >> 16) & 7, arg & 0xFFFF); break;
    case kBezier:
    case kSpline: {
        static bool warned = false;
        if (!warned) LOG_WARN("ge", "curved surfaces are not drawn");
        warned = true;
        break;
    }
    case kBbox: break;
    case kJump: list.pc = relative_address(arg & 0xFFFFFC); break;
    case kBjump: break; // bounding-box test never fails here
    case kCall:
        if (list.call_depth < 32) {
            list.call_stack[list.call_depth] = list.pc;
            list.offset_stack[list.call_depth] = g_offset_addr;
            ++list.call_depth;
            list.pc = relative_address(arg & 0xFFFFFC);
        }
        break;
    case kRet:
        if (list.call_depth > 0) {
            --list.call_depth;
            list.pc = list.call_stack[list.call_depth];
            g_offset_addr = list.offset_stack[list.call_depth];
        }
        break;
    case kEnd: end_of_list(list, arg); break;
    case kSignal:
        list.signalled = true;
        list.signal_arg = arg;
        break;
    case kFinish:
        list.finished = true;
        list.finish_arg = arg;
        break;
    case kOffsetAddr: g_offset_addr = arg << 8; break;
    case kOriginAddr: g_offset_addr = list.pc - 4; break;
    case kBoneMatrixData: matrix_data(kBoneMatrixNumber, s.bone, 96, arg); break;
    case kWorldMatrixData: matrix_data(kWorldMatrixNumber, s.world, 12, arg); break;
    case kViewMatrixData: matrix_data(kViewMatrixNumber, s.view, 12, arg); break;
    case kProjMatrixData: matrix_data(kProjMatrixNumber, s.proj, 16, arg); break;
    case kTgenMatrixData: matrix_data(kTgenMatrixNumber, s.tgen, 12, arg); break;
    case kLoadClut:
        s.arg[cmd] = arg;
        load_clut();
        break;
    case kTransferStart:
        s.arg[cmd] = arg;
        block_transfer();
        break;
    case kTexFlush:
    case kTexSync: g_texture_touched = true; break;
    case kTexAddr0:
    case kTexBufWidth0:
    case kTexSize0:
    case kTexFormat:
    case kTexMode:
        s.arg[cmd] = arg;
        g_texture_touched = true;
        break;
    default: s.arg[cmd] = arg; break;
    }
}

void run_list(int id) {
    List& list = g_lists[id];
    list.state = ListState::Running;
    while (list.state == ListState::Running) {
        if (list.stall && ((list.pc ^ list.stall) & 0x0FFFFFFF) == 0) {
            list.state = ListState::Stalled;
            break;
        }
        if (!mem::valid(list.pc, 4)) {
            LOG_ERROR("ge", "display list %d ran into unmapped memory at %08x", id, list.pc);
            list.state = ListState::Done;
            break;
        }
        u32 word = mem::r32(list.pc);
        list.pc += 4;
        ++g_stats.commands;
        execute(list, word >> 24, word & 0xFFFFFF);
    }
}

// Runs queued lists in order. Re-entrant submissions (from a finish handler) are picked up by the
// outer loop instead of nesting.
void process_queue() {
    if (g_executing) return;
    g_executing = true;
    while (!g_queue.empty()) {
        int id = g_queue.front();
        List& list = g_lists[id];
        if (list.state == ListState::Stalled && list.stall && ((list.pc ^ list.stall) & 0x0FFFFFFF) == 0) break;
        run_list(id);
        if (list.state == ListState::Stalled) break; // later lists wait behind a stalled one
        g_queue.erase(g_queue.begin());
    }
    g_executing = false;
}

} // namespace

void State::reset() {
    *this = State{};
    static constexpr f32 identity43[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::memcpy(world, identity43, sizeof(world));
    std::memcpy(view, identity43, sizeof(view));
    std::memcpy(tgen, identity43, sizeof(tgen));
    for (int i = 0; i < 8; ++i) std::memcpy(bone + i * 12, identity43, sizeof(identity43));
    proj[0] = proj[5] = proj[10] = proj[15] = 1.0f;
    arg[kFramebufWidth] = 512;
    arg[kScissor2] = 479 | (271 << 10);
    arg[kTexScaleU] = arg[kTexScaleV] = 0x3F8000; // 1.0
    arg[kMaterialAmbient] = arg[kMaterialDiffuse] = arg[kMaterialSpecular] = 0xFFFFFF;
    arg[kMaterialAlpha] = 0xFF;
    arg[kShadeMode] = 1;
}

void init(Renderer* renderer) {
    g_renderer = renderer;
    g_state.reset();
}

Renderer* renderer() { return g_renderer; }
State& state() { return g_state; }
Stats& stats() { return g_stats; }

s32 enqueue(u32 list_addr, u32 stall_addr, s32 callback_id, bool head) {
    if (!mem::valid(list_addr, 4)) return s32(kErrInvalidAddr);
    int id = -1;
    for (int i = 0; i < kMaxLists; ++i) {
        if (g_lists[i].state == ListState::Free || g_lists[i].state == ListState::Done) {
            id = i;
            break;
        }
    }
    if (id < 0) return s32(kErrOutOfMemory);
    List& list = g_lists[id];
    list = List{};
    list.state = ListState::Queued;
    list.pc = list_addr;
    list.stall = stall_addr;
    list.callback = callback_id >= 0 && callback_id < kMaxCallbacks ? callback_id : -1;
    ++g_stats.lists;
    LOG_CH("ge", "enqueue list %d at %08x stall %08x callback %d%s", id, list_addr, stall_addr, callback_id, head ? " (head)" : "");
    if (head) g_queue.insert(g_queue.begin(), id);
    else g_queue.push_back(id);
    process_queue();
    return id;
}

s32 update_stall(s32 id, u32 stall_addr) {
    if (id < 0 || id >= kMaxLists || g_lists[id].state == ListState::Free) return s32(kErrInvalidId);
    List& list = g_lists[id];
    list.stall = stall_addr;
    if (list.state == ListState::Stalled) list.state = ListState::Queued;
    process_queue();
    return 0;
}

s32 list_sync(s32 id, u32 mode) {
    if (id < 0 || id >= kMaxLists || g_lists[id].state == ListState::Free) return s32(kErrInvalidId);
    if (mode == 0) return 0;
    switch (g_lists[id].state) {
    case ListState::Done: return 0;
    case ListState::Queued: return 1;
    case ListState::Running: return 2;
    case ListState::Stalled: return 3;
    default: return 0;
    }
}

s32 draw_sync(u32 mode) {
    if (mode == 0) return 0;
    for (int id : g_queue) {
        if (g_lists[id].state == ListState::Stalled) return 3;
        if (g_lists[id].state == ListState::Running) return 2;
    }
    return 0;
}

s32 set_callback(u32 signal_fn, u32 signal_arg, u32 finish_fn, u32 finish_arg) {
    for (int i = 0; i < kMaxCallbacks; ++i) {
        if (!g_callbacks[i].used) {
            g_callbacks[i] = {true, signal_fn, signal_arg, finish_fn, finish_arg};
            return i;
        }
    }
    return s32(kErrOutOfMemory);
}

s32 unset_callback(s32 id) {
    if (id < 0 || id >= kMaxCallbacks) return s32(kErrInvalidId);
    g_callbacks[id].used = false;
    return 0;
}

} // namespace p2::ge
