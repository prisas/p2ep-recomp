#pragma once

#include "lib/common/types.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace p2::recomp {

// --- input ----------------------------------------------------------------------------------------------------

struct CodeRange {
    u32 start = 0;
    u32 end = 0; // exclusive
    bool contains(u32 a) const { return a >= start && a < end; }
};

struct ImportStub {
    std::string lib;
    std::string name; // resolved function name; empty when the NID is unknown
    u32 nid = 0;
    u32 addr = 0;
    u32 index = 0;
};

// What the analyser needs to know about a loaded executable. Built from a psp::Module for the
// game, or by hand in the tests.
struct Input {
    u32 base = 0;                // address of image[0]
    std::vector<u8> image;       // relocated memory image
    std::vector<CodeRange> code; // translatable code, sorted, non-overlapping
    std::vector<ImportStub> stubs;
    std::vector<u32> entries; // known function entry points (module entry, exports, ...)

    // Relocation-derived facts. Both are optional: without them only direct calls are discovered.
    std::vector<std::pair<u32, u32>> word_refs; // (address of a data word, absolute address it holds)
    std::vector<std::pair<u32, u32>> code_refs; // (address of a `lui`, full address its pair forms)

    bool in_code(u32 addr) const;
    bool in_image(u32 addr, u32 size = 4) const { return addr >= base && addr - base <= image.size() - size; }
    u32 word(u32 addr) const { return load_le<u32>(image.data() + (addr - base)); }
    u32 range_end(u32 addr) const; // end of the code range containing addr (0 if none)
};

// --- output ---------------------------------------------------------------------------------------------------

struct JumpTable {
    u32 jr_pc = 0;
    u32 table_addr = 0;
    std::vector<u32> targets;
};

struct Block {
    u32 start = 0;
    u32 end = 0;            // exclusive; includes the delay slot of a terminating branch
    bool loop_head = false; // target of a backward branch: must poll the time slice
};

// A function is the code reachable from `entry` without leaving [entry, limit). Addresses inside
// it that are taken as pointers elsewhere (switch labels, callbacks into the middle of a routine)
// stay callable as secondary entries rather than splitting the function.
struct Function {
    u32 entry = 0;
    u32 limit = 0;
    std::vector<Block> blocks;  // sorted by address
    std::vector<u32> secondary; // sorted; each is the start of a block
    u32 insn_count = 0;
};

// Why an address became a function of its own (first reason seen).
enum class Origin : u8 { Seed, DirectCall, DataPointer, CodePointer, BranchTarget, Extra };

struct EntryOrigin {
    Origin kind = Origin::Seed;
    u32 from = 0; // referencing address, when there is one
};

struct Stats {
    size_t code_words = 0;
    size_t covered_words = 0;
    size_t direct_call_targets = 0;
    size_t pointer_targets = 0;   // distinct code addresses taken as pointers
    size_t secondary_entries = 0; // pointer targets that live inside another function
    size_t jump_tables = 0;
    size_t jump_table_targets = 0;
    size_t indirect_jumps_unresolved = 0;
    size_t traps = 0;
    size_t passes = 0;
    size_t by_origin[6] = {};
};

struct Program {
    const Input* input = nullptr;
    std::set<u32> entries;                            // one Function each
    std::unordered_set<u32> secondary;                // callable addresses inside a function
    std::unordered_map<u32, EntryOrigin> origins;
    std::map<u32, JumpTable> tables;                  // keyed by the address of the jr
    std::unordered_map<u32, const ImportStub*> stubs; // keyed by stub address
    std::vector<Function> functions;                  // sorted by entry
    std::set<u32> trap_pcs;                           // addresses that could not be decoded
    std::vector<u32> unresolved_jumps;                // jr through a register with no recognised table
    Stats stats;

    // True when the address has a generated function symbol (a function or a secondary entry).
    bool callable(u32 addr) const { return entries.count(addr) != 0 || secondary.count(addr) != 0; }
    const ImportStub* stub_at(u32 addr) const {
        auto it = stubs.find(addr);
        return it == stubs.end() ? nullptr : it->second;
    }
};

// Discovers functions and their control flow. `extra_entries` forces additional function starts.
bool analyze(const Input& input, const std::vector<u32>& extra_entries, Program& out, std::string* error);

} // namespace p2::recomp
