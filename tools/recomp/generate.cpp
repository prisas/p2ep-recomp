#include "tools/recomp/generate.h"

#include "lib/allegrex/insn.h"
#include "lib/common/file.h"

#include <algorithm>
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

Input make_input(const psp::Module& module, const psp::NidTable& nids) {
    Input in;
    in.base = module.base;
    in.image = module.image;

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
    Input input = make_input(module, nids);
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
