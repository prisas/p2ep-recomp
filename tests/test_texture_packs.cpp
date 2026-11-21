// Community texture packs in PPSSPP's format (runtime/gfx/ppsspp_pack) and the upscaled sets
// (runtime/gfx/texture_pack): parsing, key computation and the choice between packs.

#include "runtime/gfx/ppsspp_pack.h"
#include "runtime/gfx/texture_pack.h"
#include "tests/test.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace p2;
using namespace p2::gfx;

namespace {

const char* kIni = "# comment\n"
                   "[options]\n"
                   "version = 1\n"
                   "hash = xxh64\n"
                   "ignoreAddress = true\n"
                   "reduceHash = True\n"
                   "[hashes]\n"
                   "0000000071a899ada0dcea1c = Cards/BlueCardBack.png\n"
                   "00000000d2744c7b89e1b31e = Cards\\RedCardBack.png\n"
                   "00000000d2744c7b89e1b31f_1 = Cards/Mip.png\n"
                   "00000000aaaaaaaa00000000 = \n"
                   "[hashranges]\n"
                   "0x08a00000,512,512 = 480,272\n"
                   "[reducehashranges]\n"
                   "256,256 = 0.25\n";

} // namespace

TEST(ppsspp_ini_parse) {
    ppsspp::Options o;
    std::vector<std::pair<ppsspp::Key, std::string>> hashes;
    std::string error;
    CHECK(ppsspp::parse_ini(kIni, &o, &hashes, &error));
    CHECK(o.hash == ppsspp::Hash::Xxh64);
    CHECK(o.ignore_address);
    CHECK(o.reduce_hash);
    CHECK_EQ(hashes.size(), size_t(3)); // the level-1 file is left out
    CHECK_EQ(hashes[0].first.cache_key, 0x71a899adull);
    CHECK_EQ(hashes[0].first.hash, 0xa0dcea1cu);
    CHECK_EQ(hashes[1].second, std::string("Cards/RedCardBack.png"));
    CHECK(hashes[2].second.empty());
    CHECK_EQ(o.hash_ranges.size(), size_t(1));
    CHECK(o.hash_ranges.count(u64(0x08a00000) << 32 | u64(512) << 16 | 512));
    CHECK(o.reduce_ranges.at(u64(256) << 16 | 256) == 0.25f);

    ppsspp::Options none;
    CHECK(!ppsspp::parse_ini("[options]\nversion = 1\n", &none, &hashes, &error)); // no hash setting
    CHECK(!ppsspp::parse_ini("[options]\nhash = quick\n", &none, &hashes, &error));
}

TEST(ppsspp_key_names) {
    ppsspp::Key k{0x0000000071a899adull, 0xa0dcea1c};
    CHECK_EQ(ppsspp::key_name(k), std::string("0000000071a899ada0dcea1c"));
    ppsspp::Key back;
    int level = -1;
    CHECK(ppsspp::parse_key("0000000071A899ADA0DCEA1C_2", &back, &level));
    CHECK(back == k);
    CHECK_EQ(level, 2);
    CHECK(!ppsspp::parse_key("0000000071a899ad", &back));
    CHECK(!ppsspp::parse_key("0000000071a899adzzdcea1c", &back));
}

TEST(ppsspp_cache_key) {
    std::vector<u8> memory(64 * 64 * 2, 0x5A);
    ppsspp::TextureInfo t;
    t.addr = 0x48a01230; // an uncached mirror: the top bits are dropped
    t.format = 1;        // 5551
    t.width = 64;
    t.height = 32;
    t.buf_width = 64;
    t.data = memory.data();
    t.data_size = memory.size();
    ppsspp::Options o;
    o.hash = ppsspp::Hash::Xxh32;
    CHECK_EQ(ppsspp::cache_key(o, t), (u64(0x08a01230) << 32) | 0x0506u);
    o.ignore_address = true;
    CHECK_EQ(ppsspp::cache_key(o, t), u64(0x0506));

    // Indexed: the palette hash is mixed into the low half and depends on palette and format register.
    u8 palette[64] = {};
    for (u32 i = 0; i < 64; ++i) palette[i] = u8(i * 3);
    t.format = 5; // CLUT8
    t.clut = palette;
    t.clut_bytes = 32;
    t.clut_format_reg = 0xFF03;
    const u64 a = ppsspp::cache_key(o, t);
    CHECK((a >> 32) == 0);
    CHECK(a != 0x0506u);
    t.clut_format_reg = 0xFF02;
    CHECK(ppsspp::cache_key(o, t) != a);
    t.clut_format_reg = 0xFF03;
    palette[0] ^= 1;
    CHECK(ppsspp::cache_key(o, t) != a);
}

