#pragma once

// The interface between recompiled code and the runtime. Generated translation units include this
// header (through gen_funcs.h) and nothing else.

#include "gen_config.h" // generated: P2_TEXT_BASE, P2_TEXT_WORDS

#include "runtime/cpu/vfpu.h"

namespace p2 {

struct GenEntry {
    u32 addr;
    GuestFn fn;
};

struct GenImport {
    const char* lib;
    const char* name;
    u32 nid;
    u32 stub;
    GuestFn fn;
};

// Describes the executable the generated code was translated from.
struct GenModule {
    const char* sha256;
    u32 base;
    u32 end;
    u32 entry;
    u32 gp;
    u32 text_base;
    u32 text_words;
    const GenEntry* entries;
    u32 entry_count;
    const GenImport* imports;
    u32 import_count;
};

extern const GenModule g_gen_module;

// One slot per instruction word of the executable's code; filled from g_gen_module at start-up.
extern GuestFn g_dispatch[P2_TEXT_WORDS];

// --- services the generated code calls ------------------------------------------------------------------------

// The time slice ran out: lets the scheduler deliver events and switch threads.
void rt_slice(Cpu* c, u8* m);
// Indirect transfer to an address that is not a known function entry (c->pc holds the target).
void rt_dispatch_miss(Cpu* c, u8* m);
// Reached code the recompiler could not translate.
[[noreturn]] void rt_trap(Cpu* c, u8* m, u32 pc, u32 word);
void rt_break(Cpu* c, u8* m, u32 pc, u32 code);
void rt_syscall(Cpu* c, u8* m, u32 pc, u32 code);
// A callee came back with an unexpected return address (developer builds).
void rt_bad_return(Cpu* c, u8* m, u32 expected);

extern bool g_hle_trace;
void rt_hle_enter(Cpu* c, u8* m, u32 import_index);
void rt_hle_leave(Cpu* c, u8* m, u32 import_index);

P2_INLINE GuestFn lookup_guest_fn(u32 target) {
    u32 offset = target - P2_TEXT_BASE;
    if (P2_LIKELY(offset < P2_TEXT_WORDS * 4u && !(offset & 3))) return g_dispatch[offset >> 2];
    return rt_dispatch_miss;
}

} // namespace p2

#define P2_FN(name) void name(::p2::Cpu* P2_RESTRICT c, u8* P2_RESTRICT m)

// Time accounting: every block charges its instruction count; loop heads and entries also poll.
#define P2_TICK(n) (c->downcount -= (n))
#define P2_TICK_CHECK(n)                                                                                     \
    do {                                                                                                     \
        if (P2_UNLIKELY((c->downcount -= (n)) < 0)) ::p2::rt_slice(c, m);                                    \
    } while (0)

#if defined(P2EP_DEV)
#define P2_ENTER(ra) (*c->shadow_sp++ = (ra))
#define P2_LEAVE() (--c->shadow_sp)
#define P2_REPORT_BAD_RETURN(ra) ::p2::rt_bad_return(c, m, (ra))
#else
#define P2_ENTER(ra) ((void)0)
#define P2_LEAVE() ((void)0)
#define P2_REPORT_BAD_RETURN(ra) ((void)0)
#endif

// After a call linked through $ra. A callee can leave by jumping into the middle of the function
// that called it instead of returning (routines the executable's patch adds do this). The rest of
// the caller then runs inside the call, up to and including its own return, so the call comes
// back with a return address that is not this call site's and the stack pointer above where it
// was: this function has already returned, and must do so here as well.
#define P2_AFTER_CALL(ra, sp)                                                                                \
    do {                                                                                                     \
        P2_LEAVE();                                                                                          \
        if (P2_UNLIKELY(c->r[31] != (ra))) {                                                                 \
            if (c->r[29] > (sp)) return;                                                                     \
            P2_REPORT_BAD_RETURN(ra);                                                                        \
        }                                                                                                    \
    } while (0)

// Direct call to a known function; `ra` is the guest return address already stored in $ra.
#define P2_CALL(fn, ra)                                                                                      \
    do {                                                                                                     \
        P2_ENTER(ra);                                                                                        \
        const u32 sp_at_call_ = c->r[29];                                                                    \
        fn(c, m);                                                                                            \
        P2_AFTER_CALL(ra, sp_at_call_);                                                                      \
    } while (0)

// Indirect call through the dispatch table. `checked` call sites linked through $ra.
#define P2_CALL_IND(target, ra)                                                                              \
    do {                                                                                                     \
        c->pc = (target);                                                                                    \
        P2_ENTER(ra);                                                                                        \
        const u32 sp_at_call_ = c->r[29];                                                                    \
        ::p2::lookup_guest_fn(c->pc)(c, m);                                                                  \
        P2_AFTER_CALL(ra, sp_at_call_);                                                                      \
    } while (0)

#define P2_CALL_IND_NOCHECK(target, ra)                                                                      \
    do {                                                                                                     \
        c->pc = (target);                                                                                    \
        P2_ENTER(ra);                                                                                        \
        ::p2::lookup_guest_fn(c->pc)(c, m);                                                                  \
        P2_LEAVE();                                                                                          \
    } while (0)

// Transfers that do not return here.
#define P2_TAIL(fn)                                                                                          \
    do {                                                                                                     \
        [[clang::musttail]] return fn(c, m);                                                                 \
    } while (0)

#define P2_JUMP_IND(target)                                                                                  \
    do {                                                                                                     \
        c->pc = (target);                                                                                    \
        [[clang::musttail]] return ::p2::lookup_guest_fn(c->pc)(c, m);                                       \
    } while (0)

// A system call costs about a microsecond on the console. Charging it keeps guest time moving in
// loops that poll the system (waiting for the frame counter to change, for example).
#define P2_SYSCALL_CYCLES 333

#define P2_HLE(index, fn)                                                                                    \
    do {                                                                                                     \
        c->downcount -= P2_SYSCALL_CYCLES;                                                                   \
        if (P2_UNLIKELY(::p2::g_hle_trace)) {                                                                \
            ::p2::rt_hle_enter(c, m, (index));                                                               \
            fn(c, m);                                                                                        \
            ::p2::rt_hle_leave(c, m, (index));                                                               \
        } else {                                                                                             \
            fn(c, m);                                                                                        \
        }                                                                                                    \
    } while (0)
