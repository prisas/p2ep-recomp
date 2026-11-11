#pragma once

#include "lib/common/types.h"

#include <array>
#include <string>

namespace p2 {

std::array<u8, 20> sha1(const void* data, size_t size);
std::array<u8, 32> sha256(const void* data, size_t size);

std::string to_hex(const u8* bytes, size_t size);

template <size_t N>
std::string to_hex(const std::array<u8, N>& bytes) {
    return to_hex(bytes.data(), N);
}

// 64-bit FNV-1a. Used for cache keys where cryptographic strength is irrelevant.
constexpr u64 fnv1a64(const void* data, size_t size, u64 seed = 0xcbf29ce484222325ull) {
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace p2
