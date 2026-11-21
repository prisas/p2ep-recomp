// The overlay: settings menu, system dialogs, notices and the performance display.
//
// Dialogs are driven by the console's own buttons (whatever keys or gamepad they are mapped to),
// the way the console's system software drives them; the menu uses the mouse, the keyboard or a
// gamepad through Dear ImGui's navigation.

#include "runtime/ui/overlay.h"

#include "lib/common/file.h"
#include "lib/common/log.h"
#include "runtime/audio/audio_out.h"
#include "runtime/game/tweaks.h"
#include "runtime/gfx/texture_pack.h"
#include "runtime/gfx/vk/vk_renderer.h"
#include "runtime/platform/config.h"
#include "runtime/platform/host.h"
#include "runtime/platform/upscale_job.h"
#include "runtime/platform/window.h"
#include "runtime/ui/ui_shared.h"

#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <stb_image.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <cmath>

#include <windows.h>
#include <shellapi.h>

namespace p2::ui {

namespace {

constexpr u32 kDisplayWidth = 480, kDisplayHeight = 272;
constexpr int kTabCount = 5;

struct SlotDialog {
    bool open = false;
    SlotPurpose purpose = SlotPurpose::Load;
    std::vector<SaveSlot> slots;
    std::vector<u64> icons; // one per slot; 0 when there is none
    bool icons_loaded = false;
    int cursor = 0;
    int result = kCancelled;
    bool scroll_to_cursor = false;
};

struct MessageDialog {
    bool open = false;
    std::string text;
    bool yes_no = false;
    int cursor = 0; // 0 yes, 1 no
    int result = 0;
};

struct Notice {
    std::string text;
    int frames_left = 0;
};

bool g_active = false;
SDL_Window* g_window = nullptr;
gfx::Renderer* g_renderer = nullptr;
u32 g_width = 0, g_height = 0; // drawing size without a window
f32 g_ui_scale = 1.0f;
ImFont* g_title_font = nullptr;
void (*g_screen)() = nullptr; // a full-window screen drawn instead of the menu and dialogs

bool g_menu = false;
int g_force_tab = -1;
f32 g_force_scroll = -1.0f; // scroll the settings to this position (scripted runs)...
int g_force_scroll_frames = 0; // ...for a few frames: the first ones do not know the content's height yet
int g_binding = -1; // index of the button waiting for a key, or -1

SlotDialog g_slots;
MessageDialog g_message;
std::vector<Notice> g_notices;

u32 g_pad = 0, g_pad_prev = 0;
int g_repeat = 0;

f32 g_frame_ms[120] = {};
int g_frame_at = 0;
f32 g_speed = 100.0f;

// --- helpers --------------------------------------------------------------------------------------------------

ImVec2 display_size() { return ImGui::GetIO().DisplaySize; }

void changed() {
    apply_settings();
    host::save_settings();
}

// Reads textures/replace again and redraws every texture with what is there now. Returns the
// number of replacement files.
size_t reload_pack() {
    const size_t count = gfx::textures::rescan();
    if (g_renderer) {
        g_renderer->reload_textures();
        g_renderer->set_texture_pack_active(gfx::textures::dumping() || gfx::textures::replacing());
    }
    return count;
}

// When an upscaling run started from the settings has ended: use its textures and say what it did.
void check_upscale() {
    host::upscale::Result r;
    if (!host::upscale::take_result(&r)) return;
    if (r.upscaled) reload_pack();
    if (!r.message.empty()) notify(r.message);
    else if (!r.ok && r.upscaled == 0 && r.failed == 0) notify("Upscaling stopped before it finished.");
    else if (r.upscaled == 0 && r.failed == 0) notify(r.up_to_date || r.kept ? "Every dumped texture is upscaled already." : "There are no dumped textures to upscale.");
    else if (r.failed) notify(format("Upscaled %u texture(s); %u could not be done.", r.upscaled, r.failed));
    else notify(format("Upscaled %u texture(s); they are in use now.", r.upscaled));
}

void release_icons() {
    if (g_renderer)
        for (u64 icon : g_slots.icons)
            if (icon) g_renderer->destroy_ui_texture(icon);
    g_slots.icons.clear();
    g_slots.icons_loaded = false;
}

void load_icons() {
    g_slots.icons.assign(g_slots.slots.size(), 0);
    g_slots.icons_loaded = true;
    if (!g_renderer) return;
    for (size_t i = 0; i < g_slots.slots.size(); ++i) {
        const SaveSlot& slot = g_slots.slots[i];
        std::vector<u8> bytes;
        if (!slot.exists || slot.icon.empty() || !read_file(slot.icon, bytes)) continue;
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
        if (!pixels) continue;
        if (w > 0 && h > 0 && w <= 512 && h <= 512)
            g_slots.icons[i] = g_renderer->create_ui_texture(reinterpret_cast<const u32*>(pixels), u32(w), u32(h));
        stbi_image_free(pixels);
    }
}

void close_slots(int result) {
    g_slots.result = result;
    g_slots.open = false;
    release_icons();
}

// A selectable slot is one the purpose can act on: any slot when saving, existing ones otherwise.
bool selectable(const SaveSlot& slot) { return g_slots.purpose == SlotPurpose::Save || slot.exists; }

void move_cursor(int step) {
    const int count = int(g_slots.slots.size());
    for (int tries = 0; tries < count; ++tries) {
        g_slots.cursor = (g_slots.cursor + step + count) % count;
        if (selectable(g_slots.slots[size_t(g_slots.cursor)])) break;
    }
    g_slots.scroll_to_cursor = true;
}

// --- drawing --------------------------------------------------------------------------------------------------

void apply_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.34f, 0.42f, 0.60f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.20f, 0.33f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.17f, 0.32f, 0.55f, 0.75f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.40f, 0.68f, 0.85f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.46f, 0.78f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.28f, 0.46f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.38f, 0.63f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.46f, 0.78f, 1.00f);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.28f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.36f, 0.60f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.42f, 0.70f, 1.00f);
    style.ScaleAllSizes(g_ui_scale);
}

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    const f32 size = std::round(18.0f * g_ui_scale);
    const char* windir = SDL_getenv("WINDIR");
    const std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    std::error_code ec;
    ImFont* font = nullptr;
    if (std::filesystem::exists(fonts + "segoeui.ttf", ec)) font = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), size);
    if (!font) {
        g_title_font = io.Fonts->AddFontDefault();
        return;
    }
    // Save descriptions may use Japanese punctuation and full-width characters.
    for (const char* name : {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}) {
        if (!std::filesystem::exists(fonts + name, ec)) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        if (io.Fonts->AddFontFromFileTTF((fonts + name).c_str(), size, &merge, io.Fonts->GetGlyphRangesJapanese())) break;
    }
    // Headings: semibold when the system has it.
    const f32 title_size = std::round(30.0f * g_ui_scale);
    for (const char* name : {"seguisb.ttf", "segoeui.ttf"}) {
        if (!std::filesystem::exists(fonts + name, ec)) continue;
        if ((g_title_font = io.Fonts->AddFontFromFileTTF((fonts + name).c_str(), title_size))) break;
    }
    if (!g_title_font) g_title_font = font;
}

