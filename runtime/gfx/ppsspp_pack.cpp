#include "runtime/gfx/ppsspp_pack.h"

#include "lib/common/file.h"
#include "runtime/gfx/texture_pack.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <xxhash.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace p2::gfx::ppsspp {

namespace {

// Bits per pixel of the GE texture formats: 5650, 5551, 4444, 8888, CLUT4, CLUT8, CLUT16, CLUT32,
// DXT1, DXT3, DXT5.
constexpr u32 kBitsPerPixel[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};
constexpr u32 kDataSeed = 0xBACD7814;
constexpr u32 kClutSeed = 0xC0108888;
constexpr u32 kClutFormatCommand = 0xC5; // the palette format register's command byte, xored in too

u32 log2_of(u32 v) {
    u32 n = 0;
    while (v > 1) {
        v >>= 1;
        ++n;
    }
    return n;
}

// The buffer width as the emulator reads it: aligned to 16 bytes, 16 bytes' worth when zero.
u32 buffer_width(const TextureInfo& tex) {
    const u32 bpp = kBitsPerPixel[tex.format];
    const u32 mask = tex.format <= 7 ? 0x7FFu & ~((128u / bpp) - 1u) : 0x7FFu;
    u32 bufw = tex.buf_width & mask;
    if (bufw == 0) bufw = 128u / bpp;
    return bufw;
}

u32 hash_bytes(Hash kind, const u8* data, size_t size) {
    return kind == Hash::Xxh32 ? XXH32(data, size, kDataSeed) : u32(XXH64(data, size, kDataSeed));
}

f32 reduce_factor(const Options& o, u32 w, u32 h) {
    if (!o.reduce_hash) return 1.0f;
    auto it = o.reduce_ranges.find(u64(w) << 16 | h);
    const f32 f = it != o.reduce_ranges.end() ? it->second : 0.5f;
    return std::clamp(f, 0.0f, 1.0f);
}

// [hashranges]: the size to hash for a texture of this address and size, if the pack says.
bool hash_range(const Options& o, u32 addr, u32 w, u32 h, u32* nw, u32* nh) {
    const u64 size_key = u64(w) << 16 | h;
    auto it = o.hash_ranges.find(u64(addr) << 32 | size_key);
    if (it == o.hash_ranges.end() && o.ignore_address) it = o.hash_ranges.find(size_key);
    if (it == o.hash_ranges.end()) return false;
    *nw = it->second.first;
    *nh = it->second.second;
    return true;
}

// Bytes hashed for a contiguous texture (buffer width no more than its width).
u32 contiguous_bytes(u32 bpp, u32 bufw, u32 w, u32 h, f32 factor) {
    const u32 total_pixels = bufw * h + (w - bufw);
    return u32(f32((bpp * total_pixels) / 8) * factor);
}

std::string trim(std::string_view v) {
    size_t a = 0, b = v.size();
    while (a < b && std::isspace(static_cast<unsigned char>(v[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(v[b - 1]))) --b;
    return std::string(v.substr(a, b - a));
}

std::string lower(std::string v) {
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return v;
}

bool truthy(const std::string& v) {
    const std::string l = lower(v);
    return l == "true" || l == "1" || l == "yes" || l == "on";
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::filesystem::path find_ini(const std::filesystem::path& folder) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(folder / "textures.ini", ec)) return folder / "textures.ini";
    for (int depth = 0; depth < 2; ++depth) {
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (it.depth() > depth) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && lower(it->path().filename().string()) == "textures.ini") return it->path();
        }
    }
    return {};
}

} // namespace

std::string key_name(const Key& key) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx%08x", static_cast<unsigned long long>(key.cache_key), key.hash);
    return text;
}

bool parse_key(std::string_view text, Key* key, int* level) {
    if (text.size() < 24) return false;
    u64 value = 0;
    for (size_t i = 0; i < 24; ++i) {
        const int d = hex_digit(text[i]);
        if (d < 0) return false;
        value = i < 16 ? (value << 4) | u64(d) : value;
        if (i == 15) key->cache_key = value;
    }
    u32 hash = 0;
    for (size_t i = 16; i < 24; ++i) hash = (hash << 4) | u32(hex_digit(text[i]));
    key->hash = hash;
    int lvl = 0;
    if (text.size() > 24) {
        if (text[24] != '_') return false;
        lvl = std::atoi(std::string(text.substr(25)).c_str());
    }
    if (level) *level = lvl;
    return true;
}

