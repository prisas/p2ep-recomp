#pragma once

#include "lib/psp/nid.h"
#include "lib/psp/prx.h"
#include "tools/recomp/analysis.h"
#include "tools/recomp/emit.h"

#include <filesystem>
#include <string>

namespace p2::recomp {

// Executables this project has been verified against, by SHA-256.
struct SupportedExecutable {
    const char* sha256;
    const char* description;
};

const SupportedExecutable* find_supported(const std::string& sha256);

// A file the game loads at a fixed address and then executes code from. Its contents are laid over
// the executable's memory image before analysis so that code is translated too.
struct CodeBlob {
    std::string name;
    u32 addr = 0;
    std::vector<u8> data;
};

// Translates a loaded executable into the analyser's input description.
Input make_input(const psp::Module& module, const psp::NidTable& nids, const std::vector<CodeBlob>& blobs = {},
                 const std::vector<u32>& extra_entries = {});

struct GenerateOptions {
    std::filesystem::path out_dir;
    u32 shards = 96;
    bool comments = true;
    std::vector<u32> extra_entries;
    std::vector<CodeBlob> blobs;
    std::vector<u32> hooks; // see EmitOptions::hooks
};

struct GenerateSummary {
    Stats stats;
    size_t functions = 0;
    size_t files_written = 0;
    size_t files_unchanged = 0;
    std::vector<std::string> unimplemented;
    std::string report; // human-readable; also written to gen_report.txt
};

bool generate(const psp::Module& module, const std::string& sha256, const psp::NidTable& nids,
              const GenerateOptions& options, GenerateSummary& summary, std::string* error);

} // namespace p2::recomp
