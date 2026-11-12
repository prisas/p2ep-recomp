#pragma once

// Instruction semantics shared by the recompiled code and the unit tests. Everything here is a
// small always-inline helper so the optimiser sees through it.

#include "runtime/cpu/cpu.h"

#include <bit>
#include <cmath>
#include <xmmintrin.h>

namespace p2 {

// --- guest memory -------------------------------------------------------------------------------------------
// `m` is the host address of guest address 0. The whole 4 GiB guest space is reserved, so a guest
// address is used as an index without masking; unmapped addresses fault and are reported.

P2_INLINE u8 rd8(const u8* m, u32 a) { return m[a]; }
P2_INLINE u16 rd16(const u8* m, u32 a) { return load_le<u16>(m + a); }
P2_INLINE u32 rd32(const u8* m, u32 a) { return load_le<u32>(m + a); }
P2_INLINE u64 rd64(const u8* m, u32 a) { return load_le<u64>(m + a); }
P2_INLINE f32 rdf32(const u8* m, u32 a) { return load_le<f32>(m + a); }
P2_INLINE void wr8(u8* m, u32 a, u8 v) { m[a] = v; }
P2_INLINE void wr16(u8* m, u32 a, u16 v) { store_le<u16>(m + a, v); }
P2_INLINE void wr32(u8* m, u32 a, u32 v) { store_le<u32>(m + a, v); }
P2_INLINE void wr64(u8* m, u32 a, u64 v) { store_le<u64>(m + a, v); }
P2_INLINE void wrf32(u8* m, u32 a, f32 v) { store_le<f32>(m + a, v); }

// Unaligned word access through the left/right instruction pairs.
P2_INLINE u32 op_lwl(const u8* m, u32 addr, u32 reg) {
    u32 shift = (addr & 3) * 8;
    u32 mem = rd32(m, addr & ~3u);
    return (reg & (0x00FFFFFFu >> shift)) | (mem << (24 - shift));
}

P2_INLINE u32 op_lwr(const u8* m, u32 addr, u32 reg) {
    u32 shift = (addr & 3) * 8;
    u32 mem = rd32(m, addr & ~3u);
    // (24 - shift) can be 24, so widen before shifting the mask.
    return (reg & u32(u64(0xFFFFFF00u) << (24 - shift))) | (mem >> shift);
}

P2_INLINE void op_swl(u8* m, u32 addr, u32 reg) {
    u32 shift = (addr & 3) * 8;
    u32 mem = rd32(m, addr & ~3u);
    wr32(m, addr & ~3u, (reg >> (24 - shift)) | (mem & u32(u64(0xFFFFFF00u) << shift)));
}

P2_INLINE void op_swr(u8* m, u32 addr, u32 reg) {
    u32 shift = (addr & 3) * 8;
    u32 mem = rd32(m, addr & ~3u);
    wr32(m, addr & ~3u, (reg << shift) | (mem & (0x00FFFFFFu >> (24 - shift))));
}

// --- integer ------------------------------------------------------------------------------------------------

P2_INLINE u32 op_clz(u32 v) { return u32(std::countl_zero(v)); }
P2_INLINE u32 op_clo(u32 v) { return u32(std::countl_one(v)); }
P2_INLINE u32 op_rotr(u32 v, u32 n) { return std::rotr(v, int(n & 31)); }
P2_INLINE u32 op_seb(u32 v) { return u32(s32(s8(v))); }
P2_INLINE u32 op_seh(u32 v) { return u32(s32(s16(v))); }
P2_INLINE u32 op_wsbh(u32 v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
P2_INLINE u32 op_wsbw(u32 v) { return __builtin_bswap32(v); }

P2_INLINE u32 op_bitrev(u32 v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(v);
}

P2_INLINE u32 op_ext(u32 v, u32 pos, u32 size) { return (v >> pos) & u32((u64(1) << size) - 1); }

P2_INLINE u32 op_ins(u32 dst, u32 src, u32 pos, u32 size) {
    u32 mask = u32((u64(1) << size) - 1) << pos;
    return (dst & ~mask) | ((src << pos) & mask);
}

P2_INLINE u32 op_max(u32 a, u32 b) { return s32(a) > s32(b) ? a : b; }
P2_INLINE u32 op_min(u32 a, u32 b) { return s32(a) < s32(b) ? a : b; }

P2_INLINE void op_mult(Cpu* c, u32 a, u32 b) {
    s64 r = s64(s32(a)) * s64(s32(b));
    c->lo = u32(r);
    c->hi = u32(u64(r) >> 32);
}

P2_INLINE void op_multu(Cpu* c, u32 a, u32 b) {
    u64 r = u64(a) * u64(b);
    c->lo = u32(r);
    c->hi = u32(r >> 32);
}

P2_INLINE void op_madd(Cpu* c, u32 a, u32 b) {
    u64 acc = (u64(c->hi) << 32) | c->lo;
    acc += u64(s64(s32(a)) * s64(s32(b)));
    c->lo = u32(acc);
    c->hi = u32(acc >> 32);
}

P2_INLINE void op_maddu(Cpu* c, u32 a, u32 b) {
    u64 acc = ((u64(c->hi) << 32) | c->lo) + u64(a) * u64(b);
    c->lo = u32(acc);
    c->hi = u32(acc >> 32);
}

P2_INLINE void op_msub(Cpu* c, u32 a, u32 b) {
    u64 acc = (u64(c->hi) << 32) | c->lo;
    acc -= u64(s64(s32(a)) * s64(s32(b)));
    c->lo = u32(acc);
    c->hi = u32(acc >> 32);
}

P2_INLINE void op_msubu(Cpu* c, u32 a, u32 b) {
    u64 acc = ((u64(c->hi) << 32) | c->lo) - u64(a) * u64(b);
    c->lo = u32(acc);
    c->hi = u32(acc >> 32);
}

// Division never traps. The results for a zero divisor and for the one overflowing quotient
// match what the hardware leaves in HI/LO.
P2_INLINE void op_div(Cpu* c, u32 ua, u32 ub) {
    s32 a = s32(ua), b = s32(ub);
    if (P2_UNLIKELY(b == 0)) {
        c->lo = a < 0 ? 1u : 0xFFFFFFFFu;
        c->hi = ua;
    } else if (P2_UNLIKELY(ua == 0x80000000u && b == -1)) {
        c->lo = 0x80000000u;
        c->hi = 0xFFFFFFFFu;
    } else {
        c->lo = u32(a / b);
        c->hi = u32(a % b);
    }
}

P2_INLINE void op_divu(Cpu* c, u32 a, u32 b) {
    if (P2_UNLIKELY(b == 0)) {
        c->lo = a <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu;
        c->hi = a;
    } else {
        c->lo = a / b;
        c->hi = a % b;
    }
}

// --- FPU ----------------------------------------------------------------------------------------------------

P2_INLINE u32 fpu_convert_special(f32 v, bool* special) {
    // NaN and out-of-range inputs saturate instead of producing the host's "integer indefinite".
    if (P2_UNLIKELY(!(v > -2147483904.0f && v < 2147483648.0f))) {
        *special = true;
        return (v != v || v > 0.0f) ? 0x7FFFFFFFu : 0x80000000u;
    }
    *special = false;
    return 0;
}

P2_INLINE u32 fpu_trunc_w(f32 v) {
    bool special;
    u32 r = fpu_convert_special(v, &special);
    return special ? r : u32(s32(v));
}

P2_INLINE u32 fpu_round_w(f32 v) { // to nearest, ties to even
    bool special;
    u32 r = fpu_convert_special(v, &special);
    if (special) return r;
    // Written out so the result does not depend on the host rounding mode.
    f32 down = std::floor(v);
    f32 frac = v - down;
    s32 i = s32(down);
    if (frac > 0.5f || (frac == 0.5f && (i & 1))) ++i;
    return u32(i);
}

P2_INLINE u32 fpu_ceil_w(f32 v) {
    bool special;
    u32 r = fpu_convert_special(v, &special);
    return special ? r : u32(s32(std::ceil(v)));
}

P2_INLINE u32 fpu_floor_w(f32 v) {
    bool special;
    u32 r = fpu_convert_special(v, &special);
    return special ? r : u32(s32(std::floor(v)));
}

// cvt.w.s honours the rounding mode in FCR31.
P2_INLINE u32 fpu_cvt_w(const Cpu* c, f32 v) {
    switch (c->fcr31 & 3) {
    case 0: return fpu_round_w(v);
    case 1: return fpu_trunc_w(v);
    case 2: return fpu_ceil_w(v);
    default: return fpu_floor_w(v);
    }
}

P2_INLINE u32 fpu_get_fcr31(const Cpu* c) { return (c->fcr31 & ~(1u << 23)) | (c->fcc << 23); }

// Applies the guest rounding and flush-to-zero bits to the host FPU.
P2_INLINE void fpu_apply_mode(const Cpu* c) {
    static constexpr u32 kRound[4] = {_MM_ROUND_NEAREST, _MM_ROUND_TOWARD_ZERO, _MM_ROUND_UP, _MM_ROUND_DOWN};
    u32 csr = _mm_getcsr() & ~u32(_MM_ROUND_MASK | 0x8040); // rounding, FTZ, DAZ
    csr |= kRound[c->fcr31 & 3];
    if (c->fcr31 & (1u << 24)) csr |= 0x8040;
    _mm_setcsr(csr);
}

P2_INLINE void fpu_set_fcr31(Cpu* c, u32 value) {
    c->fcr31 = value & 0x0181FFFFu & ~(1u << 23);
    c->fcc = (value >> 23) & 1;
    fpu_apply_mode(c);
}

// The eight ordered/unordered predicates of c.cond.s (bit 3 of the condition only selects signalling).
P2_INLINE u32 fpu_compare(f32 a, f32 b, u32 cond) {
    bool unordered = a != a || b != b;
    bool result = ((cond & 1) && unordered) || ((cond & 2) && a == b) || ((cond & 4) && a < b);
    return result ? 1u : 0u;
}

} // namespace p2
