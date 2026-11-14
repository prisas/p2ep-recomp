#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "tests/test.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace p2;

TEST(sha1_known_vectors) {
    CHECK_EQ(to_hex(sha1("", 0)), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(to_hex(sha1("abc", 3)), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(to_hex(sha1(two_blocks.data(), two_blocks.size())),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

TEST(sha256_known_vectors) {
    CHECK_EQ(to_hex(sha256("", 0)),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(to_hex(sha256("abc", 3)),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(to_hex(sha256(two_blocks.data(), two_blocks.size())),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(sha_padding_boundaries) {
    // Lengths around the 55/56/64 byte padding edges must not crash and must differ from each other.
    std::string previous;
    for (size_t n = 50; n <= 70; ++n) {
        std::string data(n, 'x');
        std::string digest = to_hex(sha256(data.data(), data.size()));
        CHECK(digest != previous);
        previous = digest;
    }
}

TEST(format_helpers) {
    CHECK_EQ(format("%s-%04x", "a", 0x2a), std::string("a-002a"));
    std::string out = "x";
    append_format(out, "%d", 12345);
    CHECK_EQ(out, std::string("x12345"));
    std::string longer(1000, 'y');
    out.clear();
    append_format(out, "%s!", longer.c_str());
    CHECK_EQ(out.size(), size_t(1001));
}

namespace {

// Checks that an implementation of the change-detection hash notices the kinds of edits textures see.
void check_change_detection(u64 (*hash)(const void*, size_t)) {
    std::vector<u8> data(40000);
    u32 seed = 12345;
    for (u8& b : data) {
        seed = seed * 1664525u + 1013904223u;
        b = u8(seed >> 24);
    }
    for (size_t size : {size_t(0), size_t(1), size_t(7), size_t(31), size_t(32), size_t(63), size_t(64), size_t(65), size_t(1023),
                        size_t(1024), size_t(4096), size_t(16384), size_t(40000)}) {
        const u64 base = hash(data.data(), size);
        CHECK_EQ(hash(data.data(), size), base); // repeatable
        if (size == 0) continue;
        if (size < data.size()) CHECK(hash(data.data(), size + 1) != base); // length matters
        // Any single changed byte shows, wherever it is.
        const size_t step = size > 2000 ? 97 : 1;
        bool all_differ = true;
        for (size_t i = 0; i < size; i += step) {
            data[i] ^= 0x01;
            if (hash(data.data(), size) == base) all_differ = false;
            data[i] ^= 0x81;
            if (hash(data.data(), size) == base) all_differ = false;
            data[i] ^= 0x80;
        }
        CHECK(all_differ);
        CHECK_EQ(hash(data.data(), size), base);
    }
    // Moving data around shows too: two equal-sized blocks exchanged, at aligned and unaligned offsets.
    const u64 base = hash(data.data(), data.size());
    for (size_t a : {size_t(0), size_t(64), size_t(1024), size_t(8)}) {
        for (size_t b : {size_t(4096), size_t(4160), size_t(20000), size_t(32768 + 8)}) {
            for (size_t n : {size_t(8), size_t(64), size_t(256)}) {
                std::swap_ranges(data.begin() + ptrdiff_t(a), data.begin() + ptrdiff_t(a + n), data.begin() + ptrdiff_t(b));
                CHECK(hash(data.data(), data.size()) != base);
                std::swap_ranges(data.begin() + ptrdiff_t(a), data.begin() + ptrdiff_t(a + n), data.begin() + ptrdiff_t(b));
            }
        }
    }
    // An all-zero buffer and one with a single late bit set differ, as do buffers of zeros of different lengths.
    std::vector<u8> zeros(8192, 0);
    const u64 zero_hash = hash(zeros.data(), zeros.size());
    zeros[8191] = 1;
    CHECK(hash(zeros.data(), zeros.size()) != zero_hash);
    zeros[8191] = 0;
    CHECK(hash(zeros.data(), 4096) != zero_hash);
}

} // namespace

TEST(hash_bytes_detects_changes_scalar) { check_change_detection(hash_bytes_scalar); }

TEST(hash_bytes_detects_changes_avx2) {
    if (cpu_has_avx2()) check_change_detection(hash_bytes_avx2);
}

TEST(hash_bytes_dispatch) { check_change_detection(hash_bytes); }
