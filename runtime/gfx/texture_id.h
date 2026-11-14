#pragma once

#include "lib/common/hash.h"

// The ID of a decoded texture: a hash of its pixels mixed with its size.
//
// It names the files of texture packs (<id>.png), so it must give the same result on every machine
// and must never change: tests/test_common.cpp pins its value. hash_bytes_scalar is used because
// the faster hash_bytes picks a processor-dependent implementation.
namespace p2::gfx::textures {

inline u64 texture_id(const u32* rgba, u32 width, u32 height) {
    u64 h = hash_bytes_scalar(rgba, size_t(width) * height * 4);
    h ^= (u64(width) << 32 | height) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return h;
}

} // namespace p2::gfx::textures
