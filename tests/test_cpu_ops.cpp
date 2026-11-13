#include "runtime/cpu/vfpu.h"
#include "tests/test.h"

#include <cmath>
#include <limits>

using namespace p2;

namespace {

struct Fixture {
    Cpu cpu;
    u8 mem[64];
    Fixture() {
        cpu.reset();
        for (u32 i = 0; i < sizeof(mem); ++i) mem[i] = u8(0x11 * ((i % 15) + 1));
    }
};

// Encodes a VFPU register operand: matrix, column and the size-dependent row/transpose field.
constexpr u32 vreg(u32 matrix, u32 column, u32 high) { return (matrix << 2) | column | (high << 5); }

constexpr u32 kQuad = 0x8080, kTriple = 0x8000, kPair = 0x0080, kSingle = 0;

constexpr u32 op3(u32 base, u32 size, u32 vd, u32 vs, u32 vt) { return base | size | (vt << 16) | (vs << 8) | vd; }

void set_quad(Cpu& c, u32 matrix, u32 column, f32 a, f32 b, f32 d, f32 e) {
    c.v[matrix * 4 + column + 0 * 32] = a;
    c.v[matrix * 4 + column + 1 * 32] = b;
    c.v[matrix * 4 + column + 2 * 32] = d;
    c.v[matrix * 4 + column + 3 * 32] = e;
}

f32 elem(const Cpu& c, u32 matrix, u32 column, u32 row) { return c.v[matrix * 4 + column + row * 32]; }

} // namespace

