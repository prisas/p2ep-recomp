#include "lib/allegrex/insn.h"
#include "tests/test.h"

#include <string>

using namespace p2;
using namespace p2::allegrex;

TEST(decode_core_instructions) {
    CHECK(decode(0x27BDFFE0) == Op::Addiu); // addiu sp, sp, -32
    CHECK(decode(0x03E00008) == Op::Jr);    // jr ra
    CHECK(decode(0x0C000010) == Op::Jal);
    CHECK(decode(0x00000000) == Op::Sll); // nop
    CHECK(decode(0x8FBF001C) == Op::Lw);  // lw ra, 28(sp)
    CHECK(decode(0xAFBF001C) == Op::Sw);
    CHECK(decode(0x00851021) == Op::Addu); // addu v0, a0, a1
    CHECK(decode(0x0085001A) == Op::Div);
    CHECK(decode(0x0000000C) == Op::Syscall);
    CHECK(decode(0x0007000D) == Op::Break);
    CHECK(decode(0x10000003) == Op::Beq);
    CHECK(decode(0x54400003) == Op::Bnel);
    CHECK(decode(0x04110004) == Op::Bgezal);
}

TEST(decode_allegrex_extensions) {
    CHECK(decode(0x7C042C20) == Op::Seb);    // seb a1, a0
    CHECK(decode(0x7C042E20) == Op::Seh);    // seh a1, a0
    CHECK(decode(0x7C0428A0) == Op::Wsbh);   // wsbh a1, a0
    CHECK(decode(0x7C0428E0) == Op::Wsbw);   // wsbw a1, a0
    CHECK(decode(0x7C042D20) == Op::Bitrev); // bitrev a1, a0
    CHECK(decode(0x7C853900) == Op::Ext);    // ext a1, a0, 4, 8
    CHECK(decode(0x7C855904) == Op::Ins);    // ins a1, a0, 4, 8
    CHECK(decode(0x00210802) == Op::Rotr);   // srl encoding with bit 21 set
    CHECK(decode(0x00010802) == Op::Srl);
    CHECK(decode(0x00221846) == Op::Rotrv); // srlv encoding with bit 6 set
    CHECK(decode(0x00221806) == Op::Srlv);
    CHECK(decode(0x0085102C) == Op::Max);
    CHECK(decode(0x0085102D) == Op::Min);
    CHECK(decode(0x00801016) == Op::Clz);
    CHECK(decode(0x0085001C) == Op::Madd);
    CHECK(decode(0x0085002E) == Op::Msub);
    CHECK(decode(0x0085100A) == Op::Movz);
    CHECK(decode(0x0085100B) == Op::Movn);
}

TEST(decode_fpu) {
    CHECK(decode(0x46020002) == Op::MulS); // mul.s f0, f0, f2
    CHECK(decode(0x46000004) == Op::SqrtS);
    CHECK(decode(0x4600000D) == Op::TruncWS);
    CHECK(decode(0x46800020) == Op::CvtSW);
    CHECK(decode(0x46000024) == Op::CvtWS);
    CHECK(decode(0x4602003C) == Op::CCondS); // c.lt.s f0, f2
    CHECK(decode(0x45000002) == Op::Bc1f);
    CHECK(decode(0x45010002) == Op::Bc1t);
    CHECK(decode(0x45030002) == Op::Bc1tl);
    CHECK(decode(0x44840000) == Op::Mtc1);
    CHECK(decode(0x44020000) == Op::Mfc1);
    CHECK(decode(0x44C2F800) == Op::Ctc1);
    CHECK(decode(0xC4800000) == Op::Lwc1);
    CHECK(decode(0xE4800000) == Op::Swc1);
}

