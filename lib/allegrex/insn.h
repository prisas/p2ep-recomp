#pragma once

#include "lib/common/types.h"

#include <string>

namespace p2::allegrex {

// Every instruction the Allegrex CPU core, its FPU and its vector unit (VFPU) can execute.
enum class Op : u16 {
    Invalid = 0,

    // SPECIAL
    Sll, Srl, Sra, Sllv, Srlv, Srav, Rotr, Rotrv,
    Jr, Jalr, Movz, Movn, Syscall, Break, Sync,
    Mfhi, Mthi, Mflo, Mtlo, Clz, Clo,
    Mult, Multu, Div, Divu, Madd, Maddu, Msub, Msubu,
    Add, Addu, Sub, Subu, And, Or, Xor, Nor, Slt, Sltu, Max, Min,

    // REGIMM
    Bltz, Bgez, Bltzl, Bgezl, Bltzal, Bgezal, Bltzall, Bgezall,

    // jumps, branches, immediates
    J, Jal, Beq, Bne, Blez, Bgtz, Beql, Bnel, Blezl, Bgtzl,
    Addi, Addiu, Slti, Sltiu, Andi, Ori, Xori, Lui,

    // SPECIAL2 / SPECIAL3
    Halt, Mfic, Mtic, Ext, Ins, Seb, Seh, Wsbh, Wsbw, Bitrev,

    // memory
    Lb, Lh, Lwl, Lw, Lbu, Lhu, Lwr, Sb, Sh, Swl, Sw, Swr, Cache, Ll, Sc,

    // COP0 (kernel only; never expected in game code)
    Cop0,

    // FPU
    Mfc1, Cfc1, Mtc1, Ctc1, Bc1f, Bc1t, Bc1fl, Bc1tl,
    AddS, SubS, MulS, DivS, SqrtS, AbsS, MovS, NegS,
    RoundWS, TruncWS, CeilWS, FloorWS, CvtWS, CvtSW, CCondS,
    Lwc1, Swc1,

    // VFPU: moves and branches
    Mfv, Mfvc, Mtv, Mtvc, Bvf, Bvt, Bvfl, Bvtl,
    LvS, LvQ, LvlQ, LvrQ, SvS, SvQ, SvlQ, SvrQ,
    // VFPU: three-operand arithmetic
    Vadd, Vsub, Vsbn, Vdiv, Vmul, Vdot, Vscl, Vhdp, Vcrs, Vdet,
    Vcmp, Vmin, Vmax, Vscmp, Vsge, Vslt,
    // VFPU: two-operand
    Vmov, Vabs, Vneg, Vidt, Vsat0, Vsat1, Vzero, Vone,
    Vrcp, Vrsq, Vsin, Vcos, Vexp2, Vlog2, Vsqrt, Vasin, Vnrcp, Vnsin, Vrexp2,
    Vrnds, Vrndi, Vrndf1, Vrndf2, Vf2h, Vh2f, Vsbz, Vlgb,
    Vuc2i, Vc2i, Vus2i, Vs2i, Vi2uc, Vi2c, Vi2us, Vi2s,
    Vsrt1, Vsrt2, Vbfy1, Vbfy2, Vocp, Vsocp, Vfad, Vavg, Vsrt3, Vsrt4, Vsgn,
    Vmfvc, Vmtvc, Vt4444, Vt5551, Vt5650,
    Vcst, Vf2in, Vf2iz, Vf2iu, Vf2id, Vi2f, Vcmov, Vwbn,
    // VFPU: prefixes and immediates
    Vpfxs, Vpfxt, Vpfxd, Viim, Vfim,
    // VFPU: matrices
    Vmmul, Vtfm2, Vtfm3, Vtfm4, Vmscl, Vcrsp, Vqmul, Vmmov, Vmidt, Vmzero, Vmone, Vrot,
    Vnop, Vsync, Vflush,

    Count
};

enum : u16 {
    kBranch = 1 << 0,  // conditional, pc-relative, has a delay slot
    kLikely = 1 << 1,  // delay slot is skipped when the branch is not taken
    kLink = 1 << 2,    // writes a return address
    kJump = 1 << 3,    // J / JAL
    kJumpReg = 1 << 4, // JR / JALR
    kFpu = 1 << 5,
    kVfpu = 1 << 6,
    kLoad = 1 << 7,
    kStore = 1 << 8,
};

struct OpInfo {
    const char* name;
    u16 flags;
};

const OpInfo& info(Op op);
Op decode(u32 word);

struct Insn {
    u32 word = 0;
    Op op = Op::Invalid;

    Insn() = default;
    explicit Insn(u32 w) : word(w), op(decode(w)) {}

    u32 rs() const { return (word >> 21) & 31; }
    u32 rt() const { return (word >> 16) & 31; }
    u32 rd() const { return (word >> 11) & 31; }
    u32 sa() const { return (word >> 6) & 31; }
    u32 fs() const { return (word >> 11) & 31; }
    u32 ft() const { return (word >> 16) & 31; }
    u32 fd() const { return (word >> 6) & 31; }
    u32 uimm() const { return word & 0xFFFF; }
    s32 simm() const { return s32(s16(word & 0xFFFF)); }
    u32 funct() const { return word & 63; }

    u16 flags() const { return info(op).flags; }
    bool has_delay_slot() const { return (flags() & (kBranch | kJump | kJumpReg)) != 0; }
    bool is_branch() const { return (flags() & kBranch) != 0; }
    bool is_likely() const { return (flags() & kLikely) != 0; }

    u32 branch_target(u32 pc) const { return pc + 4 + (u32(simm()) << 2); }
    u32 jump_target(u32 pc) const { return ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2); }

    // VFPU operand fields
    u32 vd() const { return word & 127; }
    u32 vs() const { return (word >> 8) & 127; }
    u32 vt() const { return (word >> 16) & 127; }
    // 0 = single, 1 = pair, 2 = triple, 3 = quad
    u32 vsize() const { return ((word >> 7) & 1) | (((word >> 15) & 1) << 1); }
    // Register operand of lv/sv: five bits in rt plus two low bits of the word.
    u32 vmem_reg() const { return ((word >> 16) & 31) | ((word & 3) << 5); }
    s32 vmem_offset() const { return s32(s16(word & 0xFFFC)); }
};

const char* gpr_name(u32 index);
std::string disassemble(u32 pc, u32 word);

} // namespace p2::allegrex
