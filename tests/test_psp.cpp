#include "lib/psp/nid.h"
#include "lib/psp/prx.h"
#include "lib/psp/sfo.h"
#include "tests/test.h"

#include <string>
#include <vector>

using namespace p2;
using namespace p2::psp;

namespace {

void put32(std::vector<u8>& v, size_t off, u32 x) { store_le<u32>(v.data() + off, x); }
void put16(std::vector<u8>& v, size_t off, u16 x) { store_le<u16>(v.data() + off, x); }

// A tiny relocatable executable with no section headers: one load segment at file offset 0x100
// and one relocation segment at 0x200. Returns the file image.
std::vector<u8> make_prx() {
    std::vector<u8> f(0x300, 0);
    const u8 ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(f.data(), ident, sizeof(ident));
    put16(f, 16, 0xFFA0); // type: PRX
    put16(f, 18, 8);      // machine: MIPS
    put32(f, 24, 0);      // entry
    put32(f, 28, 52);     // phoff
    put16(f, 42, 32);     // phentsize
    put16(f, 44, 2);      // phnum

    // PT_LOAD: file 0x100, vaddr 0, module info at file offset 0x110
    put32(f, 52 + 0, 1);
    put32(f, 52 + 4, 0x100);
    put32(f, 52 + 8, 0);
    put32(f, 52 + 12, 0x110);
    put32(f, 52 + 16, 0x100);
    put32(f, 52 + 20, 0x180);

    const size_t seg = 0x100;
    put32(f, seg + 0x00, 0x0C000010); // jal 0x40                    R_MIPS_26
    put32(f, seg + 0x04, 0x3C040002); // lui a0, 2                   R_MIPS_HI16   (0x18010)
    put32(f, seg + 0x08, 0x24848010); // addiu a0, a0, -0x7ff0       R_MIPS_LO16
    put32(f, seg + 0x0C, 0x00000040); // .word 0x40                  R_MIPS_32

    // module info at 0x10
    put16(f, seg + 0x10, 0x0000);
    std::memcpy(f.data() + seg + 0x14, "testmod", 7);
    put32(f, seg + 0x10 + 32, 0x00008000); // gp                    R_MIPS_32
    put32(f, seg + 0x10 + 36, 0);          // ent_top
    put32(f, seg + 0x10 + 40, 0);          // ent_end
    put32(f, seg + 0x10 + 44, 0x50);       // stub_top              R_MIPS_32
    put32(f, seg + 0x10 + 48, 0x64);       // stub_end              R_MIPS_32

    // one import library with two functions
    put32(f, seg + 0x50, 0x80); // name                              R_MIPS_32
    put16(f, seg + 0x54, 0x0011);
    put16(f, seg + 0x56, 0x4001);
    f[seg + 0x58] = 5; // entry size in words
    f[seg + 0x59] = 0; // variables
    put16(f, seg + 0x5A, 2);
    put32(f, seg + 0x5C, 0x90); // nid table                         R_MIPS_32
    put32(f, seg + 0x60, 0xA0); // stubs                             R_MIPS_32
    std::memcpy(f.data() + seg + 0x80, "sceTest", 8);
    put32(f, seg + 0x90, 0x11111111);
    put32(f, seg + 0x94, 0x22222222);

    // relocation segment
    const std::pair<u32, u32> rels[] = {{0x00, 4}, {0x04, 5}, {0x08, 6}, {0x0C, 2}, {0x30, 2},
                                        {0x3C, 2}, {0x40, 2}, {0x50, 2}, {0x5C, 2}, {0x60, 2}};
    size_t count = sizeof(rels) / sizeof(rels[0]);
    put32(f, 52 + 32 + 0, 0x700000A0);
    put32(f, 52 + 32 + 4, 0x200);
    put32(f, 52 + 32 + 16, u32(count * 8));
    for (size_t i = 0; i < count; ++i) {
        put32(f, 0x200 + i * 8, rels[i].first);
        put32(f, 0x200 + i * 8 + 4, rels[i].second);
    }
    return f;
}

} // namespace

TEST(nid_from_name) {
    CHECK_EQ(nid_of("sceKernelCreateThread"), 0x446D8DE6u);
    CHECK_EQ(nid_of("sceIoOpen"), 0x109F50BCu);
    CHECK_EQ(nid_of("sceDisplayWaitVblankStart"), 0x984C27E7u);
}

TEST(nid_table_parsing) {
    NidTable table;
    std::string error;
    CHECK(table.load_text("# header\n[sceDisplay]\nsceDisplayWaitVblankStart  # trailing comment\n"
                          "0xDEADBEEF sceDisplayCustom\n\n[IoFileMgrForUser]\r\nsceIoOpen\r\n",
                          &error));
    CHECK_EQ(std::string(table.lookup("sceDisplay", 0x984C27E7)), std::string("sceDisplayWaitVblankStart"));
    CHECK_EQ(std::string(table.lookup("sceDisplay", 0xDEADBEEF)), std::string("sceDisplayCustom"));
    CHECK_EQ(std::string(table.lookup("IoFileMgrForUser", 0x109F50BC)), std::string("sceIoOpen"));
    CHECK(table.lookup("sceDisplay", 0x109F50BC) == nullptr); // right NID, wrong library

    NidTable bad;
    CHECK(!bad.load_text("sceIoOpen\n", &error)); // no library header
}

