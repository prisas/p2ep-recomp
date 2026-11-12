#include "tools/recomp/emit.h"

#include "lib/allegrex/insn.h"
#include "lib/common/file.h"

#include <algorithm>
#include <unordered_set>

namespace p2::recomp {

using allegrex::Insn;
using allegrex::Op;

std::string function_name(u32 addr) { return format("F_%08X", addr); }

std::string import_thunk_name(const ImportStub& stub) {
    return stub.name.empty() ? format("I_%s_%08X", stub.lib.c_str(), stub.nid) : "I_" + stub.name;
}

std::string import_hle_name(const ImportStub& stub) {
    return stub.name.empty() ? format("hle_%s_%08X", stub.lib.c_str(), stub.nid) : "hle_" + stub.name;
}

namespace {

bool always_taken(const Insn& i) {
    switch (i.op) {
    case Op::Beq:
    case Op::Beql: return i.rs() == i.rt();
    case Op::Bgez:
    case Op::Bgezl:
    case Op::Blez:
    case Op::Blezl: return i.rs() == 0;
    default: return false;
    }
}

// Name of the vfpu:: helper implementing an instruction, or nullptr when there is none.
const char* vfpu_helper(Op op) {
    switch (op) {
    case Op::Vadd: return "vadd";
    case Op::Vsub: return "vsub";
    case Op::Vmul: return "vmul";
    case Op::Vdiv: return "vdiv";
    case Op::Vmin: return "vmin";
    case Op::Vmax: return "vmax";
    case Op::Vdot: return "vdot";
    case Op::Vscl: return "vscl";
    case Op::Vcrsp: return "vcrsp";
    case Op::Vqmul: return "vqmul";
    case Op::Vmov: return "vmov";
    case Op::Vabs: return "vabs";
    case Op::Vneg: return "vneg";
    case Op::Vsat0: return "vsat0";
    case Op::Vsat1: return "vsat1";
    case Op::Vrcp: return "vrcp";
    case Op::Vrsq: return "vrsq";
    case Op::Vsin: return "vsin";
    case Op::Vcos: return "vcos";
    case Op::Vexp2: return "vexp2";
    case Op::Vlog2: return "vlog2";
    case Op::Vsqrt: return "vsqrt";
    case Op::Vasin: return "vasin";
    case Op::Vnrcp: return "vnrcp";
    case Op::Vnsin: return "vnsin";
    case Op::Vrexp2: return "vrexp2";
    case Op::Vsgn: return "vsgn";
    case Op::Vocp: return "vocp";
    case Op::Vsocp: return "vsocp";
    case Op::Vfad: return "vfad";
    case Op::Vavg: return "vavg";
    case Op::Vzero: return "vzero";
    case Op::Vone: return "vone";
    case Op::Vcst: return "vcst";
    case Op::Vidt: return "vidt";
    case Op::Viim: return "viim";
    case Op::Vfim: return "vfim";
    case Op::Vi2f: return "vi2f";
    case Op::Vf2in:
    case Op::Vf2iz:
    case Op::Vf2iu:
    case Op::Vf2id: return "vf2i";
    case Op::Vuc2i:
    case Op::Vc2i:
    case Op::Vus2i:
    case Op::Vs2i: return "vx2i";
    case Op::Vi2uc:
    case Op::Vi2c:
    case Op::Vi2us:
    case Op::Vi2s: return "vi2x";
    case Op::Vcmp: return "vcmp";
    case Op::Vcmov: return "vcmov";
    case Op::Vrot: return "vrot";
    case Op::Vmmov: return "vmmov";
    case Op::Vmidt: return "vmidt";
    case Op::Vmzero: return "vmzero";
    case Op::Vmone: return "vmone";
    case Op::Vmscl: return "vmscl";
    case Op::Vmmul: return "vmmul";
    case Op::Vtfm2:
    case Op::Vtfm3:
    case Op::Vtfm4: return "vtfm";
    case Op::Vpfxs:
    case Op::Vpfxt:
    case Op::Vpfxd: return "vpfx";
    default: return nullptr;
    }
}

class Emitter {
public:
    Emitter(const Program& prog, const EmitOptions& opt, EmitResult& result)
        : prog_(prog), in_(*prog.input), opt_(opt), result_(result) {}

