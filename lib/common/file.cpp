#include "lib/common/file.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>

namespace p2 {

bool read_file(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
    return bool(in);
}

bool read_text_file(const std::filesystem::path& path, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return bool(out);
}

bool write_file_if_changed(const std::filesystem::path& path, std::string_view content, bool* changed) {
    std::string existing;
    if (read_text_file(path, existing) && existing == content) {
        if (changed) *changed = false;
        return true;
    }
    if (changed) *changed = true;
    return write_file(path, content.data(), content.size());
}

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, fmt, args);
    va_end(args);
    return out;
}

void append_format(std::string& out, const char* fmt, ...) {
    char small[512];
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(small, sizeof(small), fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(args);
        return;
    }
    if (static_cast<size_t>(n) < sizeof(small)) {
        out.append(small, static_cast<size_t>(n));
    } else {
        size_t old = out.size();
        out.resize(old + static_cast<size_t>(n));
        std::vsnprintf(out.data() + old, static_cast<size_t>(n) + 1, fmt, args);
    }
    va_end(args);
}

} // namespace p2
