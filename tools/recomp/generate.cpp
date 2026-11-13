#include "tools/recomp/generate.h"

#include "lib/allegrex/insn.h"
#include "lib/common/file.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace p2::recomp {

namespace {

constexpr SupportedExecutable kSupported[] = {
    {"b66bec74b78cc3e9017147135af2165e90a11e63db6767e8f28148eede03efd9",
     "NPJH-50581, English-patched build 1.0"},
};

} // namespace

const SupportedExecutable* find_supported(const std::string& sha256) {
    for (const SupportedExecutable& s : kSupported)
        if (sha256 == s.sha256) return &s;
    return nullptr;
}

Input make_input(const psp::Module& module, const psp::NidTable& nids, const std::vector<CodeBlob>& blobs,
                 const std::vector<u32>& extra_entries) {
    Input in;
    in.base = module.base;
    in.image = module.image;
    for (const CodeBlob& blob : blobs) {
        if (blob.addr < in.base || u64(blob.addr - in.base) + blob.data.size() > in.image.size()) continue;
        std::memcpy(in.image.data() + (blob.addr - in.base), blob.data.data(), blob.data.size());
    }

    std::unordered_set<u32> stub_addrs;
    u32 index = 0;
    for (const psp::ImportLib& lib : module.imports) {
        for (const psp::ImportFunc& f : lib.funcs) {
            ImportStub stub;
            stub.lib = lib.name;
            if (const char* name = nids.lookup(lib.name, f.nid)) stub.name = name;
            stub.nid = f.nid;
            stub.addr = f.stub;
            stub.index = index++;
            in.stubs.push_back(std::move(stub));
            stub_addrs.insert(f.stub);
        }
    }

    // Translate every executable section except the one holding the import stubs.
    for (const psp::Section& s : module.sections) {
        if (!s.exec() || s.size < 4) continue;
        bool holds_stubs = false;
        for (u32 a = s.addr; a < s.addr + s.size; a += 8)
            if (stub_addrs.count(a)) holds_stubs = true;
        if (!holds_stubs) in.code.push_back({s.addr, s.addr + (s.size & ~3u)});
    }
    std::sort(in.code.begin(), in.code.end(), [](const CodeRange& a, const CodeRange& b) { return a.start < b.start; });

    // Code outside the executable sections. A patched executable can carry routines in its data
    // sections, reached by direct jumps from regular code. Follow those jumps and translate
    // whatever they lead to; each maximal run of reached instructions becomes a code range.
    {
        using allegrex::Insn;
        using allegrex::Op;
        auto file_backed_data = [&](u32 addr) {
            if (addr & 3) return false;
            for (const CodeBlob& blob : blobs)
                if (addr >= blob.addr && u64(addr) + 4 <= u64(blob.addr) + blob.data.size()) return in.in_image(addr);
            const psp::Section* s = module.section_at(addr);
            return s && !s->exec() && s->type != 8 /* no-bits */ && addr + 4 <= s->addr + s->size;
        };
        std::vector<u32> work;
        const std::vector<CodeRange> regular = in.code;
        for (const CodeRange& r : regular) {
            for (u32 pc = r.start; pc + 4 <= r.end; pc += 4) {
                Insn i(in.word(pc));
                if (i.op == Op::Jal || i.op == Op::J) {
                    u32 t = i.jump_target(pc);
                    if (file_backed_data(t)) work.push_back(t);
                }
            }
        }
        // Patch routines are also reached through pointers stored in data, which carry no
        // relocation. A word that holds the address of a stack-frame prologue outside the code
        // sections is taken to be such a pointer.
        auto is_prologue = [&](u32 addr) {
            if (!file_backed_data(addr)) return false;
            u32 w = in.word(addr);
            return (w & 0xFFFF8000u) == 0x27BD8000u; // addiu sp, sp, -N
        };
        std::vector<u32> pointer_seeds;
        auto scan_pointers = [&](u32 start, u32 end) {
            for (u32 a = align_up<u32>(start, 4); a + 4 <= end; a += 4) {
                u32 value = in.word(a);
                if (value >= in.base && is_prologue(value)) {
                    work.push_back(value);
                    pointer_seeds.push_back(value);
                }
            }
        };
        for (const psp::Section& s : module.sections)
            if (s.alloc() && !s.exec() && s.type != 8 && s.size >= 4) scan_pointers(s.addr, s.addr + s.size);
        for (const CodeBlob& blob : blobs)
            if (in.in_image(blob.addr, u32(blob.data.size()))) scan_pointers(blob.addr, blob.addr + u32(blob.data.size()));
        for (u32 entry : extra_entries)
            if (file_backed_data(entry)) work.push_back(entry);

        std::set<u32> reached;
        for (bool grew = true; grew;) {
        grew = false;
        while (!work.empty()) {
            u32 pc = work.back();
            work.pop_back();
            u32 zero_run = 0;
            while (file_backed_data(pc) && !reached.count(pc)) {
                Insn i(in.word(pc));
                if (i.op == Op::Invalid) break;
                zero_run = i.word == 0 ? zero_run + 1 : 0;
                if (zero_run > 16) break; // padding, not code
                reached.insert(pc);
                if (!i.has_delay_slot()) {
                    pc += 4;
                    continue;
                }
                if (!file_backed_data(pc + 4)) break;
                reached.insert(pc + 4);
                const u16 flags = i.flags();
                if (flags & allegrex::kBranch) {
                    u32 t = i.branch_target(pc);
                    if (file_backed_data(t)) work.push_back(t);
                    const bool always = (i.op == Op::Beq || i.op == Op::Beql) && i.rs() == i.rt();
                    if (always && !(flags & allegrex::kLink)) break;
                } else if (i.op == Op::J || i.op == Op::Jal) {
                    u32 t = i.jump_target(pc);
                    if (file_backed_data(t)) work.push_back(t);
                    if (i.op == Op::J) break;
                } else if (i.op == Op::Jr) {
                    break;
                }
                pc += 8;
            }
        }
        // Patch routines are laid out back to back. A prologue that directly follows the return
        // of a discovered routine starts another one, even if nothing visible refers to it.
        for (u32 addr : reached) {
            u32 next = addr + 4;
            if (reached.count(next) || !is_prologue(next)) continue;
            if (!in.in_image(next - 8) || in.word(next - 8) != 0x03E00008u) continue; // jr ra, then its delay slot
            work.push_back(next);
            pointer_seeds.push_back(next);
            grew = true;
        }
        }
        for (u32 seed : pointer_seeds)
            if (reached.count(seed)) in.entries.push_back(seed);
        for (auto it = reached.begin(); it != reached.end();) {
            u32 start = *it, end = start + 4;
            for (++it; it != reached.end() && *it == end; ++it) end += 4;
            in.code.push_back({start, end});
        }
        std::sort(in.code.begin(), in.code.end(), [](const CodeRange& a, const CodeRange& b) { return a.start < b.start; });
    }

    in.entries.push_back(module.entry);
    if (module.module_start) in.entries.push_back(module.module_start);
    if (module.module_stop) in.entries.push_back(module.module_stop);
    for (const psp::Export& e : module.exports)
        if (!e.is_var) in.entries.push_back(e.addr);

    for (const psp::Reloc& r : module.relocs) {
        if (r.target == 0) continue;
        // Only the high half is trusted for the full address: the relocation format pairs every HI16
        // with its own low half, whereas a LO16 may belong to a `lui` issued much earlier.
        if (r.kind == psp::RelocKind::Word32) in.word_refs.push_back({r.addr, r.target});
        else if (r.kind == psp::RelocKind::Hi16) in.code_refs.push_back({r.addr, r.target});
    }
    return in;
}

