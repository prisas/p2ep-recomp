#include "lib/allegrex/insn.h"

namespace p2::allegrex {

namespace {

constexpr u16 kB = kBranch, kBL = kBranch | kLikely, kV = kVfpu, kF = kFpu;

// Order must match enum Op exactly.
constexpr OpInfo kInfo[] = {
    {"invalid", 0},

    {"sll", 0}, {"srl", 0}, {"sra", 0}, {"sllv", 0}, {"srlv", 0}, {"srav", 0}, {"rotr", 0}, {"rotrv", 0},
    {"jr", kJumpReg}, {"jalr", kJumpReg | kLink}, {"movz", 0}, {"movn", 0}, {"syscall", 0}, {"break", 0}, {"sync", 0},
    {"mfhi", 0}, {"mthi", 0}, {"mflo", 0}, {"mtlo", 0}, {"clz", 0}, {"clo", 0},
    {"mult", 0}, {"multu", 0}, {"div", 0}, {"divu", 0}, {"madd", 0}, {"maddu", 0}, {"msub", 0}, {"msubu", 0},
    {"add", 0}, {"addu", 0}, {"sub", 0}, {"subu", 0}, {"and", 0}, {"or", 0}, {"xor", 0}, {"nor", 0},
    {"slt", 0}, {"sltu", 0}, {"max", 0}, {"min", 0},

    {"bltz", kB}, {"bgez", kB}, {"bltzl", kBL}, {"bgezl", kBL},
    {"bltzal", kB | kLink}, {"bgezal", kB | kLink}, {"bltzall", kBL | kLink}, {"bgezall", kBL | kLink},

    {"j", kJump}, {"jal", kJump | kLink}, {"beq", kB}, {"bne", kB}, {"blez", kB}, {"bgtz", kB},
    {"beql", kBL}, {"bnel", kBL}, {"blezl", kBL}, {"bgtzl", kBL},
    {"addi", 0}, {"addiu", 0}, {"slti", 0}, {"sltiu", 0}, {"andi", 0}, {"ori", 0}, {"xori", 0}, {"lui", 0},

    {"halt", 0}, {"mfic", 0}, {"mtic", 0}, {"ext", 0}, {"ins", 0}, {"seb", 0}, {"seh", 0},
    {"wsbh", 0}, {"wsbw", 0}, {"bitrev", 0},

    {"lb", kLoad}, {"lh", kLoad}, {"lwl", kLoad}, {"lw", kLoad}, {"lbu", kLoad}, {"lhu", kLoad}, {"lwr", kLoad},
    {"sb", kStore}, {"sh", kStore}, {"swl", kStore}, {"sw", kStore}, {"swr", kStore},
    {"cache", 0}, {"ll", kLoad}, {"sc", kStore},

    {"cop0", 0},

    {"mfc1", kF}, {"cfc1", kF}, {"mtc1", kF}, {"ctc1", kF},
    {"bc1f", kF | kB}, {"bc1t", kF | kB}, {"bc1fl", kF | kBL}, {"bc1tl", kF | kBL},
    {"add.s", kF}, {"sub.s", kF}, {"mul.s", kF}, {"div.s", kF}, {"sqrt.s", kF}, {"abs.s", kF}, {"mov.s", kF}, {"neg.s", kF},
    {"round.w.s", kF}, {"trunc.w.s", kF}, {"ceil.w.s", kF}, {"floor.w.s", kF}, {"cvt.w.s", kF}, {"cvt.s.w", kF},
    {"c.cond.s", kF},
    {"lwc1", kF | kLoad}, {"swc1", kF | kStore},

    {"mfv", kV}, {"mfvc", kV}, {"mtv", kV}, {"mtvc", kV},
    {"bvf", kV | kB}, {"bvt", kV | kB}, {"bvfl", kV | kBL}, {"bvtl", kV | kBL},
    {"lv.s", kV | kLoad}, {"lv.q", kV | kLoad}, {"lvl.q", kV | kLoad}, {"lvr.q", kV | kLoad},
    {"sv.s", kV | kStore}, {"sv.q", kV | kStore}, {"svl.q", kV | kStore}, {"svr.q", kV | kStore},

    {"vadd", kV}, {"vsub", kV}, {"vsbn", kV}, {"vdiv", kV}, {"vmul", kV}, {"vdot", kV}, {"vscl", kV},
    {"vhdp", kV}, {"vcrs", kV}, {"vdet", kV},
    {"vcmp", kV}, {"vmin", kV}, {"vmax", kV}, {"vscmp", kV}, {"vsge", kV}, {"vslt", kV},

    {"vmov", kV}, {"vabs", kV}, {"vneg", kV}, {"vidt", kV}, {"vsat0", kV}, {"vsat1", kV}, {"vzero", kV}, {"vone", kV},
    {"vrcp", kV}, {"vrsq", kV}, {"vsin", kV}, {"vcos", kV}, {"vexp2", kV}, {"vlog2", kV}, {"vsqrt", kV},
    {"vasin", kV}, {"vnrcp", kV}, {"vnsin", kV}, {"vrexp2", kV},
    {"vrnds", kV}, {"vrndi", kV}, {"vrndf1", kV}, {"vrndf2", kV}, {"vf2h", kV}, {"vh2f", kV}, {"vsbz", kV}, {"vlgb", kV},
    {"vuc2i", kV}, {"vc2i", kV}, {"vus2i", kV}, {"vs2i", kV}, {"vi2uc", kV}, {"vi2c", kV}, {"vi2us", kV}, {"vi2s", kV},
    {"vsrt1", kV}, {"vsrt2", kV}, {"vbfy1", kV}, {"vbfy2", kV}, {"vocp", kV}, {"vsocp", kV}, {"vfad", kV}, {"vavg", kV},
    {"vsrt3", kV}, {"vsrt4", kV}, {"vsgn", kV},
    {"vmfvc", kV}, {"vmtvc", kV}, {"vt4444", kV}, {"vt5551", kV}, {"vt5650", kV},
    {"vcst", kV}, {"vf2in", kV}, {"vf2iz", kV}, {"vf2iu", kV}, {"vf2id", kV}, {"vi2f", kV}, {"vcmov", kV}, {"vwbn", kV},

    {"vpfxs", kV}, {"vpfxt", kV}, {"vpfxd", kV}, {"viim", kV}, {"vfim", kV},

    {"vmmul", kV}, {"vtfm2", kV}, {"vtfm3", kV}, {"vtfm4", kV}, {"vmscl", kV}, {"vcrsp", kV}, {"vqmul", kV},
    {"vmmov", kV}, {"vmidt", kV}, {"vmzero", kV}, {"vmone", kV}, {"vrot", kV},
    {"vnop", kV}, {"vsync", kV}, {"vflush", kV},
};

static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == size_t(Op::Count), "kInfo must cover every Op");

Op decode_special(u32 w) {
    switch (w & 63) {
    case 0: return Op::Sll;
    case 2: return (w >> 21) & 1 ? Op::Rotr : Op::Srl;
    case 3: return Op::Sra;
    case 4: return Op::Sllv;
    case 6: return (w >> 6) & 1 ? Op::Rotrv : Op::Srlv;
    case 7: return Op::Srav;
    case 8: return Op::Jr;
    case 9: return Op::Jalr;
    case 10: return Op::Movz;
    case 11: return Op::Movn;
    case 12: return Op::Syscall;
    case 13: return Op::Break;
    case 15: return Op::Sync;
    case 16: return Op::Mfhi;
    case 17: return Op::Mthi;
    case 18: return Op::Mflo;
    case 19: return Op::Mtlo;
    case 22: return Op::Clz;
    case 23: return Op::Clo;
    case 24: return Op::Mult;
    case 25: return Op::Multu;
    case 26: return Op::Div;
    case 27: return Op::Divu;
    case 28: return Op::Madd;
    case 29: return Op::Maddu;
    case 32: return Op::Add;
    case 33: return Op::Addu;
    case 34: return Op::Sub;
    case 35: return Op::Subu;
    case 36: return Op::And;
    case 37: return Op::Or;
    case 38: return Op::Xor;
    case 39: return Op::Nor;
    case 42: return Op::Slt;
    case 43: return Op::Sltu;
    case 44: return Op::Max;
    case 45: return Op::Min;
    case 46: return Op::Msub;
    case 47: return Op::Msubu;
    default: return Op::Invalid;
    }
}

Op decode_regimm(u32 w) {
    switch ((w >> 16) & 31) {
    case 0: return Op::Bltz;
    case 1: return Op::Bgez;
    case 2: return Op::Bltzl;
    case 3: return Op::Bgezl;
    case 16: return Op::Bltzal;
    case 17: return Op::Bgezal;
    case 18: return Op::Bltzall;
    case 19: return Op::Bgezall;
    default: return Op::Invalid;
    }
}

Op decode_cop1(u32 w) {
    switch ((w >> 21) & 31) {
    case 0: return Op::Mfc1;
    case 2: return Op::Cfc1;
    case 4: return Op::Mtc1;
    case 6: return Op::Ctc1;
    case 8:
        switch ((w >> 16) & 31) {
        case 0: return Op::Bc1f;
        case 1: return Op::Bc1t;
        case 2: return Op::Bc1fl;
        case 3: return Op::Bc1tl;
        default: return Op::Invalid;
        }
    case 16:
        switch (w & 63) {
        case 0: return Op::AddS;
        case 1: return Op::SubS;
        case 2: return Op::MulS;
        case 3: return Op::DivS;
        case 4: return Op::SqrtS;
        case 5: return Op::AbsS;
        case 6: return Op::MovS;
        case 7: return Op::NegS;
        case 12: return Op::RoundWS;
        case 13: return Op::TruncWS;
        case 14: return Op::CeilWS;
        case 15: return Op::FloorWS;
        case 36: return Op::CvtWS;
        default: return (w & 63) >= 48 ? Op::CCondS : Op::Invalid;
        }
    case 20: return (w & 63) == 32 ? Op::CvtSW : Op::Invalid;
    default: return Op::Invalid;
    }
}

Op decode_cop2(u32 w) {
    switch ((w >> 21) & 31) {
    case 3: return (w & 0x80) ? Op::Mfvc : Op::Mfv;
    case 7: return (w & 0x80) ? Op::Mtvc : Op::Mtv;
    case 8:
        switch ((w >> 16) & 3) {
        case 0: return Op::Bvf;
        case 1: return Op::Bvt;
        case 2: return Op::Bvfl;
        default: return Op::Bvtl;
        }
    default: return Op::Invalid;
    }
}

Op decode_vfpu4(u32 w) {
    static constexpr Op table4[32] = {
        Op::Vmov,  Op::Vabs,    Op::Vneg,  Op::Vidt,    Op::Vsat0,  Op::Vsat1,   Op::Vzero,   Op::Vone,
        Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid,
        Op::Vrcp,  Op::Vrsq,    Op::Vsin,  Op::Vcos,    Op::Vexp2,  Op::Vlog2,   Op::Vsqrt,   Op::Vasin,
        Op::Vnrcp, Op::Invalid, Op::Vnsin, Op::Invalid, Op::Vrexp2, Op::Invalid, Op::Invalid, Op::Invalid};
    static constexpr Op table7[32] = {
        Op::Vrnds,   Op::Vrndi,   Op::Vrndf1,  Op::Vrndf2,  Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid,
        Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid,
        Op::Invalid, Op::Invalid, Op::Vf2h,    Op::Vh2f,    Op::Invalid, Op::Invalid, Op::Vsbz,    Op::Vlgb,
        Op::Vuc2i,   Op::Vc2i,    Op::Vus2i,   Op::Vs2i,    Op::Vi2uc,   Op::Vi2c,    Op::Vi2us,   Op::Vi2s};
    static constexpr Op table9[32] = {
        Op::Vsrt1,   Op::Vsrt2,   Op::Vbfy1,   Op::Vbfy2,   Op::Vocp,    Op::Vsocp,   Op::Vfad,    Op::Vavg,
        Op::Vsrt3,   Op::Vsrt4,   Op::Vsgn,    Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid,
        Op::Vmfvc,   Op::Vmtvc,   Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid,
        Op::Invalid, Op::Vt4444,  Op::Vt5551,  Op::Vt5650,  Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid};

    u32 group = (w >> 21) & 31, sub = (w >> 16) & 31;
    switch (group) {
    case 0: return table4[sub];
    case 1: return table7[sub];
    case 2: return table9[sub];
    case 3: return Op::Vcst;
    case 16: return Op::Vf2in;
    case 17: return Op::Vf2iz;
    case 18: return Op::Vf2iu;
    case 19: return Op::Vf2id;
    case 20: return Op::Vi2f;
    case 21: return Op::Vcmov;
    default: return group >= 24 ? Op::Vwbn : Op::Invalid;
    }
}

Op decode_vfpu6(u32 w) {
    u32 group = (w >> 21) & 31;
    if (group < 4) return Op::Vmmul;
    if (group < 8) return Op::Vtfm2;
    if (group < 12) return Op::Vtfm3;
    if (group < 16) return Op::Vtfm4;
    if (group < 20) return Op::Vmscl;
    if (group < 24) return (((w >> 7) & 1) | (((w >> 15) & 1) << 1)) == 2 ? Op::Vcrsp : Op::Vqmul;
    if (group == 28) {
        switch ((w >> 16) & 15) {
        case 0: return Op::Vmmov;
        case 3: return Op::Vmidt;
        case 6: return Op::Vmzero;
        case 7: return Op::Vmone;
        default: return Op::Invalid;
        }
    }
    if (group == 29) return Op::Vrot;
    return Op::Invalid;
}

} // namespace

