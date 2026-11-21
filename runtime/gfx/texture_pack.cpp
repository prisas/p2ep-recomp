#include "runtime/gfx/texture_pack.h"

#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "lib/common/log.h"
#include "runtime/platform/frame.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <stb_image.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace p2::gfx::textures {

namespace {

constexpr size_t kMaxQueuedDumps = 512;            // beyond this, dumps wait for a later frame
constexpr size_t kLoadedBudget = size_t(768) << 20; // decoded replacements kept in memory
constexpr u32 kMaxReplacementSide = 8192;

std::filesystem::path g_dump_dir, g_replace_dir, g_packs_dir;
bool g_dumping = false, g_replacing = false, g_community = true;
std::string g_upscaler = kUpscalerXbr;

// --- background work: replacements are loaded before dumps are written --------------------------------

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<std::function<void()>> g_loads, g_writes;
bool g_stop = false;
std::thread g_worker;

void worker() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock, [] { return g_stop || !g_loads.empty() || !g_writes.empty(); });
            if (!g_loads.empty()) {
                job = std::move(g_loads.front());
                g_loads.pop_front();
            } else if (!g_writes.empty()) {
                job = std::move(g_writes.front());
                g_writes.pop_front();
            } else {
                return; // stopping and nothing left
            }
        }
        job();
    }
}

void post(std::deque<std::function<void()>>& queue, std::function<void()> job) {
    {
        std::lock_guard lock(g_mutex);
        if (!g_worker.joinable()) g_worker = std::thread(worker);
        queue.push_back(std::move(job));
    }
    g_wake.notify_one();
}

// --- dumping -----------------------------------------------------------------------------------------------

std::unordered_set<u64> g_dumped;   // on disk already or queued; touched only by the game's thread
u64 g_dumped_count = 0;
std::atomic<size_t> g_queued_writes{0};

std::string file_name(u64 id) { return format("%016llx.png", static_cast<unsigned long long>(id)); }

// Reads a texture ID from a file name: 16 hexadecimal digits and ".png".
bool parse_id(const std::filesystem::path& file, u64* id) {
    std::string ext = file.extension().string();
    for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    const std::string stem = file.stem().string();
    if (ext != ".png" || stem.size() != 16) return false;
    u64 value = 0;
    for (char ch : stem) {
        const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (digit < 0) return false;
        value = (value << 4) | u64(digit);
    }
    *id = value;
    return true;
}

// --- replacement -------------------------------------------------------------------------------------------

std::unordered_map<u64, std::filesystem::path> g_index; // touched only by the game's thread

// Community packs. Their files get IDs of their own (from the PPSSPP key), kept apart from g_index.
std::vector<ppsspp::Pack> g_packs;
std::unordered_map<u64, std::filesystem::path> g_community_files;
std::vector<std::unordered_set<u64>> g_community_used; // per pack: IDs used since start-up
std::unordered_map<std::string, std::pair<u32, u32>> g_png_sizes;
struct CommunityStats {
    u64 checked = 0, hits = 0;
} g_stats;

u64 community_file_id(const ppsspp::Key& key) {
    u64 h = key.cache_key * 0x9E3779B97F4A7C15ull ^ (u64(key.hash) << 1 | 1);
    h ^= h >> 29;
    return h ? h : 1;
}

std::pair<u32, u32> png_size_cached(const std::filesystem::path& file) {
    auto [it, added] = g_png_sizes.try_emplace(file.string());
    if (added) it->second = ppsspp::png_size(file);
    return it->second;
}

void load_community_packs() {
    g_packs.clear();
    g_community_files.clear();
    g_community_used.clear();
    std::error_code ec;
    if (!g_community || g_packs_dir.empty() || !std::filesystem::is_directory(g_packs_dir, ec)) return;
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(g_packs_dir, ec))
        if (entry.is_directory(ec)) folders.push_back(entry.path());
    std::sort(folders.begin(), folders.end());
    for (const std::filesystem::path& folder : folders) {
        ppsspp::Pack pack;
        std::string error;
        if (!ppsspp::load_pack(folder, &pack, &error)) {
            LOG_WARN("textures", "community pack %s not used: %s", path_utf8(folder).c_str(), error.c_str());
            continue;
        }
        LOG_INFO("textures", "community pack \"%s\": %zu texture(s)", pack.name.c_str(), pack.files.size());
        g_packs.push_back(std::move(pack));
    }
    g_community_used.resize(g_packs.size());
}

struct Loaded {
    std::shared_ptr<const Image> image; // null while loading, or after a failed load
    bool failed = false;
    std::list<u64>::iterator lru;
};
std::unordered_map<u64, Loaded> g_loaded; // shared with the worker: guarded by g_mutex
std::list<u64> g_lru;                      // most recently used first
size_t g_loaded_bytes = 0;