u32 clut_hash(const u8* clut, u32 bytes, u32 clut_format_reg) {
    return XXH32(clut, bytes, kClutSeed) ^ ((kClutFormatCommand << 24) | (clut_format_reg & 0xFFFFFF));
}

u64 cache_key(const Options& options, const TextureInfo& tex) {
    const u32 dim = log2_of(tex.width) | (log2_of(tex.height) << 8);
    u64 key = (u64(tex.addr & 0x3FFFFFFF) << 32) | dim;
    if (tex.format & 4) key ^= clut_hash(tex.clut, tex.clut_bytes, tex.clut_format_reg); // indexed formats
    if (options.ignore_address) key &= 0xFFFFFFFFull;
    return key;
}

u32 data_hash(const Options& options, const TextureInfo& tex, u32 hash_height) {
    if (tex.format > 10 || !tex.data) return 0;
    u32 w = tex.width, h = hash_height;
    u32 rw, rh;
    if (hash_range(options, tex.addr, tex.width, tex.height, &rw, &rh)) {
        w = rw;
        h = rh;
    }
    const u32 bpp = kBitsPerPixel[tex.format];
    const u32 bufw = buffer_width(tex);
    const f32 factor = reduce_factor(options, w, h);
    if (bufw <= w) {
        const u32 size = contiguous_bytes(bpp, bufw, w, h, factor);
        if (size > tex.data_size) return 0;
        return hash_bytes(options.hash, tex.data, size);
    }
    // Rows with gaps between them: each row hashed, then combined.
    const u32 line = u32(f32((bpp * w) / 8) * factor);
    const u32 stride = (bpp * bufw) / 8;
    if (h && size_t(h - 1) * stride + line > tex.data_size) return 0;
    u32 result = 0;
    for (u32 y = 0; y < h; ++y) result = (result * 11) ^ hash_bytes(options.hash, tex.data + size_t(y) * stride, line);
    return result;
}

void tall_data_hashes(const Options& options, const TextureInfo& tex, std::vector<u32>* out) {
    constexpr u32 kLowest = 272, kTallest = 512;
    out->assign(kTallest - kLowest + 1, 0);
    if (tex.format > 10 || !tex.data) return;
    const u32 w = tex.width;
    const u32 bpp = kBitsPerPixel[tex.format];
    const u32 bufw = buffer_width(tex);
    if (bufw <= w) {
        // Every height hashes a longer prefix of the same bytes: one streaming pass gives them all.
        std::vector<u32> sizes(out->size());
        bool growing = true;
        for (u32 h = kLowest; h <= kTallest; ++h) {
            sizes[h - kLowest] = contiguous_bytes(bpp, bufw, w, h, reduce_factor(options, w, h));
            if (h > kLowest && sizes[h - kLowest] < sizes[h - kLowest - 1]) growing = false;
        }
        if (!growing) { // reduce factors per height can make it shrink: hash each one on its own
            for (u32 h = kLowest; h <= kTallest; ++h) (*out)[h - kLowest] = data_hash(options, tex, h);
            return;
        }
        size_t fed = 0;
        if (options.hash == Hash::Xxh64) {
            XXH64_state_t* state = XXH64_createState();
            XXH64_reset(state, kDataSeed);
            for (size_t i = 0; i < sizes.size() && sizes[i] <= tex.data_size; ++i) {
                XXH64_update(state, tex.data + fed, sizes[i] - fed);
                fed = sizes[i];
                (*out)[i] = u32(XXH64_digest(state));
            }
            XXH64_freeState(state);
        } else {
            XXH32_state_t* state = XXH32_createState();
            XXH32_reset(state, kDataSeed);
            for (size_t i = 0; i < sizes.size() && sizes[i] <= tex.data_size; ++i) {
                XXH32_update(state, tex.data + fed, sizes[i] - fed);
                fed = sizes[i];
                (*out)[i] = XXH32_digest(state);
            }
            XXH32_freeState(state);
        }
        return;
    }
    // Rows with gaps: the combined row hash after each row.
    const u32 stride = (bpp * bufw) / 8;
    u32 result = 0;
    for (u32 y = 0; y < kTallest; ++y) {
        const u32 h = y + 1;
        const u32 line = u32(f32((bpp * w) / 8) * reduce_factor(options, w, h));
        if (size_t(y) * stride + line > tex.data_size) break;
        // A per-height reduce factor would change every row's length: recompute those heights alone.
        if (h >= kLowest && options.reduce_ranges.count(u64(w) << 16 | h)) {
            (*out)[h - kLowest] = data_hash(options, tex, h);
            continue;
        }
        result = (result * 11) ^ hash_bytes(options.hash, tex.data + size_t(y) * stride, line);
        if (h >= kLowest) (*out)[h - kLowest] = result;
    }
}

