#pragma once

#include "lib/disc/iso9660.h"
#include "lib/psp/prx.h"
#include "lib/psp/sfo.h"

#include <filesystem>
#include <memory>
#include <string>

namespace p2::recomp {

// Everything the tool reads from a disc image: the file system, the game description and the executable.
struct Game {
    std::unique_ptr<disc::Image> image;
    disc::IsoFs fs;
    psp::Sfo sfo;
    std::vector<u8> executable; // raw file contents
    std::string executable_sha256;
    psp::Module module; // relocated to the default load base

    bool load(const std::filesystem::path& image_path, std::string* error);
};

constexpr const char* kExecutablePath = "PSP_GAME/SYSDIR/EBOOT.BIN";

} // namespace p2::recomp
