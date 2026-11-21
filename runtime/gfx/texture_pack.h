#pragma once

#include "runtime/gfx/ppsspp_pack.h"
#include "runtime/gfx/texture_id.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Texture dumping and replacement ("texture packs").
//
// Every texture the game draws with has an ID: a hash of its decoded pixels and size, the same on
// every machine and whatever memory the game keeps it in. Dumping writes each texture once as
// <user dir>/textures/dump/<id>.png. A texture pack is a set of PNG files with the same names, at
// any resolution, under <user dir>/textures/replace/ (subfolders allowed); a texture that has a file
// there is drawn with the file instead.
//
// Files are read and written on a background thread: dumping never stalls the game, and a
// replacement is shown from the first frame after it has finished loading.
namespace p2::gfx::textures {

// The subfolder of textures/replace that the texture upscaler (tools/texup) writes to, one subfolder
// per method. Only the chosen method's set is used. Files directly in it (the layout before there
// were several methods) count as the xBR set. A file anywhere else in textures/replace wins over one
// there with the same name.
constexpr const char* kUpscaledFolder = "upscaled";
constexpr const char* kUpscalerXbr = "xbr";
constexpr const char* kUpscalerRealEsrgan = "realesrgan";

// The upscaling method a file in kUpscaledFolder belongs to, from its path relative to that folder.
std::string upscaler_of(const std::filesystem::path& relative);

// Community texture packs in PPSSPP's format, one per subfolder of the program's texturepacks folder
// (see ppsspp_pack.h). They win over every file in textures/replace.
struct CommunityPack {
    std::string name, author, licence;
    size_t textures = 0; // files it has
    size_t used = 0;     // of those, used since start-up
};

struct Image {
    u32 width = 0, height = 0;
    std::vector<u32> rgba; // R in the low byte, rows top to bottom
};

// `packs_dir` holds the community packs (may be empty or missing).
void init(const std::filesystem::path& user_dir, const std::filesystem::path& packs_dir = {});
// Writes out the dumps still queued and stops the background thread.
void shutdown();

std::filesystem::path dump_folder();
std::filesystem::path replace_folder();

void set_dumping(bool on);
bool dumping();
// Queues a texture to be written, unless a file for it exists already.
void dump(u64 id, const u32* rgba, u32 width, u32 height);
// Textures written since start-up.
u64 dumped_count();

// Turning replacement on (re)reads the folder; replacing() is false while it holds no files.
void set_replacing(bool on);
bool replacing();
// Which upscaled set (kUpscalerXbr or kUpscalerRealEsrgan) is used; changing it reads the folder again.
void set_upscaler(const std::string& method);
// Whether the community packs are used (they are read with the folder); changing it reads them again.
void set_community(bool on);
// Reads the folder (and the community packs) again. Returns the number of replacement files found.
size_t rescan();
size_t replacement_count();
bool has_replacement(u64 id);
std::vector<CommunityPack> community_packs();
// The replacement ID of a community pack's file for this texture, or 0 when no pack has one. Pass
// it to replacement() like any other ID.
u64 community_id(const ppsspp::TextureInfo& tex);
// Memory the game's movie player writes pictures to. Textures read from there change every frame
// and are neither dumped nor replaced.
void exclude_range(u32 guest_begin, u32 guest_end);
bool excluded(u32 guest_addr);

// The replacement image once loaded. Until then it schedules the load, returns null and sets
// `*pending`; a replacement that cannot be used returns null with `*pending` false.
std::shared_ptr<const Image> replacement(u64 id, bool* pending);

} // namespace p2::gfx::textures