bool parse_ini(const std::string& text, Options* options, std::vector<std::pair<Key, std::string>>* hashes,
               std::string* error) {
    std::string section;
    bool have_hash = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(std::string_view(text).substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            section = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section == "options") {
            const std::string k = lower(key);
            if (k == "hash") {
                const std::string v = lower(value);
                if (v == "xxh64") options->hash = Hash::Xxh64;
                else if (v == "xxh32") options->hash = Hash::Xxh32;
                else {
                    if (error) *error = "unsupported hash '" + value + "' (only xxh32 and xxh64 are)";
                    return false;
                }
                have_hash = true;
            } else if (k == "ignoreaddress") {
                options->ignore_address = truthy(value);
            } else if (k == "reducehash") {
                options->reduce_hash = truthy(value);
            }
            // version, ignoreMipmap (only the full-size level is used here) and the rest: nothing to do.
        } else if (section == "hashes") {
            Key k;
            int level = 0;
            if (!parse_key(key, &k, &level) || level != 0) continue; // smaller mipmap levels are not used
            std::string file = value.substr(0, value.find('|'));  // several files: one per level
            std::replace(file.begin(), file.end(), '\\', '/');
            hashes->emplace_back(k, file);
        } else if (section == "hashranges") {
            // addr,w,h = w2,h2
            unsigned addr = 0, w = 0, h = 0, w2 = 0, h2 = 0;
            std::string a = key;
            if (a.size() > 2 && a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) a = a.substr(2);
            if (std::sscanf(a.c_str(), "%x,%u,%u", &addr, &w, &h) == 3 && std::sscanf(value.c_str(), "%u,%u", &w2, &h2) == 2 &&
                w && h && w2 && h2 && w2 <= w && h2 <= h && w <= 1024 && h <= 1024)
                options->hash_ranges[u64(addr) << 32 | u64(w) << 16 | h] = {w2, h2};
        } else if (section == "reducehashranges") {
            unsigned w = 0, h = 0;
            const f32 f = std::strtof(value.c_str(), nullptr);
            if (std::sscanf(key.c_str(), "%u,%u", &w, &h) == 2 && f != 0.0f) options->reduce_ranges[u64(w) << 16 | h] = f;
        }
    }
    if (!have_hash) {
        if (error) *error = "textures.ini has no hash setting";
        return false;
    }
    return true;
}

bool load_pack(const std::filesystem::path& folder, Pack* pack, std::string* error) {
    const std::filesystem::path ini = find_ini(folder);
    if (ini.empty()) {
        if (error) *error = "no textures.ini";
        return false;
    }
    std::vector<u8> bytes;
    if (!read_file(ini, bytes)) {
        if (error) *error = "cannot read " + path_utf8(ini);
        return false;
    }
    pack->root = ini.parent_path();
    pack->options = {};
    pack->files.clear();
    pack->key_words.clear();
    std::vector<std::pair<Key, std::string>> hashes;
    if (!parse_ini(std::string(bytes.begin(), bytes.end()), &pack->options, &hashes, error)) return false;

    // Our own description of the pack, next to it: name, author, licence, order, and keys to
    // leave out ("exclude = <24 hex digits>, ...").
    pack->name = folder.filename().string();
    std::vector<Key> excluded;
    std::vector<u8> info;
    if (read_file(folder / "pack.ini", info)) {
        const std::string text(info.begin(), info.end());
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            const std::string line = trim(std::string_view(text).substr(p, e - p));
            p = e + 1;
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            const std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
            if (k == "name") pack->name = v;
            else if (k == "author") pack->author = v;
            else if (k == "licence" || k == "license") pack->licence = v;
            else if (k == "order") pack->order = u32(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "exclude") {
                size_t s = 0;
                while (s < v.size()) {
                    size_t c = v.find(',', s);
                    if (c == std::string::npos) c = v.size();
                    Key key;
                    if (parse_key(trim(std::string_view(v).substr(s, c - s)), &key)) excluded.push_back(key);
                    s = c + 1;
                }
            }
        }
    }

    std::error_code ec;
    // Hash-named files at the top of the pack count without a [hashes] line; listed ones come after,
    // so a listing wins (a later line for the same key wins too, as the emulator reads them).
    for (const auto& entry : std::filesystem::directory_iterator(pack->root, ec)) {
        Key k;
        int level = 0;
        const std::string stem = entry.path().stem().string();
        if (entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".png" && stem.size() == 24 &&
            parse_key(stem, &k, &level))
            pack->files[k] = entry.path();
    }
    for (const auto& [k, file] : hashes) {
        if (file.empty() || file.find("..") != std::string::npos) continue;
        const std::filesystem::path path = pack->root / utf8_path(file);
        if (std::filesystem::is_regular_file(path, ec)) pack->files[k] = path;
    }
    for (const Key& k : excluded) pack->files.erase(k);
    for (const auto& [k, file] : pack->files) pack->key_words.insert(u32(k.cache_key));
    return true;
}

