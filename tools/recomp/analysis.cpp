#include "tools/recomp/analysis.h"

#include "lib/allegrex/insn.h"
#include "lib/common/file.h"

#include <algorithm>

namespace p2::recomp {

using allegrex::Insn;
using allegrex::Op;

bool Input::in_code(u32 addr) const {
    for (const CodeRange& r : code)
        if (r.contains(addr)) return true;
    return false;
}

u32 Input::range_end(u32 addr) const {
    for (const CodeRange& r : code)
        if (r.contains(addr)) return r.end;
    return 0;
}

namespace {

// Branches whose condition is a tautology: no fall-through path exists.
bool always_taken(const Insn& i) {
    switch (i.op) {
    case Op::Beq:
    case Op::Beql: return i.rs() == i.rt();
    case Op::Bgez:
    case Op::Bgezl:
    case Op::Blez:
    case Op::Blezl: return i.rs() == 0;
    default: return false;
    }
}

bool is_call(const Insn& i) {
    return i.op == Op::Jal || i.op == Op::Jalr || ((i.flags() & allegrex::kBranch) && (i.flags() & allegrex::kLink));
}

struct FnResult {
    Function fn;
    std::vector<std::pair<u32, u32>> escapes; // (target, from): branch targets outside [entry, limit)
    std::vector<u32> traps;                   // undecodable words reached by control flow
    std::vector<u32> unresolved;              // register jumps without a table
    u32 first_uncovered = 0;                  // lowest pointer target in range that the flow never reaches
    u32 decoded_words = 0;
};

enum : u8 { kDecoded = 1, kLeader = 2, kSlot = 4, kLoopHead = 8 };

class Analyzer {
public:
    Analyzer(const Input& in, Program& prog) : in_(in), prog_(prog) {}

    bool run(const std::vector<u32>& extra_entries, std::string* error) {
        for (const ImportStub& s : in_.stubs) prog_.stubs[s.addr] = &s;
        for (auto [addr, target] : in_.word_refs) word_ref_[addr] = target;
        for (auto [pc, target] : in_.code_refs) {
            code_ref_[pc] = target;
            if (!in_.in_code(target)) data_refs_.insert(target);
        }

        sweep();
        find_jump_tables();
        seed_entries(extra_entries);
        if (prog_.entries.empty()) {
            if (error) *error = "no function entry points were found";
            return false;
        }
        iterate();
        finish();
        return true;
    }

private:
    // Linear pass over all code: direct call targets and register jumps.
    void sweep() {
        for (const CodeRange& range : in_.code) {
            for (u32 pc = range.start; pc + 4 <= range.end; pc += 4) {
                Insn i(in_.word(pc));
                ++prog_.stats.code_words;
                if (i.op == Op::Jal) {
                    u32 t = i.jump_target(pc);
                    if (in_.in_code(t)) direct_calls_.insert(t);
                } else if ((i.flags() & allegrex::kBranch) && (i.flags() & allegrex::kLink)) {
                    u32 t = i.branch_target(pc);
                    if (in_.in_code(t)) direct_calls_.insert(t);
                } else if (i.op == Op::Jr && i.rs() != 31) {
                    jr_sites_.push_back(pc);
                }
            }
        }
        prog_.stats.direct_call_targets = direct_calls_.size();
    }

    bool code_pointer_at(u32 addr, u32* target) const {
        auto it = word_ref_.find(addr);
        if (it == word_ref_.end() || !in_.in_code(it->second) || (it->second & 3)) return false;
        *target = it->second;
        return true;
    }

    // A switch statement compiles to a bounds check, a table address formed by a relocated `lui`
    // pair, a load and `jr`. The table is a run of relocated code pointers. Recognising it lets
    // the emitter turn the jump into a C++ switch; it is purely an optimisation, because every
    // table slot's target is callable through the dispatch table as well.
    void find_jump_tables() {
        for (u32 pc : jr_sites_) {
            u32 range_start = 0;
            for (const CodeRange& r : in_.code)
                if (r.contains(pc)) range_start = r.start;

            u32 table = 0, table_ref_pc = 0, bound = 0;
            for (u32 back = 1; back <= 40; ++back) {
                if (pc < range_start + back * 4) break;
                u32 a = pc - back * 4;
                Insn i(in_.word(a));
                if (i.op == Op::Jr) break; // previous function or previous switch
                if (!table) {
                    auto it = code_ref_.find(a);
                    u32 first;
                    if (it != code_ref_.end() && !(it->second & 3) && !in_.in_code(it->second) &&
                        in_.in_image(it->second) && code_pointer_at(it->second, &first)) {
                        table = it->second;
                        table_ref_pc = a;
                    }
                } else if (i.op == Op::Sltiu && table_ref_pc - a <= 16 * 4) {
                    bound = i.uimm();
                    break;
                }
            }
            if (!table) continue;

            JumpTable jt;
            jt.jr_pc = pc;
            jt.table_addr = table;
            for (u32 k = 0; k < 4096; ++k) {
                u32 slot = table + k * 4;
                u32 target;
                if (!in_.in_image(slot) || !code_pointer_at(slot, &target)) break;
                if (k > 0 && data_refs_.count(slot)) break; // another object starts here
                if (bound && k >= bound) break;
                jt.targets.push_back(target);
            }
            if (jt.targets.empty()) continue;
            prog_.stats.jump_table_targets += jt.targets.size();
            prog_.tables[pc] = std::move(jt);
        }
        prog_.stats.jump_tables = prog_.tables.size();
    }

