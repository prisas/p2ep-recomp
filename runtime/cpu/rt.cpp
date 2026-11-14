// Runtime services called directly by the recompiled code.

#include "runtime/cpu/rt.h"

#include "lib/common/log.h"
#include "runtime/cpu/recomp.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"

namespace p2 {

bool g_hle_trace = false;

void init_dispatch() {
    for (u32 i = 0; i < P2_TEXT_WORDS; ++i) g_dispatch[i] = rt_dispatch_miss;
    for (u32 i = 0; i < g_gen_module.entry_count; ++i) {
        const GenEntry& e = g_gen_module.entries[i];
        g_dispatch[(e.addr - P2_TEXT_BASE) / 4] = e.fn;
    }
    refresh_hle_trace();
}

void refresh_hle_trace() { g_hle_trace = log::channel_enabled("hle"); }

void rt_slice(Cpu* c, u8*) { kernel::on_slice_end(c); }

void rt_dispatch_miss(Cpu* c, u8*) {
    P2_FATAL("indirect jump to %08x, which is not a known entry point (ra %08x). If this is game code, add the "
             "address to data/extra_entries.txt and rebuild.",
             c->pc, c->r[kRegRa]);
}

void rt_trap(Cpu* c, u8*, u32 pc, u32 word) {
    P2_FATAL("reached untranslatable code at %08x (word %08x, ra %08x)", pc, word, c->r[kRegRa]);
}

void rt_break(Cpu* c, u8*, u32 pc, u32 code) {
    static u32 reported = 0;
    if (reported < 16) {
        ++reported;
        LOG_WARN("cpu", "break %x at %08x (ra %08x); usually a division by zero", code, pc, c->r[kRegRa]);
    }
}

void rt_syscall(Cpu* c, u8*, u32 pc, u32 code) {
    P2_FATAL("raw syscall %x at %08x (ra %08x)", code, pc, c->r[kRegRa]);
}

void rt_bad_return(Cpu* c, u8*, u32 expected) {
    static u32 reported = 0;
    if (reported < 32) {
        ++reported;
        LOG_WARN("cpu", "a call expected to return to %08x came back with ra %08x", expected, c->r[kRegRa]);
    }
}

void rt_hle_enter(Cpu* c, u8*, u32 index) {
    const GenImport& import = g_gen_module.imports[index];
    kernel::Thread* t = kernel::current_thread();
    log::write(log::Level::Info, "hle", "[%d] %s(%08x, %08x, %08x, %08x) ra %08x", t ? t->uid : 0, import.name,
               c->r[4], c->r[5], c->r[6], c->r[7], c->r[kRegRa]);
}

void rt_hle_leave(Cpu* c, u8*, u32 index) {
    const GenImport& import = g_gen_module.imports[index];
    kernel::Thread* t = kernel::current_thread();
    log::write(log::Level::Info, "hle", "[%d] %s -> %08x", t ? t->uid : 0, import.name, c->r[2]);
}

} // namespace p2