void draw_performance() {
    if (!host::settings().show_fps) return;
    const f32 pad = 8.0f * g_ui_scale;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##performance", nullptr, flags)) {
        f32 sum = 0.0f, worst = 0.0f;
        for (f32 ms : g_frame_ms) {
            sum += ms;
            worst = std::max(worst, ms);
        }
        const f32 average = sum / f32(std::size(g_frame_ms));
        ImGui::Text("%.1f fps   %.0f%% speed", average > 0.0f ? 1000.0f / average : 0.0f, double(g_speed));
        ImGui::Text("frame %.2f ms (worst %.2f)   %ux", double(average), double(worst), g_renderer ? g_renderer->scale() : 1u);
        ImGui::PlotLines("##frames", g_frame_ms, int(std::size(g_frame_ms)), g_frame_at, nullptr, 0.0f, 33.4f,
                         ImVec2(220.0f * g_ui_scale, 36.0f * g_ui_scale));
    }
    ImGui::End();
}

void draw_notices() {
    if (g_notices.empty()) return;
    const ImVec2 size = display_size();
    f32 y = size.y - 16.0f * g_ui_scale;
    for (size_t i = g_notices.size(); i-- > 0;) {
        Notice& n = g_notices[i];
        const f32 alpha = std::min(1.0f, f32(n.frames_left) / 30.0f);
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, y), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.80f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin(format("##notice%zu", i).c_str(), nullptr, flags)) {
            ImGui::TextUnformatted(n.text.c_str());
            y -= ImGui::GetWindowSize().y + 6.0f * g_ui_scale;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        --n.frames_left;
    }
    std::erase_if(g_notices, [](const Notice& n) { return n.frames_left <= 0; });
}

