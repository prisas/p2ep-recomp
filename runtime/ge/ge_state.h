#pragma once

#include "lib/common/types.h"

#include <bit>

// Raw state of the graphics engine (GE). Every display-list word is an 8-bit command and a 24-bit
// argument; most commands simply latch their argument, so the state is the array of latest
// arguments plus the matrices, with accessors that pick the fields apart.
namespace p2::ge {

enum Cmd : u8 {
    kNop = 0x00, kVaddr = 0x01, kIaddr = 0x02, kPrim = 0x04, kBezier = 0x05, kSpline = 0x06, kBbox = 0x07,
    kJump = 0x08, kBjump = 0x09, kCall = 0x0A, kRet = 0x0B, kEnd = 0x0C, kSignal = 0x0E, kFinish = 0x0F,
    kBase = 0x10, kVtype = 0x12, kOffsetAddr = 0x13, kOriginAddr = 0x14, kRegion1 = 0x15, kRegion2 = 0x16,
    kLightingEnable = 0x17, kLightEnable0 = 0x18, kClipEnable = 0x1C, kCullEnable = 0x1D, kTextureEnable = 0x1E,
    kFogEnable = 0x1F, kDitherEnable = 0x20, kBlendEnable = 0x21, kAlphaTestEnable = 0x22, kZTestEnable = 0x23,
    kStencilTestEnable = 0x24, kAntialiasEnable = 0x25, kPatchCullEnable = 0x26, kColorTestEnable = 0x27,
    kLogicOpEnable = 0x28,
    kBoneMatrixNumber = 0x2A, kBoneMatrixData = 0x2B, kMorphWeight0 = 0x2C,
    kWorldMatrixNumber = 0x3A, kWorldMatrixData = 0x3B, kViewMatrixNumber = 0x3C, kViewMatrixData = 0x3D,
    kProjMatrixNumber = 0x3E, kProjMatrixData = 0x3F, kTgenMatrixNumber = 0x40, kTgenMatrixData = 0x41,
    kViewportXScale = 0x42, kViewportYScale = 0x43, kViewportZScale = 0x44,
    kViewportXCenter = 0x45, kViewportYCenter = 0x46, kViewportZCenter = 0x47,
    kTexScaleU = 0x48, kTexScaleV = 0x49, kTexOffsetU = 0x4A, kTexOffsetV = 0x4B, kOffsetX = 0x4C, kOffsetY = 0x4D,
    kShadeMode = 0x50, kReverseNormal = 0x51, kMaterialUpdate = 0x53, kMaterialEmissive = 0x54,
    kMaterialAmbient = 0x55, kMaterialDiffuse = 0x56, kMaterialSpecular = 0x57, kMaterialAlpha = 0x58,
    kMaterialSpecularCoef = 0x5B, kAmbientColor = 0x5C, kAmbientAlpha = 0x5D, kLightMode = 0x5E,
    kLightType0 = 0x5F, kLightPos0 = 0x63, kLightDir0 = 0x6F, kLightAtt0 = 0x7B, kLightSpotExp0 = 0x87,
    kLightSpotCutoff0 = 0x8B, kLightAmbient0 = 0x8F,
    kCull = 0x9B, kFramebufPtr = 0x9C, kFramebufWidth = 0x9D, kZbufPtr = 0x9E, kZbufWidth = 0x9F,
    kTexAddr0 = 0xA0, kTexBufWidth0 = 0xA8, kClutAddr = 0xB0, kClutAddrUpper = 0xB1,
    kTransferSrc = 0xB2, kTransferSrcW = 0xB3, kTransferDst = 0xB4, kTransferDstW = 0xB5,
    kTexSize0 = 0xB8, kTexMapMode = 0xC0, kTexShadeLs = 0xC1, kTexMode = 0xC2, kTexFormat = 0xC3,
    kLoadClut = 0xC4, kClutFormat = 0xC5, kTexFilter = 0xC6, kTexWrap = 0xC7, kTexLevel = 0xC8, kTexFunc = 0xC9,
    kTexEnvColor = 0xCA, kTexFlush = 0xCB, kTexSync = 0xCC, kFog1 = 0xCD, kFog2 = 0xCE, kFogColor = 0xCF,
    kTexLodSlope = 0xD0, kFramebufPixFormat = 0xD2, kClearMode = 0xD3, kScissor1 = 0xD4, kScissor2 = 0xD5,
    kMinZ = 0xD6, kMaxZ = 0xD7, kColorTest = 0xD8, kColorRef = 0xD9, kColorTestMask = 0xDA, kAlphaTest = 0xDB,
    kStencilTest = 0xDC, kStencilOp = 0xDD, kZTest = 0xDE, kBlendMode = 0xDF, kBlendFixedA = 0xE0,
    kBlendFixedB = 0xE1, kDither0 = 0xE2, kLogicOp = 0xE6, kZWriteDisable = 0xE7, kMaskRgb = 0xE8,
    kMaskAlpha = 0xE9, kTransferStart = 0xEA, kTransferSrcPos = 0xEB, kTransferDstPos = 0xEC,
    kTransferSize = 0xEE,
};

enum PrimType : u32 { kPoints, kLines, kLineStrip, kTriangles, kTriangleStrip, kTriangleFan, kSprites };

enum TexFormat : u32 {
    kTex5650, kTex5551, kTex4444, kTex8888, kTexClut4, kTexClut8, kTexClut16, kTexClut32, kTexDxt1, kTexDxt3, kTexDxt5
};

// A 24-bit argument holding the top 24 bits of a float.
P2_INLINE f32 float24(u32 arg) { return std::bit_cast<f32>(arg << 8); }

struct State {
    u32 arg[256] = {};      // latest argument of every command
    f32 world[12] = {};     // 4x3, row vectors: out = in * M
    f32 view[12] = {};
    f32 proj[16] = {};      // 4x4
    f32 tgen[12] = {};
    f32 bone[8 * 12] = {};
    u8 clut[1024] = {};     // palette as loaded by the last LOADCLUT
    u32 clut_serial = 0;    // bumped on every LOADCLUT

