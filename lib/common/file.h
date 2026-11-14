#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace p2 {

bool read_file(const std::filesystem::path& path, std::vector<u8>& out);
bool read_text_file(const std::filesystem::path& path, std::string& out);
bool write_file(const std::filesystem::path& path, const void* data, size_t size);

// Leaves the file (and its timestamp) untouched when the content is already identical, so
// regenerating sources does not trigger needless recompiles. Returns false on I/O failure.
bool write_file_if_changed(const std::filesystem::path& path, std::string_view content, bool* changed = nullptr);

// Paths to and from UTF-8 text (settings files, SDL), whatever the system's code page.
std::filesystem::path utf8_path(std::string_view text);
std::string path_utf8(const std::filesystem::path& path);
// UTF-16 text (the command line, Windows calls) as UTF-8.
std::string to_utf8(const wchar_t* text);

// The folder holding the running program.
std::filesystem::path program_folder();

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void append_format(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace p2
