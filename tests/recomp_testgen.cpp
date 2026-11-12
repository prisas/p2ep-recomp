// Builds a small synthetic guest program, runs it through the recompiler and writes the generated
// C++ plus a table of expected results. The companion runner (recomp_run.cpp) is compiled together
// with that output and executes every case.

#include "lib/common/file.h"
#include "tools/recomp/analysis.h"
#include "tools/recomp/emit.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace p2;
using namespace p2::recomp;

namespace {

constexpr u32 kBase = 0x08804000;
constexpr u32 kDataBase = kBase + 0x4000;
constexpr u32 kImageSize = 0x6000;

enum Reg : u32 {
    zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
    s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra
};

struct Label {
    int id = -1;
};

// A just-enough MIPS assembler with forward and backward labels.
class Asm {
public:
    u32 pc() const { return kBase + u32(words_.size()) * 4; }
    void word(u32 w) { words_.push_back(w); }

    Label label() {
        label_addr_.push_back(0);
        return Label{int(label_addr_.size()) - 1};
    }
    void bind(Label l) { label_addr_[size_t(l.id)] = pc(); }
    u32 address(Label l) const { return label_addr_[size_t(l.id)]; }

    void r(u32 rs, u32 rt, u32 rd, u32 sa, u32 fn) { word((rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn); }
    void i(u32 op, u32 rs, u32 rt, u32 imm) { word((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)); }
    void branch(u32 op, u32 rs, u32 rt, Label target) {
        fixups_.push_back({words_.size(), target.id, false});
        i(op, rs, rt, 0);
    }
    void jump(u32 op, Label target) {
        fixups_.push_back({words_.size(), target.id, true});
        word(op << 26);
    }

    void nop() { word(0); }
    void addu(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 33); }
    void subu(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 35); }
    void and_(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 36); }
    void or_(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 37); }
    void xor_(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 38); }
    void nor(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 39); }
    void slt(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 42); }
    void sltu(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 43); }
    void max(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 44); }
    void min(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 45); }
    void movz(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 10); }
    void movn(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 11); }
    void sll(u32 rd, u32 rt, u32 sa) { r(0, rt, rd, sa, 0); }
    void srl(u32 rd, u32 rt, u32 sa) { r(0, rt, rd, sa, 2); }
    void sra(u32 rd, u32 rt, u32 sa) { r(0, rt, rd, sa, 3); }
    void rotr(u32 rd, u32 rt, u32 sa) { r(1, rt, rd, sa, 2); }
    void sllv(u32 rd, u32 rt, u32 rs) { r(rs, rt, rd, 0, 4); }
    void srlv(u32 rd, u32 rt, u32 rs) { r(rs, rt, rd, 0, 6); }
    void srav(u32 rd, u32 rt, u32 rs) { r(rs, rt, rd, 0, 7); }
    void mult(u32 rs, u32 rt) { r(rs, rt, 0, 0, 24); }
    void multu(u32 rs, u32 rt) { r(rs, rt, 0, 0, 25); }
    void div(u32 rs, u32 rt) { r(rs, rt, 0, 0, 26); }
    void divu(u32 rs, u32 rt) { r(rs, rt, 0, 0, 27); }
    void madd(u32 rs, u32 rt) { r(rs, rt, 0, 0, 28); }
    void mfhi(u32 rd) { r(0, 0, rd, 0, 16); }
    void mflo(u32 rd) { r(0, 0, rd, 0, 18); }
    void clz(u32 rd, u32 rs) { r(rs, 0, rd, 0, 22); }
    void jr(u32 rs) { r(rs, 0, 0, 0, 8); }
    void jalr(u32 rs) { r(rs, 0, 31, 0, 9); }
    void addiu(u32 rt, u32 rs, s32 imm) { i(9, rs, rt, u32(imm)); }
    void slti(u32 rt, u32 rs, s32 imm) { i(10, rs, rt, u32(imm)); }
    void sltiu(u32 rt, u32 rs, s32 imm) { i(11, rs, rt, u32(imm)); }
    void andi(u32 rt, u32 rs, u32 imm) { i(12, rs, rt, imm); }
    void ori(u32 rt, u32 rs, u32 imm) { i(13, rs, rt, imm); }
    void xori(u32 rt, u32 rs, u32 imm) { i(14, rs, rt, imm); }
    void lui(u32 rt, u32 imm) { i(15, 0, rt, imm); }
    void li(u32 rt, u32 value) {
        lui(rt, value >> 16);
        ori(rt, rt, value & 0xFFFF);
    }
    void beq(u32 rs, u32 rt, Label l) { branch(4, rs, rt, l); }
    void bne(u32 rs, u32 rt, Label l) { branch(5, rs, rt, l); }
    void blez(u32 rs, Label l) { branch(6, rs, 0, l); }
    void bgtz(u32 rs, Label l) { branch(7, rs, 0, l); }
    void beql(u32 rs, u32 rt, Label l) { branch(20, rs, rt, l); }
    void bnel(u32 rs, u32 rt, Label l) { branch(21, rs, rt, l); }
    void bltz(u32 rs, Label l) { branch(1, rs, 0, l); }
    void bgez(u32 rs, Label l) { branch(1, rs, 1, l); }
    void bgezal(u32 rs, Label l) { branch(1, rs, 17, l); }
    void j(Label l) { jump(2, l); }
    void jal(Label l) { jump(3, l); }
    void jal_abs(u32 target) { word((3u << 26) | ((target >> 2) & 0x03FFFFFF)); }
    void lb(u32 rt, s32 off, u32 rs) { i(32, rs, rt, u32(off)); }
    void lh(u32 rt, s32 off, u32 rs) { i(33, rs, rt, u32(off)); }
    void lwl(u32 rt, s32 off, u32 rs) { i(34, rs, rt, u32(off)); }
    void lw(u32 rt, s32 off, u32 rs) { i(35, rs, rt, u32(off)); }
    void lbu(u32 rt, s32 off, u32 rs) { i(36, rs, rt, u32(off)); }
    void lhu(u32 rt, s32 off, u32 rs) { i(37, rs, rt, u32(off)); }
    void lwr(u32 rt, s32 off, u32 rs) { i(38, rs, rt, u32(off)); }
    void sb(u32 rt, s32 off, u32 rs) { i(40, rs, rt, u32(off)); }
    void sh(u32 rt, s32 off, u32 rs) { i(41, rs, rt, u32(off)); }
    void swl(u32 rt, s32 off, u32 rs) { i(42, rs, rt, u32(off)); }
    void sw(u32 rt, s32 off, u32 rs) { i(43, rs, rt, u32(off)); }
    void swr(u32 rt, s32 off, u32 rs) { i(46, rs, rt, u32(off)); }
    void seb(u32 rd, u32 rt) { word((31u << 26) | (rt << 16) | (rd << 11) | (16u << 6) | 32); }
    void seh(u32 rd, u32 rt) { word((31u << 26) | (rt << 16) | (rd << 11) | (24u << 6) | 32); }
    void wsbh(u32 rd, u32 rt) { word((31u << 26) | (rt << 16) | (rd << 11) | (2u << 6) | 32); }
    void ext(u32 rt, u32 rs, u32 pos, u32 size) { word((31u << 26) | (rs << 21) | (rt << 16) | ((size - 1) << 11) | (pos << 6)); }
    void ins(u32 rt, u32 rs, u32 pos, u32 size) {
        word((31u << 26) | (rs << 21) | (rt << 16) | ((pos + size - 1) << 11) | (pos << 6) | 4);
    }
    void mtc1(u32 rt, u32 fs) { word((17u << 26) | (4u << 21) | (rt << 16) | (fs << 11)); }
    void mfc1(u32 rt, u32 fs) { word((17u << 26) | (rt << 16) | (fs << 11)); }
    void fpu(u32 fn, u32 fd, u32 fs, u32 ft) { word((17u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn); }
    void cvt_s_w(u32 fd, u32 fs) { word((17u << 26) | (20u << 21) | (fs << 11) | (fd << 6) | 32); }
    void c_lt_s(u32 fs, u32 ft) { word((17u << 26) | (16u << 21) | (ft << 16) | (fs << 11) | 0x3C); }
    void bc1t(Label l) { branch(17, 8, 1, l); }
    void bc1f(Label l) { branch(17, 8, 0, l); }
    void mtv(u32 rt, u32 vreg) { word((18u << 26) | (7u << 21) | (rt << 16) | vreg); }
    void mfv(u32 rt, u32 vreg) { word((18u << 26) | (3u << 21) | (rt << 16) | vreg); }
    void ret() {
        jr(ra);
        nop();
    }

    std::vector<u32> finish() {
        for (const Fixup& f : fixups_) {
            u32 target = label_addr_[size_t(f.label)];
            u32 at_pc = kBase + u32(f.index) * 4;
            if (f.absolute) words_[f.index] |= (target >> 2) & 0x03FFFFFF;
            else words_[f.index] |= u32((s32(target) - s32(at_pc + 4)) >> 2) & 0xFFFF;
        }
        return words_;
    }

private:
    struct Fixup {
        size_t index;
        int label;
        bool absolute;
    };
    std::vector<u32> words_;
    std::vector<u32> label_addr_;
    std::vector<Fixup> fixups_;
};