    // --- vertices ---
    u32 vtype() const { return arg[kVtype]; }
    bool through() const { return (vtype() >> 23) & 1; }

    // --- enables ---
    bool lighting() const { return arg[kLightingEnable] & 1; }
    bool light_on(u32 i) const { return arg[kLightEnable0 + i] & 1; }
    bool cull_enabled() const { return arg[kCullEnable] & 1; }
    bool texture_enabled() const { return arg[kTextureEnable] & 1; }
    bool fog_enabled() const { return arg[kFogEnable] & 1; }
    bool blend_enabled() const { return arg[kBlendEnable] & 1; }
    bool alpha_test_enabled() const { return arg[kAlphaTestEnable] & 1; }
    bool depth_test_enabled() const { return arg[kZTestEnable] & 1; }
    bool stencil_test_enabled() const { return arg[kStencilTestEnable] & 1; }
    bool color_test_enabled() const { return arg[kColorTestEnable] & 1; }
    bool clear_mode() const { return arg[kClearMode] & 1; }

    // --- render target ---
    u32 fb_addr() const { return 0x04000000u | ((arg[kFramebufPtr] | ((arg[kFramebufWidth] & 0xFF0000) << 8)) & 0x001FFFF0u); }
    u32 fb_stride() const { return arg[kFramebufWidth] & 0x7FC; }
    u32 fb_format() const { return arg[kFramebufPixFormat] & 3; }
    u32 zb_addr() const { return 0x04000000u | ((arg[kZbufPtr] | ((arg[kZbufWidth] & 0xFF0000) << 8)) & 0x001FFFF0u); }
    u32 zb_stride() const { return arg[kZbufWidth] & 0x7FC; }

    // --- viewport and scissor ---
    f32 vp_xscale() const { return float24(arg[kViewportXScale]); }
    f32 vp_yscale() const { return float24(arg[kViewportYScale]); }
    f32 vp_zscale() const { return float24(arg[kViewportZScale]); }
    f32 vp_xcenter() const { return float24(arg[kViewportXCenter]); }
    f32 vp_ycenter() const { return float24(arg[kViewportYCenter]); }
    f32 vp_zcenter() const { return float24(arg[kViewportZCenter]); }
    f32 offset_x() const { return f32(arg[kOffsetX] & 0xFFFF) / 16.0f; }
    f32 offset_y() const { return f32(arg[kOffsetY] & 0xFFFF) / 16.0f; }
    u32 scissor_x1() const { return arg[kScissor1] & 0x3FF; }
    u32 scissor_y1() const { return (arg[kScissor1] >> 10) & 0x3FF; }
    u32 scissor_x2() const { return arg[kScissor2] & 0x3FF; }
    u32 scissor_y2() const { return (arg[kScissor2] >> 10) & 0x3FF; }

