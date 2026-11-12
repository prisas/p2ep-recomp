#pragma once

#include "tools/recomp/analysis.h"

#include <map>
#include <string>
#include <vector>

namespace p2::recomp {

struct EmitOptions {
    u32 shards = 96;
    bool comments = true; // annotate every statement with the guest address and disassembly
    std::string sha256;   // identity of the executable the code is generated from
    u32 module_base = 0;
    u32 module_end = 0;
    u32 module_entry = 0;
    u32 module_gp = 0;
};

struct EmitResult {
    // File name -> contents: gen_config.h, gen_funcs.h, gen_table.cpp, gen_000.cpp ...
    std::map<std::string, std::string> files;
    // Instructions reachable by control flow that the emitter cannot translate.
    std::vector<std::string> unimplemented;
    u32 text_base = 0;
    u32 text_words = 0;
};

void emit(const Program& program, const EmitOptions& options, EmitResult& out);

std::string function_name(u32 addr);
std::string import_thunk_name(const ImportStub& stub);
std::string import_hle_name(const ImportStub& stub);

} // namespace p2::recomp