const OpInfo& info(Op op) { return kInfo[size_t(op) < size_t(Op::Count) ? size_t(op) : 0]; }

Op decode(u32 w) {
    switch (w >> 26) {
    case 0x00: return decode_special(w);
    case 0x01: return decode_regimm(w);
    case 0x02: return Op::J;
    case 0x03: return Op::Jal;
    case 0x04: return Op::Beq;
    case 0x05: return Op::Bne;
    case 0x06: return Op::Blez;
    case 0x07: return Op::Bgtz;
    case 0x08: return Op::Addi;
    case 0x09: return Op::Addiu;
    case 0x0A: return Op::Slti;
    case 0x0B: return Op::Sltiu;
    case 0x0C: return Op::Andi;
    case 0x0D: return Op::Ori;
    case 0x0E: return Op::Xori;
    case 0x0F: return Op::Lui;
    case 0x10: return Op::Cop0;
    case 0x11: return decode_cop1(w);
    case 0x12: return decode_cop2(w);
    case 0x14: return Op::Beql;
    case 0x15: return Op::Bnel;
    case 0x16: return Op::Blezl;
    case 0x17: return Op::Bgtzl;
    case 0x18:
        switch ((w >> 23) & 7) {
        case 0: return Op::Vadd;
        case 1: return Op::Vsub;
        case 2: return Op::Vsbn;
        case 7: return Op::Vdiv;
        default: return Op::Invalid;
        }
    case 0x19:
        switch ((w >> 23) & 7) {
        case 0: return Op::Vmul;
        case 1: return Op::Vdot;
        case 2: return Op::Vscl;
        case 4: return Op::Vhdp;
        case 5: return Op::Vcrs;
        case 6: return Op::Vdet;
        default: return Op::Invalid;
        }
    case 0x1B:
        switch ((w >> 23) & 7) {
        case 0: return Op::Vcmp;
        case 2: return Op::Vmin;
        case 3: return Op::Vmax;
        case 5: return Op::Vscmp;
        case 6: return Op::Vsge;
        case 7: return Op::Vslt;
        default: return Op::Invalid;
        }
    case 0x1C:
        switch (w & 63) {
        case 0x00: return Op::Halt;
        case 0x24: return Op::Mfic;
        case 0x26: return Op::Mtic;
        default: return Op::Invalid;
        }
    case 0x1F:
        switch (w & 63) {
        case 0x00: return Op::Ext;
        case 0x04: return Op::Ins;
        case 0x20:
            switch ((w >> 6) & 31) {
            case 2: return Op::Wsbh;
            case 3: return Op::Wsbw;
            case 16: return Op::Seb;
            case 20: return Op::Bitrev;
            case 24: return Op::Seh;
            default: return Op::Invalid;
            }
        default: return Op::Invalid;
        }
    case 0x20: return Op::Lb;
    case 0x21: return Op::Lh;
    case 0x22: return Op::Lwl;
    case 0x23: return Op::Lw;
    case 0x24: return Op::Lbu;
    case 0x25: return Op::Lhu;
    case 0x26: return Op::Lwr;
    case 0x28: return Op::Sb;
    case 0x29: return Op::Sh;
    case 0x2A: return Op::Swl;
    case 0x2B: return Op::Sw;
    case 0x2E: return Op::Swr;
    case 0x2F: return Op::Cache;
    case 0x30: return Op::Ll;
    case 0x31: return Op::Lwc1;
    case 0x32: return Op::LvS;
    case 0x34: return decode_vfpu4(w);
    case 0x35: return (w & 2) ? Op::LvrQ : Op::LvlQ;
    case 0x36: return Op::LvQ;
    case 0x37:
        switch ((w >> 23) & 7) {
        case 0:
        case 1: return Op::Vpfxs;
        case 2:
        case 3: return Op::Vpfxt;
        case 4:
        case 5: return Op::Vpfxd;
        case 6: return Op::Viim;
        default: return Op::Vfim;
        }
    case 0x38: return Op::Sc;
    case 0x39: return Op::Swc1;
    case 0x3A: return Op::SvS;
    case 0x3C: return decode_vfpu6(w);
    case 0x3D: return (w & 2) ? Op::SvrQ : Op::SvlQ;
    case 0x3E: return Op::SvQ;
    case 0x3F:
        switch (w) {
        case 0xFFFF0000u: return Op::Vnop;
        case 0xFFFF0320u: return Op::Vsync;
        case 0xFFFF040Du: return Op::Vflush;
        default: return Op::Invalid;
        }
    default: return Op::Invalid;
    }
}

} // namespace p2::allegrex