TEST(ops_unaligned_loads) {
    Fixture f;
    const u8 bytes[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::memcpy(f.mem, bytes, 8);
    for (u32 offset = 0; offset < 4; ++offset) {
        u32 reg = 0xDEADBEEF;
        reg = op_lwl(f.mem, offset + 3, reg);
        reg = op_lwr(f.mem, offset, reg);
        CHECK_EQ(reg, load_le<u32>(bytes + offset));

        // The other order must give the same result.
        reg = 0xDEADBEEF;
        reg = op_lwr(f.mem, offset, reg);
        reg = op_lwl(f.mem, offset + 3, reg);
        CHECK_EQ(reg, load_le<u32>(bytes + offset));
    }
    // A lone lwr at offset 1 keeps the top byte of the register.
    CHECK_EQ(op_lwr(f.mem, 1, 0xAA000000u), 0xAA443322u);
    CHECK_EQ(op_lwl(f.mem, 1, 0x0000BBCCu), 0x2211BBCCu);
}

TEST(ops_unaligned_stores) {
    for (u32 offset = 0; offset < 4; ++offset) {
        Fixture f;
        std::memset(f.mem, 0xEE, 16);
        op_swl(f.mem, offset + 3, 0xA1B2C3D4u);
        op_swr(f.mem, offset, 0xA1B2C3D4u);
        CHECK_EQ(load_le<u32>(f.mem + offset), 0xA1B2C3D4u);
        for (u32 i = 0; i < 12; ++i)
            if (i < offset || i >= offset + 4) CHECK_EQ(f.mem[i], u8(0xEE));
    }
}

TEST(ops_division) {
    Cpu c;
    c.reset();
    op_div(&c, u32(-7), 2);
    CHECK_EQ(c.lo, u32(-3));
    CHECK_EQ(c.hi, u32(-1));
    op_div(&c, 7, 0);
    CHECK_EQ(c.lo, 0xFFFFFFFFu);
    CHECK_EQ(c.hi, 7u);
    op_div(&c, u32(-7), 0);
    CHECK_EQ(c.lo, 1u);
    CHECK_EQ(c.hi, u32(-7));
    op_div(&c, 0x80000000u, 0xFFFFFFFFu);
    CHECK_EQ(c.lo, 0x80000000u);
    op_divu(&c, 100, 7);
    CHECK_EQ(c.lo, 14u);
    CHECK_EQ(c.hi, 2u);
    op_divu(&c, 0x12345, 0);
    CHECK_EQ(c.lo, 0xFFFFFFFFu);
    CHECK_EQ(c.hi, 0x12345u);
    op_divu(&c, 0x1234, 0);
    CHECK_EQ(c.lo, 0xFFFFu);
}

TEST(ops_multiply_accumulate) {
    Cpu c;
    c.reset();
    op_mult(&c, u32(-3), 5);
    CHECK_EQ(c.lo, u32(-15));
    CHECK_EQ(c.hi, 0xFFFFFFFFu);
    op_multu(&c, 0xFFFFFFFFu, 2);
    CHECK_EQ(c.lo, 0xFFFFFFFEu);
    CHECK_EQ(c.hi, 1u);
    c.hi = 0;
    c.lo = 10;
    op_madd(&c, 3, u32(-4));
    CHECK_EQ(c.lo, u32(-2));
    CHECK_EQ(c.hi, 0xFFFFFFFFu);
    c.hi = 0;
    c.lo = 10;
    op_msub(&c, 3, 4);
    CHECK_EQ(c.lo, u32(-2));
    CHECK_EQ(c.hi, 0xFFFFFFFFu);
    c.hi = 1;
    c.lo = 0;
    op_maddu(&c, 0x80000000u, 2);
    CHECK_EQ(c.lo, 0u);
    CHECK_EQ(c.hi, 2u);
    op_msubu(&c, 1, 1);
    CHECK_EQ(c.lo, 0xFFFFFFFFu);
    CHECK_EQ(c.hi, 1u);
}

TEST(ops_bit_manipulation) {
    CHECK_EQ(op_clz(0), 32u);
    CHECK_EQ(op_clz(1), 31u);
    CHECK_EQ(op_clz(0x80000000u), 0u);
    CHECK_EQ(op_clo(0xFFFFFFFFu), 32u);
    CHECK_EQ(op_clo(0xF0000000u), 4u);
    CHECK_EQ(op_rotr(0x80000001u, 1), 0xC0000000u);
    CHECK_EQ(op_rotr(0x12345678u, 0), 0x12345678u);
    CHECK_EQ(op_rotr(0x12345678u, 36), 0x81234567u);
    CHECK_EQ(op_seb(0x80), 0xFFFFFF80u);
    CHECK_EQ(op_seb(0x17F), 0x7Fu);
    CHECK_EQ(op_seh(0x8000), 0xFFFF8000u);
    CHECK_EQ(op_wsbh(0x11223344u), 0x22114433u);
    CHECK_EQ(op_wsbw(0x11223344u), 0x44332211u);
    CHECK_EQ(op_bitrev(0x00000001u), 0x80000000u);
    CHECK_EQ(op_bitrev(0x12345678u), 0x1E6A2C48u);
    CHECK_EQ(op_ext(0xABCD1234u, 4, 8), 0x23u);
    CHECK_EQ(op_ext(0xABCD1234u, 0, 32), 0xABCD1234u);
    CHECK_EQ(op_ins(0xFFFFFFFFu, 0, 8, 8), 0xFFFF00FFu);
    CHECK_EQ(op_ins(0, 0xFFFFFFFFu, 0, 32), 0xFFFFFFFFu);
    CHECK_EQ(op_ins(0x12345678u, 0xAB, 28, 4), 0xB2345678u);
    CHECK_EQ(op_max(u32(-1), 1), 1u);
    CHECK_EQ(op_min(u32(-1), 1), u32(-1));
}

TEST(ops_fpu_conversions) {
    CHECK_EQ(fpu_trunc_w(1.9f), 1u);
    CHECK_EQ(fpu_trunc_w(-1.9f), u32(-1));
    CHECK_EQ(fpu_round_w(2.5f), 2u);
    CHECK_EQ(fpu_round_w(3.5f), 4u);
    CHECK_EQ(fpu_round_w(-2.5f), u32(-2));
    CHECK_EQ(fpu_round_w(-3.5f), u32(-4));
    CHECK_EQ(fpu_round_w(2.4f), 2u);
    CHECK_EQ(fpu_round_w(2.6f), 3u);
    CHECK_EQ(fpu_ceil_w(1.1f), 2u);
    CHECK_EQ(fpu_ceil_w(-1.1f), u32(-1));
    CHECK_EQ(fpu_floor_w(1.9f), 1u);
    CHECK_EQ(fpu_floor_w(-1.1f), u32(-2));

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    CHECK_EQ(fpu_trunc_w(nan), 0x7FFFFFFFu);
    CHECK_EQ(fpu_trunc_w(inf), 0x7FFFFFFFu);
    CHECK_EQ(fpu_trunc_w(-inf), 0x80000000u);
    CHECK_EQ(fpu_trunc_w(3e9f), 0x7FFFFFFFu);
    CHECK_EQ(fpu_trunc_w(-3e9f), 0x80000000u);
    CHECK_EQ(fpu_trunc_w(-2147483648.0f), 0x80000000u);
    CHECK_EQ(fpu_trunc_w(2147483520.0f), 2147483520u);

    Cpu c;
    c.reset();
    c.fcr31 = 0;
    CHECK_EQ(fpu_cvt_w(&c, 2.5f), 2u);
    c.fcr31 = 1;
    CHECK_EQ(fpu_cvt_w(&c, 2.9f), 2u);
    c.fcr31 = 2;
    CHECK_EQ(fpu_cvt_w(&c, 2.1f), 3u);
    c.fcr31 = 3;
    CHECK_EQ(fpu_cvt_w(&c, -2.1f), u32(-3));
}

TEST(ops_fpu_compare_and_control) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK_EQ(fpu_compare(1.0f, 2.0f, 4), 1u); // olt
    CHECK_EQ(fpu_compare(2.0f, 1.0f, 4), 0u);
    CHECK_EQ(fpu_compare(1.0f, 1.0f, 6), 1u); // ole
    CHECK_EQ(fpu_compare(1.0f, 1.0f, 2), 1u); // eq
    CHECK_EQ(fpu_compare(nan, 1.0f, 2), 0u);
    CHECK_EQ(fpu_compare(nan, 1.0f, 1), 1u); // un
    CHECK_EQ(fpu_compare(nan, 1.0f, 3), 1u); // ueq
    CHECK_EQ(fpu_compare(nan, 1.0f, 5), 1u); // ult
    CHECK_EQ(fpu_compare(nan, 1.0f, 4), 0u);
    CHECK_EQ(fpu_compare(1.0f, 2.0f, 0), 0u);

    Cpu c;
    c.reset();
    const u32 saved = _mm_getcsr();
    fpu_set_fcr31(&c, (1u << 23) | 1);
    CHECK_EQ(c.fcc, 1u);
    CHECK_EQ(fpu_get_fcr31(&c), (1u << 23) | 1u);
    CHECK_EQ(_mm_getcsr() & u32(_MM_ROUND_MASK), u32(_MM_ROUND_TOWARD_ZERO));
    fpu_set_fcr31(&c, 0);
    CHECK_EQ(c.fcc, 0u);
    CHECK_EQ(_mm_getcsr() & u32(_MM_ROUND_MASK), u32(_MM_ROUND_NEAREST));
    _mm_setcsr(saved);
}

