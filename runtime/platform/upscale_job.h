#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <string>
#include <vector>

// Runs the texture upscaler (p2texup.exe, next to this program) in the background, at low priority
// so the game keeps its speed, and follows its progress. See tools/texup.
namespace p2::host::upscale {

// True when p2texup.exe is next to this program.
bool available();
// True when Real-ESRGAN is installed next to this program (realesrgan\realesrgan-ncnn-vulkan.exe).
bool realesrgan_available();
// The Real-ESRGAN models installed (empty without Real-ESRGAN).
std::vector<std::string> realesrgan_models();

// Starts upscaling the dumps of `user_dir` with `method` ("xbr" or "realesrgan"; `model` for the
// latter). Returns false with `error` set when it cannot start.
bool start(const std::filesystem::path& user_dir, const std::string& method, const std::string& model, std::string* error);
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
    std::string message;  // why nothing could be done, when the program says
};
// True once after a run has ended, with what it did.
bool take_result(Result* result);

// Ends a run still going (when the game quits). Files are written under temporary names first, so
// nothing half-written is left behind.
void stop();

} // namespace p2::host::upscale