void button_hint(const char* confirm, const char* back) {
    ImGui::Separator();
    ImGui::TextDisabled("Cross: %s      Circle: %s", confirm, back);
}

void draw_slot_dialog() {
    if (!g_slots.open) return;
    if (!g_slots.icons_loaded) load_icons();
    const ImVec2 size = display_size();
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(size.x * 0.86f, 820.0f * g_ui_scale), size.y * 0.84f), ImGuiCond_Always);
    const char* title = g_slots.purpose == SlotPurpose::Save ? "Save" : g_slots.purpose == SlotPurpose::Load ? "Load" : "Delete";
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin(format("%s###slots", title).c_str(), nullptr, flags)) {
        const f32 footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##list", ImVec2(0.0f, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        const f32 icon_h = 80.0f * g_ui_scale * 0.8f, icon_w = icon_h * 144.0f / 80.0f;
        const f32 row_h = icon_h + ImGui::GetStyle().ItemSpacing.y;
        for (int i = 0; i < int(g_slots.slots.size()); ++i) {
            const SaveSlot& slot = g_slots.slots[size_t(i)];
            ImGui::PushID(i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(!selectable(slot));
            if (ImGui::Selectable("##row", i == g_slots.cursor, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h))) {
                g_slots.cursor = i;
                close_slots(i);
            }
            if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
                g_slots.cursor = i;
            ImGui::EndDisabled();
            if (i == g_slots.cursor && g_slots.scroll_to_cursor) ImGui::SetScrollHereY(0.5f);

            ImDrawList* draw = ImGui::GetWindowDrawList();
            const f32 inset = 4.0f * g_ui_scale;
            const ImVec2 icon_min(at.x + inset, at.y + (row_h - icon_h) * 0.5f), icon_max(icon_min.x + icon_w, icon_min.y + icon_h);
            const u64 icon = size_t(i) < g_slots.icons.size() ? g_slots.icons[size_t(i)] : 0;
            if (icon) draw->AddImage(ImTextureID(icon), icon_min, icon_max);
            else draw->AddRect(icon_min, icon_max, ImGui::GetColorU32(ImGuiCol_Border), 3.0f);

            const ImU32 text = ImGui::GetColorU32(selectable(slot) ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            const f32 text_x = icon_max.x + 12.0f * g_ui_scale, line = ImGui::GetTextLineHeight();
            if (slot.exists) {
                draw->AddText(ImVec2(text_x, icon_min.y), text, slot.title.c_str());
                // The first line of the description says where the save was made.
                const size_t eol = slot.detail.find('\n');
                const std::string first = slot.detail.substr(0, eol);
                draw->AddText(ImVec2(text_x, icon_min.y + line * 1.15f), text, first.c_str());
                draw->AddText(ImVec2(text_x, icon_min.y + line * 2.3f), dim, slot.when.c_str());
            } else {
                draw->AddText(ImVec2(text_x, icon_min.y + (icon_h - line) * 0.5f), dim,
                              g_slots.purpose == SlotPurpose::Save ? "New save data" : "No data");
            }
            const std::string label = format("%d", i + 1);
            const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
            draw->AddText(ImVec2(at.x + ImGui::GetContentRegionAvail().x - label_size.x - inset, icon_min.y), dim, label.c_str());
            ImGui::PopID();
        }
        g_slots.scroll_to_cursor = false;
        ImGui::EndChild();
        button_hint(g_slots.purpose == SlotPurpose::Save ? "save here" : g_slots.purpose == SlotPurpose::Load ? "load" : "delete", "back");
    }
    ImGui::End();
}

void draw_message() {
    if (!g_message.open) return;
    const ImVec2 size = display_size();
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * g_ui_scale, 0.0f), ImVec2(size.x * 0.8f, size.y * 0.8f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##message", nullptr, flags)) {
        ImGui::PushTextWrapPos(std::min(size.x * 0.7f, 560.0f * g_ui_scale));
        ImGui::TextUnformatted(g_message.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (g_message.yes_no) {
            const ImVec2 button(110.0f * g_ui_scale, 0.0f);
            const f32 total = button.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowSize().x - total) * 0.5f));
            for (int i = 0; i < 2; ++i) {
                if (i) ImGui::SameLine();
                const bool current = g_message.cursor == i;
                if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(i == 0 ? "Yes" : "No", button)) {
                    g_message.result = i + 1;
                    g_message.open = false;
                }
                if (current) ImGui::PopStyleColor();
            }
            button_hint("choose", "no");
        } else {
            button_hint("OK", "OK");
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_message.result = 0;
                g_message.open = false;
            }
        }
    }
    ImGui::End();
}

