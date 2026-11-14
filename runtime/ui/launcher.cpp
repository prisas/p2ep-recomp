#include "runtime/ui/launcher.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/gfx/vk/vk_renderer.h"
#include "runtime/platform/config.h"
#include "runtime/platform/window.h"
#include "runtime/ui/ui_shared.h"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_internal.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <stb_image.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <cmath>
#include <mutex>

#include <windows.h>

namespace p2::ui {

namespace {

struct Picture {
    u64 texture = 0; // overlay texture, 0 when there is none
    u32 width = 0, height = 0;
};

struct Launcher {
    Picture background;
    ImageCheck check = nullptr;
    std::filesystem::path image;
    std::string image_text; // the image's path as shown, UTF-8
    std::string problem;    // why the image cannot be played; empty when it can
    bool ready = false;     // the image is the game and can be played
    bool play = false;
    bool focus = true;      // move the keyboard focus to Play (or Browse) on the next frame
    std::string dialog_start; // where the file dialog opens; kept alive while it is up

    // Paths arrive from the file dialog, which answers on a thread of its own, and from drops.
    std::mutex mutex;
    std::string picked;
    bool dialog_open = false;
};
Launcher g;

constexpr SDL_DialogFileFilter kFilters[] = {{"Disc images (*.chd, *.iso)", "chd;iso"}, {"All files", "*"}};

// The background picture embedded in the program, when the build had one (resource 2, see
// runtime/CMakeLists.txt): a PNG or JPEG file as it was.
Picture load_background(gfx::Renderer& renderer) {
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!info) return {};
    const HGLOBAL data = LoadResource(nullptr, info);
    const void* bytes = data ? LockResource(data) : nullptr;
    const DWORD size = SizeofResource(nullptr, info);
    if (!bytes || !size) return {};
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), int(size), &w, &h, &channels, 4);
    if (!pixels) {
        LOG_WARN("launcher", "the background picture cannot be decoded: %s", stbi_failure_reason());
        return {};
    }
    Picture picture;
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192)
        picture = {renderer.create_ui_texture(reinterpret_cast<const u32*>(pixels), u32(w), u32(h)), u32(w), u32(h)};
    stbi_image_free(pixels);
    return picture;
}

// The picture fills the window with its shape kept; what does not fit is cut from the left and
// evenly from top and bottom, so its right-hand side stays in view. A dark veil over it keeps the
// launcher's text readable. Without a picture, a plain gradient.
void draw_background(const ImVec2& size) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const Picture& picture = g.background;
    if (!picture.texture) {
        draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, IM_COL32(20, 30, 52, 255), IM_COL32(20, 30, 52, 255),
                                      IM_COL32(6, 7, 10, 255), IM_COL32(6, 7, 10, 255));
        return;
    }
    const f32 cover = std::max(size.x / f32(picture.width), size.y / f32(picture.height));
    const f32 shown_u = std::min(1.0f, size.x / (cover * f32(picture.width)));
    const f32 shown_v = std::min(1.0f, size.y / (cover * f32(picture.height)));
    const ImVec2 uv0(1.0f - shown_u, (1.0f - shown_v) * 0.5f);
    draw->AddImage(ImTextureID(picture.texture), ImVec2(0.0f, 0.0f), size, uv0, ImVec2(1.0f, uv0.y + shown_v));
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), size, IM_COL32(10, 6, 14, 165));
}

void choose(const std::filesystem::path& image) {
    g.image = image;
    g.image_text = path_utf8(image);
    g.problem.clear();
    g.ready = g.check(image, &g.problem);
    LOG_INFO("launcher", "%s: %s", g.image_text.c_str(), g.ready ? "ready" : g.problem.c_str());
    if (g.ready) {
        host::settings().image = g.image_text;
        host::save_settings();
    }
    g.focus = true;
}

void SDLCALL dialog_done(void*, const char* const* files, int) {
    if (!files) LOG_WARN("launcher", "the file dialog failed: %s", SDL_GetError());
    std::lock_guard lock(g.mutex);
    g.dialog_open = false;
    if (files && files[0]) g.picked = files[0]; // an empty list: cancelled
}

void dropped(const char* path) {
    std::lock_guard lock(g.mutex);
    g.picked = path;
}

void browse() {
    {
        std::lock_guard lock(g.mutex);
        if (g.dialog_open) return;
        g.dialog_open = true;
    }
    g.dialog_start = g.image_text;
    LOG_INFO("launcher", "choosing a disc image");
    SDL_ShowOpenFileDialog(dialog_done, nullptr, static_cast<SDL_Window*>(host::window_handle()), kFilters,
                           int(std::size(kFilters)), g.dialog_start.empty() ? nullptr : g.dialog_start.c_str(), false);
}