    bool valid_entry(u32 addr) const { return (addr & 3) == 0 && in_.in_code(addr); }

    void add_function(u32 addr, Origin kind, u32 from = 0) {
        if (!valid_entry(addr)) return;
        if (prog_.entries.insert(addr).second) prog_.origins[addr] = {kind, from};
    }

    void seed_entries(const std::vector<u32>& extra_entries) {
        for (const CodeRange& r : in_.code) add_function(r.start, Origin::Seed);
        for (u32 e : in_.entries) add_function(e, Origin::Seed);
        for (u32 e : extra_entries) add_function(e, Origin::Extra);
        for (u32 e : direct_calls_) add_function(e, Origin::DirectCall);

        // Every code address stored in data or formed by an instruction pair must stay callable.
        // Whether it is a function of its own or a label inside one is decided by the flow analysis.
        for (auto [addr, target] : in_.word_refs)
            if (valid_entry(target) && pointer_targets_.insert(target).second)
                pointer_origin_[target] = {Origin::DataPointer, addr};
        for (auto [pc, target] : in_.code_refs)
            if (valid_entry(target) && pointer_targets_.insert(target).second)
                pointer_origin_[target] = {Origin::CodePointer, pc};
        prog_.stats.pointer_targets = pointer_targets_.size();
    }

    FnResult analyze_function(u32 entry, u32 limit) {
        FnResult r;
        r.fn.entry = entry;
        r.fn.limit = limit;
        const u32 words = (limit - entry) / 4;
        std::vector<u8> state(words, 0);
        std::vector<u32> work{entry};
        state[0] |= kLeader;

        auto add_target = [&](u32 target, u32 from) {
            if (target & 3) return;
            if (target >= entry && target < limit) {
                u8& s = state[(target - entry) / 4];
                if (target <= from) s |= kLoopHead;
                if (!(s & kLeader)) {
                    s |= kLeader;
                    work.push_back(target);
                }
            } else if (in_.in_code(target)) {
                r.escapes.push_back({target, from});
            }
            // Anything else (an import stub or an address outside the image) is resolved by the emitter.
        };

        u32 last_pc = entry; // highest address the flow has reached
        auto run_flow = [&]() {
        while (!work.empty()) {
            u32 pc = work.back();
            work.pop_back();
            while (pc < limit) {
                u8& s = state[(pc - entry) / 4];
                if (s & kDecoded) break;
                s |= kDecoded;
                last_pc = std::max(last_pc, pc);

                Insn i(in_.word(pc));
                if (i.op == Op::Invalid) {
                    r.traps.push_back(pc);
                    break;
                }
                if (!i.has_delay_slot()) {
                    pc += 4;
                    continue;
                }

                u32 slot = pc + 4;
                if (!in_.in_code(slot)) {
                    r.traps.push_back(pc);
                    break;
                }
                Insn d(in_.word(slot));
                if (d.op == Op::Invalid || d.has_delay_slot()) {
                    r.traps.push_back(pc);
                    break;
                }
                if (slot < limit) {
                    state[(slot - entry) / 4] |= kSlot;
                    last_pc = std::max(last_pc, slot);
                }

                if (is_call(i)) {
                    pc += 8;
                    continue;
                }
                if (i.flags() & allegrex::kBranch) {
                    add_target(i.branch_target(pc), pc);
                    if (!always_taken(i)) add_target(pc + 8, pc);
                } else if (i.op == Op::J) {
                    add_target(i.jump_target(pc), pc);
                } else if (i.op == Op::Jr && i.rs() != 31) {
                    auto it = prog_.tables.find(pc);
                    if (it != prog_.tables.end()) {
                        for (u32 t : it->second.targets) add_target(t, pc);
                    } else {
                        r.unresolved.push_back(pc);
                    }
                }
                break;
            }
        }
        };
        run_flow();

        // Pointer targets inside the range become secondary entries. One the flow did not reach
        // is still part of this function when the function's code continues past it (a switch
        // slot the bounds check excludes, for instance): it is analysed as an extra root. The
        // first one beyond the end of the function starts a function of its own.
        for (auto it = pointer_targets_.upper_bound(entry); it != pointer_targets_.end() && *it < limit; ++it) {
            const u32 target = *it;
            const u32 index = (target - entry) / 4;
            if (!(state[index] & kDecoded)) {
                if (target > last_pc) {
                    r.first_uncovered = target;
                    break;
                }
                state[index] |= kLeader;
                work.push_back(target);
                run_flow();
            }
            state[index] |= kLeader;
            r.fn.secondary.push_back(target);
        }
        if (r.first_uncovered) return r; // the caller shrinks the range and retries

        for (u32 w = 0; w < words; ++w)
            if (state[w] & (kDecoded | kSlot)) ++r.decoded_words;

        // Carve blocks out of the leaders.
        for (u32 w = 0; w < words; ++w) {
            if ((state[w] & (kLeader | kDecoded)) != (kLeader | kDecoded)) continue;
            Block b;
            b.start = entry + w * 4;
            b.loop_head = (state[w] & kLoopHead) != 0;
            u32 pc = b.start;
            while (true) {
                if (pc >= limit) break;
                if (pc != b.start && (state[(pc - entry) / 4] & kLeader)) break;
                Insn i(in_.word(pc));
                ++r.fn.insn_count;
                if (i.op == Op::Invalid) {
                    pc += 4;
                    break;
                }
                if (!i.has_delay_slot()) {
                    pc += 4;
                    continue;
                }
                ++r.fn.insn_count;
                pc += 8;
                if (!is_call(i)) break;
            }
            b.end = pc;
            r.fn.blocks.push_back(b);
        }
        return r;
    }

