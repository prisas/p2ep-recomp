#include "lib/allegrex/insn.h"
#include "lib/common/file.h"
#include "lib/common/log.h"
#include "lib/psp/nid.h"
#include "tools/recomp/game.h"

#include <algorithm>
#include <cstdio>
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
              "      Copy one file out of the image.\n");
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
    return usage();
}