// Puts the keyboard and gamepad focus on the item just drawn, outlined, so that Enter (or the
// gamepad's confirm button) presses it.
void focus_last_item() {
    ImGui::SetWindowFocus();
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    g.focus = false;
}

void draw() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 size = io.DisplaySize;
    const f32 scale = ui_scale();

    draw_background(size);
    // Hints a little brighter than elsewhere, to read well over the picture.
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.74f, 0.72f, 0.78f, 1.0f));

    // A window over the whole picture holding one column, centred, no wider than reads comfortably.
    // Only that window goes without padding: the column, the settings and the lists that drop
    // down from them keep the normal style.
    const f32 margin = 16.0f * scale, top = 18.0f * scale;
    const f32 column = std::min(size.x - 2.0f * margin, 820.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoBackground;
    const bool open = ImGui::Begin("##launcher", nullptr, flags);
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetCursorPos(ImVec2(std::floor((size.x - column) * 0.5f), top));
        ImGui::BeginChild("##column", ImVec2(column, std::max(1.0f, size.y - 2.0f * top)), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        ImGui::PushFont(title_font());
        ImGui::TextUnformatted("Persona 2: Eternal Punishment");
        ImGui::PopFont();
        ImGui::TextDisabled("Choose the game's disc image, adjust the settings, then play.");
        ImGui::Spacing();

        ImGui::SeparatorText("Disc image");
        const char* browse_label = "Browse...";
        const f32 browse_width = ImGui::CalcTextSize(browse_label).x + style.FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(-(browse_width + style.ItemSpacing.x));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true); // shown, not edited: the keyboard skips it
        ImGui::InputTextWithHint("##image", "No disc image chosen", g.image_text.data(), g.image_text.size() + 1,
                                 ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemFlag();
        ImGui::SameLine();
        if (ImGui::Button(browse_label)) browse();
        if (!g.ready) {
            ImGui::SetItemDefaultFocus();
            if (g.focus) focus_last_item();
        }
        ImGui::PushTextWrapPos(0.0f);
        if (g.image_text.empty()) {
            ImGui::TextDisabled("Choose the game's .chd or .iso file, or drop it onto this window.");
        } else if (g.ready) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f), "Ready to play.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
            ImGui::TextUnformatted(g.problem.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();

        ImGui::SeparatorText("Settings");
        // The settings sit on a dark panel of their own: most of the text is there.
        const f32 footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(9, 6, 13, 200));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * scale);
        ImGui::BeginChild("##settings", ImVec2(0.0f, -footer), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        settings_tabs();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Quit, then Play, at the right.
        const f32 button_width = std::max(ImGui::CalcTextSize("Play").x + style.FramePadding.x * 2.0f, 110.0f * scale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2.0f * button_width - style.ItemSpacing.x);
        if (ImGui::Button("Quit", ImVec2(button_width, 0.0f))) host::request_quit();
        ImGui::SameLine();
        ImGui::BeginDisabled(!g.ready);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.44f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.53f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.60f, 1.00f, 1.0f));
        if (ImGui::Button("Play", ImVec2(button_width, 0.0f))) g.play = true;
        if (g.ready) {
            ImGui::SetItemDefaultFocus(); // where the focus goes when the window gets it back
            if (g.focus) focus_last_item();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

} // namespace

std::filesystem::path run_launcher(gfx::Renderer& renderer, ImageCheck check, const std::filesystem::path& image,
                                   const std::string& problem) {
    g.background = load_background(renderer);
    g.check = check;
    g.image = image;
    g.image_text = path_utf8(image);
    g.problem = problem;
    g.ready = !image.empty() && problem.empty();
    g.play = false;
    g.focus = true;
    set_screen(draw);
    host::set_drop_handler(dropped);

    bool running = true;
    u64 next = SDL_GetTicksNS();
    while (running && !g.play) {
        running = host::pump_events();
        std::string picked;
        {
            std::lock_guard lock(g.mutex);
            picked.swap(g.picked);
        }
        if (!picked.empty()) choose(utf8_path(picked));
        renderer.present(ge::DisplayTarget{});
        // At most 60 pictures a second when vertical sync does not hold presentation back.
        next += 16'666'667;
        const u64 now = SDL_GetTicksNS();
        if (now < next) SDL_DelayPrecise(next - now);
        else next = now;
    }

    set_screen(nullptr);
    host::set_drop_handler(nullptr);
    if (g.background.texture) renderer.destroy_ui_texture(g.background.texture);
    g.background = {};
    return g.play ? g.image : std::filesystem::path();
}

} // namespace p2::ui
