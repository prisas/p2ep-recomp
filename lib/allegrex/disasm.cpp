#include "lib/allegrex/insn.h"

#include "lib/common/file.h"

namespace p2::allegrex {

const char* gpr_name(u32 index) {
    static const char* const names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                          "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                          "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    return names[index & 31];
}

std::string disassemble(u32 pc, u32 word) {
    Insn i(word);
    const char* n = info(i.op).name;
    const char* rs = gpr_name(i.rs());
    const char* rt = gpr_name(i.rt());
    const char* rd = gpr_name(i.rd());

    if (word == 0) return "nop";

    switch (i.op) {
    case Op::Invalid: return format(".word 0x%08x", word);

    case Op::Sll: case Op::Srl: case Op::Sra: case Op::Rotr:
        return format("%s %s, %s, %u", n, rd, rt, i.sa());
    case Op::Sllv: case Op::Srlv: case Op::Srav: case Op::Rotrv:
        return format("%s %s, %s, %s", n, rd, rt, rs);

    case Op::Jr: return format("jr %s", rs);
    case Op::Jalr: return format("jalr %s, %s", rd, rs);
    case Op::Syscall: return format("syscall 0x%x", (word >> 6) & 0xFFFFF);
    case Op::Break: return format("break 0x%x", (word >> 6) & 0xFFFFF);
    case Op::Sync: return "sync";

    case Op::Mfhi: case Op::Mflo: return format("%s %s", n, rd);
    case Op::Mthi: case Op::Mtlo: return format("%s %s", n, rs);
    case Op::Clz: case Op::Clo: return format("%s %s, %s", n, rd, rs);
    case Op::Mult: case Op::Multu: case Op::Div: case Op::Divu:
    case Op::Madd: case Op::Maddu: case Op::Msub: case Op::Msubu:
        return format("%s %s, %s", n, rs, rt);

    case Op::Movz: case Op::Movn: case Op::Add: case Op::Addu: case Op::Sub: case Op::Subu:
    case Op::And: case Op::Or: case Op::Xor: case Op::Nor: case Op::Slt: case Op::Sltu:
    case Op::Max: case Op::Min:
        return format("%s %s, %s, %s", n, rd, rs, rt);

    case Op::Bltz: case Op::Bgez: case Op::Bltzl: case Op::Bgezl:
    case Op::Bltzal: case Op::Bgezal: case Op::Bltzall: case Op::Bgezall:
    case Op::Blez: case Op::Bgtz: case Op::Blezl: case Op::Bgtzl:
        return format("%s %s, 0x%08x", n, rs, i.branch_target(pc));
    case Op::Beq: case Op::Bne: case Op::Beql: case Op::Bnel:
        return format("%s %s, %s, 0x%08x", n, rs, rt, i.branch_target(pc));
    case Op::J: case Op::Jal: return format("%s 0x%08x", n, i.jump_target(pc));

    case Op::Addi: case Op::Addiu: case Op::Slti: case Op::Sltiu:
        return format("%s %s, %s, %d", n, rt, rs, i.simm());
    case Op::Andi: case Op::Ori: case Op::Xori:
        return format("%s %s, %s, 0x%x", n, rt, rs, i.uimm());
    case Op::Lui: return format("lui %s, 0x%x", rt, i.uimm());

    case Op::Ext: return format("ext %s, %s, %u, %u", rt, rs, i.sa(), i.rd() + 1);
    case Op::Ins: return format("ins %s, %s, %u, %u", rt, rs, i.sa(), i.rd() + 1 - i.sa());
    case Op::Seb: case Op::Seh: case Op::Wsbh: case Op::Wsbw: case Op::Bitrev:
        return format("%s %s, %s", n, rd, rt);
    case Op::Halt: return "halt";
    case Op::Mfic: case Op::Mtic: return format("%s %s", n, rt);

    case Op::Lb: case Op::Lh: case Op::Lwl: case Op::Lw: case Op::Lbu: case Op::Lhu: case Op::Lwr:
    case Op::Sb: case Op::Sh: case Op::Swl: case Op::Sw: case Op::Swr: case Op::Ll: case Op::Sc:
        return format("%s %s, %d(%s)", n, rt, i.simm(), rs);
    case Op::Cache: return format("cache 0x%x, %d(%s)", i.rt(), i.simm(), rs);
    case Op::Cop0: return format("cop0 0x%08x", word);

    case Op::Mfc1: case Op::Mtc1: return format("%s %s, f%u", n, rt, i.fs());
    case Op::Cfc1: case Op::Ctc1: return format("%s %s, $%u", n, rt, i.fs());
    case Op::Bc1f: case Op::Bc1t: case Op::Bc1fl: case Op::Bc1tl:
    case Op::Bvf: case Op::Bvt: case Op::Bvfl: case Op::Bvtl:
        return format("%s 0x%08x", n, i.branch_target(pc));
    case Op::AddS: case Op::SubS: case Op::MulS: case Op::DivS:
        return format("%s f%u, f%u, f%u", n, i.fd(), i.fs(), i.ft());
    case Op::SqrtS: case Op::AbsS: case Op::MovS: case Op::NegS:
    case Op::RoundWS: case Op::TruncWS: case Op::CeilWS: case Op::FloorWS: case Op::CvtWS: case Op::CvtSW:
        return format("%s f%u, f%u", n, i.fd(), i.fs());
    case Op::CCondS: {
        static const char* const cond[16] = {"f",  "un",   "eq",  "ueq", "olt", "ult", "ole", "ule",
                                             "sf", "ngle", "seq", "ngl", "lt",  "nge", "le",  "ngt"};
        return format("c.%s.s f%u, f%u", cond[word & 15], i.fs(), i.ft());
    }
    case Op::Lwc1: case Op::Swc1: return format("%s f%u, %d(%s)", n, i.ft(), i.simm(), rs);

    case Op::LvS: case Op::LvQ: case Op::LvlQ: case Op::LvrQ:
    case Op::SvS: case Op::SvQ: case Op::SvlQ: case Op::SvrQ:
        return format("%s v%03u, %d(%s)", n, i.vmem_reg(), i.vmem_offset(), rs);
    case Op::Mfv: case Op::Mtv: return format("%s %s, v%03u", n, rt, word & 127);
    case Op::Mfvc: case Op::Mtvc: return format("%s %s, vc%u", n, rt, word & 127);

    default:
        if (i.flags() & kVfpu) {
            static const char suffix[4] = {'s', 'p', 't', 'q'};
            return format("%s.%c 0x%08x", n, suffix[i.vsize()], word);
        }
        return format("%s 0x%08x", n, word);
    }
}

} // namespace p2::allegrex
