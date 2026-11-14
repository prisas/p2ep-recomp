#include "lib/common/hash.h"

#include <bit>
#include <cstring>

#include <immintrin.h>
#include <intrin.h>

namespace p2 {

namespace {

u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]); }

void put_be32(u8* p, u32 v) {
    p[0] = u8(v >> 24);
    p[1] = u8(v >> 16);
    p[2] = u8(v >> 8);
    p[3] = u8(v);
}

// Feeds `data` through a 64-byte block compression function with Merkle-Damgard padding (big-endian length).
template <class Compress>
void md_process(const u8* data, size_t size, Compress&& compress) {
    size_t full = size / 64;
    for (size_t i = 0; i < full; ++i) compress(data + i * 64);

    u8 tail[128] = {};
    size_t rem = size - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem + 1 + 8 <= 64 ? 64 : 128;
    u64 bits = u64(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = u8(bits >> (8 * i));
    compress(tail);
    if (tail_len == 128) compress(tail + 64);
}

} // namespace

std::array<u8, 20> sha1(const void* data, size_t size) {
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    md_process(static_cast<const u8*>(data), size, [&](const u8* block) {
        u32 w[80];
        for (int i = 0; i < 16; ++i) w[i] = be32(block + i * 4);
        for (int i = 16; i < 80; ++i) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            u32 t = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    });

    std::array<u8, 20> out;
    for (int i = 0; i < 5; ++i) put_be32(out.data() + i * 4, h[i]);
    return out;
}

std::array<u8, 32> sha256(const void* data, size_t size) {
    static constexpr u32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    md_process(static_cast<const u8*>(data), size, [&](const u8* block) {
        u32 w[64];
        for (int i = 0; i < 16; ++i) w[i] = be32(block + i * 4);
        for (int i = 16; i < 64; ++i) {
            u32 s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            u32 s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            u32 ch = (e & f) ^ (~e & g);
            u32 t1 = hh + s1 + ch + k[i] + w[i];
            u32 s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            u32 maj = (a & b) ^ (a & c) ^ (b & c);
            u32 t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    });

    std::array<u8, 32> out;
    for (int i = 0; i < 8; ++i) put_be32(out.data() + i * 4, h[i]);
    return out;
}

// --- fast change-detection hash ---------------------------------------------------------------------------------

namespace {

constexpr u64 kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
              kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;

inline u64 avalanche(u64 h) {
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

inline u64 lane_round(u64 acc, u64 input) { return std::rotl(acc + input * kP2, 31) * kP1; }

} // namespace

// Four independent lanes, so consecutive words do not wait for one another's multiplications.
u64 hash_bytes_scalar(const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + size;
    u64 h = kP5;
    if (size >= 32) {
        u64 a = kP1 + kP2, b = kP2, c = 0, d = 0 - kP1;
        do {
            a = lane_round(a, load_le<u64>(p));
            b = lane_round(b, load_le<u64>(p + 8));
            c = lane_round(c, load_le<u64>(p + 16));
            d = lane_round(d, load_le<u64>(p + 24));
            p += 32;
        } while (p + 32 <= end);
        h = std::rotl(a, 1) + std::rotl(b, 7) + std::rotl(c, 12) + std::rotl(d, 18);
    }
    h += size;
    for (; p + 8 <= end; p += 8) h = std::rotl(h ^ lane_round(0, load_le<u64>(p)), 27) * kP1 + kP4;
    for (; p < end; ++p) h = std::rotl(h ^ (*p * kP5), 11) * kP1;
    return avalanche(h);
}

__attribute__((target("xsave"))) bool cpu_has_avx2() {
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuid(regs, 1);
    const bool os_saves_ymm = (regs[2] & (1 << 27)) && (regs[2] & (1 << 28)) && (_xgetbv(0) & 6) == 6;
    if (!os_saves_ymm) return false;
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
}

// Eight 64-bit accumulators in two vectors. Each 64-byte stripe adds, per lane, the product of the
// two halves of its keyed word and the neighbouring lane's raw word, so any changed word alters a
// lane for certain; the key advances every stripe so that moving data between stripes shows too.
#define P2_AVX2 __attribute__((target("avx2")))

namespace {

struct WideState {
    __m256i acc[2];
    __m256i key[2];
};

P2_AVX2 inline void wide_stripe(WideState& s, const u8* at) {
    const __m256i step = _mm256_set1_epi64x(s64(0x9E3779B97F4A7C15ull));
    for (int i = 0; i < 2; ++i) {
        const __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(at + i * 32));
        const __m256i m = _mm256_xor_si256(d, s.key[i]);
        s.acc[i] = _mm256_add_epi64(s.acc[i], _mm256_shuffle_epi32(d, _MM_SHUFFLE(1, 0, 3, 2)));
        s.acc[i] = _mm256_add_epi64(s.acc[i], _mm256_mul_epu32(m, _mm256_srli_epi64(m, 32)));
        s.key[i] = _mm256_add_epi64(s.key[i], step);
    }
}

// Mixes the accumulators so that long inputs keep using all their bits.
P2_AVX2 inline void wide_scramble(WideState& s) {
    const __m256i prime = _mm256_set1_epi64x(s64(0x9E3779B1ull));
    for (int i = 0; i < 2; ++i) {
        const __m256i x = _mm256_xor_si256(_mm256_xor_si256(s.acc[i], _mm256_srli_epi64(s.acc[i], 47)), s.key[i]);
        const __m256i low = _mm256_mul_epu32(x, prime);
        const __m256i high = _mm256_mul_epu32(_mm256_srli_epi64(x, 32), prime);
        s.acc[i] = _mm256_add_epi64(low, _mm256_slli_epi64(high, 32));
    }
}

} // namespace

P2_AVX2 u64 hash_bytes_avx2(const void* data, size_t size) {
    const u8* p = static_cast<const u8*>(data);
    WideState s;
    s.key[0] = _mm256_set_epi64x(s64(0x1F67B3B7A4A44072ull), s64(0x78E5C0CC4EE679CBull), s64(0x2172FFCC7DD05A82ull),
                                 s64(0xBE4BA423396CFEB8ull));
    s.key[1] = _mm256_set_epi64x(s64(0x4575B9D5D9C94AEDull), s64(0x3A6E0B4D8F0D1B3Dull), s64(0xD8ACDEA946EF1938ull),
                                 s64(0xCB79E64ECCC0E578ull));
    s.acc[0] = _mm256_set_epi64x(s64(kP4), s64(kP3), s64(kP2), s64(kP1));
    s.acc[1] = _mm256_set_epi64x(s64(kP1), s64(kP5), s64(kP3), s64(kP2));

    size_t remaining = size;
    for (u32 n = 0; remaining >= 64; remaining -= 64, p += 64) {
        wide_stripe(s, p);
        if ((++n & 15) == 0) wide_scramble(s);
    }
    if (remaining) {
        u8 tail[64] = {};
        std::memcpy(tail, p, remaining);
        wide_stripe(s, tail);
    }
    wide_scramble(s);

    u64 lanes[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), s.acc[0]);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes + 4), s.acc[1]);
    u64 h = u64(size) * kP1 + kP5;
    for (u64 lane : lanes) h = std::rotl(h ^ avalanche(lane), 27) * kP1 + kP4;
    return avalanche(h);
}

u64 hash_bytes(const void* data, size_t size) {
    static u64 (*const chosen)(const void*, size_t) = cpu_has_avx2() ? hash_bytes_avx2 : hash_bytes_scalar;
    return chosen(data, size);
}

std::string to_hex(const u8* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return out;
}

} // namespace p2