TEST(vfpu_register_addressing) {
    u8 r[4];
    vfpu::vec_regs(vreg(0, 0, 0), 3, r); // column 0 of matrix 0
    CHECK(r[0] == 0 && r[1] == 32 && r[2] == 64 && r[3] == 96);
    vfpu::vec_regs(vreg(0, 0, 1), 3, r); // row 0 of matrix 0
    CHECK(r[0] == 0 && r[1] == 1 && r[2] == 2 && r[3] == 3);
    vfpu::vec_regs(vreg(2, 1, 0), 3, r); // column 1 of matrix 2
    CHECK(r[0] == 9 && r[1] == 41 && r[2] == 73 && r[3] == 105);
    vfpu::vec_regs(vreg(1, 3, 2), 0, r); // single: matrix 1, column 3, row 2
    CHECK(r[0] == 4 + 3 + 64);
    vfpu::vec_regs(vreg(0, 2, 2), 2, r); // triple starting at row 1
    CHECK(r[0] == 2 + 32 && r[1] == 2 + 64 && r[2] == 2 + 96);
    vfpu::vec_regs(vreg(0, 1, 2), 1, r); // pair starting at row 2
    CHECK(r[0] == 1 + 64 && r[1] == 1 + 96);

    u8 m[16];
    vfpu::mat_regs(vreg(3, 0, 0), 3, m);
    CHECK(m[0] == 12 && m[1] == 12 + 32 && m[4] == 13 && m[15] == 15 + 96);
    vfpu::mat_regs(vreg(3, 0, 1), 3, m); // transposed view
    CHECK(m[0] == 12 && m[1] == 13 && m[4] == 12 + 32 && m[15] == 15 + 96);
}

