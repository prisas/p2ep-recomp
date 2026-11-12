#include "runtime/mem/memory.h"

#include <windows.h>

#include <memoryapi.h>

#pragma comment(lib, "mincore.lib")

namespace p2::mem {

u8* g_base = nullptr;

namespace {

constexpr u64 kSpaceSize = (1ull << 32) + 0x10000; // one extra page run so a wide access at the very top stays inside
constexpr u32 kGranularity = 0x10000;

struct View {
    u32 addr;
    u32 size;
    HANDLE section;
};

bool map_view(const View& v) {
    u8* where = g_base + v.addr;
    // Carve a placeholder of exactly this size out of the big reservation, then replace it with the view.
    if (!VirtualFree(where, v.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return false;
    void* mapped = MapViewOfFile3(v.section, nullptr, where, 0, v.size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                  nullptr, 0);
    return mapped == where;
}

} // namespace

bool init(std::string* error) {
    HANDLE ram = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kRamSize, nullptr);
    HANDLE vram = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kVramSize, nullptr);
    HANDLE scratch = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kGranularity, nullptr);
    if (!ram || !vram || !scratch) {
        if (error) *error = "cannot allocate guest memory";
        return false;
    }

    g_base = static_cast<u8*>(VirtualAlloc2(nullptr, nullptr, kSpaceSize, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                            PAGE_NOACCESS, nullptr, 0));
    if (!g_base) {
        if (error) *error = "cannot reserve the 4 GiB guest address space";
        return false;
    }

    for (u32 segment = 0; segment < 4; ++segment) {
        const u32 bits = segment << 30;
        bool ok = map_view({bits | kScratchBase, kGranularity, scratch});
        for (u32 mirror = kVramBase; mirror < kVramMirrorEnd && ok; mirror += kVramSize)
            ok = map_view({bits | mirror, kVramSize, vram});
        ok = ok && map_view({bits | kRamBase, kRamSize, ram});
        if (!ok) {
            if (error) *error = "cannot map guest memory";
            return false;
        }
    }
    return true;
}

bool valid(u32 addr, u32 size) {
    const u32 a = physical(addr);
    const u64 end = u64(a) + size;
    if (a >= kRamBase && end <= u64(kRamBase) + kRamSize) return true;
    if (a >= kVramBase && end <= kVramMirrorEnd) return true;
    if (a >= kScratchBase && end <= u64(kScratchBase) + kScratchSize) return true;
    return false;
}

std::string read_string(u32 addr, u32 max) {
    std::string out;
    while (out.size() < max && valid(addr)) {
        char ch = char(g_base[addr++]);
        if (!ch) break;
        out.push_back(ch);
    }
    return out;
}

void write_string(u32 addr, const std::string& text, u32 capacity) {
    if (capacity == 0 || !valid(addr, capacity)) return;
    u32 n = u32(text.size()) < capacity - 1 ? u32(text.size()) : capacity - 1;
    std::memcpy(g_base + addr, text.data(), n);
    g_base[addr + n] = 0;
}

bool host_to_guest(const void* host, u32* guest) {
    const u8* p = static_cast<const u8*>(host);
    if (!g_base || p < g_base || p >= g_base + kSpaceSize) return false;
    *guest = u32(p - g_base);
    return true;
}

} // namespace p2::mem