TEST(ppsspp_data_hash) {
    std::vector<u8> memory(1024 * 512 * 2);
    for (size_t i = 0; i < memory.size(); ++i) memory[i] = u8(i * 7 + (i >> 9));
    ppsspp::TextureInfo t;
    t.format = 1; // 16 bits per pixel
    t.width = 64;
    t.height = 64;
    t.buf_width = 64;
    t.data = memory.data();
    t.data_size = memory.size();
    ppsspp::Options o;
    o.hash = ppsspp::Hash::Xxh64;
    const u32 full = ppsspp::data_hash(o, t, 64);
    CHECK(full != 0);
    // The hashed range: only the texture's bytes count.
    memory[64 * 64 * 2] ^= 0xFF;
    CHECK_EQ(ppsspp::data_hash(o, t, 64), full);
    memory[64 * 64 * 2 - 1] ^= 0xFF;
    CHECK(ppsspp::data_hash(o, t, 64) != full);
    memory[64 * 64 * 2 - 1] ^= 0xFF;
    // reduceHash: half the bytes by default, so the second half no longer counts.
    o.reduce_hash = true;
    const u32 half = ppsspp::data_hash(o, t, 64);
    CHECK(half != full);
    memory[64 * 64 * 2 - 1] ^= 0xFF;
    CHECK_EQ(ppsspp::data_hash(o, t, 64), half);
    o.reduce_hash = false;
    // Rows with gaps (buffer wider than the texture): a row's tail beyond the width does not count.
    t.buf_width = 128;
    const u32 rows = ppsspp::data_hash(o, t, 64);
    memory[64 * 2 + 5] ^= 0xFF; // first row, past the 64 pixels
    CHECK_EQ(ppsspp::data_hash(o, t, 64), rows);
    memory[5] ^= 0xFF;
    CHECK(ppsspp::data_hash(o, t, 64) != rows);
    // Past readable memory: 0, as the emulator gives.
    t.data_size = 100;
    CHECK_EQ(ppsspp::data_hash(o, t, 64), 0u);
}

TEST(ppsspp_tall_hashes_match_single_heights) {
    std::vector<u8> memory(512 * 512 * 2);
    for (size_t i = 0; i < memory.size(); ++i) memory[i] = u8(i * 13 + (i >> 11));
    ppsspp::TextureInfo t;
    t.format = 1;
    t.width = 512;
    t.height = 512;
    t.data = memory.data();
    t.data_size = memory.size();
    for (u32 bufw : {512u, 1024u}) {
        t.buf_width = bufw & 0x7FF;
        if (bufw == 1024) t.width = 256; // gaps: the buffer is wider than the texture
        for (ppsspp::Hash kind : {ppsspp::Hash::Xxh32, ppsspp::Hash::Xxh64}) {
            ppsspp::Options o;
            o.hash = kind;
            std::vector<u32> tall;
            ppsspp::tall_data_hashes(o, t, &tall);
            CHECK_EQ(tall.size(), size_t(241));
            for (u32 h : {272u, 300u, 511u, 512u}) CHECK_EQ(tall[h - 272], ppsspp::data_hash(o, t, h));
        }
    }
}

TEST(ppsspp_png_size_and_choice) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "p2_test_packs";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // A PNG header is enough for png_size.
    auto png = [&](const fs::path& file, u32 w, u32 h) {
        u8 head[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        for (int i = 0; i < 4; ++i) {
            head[16 + i] = u8(w >> (24 - 8 * i));
            head[20 + i] = u8(h >> (24 - 8 * i));
        }
        FILE* f = nullptr;
        _wfopen_s(&f, file.c_str(), L"wb");
        fwrite(head, 1, sizeof(head), f);
        fclose(f);
    };
    png(dir / "a.png", 320, 320);
    png(dir / "b.png", 640, 640);
    png(dir / "c.png", 320, 320);
    CHECK(ppsspp::png_size(dir / "b.png") == std::make_pair(640u, 640u));
    CHECK(ppsspp::png_size(dir / "missing.png") == std::make_pair(0u, 0u));

    // Three packs with the same texture: the largest picture wins, then the lowest order.
    std::vector<u8> memory(32 * 32 * 2, 9);
    ppsspp::TextureInfo t;
    t.format = 1;
    t.width = 32;
    t.height = 32;
    t.buf_width = 32;
    t.data = memory.data();
    t.data_size = memory.size();
    ppsspp::Options o;
    o.ignore_address = true;
    const ppsspp::Key key{ppsspp::cache_key(o, t), ppsspp::data_hash(o, t, 32)};
    std::vector<ppsspp::Pack> packs(3);
    const fs::path files[3] = {dir / "a.png", dir / "b.png", dir / "c.png"};
    const u32 orders[3] = {1, 3, 2};
    for (int i = 0; i < 3; ++i) {
        packs[i].options = o;
        packs[i].order = orders[i];
        packs[i].files[key] = files[i];
        packs[i].key_words.insert(u32(key.cache_key));
    }
    auto size_of = [](const fs::path& f) { return ppsspp::png_size(f); };
    auto m = ppsspp::find(packs, t, size_of);
    CHECK(m && *m->file == files[1]); // 640x640
    packs[1].files.clear();
    packs[1].key_words.clear();
    m = ppsspp::find(packs, t, size_of);
    CHECK(m && *m->file == files[0]); // a tie at 320x320: order 1 beats order 2
    memory[0] = 1;                    // different data: no file
    CHECK(!ppsspp::find(packs, t, size_of));
    fs::remove_all(dir, ec);
}

TEST(upscaled_sets_by_folder) {
    CHECK_EQ(textures::upscaler_of("0123456789abcdef.png"), std::string("xbr")); // the layout before methods
    CHECK_EQ(textures::upscaler_of("xbr/0123456789abcdef.png"), std::string("xbr"));
    CHECK_EQ(textures::upscaler_of("realesrgan/0123456789abcdef.png"), std::string("realesrgan"));
    CHECK_EQ(textures::upscaler_of("RealESRGAN/0123456789abcdef.png"), std::string("realesrgan"));
    CHECK_EQ(textures::upscaler_of("other/0123456789abcdef.png"), std::string("other"));
}