TEST(vfpu_arithmetic) {
    Fixture f;
    set_quad(f.cpu, 0, 0, 1, 2, 3, 4);
    set_quad(f.cpu, 0, 1, 10, 20, 30, 40);
    vfpu::vadd(&f.cpu, op3(0x60000000, kQuad, vreg(0, 2, 0), vreg(0, 0, 0), vreg(0, 1, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 11.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 3), 44.0f);
    vfpu::vsub(&f.cpu, op3(0x60800000, kQuad, vreg(0, 2, 0), vreg(0, 1, 0), vreg(0, 0, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 1), 18.0f);
    vfpu::vmul(&f.cpu, op3(0x64000000, kTriple, vreg(0, 2, 0), vreg(0, 0, 0), vreg(0, 1, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 2), 90.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 3), 36.0f); // fourth lane untouched by a triple
    vfpu::vdiv(&f.cpu, op3(0x63800000, kPair, vreg(0, 2, 0), vreg(0, 1, 0), vreg(0, 0, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 10.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 1), 10.0f);

    vfpu::vdot(&f.cpu, op3(0x64800000, kQuad, vreg(1, 0, 0), vreg(0, 0, 0), vreg(0, 1, 0)));
    CHECK_EQ(f.cpu.v[4], 300.0f);
    f.cpu.v[5] = 0.5f; // S110... single register matrix 1 column 1 row 0
    vfpu::vscl(&f.cpu, op3(0x65000000, kQuad, vreg(0, 3, 0), vreg(0, 0, 0), vreg(1, 1, 0)));
    CHECK_EQ(elem(f.cpu, 0, 3, 3), 2.0f);

    vfpu::vmin(&f.cpu, op3(0x6D000000, kQuad, vreg(0, 2, 0), vreg(0, 0, 0), vreg(0, 1, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 1.0f);
    vfpu::vmax(&f.cpu, op3(0x6D800000, kQuad, vreg(0, 2, 0), vreg(0, 0, 0), vreg(0, 1, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 10.0f);

    set_quad(f.cpu, 0, 0, 1, 0, 0, 0);
    set_quad(f.cpu, 0, 1, 0, 1, 0, 0);
    vfpu::vcrsp(&f.cpu, op3(0xF2800000, kTriple, vreg(0, 2, 0), vreg(0, 0, 0), vreg(0, 1, 0)));
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 0.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 1), 0.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 2), 1.0f);
}

TEST(vfpu_prefixes) {
    Fixture f;
    set_quad(f.cpu, 0, 0, 1, 2, 3, 4);
    // Reverse the lanes for the next instruction only.
    vfpu::vpfx(&f.cpu, 0xDC000000 | 0x1B);
    vfpu::vmov(&f.cpu, op3(0xD0000000, kQuad, vreg(0, 1, 0), vreg(0, 0, 0), 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 0), 4.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 3), 1.0f);
    CHECK_EQ(f.cpu.vctrl[kVfpuSpfx], kVfpuPrefixIdentity);
    vfpu::vmov(&f.cpu, op3(0xD0000000, kQuad, vreg(0, 1, 0), vreg(0, 0, 0), 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 0), 1.0f);

    // Negate lane 0, absolute value of lane 1, constants 1 and 1/2 in lanes 2 and 3.
    set_quad(f.cpu, 0, 0, 5, -6, 7, 8);
    u32 prefix = 0xE4 | (1u << 16) | (1u << 9) | (1u << 14) | (1u << 15);
    prefix = (prefix & ~0xF0u) | (1u << 4) | (3u << 6); // constant selectors: 1 -> 1.0, 3 -> 0.5
    vfpu::vpfx(&f.cpu, 0xDC000000 | prefix);
    vfpu::vmov(&f.cpu, op3(0xD0000000, kQuad, vreg(0, 1, 0), vreg(0, 0, 0), 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 0), -5.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 1), 6.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 2), 1.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 3), 0.5f);

    // Destination prefix: clamp lane 0 to [0,1], lane 1 to [-1,1], mask lane 3 from being written.
    set_quad(f.cpu, 0, 0, 5, -6, 7, 8);
    set_quad(f.cpu, 0, 1, 0, 0, 0, 99);
    vfpu::vpfx(&f.cpu, 0xDE000000 | 1 | (3 << 2) | (1 << 11));
    vfpu::vmov(&f.cpu, op3(0xD0000000, kQuad, vreg(0, 1, 0), vreg(0, 0, 0), 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 0), 1.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 1), -1.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 2), 7.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 3), 99.0f);
    CHECK_EQ(f.cpu.vctrl[kVfpuDpfx], 0u);
}