struct Result {
    u32 v0 = 0, v1 = 0;
};

struct Case {
    std::string name;
    u32 entry;
    u32 args[4];
    Result expected;
};

std::vector<Case> g_cases;

using Ref = std::function<Result(u32, u32, u32, u32)>;

void add_cases(const std::string& name, u32 entry, const Ref& ref, std::initializer_list<std::array<u32, 4>> inputs) {
    for (const auto& in : inputs) g_cases.push_back({name, entry, {in[0], in[1], in[2], in[3]}, ref(in[0], in[1], in[2], in[3])});
}

u32 f2u(f32 f) { return std::bit_cast<u32>(f); }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: p2_recomp_testgen <output-dir>\n");
        return 2;
    }
    const std::filesystem::path out_dir = argv[1];

    Asm a;
    Input in;
    in.base = kBase;
    in.image.assign(kImageSize, 0);
    auto data32 = [&](u32 addr, u32 value) { store_le<u32>(in.image.data() + (addr - kBase), value); };

    // --- integer arithmetic ------------------------------------------------------------------------------
    u32 fn = a.pc();
    a.addu(t0, a0, a1);
    a.subu(t1, a0, a1);
    a.and_(t2, a0, a1);
    a.or_(t3, a0, a1);
    a.xor_(t4, t0, t1);
    a.nor(t5, t2, t3);
    a.slt(t6, a0, a1);
    a.sltu(t7, a0, a1);
    a.xor_(v0, t4, t5);
    a.sll(t6, t6, 4);
    a.or_(v1, t6, t7);
    a.jr(ra);
    a.addu(v0, v0, t0); // delay slot of the return still executes
    add_cases("arith", fn, [](u32 x, u32 y, u32, u32) {
        u32 t0 = x + y, t1 = x - y, t2 = x & y, t3 = x | y;
        return Result{((t0 ^ t1) ^ ~(t2 | t3)) + t0, (u32(s32(x) < s32(y)) << 4) | u32(x < y)};
    }, {{{5, 7, 0, 0}}, {{0xFFFFFFFF, 1, 0, 0}}, {{0x80000000, 0x7FFFFFFF, 0, 0}}, {{0, 0, 0, 0}}});

    fn = a.pc();
    a.sll(t0, a0, 3);
    a.srl(t1, a0, 5);
    a.sra(t2, a0, 5);
    a.sllv(t3, a0, a1);
    a.srlv(t4, a0, a1);
    a.srav(t5, a0, a1);
    a.rotr(t6, a0, 8);
    a.xor_(v0, t0, t1);
    a.xor_(v0, v0, t2);
    a.xor_(v0, v0, t3);
    a.xor_(v1, t4, t5);
    a.jr(ra);
    a.xor_(v1, v1, t6);
    add_cases("shifts", fn, [](u32 x, u32 y, u32, u32) {
        u32 s = y & 31;
        return Result{(x << 3) ^ (x >> 5) ^ u32(s32(x) >> 5) ^ (x << s),
                      (x >> s) ^ u32(s32(x) >> s) ^ std::rotr(x, 8)};
    }, {{{0x80000001, 4, 0, 0}}, {{0x12345678, 33, 0, 0}}, {{0xFFFF0000, 31, 0, 0}}});

    fn = a.pc();
    a.addiu(t0, a0, -100);
    a.slti(t1, a0, -5);
    a.sltiu(t2, a0, -5); // compares against 0xFFFFFFFB unsigned
    a.andi(t3, a0, 0xF0F0);
    a.ori(t4, a0, 0x8001);
    a.xori(t5, a0, 0xFFFF);
    a.lui(t6, 0xABCD);
    a.addu(v0, t0, t3);
    a.addu(v0, v0, t4);
    a.addu(v0, v0, t5);
    a.addu(v0, v0, t6);
    a.sll(t1, t1, 1);
    a.jr(ra);
    a.or_(v1, t1, t2);
    add_cases("immediates", fn, [](u32 x, u32, u32, u32) {
        return Result{(x - 100) + (x & 0xF0F0) + (x | 0x8001) + (x ^ 0xFFFF) + 0xABCD0000u,
                      (u32(s32(x) < -5) << 1) | u32(x < 0xFFFFFFFBu)};
    }, {{{0, 0, 0, 0}}, {{u32(-6), 0, 0, 0}}, {{0xFFFFFFFC, 0, 0, 0}}, {{0x12348765, 0, 0, 0}}});

    fn = a.pc();
    a.mult(a0, a1);
    a.mflo(t0);
    a.mfhi(t1);
    a.madd(a0, a1);
    a.mflo(t2);
    a.div(a0, a1);
    a.mflo(t3);
    a.mfhi(t4);
    a.divu(a0, a1);
    a.mflo(t5);
    a.addu(v0, t0, t1);
    a.addu(v0, v0, t2);
    a.addu(v1, t3, t4);
    a.jr(ra);
    a.addu(v1, v1, t5);
    add_cases("muldiv", fn, [](u32 x, u32 y, u32, u32) {
        s64 p = s64(s32(x)) * s64(s32(y));
        u64 acc = u64(p) + u64(p);
        return Result{u32(p) + u32(u64(p) >> 32) + u32(acc), u32(s32(x) / s32(y)) + u32(s32(x) % s32(y)) + x / y};
    }, {{{100, 7, 0, 0}}, {{u32(-100), 7, 0, 0}}, {{0x7FFFFFFF, 0x7FFFFFFF, 0, 0}}, {{12345, u32(-3), 0, 0}}});

    fn = a.pc();
    a.clz(t0, a0);
    a.seb(t1, a0);
    a.seh(t2, a0);
    a.wsbh(t3, a0);
    a.ext(t4, a0, 4, 12);
    a.addu(t5, a1, zero);
    a.ins(t5, a0, 8, 16);
    a.max(t6, a0, a1);
    a.min(t7, a0, a1);
    a.addu(v0, t0, t1);
    a.addu(v0, v0, t2);
    a.addu(v0, v0, t3);
    a.addu(v1, t4, t5);
    a.addu(v1, v1, t6);
    a.addu(v1, v1, t7);
    a.addiu(t8, zero, 77);
    a.movz(v0, t8, a2); // if a2 == 0: v0 = 77
    a.jr(ra);
    a.movn(v1, t8, a2); // if a2 != 0: v1 = 77
    add_cases("bitops", fn, [](u32 x, u32 y, u32 z, u32) {
        u32 ins = (y & ~0x00FFFF00u) | ((x << 8) & 0x00FFFF00u);
        u32 mx = s32(x) > s32(y) ? x : y, mn = s32(x) < s32(y) ? x : y;
        Result r{u32(std::countl_zero(x)) + u32(s32(s8(x))) + u32(s32(s16(x))) + (((x & 0x00FF00FF) << 8) | ((x >> 8) & 0x00FF00FF)),
                 ((x >> 4) & 0xFFF) + ins + mx + mn};
        if (z == 0) r.v0 = 77;
        else r.v1 = 77;
        return r;
    }, {{{0x00F1E2D3, 0xAAAAAAAA, 0, 0}}, {{0x80FF7F80, 5, 1, 0}}, {{0, u32(-1), 0, 0}}});

    // --- branches and delay slots ------------------------------------------------------------------------
    fn = a.pc();
    {
        Label taken = a.label(), done = a.label();
        a.addiu(v0, zero, 0);
        a.addiu(v1, zero, 0);
        a.beq(a0, a1, taken);
        a.addiu(v0, v0, 1); // executes either way
        a.addiu(v1, v1, 10);
        a.bind(taken);
        a.bnel(a0, a1, done);
        a.addiu(v0, v0, 100); // only when the likely branch is taken
        a.addiu(v1, v1, 1000);
        a.bind(done);
        a.ret();
    }
    add_cases("branch_delay", fn, [](u32 x, u32 y, u32, u32) {
        Result r{1, 0};
        if (x != y) r.v1 += 10;
        if (x != y) r.v0 += 100;
        else r.v1 += 1000;
        return r;
    }, {{{1, 1, 0, 0}}, {{1, 2, 0, 0}}});

    fn = a.pc();
    {
        // The delay slot overwrites the register the branch tests: the old value decides.
        Label yes = a.label();
        a.addiu(v0, zero, 1);
        a.bne(a0, zero, yes);
        a.addiu(a0, zero, 0);
        a.addiu(v0, zero, 2);
        a.bind(yes);
        a.jr(ra);
        a.addu(v1, a0, zero);
    }
    add_cases("branch_latch", fn, [](u32 x, u32, u32, u32) { return Result{x ? 1u : 2u, 0}; },
              {{{0, 0, 0, 0}}, {{9, 0, 0, 0}}});

    fn = a.pc();
    {
        Label loop = a.label(), neg = a.label(), out = a.label();
        a.addiu(v0, zero, 0);
        a.addiu(v1, zero, 0);
        a.blez(a0, out);
        a.nop();
        a.bind(loop);
        a.addu(v0, v0, a0);
        a.addiu(a0, a0, -1);
        a.bgtz(a0, loop);
        a.addiu(v1, v1, 1);
        a.bltz(a1, neg);
        a.nop();
        a.bgez(a1, out);
        a.addiu(v1, v1, 0x100);
        a.bind(neg);
        a.addiu(v1, v1, 0x200);
        a.bind(out);
        a.ret();
    }
    add_cases("loops", fn, [](u32 n, u32 y, u32, u32) {
        Result r{};
        if (s32(n) <= 0) return r;
        for (s32 k = s32(n); k > 0; --k) {
            r.v0 += u32(k);
            ++r.v1;
        }
        r.v1 += s32(y) < 0 ? 0x200u : 0x100u;
        return r;
    }, {{{10, 0, 0, 0}}, {{1000, u32(-1), 0, 0}}, {{0, 5, 0, 0}}, {{u32(-4), 5, 0, 0}}});

    // --- calls ---------------------------------------------------------------------------------------------
    Label leaf = a.label(), mid = a.label();
    const u32 fn_calls = a.pc();
    a.addiu(sp, sp, -16);
    a.sw(ra, 0, sp);
    a.sw(s0, 4, sp);
    a.jal(mid);
    a.addu(s0, a0, zero); // delay slot runs before the callee
    a.addu(v0, v0, s0);
    a.bgezal(zero, leaf); // unconditional call through a branch-and-link
    a.addiu(a0, zero, 5);
    a.addu(v1, v0, zero);
    a.lw(s0, 4, sp);
    a.lw(ra, 0, sp);
    a.jr(ra);
    a.addiu(sp, sp, 16);

    a.bind(mid); // calls the leaf and adds to its result
    a.addiu(sp, sp, -8);
    a.sw(ra, 0, sp);
    a.jal(leaf);
    a.addiu(a0, a0, 1);
    a.lw(ra, 0, sp);
    a.addiu(v0, v0, 3);
    a.jr(ra);
    a.addiu(sp, sp, 8);

    a.bind(leaf); // v0 = a0 * 2
    a.jr(ra);
    a.addu(v0, a0, a0);
    add_cases("calls", fn_calls, [](u32 x, u32, u32, u32) { return Result{10, 10}; }, {{{4, 0, 0, 0}}, {{100, 0, 0, 0}}});
    // v0 after the first call is (x+1)*2+3+x, then the second call overwrites it with 10.

    // Indirect call through a pointer loaded from data, then a tail jump to a function.
    const u32 fn_ptr_slot = kDataBase + 0x100;
    const u32 fn_indirect = a.pc();
    {
        a.addiu(sp, sp, -8);
        a.sw(ra, 0, sp);
        a.li(t0, fn_ptr_slot);
        a.lw(t9, 0, t0);
        a.jalr(t9);
        a.addiu(a0, a0, 2);
        a.lw(ra, 0, sp);
        a.addiu(sp, sp, 8);
        a.j(leaf); // tail call: returns to our caller
        a.addu(a0, v0, zero);
    }
    add_cases("indirect_and_tail", fn_indirect, [](u32 x, u32, u32, u32) { return Result{(x + 2) * 2 * 2, 0}; },
              {{{1, 0, 0, 0}}, {{20, 0, 0, 0}}});

    // Import call.
    const u32 stub_addr = kBase + 0x3000;
    fn = a.pc();
    a.addiu(sp, sp, -8);
    a.sw(ra, 0, sp);
    a.jal_abs(stub_addr);
    a.addiu(a1, a1, 1);
    a.lw(ra, 0, sp);
    a.addiu(v0, v0, 1);
    a.jr(ra);
    a.addiu(sp, sp, 8);
    add_cases("import", fn, [](u32 x, u32 y, u32, u32) { return Result{x * 1000 + (y + 1) + 1, 0}; }, {{{3, 4, 0, 0}}});

    // --- switch through a jump table ---------------------------------------------------------------------
    const u32 table_addr = kDataBase + 0x200;
    const u32 fn_switch = a.pc();
    u32 switch_lui_pc;
    Label cases_[4] = {a.label(), a.label(), a.label(), a.label()};
    {
        Label dflt = a.label();
        a.sltiu(t0, a0, 4);
        a.beq(t0, zero, dflt);
        a.sll(t1, a0, 2);
        switch_lui_pc = a.pc();
        a.lui(t2, (table_addr + 0x8000) >> 16); // the low half is sign-extended, so round the high half
        a.addiu(t2, t2, s32(s16(table_addr & 0xFFFF)));
        a.addu(t1, t1, t2);
        a.lw(t3, 0, t1);
        a.jr(t3);
        a.addiu(v0, zero, 0);
        a.bind(cases_[0]);
        a.jr(ra);
        a.addiu(v0, zero, 11);
        a.bind(cases_[1]);
        a.addiu(v0, zero, 22);
        a.bind(cases_[2]); // case 1 falls into case 2
        a.jr(ra);
        a.addiu(v0, v0, 33);
        a.bind(cases_[3]);
        a.jr(ra);
        a.addiu(v0, zero, 44);
        a.bind(dflt);
        a.jr(ra);
        a.addiu(v0, zero, 99);
    }
    add_cases("switch", fn_switch, [](u32 x, u32, u32, u32) {
        static const u32 results[4] = {11, 55, 33, 44};
        return Result{x < 4 ? results[x] : 99u, 0};
    }, {{{0, 0, 0, 0}}, {{1, 0, 0, 0}}, {{2, 0, 0, 0}}, {{3, 0, 0, 0}}, {{4, 0, 0, 0}}, {{u32(-1), 0, 0, 0}}});

    // A function whose inner label is called through a pointer from elsewhere (secondary entry).
    const u32 inner_ptr_slot = kDataBase + 0x104;
    const u32 fn_outer = a.pc();
    Label inner = a.label();
    a.addiu(a0, a0, 1000);
    a.bind(inner);
    a.jr(ra);
    a.addiu(v0, a0, 7);
    const u32 fn_call_inner = a.pc();
    a.addiu(sp, sp, -8);
    a.sw(ra, 0, sp);
    a.li(t0, inner_ptr_slot);
    a.lw(t9, 0, t0);
    a.jalr(t9);
    a.nop();
    a.lw(ra, 0, sp);
    a.jr(ra);
    a.addiu(sp, sp, 8);
    add_cases("secondary_entry_outer", fn_outer, [](u32 x, u32, u32, u32) { return Result{x + 1007, 0}; }, {{{1, 0, 0, 0}}});
    add_cases("secondary_entry_inner", fn_call_inner, [](u32 x, u32, u32, u32) { return Result{x + 7, 0}; }, {{{1, 0, 0, 0}}});

    // --- memory --------------------------------------------------------------------------------------------
    const u32 scratch = kDataBase + 0x400;
    fn = a.pc();
    a.li(t0, scratch);
    a.sw(a0, 0, t0);
    a.sh(a1, 4, t0);
    a.sb(a1, 6, t0);
    a.lb(t1, 3, t0);
    a.lbu(t2, 3, t0);
    a.lh(t3, 2, t0);
    a.lhu(t4, 2, t0);
    a.lw(t5, 4, t0);
    a.addu(v0, t1, t2);
    a.addu(v0, v0, t3);
    a.addu(v0, v0, t4);
    a.jr(ra);
    a.addu(v1, t5, zero);
    add_cases("memory", fn, [](u32 x, u32 y, u32, u32) {
        u32 b3 = x >> 24, h1 = x >> 16;
        return Result{u32(s32(s8(b3))) + b3 + u32(s32(s16(h1))) + h1, (y & 0xFFFF) | ((y & 0xFF) << 16)};
    }, {{{0x80FF1234, 0xABCD, 0, 0}}, {{0x7F001234, 0x1FF, 0, 0}}});

    fn = a.pc();
    a.li(t0, scratch + 0x41); // deliberately unaligned
    a.swl(a0, 3, t0);
    a.swr(a0, 0, t0);
    a.lwl(v0, 3, t0);
    a.lwr(v0, 0, t0);
    a.li(t1, scratch + 0x40);
    a.lw(v1, 0, t1); // the aligned word that the unaligned store partly overwrote
    a.ret();
    add_cases("unaligned", fn, [](u32 x, u32, u32, u32) { return Result{x, x << 8}; }, {{{0xA1B2C3D4, 0, 0, 0}}});

    // --- FPU -----------------------------------------------------------------------------------------------
    fn = a.pc();
    {
        Label less = a.label();
        a.mtc1(a0, 0);
        a.mtc1(a1, 1);
        a.cvt_s_w(0, 0);
        a.cvt_s_w(1, 1);
        a.fpu(0, 2, 0, 1);  // f2 = f0 + f1
        a.fpu(2, 3, 0, 1);  // f3 = f0 * f1
        a.fpu(3, 4, 3, 2);  // f4 = f3 / f2
        a.fpu(13, 5, 4, 0); // f5 = trunc(f4)
        a.mfc1(v0, 5);
        a.c_lt_s(0, 1);
        a.bc1t(less);
        a.addiu(v1, zero, 1);
        a.addiu(v1, zero, 2);
        a.bind(less);
        a.ret();
    }
    add_cases("fpu", fn, [](u32 x, u32 y, u32, u32) {
        f32 fx = f32(s32(x)), fy = f32(s32(y));
        return Result{u32(s32((fx * fy) / (fx + fy))), fx < fy ? 1u : 2u};
    }, {{{7, 9, 0, 0}}, {{100, 3, 0, 0}}, {{u32(-50), 7, 0, 0}}});

    fn = a.pc();
    a.mtc1(a0, 0);
    a.fpu(4, 1, 0, 0); // sqrt
    a.fpu(1, 2, 1, 0); // f2 = sqrt(x) - x
    a.mfc1(v0, 1);
    a.jr(ra);
    a.mfc1(v1, 2);
    add_cases("fpu_bits", fn, [](u32 x, u32, u32, u32) {
        f32 fx = std::bit_cast<f32>(x);
        f32 root = std::sqrt(fx);
        return Result{f2u(root), f2u(root - fx)};
    }, {{{f2u(16.0f), 0, 0, 0}}, {{f2u(2.0f), 0, 0, 0}}});

    // --- VFPU ----------------------------------------------------------------------------------------------
    fn = a.pc();
    a.mtv(a0, 0);
    a.mtv(a1, 1);
    a.word(0x60000000 | (1u << 16) | (0u << 8) | 2); // vadd.s S002? (single register 2) = S000 + S001
    a.jr(ra);
    a.mfv(v0, 2);
    add_cases("vfpu", fn, [](u32 x, u32 y, u32, u32) {
        return Result{f2u(std::bit_cast<f32>(x) + std::bit_cast<f32>(y)), 0};
    }, {{{f2u(1.5f), f2u(2.25f), 0, 0}}});

    // --- assemble the input ------------------------------------------------------------------------------
    std::vector<u32> code = a.finish();
    if (code.size() * 4 > 0x3000) {
        std::fprintf(stderr, "test program too large\n");
        return 1;
    }
    for (size_t k = 0; k < code.size(); ++k) store_le<u32>(in.image.data() + k * 4, code[k]);
    in.code.push_back({kBase, kBase + u32(code.size()) * 4});

    ImportStub stub;
    stub.lib = "TestLib";
    stub.name = "testImport";
    stub.nid = 0x12345678;
    stub.addr = stub_addr;
    stub.index = 0;
    in.stubs.push_back(stub);

    // Data: function pointers and the jump table, with the relocation facts a real executable carries.
    data32(fn_ptr_slot, a.address(leaf));
    in.word_refs.push_back({fn_ptr_slot, a.address(leaf)});
    data32(inner_ptr_slot, a.address(inner));
    in.word_refs.push_back({inner_ptr_slot, a.address(inner)});
    for (u32 k = 0; k < 4; ++k) {
        data32(table_addr + k * 4, a.address(cases_[k]));
        in.word_refs.push_back({table_addr + k * 4, a.address(cases_[k])});
    }
    in.code_refs.push_back({switch_lui_pc, table_addr});

    // Only functions called from outside the program need to be named; the rest is discovered.
    for (const Case& c : g_cases) in.entries.push_back(c.entry);

    Program program;
    std::string error;
    if (!analyze(in, {}, program, &error)) {
        std::fprintf(stderr, "analysis failed: %s\n", error.c_str());
        return 1;
    }

    EmitOptions options;
    options.shards = 1;
    options.sha256 = "synthetic";
    options.module_base = kBase;
    options.module_end = kBase + kImageSize;
    options.module_entry = kBase;
    EmitResult emitted;
    emit(program, options, emitted);
    if (!emitted.unimplemented.empty()) {
        for (const std::string& line : emitted.unimplemented) std::fprintf(stderr, "untranslated: %s\n", line.c_str());
        return 1;
    }

    // Structural expectations the runner cannot observe.
    bool ok = true;
    auto expect = [&](bool cond, const char* what) {
        if (!cond) {
            std::fprintf(stderr, "analysis expectation failed: %s\n", what);
            ok = false;
        }
    };
    expect(program.tables.size() == 1, "one switch table recognised");
    expect(!program.tables.empty() && program.tables.begin()->second.targets.size() == 4, "switch table has four slots");
    expect(program.secondary.count(a.address(inner)) == 1, "inner label is a secondary entry");
    expect(program.entries.count(a.address(leaf)) == 1, "leaf is a function");
    expect(program.entries.count(a.address(inner)) == 0, "inner label did not split its function");
    expect(program.stats.traps == 0, "no undecodable words");
    if (!ok) return 1;

    // --- expected results and the memory image ---------------------------------------------------------
    std::string cases = "// Generated by p2_recomp_testgen.\n";
    append_format(cases, "static const u32 kImageBase = 0x%08Xu;\nstatic const u8 kImage[%u] = {", kBase, kImageSize);
    for (u32 k = 0; k < kImageSize; ++k) {
        if (k % 24 == 0) cases += "\n   ";
        append_format(cases, " %u,", in.image[k]);
    }
    cases += "\n};\n\nstatic const TestCase kCases[] = {\n";
    for (const Case& c : g_cases)
        append_format(cases, "    {\"%s\", 0x%08Xu, {0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu}, 0x%08Xu, 0x%08Xu},\n", c.name.c_str(),
                      c.entry, c.args[0], c.args[1], c.args[2], c.args[3], c.expected.v0, c.expected.v1);
    cases += "};\n";
    emitted.files["gen_cases.inc"] = cases;

    for (const auto& [name, content] : emitted.files) {
        if (!write_file_if_changed(out_dir / name, content)) {
            std::fprintf(stderr, "cannot write %s\n", (out_dir / name).string().c_str());
            return 1;
        }
    }
    std::printf("%zu cases, %zu functions\n", g_cases.size(), program.functions.size());
    return 0;
}