    u32 limit_of(std::set<u32>::const_iterator it) const {
        u32 limit = in_.range_end(*it);
        auto next = std::next(it);
        if (next != prog_.entries.end() && *next < limit) limit = *next;
        return limit;
    }

    void iterate() {
        bool changed = true;
        while (changed) {
            changed = false;
            ++prog_.stats.passes;
            std::vector<std::pair<u32, u32>> escapes;

            // Entries inserted during the walk lie above the current one and are visited in turn.
            for (auto it = prog_.entries.begin(); it != prog_.entries.end(); ++it) {
                const u32 entry = *it;
                while (true) {
                    const u32 limit = limit_of(it);
                    auto cached = results_.find(entry);
                    if (cached != results_.end() && cached->second.fn.limit == limit) break;

                    FnResult r = analyze_function(entry, limit);
                    if (r.first_uncovered) {
                        prog_.entries.insert(r.first_uncovered);
                        prog_.origins[r.first_uncovered] = pointer_origin_[r.first_uncovered];
                        continue;
                    }
                    for (const auto& e : r.escapes) escapes.push_back(e);
                    results_[entry] = std::move(r);
                    break;
                }
            }

            // A branch out of its function lands either on another function's entry (a tail call)
            // or inside another function. The latter becomes a pointer target of that function,
            // which keeps the function whole and makes the label a secondary entry.
            for (auto [target, from] : escapes) {
                if (prog_.entries.count(target) || !pointer_targets_.insert(target).second) continue;
                pointer_origin_[target] = {Origin::BranchTarget, from};
                auto owner = prog_.entries.upper_bound(target);
                if (owner != prog_.entries.begin()) results_.erase(*std::prev(owner));
                changed = true;
            }
        }
    }

    void finish() {
        for (u32 entry : prog_.entries) {
            FnResult& r = results_[entry];
            prog_.stats.covered_words += r.decoded_words;
            prog_.stats.traps += r.traps.size();
            for (u32 pc : r.traps) prog_.trap_pcs.insert(pc);
            for (u32 pc : r.unresolved) prog_.unresolved_jumps.push_back(pc);
            for (u32 s : r.fn.secondary) prog_.secondary.insert(s);
            ++prog_.stats.by_origin[size_t(prog_.origins[entry].kind)];
            prog_.functions.push_back(std::move(r.fn));
        }
        std::sort(prog_.unresolved_jumps.begin(), prog_.unresolved_jumps.end());
        prog_.stats.indirect_jumps_unresolved = prog_.unresolved_jumps.size();
        prog_.stats.secondary_entries = prog_.secondary.size();
    }

    const Input& in_;
    Program& prog_;
    std::unordered_map<u32, u32> word_ref_;
    std::unordered_map<u32, u32> code_ref_;
    std::unordered_set<u32> data_refs_;
    std::set<u32> pointer_targets_;
    std::unordered_map<u32, EntryOrigin> pointer_origin_;
    std::set<u32> direct_calls_;
    std::vector<u32> jr_sites_;
    std::map<u32, FnResult> results_;
};

} // namespace

bool analyze(const Input& input, const std::vector<u32>& extra_entries, Program& out, std::string* error) {
    out = Program{};
    out.input = &input;
    Analyzer analyzer(input, out);
    return analyzer.run(extra_entries, error);
}

} // namespace p2::recomp
