#include "runtime/platform/host.h"

#include <memory>

namespace p2::host {

namespace {
Options g_options;
std::unique_ptr<disc::Image> g_image;
disc::IsoFs g_iso;
} // namespace

Options& options() { return g_options; }
disc::Image* image() { return g_image.get(); }
disc::IsoFs& iso() { return g_iso; }

bool open_image(const std::filesystem::path& path, std::string* error) {
    g_image = disc::Image::open(path, error);
    if (!g_image) return false;
    return g_iso.mount(g_image.get(), error);
}

std::filesystem::path memory_stick_dir() {
    std::filesystem::path dir = g_options.user_dir / "ms0";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace p2::host