void general_tab() {
    host::Settings& s = host::settings();
    if (ImGui::Checkbox("Skip the intro at start-up", &s.skip_intro)) changed();
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Starts at the main menu: the notices, logos and opening movie are skipped. "
                        "Takes effect the next time the game starts.");
    ImGui::PopTextWrapPos();
    ImGui::Unindent();

    if (ImGui::Checkbox("Show the launcher at start", &s.show_launcher)) changed();
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("When off, the game starts straight away with the last disc image chosen. Hold Shift "
                        "while starting it to see the launcher anyway.");
    ImGui::PopTextWrapPos();
    ImGui::Unindent();

    ImGui::SeparatorText("Gameplay");
    int rate = int(s.encounter_rate);
    if (ImGui::SliderInt("Enemy encounters", &rate, 0, int(game::kMaxEncounterRate), "%d%%")) {
        s.encounter_rate = u32(std::clamp(rate, 0, int(game::kMaxEncounterRate)) + 2) / 5 * 5; // steps of 5%
        changed();
    }
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    if (s.encounter_rate == 100) {
        ImGui::TextDisabled("Random battles as often as in the original game.");
    } else if (s.encounter_rate > 100) {
        ImGui::TextDisabled("Random battles %.1f times as often. Rewards are unchanged.", f64(s.encounter_rate) / 100.0);
    } else {
        ImGui::TextDisabled("%s Each battle gives %.2gx the experience and money.",
                            s.encounter_rate == 0 ? "No random battles (battles the story sets up still happen)."
                                                  : "Fewer random battles.",
                            f64(game::reward_multiplier(s.encounter_rate)));
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent();
}