TEST(vfpu_constants_and_conversions) {
    Fixture f;
    vfpu::vcst(&f.cpu, op3(0xD0600000, kSingle, vreg(0, 0, 0), 0, 9)); // pi
    CHECK(std::fabs(f.cpu.v[0] - 3.14159265f) < 1e-6f);
    vfpu::vzero(&f.cpu, op3(0xD0060000, kQuad, vreg(0, 1, 0), 0, 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 2), 0.0f);
    vfpu::vone(&f.cpu, op3(0xD0070000, kQuad, vreg(0, 1, 0), 0, 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 2), 1.0f);
    vfpu::vidt(&f.cpu, op3(0xD0030000, kQuad, vreg(0, 2, 0), 0, 0));
    CHECK_EQ(elem(f.cpu, 0, 2, 2), 1.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 0.0f);

    vfpu::viim(&f.cpu, 0xDF000000 | (vreg(0, 0, 0) << 16) | 0xFFFE);
    CHECK_EQ(f.cpu.v[0], -2.0f);
    vfpu::vfim(&f.cpu, 0xDF800000 | (vreg(0, 0, 0) << 16) | 0x3C00);
    CHECK_EQ(f.cpu.v[0], 1.0f);
    CHECK_EQ(vfpu::half_to_float(0xC000), -2.0f);
    CHECK_EQ(vfpu::half_to_float(0x0001), std::ldexp(1.0f, -24));
    CHECK_EQ(vfpu::half_to_float(0x7C00), std::numeric_limits<f32>::infinity());

    f.cpu.vi[0] = 256;
    vfpu::vi2f(&f.cpu, op3(0xD2800000, kSingle, vreg(0, 1, 0), vreg(0, 0, 0), 4)); // divide by 2^4
    CHECK_EQ(f.cpu.v[1], 16.0f);
    f.cpu.v[0] = 2.75f;
    vfpu::vf2i(&f.cpu, op3(0xD2200000, kSingle, vreg(0, 1, 0), vreg(0, 0, 0), 1)); // vf2iz, times 2
    CHECK_EQ(f.cpu.vi[1], 5u);
    f.cpu.v[0] = -2.75f;
    vfpu::vf2i(&f.cpu, op3(0xD2600000, kSingle, vreg(0, 1, 0), vreg(0, 0, 0), 0)); // vf2id
    CHECK_EQ(f.cpu.vi[1], u32(-3));
    vfpu::vf2i(&f.cpu, op3(0xD2400000, kSingle, vreg(0, 1, 0), vreg(0, 0, 0), 0)); // vf2iu
    CHECK_EQ(f.cpu.vi[1], u32(-2));

    // Colour bytes to integers and back.
    f.cpu.vi[0] = 0x80FF4000u;
    vfpu::vx2i(&f.cpu, op3(0xD0380000, kSingle, vreg(0, 1, 0), vreg(0, 0, 0), 0) | kQuad); // vuc2i
    CHECK_EQ(f.cpu.vi[1 + 0 * 32], 0u);
    CHECK_EQ(f.cpu.vi[1 + 1 * 32], 0x20202020u);
    CHECK_EQ(f.cpu.vi[1 + 2 * 32], 0x7FFFFFFFu);
    vfpu::vi2x(&f.cpu, op3(0xD03C0000, kQuad, vreg(0, 2, 0), vreg(0, 1, 0), 0)); // vi2uc
    CHECK_EQ(f.cpu.vi[2], 0x80FF4000u);
}