TEST(sfo_round_trip) {
    Sfo sfo;
    sfo.set_string("TITLE", "Example", 128);
    sfo.set_int("PARENTAL_LEVEL", 5);
    const u8 blob[] = {1, 2, 3, 4, 5};
    sfo.set_raw("SAVEDATA_PARAMS", blob, sizeof(blob), 128);
    std::vector<u8> bytes = sfo.serialize();

    Sfo parsed;
    CHECK(parsed.parse(bytes.data(), bytes.size()));
    CHECK_EQ(parsed.get_string("TITLE"), std::string("Example"));
    CHECK_EQ(parsed.get_int("PARENTAL_LEVEL"), 5u);
    const SfoEntry* raw = parsed.find("SAVEDATA_PARAMS");
    CHECK(raw != nullptr);
    if (raw) {
        CHECK(raw->raw.size() >= sizeof(blob));
        CHECK(std::memcmp(raw->raw.data(), blob, sizeof(blob)) == 0);
    }
    CHECK(parsed.find("MISSING") == nullptr);
    CHECK_EQ(parsed.get_int("MISSING", 7), 7u);

    u8 garbage[32] = {};
    CHECK(!parsed.parse(garbage, sizeof(garbage)));
}

TEST(prx_relocation_and_tables) {
    std::vector<u8> file = make_prx();
    Module m;
    std::string error;
    CHECK(load_prx(file, kDefaultLoadBase, m, &error));
    if (!error.empty()) std::printf("    load_prx: %s\n", error.c_str());

    const u32 base = kDefaultLoadBase;
    CHECK(m.relocatable);
    CHECK_EQ(m.base, base);
    CHECK_EQ(m.end, base + 0x180);
    CHECK_EQ(m.name, std::string("testmod"));
    CHECK_EQ(m.gp, base + 0x8000);

    // jal 0x40 -> jal base+0x40
    CHECK_EQ(m.read32(base + 0x00), 0x0C000000u | (((base + 0x40) >> 2) & 0x03FFFFFFu));
    // 0x18010 + base = 0x0881C010: the low half is negative as a signed 16-bit value, so the high half carries.
    CHECK_EQ(m.read32(base + 0x04), 0x3C040882u);
    CHECK_EQ(m.read32(base + 0x08), 0x2484C010u);
    CHECK_EQ(m.read32(base + 0x0C), base + 0x40);

    const Reloc* jal = m.reloc_at(base + 0x00);
    const Reloc* hi = m.reloc_at(base + 0x04);
    const Reloc* lo = m.reloc_at(base + 0x08);
    const Reloc* word = m.reloc_at(base + 0x0C);
    CHECK(jal && hi && lo && word);
    if (jal && hi && lo && word) {
        CHECK(jal->kind == RelocKind::Jump26);
        CHECK_EQ(jal->target, base + 0x40);
        CHECK(hi->kind == RelocKind::Hi16);
        CHECK_EQ(hi->target, base + 0x18010);
        CHECK(lo->kind == RelocKind::Lo16);
        CHECK_EQ(lo->target, base + 0x18010);
        CHECK(word->kind == RelocKind::Word32);
        CHECK_EQ(word->target, base + 0x40);
    }
    CHECK(m.reloc_at(base + 0x10) == nullptr);

    CHECK_EQ(m.imports.size(), size_t(1));
    if (m.imports.size() == 1) {
        const ImportLib& lib = m.imports[0];
        CHECK_EQ(lib.name, std::string("sceTest"));
        CHECK_EQ(lib.funcs.size(), size_t(2));
        if (lib.funcs.size() == 2) {
            CHECK_EQ(lib.funcs[0].nid, 0x11111111u);
            CHECK_EQ(lib.funcs[0].stub, base + 0xA0);
            CHECK_EQ(lib.funcs[1].nid, 0x22222222u);
            CHECK_EQ(lib.funcs[1].stub, base + 0xA8);
        }
    }
}

TEST(prx_rejects_bad_input) {
    Module m;
    std::string error;
    std::vector<u8> encrypted = {'~', 'P', 'S', 'P', 0, 0, 0, 0};
    encrypted.resize(64);
    CHECK(!load_prx(encrypted, kDefaultLoadBase, m, &error));
    CHECK(error.find("encrypted") != std::string::npos);

    std::vector<u8> truncated = make_prx();
    truncated.resize(0x120);
    CHECK(!load_prx(truncated, kDefaultLoadBase, m, &error));

    std::vector<u8> empty;
    CHECK(!load_prx(empty, kDefaultLoadBase, m, &error));
}
