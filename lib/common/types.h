#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

#define P2_INLINE __attribute__((always_inline)) inline
#define P2_LIKELY(x) __builtin_expect(!!(x), 1)
#define P2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define P2_RESTRICT __restrict

namespace p2 {

// Unaligned little-endian access. The host is little-endian, as is the guest.
template <class T>
P2_INLINE T load_le(const void* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
P2_INLINE void store_le(void* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

P2_INLINE u32 load_be32(const void* p) { return __builtin_bswap32(load_le<u32>(p)); }
P2_INLINE u16 load_be16(const void* p) { return __builtin_bswap16(load_le<u16>(p)); }

template <class T>
constexpr T align_up(T v, T a) {
    return (v + (a - 1)) & ~(a - 1);
}

template <class T>
constexpr T align_down(T v, T a) {
    return v & ~(a - 1);
}

} // namespace p2
