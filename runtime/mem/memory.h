#pragma once

#include "lib/common/types.h"

#include <string>

// The guest's 32-bit address space. All 4 GiB are reserved on the host so a guest address can be
// used as an index with no masking; RAM, video memory and the scratchpad are mapped at each of the
// mirrors the hardware exposes (cached, uncached and kernel segments), and everything else faults.
namespace p2::mem {

constexpr u32 kRamBase = 0x08000000;
constexpr u32 kRamSize = 0x02000000; // 32 MiB
constexpr u32 kUserBase = 0x08800000;
constexpr u32 kUserEnd = kRamBase + kRamSize;
constexpr u32 kKernelBase = 0x08000000;
constexpr u32 kKernelEnd = 0x08400000; // the part of kernel RAM the runtime hands out
constexpr u32 kVramBase = 0x04000000;
constexpr u32 kVramSize = 0x00200000; // 2 MiB, mirrored four times
constexpr u32 kVramMirrorEnd = 0x04800000;
constexpr u32 kScratchBase = 0x00010000;
constexpr u32 kScratchSize = 0x00004000;

extern u8* g_base;

bool init(std::string* error);

// True when [addr, addr + size) lies entirely inside mapped guest memory.
bool valid(u32 addr, u32 size = 1);

// Strips the segment bits, giving the address inside the 0x00000000-0x3FFFFFFF mirror.
P2_INLINE u32 physical(u32 addr) { return addr & 0x3FFFFFFFu; }

P2_INLINE u8* ptr(u32 addr) { return g_base + addr; }
P2_INLINE u8 r8(u32 a) { return g_base[a]; }
P2_INLINE u16 r16(u32 a) { return load_le<u16>(g_base + a); }
P2_INLINE u32 r32(u32 a) { return load_le<u32>(g_base + a); }
P2_INLINE u64 r64(u32 a) { return load_le<u64>(g_base + a); }
P2_INLINE void w8(u32 a, u8 v) { g_base[a] = v; }
P2_INLINE void w16(u32 a, u16 v) { store_le<u16>(g_base + a, v); }
P2_INLINE void w32(u32 a, u32 v) { store_le<u32>(g_base + a, v); }
P2_INLINE void w64(u32 a, u64 v) { store_le<u64>(g_base + a, v); }

// Reads a NUL-terminated string; stops at unmapped memory or `max` characters.
std::string read_string(u32 addr, u32 max = 1024);
// Copies at most `capacity - 1` characters plus a terminator.
void write_string(u32 addr, const std::string& text, u32 capacity);

// Classifies a host fault address. Returns true and the guest address when it lies in the guest space.
bool host_to_guest(const void* host, u32* guest);

// --- write watch (debugging) ------------------------------------------------------------------------------------
// Makes the pages holding [addr, addr + size) read-only in every mirror, so that writes to them
// fault. The fault handler passes each write fault to `watch_fault`: when it returns true the
// pages are writable again and the faulting instruction must be single-stepped, after which
// `watch_rearm` restores the protection. `*hit` tells whether the write touched the watched bytes.
void watch_writes(u32 addr, u32 size);
void unwatch_writes();
bool watch_fault(const void* host, bool* hit, u32* guest);
bool watch_rearm();

} // namespace p2::mem
