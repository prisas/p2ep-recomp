#include "lib/psp/prx.h"

#include "lib/common/file.h"

#include <algorithm>

namespace p2::psp {

namespace {

constexpr u16 kTypeExec = 2;
constexpr u16 kTypePrx = 0xFFA0;
constexpr u32 kPtLoad = 1;
constexpr u32 kPrxReloc = 0x700000A0;  // section and segment type
constexpr u32 kPrxReloc2 = 0x700000A1; // compressed relocation format (not used by this title)

constexpr u32 kRMipsNone = 0, kRMips16 = 1, kRMips32 = 2, kRMips26 = 4, kRMipsHi16 = 5, kRMipsLo16 = 6,
              kRMipsGpRel16 = 7;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

struct RawRel {
    u32 offset;
    u32 info;
};

} // namespace

std::string Module::read_cstring(u32 addr, size_t max) const {
    std::string out;
    while (contains(addr) && out.size() < max) {
        char ch = char(read8(addr++));
        if (!ch) break;
        out.push_back(ch);
    }
    return out;
}

const Section* Module::section(std::string_view section_name) const {
    for (const Section& s : sections)
        if (s.name == section_name) return &s;
    return nullptr;
}

const Section* Module::section_at(u32 addr) const {
    for (const Section& s : sections)
        if (s.contains(addr)) return &s;
    return nullptr;
}

bool Module::is_code(u32 addr) const {
    for (const Section& s : sections)
        if (s.exec() && s.contains(addr)) return true;
    return false;
}

const Export* Module::find_export(u32 nid, bool is_var) const {
    for (const Export& e : exports)
        if (e.lib.empty() && e.nid == nid && e.is_var == is_var) return &e;
    return nullptr;
}

const Reloc* Module::reloc_at(u32 addr) const {
    auto it = std::lower_bound(relocs.begin(), relocs.end(), addr,
                               [](const Reloc& r, u32 a) { return r.addr < a; });
    return it != relocs.end() && it->addr == addr ? &*it : nullptr;
}

bool load_prx(const std::vector<u8>& file, u32 base, Module& m, std::string* error) {
    m = Module{};
    if (file.size() < 52 || std::memcmp(file.data(), "\x7f"
                                                     "ELF",
                                        4) != 0) {
        if (file.size() >= 4 && std::memcmp(file.data(), "~PSP", 4) == 0)
            return fail(error, "the executable is encrypted; a decrypted executable is required");
        return fail(error, "the executable is not an ELF file");
    }
    if (file[4] != 1 || file[5] != 1) return fail(error, "the executable is not a 32-bit little-endian ELF");

    const u8* d = file.data();
    u16 e_type = load_le<u16>(d + 16);
    u16 e_machine = load_le<u16>(d + 18);
    u32 e_entry = load_le<u32>(d + 24);
    u32 e_phoff = load_le<u32>(d + 28);
    u32 e_shoff = load_le<u32>(d + 32);
    u16 e_phentsize = load_le<u16>(d + 42);
    u16 e_phnum = load_le<u16>(d + 44);
    u16 e_shentsize = load_le<u16>(d + 46);
    u16 e_shnum = load_le<u16>(d + 48);
    u16 e_shstrndx = load_le<u16>(d + 50);

    if (e_machine != 8) return fail(error, "the executable is not MIPS code");
    if (e_type != kTypeExec && e_type != kTypePrx) return fail(error, "unsupported ELF type");
    if (e_phnum == 0 || e_phentsize < 32 || u64(e_phoff) + u64(e_phnum) * e_phentsize > file.size())
        return fail(error, "the executable has no valid program headers");

    m.relocatable = e_type == kTypePrx;
    const u32 bias = m.relocatable ? base : 0;

    // --- program headers -------------------------------------------------------------------------------
    struct RawSegment {
        u32 type, offset, vaddr, paddr, filesz, memsz, flags;
    };
    std::vector<RawSegment> raw_segments;
    std::vector<u32> segment_addr; // relocated start address per program header index
    u32 lo = ~0u, hi = 0;
    for (u32 i = 0; i < e_phnum; ++i) {
        const u8* p = d + e_phoff + i * e_phentsize;
        RawSegment s{load_le<u32>(p),      load_le<u32>(p + 4),  load_le<u32>(p + 8), load_le<u32>(p + 12),
                     load_le<u32>(p + 16), load_le<u32>(p + 20), load_le<u32>(p + 24)};
        raw_segments.push_back(s);
        segment_addr.push_back(s.vaddr + bias);
        m.segments.push_back({s.type, s.offset, s.vaddr + bias, s.filesz, s.memsz, s.flags});
        if (s.type != kPtLoad || s.memsz == 0) continue;
        if (s.filesz > s.memsz || u64(s.offset) + s.filesz > file.size())
            return fail(error, "a load segment lies outside the file");
        lo = std::min(lo, s.vaddr + bias);
        hi = std::max(hi, s.vaddr + bias + s.memsz);
    }
    if (lo >= hi || hi - lo > (64u << 20)) return fail(error, "the executable has no loadable data");

    m.base = lo;
    m.end = hi;
    m.entry = e_entry + bias;
    m.image.assign(hi - lo, 0);
    for (const RawSegment& s : raw_segments) {
        if (s.type != kPtLoad || s.filesz == 0) continue;
        std::memcpy(m.image.data() + (s.vaddr + bias - lo), d + s.offset, s.filesz);
    }

    // --- section headers (optional) --------------------------------------------------------------------
    if (e_shnum && e_shentsize >= 40 && u64(e_shoff) + u64(e_shnum) * e_shentsize <= file.size() &&
        e_shstrndx < e_shnum) {
        const u8* strhdr = d + e_shoff + e_shstrndx * e_shentsize;
        u32 str_off = load_le<u32>(strhdr + 16), str_size = load_le<u32>(strhdr + 20);
        for (u32 i = 0; i < e_shnum; ++i) {
            const u8* s = d + e_shoff + i * e_shentsize;
            Section sec;
            u32 name_off = load_le<u32>(s);
            sec.type = load_le<u32>(s + 4);
            sec.flags = load_le<u32>(s + 8);
            sec.addr = load_le<u32>(s + 12);
            sec.offset = load_le<u32>(s + 16);
            sec.size = load_le<u32>(s + 20);
            if (sec.alloc()) sec.addr += bias;
            if (u64(str_off) + str_size <= file.size() && name_off < str_size) {
                const char* name = reinterpret_cast<const char*>(d + str_off + name_off);
                sec.name.assign(name, strnlen(name, str_size - name_off));
            }
            m.sections.push_back(std::move(sec));
        }
    }

    // --- relocations -----------------------------------------------------------------------------------
    if (m.relocatable) {
        std::vector<std::pair<u32, u32>> tables; // file offset, byte size
        for (const Section& s : m.sections) {
            if (s.type == kPrxReloc2) return fail(error, "compressed relocations are not supported");
            if (s.type == kPrxReloc) tables.push_back({s.offset, s.size});
        }
        if (m.sections.empty()) {
            for (const RawSegment& s : raw_segments) {
                if (s.type == kPrxReloc2) return fail(error, "compressed relocations are not supported");
                if (s.type == kPrxReloc) tables.push_back({s.offset, s.filesz});
            }
        }

        auto word_ok = [&](u32 addr) { return addr >= lo && addr <= hi - 4; };
        auto rd = [&](u32 addr) { return load_le<u32>(m.image.data() + (addr - lo)); };
        auto wr = [&](u32 addr, u32 v) { store_le<u32>(m.image.data() + (addr - lo), v); };

        for (auto [table_off, table_size] : tables) {
            if (u64(table_off) + table_size > file.size()) return fail(error, "relocation table lies outside the file");
            u32 count = table_size / 8;
            std::vector<RawRel> rels(count);
            std::vector<u32> original(count, 0);
            auto addr_of = [&](const RawRel& r, u32* out_addr) {
                u32 seg = (r.info >> 8) & 0xFF;
                if (seg >= segment_addr.size()) return false;
                *out_addr = r.offset + segment_addr[seg];
                return word_ok(*out_addr);
            };
            for (u32 i = 0; i < count; ++i) {
                rels[i] = {load_le<u32>(d + table_off + i * 8), load_le<u32>(d + table_off + i * 8 + 4)};
                u32 addr;
                if (addr_of(rels[i], &addr)) original[i] = rd(addr);
            }

            u32 last_hi = 0;
            bool have_hi = false;
            for (u32 i = 0; i < count; ++i) {
                u32 type = rels[i].info & 0xFF;
                u32 addr_seg = (rels[i].info >> 16) & 0xFF;
                u32 addr;
                if (type == kRMipsNone) continue;
                if (!addr_of(rels[i], &addr) || addr_seg >= segment_addr.size())
                    return fail(error, "relocation refers to an invalid segment or address");
                const u32 delta = segment_addr[addr_seg];
                u32 op = original[i];
                Reloc rec{addr, 0, RelocKind::Word32};

                switch (type) {
                case kRMips32:
                    op += delta;
                    rec.target = op;
                    rec.kind = RelocKind::Word32;
                    break;
                case kRMips26:
                    op = (op & 0xFC000000u) | (((op & 0x03FFFFFFu) + (delta >> 2)) & 0x03FFFFFFu);
                    rec.target = ((addr + 4) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2);
                    rec.kind = RelocKind::Jump26;
                    break;
                case kRMipsHi16: {
                    // The low half lives in the next non-HI16 relocation; its sign carries into the high half.
                    u32 full = (op & 0xFFFFu) << 16;
                    bool paired = false;
                    for (u32 t = i + 1; t < count; ++t) {
                        if ((rels[t].info & 0xFF) == kRMipsHi16) continue;
                        full += u32(s32(s16(original[t] & 0xFFFFu)));
                        paired = true;
                        break;
                    }
                    if (!paired) return fail(error, "HI16 relocation without a matching LO16");
                    full += delta;
                    u32 low = full & 0xFFFFu;
                    u32 high = (full - u32(s32(s16(low)))) >> 16;
                    op = (op & 0xFFFF0000u) | (high & 0xFFFFu);
                    last_hi = high & 0xFFFFu;
                    have_hi = true;
                    rec.target = full;
                    rec.kind = RelocKind::Hi16;
                    break;
                }
                case kRMipsLo16: {
                    u32 low = ((op & 0xFFFFu) + delta) & 0xFFFFu;
                    op = (op & 0xFFFF0000u) | low;
                    rec.target = have_hi ? (last_hi << 16) + u32(s32(s16(low))) : 0;
                    rec.kind = RelocKind::Lo16;
                    break;
                }
                case kRMips16:
                    op = (op & 0xFFFF0000u) | ((u32(s32(s16(op & 0xFFFFu))) + delta) & 0xFFFFu);
                    rec.kind = RelocKind::Abs16;
                    break;
                case kRMipsGpRel16:
                    rec.kind = RelocKind::GpRel16; // $gp moves with the module; nothing to patch
                    break;
                default:
                    return fail(error, "unsupported relocation type");
                }

                wr(addr, op);
                m.relocs.push_back(rec);
            }
        }
        std::stable_sort(m.relocs.begin(), m.relocs.end(),
                         [](const Reloc& a, const Reloc& b) { return a.addr < b.addr; });
    }

    // --- module info -----------------------------------------------------------------------------------
    u32 info_addr = 0;
    if (const Section* s = m.section(".rodata.sceModuleInfo")) {
        info_addr = s->addr;
    } else {
        const RawSegment& s0 = raw_segments[0];
        info_addr = s0.vaddr + bias + (s0.paddr & 0x7FFFFFFFu) - s0.offset;
    }
    if (!m.contains(info_addr, 52)) return fail(error, "module info lies outside the executable");

    m.attr = m.read16(info_addr);
    {
        char name[28] = {};
        std::memcpy(name, m.image.data() + (info_addr + 4 - m.base), 27);
        m.name = name;
    }
    m.gp = m.read32(info_addr + 32);
    u32 ent_top = m.read32(info_addr + 36), ent_end = m.read32(info_addr + 40);
    u32 stub_top = m.read32(info_addr + 44), stub_end = m.read32(info_addr + 48);

    // --- imports ---------------------------------------------------------------------------------------
    for (u32 p = stub_top; p < stub_end;) {
        if (!m.contains(p, 20)) return fail(error, "import table lies outside the executable");
        u32 name_ptr = m.read32(p);
        u32 words = m.read8(p + 8);
        ImportLib lib;
        lib.name = m.contains(name_ptr) ? m.read_cstring(name_ptr) : std::string();
        lib.version = m.read16(p + 4);
        lib.attr = m.read16(p + 6);
        lib.var_count = m.read8(p + 9);
        u32 func_count = m.read16(p + 10);
        u32 nid_ptr = m.read32(p + 12);
        u32 stub_ptr = m.read32(p + 16);
        if (words < 5 || (func_count && !m.contains(nid_ptr, func_count * 4)))
            return fail(error, "import table entry is malformed");
        for (u32 i = 0; i < func_count; ++i)
            lib.funcs.push_back({m.read32(nid_ptr + i * 4), stub_ptr + i * 8});
        m.imports.push_back(std::move(lib));
        p += words * 4;
    }

    // --- exports ---------------------------------------------------------------------------------------
    for (u32 p = ent_top; p < ent_end;) {
        if (!m.contains(p, 16)) return fail(error, "export table lies outside the executable");
        u32 name_ptr = m.read32(p);
        u32 words = m.read8(p + 8);
        u32 var_count = m.read8(p + 9);
        u32 func_count = m.read16(p + 10);
        u32 table = m.read32(p + 12);
        if (words < 4) return fail(error, "export table entry is malformed");
        u32 total = func_count + var_count;
        if (total && !m.contains(table, total * 8)) return fail(error, "export table entry is malformed");
        std::string lib_name = name_ptr && m.contains(name_ptr) ? m.read_cstring(name_ptr) : std::string();
        for (u32 i = 0; i < total; ++i) {
            Export e{lib_name, m.read32(table + i * 4), m.read32(table + (total + i) * 4), i >= func_count};
            m.exports.push_back(e);
        }
        p += words * 4;
    }

    if (const Export* e = m.find_export(kNidModuleStart, false)) m.module_start = e->addr;
    if (const Export* e = m.find_export(kNidModuleStop, false)) m.module_stop = e->addr;
    if (const Export* e = m.find_export(kNidModuleSdkVersion, true); e && m.contains(e->addr, 4))
        m.sdk_version = m.read32(e->addr);
    if (const Export* e = m.find_export(kNidModuleStartThreadParameter, true); e && m.contains(e->addr, 16)) {
        // { count, priority, stack size, attributes }
        u32 count = m.read32(e->addr);
        if (count >= 1) m.start_thread_priority = m.read32(e->addr + 4);
        if (count >= 2) m.start_thread_stack = m.read32(e->addr + 8);
        if (count >= 3) m.start_thread_attr = m.read32(e->addr + 12);
    }
    return true;
}

} // namespace p2::psp