void load(u64 id, std::filesystem::path path) {
    auto image = std::make_shared<Image>();
    int w = 0, h = 0, channels = 0;
    std::vector<u8> bytes;
    stbi_uc* pixels = nullptr;
    if (read_file(path, bytes)) pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    const bool ok = pixels && w > 0 && h > 0 && u32(w) <= kMaxReplacementSide && u32(h) <= kMaxReplacementSide;
    if (ok) {
        image->width = u32(w);
        image->height = u32(h);
        image->rgba.resize(size_t(w) * size_t(h));
        std::memcpy(image->rgba.data(), pixels, image->rgba.size() * 4);
    }
    if (pixels) stbi_image_free(pixels);
    if (!ok) LOG_WARN("textures", "cannot use replacement %s", path.string().c_str());

    std::lock_guard lock(g_mutex);
    auto it = g_loaded.find(id);
    if (it == g_loaded.end()) return; // forgotten meanwhile (the folder was read again)
    if (!ok) {
        it->second.failed = true;
        return;
    }
    it->second.image = image;
    g_loaded_bytes += image->rgba.size() * 4;
    // Keep within the memory budget; the least recently used images go first and load again on demand.
    while (g_loaded_bytes > kLoadedBudget && g_lru.size() > 1) {
        const u64 victim = g_lru.back();
        auto v = g_loaded.find(victim);
        if (v != g_loaded.end() && v->second.image) g_loaded_bytes -= v->second.image->rgba.size() * 4;
        g_loaded.erase(victim);
        g_lru.pop_back();
    }
}

void forget_loaded() {
    std::lock_guard lock(g_mutex);
    g_loaded.clear();
    g_lru.clear();
    g_loaded_bytes = 0;
}

} // namespace

void init(const std::filesystem::path& user_dir, const std::filesystem::path& packs_dir) {
    g_dump_dir = user_dir / "textures" / "dump";
    g_replace_dir = user_dir / "textures" / "replace";
    g_packs_dir = packs_dir;
}

void shutdown() {
    {
        std::lock_guard lock(g_mutex);
        g_loads.clear(); // nobody waits for these any more; queued dumps are still written
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_worker.joinable()) g_worker.join();
    if (g_dumped_count) LOG_INFO("textures", "%llu texture(s) dumped to %s", static_cast<unsigned long long>(g_dumped_count),
                                 g_dump_dir.string().c_str());
    for (size_t i = 0; i < g_packs.size(); ++i)
        LOG_INFO("textures", "community pack \"%s\": %zu of its %zu texture(s) used", g_packs[i].name.c_str(),
                 g_community_used[i].size(), g_packs[i].files.size());
    if (g_stats.checked)
        LOG_INFO("textures", "community packs: %llu texture(s) checked, %llu matched", static_cast<unsigned long long>(g_stats.checked),
                 static_cast<unsigned long long>(g_stats.hits));
}

std::filesystem::path dump_folder() { return g_dump_dir; }
std::filesystem::path replace_folder() { return g_replace_dir; }

void set_dumping(bool on) {
    if (on == g_dumping) return;
    g_dumping = on;
    if (!on) return;
    // Textures dumped in earlier sessions are not written again.
    std::error_code ec;
    std::filesystem::create_directories(g_dump_dir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(g_dump_dir, ec)) {
        u64 id;
        if (entry.is_regular_file(ec) && parse_id(entry.path(), &id)) g_dumped.insert(id);
    }
    LOG_INFO("textures", "dumping textures to %s (%zu there already)", g_dump_dir.string().c_str(), g_dumped.size());
}

bool dumping() { return g_dumping; }

void dump(u64 id, const u32* rgba, u32 width, u32 height) {
    if (!g_dumping || g_dumped.count(id) || g_queued_writes.load() >= kMaxQueuedDumps) return;
    g_dumped.insert(id);
    ++g_dumped_count;
    ++g_queued_writes;
    std::vector<u32> pixels(rgba, rgba + size_t(width) * height);
    const std::filesystem::path path = g_dump_dir / file_name(id);
    post(g_writes, [pixels = std::move(pixels), width, height, path] {
        // Written under a temporary name first, so a texture pack never picks up half a file.
        std::filesystem::path partial = path;
        partial += ".partial";
        std::error_code ec;
        if (host::write_png(partial.string(), width, height, pixels.data())) std::filesystem::rename(partial, path, ec);
        else LOG_WARN("textures", "cannot write %s", path.string().c_str());
        --g_queued_writes;
    });
}

u64 dumped_count() { return g_dumped_count; }

// The few most recent movie picture buffers, as physical address ranges.
namespace {
struct Range {
    u32 begin = 0, end = 0;
};
Range g_excluded[8];
u32 g_next_excluded = 0;
constexpr u32 kPhysicalMask = 0x3FFFFFFF; // the same memory appears at several mirrors
} // namespace

