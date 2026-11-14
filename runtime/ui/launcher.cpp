#include "runtime/ui/launcher.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/ge/renderer.h"
#include "runtime/platform/config.h"
#include "runtime/platform/window.h"
#include "runtime/ui/ui_shared.h"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <mutex>

namespace p2::ui {

namespace {

struct Launcher {
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

    ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), size, IM_COL32(20, 30, 52, 255),
                                                           IM_COL32(20, 30, 52, 255), IM_COL32(6, 7, 10, 255),
                                                           IM_COL32(6, 7, 10, 255));
    // One column, centred, no wider than reads comfortably.
    const f32 margin = 16.0f * scale;
    const f32 column = std::min(size.x - 2.0f * margin, 820.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::max(margin, (size.x - column) * 0.5f), 18.0f * scale));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##launcher", nullptr, flags)) {
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
        const f32 footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::BeginChild("##settings", ImVec2(0.0f, -footer));
        settings_tabs();
        ImGui::EndChild();
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
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace

std::filesystem::path run_launcher(ge::Renderer& renderer, ImageCheck check, const std::filesystem::path& image,
                                   const std::string& problem) {
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
    return g.play ? g.image : std::filesystem::path();
}

} // namespace p2::ui