TEST(decode_vfpu) {
    CHECK(decode(0xD8800000) == Op::LvQ);
    CHECK(decode(0xF8800000) == Op::SvQ);
    CHECK(decode(0xC8800000) == Op::LvS);
    CHECK(decode(0xE8800000) == Op::SvS);
    CHECK(decode(0xFFFF040D) == Op::Vflush);
    CHECK(decode(0xFFFF0000) == Op::Vnop);
    CHECK(decode(0x60000000) == Op::Vadd);
    CHECK(decode(0x60800000) == Op::Vsub);
    CHECK(decode(0x63800000) == Op::Vdiv);
    CHECK(decode(0x64000000) == Op::Vmul);
    CHECK(decode(0x64800000) == Op::Vdot);
    CHECK(decode(0x65000000) == Op::Vscl);
    CHECK(decode(0xD0000000) == Op::Vmov);
    CHECK(decode(0xD0060000) == Op::Vzero);
    CHECK(decode(0xD0160000) == Op::Vsqrt);
    CHECK(decode(0xD0600000) == Op::Vcst);
    CHECK(decode(0xD2800000) == Op::Vi2f);
    CHECK(decode(0xDC000000) == Op::Vpfxs);
    CHECK(decode(0xDD000000) == Op::Vpfxt);
    CHECK(decode(0xDE000000) == Op::Vpfxd);
    CHECK(decode(0xDF000000) == Op::Viim);
    CHECK(decode(0xDF800000) == Op::Vfim);
    CHECK(decode(0xF0000000) == Op::Vmmul);
    CHECK(decode(0xF3800000) == Op::Vmmov);
    CHECK(decode(0xF3830000) == Op::Vmidt);
    CHECK(decode(0xF3A00000) == Op::Vrot);
    CHECK(decode(0x48E40000) == Op::Mtv);
    CHECK(decode(0x48640080) == Op::Mfvc);
    CHECK(decode(0x49000000) == Op::Bvf);
    CHECK(decode(0x49010000) == Op::Bvt);

    Insn quad(0xD0008080); // vmov.q: both size bits set
    CHECK_EQ(quad.vsize(), 3u);
    Insn single(0xD0000000);
    CHECK_EQ(single.vsize(), 0u);
    Insn load(0xD8830011); // lv.q: register 3 | (1 << 5), offset 16
    CHECK_EQ(load.vmem_reg(), 35u);
    CHECK_EQ(load.vmem_offset(), 16);
}

TEST(decode_rejects_unassigned_encodings) {
    CHECK(decode(0x4C000000) == Op::Invalid); // COP3
    CHECK(decode(0x68000000) == Op::Invalid);
    CHECK(decode(0x0000003F) == Op::Invalid);
    CHECK(decode(0xFFFFFFFF) == Op::Invalid);
    CHECK(decode(0x7C000001) == Op::Invalid);
}

TEST(branch_and_jump_targets) {
    Insn beq(0x10000003);
    CHECK_EQ(beq.branch_target(0x08804000), 0x08804010u);
    Insn back(0x1000FFFF); // branch to itself
    CHECK_EQ(back.branch_target(0x08804000), 0x08804000u);
    Insn jal(0x0E201000);
    CHECK_EQ(jal.jump_target(0x08804000), 0x08804000u);
    CHECK(jal.has_delay_slot());
    CHECK(!Insn(0x27BDFFE0).has_delay_slot());
    CHECK(Insn(0x54400003).is_likely());
    CHECK(!Insn(0x14400003).is_likely());
}

TEST(disassembly_text) {
    CHECK_EQ(disassemble(0, 0x27BDFFE0), std::string("addiu sp, sp, -32"));
    CHECK_EQ(disassemble(0, 0x03E00008), std::string("jr ra"));
    CHECK_EQ(disassemble(0, 0x00000000), std::string("nop"));
    CHECK_EQ(disassemble(0, 0x8FBF001C), std::string("lw ra, 28(sp)"));
    CHECK_EQ(disassemble(0x08804000, 0x10000003), std::string("beq zero, zero, 0x08804010"));
    CHECK_EQ(disassemble(0, 0x7C853900), std::string("ext a1, a0, 4, 8"));
    CHECK_EQ(disassemble(0, 0x4602003C), std::string("c.lt.s f0, f2"));
}
