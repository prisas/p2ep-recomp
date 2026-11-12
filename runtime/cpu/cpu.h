#pragma once

#include "lib/common/types.h"

namespace p2 {

// VFPU control register indices.
enum : u32 {
    kVfpuSpfx = 0, // source prefix
    kVfpuTpfx = 1, // target prefix
    kVfpuDpfx = 2, // destination prefix
    kVfpuCc = 3,   // condition codes
    kVfpuInf4 = 4,
    kVfpuRsv5 = 5,
    kVfpuRsv6 = 6,
    kVfpuRev = 7,
    kVfpuRcx0 = 8, // random number state, 8 words
    kVfpuCtrlCount = 16,
};

constexpr u32 kVfpuPrefixIdentity = 0xE4;

// Architectural state of one guest thread. Recompiled code reads and writes it directly.
struct Cpu {
    u32 r[32];
    u32 hi;
    u32 lo;
    union {
        f32 f[32];
        u32 fi[32];
    };
    u32 fcr31; // FPU control/status, without the condition bit
    u32 fcc;   // FPU condition bit (0 or 1)

    s32 downcount; // instructions left in the current time slice
    u32 pc;        // target of the latest indirect transfer; diagnostic otherwise
    // Set by a secondary-entry thunk for the function it jumps into, which clears it at once.
    u32 sec_entry;
    u32 reserved0;

    union {
        f32 v[128]; // VFPU registers, indexed by matrix * 4 + column + row * 32
        u32 vi[128];
    };
    u32 vctrl[kVfpuCtrlCount];

    // Guest return addresses of the active calls (developer builds only).
    u32* shadow_sp;
    u32* shadow_base;

    void* thread; // owning kernel thread

    void reset() {
        std::memset(static_cast<void*>(this), 0, sizeof(*this));
        fcr31 = 0x00000E00;
        vctrl[kVfpuSpfx] = kVfpuPrefixIdentity;
        vctrl[kVfpuTpfx] = kVfpuPrefixIdentity;
        vctrl[kVfpuCc] = 0x3F;
        vctrl[kVfpuRev] = 0x7772CEAB;
        vctrl[kVfpuRcx0 + 0] = 0x3F800001;
        vctrl[kVfpuRcx0 + 1] = 0x3F800002;
        vctrl[kVfpuRcx0 + 2] = 0x3F800004;
        vctrl[kVfpuRcx0 + 3] = 0x3F800008;
        for (u32 i = 4; i < 8; ++i) vctrl[kVfpuRcx0 + i] = 0x3F800000;
    }
};

// Register numbers used by the calling convention.
enum : u32 {
    kRegV0 = 2, kRegV1 = 3,
    kRegA0 = 4, kRegA1 = 5, kRegA2 = 6, kRegA3 = 7,
    kRegT0 = 8, kRegT1 = 9, kRegT2 = 10, kRegT3 = 11,
    kRegK0 = 26, kRegK1 = 27,
    kRegGp = 28, kRegSp = 29, kRegFp = 30, kRegRa = 31,
};

using GuestFn = void (*)(Cpu*, u8*);

} // namespace p2
