#include "lib/disc/iso9660.h"

#include <cctype>

namespace p2::disc {

std::string IsoFs::normalize(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == '/' && (out.empty() || out.back() == '/')) continue; // drop leading and doubled slashes
        out.push_back(char(std::toupper(static_cast<unsigned char>(ch))));
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

bool IsoFs::mount(Image* image, std::string* error) {
    image_ = image;
    entries_.clear();
    index_.clear();

    u8 pvd[kSectorSize];
    if (!image->read_sectors(16, 1, pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        if (error) *error = "not an ISO9660 image (primary volume descriptor missing)";
        return false;
    }
    volume_id_.assign(reinterpret_cast<const char*>(pvd + 40), 32);
    while (!volume_id_.empty() && volume_id_.back() == ' ') volume_id_.pop_back();

    entries_.push_back({"", load_le<u32>(pvd + 158), load_le<u32>(pvd + 166), true});
    index_[""] = 0;
    return walk(entries_[0].lba, entries_[0].size, "", 0, error);
}

bool IsoFs::walk(u32 lba, u32 size, const std::string& prefix, int depth, std::string* error) {
    if (depth > 16 || size > (16u << 20)) {
        if (error) *error = "ISO9660 directory tree is malformed";
        return false;
    }
    std::vector<u8> dir(align_up<size_t>(size, kSectorSize));
    if (!image_->read(u64(lba) * kSectorSize, dir.data(), dir.size())) {
        if (error) *error = "cannot read ISO9660 directory";
        return false;
    }

    size_t pos = 0;
    while (pos + 34 <= size) {
        u8 length = dir[pos];
        if (length == 0) { // records never straddle sectors; skip the padding
            pos = align_up<size_t>(pos + 1, kSectorSize);
            continue;
        }
        if (pos + length > dir.size() || length < 34) break;

        const u8* rec = dir.data() + pos;
        u8 name_len = rec[32];
        bool is_dir = (rec[25] & 2) != 0;
        if (33u + name_len <= length && !(name_len == 1 && rec[33] <= 1)) {
            std::string name(reinterpret_cast<const char*>(rec + 33), name_len);
            if (size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
            if (!name.empty() && name.back() == '.') name.pop_back();

            IsoEntry entry{prefix + name, load_le<u32>(rec + 2), load_le<u32>(rec + 10), is_dir};
            index_[normalize(entry.path)] = entries_.size();
            entries_.push_back(entry);
            if (is_dir && !walk(entry.lba, entry.size, entry.path + "/", depth + 1, error)) return false;
        }
        pos += length;
    }
    return true;
}

const IsoEntry* IsoFs::find(std::string_view path) const {
    auto it = index_.find(normalize(path));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

std::vector<const IsoEntry*> IsoFs::list(std::string_view dir) const {
    std::vector<const IsoEntry*> out;
    const IsoEntry* parent = find(dir);
    if (!parent || !parent->is_dir) return out;
    std::string prefix = parent->path.empty() ? std::string() : parent->path + "/";
    for (const IsoEntry& entry : entries_) {
        if (&entry == parent || entry.path.size() <= prefix.size()) continue;
        if (entry.path.compare(0, prefix.size(), prefix) != 0) continue;
        if (entry.path.find('/', prefix.size()) != std::string::npos) continue;
        out.push_back(&entry);
    }
    return out;
}

bool IsoFs::read(const IsoEntry& entry, u64 offset, void* dst, size_t size) const {
    if (offset > entry.size || size > entry.size - offset) return false;
    return image_->read(u64(entry.lba) * kSectorSize + offset, dst, size);
}

bool IsoFs::read_all(std::string_view path, std::vector<u8>& out) const {
    const IsoEntry* entry = find(path);
    if (!entry || entry->is_dir) return false;
    out.resize(entry->size);
    return entry->size == 0 || read(*entry, 0, out.data(), out.size());
}

} // namespace p2::disc
