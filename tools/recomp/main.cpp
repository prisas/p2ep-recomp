#include "lib/allegrex/insn.h"
#include "lib/common/file.h"
#include "lib/common/log.h"
#include "lib/psp/nid.h"
#include "tools/recomp/game.h"
#include "tools/recomp/generate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace p2;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options; // --key value, or --flag ("1")

    bool has(const std::string& key) const { return options.count(key) != 0; }
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        auto it = options.find(key);
        return it == options.end() ? fallback : it->second;
    }
};

Args parse_args(int argc, char** argv, int first, const std::vector<std::string>& flags) {
    Args args;
    for (int i = first; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            std::string key = a.substr(2);
            bool is_flag = std::find(flags.begin(), flags.end(), key) != flags.end();
            if (is_flag || i + 1 >= argc) args.options[key] = "1";
            else args.options[key] = argv[++i];
        } else {
            args.positional.push_back(a);
        }
    }
    return args;
}

int usage() {
    std::puts("p2recomp - static recompiler for the game executable\n"
              "\n"
              "  p2recomp info <image> [--api <dir>] [--files] [--sections] [--imports] [--census]\n"
              "      Describe the disc image and its executable.\n"
              "  p2recomp extract <image> <path-in-image> <output-file>\n"
              "      Copy one file out of the image.\n"
              "  p2recomp disasm <image> <hex-address> [count]\n"
              "      Disassemble part of the relocated executable.\n"
              "  p2recomp gen --image <image> --out <dir> --api <dir> [--shards N] [--entries <file>]\n"
              "               [--stamp <file>] [--no-comments] [--allow-unknown] [--keep-going]\n"
              "      Translate the executable to C++.\n");
    return 2;
}

