#pragma once

// Vector unit (VFPU) semantics. Each instruction is a function taking the raw instruction word;
// with a constant word the register selection folds away at compile time. Only the instructions
// the game executable contains are implemented; the recompiler refuses anything else.

#include "runtime/cpu/ops.h"

#include <algorithm>
#include <cmath>

namespace p2::vfpu {

inline constexpr f32 kPiOver2 = 1.57079632679489661923f;

// --- register addressing ------------------------------------------------------------------------------------
// A register operand is 7 bits: matrix (bits 2-4), index (bits 0-1) and a row/transpose field
// (bits 5-6) whose meaning depends on the operand size. size: 0 single, 1 pair, 2 triple, 3 quad.

P2_INLINE void vec_regs(u32 reg, u32 size, u8 out[4]) {
    u32 mtx = (reg >> 2) & 7, col = reg & 3, row = 0;
    bool transpose = ((reg >> 5) & 1) != 0;
    switch (size) {
    case 0:
        transpose = false;
        row = (reg >> 5) & 3;
        break;
    case 1: row = (reg >> 5) & 2; break;
    case 2: row = (reg >> 6) & 1; break;
    default: row = (reg >> 5) & 2; break;
    }
    for (u32 i = 0; i <= size; ++i) {
        u32 index = mtx * 4;
        if (transpose) index += ((row + i) & 3) + col * 32;
        else index += col + ((row + i) & 3) * 32;
        out[i] = u8(index);
    }
}

// out[column * 4 + row]
P2_INLINE void mat_regs(u32 reg, u32 size, u8 out[16]) {
    u32 mtx = (reg >> 2) & 7, col = reg & 3, row = 0;
    bool transpose = ((reg >> 5) & 1) != 0;
    switch (size) {
    case 0:
        transpose = false;
        row = (reg >> 5) & 3;
        break;
    case 1: row = (reg >> 5) & 2; break;
    case 2: row = (reg >> 6) & 1; break;
    default: row = (reg >> 5) & 2; break;
    }
    for (u32 i = 0; i <= size; ++i) {
        for (u32 j = 0; j <= size; ++j) {
            u32 index = mtx * 4;
            if (transpose) index += ((row + i) & 3) + ((col + j) & 3) * 32;
            else index += ((col + j) & 3) + ((row + i) & 3) * 32;
            out[j * 4 + i] = u8(index);
        }
    }
}

union Vec {
    f32 f[4];
    u32 u[4];
    s32 i[4];
};

P2_INLINE void read_vec(const Cpu* c, Vec& d, u32 size, u32 reg) {
    u8 r[4];
    vec_regs(reg, size, r);
    for (u32 i = 0; i < 4; ++i) d.u[i] = i <= size ? c->vi[r[i]] : 0;
}

// Honours the write mask of the destination prefix.
P2_INLINE void write_vec(Cpu* c, const Vec& s, u32 size, u32 reg) {
    u8 r[4];
    vec_regs(reg, size, r);
    u32 mask = (c->vctrl[kVfpuDpfx] >> 8) & 15;
    for (u32 i = 0; i <= size; ++i)
        if (!((mask >> i) & 1)) c->vi[r[i]] = s.u[i];
}

P2_INLINE void read_mat(const Cpu* c, f32 d[16], u32 size, u32 reg) {
    u8 r[16];
    mat_regs(reg, size, r);
    for (u32 j = 0; j < 4; ++j)
        for (u32 i = 0; i < 4; ++i) d[j * 4 + i] = (i <= size && j <= size) ? c->v[r[j * 4 + i]] : 0.0f;
}

P2_INLINE void write_mat(Cpu* c, const f32 s[16], u32 size, u32 reg) {
    u8 r[16];
    mat_regs(reg, size, r);
    for (u32 j = 0; j <= size; ++j)
        for (u32 i = 0; i <= size; ++i) c->v[r[j * 4 + i]] = s[j * 4 + i];
}

// --- prefixes -----------------------------------------------------------------------------------------------

// Source/target prefix: per-element swizzle, absolute value, constant substitution and negation.
inline void apply_prefix_st(Vec& v, u32 prefix, u32 size) {
    static constexpr f32 constants[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};
    Vec orig = v;
    for (u32 i = 0; i <= size; ++i) {
        u32 regnum = (prefix >> (i * 2)) & 3;
        u32 abs = (prefix >> (8 + i)) & 1;
        u32 constant = (prefix >> (12 + i)) & 1;
        u32 negate = (prefix >> (16 + i)) & 1;
        if (!constant) {
            v.u[i] = orig.u[regnum];
            if (abs) v.u[i] &= 0x7FFFFFFFu;
        } else {
            v.f[i] = constants[regnum + (abs << 2)];
        }
        if (negate) v.u[i] ^= 0x80000000u;
    }
}

P2_INLINE void swizzle_s(const Cpu* c, Vec& v, u32 size) {
    if (P2_UNLIKELY(c->vctrl[kVfpuSpfx] != kVfpuPrefixIdentity)) apply_prefix_st(v, c->vctrl[kVfpuSpfx], size);
}

P2_INLINE void swizzle_t(const Cpu* c, Vec& v, u32 size) {
    if (P2_UNLIKELY(c->vctrl[kVfpuTpfx] != kVfpuPrefixIdentity)) apply_prefix_st(v, c->vctrl[kVfpuTpfx], size);
}

P2_INLINE f32 clamp_nan_safe(f32 v, f32 lo, f32 hi) {
    // NaN keeps its sign's bound, matching the saturation hardware.
    if (v != v) return std::signbit(v) ? lo : hi;
    return v < lo ? lo : (v > hi ? hi : v);
}

// Destination prefix: per-element saturation (the write mask is applied by write_vec).
inline void apply_prefix_d(const Cpu* c, Vec& v, u32 size) {
    u32 prefix = c->vctrl[kVfpuDpfx];
    for (u32 i = 0; i <= size; ++i) {
        u32 sat = (prefix >> (i * 2)) & 3;
        if (sat == 1) v.f[i] = clamp_nan_safe(v.f[i], 0.0f, 1.0f);
        else if (sat == 3) v.f[i] = clamp_nan_safe(v.f[i], -1.0f, 1.0f);
    }
}

P2_INLINE void saturate_d(const Cpu* c, Vec& v, u32 size) {
    if (P2_UNLIKELY(c->vctrl[kVfpuDpfx] != 0)) apply_prefix_d(c, v, size);
}

// Prefixes last for exactly one instruction.
P2_INLINE void eat_prefixes(Cpu* c) {
    c->vctrl[kVfpuSpfx] = kVfpuPrefixIdentity;
    c->vctrl[kVfpuTpfx] = kVfpuPrefixIdentity;
    c->vctrl[kVfpuDpfx] = 0;
}

// --- operand fields -----------------------------------------------------------------------------------------

P2_INLINE u32 f_vd(u32 w) { return w & 127; }
P2_INLINE u32 f_vs(u32 w) { return (w >> 8) & 127; }
P2_INLINE u32 f_vt(u32 w) { return (w >> 16) & 127; }
P2_INLINE u32 f_size(u32 w) { return ((w >> 7) & 1) | (((w >> 15) & 1) << 1); }
// Single loads/stores take two low bits of the word as the top of the register number. Quad
// loads/stores take only bit 0; bit 1 is the cache write-back flag of sv.q and not part of it.
P2_INLINE u32 f_mem_reg(u32 w) { return ((w >> 16) & 31) | ((w & 3) << 5); }
P2_INLINE u32 f_mem_reg_quad(u32 w) { return ((w >> 16) & 31) | ((w & 1) << 5); }

// --- math helpers -------------------------------------------------------------------------------------------

// Angles are measured in quarter turns: sin(1.0) is exactly 1.
inline f32 quarter_sin(f32 a) {
    f32 whole = std::floor(a * 0.25f) * 4.0f;
    f32 r = a - whole; // [0, 4)
    if (r == 0.0f || r == 2.0f) return 0.0f;
    if (r == 1.0f) return 1.0f;
    if (r == 3.0f) return -1.0f;
    return std::sin(r * kPiOver2);
}

inline f32 quarter_cos(f32 a) {
    f32 whole = std::floor(a * 0.25f) * 4.0f;
    f32 r = a - whole;
    if (r == 1.0f || r == 3.0f) return 0.0f;
    if (r == 0.0f) return 1.0f;
    if (r == 2.0f) return -1.0f;
    return std::cos(r * kPiOver2);
}

inline f32 quarter_asin(f32 a) { return std::asin(a) / kPiOver2; }

inline f32 half_to_float(u16 h) {
    u32 sign = u32(h >> 15) << 31;
    u32 exponent = (h >> 10) & 31;
    u32 mantissa = h & 0x3FF;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else { // subnormal: renormalise
            int shift = std::countl_zero(mantissa) - 21;
            mantissa = (mantissa << shift) & 0x3FF;
            bits = sign | (u32(113 - shift) << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    return std::bit_cast<f32>(bits);
}

// Sign-magnitude comparison of the raw bits: orders NaNs consistently, like the hardware's vmin/vmax.
P2_INLINE s32 float_order(u32 bits) { return (bits & 0x80000000u) ? -s32(bits & 0x7FFFFFFFu) : s32(bits); }

// --- memory -------------------------------------------------------------------------------------------------

P2_INLINE void lv_s(Cpu* c, const u8* m, u32 w, u32 addr) { c->vi[f_mem_reg(w)] = rd32(m, addr); }
P2_INLINE void sv_s(const Cpu* c, u8* m, u32 w, u32 addr) { wr32(m, addr, c->vi[f_mem_reg(w)]); }

P2_INLINE void lv_q(Cpu* c, const u8* m, u32 w, u32 addr) {
    Vec v;
    for (u32 i = 0; i < 4; ++i) v.u[i] = rd32(m, addr + i * 4);
    write_vec(c, v, 3, f_mem_reg_quad(w));
}

P2_INLINE void sv_q(const Cpu* c, u8* m, u32 w, u32 addr) {
    Vec v;
    read_vec(c, v, 3, f_mem_reg_quad(w));
    for (u32 i = 0; i < 4; ++i) wr32(m, addr + i * 4, v.u[i]);
}

// --- moves between the CPU and the VFPU ---------------------------------------------------------------------

P2_INLINE u32 mfv(const Cpu* c, u32 w) { return c->vi[w & 127]; }
P2_INLINE void mtv(Cpu* c, u32 w, u32 value) { c->vi[w & 127] = value; }
P2_INLINE u32 mfvc(const Cpu* c, u32 w) {
    u32 index = w & 127;
    return index < kVfpuCtrlCount ? c->vctrl[index] : 0;
}
P2_INLINE void mtvc(Cpu* c, u32 w, u32 value) {
    u32 index = w & 127;
    if (index < kVfpuCtrlCount) c->vctrl[index] = value;
}

// vpfxs / vpfxt / vpfxd
P2_INLINE void vpfx(Cpu* c, u32 w) {
    u32 which = (w >> 24) & 3;
    u32 data = w & 0xFFFFF;
    if (which == kVfpuDpfx) data &= 0xFFF;
    c->vctrl[which] = data;
}

// --- element-wise arithmetic --------------------------------------------------------------------------------

enum class Bin { Add, Sub, Mul, Div, Min, Max };

template <Bin kind>
P2_INLINE void binary(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, t, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    read_vec(c, t, size, f_vt(w));
    swizzle_t(c, t, size);
    for (u32 i = 0; i <= size; ++i) {
        switch (kind) {
        case Bin::Add: d.f[i] = s.f[i] + t.f[i]; break;
        case Bin::Sub: d.f[i] = s.f[i] - t.f[i]; break;
        case Bin::Mul: d.f[i] = s.f[i] * t.f[i]; break;
        case Bin::Div: d.f[i] = s.f[i] / t.f[i]; break;
        case Bin::Min: d.u[i] = float_order(s.u[i]) < float_order(t.u[i]) ? s.u[i] : t.u[i]; break;
        case Bin::Max: d.u[i] = float_order(s.u[i]) > float_order(t.u[i]) ? s.u[i] : t.u[i]; break;
        }
    }
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vadd(Cpu* c, u32 w) { binary<Bin::Add>(c, w); }
P2_INLINE void vsub(Cpu* c, u32 w) { binary<Bin::Sub>(c, w); }
P2_INLINE void vmul(Cpu* c, u32 w) { binary<Bin::Mul>(c, w); }
P2_INLINE void vdiv(Cpu* c, u32 w) { binary<Bin::Div>(c, w); }
P2_INLINE void vmin(Cpu* c, u32 w) { binary<Bin::Min>(c, w); }
P2_INLINE void vmax(Cpu* c, u32 w) { binary<Bin::Max>(c, w); }

P2_INLINE void vdot(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, t, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, 3);
    read_vec(c, t, size, f_vt(w));
    swizzle_t(c, t, 3);
    f32 sum = 0.0f;
    for (u32 i = 0; i <= size; ++i) sum += s.f[i] * t.f[i];
    d.f[0] = sum;
    saturate_d(c, d, 0);
    write_vec(c, d, 0, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vscl(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    f32 scale = c->v[f_vt(w)];
    for (u32 i = 0; i <= size; ++i) d.f[i] = s.f[i] * scale;
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

// vcrsp.t: cross product
P2_INLINE void vcrsp(Cpu* c, u32 w) {
    Vec s, t, d{};
    read_vec(c, s, 2, f_vs(w));
    read_vec(c, t, 2, f_vt(w));
    d.f[0] = s.f[1] * t.f[2] - s.f[2] * t.f[1];
    d.f[1] = s.f[2] * t.f[0] - s.f[0] * t.f[2];
    d.f[2] = s.f[0] * t.f[1] - s.f[1] * t.f[0];
    write_vec(c, d, 2, f_vd(w));
    eat_prefixes(c);
}

// vqmul.q: quaternion product
P2_INLINE void vqmul(Cpu* c, u32 w) {
    Vec s, t, d{};
    read_vec(c, s, 3, f_vs(w));
    read_vec(c, t, 3, f_vt(w));
    d.f[0] = s.f[0] * t.f[3] + s.f[1] * t.f[2] - s.f[2] * t.f[1] + s.f[3] * t.f[0];
    d.f[1] = -s.f[0] * t.f[2] + s.f[1] * t.f[3] + s.f[2] * t.f[0] + s.f[3] * t.f[1];
    d.f[2] = s.f[0] * t.f[1] - s.f[1] * t.f[0] + s.f[2] * t.f[3] + s.f[3] * t.f[2];
    d.f[3] = -s.f[0] * t.f[0] - s.f[1] * t.f[1] - s.f[2] * t.f[2] + s.f[3] * t.f[3];
    write_vec(c, d, 3, f_vd(w));
    eat_prefixes(c);
}

// --- element-wise unary -------------------------------------------------------------------------------------

enum class Un {
    Mov, Abs, Neg, Sat0, Sat1, Rcp, Rsq, Sin, Cos, Exp2, Log2, Sqrt, Asin, Nrcp, Nsin, Rexp2, Sgn, Ocp
};

template <Un kind>
P2_INLINE void unary(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    for (u32 i = 0; i <= size; ++i) {
        f32 x = s.f[i];
        switch (kind) {
        case Un::Mov: d.u[i] = s.u[i]; break;
        case Un::Abs: d.u[i] = s.u[i] & 0x7FFFFFFFu; break;
        case Un::Neg: d.u[i] = s.u[i] ^ 0x80000000u; break;
        case Un::Sat0: d.f[i] = clamp_nan_safe(x, 0.0f, 1.0f); break;
        case Un::Sat1: d.f[i] = clamp_nan_safe(x, -1.0f, 1.0f); break;
        case Un::Rcp: d.f[i] = 1.0f / x; break;
        case Un::Rsq: d.f[i] = 1.0f / std::sqrt(x); break;
        case Un::Sin: d.f[i] = quarter_sin(x); break;
        case Un::Cos: d.f[i] = quarter_cos(x); break;
        case Un::Exp2: d.f[i] = std::exp2(x); break;
        case Un::Log2: d.f[i] = std::log2(x); break;
        case Un::Sqrt: d.f[i] = std::fabs(std::sqrt(x)); break;
        case Un::Asin: d.f[i] = quarter_asin(x); break;
        case Un::Nrcp: d.f[i] = -1.0f / x; break;
        case Un::Nsin: d.f[i] = -quarter_sin(x); break;
        case Un::Rexp2: d.f[i] = 1.0f / std::exp2(x); break;
        case Un::Sgn:
            d.f[i] = (s.u[i] & 0x7FFFFFFFu) == 0 ? 0.0f : ((s.u[i] >> 31) ? -1.0f : 1.0f);
            break;
        case Un::Ocp: d.f[i] = 1.0f - x; break;
        }
    }
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vmov(Cpu* c, u32 w) { unary<Un::Mov>(c, w); }
P2_INLINE void vabs(Cpu* c, u32 w) { unary<Un::Abs>(c, w); }
P2_INLINE void vneg(Cpu* c, u32 w) { unary<Un::Neg>(c, w); }
P2_INLINE void vsat0(Cpu* c, u32 w) { unary<Un::Sat0>(c, w); }
P2_INLINE void vsat1(Cpu* c, u32 w) { unary<Un::Sat1>(c, w); }
P2_INLINE void vrcp(Cpu* c, u32 w) { unary<Un::Rcp>(c, w); }
P2_INLINE void vrsq(Cpu* c, u32 w) { unary<Un::Rsq>(c, w); }
P2_INLINE void vsin(Cpu* c, u32 w) { unary<Un::Sin>(c, w); }
P2_INLINE void vcos(Cpu* c, u32 w) { unary<Un::Cos>(c, w); }
P2_INLINE void vexp2(Cpu* c, u32 w) { unary<Un::Exp2>(c, w); }
P2_INLINE void vlog2(Cpu* c, u32 w) { unary<Un::Log2>(c, w); }
P2_INLINE void vsqrt(Cpu* c, u32 w) { unary<Un::Sqrt>(c, w); }
P2_INLINE void vasin(Cpu* c, u32 w) { unary<Un::Asin>(c, w); }
P2_INLINE void vnrcp(Cpu* c, u32 w) { unary<Un::Nrcp>(c, w); }
P2_INLINE void vnsin(Cpu* c, u32 w) { unary<Un::Nsin>(c, w); }
P2_INLINE void vrexp2(Cpu* c, u32 w) { unary<Un::Rexp2>(c, w); }
P2_INLINE void vsgn(Cpu* c, u32 w) { unary<Un::Sgn>(c, w); }
P2_INLINE void vocp(Cpu* c, u32 w) { unary<Un::Ocp>(c, w); }

// vsocp.p: pairs of (1 - x, x), each clamped to [0, 1]; the result is twice as wide as the source.
P2_INLINE void vsocp(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    u32 out_size = size == 0 ? 1 : 3;
    u32 pairs = size == 0 ? 1 : 2;
    for (u32 i = 0; i < pairs; ++i) {
        d.f[i * 2] = clamp_nan_safe(1.0f - s.f[i], 0.0f, 1.0f);
        d.f[i * 2 + 1] = clamp_nan_safe(s.f[i], 0.0f, 1.0f);
    }
    write_vec(c, d, out_size, f_vd(w));
    eat_prefixes(c);
}

// vfad / vavg: horizontal sum and mean
P2_INLINE void vfad(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    f32 sum = 0.0f;
    for (u32 i = 0; i <= size; ++i) sum += s.f[i];
    d.f[0] = sum;
    saturate_d(c, d, 0);
    write_vec(c, d, 0, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vavg(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    f32 sum = 0.0f;
    for (u32 i = 0; i <= size; ++i) sum += s.f[i];
    d.f[0] = sum / f32(size + 1);
    saturate_d(c, d, 0);
    write_vec(c, d, 0, f_vd(w));
    eat_prefixes(c);
}

// --- constants and initialisers -----------------------------------------------------------------------------

P2_INLINE void vfill(Cpu* c, u32 w, f32 value) {
    u32 size = f_size(w);
    Vec d;
    for (u32 i = 0; i < 4; ++i) d.f[i] = value;
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vzero(Cpu* c, u32 w) { vfill(c, w, 0.0f); }
P2_INLINE void vone(Cpu* c, u32 w) { vfill(c, w, 1.0f); }

P2_INLINE void vcst(Cpu* c, u32 w) {
    static constexpr f32 constants[32] = {
        0.0f,
        3.402823466e+38f,       // largest finite value
        1.41421356237309504880f, // sqrt(2)
        0.70710678118654752440f, // sqrt(1/2)
        1.12837916709551257390f, // 2/sqrt(pi)
        0.63661977236758134308f, // 2/pi
        0.31830988618379067154f, // 1/pi
        0.78539816339744830962f, // pi/4
        1.57079632679489661923f, // pi/2
        3.14159265358979323846f, // pi
        2.71828182845904523536f, // e
        1.44269504088896340736f, // log2(e)
        0.43429448190325182765f, // log10(e)
        0.69314718055994530942f, // ln(2)
        2.30258509299404568402f, // ln(10)
        6.28318530717958647692f, // 2*pi
        0.52359877559829887308f, // pi/6
        0.30102999566398119521f, // log10(2)
        3.32192809488736234787f, // log2(10)
        0.86602540378443864676f, // sqrt(3)/2
    };
    vfill(c, w, constants[(w >> 16) & 31]);
}

// vidt: one row of the identity matrix, selected by the destination register's index.
P2_INLINE void vidt(Cpu* c, u32 w) {
    u32 size = f_size(w), vd = f_vd(w);
    u32 lane = size == 1 ? (vd & 1) : (vd & 3);
    Vec d;
    for (u32 i = 0; i < 4; ++i) d.f[i] = i == lane ? 1.0f : 0.0f;
    saturate_d(c, d, size);
    write_vec(c, d, size, vd);
    eat_prefixes(c);
}

// viim / vfim: load a 16-bit integer or half-float immediate into a single register.
P2_INLINE void viim(Cpu* c, u32 w) {
    c->v[f_vt(w)] = f32(s32(s16(w & 0xFFFF)));
    eat_prefixes(c);
}

P2_INLINE void vfim(Cpu* c, u32 w) {
    c->v[f_vt(w)] = half_to_float(u16(w & 0xFFFF));
    eat_prefixes(c);
}

// --- conversions --------------------------------------------------------------------------------------------

P2_INLINE void vi2f(Cpu* c, u32 w) {
    u32 size = f_size(w);
    f32 scale = 1.0f / f32(u32(1) << ((w >> 16) & 31));
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    for (u32 i = 0; i <= size; ++i) d.f[i] = f32(s.i[i]) * scale;
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

// vf2in / vf2iz / vf2iu / vf2id: scale by 2^imm, then round to nearest / toward zero / up / down.
P2_INLINE void vf2i(Cpu* c, u32 w) {
    u32 size = f_size(w);
    f64 scale = f64(u32(1) << ((w >> 16) & 31));
    u32 mode = (w >> 21) & 31;
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    for (u32 i = 0; i <= size; ++i) {
        if (s.f[i] != s.f[i]) {
            d.u[i] = 0x7FFFFFFFu;
            continue;
        }
        f64 v = f64(s.f[i]) * scale;
        if (v > 2147483647.0) {
            d.u[i] = 0x7FFFFFFFu;
        } else if (v <= -2147483648.0) {
            d.u[i] = 0x80000000u;
        } else {
            f64 r;
            switch (mode) {
            case 16: r = std::nearbyint(v); break;
            case 17: r = std::trunc(v); break;
            case 18: r = std::ceil(v); break;
            default: r = std::floor(v); break;
            }
            d.i[i] = s32(r);
        }
    }
    write_vec(c, d, size, f_vd(w)); // saturation does not apply to integer results
    eat_prefixes(c);
}

// vuc2i / vc2i / vus2i / vs2i: widen packed bytes or shorts into 31/32-bit integers.
P2_INLINE void vx2i(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    u32 out_size = 3;
    switch ((w >> 16) & 3) {
    case 0: { // vuc2i
        u32 value = s.u[0];
        for (u32 i = 0; i < 4; ++i) {
            d.u[i] = ((value & 0xFF) * 0x01010101u) >> 1;
            value >>= 8;
        }
        break;
    }
    case 1: { // vc2i
        u32 value = s.u[0];
        d.u[0] = (value & 0xFF) << 24;
        d.u[1] = (value & 0xFF00) << 16;
        d.u[2] = (value & 0xFF0000) << 8;
        d.u[3] = value & 0xFF000000u;
        break;
    }
    case 2: { // vus2i
        u32 count = size == 0 ? 1 : 2;
        out_size = size == 0 ? 1 : 3;
        for (u32 i = 0; i < count; ++i) {
            d.u[i * 2] = (s.u[i] & 0xFFFF) << 15;
            d.u[i * 2 + 1] = (s.u[i] & 0xFFFF0000u) >> 1;
        }
        break;
    }
    default: { // vs2i
        u32 count = size == 0 ? 1 : 2;
        out_size = size == 0 ? 1 : 3;
        for (u32 i = 0; i < count; ++i) {
            d.u[i * 2] = (s.u[i] & 0xFFFF) << 16;
            d.u[i * 2 + 1] = s.u[i] & 0xFFFF0000u;
        }
        break;
    }
    }
    saturate_d(c, d, out_size);
    write_vec(c, d, out_size, f_vd(w));
    eat_prefixes(c);
}

// vi2uc / vi2c / vi2us / vi2s: narrow integers back into packed bytes or shorts.
P2_INLINE void vi2x(Cpu* c, u32 w) {
    u32 size = f_size(w);
    Vec s, d{};
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    u32 out_size = 0;
    switch ((w >> 16) & 3) {
    case 0: // vi2uc
        for (u32 i = 0; i < 4; ++i) {
            s32 v = s.i[i] < 0 ? 0 : s.i[i];
            d.u[0] |= (u32(v >> 23) & 0xFF) << (i * 8);
        }
        break;
    case 1: // vi2c
        for (u32 i = 0; i < 4; ++i) d.u[0] |= (s.u[i] >> 24) << (i * 8);
        break;
    case 2: { // vi2us
        u32 count = (size + 2) / 2;
        for (u32 i = 0; i < count; ++i) {
            s32 low = s.i[i * 2] < 0 ? 0 : s.i[i * 2];
            s32 high = s.i[i * 2 + 1] < 0 ? 0 : s.i[i * 2 + 1];
            d.u[i] = u32(low >> 15) | (u32(high >> 15) << 16);
        }
        out_size = size == 3 ? 1 : 0;
        break;
    }
    default: { // vi2s
        u32 count = (size + 2) / 2;
        for (u32 i = 0; i < count; ++i) d.u[i] = (s.u[i * 2] >> 16) | ((s.u[i * 2 + 1] >> 16) << 16);
        out_size = size == 3 ? 1 : 0;
        break;
    }
    }
    write_vec(c, d, out_size, f_vd(w));
    eat_prefixes(c);
}

// --- comparison and selection -------------------------------------------------------------------------------

P2_INLINE void vcmp(Cpu* c, u32 w) {
    u32 size = f_size(w), cond = w & 15;
    Vec s, t;
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    read_vec(c, t, size, f_vt(w));
    swizzle_t(c, t, size);
    u32 cc = 0, any = 0, all = 1, affected = (1u << 4) | (1u << 5);
    for (u32 i = 0; i <= size; ++i) {
        f32 a = s.f[i], b = t.f[i];
        bool nan = a != a, inf = std::isinf(a);
        bool r;
        switch (cond) {
        case 0: r = false; break;
        case 1: r = a == b; break;
        case 2: r = a < b; break;
        case 3: r = a <= b; break;
        case 4: r = true; break;
        case 5: r = a != b; break;
        case 6: r = a >= b; break;
        case 7: r = a > b; break;
        case 8: r = a == 0.0f; break;
        case 9: r = nan; break;
        case 10: r = inf; break;
        case 11: r = nan || inf; break;
        case 12: r = a != 0.0f; break;
        case 13: r = !nan; break;
        case 14: r = !inf; break;
        default: r = !(nan || inf); break;
        }
        cc |= u32(r) << i;
        any |= u32(r);
        all &= u32(r);
        affected |= 1u << i;
    }
    u32 value = cc | (any << 4) | (all << 5);
    c->vctrl[kVfpuCc] = (c->vctrl[kVfpuCc] & ~affected) | (value & affected);
    eat_prefixes(c);
}

// vcmovt / vcmovf
P2_INLINE void vcmov(Cpu* c, u32 w) {
    u32 size = f_size(w);
    u32 tf = (w >> 19) & 1, imm3 = (w >> 16) & 7;
    Vec s, d;
    read_vec(c, s, size, f_vs(w));
    swizzle_s(c, s, size);
    read_vec(c, d, size, f_vd(w));
    swizzle_t(c, d, size);
    u32 cc = c->vctrl[kVfpuCc];
    if (imm3 < 6) {
        if (((cc >> imm3) & 1) == (tf ^ 1))
            for (u32 i = 0; i <= size; ++i) d.u[i] = s.u[i];
    } else if (imm3 == 6) {
        for (u32 i = 0; i <= size; ++i)
            if (((cc >> i) & 1) == (tf ^ 1)) d.u[i] = s.u[i];
    }
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

// --- rotation -----------------------------------------------------------------------------------------------

// vrot: writes sin/cos of a quarter-turn angle into selected lanes.
P2_INLINE void vrot(Cpu* c, u32 w) {
    u32 size = f_size(w);
    u32 imm = (w >> 16) & 31;
    u32 sin_lane = (imm >> 2) & 3, cos_lane = imm & 3;
    f32 angle = c->v[f_vs(w)];
    f32 sine = quarter_sin(angle), cosine = quarter_cos(angle);
    if (imm & 0x10) sine = -sine;
    Vec d{};
    if (sin_lane == cos_lane) {
        for (u32 i = 0; i < 4; ++i) d.f[i] = sine;
    } else {
        d.f[sin_lane] = sine;
    }
    d.f[cos_lane] = cosine;
    saturate_d(c, d, size);
    write_vec(c, d, size, f_vd(w));
    eat_prefixes(c);
}

// --- matrices -----------------------------------------------------------------------------------------------

P2_INLINE void vmmov(Cpu* c, u32 w) {
    u32 size = f_size(w);
    f32 s[16];
    read_mat(c, s, size, f_vs(w));
    write_mat(c, s, size, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vmfill(Cpu* c, u32 w, f32 diagonal, f32 other) {
    u32 size = f_size(w);
    f32 d[16];
    for (u32 j = 0; j < 4; ++j)
        for (u32 i = 0; i < 4; ++i) d[j * 4 + i] = i == j ? diagonal : other;
    write_mat(c, d, size, f_vd(w));
    eat_prefixes(c);
}

P2_INLINE void vmidt(Cpu* c, u32 w) { vmfill(c, w, 1.0f, 0.0f); }
P2_INLINE void vmzero(Cpu* c, u32 w) { vmfill(c, w, 0.0f, 0.0f); }
P2_INLINE void vmone(Cpu* c, u32 w) { vmfill(c, w, 1.0f, 1.0f); }

P2_INLINE void vmscl(Cpu* c, u32 w) {
    u32 size = f_size(w);
    f32 s[16];
    read_mat(c, s, size, f_vs(w));
    f32 scale = c->v[f_vt(w)];
    for (f32& e : s) e *= scale;
    write_mat(c, s, size, f_vd(w));
    eat_prefixes(c);
}

// vmmul: element (column a, row b) of the result is the dot product of column b of the first
// operand with column a of the second, which is how the hardware defines it.
P2_INLINE void vmmul(Cpu* c, u32 w) {
    u32 size = f_size(w);
    f32 s[16], t[16], d[16];
    read_mat(c, s, size, f_vs(w));
    read_mat(c, t, size, f_vt(w));
    for (u32 a = 0; a <= size; ++a) {
        for (u32 b = 0; b <= size; ++b) {
            f32 sum = 0.0f;
            for (u32 k = 0; k < 4; ++k) sum += s[b * 4 + k] * t[a * 4 + k];
            d[a * 4 + b] = sum;
        }
    }
    write_mat(c, d, size, f_vd(w));
    eat_prefixes(c);
}

// vtfmN / vhtfmN: matrix times vector. The homogeneous forms treat the missing last component as 1.
P2_INLINE void vtfm(Cpu* c, u32 w) {
    u32 ins = (w >> 23) & 3;       // matrix side - 1
    u32 n = f_size(w) + 1;         // elements named by the instruction
    bool homogeneous = n == ins;   // one element short of the matrix side
    u32 side = ins + 1;
    f32 s[16];
    Vec t, d{};
    read_mat(c, s, ins, f_vs(w));
    read_vec(c, t, ins, f_vt(w));
    if (homogeneous) t.f[side - 1] = 1.0f;
    for (u32 i = 0; i < side; ++i) {
        f32 sum = 0.0f;
        for (u32 k = 0; k < side; ++k) sum += s[k * 4 + i] * t.f[k];
        d.f[i] = sum;
    }
    write_vec(c, d, ins, f_vd(w));
    eat_prefixes(c);
}

} // namespace p2::vfpu