void exclude_range(u32 guest_begin, u32 guest_end) {
    const u32 begin = guest_begin & kPhysicalMask, end = begin + (guest_end - guest_begin);
    for (const Range& r : g_excluded)
        if (r.begin == begin && r.end == end) return;
    g_excluded[g_next_excluded] = {begin, end};
    g_next_excluded = (g_next_excluded + 1) % 8;
}

bool excluded(u32 guest_addr) {
    const u32 a = guest_addr & kPhysicalMask;
    for (const Range& r : g_excluded)
        if (a >= r.begin && a < r.end) return true;
    return false;
}

void set_replacing(bool on) {
    if (on == g_replacing) return;
    g_replacing = on;
    if (on) rescan();
}

bool replacing() { return g_replacing && (!g_index.empty() || !g_packs.empty()); }

void set_upscaler(const std::string& method) {
    if (method == g_upscaler) return;
    g_upscaler = method;
    if (g_replacing) rescan();
}

void set_community(bool on) {
    if (on == g_community) return;
    g_community = on;
    if (g_replacing) rescan();
}

size_t rescan() {
    g_index.clear();
    forget_loaded();
    load_community_packs();
    std::error_code ec;
    std::filesystem::create_directories(g_replace_dir, ec);
    // Two files with the same name: one made by hand wins over the upscaler's (kUpscaledFolder), and
    // of the upscaler's sets only the chosen method's is used.
    const std::filesystem::path generated = g_replace_dir / kUpscaledFolder;
    auto made_by_upscaler = [&](const std::filesystem::path& file) {
        const std::filesystem::path relative = file.lexically_relative(generated);
        return !relative.empty() && *relative.begin() != "..";
    };
    // Linked folders (junctions, symbolic links) are followed: a pack can live elsewhere.
    constexpr auto kFollow = std::filesystem::directory_options::follow_directory_symlink |
                             std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(g_replace_dir, kFollow, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        u64 id;
        if (it.depth() > 8) it.disable_recursion_pending(); // a link back up would never end
        if (!it->is_regular_file(ec) || !parse_id(it->path(), &id)) continue;
        const bool generated_file = made_by_upscaler(it->path());
        if (generated_file && upscaler_of(it->path().lexically_relative(generated)) != g_upscaler) continue;
        auto [entry, added] = g_index.try_emplace(id, it->path());
        if (!added && made_by_upscaler(entry->second) && !generated_file) entry->second = it->path();
    }
    if (!g_index.empty()) LOG_INFO("textures", "%zu replacement texture(s) in %s", g_index.size(), g_replace_dir.string().c_str());
    size_t community = 0;
    for (const ppsspp::Pack& pack : g_packs) community += pack.files.size();
    return g_index.size() + community;
}

size_t replacement_count() {
    size_t count = g_index.size();
    for (const ppsspp::Pack& pack : g_packs) count += pack.files.size();
    return count;
}

bool has_replacement(u64 id) { return g_replacing && (g_index.count(id) != 0 || g_community_files.count(id) != 0); }

std::vector<CommunityPack> community_packs() {
    std::vector<CommunityPack> out;
    for (size_t i = 0; i < g_packs.size(); ++i)
        out.push_back({g_packs[i].name, g_packs[i].author, g_packs[i].licence, g_packs[i].files.size(),
                       g_community_used[i].size()});
    return out;
}

u64 community_id(const ppsspp::TextureInfo& tex) {
    if (!g_replacing || g_packs.empty()) return 0;
    ++g_stats.checked;
    const auto match = ppsspp::find(g_packs, tex, png_size_cached);
    if (!match) return 0;
    ++g_stats.hits;
    const u64 id = community_file_id(match->key);
    g_community_files.try_emplace(id, *match->file);
    const size_t pack = size_t(match->pack - g_packs.data());
    if (g_community_used[pack].insert(id).second)
        LOG_CH("texpack", "hit %s: %s (%s)", ppsspp::key_name(match->key).c_str(), path_utf8(*match->file).c_str(),
               match->pack->name.c_str());
    return id;
}

std::shared_ptr<const Image> replacement(u64 id, bool* pending) {
    *pending = false;
    auto file = g_index.find(id);
    if (file == g_index.end()) {
        file = g_community_files.find(id);
        if (file == g_community_files.end()) return nullptr;
    }
    if (!g_replacing) return nullptr;
    std::lock_guard lock(g_mutex);
    auto it = g_loaded.find(id);
    if (it != g_loaded.end()) {
        g_lru.splice(g_lru.begin(), g_lru, it->second.lru);
        *pending = !it->second.image && !it->second.failed;
        return it->second.image;
    }
    *pending = true;
    g_lru.push_front(id);
    g_loaded[id] = Loaded{nullptr, false, g_lru.begin()};
    if (!g_worker.joinable()) g_worker = std::thread(worker);
    g_loads.push_back([id, path = file->second] { load(id, path); });
    g_wake.notify_one();
    return nullptr;
}

} // namespace p2::gfx::textures
