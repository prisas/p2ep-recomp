#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <string>

// Runs the texture upscaler (p2texup.exe, next to this program) in the background, at low priority
// so the game keeps its speed, and follows its progress. See tools/texup.
namespace p2::host::upscale {

// True when p2texup.exe is next to this program.
bool available();

// Starts upscaling the dumps of `user_dir`. Returns false with `error` set when it cannot start.
bool start(const std::filesystem::path& user_dir, std::string* error);
bool running();

struct Progress {
    u32 done = 0, total = 0;
};
Progress progress();

struct Result {
    bool ok = false;      // the program ran to the end without failures
    u32 upscaled = 0;     // textures written this time
    u32 up_to_date = 0;   // done in an earlier run
    u32 kept = 0;         // left alone: there is a replacement of the player's own
    u32 failed = 0;
};
// True once after a run has ended, with what it did.
bool take_result(Result* result);

// Ends a run still going (when the game quits). Files are written under temporary names first, so
// nothing half-written is left behind.
void stop();

} // namespace p2::host::upscale
