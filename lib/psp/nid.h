#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace p2::psp {

// A PSP import is identified by a 32-bit NID: the first four bytes of the SHA-1 of the function
// name, read as a little-endian integer. Later firmware revisions assign a few NIDs arbitrarily.
u32 nid_of(std::string_view name);

// Maps (library, NID) to function names. Loaded from plain-text lists:
//
//     # comment
//     [sceDisplay]
//     sceDisplaySetMode                 NID derived from the name
//     0x12345678 sceSomethingRenamed    explicit NID
class NidTable {
public:
    bool load_text(std::string_view text, std::string* error);
    bool load_dir(const std::filesystem::path& dir, std::string* error);

    // Returns nullptr when the NID is unknown for that library.
    const char* lookup(std::string_view lib, u32 nid) const;
    size_t size() const { return names_.size(); }

private:
    static std::string key(std::string_view lib, u32 nid);
    std::unordered_map<std::string, std::string> names_;
};

} // namespace p2::psp