    void function(const Function& fn, std::string& out) {
        out_ = &out;
        fn_ = &fn;
        block_starts_.clear();
        for (const Block& b : fn.blocks) block_starts_.insert(b.start);

        const std::string name = function_name(fn.entry);
        append_format(out, "P2_FN(%s) {\n", name.c_str());
        if (!fn.secondary.empty()) {
            // Entered through one of the thunks below: resume at the requested label. This must
            // come before anything that can yield, so the request cannot leak into other code.
            raw(1, "if (P2_UNLIKELY(c->sec_entry)) {");
            raw(2, "const u32 entry_ = c->sec_entry;");
            raw(2, "c->sec_entry = 0;");
            raw(2, "switch (entry_) {");
            for (size_t k = 0; k < fn.secondary.size(); ++k)
                raw(2, format("case %zu: goto L_%08X;", k + 1, fn.secondary[k]));
            raw(2, "default: break;");
            raw(2, "}");
            raw(1, "}");
        }
        if (fn.blocks.empty() || fn.blocks.front().start != fn.entry) {
            // Entry was not decodable at all.
            append_format(out, "    ::p2::rt_trap(c, m, 0x%08Xu, 0u);\n", fn.entry);
        }
        for (size_t k = 0; k < fn.blocks.size(); ++k)
            block(fn.blocks[k], k + 1 < fn.blocks.size() ? &fn.blocks[k + 1] : nullptr);
        out += "}\n";
        for (size_t k = 0; k < fn.secondary.size(); ++k)
            append_format(out, "P2_FN(%s) {\n    c->sec_entry = %zu;\n    P2_TAIL(%s);\n}\n",
                          function_name(fn.secondary[k]).c_str(), k + 1, name.c_str());
        out += "\n";
    }

private:
    // --- small text helpers -------------------------------------------------------------------------------

    static std::string R(u32 n) { return n ? format("c->r[%u]", n) : std::string("0u"); }

    static std::string mem_addr(const Insn& i, s32 offset) {
        if (i.rs() == 0) return format("0x%08Xu", u32(offset));
        if (offset == 0) return R(i.rs());
        return format("%s + 0x%Xu", R(i.rs()).c_str(), u32(offset));
    }

    void line(int indent, u32 pc, u32 word, const std::string& text) {
        std::string& out = *out_;
        out.append(size_t(indent) * 4, ' ');
        out += text;
        if (opt_.comments && pc) {
            if (text.size() < 58) out.append(58 - text.size(), ' ');
            append_format(out, " // %08X  %s", pc, allegrex::disassemble(pc, word).c_str());
        }
        out += '\n';
    }

    void raw(int indent, const std::string& text) {
        out_->append(size_t(indent) * 4, ' ');
        *out_ += text;
        *out_ += '\n';
    }

    // Assignment to a general register; writes to $zero vanish.
    void set(int indent, u32 pc, u32 word, u32 reg, const std::string& expr) {
        line(indent, pc, word, reg ? format("c->r[%u] = %s;", reg, expr.c_str()) : std::string(";"));
    }

    void unimplemented(int indent, u32 pc, u32 word) {
        result_.unimplemented.push_back(format("%08X  %08X  %s", pc, word, allegrex::disassemble(pc, word).c_str()));
        line(indent, pc, word, format("::p2::rt_trap(c, m, 0x%08Xu, 0x%08Xu);", pc, word));
    }

    // --- straight-line instructions -----------------------------------------------------------------------

