#pragma once

#include "lib/disc/iso9660.h"

#include <filesystem>
#include <string>

// Host-side resources shared by the system-call implementations.
namespace p2::host {

struct Options {
    std::filesystem::path image;    // the user's disc image
    std::filesystem::path user_dir; // saves, configuration, caches
    bool headless = false;          // no window, no audio device
    bool unthrottled = false;       // run as fast as possible
    u64 frame_limit = 0;            // stop after this many displayed frames (0: never)
    std::filesystem::path dump_dir; // write displayed frames here as PNG
    u32 dump_every = 0;             // every Nth frame (0: never)
    std::filesystem::path texture_dump_dir; // write every decoded texture here as PNG (debugging)
};

Options& options();

disc::Image* image();
disc::IsoFs& iso();
bool open_image(const std::filesystem::path& path, std::string* error);

// Directory backing the memory stick ("ms0:").
std::filesystem::path memory_stick_dir();

} // namespace p2::host