TEST(vfpu_compare_and_select) {
    Fixture f;
    set_quad(f.cpu, 0, 0, 1, 5, 3, 9);
    set_quad(f.cpu, 0, 1, 2, 4, 3, 8);
    vfpu::vcmp(&f.cpu, op3(0x6C000000, kQuad, 2 /* LT */, vreg(0, 0, 0), vreg(0, 1, 0)));
    u32 cc = f.cpu.vctrl[kVfpuCc];
    CHECK_EQ(cc & 0xF, 1u);        // only lane 0 is less
    CHECK_EQ((cc >> 4) & 1, 1u);   // any
    CHECK_EQ((cc >> 5) & 1, 0u);   // all

    // vcmovt on "any": copies the whole vector.
    set_quad(f.cpu, 0, 2, 0, 0, 0, 0);
    vfpu::vcmov(&f.cpu, 0xD2A00000 | kQuad | (4u << 16) | (vreg(0, 0, 0) << 8) | vreg(0, 2, 0));
    CHECK_EQ(elem(f.cpu, 0, 2, 1), 5.0f);
    // vcmovt per lane (imm3 == 6): only lane 0 copies.
    set_quad(f.cpu, 0, 2, 0, 0, 0, 0);
    vfpu::vcmov(&f.cpu, 0xD2A00000 | kQuad | (6u << 16) | (vreg(0, 0, 0) << 8) | vreg(0, 2, 0));
    CHECK_EQ(elem(f.cpu, 0, 2, 0), 1.0f);
    CHECK_EQ(elem(f.cpu, 0, 2, 1), 0.0f);
}

TEST(vfpu_matrices) {
    Fixture f;
    vfpu::vmidt(&f.cpu, 0xF3830000 | kQuad | vreg(0, 0, 0));
    CHECK_EQ(elem(f.cpu, 0, 0, 0), 1.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 0), 0.0f);
    CHECK_EQ(elem(f.cpu, 0, 3, 3), 1.0f);

    // Matrix 1: scale by (2,3,4) with translation (10,20,30) in the last column.
    vfpu::vmidt(&f.cpu, 0xF3830000 | kQuad | vreg(1, 0, 0));
    f.cpu.v[4 + 0 + 0 * 32] = 2;
    f.cpu.v[4 + 1 + 1 * 32] = 3;
    f.cpu.v[4 + 2 + 2 * 32] = 4;
    set_quad(f.cpu, 1, 3, 10, 20, 30, 1);

    set_quad(f.cpu, 2, 0, 1, 1, 1, 1);
    vfpu::vtfm(&f.cpu, 0xF1800000 | kQuad | (vreg(2, 0, 0) << 16) | (vreg(1, 0, 0) << 8) | vreg(2, 1, 0)); // vtfm4.q
    CHECK_EQ(elem(f.cpu, 2, 1, 0), 12.0f);
    CHECK_EQ(elem(f.cpu, 2, 1, 1), 23.0f);
    CHECK_EQ(elem(f.cpu, 2, 1, 2), 34.0f);
    CHECK_EQ(elem(f.cpu, 2, 1, 3), 1.0f);

    // Homogeneous form: a triple with an implied w of 1 gives the same result.
    set_quad(f.cpu, 2, 0, 1, 1, 1, 77);
    vfpu::vtfm(&f.cpu, 0xF1800000 | kTriple | (vreg(2, 0, 0) << 16) | (vreg(1, 0, 0) << 8) | vreg(2, 2, 0)); // vhtfm4.q
    CHECK_EQ(elem(f.cpu, 2, 2, 0), 12.0f);
    CHECK_EQ(elem(f.cpu, 2, 2, 2), 34.0f);

    // Identity times M is M.
    vfpu::vmmul(&f.cpu, 0xF0000000 | kQuad | (vreg(1, 0, 0) << 16) | (vreg(0, 0, 0) << 8) | vreg(3, 0, 0));
    CHECK_EQ(elem(f.cpu, 3, 0, 0), 2.0f);
    CHECK_EQ(elem(f.cpu, 3, 3, 1), 20.0f);
    vfpu::vmmov(&f.cpu, 0xF3800000 | kQuad | (vreg(1, 0, 0) << 8) | vreg(4, 0, 0));
    CHECK_EQ(elem(f.cpu, 4, 2, 2), 4.0f);
    f.cpu.v[20] = 2.0f;
    vfpu::vmscl(&f.cpu, 0xF2000000 | kQuad | (20u << 16) | (vreg(1, 0, 0) << 8) | vreg(4, 0, 0));
    CHECK_EQ(elem(f.cpu, 4, 2, 2), 8.0f);
}

