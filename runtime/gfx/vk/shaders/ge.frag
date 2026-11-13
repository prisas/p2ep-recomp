#version 450

// One shader for the whole fixed-function pixel pipeline; the state word selects the stages.

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform Params {
    vec2 pixel_to_ndc;
    vec2 uv_scale;
    vec4 tex_env_color;
    vec4 fog_color;
    uint flags;
    uint alpha_test; // func | ref << 8 | mask << 16
    uint color_ref;
    uint color_mask;
    float stencil_value;
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) flat in vec4 v_color_flat;
layout(location = 2) in vec3 v_specular;
layout(location = 3) in vec3 v_texcoord;
layout(location = 4) in float v_fog;

// Output 0 is what gets stored: colour, plus the value destined for the alpha channel, which on
// this hardware doubles as the stencil buffer. Output 1 carries the alpha used as a blend factor.
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_blend;

const uint F_TEXTURE = 1u << 0;
const uint F_TEX_ALPHA = 1u << 4;
const uint F_COLOR_DOUBLE = 1u << 5;
const uint F_FOG = 1u << 6;
const uint F_ALPHA_TEST = 1u << 7;
const uint F_COLOR_TEST = 1u << 8;
const uint F_FLAT = 1u << 11;
const uint F_CLEAR = 1u << 12;
const uint F_WRITE_STENCIL = 1u << 13;
const uint F_DOUBLE_ALPHA = 1u << 14;
const uint F_PROJECT = 1u << 15;

bool compare(uint func, int value, int ref) {
    switch (func) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return value == ref;
    case 3u: return value != ref;
    case 4u: return value < ref;
    case 5u: return value <= ref;
    case 6u: return value > ref;
    default: return value >= ref;
    }
}

void main() {
    vec4 color = (pc.flags & F_FLAT) != 0u ? v_color_flat : v_color;

    if ((pc.flags & F_CLEAR) != 0u) {
        o_color = color;
        o_blend = color;
        return;
    }

    if ((pc.flags & F_TEXTURE) != 0u) {
        vec2 uv = v_texcoord.xy;
        if ((pc.flags & F_PROJECT) != 0u) uv /= v_texcoord.z;
        vec4 t = texture(u_texture, uv * pc.uv_scale);
        bool use_alpha = (pc.flags & F_TEX_ALPHA) != 0u;
        switch ((pc.flags >> 1) & 7u) {
        case 0u: // modulate
            color = vec4(color.rgb * t.rgb, use_alpha ? color.a * t.a : color.a);
            break;
        case 1u: // decal
            color = vec4(use_alpha ? mix(color.rgb, t.rgb, t.a) : t.rgb, color.a);
            break;
        case 2u: // blend with the environment colour
            color = vec4(mix(color.rgb, pc.tex_env_color.rgb, t.rgb), use_alpha ? color.a * t.a : color.a);
            break;
        case 3u: // replace
            color = vec4(t.rgb, use_alpha ? t.a : color.a);
            break;
        default: // add
            color = vec4(color.rgb + t.rgb, use_alpha ? color.a * t.a : color.a);
            break;
        }
        if ((pc.flags & F_COLOR_DOUBLE) != 0u) color.rgb *= 2.0;
    }

    color.rgb = clamp(color.rgb + v_specular, 0.0, 1.0);
    color.a = clamp(color.a, 0.0, 1.0);

    if ((pc.flags & F_ALPHA_TEST) != 0u) {
        int mask = int((pc.alpha_test >> 16) & 0xFFu);
        int value = int(color.a * 255.0 + 0.5) & mask;
        int ref = int((pc.alpha_test >> 8) & 0xFFu) & mask;
        if (!compare(pc.alpha_test & 7u, value, ref)) discard;
    }

    if ((pc.flags & F_COLOR_TEST) != 0u) {
        uint func = (pc.flags >> 9) & 3u;
        ivec3 c = ivec3(color.rgb * 255.0 + 0.5);
        uint packed = (uint(c.r) | (uint(c.g) << 8) | (uint(c.b) << 16)) & pc.color_mask;
        bool equal = packed == (pc.color_ref & pc.color_mask);
        if (func == 0u || (func == 2u && !equal) || (func == 3u && equal)) discard;
    }

    if ((pc.flags & F_FOG) != 0u) color.rgb = mix(pc.fog_color.rgb, color.rgb, clamp(v_fog, 0.0, 1.0));

    float blend_alpha = (pc.flags & F_DOUBLE_ALPHA) != 0u ? min(color.a * 2.0, 1.0) : color.a;
    o_color = vec4(color.rgb, (pc.flags & F_WRITE_STENCIL) != 0u ? pc.stencil_value : color.a);
    o_blend = vec4(blend_alpha);
}
