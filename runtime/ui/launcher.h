#pragma once

#include <filesystem>
#include <string>

namespace p2::gfx {
class Renderer;
}

// The launcher: shown in the game window when the program starts without a disc image. The player
// picks the image, which is checked to be the game this build was made from, adjusts the settings
// and starts the game.
namespace p2::ui {

// Opens a disc image and checks it. On failure `problem` says why, in words for the player.
using ImageCheck = bool (*)(const std::filesystem::path& image, std::string* problem);

// Runs until the player starts the game, returning the image to play, or closes the window,
// returning an empty path. `image` is offered first; `problem` is what checking it found wrong
// (empty when it is fine or when there is no image).
std::filesystem::path run_launcher(gfx::Renderer& renderer, ImageCheck check, const std::filesystem::path& image,
                                   const std::string& problem);

} // namespace p2::ui