TEST(vfpu_rotation_and_memory) {
    Fixture f;
    f.cpu.v[0] = 1.0f; // a quarter turn
    // vrot: cosine in lane 0, sine in lane 1.
    vfpu::vrot(&f.cpu, 0xF3A00000 | kPair | ((1u << 2 | 0u) << 16) | (vreg(0, 0, 0) << 8) | vreg(0, 1, 0));
    CHECK_EQ(elem(f.cpu, 0, 1, 0), 0.0f);
    CHECK_EQ(elem(f.cpu, 0, 1, 1), 1.0f);
    CHECK_EQ(vfpu::quarter_sin(2.0f), 0.0f);
    CHECK_EQ(vfpu::quarter_cos(2.0f), -1.0f);
    CHECK_EQ(vfpu::quarter_sin(-1.0f), -1.0f);
    CHECK(std::fabs(vfpu::quarter_sin(0.5f) - 0.70710678f) < 1e-6f);

    set_quad(f.cpu, 5, 2, 1.5f, 2.5f, 3.5f, 4.5f);
    const u32 reg = vreg(5, 2, 0); // lv/sv encode the register in rt plus the two low bits
    const u32 mem_bits = ((reg & 31) << 16) | (reg >> 5);
    vfpu::sv_q(&f.cpu, f.mem, 0xF8000000 | mem_bits, 16);
    CHECK_EQ(load_le<f32>(f.mem + 16), 1.5f);
    CHECK_EQ(load_le<f32>(f.mem + 28), 4.5f);
    set_quad(f.cpu, 5, 2, 0, 0, 0, 0);
    vfpu::lv_q(&f.cpu, f.mem, 0xD8000000 | mem_bits, 16);
    CHECK_EQ(elem(f.cpu, 5, 2, 2), 3.5f);
    vfpu::sv_s(&f.cpu, f.mem, 0xE8000000 | mem_bits, 0);
    CHECK_EQ(load_le<f32>(f.mem), 1.5f);

    // sv.q with the cache write-back flag (bit 1) must address the same register, in the same order.
    set_quad(f.cpu, 0, 0, 10, 20, 30, 40);
    vfpu::sv_q(&f.cpu, f.mem, 0xF8000002, 32);
    CHECK_EQ(load_le<f32>(f.mem + 32), 10.0f);
    CHECK_EQ(load_le<f32>(f.mem + 36), 20.0f);
    CHECK_EQ(load_le<f32>(f.mem + 40), 30.0f);
    CHECK_EQ(load_le<f32>(f.mem + 44), 40.0f);
}
