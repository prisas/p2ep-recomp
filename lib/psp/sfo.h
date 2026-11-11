#pragma once

#include "lib/common/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace p2::psp {

// PARAM.SFO: a small key/value table describing a game or a save.
struct SfoEntry {
    std::string key;
    bool is_int = false;
    u32 int_value = 0;
    std::string str_value;   // text entries (UTF-8)
    std::vector<u8> raw;     // binary entries keep their bytes here
    u32 max_size = 0;        // reserved size of the value on disc
    u16 format = 0;          // 0x0004 binary, 0x0204 text, 0x0404 integer
};

class Sfo {
public:
    bool parse(const u8* data, size_t size);
    std::vector<u8> serialize() const;

    const SfoEntry* find(std::string_view key) const;
    std::string get_string(std::string_view key, std::string_view fallback = {}) const;
    u32 get_int(std::string_view key, u32 fallback = 0) const;

    void set_string(std::string_view key, std::string_view value, u32 max_size);
    void set_int(std::string_view key, u32 value);
    void set_raw(std::string_view key, const u8* data, size_t size, u32 max_size);

    const std::vector<SfoEntry>& entries() const { return entries_; }

private:
    SfoEntry& upsert(std::string_view key);
    std::vector<SfoEntry> entries_;
};

} // namespace p2::psp
