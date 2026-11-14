#pragma once

#include "lib/common/types.h"

struct ImFont;

// Shared by the overlay and the launcher, which draw with the same Dear ImGui context.
namespace p2::ui {

ImFont* title_font(); // larger type for headings
f32 ui_scale();       // the display's scale factor, applied to every size

// The settings, one tab per group: what the in-game menu and the launcher show.
void settings_tabs();

// While set, the overlay draws this full-window screen instead of the game's menu and dialogs.
void set_screen(void (*draw)());

} // namespace p2::ui
