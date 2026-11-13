#version 450

// Vertices arrive already transformed: xy/w in framebuffer pixels, z/w in [0, 1].
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec3 in_texcoord;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_specular;
layout(location = 4) in float in_fog;

layout(push_constant) uniform Params {
    vec2 pixel_to_ndc; // 2 / framebuffer size
    vec2 uv_scale;
    vec4 tex_env_color;
    vec4 fog_color;
    uint flags;
    uint alpha_test;
    uint color_ref;
    uint color_mask;
    float stencil_value;
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) flat out vec4 v_color_flat;
layout(location = 2) out vec3 v_specular;
layout(location = 3) out vec3 v_texcoord;
layout(location = 4) out float v_fog;

void main() {
    gl_Position = vec4(in_position.x * pc.pixel_to_ndc.x - in_position.w,
                       in_position.y * pc.pixel_to_ndc.y - in_position.w,
                       in_position.z,
                       in_position.w);
    v_color = in_color;
    v_color_flat = in_color;
    v_specular = in_specular.rgb;
    v_texcoord = in_texcoord;
    v_fog = in_fog;
}
