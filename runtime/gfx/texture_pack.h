#pragma once

#include "runtime/gfx/texture_id.h"

#include <filesystem>
#include <memory>
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

struct Image {
    u32 width = 0, height = 0;
    std::vector<u32> rgba; // R in the low byte, rows top to bottom
};

void init(const std::filesystem::path& user_dir);
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
// Reads the folder again. Returns the number of replacement files found.
size_t rescan();
size_t replacement_count();
bool has_replacement(u64 id);
// Memory the game's movie player writes pictures to. Textures read from there change every frame
// and are neither dumped nor replaced.
void exclude_range(u32 guest_begin, u32 guest_end);
bool excluded(u32 guest_addr);

// The replacement image once loaded. Until then it schedules the load, returns null and sets
// `*pending`; a replacement that cannot be used returns null with `*pending` false.
std::shared_ptr<const Image> replacement(u64 id, bool* pending);

} // namespace p2::gfx::textures