std::optional<Match> find(const std::vector<Pack>& packs, const TextureInfo& tex,
                          const std::function<std::pair<u32, u32>(const std::filesystem::path&)>& size_of) {
    std::optional<Match> best;
    u64 best_pixels = 0;
    // Packs with the same options share their hashes.
    struct Computed {
        const Options* options;
        u64 cache_key;
        bool hashed = false;
        std::vector<u32> hashes; // the data hashes to try, most likely first
    };
    std::vector<Computed> computed;
    computed.reserve(packs.size());
    for (const Pack& pack : packs) {
        const Options& o = pack.options;
        auto same = std::find_if(computed.begin(), computed.end(), [&](const Computed& c) {
            return c.options->hash == o.hash && c.options->ignore_address == o.ignore_address &&
                   c.options->reduce_hash == o.reduce_hash && c.options->reduce_ranges == o.reduce_ranges &&
                   c.options->hash_ranges == o.hash_ranges;
        });
        Computed* c = same != computed.end() ? &*same : &computed.emplace_back(Computed{&o, cache_key(o, tex)});
        // Most textures stop here: nothing in this pack has their address, size and palette.
        if (!pack.key_words.count(u32(c->cache_key))) continue;
        if (!c->hashed) {
            c->hashed = true;
            u32 rw, rh;
            if (tex.height == 512 && !hash_range(o, tex.addr, tex.width, tex.height, &rw, &rh)) {
                // The emulator may have hashed fewer rows of a 512-row texture: try them all.
                std::vector<u32> tall;
                tall_data_hashes(o, tex, &tall);
                c->hashes.push_back(tall.back());
                for (size_t i = 0; i + 1 < tall.size(); ++i) c->hashes.push_back(tall[i]);
            } else {
                c->hashes.push_back(data_hash(o, tex, tex.height));
            }
        }
        const std::filesystem::path* file = nullptr;
        Key key;
        for (u32 h : c->hashes) {
            auto it = pack.files.find(Key{c->cache_key, h});
            if (it != pack.files.end()) {
                file = &it->second;
                key = it->first;
                break;
            }
        }
        if (!file) { // a listing that leaves the data hash out matches any data
            auto it = pack.files.find(Key{c->cache_key & 0xFFFFFFFFull, 0});
            if (it != pack.files.end()) {
                file = &it->second;
                key = it->first;
            }
        }
        if (!file) continue;
        const auto [w, h] = size_of(*file);
        const u64 pixels = u64(w) * h;
        if (!best || pixels > best_pixels || (pixels == best_pixels && pack.order < best->pack->order)) {
            best = Match{&pack, file, key};
            best_pixels = pixels;
        }
    }
    return best;
}

std::pair<u32, u32> png_size(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    u8 head[24] = {};
    if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return {0, 0};
    static constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!std::equal(kSignature, kSignature + 8, head) || std::string_view(reinterpret_cast<char*>(head + 12), 4) != "IHDR")
        return {0, 0};
    auto be32 = [&](int at) { return u32(head[at]) << 24 | u32(head[at + 1]) << 16 | u32(head[at + 2]) << 8 | u32(head[at + 3]); };
    return {be32(16), be32(20)};
}

} // namespace p2::gfx::ppsspp

namespace p2::gfx::textures {

// Here rather than in texture_pack.cpp so the unit tests can use it without the renderer.
std::string upscaler_of(const std::filesystem::path& relative) {
    if (std::distance(relative.begin(), relative.end()) > 1) {
        // A subfolder: its name is the method (another name is a set no setting picks).
        std::string first = relative.begin()->string();
        for (char& ch : first) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return first;
    }
    return kUpscalerXbr; // directly in the folder: from before there were several methods
}

} // namespace p2::gfx::textures
