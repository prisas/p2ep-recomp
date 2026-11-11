#include "lib/psp/nid.h"

#include "lib/common/file.h"
#include "lib/common/hash.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace p2::psp {

u32 nid_of(std::string_view name) {
    auto digest = sha1(name.data(), name.size());
    return load_le<u32>(digest.data());
}

std::string NidTable::key(std::string_view lib, u32 nid) {
    return format("%.*s:%08X", int(lib.size()), lib.data(), nid);
}

bool NidTable::load_text(std::string_view text, std::string* error) {
    std::string lib;
    size_t line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;

        if (size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                if (error) *error = format("line %zu: unterminated library header", line_no);
                return false;
            }
            lib = std::string(line.substr(1, line.size() - 2));
            continue;
        }
        if (lib.empty()) {
            if (error) *error = format("line %zu: function listed before any [library] header", line_no);
            return false;
        }

        u32 nid;
        std::string_view name = line;
        if (line.size() > 2 && line[0] == '0' && (line[1] == 'x' || line[1] == 'X')) {
            size_t space = line.find_first_of(" \t");
            if (space == std::string_view::npos) {
                if (error) *error = format("line %zu: explicit NID without a name", line_no);
                return false;
            }
            std::string hex(line.substr(2, space - 2));
            nid = u32(std::strtoul(hex.c_str(), nullptr, 16));
            name = line.substr(space);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        } else {
            nid = nid_of(name);
        }
        names_[key(lib, nid)] = std::string(name);
    }
    return true;
}

bool NidTable::load_dir(const std::filesystem::path& dir, std::string* error) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".txt") files.push_back(entry.path());
    if (ec) {
        if (error) *error = "cannot list " + dir.string();
        return false;
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::string text, inner;
        if (!read_text_file(file, text)) {
            if (error) *error = "cannot read " + file.string();
            return false;
        }
        if (!load_text(text, &inner)) {
            if (error) *error = file.filename().string() + ": " + inner;
            return false;
        }
    }
    return true;
}

const char* NidTable::lookup(std::string_view lib, u32 nid) const {
    auto it = names_.find(key(lib, nid));
    return it == names_.end() ? nullptr : it->second.c_str();
}

} // namespace p2::psp