bool generate(const psp::Module& module, const std::string& sha256, const psp::NidTable& nids,
              const GenerateOptions& options, GenerateSummary& summary, std::string* error) {
    Input input = make_input(module, nids, options.blobs, options.extra_entries);
    if (input.code.empty()) {
        if (error) *error = "the executable has no section headers describing its code";
        return false;
    }

    Program program;
    if (!analyze(input, options.extra_entries, program, error)) return false;

    EmitOptions emit_options;
    emit_options.shards = options.shards;
    emit_options.comments = options.comments;
    emit_options.sha256 = sha256;
    emit_options.module_base = module.base;
    emit_options.module_end = module.end;
    emit_options.module_entry = module.module_start ? module.module_start : module.entry;
    emit_options.module_gp = module.gp;

    EmitResult emitted;
    emit(program, emit_options, emitted);

    summary.stats = program.stats;
    summary.functions = program.functions.size();
    summary.unimplemented = emitted.unimplemented;

    // --- report ---
    std::string& rep = summary.report;
    const Stats& st = program.stats;
    append_format(rep, "executable sha256        %s\n", sha256.c_str());
    append_format(rep, "code words               %zu\n", st.code_words);
    append_format(rep, "translated words         %zu (%.2f%%)\n", st.covered_words,
                  st.code_words ? 100.0 * double(st.covered_words) / double(st.code_words) : 0.0);
    append_format(rep, "functions                %zu\n", summary.functions);
    {
        static const char* const names[] = {"module entry points", "direct call targets", "data pointers",
                                            "code pointers", "split at branch target", "forced"};
        for (size_t k = 0; k < 6; ++k)
            if (st.by_origin[k]) append_format(rep, "  %-22s %zu\n", names[k], st.by_origin[k]);
    }
    append_format(rep, "code pointer targets     %zu (%zu are labels inside a function)\n", st.pointer_targets,
                  st.secondary_entries);
    append_format(rep, "switch tables           %zu (%zu targets)\n", st.jump_tables, st.jump_table_targets);
    append_format(rep, "register jumps, dynamic  %zu\n", st.indirect_jumps_unresolved);
    append_format(rep, "undecodable words hit    %zu\n", st.traps);
    append_format(rep, "untranslated opcodes     %zu\n", emitted.unimplemented.size());
    append_format(rep, "imports                  %zu\n", input.stubs.size());
    append_format(rep, "analysis passes          %zu\n", st.passes);
    append_format(rep, "dispatch table           base %08X, %u words\n", emitted.text_base, emitted.text_words);
    {
        // Code ranges that are not executable sections: routines a patch placed among the data.
        size_t islands = 0, island_words = 0;
        std::string list;
        for (const CodeRange& r : input.code) {
            const psp::Section* s = module.section_at(r.start);
            if (s && s->exec()) continue;
            ++islands;
            island_words += (r.end - r.start) / 4;
            const char* where = s ? s->name.c_str() : "?";
            for (const CodeBlob& blob : options.blobs)
                if (r.start >= blob.addr && r.start - blob.addr < blob.data.size()) where = blob.name.c_str();
            if (islands <= 40) append_format(list, "  %08X-%08X  %s\n", r.start, r.end, where);
        }
        append_format(rep, "code in data sections    %zu range(s), %zu words\n%s", islands, island_words, list.c_str());
    }

    // Functions that exist only because another function branches into them.
    {
        size_t shown = 0;
        for (u32 e : program.entries) {
            auto it = program.origins.find(e);
            if (it == program.origins.end() || it->second.kind != Origin::BranchTarget) continue;
            if (!shown) rep += "\nfunctions split at a branch target (target <- branch [containing function, its limit]):\n";
            if (++shown > 40) break;
            u32 from = it->second.from;
            auto owner = program.entries.upper_bound(from);
            u32 owner_entry = owner == program.entries.begin() ? 0 : *std::prev(owner);
            u32 owner_limit = owner == program.entries.end() ? 0 : *owner;
            append_format(rep, "  %08X <- %08X [%08X, %08X)  %s\n", e, from, owner_entry, owner_limit,
                          allegrex::disassemble(from, input.word(from)).c_str());
        }
    }

    if (!emitted.unimplemented.empty()) {
        rep += "\nuntranslated instructions:\n";
        for (const std::string& line : emitted.unimplemented) rep += "  " + line + "\n";
    }
    if (!program.unresolved_jumps.empty()) {
        rep += "\nregister jumps resolved at run time (address, preceding code):\n";
        for (u32 pc : program.unresolved_jumps) {
            append_format(rep, "  %08X:\n", pc);
            for (u32 a = pc - 20; a <= pc + 4; a += 4)
                if (input.in_code(a))
                    append_format(rep, "      %08X  %s\n", a, allegrex::disassemble(a, input.word(a)).c_str());
        }
    }
    size_t unnamed = 0;
    for (const ImportStub& s : input.stubs)
        if (s.name.empty()) {
            if (!unnamed++) rep += "\nimports without a known name:\n";
            append_format(rep, "  %s %08X\n", s.lib.c_str(), s.nid);
        }
    emitted.files["gen_report.txt"] = rep;

    // --- write ---
    std::error_code ec;
    std::filesystem::create_directories(options.out_dir, ec);
    for (const auto& [name, content] : emitted.files) {
        bool changed = false;
        if (!write_file_if_changed(options.out_dir / name, content, &changed)) {
            if (error) *error = "cannot write " + (options.out_dir / name).string();
            return false;
        }
        ++(changed ? summary.files_written : summary.files_unchanged);
    }
    // Shards left over from a run with a larger shard count would otherwise go stale.
    for (u32 shard = options.shards; shard < 4096; ++shard) {
        auto stale = options.out_dir / format("gen_%03u.cpp", shard);
        if (!std::filesystem::exists(stale, ec)) break;
        std::filesystem::remove(stale, ec);
    }
    return true;
}

} // namespace p2::recomp
