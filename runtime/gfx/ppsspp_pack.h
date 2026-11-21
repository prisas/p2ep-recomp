#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Texture packs in the format of the PSP emulator PPSSPP, used as they are.
//
// Such a pack is a folder with a textures.ini and PNG files. A texture is named by 24 hexadecimal
// digits: a 64-bit "cache key" made from the texture's address, size and palette, then a 32-bit hash
// of its data in guest memory, each computed the way that emulator does (the [options] of
// textures.ini choose the hash and whether the address counts). [hashes] maps names to files.
// Nothing here depends on how this program decodes or names textures: the keys come from the GE
// registers and guest memory alone.
namespace p2::gfx::ppsspp {

enum class Hash { Xxh32, Xxh64 };

struct Options {
    Hash hash = Hash::Xxh64;
    bool ignore_address = false;
    bool reduce_hash = false;
    std::map<u64, f32> reduce_ranges;                     // (w << 16 | h) -> fraction of the data hashed
    std::map<u64, std::pair<u32, u32>> hash_ranges;       // (addr << 32 | w << 16 | h) -> hashed w, h
};

// A texture as the game sets it up for a draw.
struct TextureInfo {
    u32 addr = 0;
    u32 format = 0;           // GE texture format, 0-10
    u32 width = 0, height = 0;
    u32 buf_width = 0;        // the buffer width register's 11 bits
    const u8* data = nullptr; // guest memory at addr
    size_t data_size = 0;     // bytes readable from there
    // Indexed formats: the palette buffer as loaded (it keeps older entries past the last load),
    // the number of its bytes that count, and the palette format register's 24 bits.
    const u8* clut = nullptr;
    u32 clut_bytes = 0;
    u32 clut_format_reg = 0;
};

struct Key {
    u64 cache_key = 0;
    u32 hash = 0;
    bool operator==(const Key& o) const { return cache_key == o.cache_key && hash == o.hash; }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return size_t(k.cache_key * 0x9E3779B97F4A7C15ull ^ k.hash); }
};

// "%016llx%08x", as the files are named.
std::string key_name(const Key& key);
// Reads 24 hexadecimal digits (an optional "_<level>" suffix is ignored; *level gets it, 0 without).
bool parse_key(std::string_view text, Key* key, int* level = nullptr);

// The palette's part of the key: XXH32 of the palette bytes, xored with the palette format command.
u32 clut_hash(const u8* clut, u32 bytes, u32 clut_format_reg);
// The address and size part (address dropped when the pack ignores it), palette mixed in.
u64 cache_key(const Options& options, const TextureInfo& tex);
// The data hash with the texture taken as `hash_height` rows tall (normally its height). Returns 0
// when the data runs past readable memory, as the emulator does.
u32 data_hash(const Options& options, const TextureInfo& tex, u32 hash_height);
// Data hashes for every height PPSSPP may use for a 512-row texture (its "max seen V": 512, or the
// lowest row drawn from when that is 272 or more), in one pass: `out[h - 272]` for h = 272..512.
void tall_data_hashes(const Options& options, const TextureInfo& tex, std::vector<u32>* out);

struct Pack {
    std::filesystem::path root;  // the folder with textures.ini
    std::string name, author, licence;
    u32 order = 100;             // breaks resolution ties: lower wins
    Options options;
    std::unordered_map<Key, std::filesystem::path, KeyHash> files;
    std::unordered_set<u32> key_words; // low 32 bits of every cache key, to rule out most textures fast
};

// Reads textures.ini. Keys in [hashes] with an empty value or a missing file are left out.
bool parse_ini(const std::string& text, Options* options, std::vector<std::pair<Key, std::string>>* hashes,
               std::string* error);
// Loads a pack from `folder`: textures.ini there or in a subfolder (as the packs ship, in a folder named
// after the game). pack.ini next to it, if any, gives name, author, licence, order and keys to leave out.
bool load_pack(const std::filesystem::path& folder, Pack* pack, std::string* error);

struct Match {
    const Pack* pack = nullptr;
    const std::filesystem::path* file = nullptr;
    Key key;
};
// The file for a texture: among the packs that have one, the largest picture, then the lowest order.
// `size_of` gives a file's picture size (width, height), for the choice.
std::optional<Match> find(const std::vector<Pack>& packs, const TextureInfo& tex,
                          const std::function<std::pair<u32, u32>(const std::filesystem::path&)>& size_of);

// Width and height from a PNG file's header, without decoding it. (0, 0) if it is not a PNG.
std::pair<u32, u32> png_size(const std::filesystem::path& file);

} // namespace p2::gfx::ppsspp
