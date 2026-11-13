#pragma once

#include "runtime/ge/renderer.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace p2::gfx {

struct RendererOptions {
    void* sdl_window = nullptr; // SDL_Window*; null renders off-screen only
    u32 scale = 1;              // internal resolution multiplier
    bool vsync = true;
    // Draw the Dear ImGui overlay over the picture. The ImGui context must exist already.
    bool overlay = false;
    // Without a window: also compose the final picture, overlay included, into an image of this
    // size so it can be captured (0: skip the composition).
    u32 offscreen_width = 0, offscreen_height = 0;
    std::filesystem::path cache_dir; // where the pipeline cache is kept; empty: no cache on disk
};

// How the game picture is fitted to the output.
enum class Fit : u32 { Aspect, Integer, Stretch };

// The renderer as the host sees it: the graphics-engine backend plus presentation controls.
class Renderer : public ge::Renderer {
public:
    virtual void set_scale(u32 scale) = 0;
    virtual u32 scale() const = 0;
    virtual void set_vsync(bool on) = 0;
    virtual void set_fit(Fit fit, bool smooth) = 0;

    // Size of the final picture (window or off-screen image) in pixels; zero when there is none.
    virtual void output_size(u32& width, u32& height) const = 0;
    // Called once per presented frame to build the overlay (ImGui::NewFrame() ... ImGui::Render()).
    virtual void set_overlay_builder(std::function<void()> build) = 0;
    // Reads back the off-screen final picture as RGBA8. False when there is none.
    virtual bool capture_output(std::vector<u8>& rgba, u32& width, u32& height) = 0;

    // Images the overlay can draw. The handle is an ImGui texture id; 0 means failure.
    virtual u64 create_ui_texture(const u32* rgba, u32 width, u32 height) = 0;
    virtual void destroy_ui_texture(u64 handle) = 0;
};

std::unique_ptr<Renderer> create_vulkan_renderer(const RendererOptions& options, std::string* error);

} // namespace p2::gfx
