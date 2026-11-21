#pragma once

#include "tools/texup/texture_ops.h"

#include <filesystem>
#include <string>
#include <vector>

// The Real-ESRGAN upscaler (realesrgan-ncnn-vulkan, a separate program that runs its neural network
// on the GPU through Vulkan). It works on whole folders of PNG files; p2texup gives it opaque
// pictures and handles transparency itself (see main.cpp).
namespace p2::texup::realesrgan {

// Where the program is looked for by default: realesrgan\realesrgan-ncnn-vulkan.exe next to p2texup.
std::filesystem::path default_program();

// The scale a model upscales by when asked for `wanted`: the realesr-animevideov3 models do 2, 3
// and 4; the others only 4 (the result is then resized down).
u32 native_scale(const std::string& model, u32 wanted);

// The model names a models folder offers, as the program takes them (realesr-animevideov3-x2/x3/x4
// count as one, "realesr-animevideov3"). Sorted.
std::vector<std::string> list_models(const std::filesystem::path& models_dir);

// The command line that upscales every PNG of `input` into `output`, on GPU number `gpu` (-1: the
// program's own choice).
std::wstring command_line(const std::filesystem::path& program, const std::filesystem::path& input,
                          const std::filesystem::path& output, const std::string& model, u32 scale, int gpu = -1);

// True when every pixel is black although `source` has some that are not: the known fault where some
// GPUs and drivers produce only black pictures.
bool black_result(const Rgba& source, const Rgba& result);

// Runs a command line with no window, at below-normal priority, in a job that ends it if this
// program ends first. Returns its exit code (-1 if it could not start); its output goes to `output`.
int run(const std::wstring& command, std::string* output);

} // namespace p2::texup::realesrgan