    // --- textures ---
    u32 tex_addr(u32 level) const {
        return (arg[kTexAddr0 + level] & 0xFFFFF0) | ((arg[kTexBufWidth0 + level] << 8) & 0x0F000000);
    }
    u32 tex_buf_width(u32 level) const { return arg[kTexBufWidth0 + level] & 0x7FF; }
    u32 tex_width(u32 level) const { return 1u << (arg[kTexSize0 + level] & 0xF); }
    u32 tex_height(u32 level) const { return 1u << ((arg[kTexSize0 + level] >> 8) & 0xF); }
    u32 tex_format() const { return arg[kTexFormat] & 0xF; }
    bool tex_swizzled() const { return arg[kTexMode] & 1; }
    u32 tex_func() const { return arg[kTexFunc] & 7; }
    bool tex_uses_alpha() const { return (arg[kTexFunc] >> 8) & 1; }
    bool tex_color_double() const { return (arg[kTexFunc] >> 16) & 1; }
    bool tex_min_linear() const { return arg[kTexFilter] & 1; }
    bool tex_mag_linear() const { return (arg[kTexFilter] >> 8) & 1; }
    bool tex_clamp_s() const { return arg[kTexWrap] & 1; }
    bool tex_clamp_t() const { return (arg[kTexWrap] >> 8) & 1; }
    u32 tex_map_mode() const { return arg[kTexMapMode] & 3; }
    u32 tex_proj_source() const { return (arg[kTexMapMode] >> 8) & 3; }
    f32 tex_scale_u() const { return float24(arg[kTexScaleU]); }
    f32 tex_scale_v() const { return float24(arg[kTexScaleV]); }
    f32 tex_offset_u() const { return float24(arg[kTexOffsetU]); }
    f32 tex_offset_v() const { return float24(arg[kTexOffsetV]); }

    u32 clut_addr() const { return (arg[kClutAddr] & 0xFFFFF0) | ((arg[kClutAddrUpper] << 8) & 0x0F000000); }
    u32 clut_format() const { return arg[kClutFormat] & 3; }
    u32 clut_shift() const { return (arg[kClutFormat] >> 2) & 0x1F; }
    u32 clut_mask() const { return (arg[kClutFormat] >> 8) & 0xFF; }
    u32 clut_base() const { return ((arg[kClutFormat] >> 16) & 0x1F) << 4; }

    // --- pixel tests and blending ---
    u32 alpha_func() const { return arg[kAlphaTest] & 7; }
    u32 alpha_ref() const { return (arg[kAlphaTest] >> 8) & 0xFF; }
    u32 alpha_mask() const { return (arg[kAlphaTest] >> 16) & 0xFF; }
    u32 color_func() const { return arg[kColorTest] & 3; }
    u32 depth_func() const { return arg[kZTest] & 7; }
    bool depth_write() const { return !(arg[kZWriteDisable] & 1); }
    u32 blend_src() const { return arg[kBlendMode] & 0xF; }
    u32 blend_dst() const { return (arg[kBlendMode] >> 4) & 0xF; }
    u32 blend_eq() const { return (arg[kBlendMode] >> 8) & 7; }
    u32 stencil_func() const { return arg[kStencilTest] & 7; }
    u32 stencil_ref() const { return (arg[kStencilTest] >> 8) & 0xFF; }
    u32 stencil_mask() const { return (arg[kStencilTest] >> 16) & 0xFF; }
    u32 stencil_sfail() const { return arg[kStencilOp] & 7; }
    u32 stencil_zfail() const { return (arg[kStencilOp] >> 8) & 7; }
    u32 stencil_zpass() const { return (arg[kStencilOp] >> 16) & 7; }
    bool flat_shading() const { return !(arg[kShadeMode] & 1); }
    bool cull_ccw() const { return arg[kCull] & 1; }

    void reset();
};

} // namespace p2::ge
