#include "lib/disc/iso9660.h"
#include "tests/test.h"

#include <string>
#include <vector>

using namespace p2;
using namespace p2::disc;

namespace {

class MemoryImage final : public Image {
public:
    explicit MemoryImage(size_t sectors) : data_(sectors * kSectorSize, 0) {}
    u64 size() const override { return data_.size(); }
    const char* format_name() const override { return "memory"; }
    bool read(u64 offset, void* dst, size_t size) override {
        if (offset > data_.size() || size > data_.size() - offset) return false;
        std::memcpy(dst, data_.data() + offset, size);
        return true;
    }
    u8* sector(u32 lba) { return data_.data() + size_t(lba) * kSectorSize; }

private:
    std::vector<u8> data_;
};

// Appends one ISO9660 directory record and returns its length.
size_t add_record(u8* dir, size_t pos, const std::string& name, u32 lba, u32 size, bool is_dir) {
    size_t length = (33 + name.size() + 1) & ~size_t(1);
    u8* r = dir + pos;
    r[0] = u8(length);
    store_le<u32>(r + 2, lba);
    store_le<u32>(r + 10, size);
    r[25] = is_dir ? 2 : 0;
    r[32] = u8(name.size());
    std::memcpy(r + 33, name.data(), name.size());
    return length;
}

MemoryImage make_iso() {
    MemoryImage image(40);
    u8* pvd = image.sector(16);
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    std::memset(pvd + 40, ' ', 32);
    std::memcpy(pvd + 40, "TESTVOL", 7);
    store_le<u32>(pvd + 158, 20);          // root directory extent
    store_le<u32>(pvd + 166, kSectorSize); // root directory size

    u8* root = image.sector(20);
    size_t pos = 0;
    pos += add_record(root, pos, std::string(1, '\0'), 20, kSectorSize, true);
    pos += add_record(root, pos, std::string(1, '\1'), 20, kSectorSize, true);
    pos += add_record(root, pos, "PSP_GAME", 21, kSectorSize * 2, true);
    pos += add_record(root, pos, "UMD_DATA.BIN;1", 30, 5, false);

    // A two-sector directory: the second record starts in the second sector after zero padding.
    u8* game = image.sector(21);
    pos = 0;
    pos += add_record(game, pos, std::string(1, '\0'), 21, kSectorSize * 2, true);
    pos += add_record(game, pos, std::string(1, '\1'), 20, kSectorSize, true);
    pos += add_record(game, pos, "PARAM.SFO;1", 31, 3000, false);
    add_record(game, kSectorSize, "ICON0.PNG;1", 33, 7, false);

    std::memcpy(image.sector(30), "hello", 5);
    for (u32 i = 0; i < 3000; ++i) image.sector(31)[i] = u8(i * 7);
    std::memcpy(image.sector(33), "pngdata", 7);
    return image;
}

} // namespace

TEST(iso_directory_tree) {
    MemoryImage image = make_iso();
    IsoFs fs;
    std::string error;
    CHECK(fs.mount(&image, &error));
    CHECK_EQ(fs.volume_id(), std::string("TESTVOL"));

    const IsoEntry* umd = fs.find("UMD_DATA.BIN");
    CHECK(umd != nullptr);
    if (umd) {
        CHECK_EQ(umd->size, 5u);
        CHECK(!umd->is_dir);
    }
    CHECK(fs.find("psp_game/param.sfo") != nullptr);  // case-insensitive
    CHECK(fs.find("/PSP_GAME\\PARAM.SFO") != nullptr); // leading slash, backslash
    CHECK(fs.find("PSP_GAME/ICON0.PNG") != nullptr);   // record in the directory's second sector
    CHECK(fs.find("PSP_GAME/MISSING") == nullptr);
    CHECK(fs.find("") != nullptr);

    auto root = fs.list("");
    CHECK_EQ(root.size(), size_t(2));
    auto game = fs.list("PSP_GAME");
    CHECK_EQ(game.size(), size_t(2));
    CHECK(fs.list("UMD_DATA.BIN").empty());
}

TEST(iso_file_reads) {
    MemoryImage image = make_iso();
    IsoFs fs;
    std::string error;
    CHECK(fs.mount(&image, &error));

    std::vector<u8> data;
    CHECK(fs.read_all("UMD_DATA.BIN", data));
    CHECK_EQ(std::string(data.begin(), data.end()), std::string("hello"));

    CHECK(fs.read_all("PSP_GAME/PARAM.SFO", data));
    CHECK_EQ(data.size(), size_t(3000));
    bool ok = true;
    for (u32 i = 0; i < 3000; ++i) ok = ok && data[i] == u8(i * 7);
    CHECK(ok);

    const IsoEntry* sfo = fs.find("PSP_GAME/PARAM.SFO");
    u8 part[4];
    CHECK(sfo && fs.read(*sfo, 2996, part, 4));
    CHECK(sfo && !fs.read(*sfo, 2997, part, 4)); // past the end of the file
    CHECK(!fs.read_all("PSP_GAME", data));       // a directory is not a file
}

TEST(iso_rejects_non_iso) {
    MemoryImage image(40);
    IsoFs fs;
    std::string error;
    CHECK(!fs.mount(&image, &error));
    CHECK(!error.empty());
}
