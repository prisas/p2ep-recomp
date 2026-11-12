// Executes the synthetic guest program produced by recomp_testgen and compares every result with
// the reference values computed when the program was generated.

#include "gen_funcs.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <windows.h>

using namespace p2;

struct TestCase {
    const char* name;
    u32 entry;
    u32 args[4];
    u32 v0;
    u32 v1;
};

#include "gen_cases.inc"

namespace {
int g_runtime_errors = 0;
u64 g_slices = 0;
} // namespace

// --- the few runtime services the generated code needs -------------------------------------------------------

namespace p2 {

bool g_hle_trace = false;

void rt_slice(Cpu* c, u8*) {
    ++g_slices;
    c->downcount = 64; // small on purpose, so loops cross many slice boundaries
}

void rt_dispatch_miss(Cpu* c, u8*) {
    std::printf("    dispatch miss at %08X\n", c->pc);
    ++g_runtime_errors;
}

void rt_trap(Cpu*, u8*, u32 pc, u32 word) {
    std::printf("    trap at %08X (%08X)\n", pc, word);
    std::exit(1);
}

void rt_break(Cpu*, u8*, u32 pc, u32) {
    std::printf("    break at %08X\n", pc);
    ++g_runtime_errors;
}

void rt_syscall(Cpu*, u8*, u32 pc, u32) {
    std::printf("    syscall at %08X\n", pc);
    ++g_runtime_errors;
}

void rt_bad_return(Cpu* c, u8*, u32 expected) {
    std::printf("    callee returned with ra=%08X, expected %08X\n", c->r[31], expected);
    ++g_runtime_errors;
}

void rt_hle_enter(Cpu*, u8*, u32) {}
void rt_hle_leave(Cpu*, u8*, u32) {}

} // namespace p2

void hle_testImport(Cpu* c, u8*) { c->r[2] = c->r[4] * 1000 + c->r[5]; }

int main(int argc, char**) {
    const bool verbose = argc > 1; // any argument: name each case before it runs
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Reserve the whole 32-bit guest space and commit only the program image and a stack.
    u8* m = static_cast<u8*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
    const u32 stack_top = 0x09000000;
    if (!m || !VirtualAlloc(m + kImageBase, sizeof(kImage), MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(m + stack_top - 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
        std::printf("cannot reserve guest memory\n");
        return 1;
    }
    std::memcpy(m + kImageBase, kImage, sizeof(kImage));

    for (u32 i = 0; i < P2_TEXT_WORDS; ++i) g_dispatch[i] = rt_dispatch_miss;
    for (u32 i = 0; i < g_gen_module.entry_count; ++i) {
        const GenEntry& e = g_gen_module.entries[i];
        g_dispatch[(e.addr - P2_TEXT_BASE) / 4] = e.fn;
    }

    std::vector<u32> shadow(4096);
    int failed = 0, ran = 0;
    for (const TestCase& t : kCases) {
        Cpu cpu;
        cpu.reset();
        cpu.shadow_base = cpu.shadow_sp = shadow.data();
        cpu.downcount = 64;
        cpu.r[kRegSp] = stack_top - 64;
        cpu.r[kRegRa] = 0xDEAD0000;
        for (int k = 0; k < 4; ++k) cpu.r[kRegA0 + k] = t.args[k];
        // Callee-saved registers must survive.
        for (u32 k = 16; k < 24; ++k) cpu.r[k] = 0x5A5A0000 + k;

        if (verbose) std::printf("[run] %s at %08X\n", t.name, t.entry);
        g_runtime_errors = 0;
        cpu.pc = t.entry;
        lookup_guest_fn(t.entry)(&cpu, m);
        ++ran;

        bool ok = g_runtime_errors == 0 && cpu.r[kRegV0] == t.v0 && cpu.r[kRegV1] == t.v1 &&
                  cpu.r[kRegSp] == stack_top - 64 && cpu.shadow_sp == shadow.data() && cpu.sec_entry == 0;
        for (u32 k = 16; k < 24; ++k) ok = ok && cpu.r[k] == 0x5A5A0000 + k;
        if (!ok) {
            ++failed;
            std::printf("[FAIL] %s(%08X, %08X, %08X, %08X): v0=%08X v1=%08X, expected v0=%08X v1=%08X\n", t.name,
                        t.args[0], t.args[1], t.args[2], t.args[3], cpu.r[kRegV0], cpu.r[kRegV1], t.v0, t.v1);
        }
    }
    if (g_slices == 0) {
        std::printf("[FAIL] the time-slice poll never fired\n");
        ++failed;
    }
    std::printf("%d recompiled case(s), %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
