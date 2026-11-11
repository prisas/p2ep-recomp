#pragma once

#include "lib/common/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace p2::psp {

// Where the firmware places the first user module; relocatable executables are fixed here.
constexpr u32 kDefaultLoadBase = 0x08804000;

constexpr u32 kNidModuleStart = 0xD632ACDB;
constexpr u32 kNidModuleStop = 0xCEE8593C;
constexpr u32 kNidModuleInfo = 0xF01D73A7;
constexpr u32 kNidModuleSdkVersion = 0x11B97506;
constexpr u32 kNidModuleStartThreadParameter = 0x0F7C276C;

struct Section {
    std::string name;
    u32 type = 0;
    u32 flags = 0;
    u32 addr = 0; // guest address after relocation (0 when not loaded)
    u32 offset = 0;
    u32 size = 0;

    bool alloc() const { return (flags & 2) != 0; }
    bool exec() const { return (flags & 4) != 0; }
    bool contains(u32 a) const { return alloc() && a >= addr && a - addr < size; }
};

struct Segment {
    u32 type = 0;
    u32 offset = 0;
    u32 vaddr = 0; // guest address after relocation
    u32 filesz = 0;
    u32 memsz = 0;
    u32 flags = 0;
};

struct ImportFunc {
    u32 nid = 0;
    u32 stub = 0; // address of the 8-byte stub the loader patches
};

struct ImportLib {
    std::string name;
    u16 version = 0;
    u16 attr = 0;
    std::vector<ImportFunc> funcs;
    u32 var_count = 0;
};

struct Export {
    std::string lib; // empty for the module's own system exports
    u32 nid = 0;
    u32 addr = 0;
    bool is_var = false;
};

enum class RelocKind : u8 { Word32, Jump26, Hi16, Lo16, Abs16, GpRel16 };

struct Reloc {
    u32 addr = 0;   // address of the patched word
    u32 target = 0; // absolute address the relocation refers to (0 when it cannot be derived)
    RelocKind kind = RelocKind::Word32;
};

struct Module {
    std::string name;
    bool relocatable = false;
    u32 base = 0;  // first loaded address
    u32 end = 0;   // one past the last loaded address, including zero-initialised data
    u32 entry = 0; // ELF entry point
    u32 gp = 0;
    u16 attr = 0;

    std::vector<u8> image; // relocated contents of [base, end)
    std::vector<Section> sections;
    std::vector<Segment> segments;
    std::vector<ImportLib> imports;
    std::vector<Export> exports;
    std::vector<Reloc> relocs; // sorted by address

    u32 module_start = 0;
    u32 module_stop = 0;
    u32 sdk_version = 0;
    // From module_start_thread_parameter, when exported.
    u32 start_thread_priority = 0;
    u32 start_thread_stack = 0;
    u32 start_thread_attr = 0;

    bool contains(u32 addr, u32 size = 1) const { return addr >= base && addr - base <= image.size() - size; }
    u32 read32(u32 addr) const { return load_le<u32>(image.data() + (addr - base)); }
    u16 read16(u32 addr) const { return load_le<u16>(image.data() + (addr - base)); }
    u8 read8(u32 addr) const { return image[addr - base]; }
    std::string read_cstring(u32 addr, size_t max = 256) const;

    const Section* section(std::string_view name) const;
    const Section* section_at(u32 addr) const;
    bool is_code(u32 addr) const; // inside an executable section
    const Export* find_export(u32 nid, bool is_var) const;
    const Reloc* reloc_at(u32 addr) const;
};

// Loads an unencrypted PSP executable (fixed-address ELF or relocatable PRX) and relocates it to `base`.
bool load_prx(const std::vector<u8>& file, u32 base, Module& out, std::string* error);

} // namespace p2::psp
