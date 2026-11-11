#include "tools/recomp/game.h"

#include "lib/common/hash.h"

namespace p2::recomp {

bool Game::load(const std::filesystem::path& image_path, std::string* error) {
    image = disc::Image::open(image_path, error);
    if (!image) return false;
    if (!fs.mount(image.get(), error)) return false;

    std::vector<u8> sfo_bytes;
    if (!fs.read_all("PSP_GAME/PARAM.SFO", sfo_bytes) || !sfo.parse(sfo_bytes.data(), sfo_bytes.size())) {
        if (error) *error = "the image has no readable PSP_GAME/PARAM.SFO; is this a PSP game disc?";
        return false;
    }
    if (!fs.read_all(kExecutablePath, executable)) {
        if (error) *error = std::string("cannot read ") + kExecutablePath + " from the image";
        return false;
    }
    executable_sha256 = to_hex(sha256(executable.data(), executable.size()));
    return psp::load_prx(executable, psp::kDefaultLoadBase, module, error);
}

} // namespace p2::recomp