void video_tab() {
    host::Settings& s = host::settings();
    int scale = int(s.scale);
    const char* scales = "Match the window\0"
                         "1x  (480 x 272)\0"
                         "2x  (960 x 544)\0"
                         "3x  (1440 x 816)\0"
                         "4x  (1920 x 1088)\0"
                         "5x  (2400 x 1360)\0"
                         "6x  (2880 x 1632)\0"
                         "7x  (3360 x 1904)\0"
                         "8x  (3840 x 2176)\0"
                         "9x  (4320 x 2448)\0"
                         "10x (4800 x 2720)\0";
    if (ImGui::Combo("Rendering resolution", &scale, scales)) {
        s.scale = u32(scale);
        changed();
    }
    int scaling = int(s.scaling);
    if (ImGui::Combo("Picture size", &scaling, "Fit the window\0Whole multiples only\0Stretch to fill\0")) {
        s.scaling = host::Scaling(scaling);
        changed();
    }
    if (ImGui::Checkbox("Smooth scaling", &s.smooth)) changed();

    // Window size, in pixels: multiples of the console's 480x272 and common sizes that fit the display.
    const u32 window_w = s.run_window_width ? s.run_window_width : s.window_width;
    const u32 window_h = s.run_window_width ? s.run_window_height : s.window_height;
    auto label = [](u32 w, u32 h, u32 multiple) {
        return multiple ? format("%u x %u  (%ux)", w, h, multiple) : format("%u x %u", w, h);
    };
    const std::vector<host::WindowSize> choices = host::window_size_choices();
    std::string current = format("%u x %u  (custom)", window_w, window_h);
    for (const host::WindowSize& c : choices)
        if (c.width == window_w && c.height == window_h) current = label(c.width, c.height, c.multiple);
    if (ImGui::BeginCombo("Window size", current.c_str())) {
        for (const host::WindowSize& c : choices) {
            const bool selected = c.width == window_w && c.height == window_h;
            if (ImGui::Selectable(label(c.width, c.height, c.multiple).c_str(), selected)) {
                s.window_width = c.width;
                s.window_height = c.height;
                s.run_window_width = s.run_window_height = 0;
                changed();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (s.fullscreen) {
        ImGui::Indent();
        ImGui::TextDisabled("Used when the game is not in full screen.");
        ImGui::Unindent();
    }
    if (ImGui::Checkbox("Full screen", &s.fullscreen)) changed();
    if (ImGui::Checkbox("Vertical sync", &s.vsync)) changed();
    if (ImGui::Checkbox("Show performance", &s.show_fps)) changed();

    ImGui::SeparatorText("Textures");
    if (ImGui::Checkbox("Use replacement textures", &s.replace_textures)) changed();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) notify(format("%zu replacement texture(s) found.", reload_pack()));
    ImGui::Indent();
    const size_t replacements = gfx::textures::replacement_count();
    if (replacements) ImGui::TextDisabled("%zu replacement texture(s) available.", replacements);
    else ImGui::TextDisabled("No texture pack: put one in textures\\replace in the user folder.");
    ImGui::Unindent();

    // Community packs, next to the program: they win over everything in textures\replace.
    ImGui::BeginDisabled(!s.replace_textures);
    if (ImGui::Checkbox("Use community texture packs", &s.community_packs)) changed();
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    const std::vector<gfx::textures::CommunityPack> packs = gfx::textures::community_packs();
    if (!s.community_packs || !s.replace_textures) {
        ImGui::TextDisabled("Hand-made and AI-upscaled packs made by players, drawn before anything else.");
    } else if (packs.empty()) {
        ImGui::TextDisabled("None installed: they go in the texturepacks folder next to the program.");
    } else {
        for (const gfx::textures::CommunityPack& p : packs)
            ImGui::TextDisabled("%s, by %s (%s): %zu of its %zu texture(s) used so far.", p.name.c_str(),
                                p.author.empty() ? "unknown" : p.author.c_str(), p.licence.empty() ? "licence not stated" : p.licence.c_str(),
                                p.used, p.textures);
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent();
    ImGui::EndDisabled();

    // The upscaled set drawn (and made by the button below): one per method, kept side by side.
    const bool realesrgan = host::upscale::realesrgan_available();
    const char* const method_labels[2] = {"xBR (sharp pixel art, fast)", "Real-ESRGAN (AI, on the GPU)"};
    const int method = s.upscaler == gfx::textures::kUpscalerRealEsrgan ? 1 : 0;
    if (ImGui::BeginCombo("Upscaler", method_labels[method])) {
        for (int m = 0; m < 2; ++m) {
            ImGui::BeginDisabled(m == 1 && !realesrgan);
            if (ImGui::Selectable(method_labels[m], m == method)) {
                s.upscaler = m == 1 ? gfx::textures::kUpscalerRealEsrgan : gfx::textures::kUpscalerXbr;
                changed();
            }
            ImGui::EndDisabled();
            if (m == method) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (s.upscaler == gfx::textures::kUpscalerRealEsrgan) {
        const std::vector<std::string> models = host::upscale::realesrgan_models();
        if (ImGui::BeginCombo("Model", s.realesrgan_model.c_str())) {
            for (const std::string& m : models) {
                if (ImGui::Selectable(m.c_str(), m == s.realesrgan_model)) {
                    s.realesrgan_model = m;
                    changed();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    if (!realesrgan)
        ImGui::TextDisabled("Real-ESRGAN is not installed: it goes in the realesrgan folder next to the program.");
    else if (s.upscaler == gfx::textures::kUpscalerRealEsrgan)
        ImGui::TextDisabled("Detailed on drawn art; uses the graphics card, so the game may slow down while it works. "
                            "realesrgan-x4plus-anime suits this game's art.");
    else
        ImGui::TextDisabled("Each method keeps its own set in textures\\replace\\upscaled: switching back needs no new upscaling.");
    ImGui::PopTextWrapPos();
    ImGui::Unindent();

    bool dump = s.dump_textures || s.run_dump_textures;
    ImGui::BeginDisabled(s.run_dump_textures);
    if (ImGui::Checkbox("Dump textures", &dump)) {
        s.dump_textures = dump;
        changed();
    }
    ImGui::EndDisabled();
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Writes each texture the game draws to textures\\dump in the user folder, once, as PNG: the "
                        "starting point for a texture pack.");
    if (gfx::textures::dumping())
        ImGui::TextDisabled("%llu written so far.", static_cast<unsigned long long>(gfx::textures::dumped_count()));
    ImGui::PopTextWrapPos();
    ImGui::Unindent();

    // The dumps made into a pack by p2texup.exe, in the background; the pack reloads when it is done.
    const bool tool = host::upscale::available(), busy = host::upscale::running();
    const bool method_ready = s.upscaler != gfx::textures::kUpscalerRealEsrgan || realesrgan;
    ImGui::BeginDisabled(!tool || busy || !method_ready);
    if (ImGui::Button("Upscale dumped textures")) {
        std::string error;
        if (!host::upscale::start(host::options().user_dir, s.upscaler, s.realesrgan_model, &error)) notify(error);
    }
    ImGui::EndDisabled();
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.0f);
    if (busy) {
        const host::upscale::Progress p = host::upscale::progress();
        if (p.total) ImGui::TextDisabled("Upscaling: %u of %u...", p.done, p.total);
        else ImGui::TextDisabled("Upscaling: looking for textures to do...");
    } else if (!tool) {
        ImGui::TextDisabled("Needs p2texup.exe next to the program.");
    } else {
        ImGui::TextDisabled("Makes 4x versions of the dumped textures with the upscaler above, in the background, "
                            "and uses them. Textures done before are skipped, and so are those with a replacement "
                            "of your own.");
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent();

    if (ImGui::Button("Open the textures folder")) {
        std::error_code ec;
        const std::filesystem::path folder = gfx::textures::dump_folder().parent_path();
        std::filesystem::create_directories(folder, ec);
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void audio_tab() {
    host::Settings& s = host::settings();
    int volume = int(s.volume);
    if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%")) {
        s.volume = u32(volume);
        changed();
    }
    if (ImGui::Checkbox("Reverb on sound effects", &s.reverb)) changed();
    ImGui::TextDisabled("Where the game asks for it. An approximation of the console's effect.");
}

void controls_tab() {
    host::Settings& s = host::settings();
    static constexpr u32 speeds[] = {0, 2, 3, 4, 8};
    int speed = 0;
    for (int i = 0; i < int(std::size(speeds)); ++i)
        if (speeds[i] == s.turbo_speed) speed = i;
    if (ImGui::Combo("Fast-forward speed", &speed, "Unlimited\0"
                                                   "2x\0"
                                                   "3x\0"
                                                   "4x\0"
                                                   "8x\0")) {
        s.turbo_speed = speeds[speed];
        changed();
    }
    ImGui::TextDisabled("Hold Tab or the right trigger to fast-forward.");
    ImGui::Spacing();

    if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(host::kPadButtonCount); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(host::kPadButtonNames[i]);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (g_binding == i) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "press a key (Esc cancels)");
            else ImGui::TextUnformatted(SDL_GetScancodeName(SDL_Scancode(s.keys[i])));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(g_binding == i ? "Cancel" : "Change")) g_binding = g_binding == i ? -1 : i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Restore default keys")) {
        s.reset_keys();
        g_binding = -1;
        changed();
    }
    ImGui::TextDisabled("A gamepad works without setup: its buttons map to the console's by position.");
}

void help_tab() {
    ImGui::TextUnformatted("Esc or F1");
    ImGui::SameLine(160.0f * g_ui_scale);
    ImGui::TextDisabled("open or close the settings during play (the game pauses)");
    ImGui::TextUnformatted("F11 or Alt+Enter");
    ImGui::SameLine(160.0f * g_ui_scale);
    ImGui::TextDisabled("full screen");
    ImGui::TextUnformatted("Tab (hold)");
    ImGui::SameLine(160.0f * g_ui_scale);
    ImGui::TextDisabled("fast-forward");
    ImGui::Spacing();
    ImGui::TextWrapped("Saves are ordinary files in the user folder, under ms0/PSP/SAVEDATA. Settings are kept in "
                       "settings.ini next to them.");
}

void draw_menu() {
    if (!g_menu) return;
    const ImVec2 size = display_size();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), size, IM_COL32(0, 0, 0, 120));
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(size.x * 0.92f, 640.0f * g_ui_scale), std::min(size.y * 0.92f, 520.0f * g_ui_scale)),
                             ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (ImGui::Begin("Settings", &open, flags)) {
        const f32 footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##tabs", ImVec2(0.0f, -footer));
        settings_tabs();
        if (g_force_scroll_frames > 0) {
            ImGui::SetScrollY(g_force_scroll * g_ui_scale);
            --g_force_scroll_frames;
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("Resume")) open = false;
        ImGui::SameLine();
        if (ImGui::Button("Quit the game")) host::request_quit();
    }
    ImGui::End();
    if (!open) {
        g_menu = false;
        g_binding = -1;
    }
}

void build() {
    check_upscale();
    ImGuiIO& io = ImGui::GetIO();
    if (g_window) {
        ImGui_ImplSDL3_NewFrame();
    } else {
        io.DisplaySize = ImVec2(f32(g_width), f32(g_height));
        io.DeltaTime = 1.0f / 60.0f;
    }
    ImGui::NewFrame();
    if (g_screen) {
        g_screen();
        draw_notices();
    } else {
        draw_performance();
        draw_slot_dialog();
        draw_message();
        draw_notices();
        draw_menu();
    }
    ImGui::Render();
}

} // namespace

ImFont* title_font() { return g_title_font ? g_title_font : ImGui::GetFont(); }
f32 ui_scale() { return g_ui_scale; }

void settings_tabs() {
    if (!ImGui::BeginTabBar("##bar")) return;
    static constexpr const char* names[kTabCount] = {"General", "Video", "Audio", "Controls", "Help"};
    static void (*const tabs[kTabCount])() = {general_tab, video_tab, audio_tab, controls_tab, help_tab};
    for (int i = 0; i < kTabCount; ++i) {
        const ImGuiTabItemFlags tab_flags = g_force_tab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(names[i], nullptr, tab_flags)) {
            ImGui::Spacing();
            tabs[i]();
            ImGui::EndTabItem();
        }
    }
    g_force_tab = -1;
    ImGui::EndTabBar();
}

void set_screen(void (*draw)()) {
    g_screen = draw;
    g_menu = false;
    g_binding = -1;
}

// --- lifecycle ------------------------------------------------------------------------------------------------

bool init(void* sdl_window, u32 width, u32 height) {
    g_window = static_cast<SDL_Window*>(sdl_window);
    g_width = width;
    g_height = height;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing about the overlay's layout is worth remembering
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    g_ui_scale = g_window ? std::clamp(SDL_GetWindowDisplayScale(g_window), 1.0f, 3.0f) : 1.0f;
    load_fonts();
    apply_style();
    if (g_window && !ImGui_ImplSDL3_InitForVulkan(g_window)) {
        ImGui::DestroyContext();
        return false;
    }
    g_active = true;
    return true;
}

void shutdown() {
    if (!g_active) return;
    g_active = false;
    g_renderer = nullptr;
    if (g_window) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g_window = nullptr;
}

bool active() { return g_active; }

void attach(gfx::Renderer* renderer) {
    g_renderer = renderer;
    if (!renderer) return;
    if (g_active) renderer->set_overlay_builder(build);
    apply_settings();
}

void apply_settings() {
    const host::Settings& s = host::settings();
    audio::output_set_volume(f32(s.volume) / 100.0f);
    host::set_fullscreen(s.fullscreen);
    if (s.run_window_width) host::set_window_size(s.run_window_width, s.run_window_height);
    else host::set_window_size(s.window_width, s.window_height);
    if (!g_renderer) return;

    // Texture packs: switching replacement on or off, or which files it uses, redraws every texture
    // from scratch.
    static std::string applied_upscaler = s.upscaler;
    static bool applied_community = s.community_packs;
    const bool choice_changed = s.upscaler != applied_upscaler || s.community_packs != applied_community;
    applied_upscaler = s.upscaler;
    applied_community = s.community_packs;
    const bool was_replacing = gfx::textures::replacing();
    gfx::textures::set_upscaler(s.upscaler);
    gfx::textures::set_community(s.community_packs);
    gfx::textures::set_replacing(s.replace_textures);
    gfx::textures::set_dumping(s.dump_textures || s.run_dump_textures);
    if (gfx::textures::replacing() != was_replacing || (choice_changed && gfx::textures::replacing())) g_renderer->reload_textures();
    g_renderer->set_texture_pack_active(gfx::textures::dumping() || gfx::textures::replacing());

    g_renderer->set_vsync(s.vsync && !s.run_unthrottled);
    g_renderer->set_fit(gfx::Fit(u32(s.scaling)), s.smooth);
    u32 scale = s.run_scale ? s.run_scale : s.scale;
    if (scale == 0) { // follow the output: enough pixels that the picture is never magnified
        u32 w = 0, h = 0;
        g_renderer->output_size(w, h);
        const f32 factor = std::min(f32(w) / f32(kDisplayWidth), f32(h) / f32(kDisplayHeight));
        scale = std::max<u32>(u32(std::ceil(factor)), 1);
    }
    g_renderer->set_scale(scale);
}

// --- input ----------------------------------------------------------------------------------------------------

bool process_event(const void* sdl_event) {
    if (!g_active || !g_window) return false;
    const SDL_Event* event = static_cast<const SDL_Event*>(sdl_event);
    if (g_binding >= 0 && event->type == SDL_EVENT_KEY_DOWN) {
        const SDL_Scancode key = event->key.scancode;
        if (key != SDL_SCANCODE_ESCAPE && key != SDL_SCANCODE_F1 && key != SDL_SCANCODE_F11 && key != SDL_SCANCODE_TAB) {
            host::Settings& s = host::settings();
            // A key drives one button: take it away from whichever had it.
            for (u32 i = 0; i < host::kPadButtonCount; ++i)
                if (s.keys[i] == u32(key) && int(i) != g_binding) s.keys[i] = s.keys[g_binding];
            s.keys[g_binding] = u32(key);
            changed();
        }
        g_binding = -1;
        return true;
    }
    ImGui_ImplSDL3_ProcessEvent(event);
    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && host::settings().scale == 0) apply_settings();
    return false;
}

