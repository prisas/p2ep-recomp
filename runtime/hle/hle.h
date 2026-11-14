#pragma once

// Conventions for high-level implementations of PSP system calls.
//
// Every imported function `sceFoo` is implemented as `void hle_sceFoo(Cpu* c, u8* m)`. Arguments
// arrive in $a0-$a3 then $t0-$t3; a 64-bit argument occupies an even-aligned register pair. The
// result goes to $v0 (and $v1 for 64-bit results).

#include "lib/common/log.h"
#include "runtime/cpu/cpu.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"

#define HLE(name) void hle_##name(::p2::Cpu* c, [[maybe_unused]] u8* m)

#define ARG(n) (c->r[4 + (n)])
#define ARG64(n) (u64(c->r[4 + (n)]) | (u64(c->r[5 + (n)]) << 32))

#define RET(value)                                                                                           \
    do {                                                                                                     \
        c->r[2] = u32(value);                                                                                \
        return;                                                                                              \
    } while (0)

#define RET64(value)                                                                                         \
    do {                                                                                                     \
        const u64 p2_ret_ = u64(value);                                                                      \
        c->r[2] = u32(p2_ret_);                                                                              \
        c->r[3] = u32(p2_ret_ >> 32);                                                                        \
        return;                                                                                              \
    } while (0)

// For calls whose behaviour is not implemented yet: logs once per call site and returns `value`.
#define HLE_STUB(name, value)                                                                                \
    HLE(name) {                                                                                              \
        static bool p2_warned_ = false;                                                                      \
        if (!p2_warned_) {                                                                                   \
            p2_warned_ = true;                                                                               \
            LOG_WARN("hle", "unimplemented: " #name "(%08x, %08x, %08x, %08x)", ARG(0), ARG(1), ARG(2),      \
                     ARG(3));                                                                                \
        }                                                                                                    \
        RET(value);                                                                                          \
    }

namespace p2::hle {

// Common error codes outside the kernel range.
enum : u32 {
    kErrNoEnt = 0x80010002,
    kErrBadF = 0x80010009,
    kErrExist = 0x80010011,
    kErrNoDev = 0x80010013,
    kErrInval = 0x80010016,
    kErrMFile = 0x80010018,
    kErrAsyncBusy = 0x80020329,
    kErrNoAsync = 0x8002032A,
    kErrUnsupported = 0x80020325,
};

// How many movies the game has shut down so far, whether they played to the end or were skipped.
u32 movies_finished();

} // namespace p2::hle
