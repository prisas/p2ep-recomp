#pragma once

#include "runtime/ge/renderer.h"

#include <memory>
#include <string>

namespace p2::gfx {

struct RendererOptions {
    void* sdl_window = nullptr; // SDL_Window*; null renders off-screen only
    u32 scale = 1;              // internal resolution multiplier
    bool vsync = true;
};

// Creates the Vulkan implementation of the graphics-engine renderer.
std::unique_ptr<ge::Renderer> create_vulkan_renderer(const RendererOptions& options, std::string* error);

} // namespace p2::gfx