void set_pad(u32 buttons) {
    g_pad_prev = g_pad;
    g_pad = buttons;
    u32 pressed = g_pad & ~g_pad_prev;
    // Holding a direction repeats it.
    const u32 directions = g_pad & (host::kPadUp | host::kPadDown);
    if (directions && directions == (g_pad_prev & (host::kPadUp | host::kPadDown))) {
        if (++g_repeat > 24 && g_repeat % 6 == 0) pressed |= directions;
    } else {
        g_repeat = 0;
    }
    if (g_menu) return;

    if (g_message.open) {
        if (g_message.yes_no) {
            if (pressed & (host::kPadLeft | host::kPadRight | host::kPadUp | host::kPadDown)) g_message.cursor ^= 1;
            if (pressed & host::kPadCross) {
                g_message.result = g_message.cursor + 1;
                g_message.open = false;
            } else if (pressed & host::kPadCircle) {
                g_message.result = 2;
                g_message.open = false;
            }
        } else if (pressed & (host::kPadCross | host::kPadCircle)) {
            g_message.result = (pressed & host::kPadCross) ? 0 : 3;
            g_message.open = false;
        }
        return;
    }
    if (g_slots.open) {
        if (pressed & host::kPadUp) move_cursor(-1);
        if (pressed & host::kPadDown) move_cursor(1);
        if (pressed & host::kPadCircle) close_slots(kCancelled);
        else if ((pressed & host::kPadCross) && !g_slots.slots.empty() && selectable(g_slots.slots[size_t(g_slots.cursor)]))
            close_slots(g_slots.cursor);
    }
}

