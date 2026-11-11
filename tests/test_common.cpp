#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "tests/test.h"

#include <string>

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