    void insn(int indent, u32 pc, const Insn& i) {
        const u32 w = i.word;
        const std::string rs = R(i.rs()), rt = R(i.rt());
        const u32 rd = i.rd();
        auto F = [](u32 n) { return format("c->f[%u]", n); };
        auto FI = [](u32 n) { return format("c->fi[%u]", n); };
        auto stmt = [&](const std::string& text) { line(indent, pc, w, text); };

        switch (i.op) {
        case Op::Sll: set(indent, pc, w, rd, format("%s << %u", rt.c_str(), i.sa())); break;
        case Op::Srl: set(indent, pc, w, rd, format("%s >> %u", rt.c_str(), i.sa())); break;
        case Op::Sra: set(indent, pc, w, rd, format("u32(s32(%s) >> %u)", rt.c_str(), i.sa())); break;
        case Op::Rotr: set(indent, pc, w, rd, format("::p2::op_rotr(%s, %u)", rt.c_str(), i.sa())); break;
        case Op::Sllv: set(indent, pc, w, rd, format("%s << (%s & 31)", rt.c_str(), rs.c_str())); break;
        case Op::Srlv: set(indent, pc, w, rd, format("%s >> (%s & 31)", rt.c_str(), rs.c_str())); break;
        case Op::Srav: set(indent, pc, w, rd, format("u32(s32(%s) >> (%s & 31))", rt.c_str(), rs.c_str())); break;
        case Op::Rotrv: set(indent, pc, w, rd, format("::p2::op_rotr(%s, %s)", rt.c_str(), rs.c_str())); break;

        case Op::Movz:
            stmt(rd ? format("if (%s == 0) c->r[%u] = %s;", rt.c_str(), rd, rs.c_str()) : ";");
            break;
        case Op::Movn:
            stmt(rd ? format("if (%s != 0) c->r[%u] = %s;", rt.c_str(), rd, rs.c_str()) : ";");
            break;

        case Op::Mfhi: set(indent, pc, w, rd, "c->hi"); break;
        case Op::Mflo: set(indent, pc, w, rd, "c->lo"); break;
        case Op::Mthi: stmt(format("c->hi = %s;", rs.c_str())); break;
        case Op::Mtlo: stmt(format("c->lo = %s;", rs.c_str())); break;
        case Op::Clz: set(indent, pc, w, rd, format("::p2::op_clz(%s)", rs.c_str())); break;
        case Op::Clo: set(indent, pc, w, rd, format("::p2::op_clo(%s)", rs.c_str())); break;

        case Op::Mult: stmt(format("::p2::op_mult(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Multu: stmt(format("::p2::op_multu(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Div: stmt(format("::p2::op_div(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Divu: stmt(format("::p2::op_divu(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Madd: stmt(format("::p2::op_madd(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Maddu: stmt(format("::p2::op_maddu(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Msub: stmt(format("::p2::op_msub(c, %s, %s);", rs.c_str(), rt.c_str())); break;
        case Op::Msubu: stmt(format("::p2::op_msubu(c, %s, %s);", rs.c_str(), rt.c_str())); break;

        case Op::Add:
        case Op::Addu: set(indent, pc, w, rd, format("%s + %s", rs.c_str(), rt.c_str())); break;
        case Op::Sub:
        case Op::Subu: set(indent, pc, w, rd, format("%s - %s", rs.c_str(), rt.c_str())); break;
        case Op::And: set(indent, pc, w, rd, format("%s & %s", rs.c_str(), rt.c_str())); break;
        case Op::Or: set(indent, pc, w, rd, format("%s | %s", rs.c_str(), rt.c_str())); break;
        case Op::Xor: set(indent, pc, w, rd, format("%s ^ %s", rs.c_str(), rt.c_str())); break;
        case Op::Nor: set(indent, pc, w, rd, format("~(%s | %s)", rs.c_str(), rt.c_str())); break;
        case Op::Slt: set(indent, pc, w, rd, format("u32(s32(%s) < s32(%s))", rs.c_str(), rt.c_str())); break;
        case Op::Sltu: set(indent, pc, w, rd, format("u32(%s < %s)", rs.c_str(), rt.c_str())); break;
        case Op::Max: set(indent, pc, w, rd, format("::p2::op_max(%s, %s)", rs.c_str(), rt.c_str())); break;
        case Op::Min: set(indent, pc, w, rd, format("::p2::op_min(%s, %s)", rs.c_str(), rt.c_str())); break;

        case Op::Addi:
        case Op::Addiu:
            if (i.rs() == 0) set(indent, pc, w, i.rt(), format("0x%08Xu", u32(i.simm())));
            else set(indent, pc, w, i.rt(), format("%s + 0x%Xu", rs.c_str(), u32(i.simm())));
            break;
        case Op::Slti: set(indent, pc, w, i.rt(), format("u32(s32(%s) < %d)", rs.c_str(), i.simm())); break;
        case Op::Sltiu: set(indent, pc, w, i.rt(), format("u32(%s < 0x%Xu)", rs.c_str(), u32(i.simm()))); break;
        case Op::Andi: set(indent, pc, w, i.rt(), format("%s & 0x%Xu", rs.c_str(), i.uimm())); break;
        case Op::Ori: set(indent, pc, w, i.rt(), format("%s | 0x%Xu", rs.c_str(), i.uimm())); break;
        case Op::Xori: set(indent, pc, w, i.rt(), format("%s ^ 0x%Xu", rs.c_str(), i.uimm())); break;
        case Op::Lui: set(indent, pc, w, i.rt(), format("0x%08Xu", i.uimm() << 16)); break;

        case Op::Ext:
            set(indent, pc, w, i.rt(), format("::p2::op_ext(%s, %u, %u)", rs.c_str(), i.sa(), rd + 1));
            break;
        case Op::Ins:
            if (rd < i.sa()) {
                unimplemented(indent, pc, w);
            } else {
                set(indent, pc, w, i.rt(),
                    format("::p2::op_ins(%s, %s, %u, %u)", rt.c_str(), rs.c_str(), i.sa(), rd - i.sa() + 1));
            }
            break;
        case Op::Seb: set(indent, pc, w, rd, format("::p2::op_seb(%s)", rt.c_str())); break;
        case Op::Seh: set(indent, pc, w, rd, format("::p2::op_seh(%s)", rt.c_str())); break;
        case Op::Wsbh: set(indent, pc, w, rd, format("::p2::op_wsbh(%s)", rt.c_str())); break;
        case Op::Wsbw: set(indent, pc, w, rd, format("::p2::op_wsbw(%s)", rt.c_str())); break;
        case Op::Bitrev: set(indent, pc, w, rd, format("::p2::op_bitrev(%s)", rt.c_str())); break;

        case Op::Lb: set(indent, pc, w, i.rt(), format("u32(s32(s8(::p2::rd8(m, %s))))", mem_addr(i, i.simm()).c_str())); break;
        case Op::Lbu: set(indent, pc, w, i.rt(), format("::p2::rd8(m, %s)", mem_addr(i, i.simm()).c_str())); break;
        case Op::Lh: set(indent, pc, w, i.rt(), format("u32(s32(s16(::p2::rd16(m, %s))))", mem_addr(i, i.simm()).c_str())); break;
        case Op::Lhu: set(indent, pc, w, i.rt(), format("::p2::rd16(m, %s)", mem_addr(i, i.simm()).c_str())); break;
        case Op::Lw:
        case Op::Ll: set(indent, pc, w, i.rt(), format("::p2::rd32(m, %s)", mem_addr(i, i.simm()).c_str())); break;
        case Op::Lwl:
            set(indent, pc, w, i.rt(), format("::p2::op_lwl(m, %s, %s)", mem_addr(i, i.simm()).c_str(), rt.c_str()));
            break;
        case Op::Lwr:
            set(indent, pc, w, i.rt(), format("::p2::op_lwr(m, %s, %s)", mem_addr(i, i.simm()).c_str(), rt.c_str()));
            break;
        case Op::Sb: stmt(format("::p2::wr8(m, %s, u8(%s));", mem_addr(i, i.simm()).c_str(), rt.c_str())); break;
        case Op::Sh: stmt(format("::p2::wr16(m, %s, u16(%s));", mem_addr(i, i.simm()).c_str(), rt.c_str())); break;
        case Op::Sw: stmt(format("::p2::wr32(m, %s, %s);", mem_addr(i, i.simm()).c_str(), rt.c_str())); break;
        case Op::Swl: stmt(format("::p2::op_swl(m, %s, %s);", mem_addr(i, i.simm()).c_str(), rt.c_str())); break;
        case Op::Swr: stmt(format("::p2::op_swr(m, %s, %s);", mem_addr(i, i.simm()).c_str(), rt.c_str())); break;
        case Op::Sc:
            stmt(format("::p2::wr32(m, %s, %s);%s", mem_addr(i, i.simm()).c_str(), rt.c_str(),
                        i.rt() ? format(" c->r[%u] = 1;", i.rt()).c_str() : ""));
            break;
        case Op::Cache:
        case Op::Sync: stmt(";"); break;

        case Op::Break: stmt(format("::p2::rt_break(c, m, 0x%08Xu, 0x%Xu);", pc, (w >> 6) & 0xFFFFF)); break;
        case Op::Syscall: stmt(format("::p2::rt_syscall(c, m, 0x%08Xu, 0x%Xu);", pc, (w >> 6) & 0xFFFFF)); break;

        // --- FPU ---
        case Op::Mfc1: set(indent, pc, w, i.rt(), FI(i.fs())); break;
        case Op::Mtc1: stmt(format("%s = %s;", FI(i.fs()).c_str(), rt.c_str())); break;
        case Op::Cfc1:
            if (i.fs() == 31) set(indent, pc, w, i.rt(), "::p2::fpu_get_fcr31(c)");
            else set(indent, pc, w, i.rt(), i.fs() == 0 ? "0x00003351u" : "0u");
            break;
        case Op::Ctc1:
            stmt(i.fs() == 31 ? format("::p2::fpu_set_fcr31(c, %s);", rt.c_str()) : std::string(";"));
            break;
        case Op::AddS: stmt(format("%s = %s + %s;", F(i.fd()).c_str(), F(i.fs()).c_str(), F(i.ft()).c_str())); break;
        case Op::SubS: stmt(format("%s = %s - %s;", F(i.fd()).c_str(), F(i.fs()).c_str(), F(i.ft()).c_str())); break;
        case Op::MulS: stmt(format("%s = %s * %s;", F(i.fd()).c_str(), F(i.fs()).c_str(), F(i.ft()).c_str())); break;
        case Op::DivS: stmt(format("%s = %s / %s;", F(i.fd()).c_str(), F(i.fs()).c_str(), F(i.ft()).c_str())); break;
        case Op::SqrtS: stmt(format("%s = std::sqrt(%s);", F(i.fd()).c_str(), F(i.fs()).c_str())); break;
        case Op::AbsS: stmt(format("%s = %s & 0x7FFFFFFFu;", FI(i.fd()).c_str(), FI(i.fs()).c_str())); break;
        case Op::MovS: stmt(format("%s = %s;", FI(i.fd()).c_str(), FI(i.fs()).c_str())); break;
        case Op::NegS: stmt(format("%s = %s ^ 0x80000000u;", FI(i.fd()).c_str(), FI(i.fs()).c_str())); break;
        case Op::RoundWS: stmt(format("%s = ::p2::fpu_round_w(%s);", FI(i.fd()).c_str(), F(i.fs()).c_str())); break;
        case Op::TruncWS: stmt(format("%s = ::p2::fpu_trunc_w(%s);", FI(i.fd()).c_str(), F(i.fs()).c_str())); break;
        case Op::CeilWS: stmt(format("%s = ::p2::fpu_ceil_w(%s);", FI(i.fd()).c_str(), F(i.fs()).c_str())); break;
        case Op::FloorWS: stmt(format("%s = ::p2::fpu_floor_w(%s);", FI(i.fd()).c_str(), F(i.fs()).c_str())); break;
        case Op::CvtWS: stmt(format("%s = ::p2::fpu_cvt_w(c, %s);", FI(i.fd()).c_str(), F(i.fs()).c_str())); break;
        case Op::CvtSW: stmt(format("%s = f32(s32(%s));", F(i.fd()).c_str(), FI(i.fs()).c_str())); break;
        case Op::CCondS: {
            const std::string a = F(i.fs()), b = F(i.ft());
            std::string expr;
            switch (w & 7) {
            case 0: expr = "0u"; break;
            case 2: expr = format("u32(%s == %s)", a.c_str(), b.c_str()); break;
            case 4: expr = format("u32(%s < %s)", a.c_str(), b.c_str()); break;
            case 6: expr = format("u32(%s <= %s)", a.c_str(), b.c_str()); break;
            default: expr = format("::p2::fpu_compare(%s, %s, %u)", a.c_str(), b.c_str(), w & 7); break;
            }
            stmt(format("c->fcc = %s;", expr.c_str()));
            break;
        }
        case Op::Lwc1: stmt(format("%s = ::p2::rd32(m, %s);", FI(i.ft()).c_str(), mem_addr(i, i.simm()).c_str())); break;
        case Op::Swc1: stmt(format("::p2::wr32(m, %s, %s);", mem_addr(i, i.simm()).c_str(), FI(i.ft()).c_str())); break;

        // --- VFPU ---
        case Op::LvS: stmt(format("::p2::vfpu::lv_s(c, m, 0x%08Xu, %s);", w, mem_addr(i, i.vmem_offset()).c_str())); break;
        case Op::LvQ: stmt(format("::p2::vfpu::lv_q(c, m, 0x%08Xu, %s);", w, mem_addr(i, i.vmem_offset()).c_str())); break;
        case Op::SvS: stmt(format("::p2::vfpu::sv_s(c, m, 0x%08Xu, %s);", w, mem_addr(i, i.vmem_offset()).c_str())); break;
        case Op::SvQ: stmt(format("::p2::vfpu::sv_q(c, m, 0x%08Xu, %s);", w, mem_addr(i, i.vmem_offset()).c_str())); break;
        case Op::Mfv: set(indent, pc, w, i.rt(), format("::p2::vfpu::mfv(c, 0x%08Xu)", w)); break;
        case Op::Mfvc: set(indent, pc, w, i.rt(), format("::p2::vfpu::mfvc(c, 0x%08Xu)", w)); break;
        case Op::Mtv: stmt(format("::p2::vfpu::mtv(c, 0x%08Xu, %s);", w, rt.c_str())); break;
        case Op::Mtvc: stmt(format("::p2::vfpu::mtvc(c, 0x%08Xu, %s);", w, rt.c_str())); break;
        case Op::Vnop:
        case Op::Vsync:
        case Op::Vflush: stmt(";"); break;

        default:
            if (const char* helper = vfpu_helper(i.op)) stmt(format("::p2::vfpu::%s(c, 0x%08Xu);", helper, w));
            else unimplemented(indent, pc, w);
            break;
        }
    }

    // Collapses lwl/lwr and swl/swr pairs into one unaligned access. Returns true when it did.
    bool unaligned_pair(int indent, u32 pc, const Insn& a, const Insn& b) {
        auto pair = [&](Op left, Op right, const Insn** l, const Insn** r) {
            if (a.op == left && b.op == right) {
                *l = &a;
                *r = &b;
                return true;
            }
            if (a.op == right && b.op == left) {
                *l = &b;
                *r = &a;
                return true;
            }
            return false;
        };
        const Insn *l = nullptr, *r = nullptr;
        bool load = pair(Op::Lwl, Op::Lwr, &l, &r);
        if (!load && !pair(Op::Swl, Op::Swr, &l, &r)) return false;
        if (l->rt() != r->rt() || l->rs() != r->rs() || l->simm() != r->simm() + 3) return false;
        if (load && l->rt() == l->rs()) return false; // the first half would clobber the base
        if (b.has_delay_slot()) return false;

        std::string addr = mem_addr(*r, r->simm());
        std::string text;
        if (load) text = l->rt() ? format("c->r[%u] = ::p2::rd32(m, %s);", l->rt(), addr.c_str()) : std::string(";");
        else text = format("::p2::wr32(m, %s, %s);", addr.c_str(), R(l->rt()).c_str());
        line(indent, pc, a.word, text);
        if (opt_.comments) line(indent, pc + 4, b.word, "");
        return true;
    }

    // --- control flow -------------------------------------------------------------------------------------

    std::string transfer(u32 target) const {
        if (target >= fn_->entry && target < fn_->limit && block_starts_.count(target))
            return format("goto L_%08X;", target);
        if (prog_.callable(target)) return format("P2_TAIL(%s);", function_name(target).c_str());
        if (const ImportStub* stub = prog_.stub_at(target)) return format("P2_TAIL(%s);", import_thunk_name(*stub).c_str());
        return format("P2_JUMP_IND(0x%08Xu);", target);
    }

    std::string call(u32 target, u32 ra) const {
        if (prog_.callable(target)) return format("P2_CALL(%s, 0x%08Xu);", function_name(target).c_str(), ra);
        if (const ImportStub* stub = prog_.stub_at(target))
            return format("P2_CALL(%s, 0x%08Xu);", import_thunk_name(*stub).c_str(), ra);
        return format("P2_CALL_IND(0x%08Xu, 0x%08Xu);", target, ra);
    }

    static std::string condition(const Insn& i) {
        const std::string rs = R(i.rs()), rt = R(i.rt());
        switch (i.op) {
        case Op::Beq:
        case Op::Beql: return format("%s == %s", rs.c_str(), rt.c_str());
        case Op::Bne:
        case Op::Bnel: return format("%s != %s", rs.c_str(), rt.c_str());
        case Op::Blez:
        case Op::Blezl: return format("s32(%s) <= 0", rs.c_str());
        case Op::Bgtz:
        case Op::Bgtzl: return format("s32(%s) > 0", rs.c_str());
        case Op::Bltz:
        case Op::Bltzl:
        case Op::Bltzal:
        case Op::Bltzall: return format("s32(%s) < 0", rs.c_str());
        case Op::Bgez:
        case Op::Bgezl:
        case Op::Bgezal:
        case Op::Bgezall: return format("s32(%s) >= 0", rs.c_str());
        case Op::Bc1f:
        case Op::Bc1fl: return "c->fcc == 0";
        case Op::Bc1t:
        case Op::Bc1tl: return "c->fcc != 0";
        case Op::Bvf:
        case Op::Bvfl: return format("((c->vctrl[::p2::kVfpuCc] >> %u) & 1) == 0", (i.word >> 18) & 7);
        case Op::Bvt:
        case Op::Bvtl: return format("((c->vctrl[::p2::kVfpuCc] >> %u) & 1) != 0", (i.word >> 18) & 7);
        default: return "false";
        }
    }

    // Emits an instruction with a delay slot. Returns true when control cannot fall through to pc + 8.
    bool control(u32 pc, const Insn& i, const Insn& slot) {
        const u32 ra = pc + 8;
        const u16 flags = i.flags();

        if (flags & allegrex::kBranch) {
            const u32 target = i.branch_target(pc);
            const std::string cond = condition(i);
            if (flags & allegrex::kLink) {
                line(1, pc, i.word, format("c->r[31] = 0x%08Xu;", ra));
                if (flags & allegrex::kLikely) {
                    raw(1, format("if (%s) {", cond.c_str()));
                    insn(2, pc + 4, slot);
                    raw(2, call(target, ra));
                    raw(1, "}");
                } else {
                    raw(1, "{");
                    raw(2, format("const bool taken_ = %s;", cond.c_str()));
                    insn(2, pc + 4, slot);
                    raw(2, format("if (taken_) %s", call(target, ra).c_str()));
                    raw(1, "}");
                }
                return false;
            }
            if (always_taken(i)) {
                line(1, pc, i.word, "");
                insn(1, pc + 4, slot);
                raw(1, transfer(target));
                return true;
            }
            if (flags & allegrex::kLikely) {
                line(1, pc, i.word, format("if (%s) {", cond.c_str()));
                insn(2, pc + 4, slot);
                raw(2, transfer(target));
                raw(1, "}");
                return false;
            }
            line(1, pc, i.word, "{");
            raw(2, format("const bool taken_ = %s;", cond.c_str()));
            insn(2, pc + 4, slot);
            raw(2, format("if (taken_) %s", transfer(target).c_str()));
            raw(1, "}");
            return false;
        }

        switch (i.op) {
        case Op::J:
            line(1, pc, i.word, "");
            insn(1, pc + 4, slot);
            raw(1, transfer(i.jump_target(pc)));
            return true;
        case Op::Jal:
            line(1, pc, i.word, format("c->r[31] = 0x%08Xu;", ra));
            insn(1, pc + 4, slot);
            raw(1, call(i.jump_target(pc), ra));
            return false;
        case Op::Jalr:
            line(1, pc, i.word, "{");
            raw(2, format("const u32 target_ = %s;", R(i.rs()).c_str()));
            if (i.rd()) raw(2, format("c->r[%u] = 0x%08Xu;", i.rd(), ra));
            insn(2, pc + 4, slot);
            raw(2, format("%s(target_, 0x%08Xu);", i.rd() == 31 ? "P2_CALL_IND" : "P2_CALL_IND_NOCHECK", ra));
            raw(1, "}");
            return false;
        case Op::Jr: {
            if (i.rs() == 31) {
                line(1, pc, i.word, "");
                insn(1, pc + 4, slot);
                raw(1, "return;");
                return true;
            }
            line(1, pc, i.word, "{");
            raw(2, format("const u32 target_ = %s;", R(i.rs()).c_str()));
            insn(2, pc + 4, slot);
            auto it = prog_.tables.find(pc);
            if (it != prog_.tables.end()) {
                std::vector<u32> targets = it->second.targets;
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
                raw(2, "switch (target_) {");
                for (u32 t : targets) raw(2, format("case 0x%08Xu: %s", t, transfer(t).c_str()));
                raw(2, "default: P2_JUMP_IND(target_);");
                raw(2, "}");
            } else {
                raw(2, "P2_JUMP_IND(target_);");
            }
            raw(1, "}");
            return true;
        }
        default:
            unimplemented(1, pc, i.word);
            return true;
        }
    }

    void block(const Block& b, const Block* next) {
        append_format(*out_, "L_%08X:\n", b.start);
        const u32 count = (b.end - b.start) / 4;
        const bool poll = b.loop_head || b.start == fn_->entry;
        raw(1, format(poll ? "P2_TICK_CHECK(%u);" : "P2_TICK(%u);", count));

        bool terminated = false;
        u32 pc = b.start;
        while (pc < b.end) {
            Insn i(in_.word(pc));
            if (i.op == Op::Invalid) {
                result_.unimplemented.push_back(format("%08X  %08X  (undecodable)", pc, i.word));
                line(1, pc, i.word, format("::p2::rt_trap(c, m, 0x%08Xu, 0x%08Xu);", pc, i.word));
                terminated = true;
                pc += 4;
                break;
            }
            if (!i.has_delay_slot()) {
                if (pc + 8 <= b.end) {
                    Insn following(in_.word(pc + 4));
                    if (unaligned_pair(1, pc, i, following)) {
                        pc += 8;
                        continue;
                    }
                }
                insn(1, pc, i);
                pc += 4;
                continue;
            }
            Insn slot(in_.word(pc + 4));
            terminated = control(pc, i, slot);
            pc += 8;
        }
        if (!terminated && !(next && next->start == b.end)) raw(1, transfer(b.end));
    }

    const Program& prog_;
    const Input& in_;
    const EmitOptions& opt_;
    EmitResult& result_;
    std::string* out_ = nullptr;
    const Function* fn_ = nullptr;
    std::unordered_set<u32> block_starts_;
};

const char* const kBanner = "// Generated by p2recomp from the game executable. Do not edit; do not redistribute.\n";

} // namespace

void emit(const Program& prog, const EmitOptions& opt, EmitResult& out) {
    const Input& in = *prog.input;

    // The dispatch table spans all code plus the import stubs.
    u32 lo = ~0u, hi = 0;
    for (const CodeRange& r : in.code) {
        lo = std::min(lo, r.start);
        hi = std::max(hi, r.end);
    }
    for (const ImportStub& s : in.stubs) {
        lo = std::min(lo, s.addr);
        hi = std::max(hi, s.addr + 8);
    }
    out.text_base = lo;
    out.text_words = (hi - lo) / 4;

    out.files["gen_config.h"] = format("%s#pragma once\n\n#define P2_TEXT_BASE 0x%08Xu\n#define P2_TEXT_WORDS %uu\n",
                                       kBanner, out.text_base, out.text_words);

    // --- declarations ---
    {
        std::string h = kBanner;
        h += "#pragma once\n\n#include \"runtime/cpu/recomp.h\"\n\n";
        for (const Function& fn : prog.functions) {
            append_format(h, "P2_FN(%s);\n", function_name(fn.entry).c_str());
            for (u32 s : fn.secondary) append_format(h, "P2_FN(%s);\n", function_name(s).c_str());
        }
        h += "\n";
        for (const ImportStub& s : in.stubs) {
            append_format(h, "P2_FN(%s);\n", import_thunk_name(s).c_str());
            append_format(h, "void %s(::p2::Cpu* c, u8* m);\n", import_hle_name(s).c_str());
        }
        out.files["gen_funcs.h"] = std::move(h);
    }

    // --- function bodies, split into shards of similar size ---
    {
        Emitter emitter(prog, opt, out);
        u64 total = 0;
        for (const Function& fn : prog.functions) total += fn.insn_count + 8;
        const u32 shards = std::max<u32>(opt.shards, 1);
        const u64 per_shard = total / shards + 1;

        size_t next_fn = 0;
        for (u32 shard = 0; shard < shards; ++shard) {
            std::string body = kBanner;
            body += "#include \"gen_funcs.h\"\n\n";
            u64 budget = 0;
            while (next_fn < prog.functions.size() && (budget < per_shard || shard + 1 == shards)) {
                const Function& fn = prog.functions[next_fn++];
                emitter.function(fn, body);
                budget += fn.insn_count + 8;
            }
            out.files[format("gen_%03u.cpp", shard)] = std::move(body);
        }
    }

    // --- tables ---
    {
        std::string t = kBanner;
        t += "#include \"gen_funcs.h\"\n\n";
        for (const ImportStub& s : in.stubs)
            append_format(t, "P2_FN(%s) { P2_HLE(%u, %s); }\n", import_thunk_name(s).c_str(), s.index,
                          import_hle_name(s).c_str());
        t += "\nnamespace p2 {\n\nstatic const GenEntry kEntries[] = {\n";
        size_t entry_count = in.stubs.size();
        for (const Function& fn : prog.functions) {
            append_format(t, "    {0x%08Xu, %s},\n", fn.entry, function_name(fn.entry).c_str());
            for (u32 s : fn.secondary) append_format(t, "    {0x%08Xu, %s},\n", s, function_name(s).c_str());
            entry_count += 1 + fn.secondary.size();
        }
        for (const ImportStub& s : in.stubs)
            append_format(t, "    {0x%08Xu, %s},\n", s.addr, import_thunk_name(s).c_str());
        t += "};\n\nstatic const GenImport kImports[] = {\n";
        for (const ImportStub& s : in.stubs)
            append_format(t, "    {\"%s\", \"%s\", 0x%08Xu, 0x%08Xu, %s},\n", s.lib.c_str(),
                          s.name.empty() ? "?" : s.name.c_str(), s.nid, s.addr, import_hle_name(s).c_str());
        if (in.stubs.empty()) t += "    {nullptr, nullptr, 0, 0, nullptr},\n";
        t += "};\n\n";
        append_format(t,
                      "const GenModule g_gen_module = {\n    \"%s\",\n    0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu,\n"
                      "    P2_TEXT_BASE, P2_TEXT_WORDS,\n    kEntries, %zuu,\n    kImports, %zuu,\n};\n\n",
                      opt.sha256.c_str(), opt.module_base, opt.module_end, opt.module_entry, opt.module_gp,
                      entry_count, in.stubs.size());
        t += "GuestFn g_dispatch[P2_TEXT_WORDS];\n\n} // namespace p2\n";
        out.files["gen_table.cpp"] = std::move(t);
    }
}

} // namespace p2::recomp