bool captures_pad() { return g_active && (g_menu || g_slots.open || g_message.open); }

void toggle_menu() {
    if (!g_active || g_screen) return;
    g_menu = !g_menu;
    g_binding = -1;
}

void open_menu(int tab, f32 scroll) {
    if (!g_active || g_screen) return;
    g_menu = true;
    g_force_tab = std::clamp(tab, 0, kTabCount - 1);
    g_force_scroll = scroll;
    g_force_scroll_frames = scroll >= 0.0f ? 3 : 0;
}

bool menu_open() { return g_active && g_menu; }

void frame_stats(f32 frame_ms, f32 speed_percent) {
    g_frame_ms[g_frame_at] = frame_ms;
    g_frame_at = (g_frame_at + 1) % int(std::size(g_frame_ms));
    g_speed = speed_percent;
}

void notify(const std::string& text) {
    if (!g_active) return;
    if (g_notices.size() >= 4) g_notices.erase(g_notices.begin());
    g_notices.push_back({text, 210});
}

// --- system dialogs -------------------------------------------------------------------------------------------

bool interactive() { return g_active; }

void open_slot_dialog(SlotPurpose purpose, std::vector<SaveSlot> slots, int focus) {
    release_icons();
    g_slots.open = true;
    g_slots.purpose = purpose;
    g_slots.slots = std::move(slots);
    g_slots.result = kCancelled;
    g_slots.cursor = std::clamp(focus, 0, std::max(0, int(g_slots.slots.size()) - 1));
    g_slots.scroll_to_cursor = true;
    if (!g_slots.slots.empty() && !selectable(g_slots.slots[size_t(g_slots.cursor)])) move_cursor(1);
    g_pad_prev = g_pad; // a button still held from the game is not a press here
}

int slot_dialog_result() { return g_slots.open ? kPending : g_slots.result; }

void open_message(const std::string& text, bool yes_no, bool default_no) {
    g_message.open = true;
    g_message.text = text;
    g_message.yes_no = yes_no;
    g_message.cursor = default_no ? 1 : 0;
    g_message.result = 0;
    g_pad_prev = g_pad;
}

int message_result() { return g_message.open ? kPending : g_message.result; }

} // namespace p2::ui
