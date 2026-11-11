#pragma once

#include "lib/disc/image.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace p2::disc {

struct IsoEntry {
    std::string path; // forward slashes, no leading slash, original case
    u32 lba = 0;
    u32 size = 0;
    bool is_dir = false;
};

// Read-only ISO9660 directory tree (the flat level-1 names a UMD uses; no Joliet/Rock Ridge).
class IsoFs {
public:
    bool mount(Image* image, std::string* error);

    Image* image() const { return image_; }
    const std::string& volume_id() const { return volume_id_; }
    const std::vector<IsoEntry>& entries() const { return entries_; }

    // Case-insensitive; accepts either slash direction and an optional leading slash. "" is the root.
    const IsoEntry* find(std::string_view path) const;
    std::vector<const IsoEntry*> list(std::string_view dir) const;

    bool read(const IsoEntry& entry, u64 offset, void* dst, size_t size) const;
    bool read_all(std::string_view path, std::vector<u8>& out) const;

private:
    bool walk(u32 lba, u32 size, const std::string& prefix, int depth, std::string* error);
    static std::string normalize(std::string_view path);

    Image* image_ = nullptr;
    std::string volume_id_;
    std::vector<IsoEntry> entries_;
    std::unordered_map<std::string, size_t> index_;
};

} // namespace p2::disc