int cmd_info(const Args& args) {
    if (args.positional.empty()) return usage();
    recomp::Game game;
    std::string error;
    if (!game.load(args.positional[0], &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    psp::NidTable nids;
    if (args.has("api") && !nids.load_dir(args.get("api"), &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const psp::Module& m = game.module;
    std::printf("image       %s, %llu bytes, volume '%s', %zu entries\n", game.image->format_name(),
                static_cast<unsigned long long>(game.image->size()), game.fs.volume_id().c_str(),
                game.fs.entries().size());
    std::printf("disc id     %s  version %s  firmware %s\n", game.sfo.get_string("DISC_ID").c_str(),
                game.sfo.get_string("DISC_VERSION").c_str(), game.sfo.get_string("PSP_SYSTEM_VER").c_str());
    std::printf("executable  %zu bytes  sha256 %s\n", game.executable.size(), game.executable_sha256.c_str());
    std::printf("module      '%s' %s  base %08x  end %08x  entry %08x  gp %08x  attr %04x\n", m.name.c_str(),
                m.relocatable ? "relocatable" : "fixed", m.base, m.end, m.entry, m.gp, m.attr);
    std::printf("            module_start %08x  module_stop %08x  sdk %08x  start thread prio %u stack %u attr %08x\n",
                m.module_start, m.module_stop, m.sdk_version, m.start_thread_priority, m.start_thread_stack,
                m.start_thread_attr);
    std::printf("            %zu relocations, %zu sections, %zu import libraries, %zu exports\n", m.relocs.size(),
                m.sections.size(), m.imports.size(), m.exports.size());

    if (args.has("files")) {
        std::puts("\nfiles:");
        for (const auto& e : game.fs.entries())
            if (!e.is_dir) std::printf("  %-52s lba %7u  %10u\n", e.path.c_str(), e.lba, e.size);
    }

    if (args.has("sections")) {
        std::puts("\nsections:");
        for (const auto& s : m.sections)
            if (s.alloc())
                std::printf("  %-26s %08x - %08x  %8u bytes  %s\n", s.name.c_str(), s.addr, s.addr + s.size, s.size,
                            s.exec() ? "code" : (s.type == 8 ? "bss" : "data"));
        std::puts("exports:");
        for (const auto& e : m.exports)
            std::printf("  %-12s %s %08X -> %08x\n", e.lib.empty() ? "(module)" : e.lib.c_str(),
                        e.is_var ? "var " : "func", e.nid, e.addr);
    }

    if (args.has("imports")) {
        std::puts("\nimports:");
        size_t unknown = 0, total = 0;
        for (const auto& lib : m.imports) {
            std::printf("[%s]  # %zu functions, version %04x, attr %04x\n", lib.name.c_str(), lib.funcs.size(),
                        lib.version, lib.attr);
            for (const auto& f : lib.funcs) {
                const char* name = nids.lookup(lib.name, f.nid);
                ++total;
                if (!name) ++unknown;
                std::printf("0x%08X %-44s # stub %08x\n", f.nid, name ? name : "?", f.stub);
            }
        }
        std::printf("# %zu imports, %zu without a name\n", total, unknown);
    }

    if (args.has("census")) {
        std::map<std::string, size_t> counts;
        size_t words = 0, invalid = 0;
        for (const auto& s : m.sections) {
            if (!s.exec()) continue;
            for (u32 a = s.addr; a + 4 <= s.addr + s.size; a += 4) {
                allegrex::Op op = allegrex::decode(m.read32(a));
                ++words;
                if (op == allegrex::Op::Invalid) ++invalid;
                ++counts[allegrex::info(op).name];
            }
        }
        std::vector<std::pair<size_t, std::string>> sorted;
        for (auto& [name, n] : counts) sorted.push_back({n, name});
        std::sort(sorted.rbegin(), sorted.rend());
        std::printf("\nopcode census over executable sections (%zu words, %zu undecodable):\n", words, invalid);
        int column = 0;
        for (auto& [n, name] : sorted) {
            std::printf("  %-10s %7zu", name.c_str(), n);
            if (++column % 5 == 0) std::puts("");
        }
        std::puts("");
    }
    return 0;
}

int cmd_gen(const Args& args) {
    if (!args.has("image") || !args.has("out") || !args.has("api")) return usage();
    recomp::Game game;
    std::string error;
    if (!game.load(args.get("image"), &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const recomp::SupportedExecutable* supported = recomp::find_supported(game.executable_sha256);
    if (!supported && !args.has("allow-unknown")) {
        std::fprintf(stderr,
                     "error: this executable is not a supported build.\n"
                     "       sha256 %s\n"
                     "       Pass --allow-unknown to translate it anyway; the result is untested.\n",
                     game.executable_sha256.c_str());
        return 1;
    }

    psp::NidTable nids;
    if (!nids.load_dir(args.get("api"), &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    recomp::GenerateOptions options;
    options.out_dir = args.get("out");
    options.shards = u32(std::strtoul(args.get("shards", "96").c_str(), nullptr, 10));
    options.comments = !args.has("no-comments");
    if (args.has("entries")) {
        std::string text;
        if (!read_text_file(args.get("entries"), text)) {
            std::fprintf(stderr, "error: cannot read %s\n", args.get("entries").c_str());
            return 1;
        }
        // One hexadecimal address per line; '#' starts a comment.
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string entry = text.substr(pos, end - pos);
            if (size_t hash = entry.find('#'); hash != std::string::npos) entry.resize(hash);
            char* stop = nullptr;
            unsigned long value = std::strtoul(entry.c_str(), &stop, 16);
            if (stop != entry.c_str()) options.extra_entries.push_back(u32(value));
            pos = end + 1;
        }
    }

    recomp::GenerateSummary summary;
    if (!recomp::generate(game.module, game.executable_sha256, nids, options, summary, &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s", summary.report.c_str());
    std::printf("%zu file(s) written, %zu unchanged, in %s\n", summary.files_written, summary.files_unchanged,
                options.out_dir.string().c_str());

    if (!summary.unimplemented.empty() && !args.has("keep-going")) {
        std::fprintf(stderr, "error: %zu instruction(s) could not be translated (see gen_report.txt)\n",
                     summary.unimplemented.size());
        return 1;
    }
    if (args.has("stamp")) {
        std::string stamp = game.executable_sha256 + "\n";
        if (!write_file(args.get("stamp"), stamp.data(), stamp.size())) {
            std::fprintf(stderr, "error: cannot write %s\n", args.get("stamp").c_str());
            return 1;
        }
    }
    return 0;
}

// Disassembles a stretch of the relocated executable, marking relocated words with their targets.
int cmd_disasm(const Args& args) {
    if (args.positional.size() < 2) return usage();
    recomp::Game game;
    std::string error;
    if (!game.load(args.positional[0], &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const psp::Module& m = game.module;
    u32 addr = u32(std::strtoul(args.positional[1].c_str(), nullptr, 16)) & ~3u;
    u32 count = args.positional.size() > 2 ? u32(std::strtoul(args.positional[2].c_str(), nullptr, 0)) : 24;
    for (u32 k = 0; k < count; ++k, addr += 4) {
        if (!m.contains(addr, 4)) break;
        u32 word = m.read32(addr);
        std::string text = m.is_code(addr) ? allegrex::disassemble(addr, word) : format(".word 0x%08x", word);
        const psp::Reloc* r = m.reloc_at(addr);
        if (r && r->target) std::printf("%08X  %08X  %-36s -> %08X\n", addr, word, text.c_str(), r->target);
        else std::printf("%08X  %08X  %s\n", addr, word, text.c_str());
    }
    return 0;
}

// Lists every relocated word or instruction that refers to an address.
int cmd_refs(const Args& args) {
    if (args.positional.size() < 2) return usage();
    recomp::Game game;
    std::string error;
    if (!game.load(args.positional[0], &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const psp::Module& m = game.module;
    u32 target = u32(std::strtoul(args.positional[1].c_str(), nullptr, 16));
    static const char* const kinds[] = {"word", "jump", "hi16", "lo16", "abs16", "gprel"};
    size_t shown = 0;
    for (const psp::Reloc& r : m.relocs) {
        if (r.target != target) continue;
        u32 word = m.read32(r.addr);
        std::printf("%08X  %-5s %s\n", r.addr, kinds[size_t(r.kind)],
                    m.is_code(r.addr) ? allegrex::disassemble(r.addr, word).c_str() : "(data)");
        if (++shown >= 64) break;
    }
    if (!shown) std::puts("no references");
    return 0;
}

int cmd_extract(const Args& args) {
    if (args.positional.size() < 3) return usage();
    std::string error;
    auto image = disc::Image::open(args.positional[0], &error);
    disc::IsoFs fs;
    if (!image || !fs.mount(image.get(), &error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::vector<u8> data;
    if (!fs.read_all(args.positional[1], data)) {
        std::fprintf(stderr, "error: '%s' not found in the image\n", args.positional[1].c_str());
        return 1;
    }
    if (!write_file(args.positional[2], data.data(), data.size())) {
        std::fprintf(stderr, "error: cannot write %s\n", args.positional[2].c_str());
        return 1;
    }
    std::printf("wrote %zu bytes to %s\n", data.size(), args.positional[2].c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string command = argv[1];
    if (command == "info") return cmd_info(parse_args(argc, argv, 2, {"files", "sections", "imports", "census"}));
    if (command == "extract") return cmd_extract(parse_args(argc, argv, 2, {}));
    if (command == "disasm") return cmd_disasm(parse_args(argc, argv, 2, {}));
    if (command == "refs") return cmd_refs(parse_args(argc, argv, 2, {}));
    if (command == "gen") return cmd_gen(parse_args(argc, argv, 2, {"allow-unknown", "no-comments", "keep-going"}));
    return usage();
}
