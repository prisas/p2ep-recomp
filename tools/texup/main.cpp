// p2texup: makes a texture pack from the textures the game dumped.
//
// Every <user dir>/textures/dump/<id>.png is upscaled (see upscaler.h) to
// <user dir>/textures/replace/upscaled/<id>.png, where the game finds it. Textures already done (the
// result is newer than the dump) are skipped, and so are textures that have a replacement of their
// own elsewhere in textures/replace: hand-made work is never overridden.

#include "lib/common/file.h"
#include "runtime/gfx/texture_pack.h"
#include "tools/texup/upscaler.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <stb_image.h>
#include <stb_image_write.h>
#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
extern "C" {
#include <libavutil/log.h>
}
#pragma clang diagnostic pop

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using namespace p2;
namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path user_dir;
    texup::Settings settings;
    bool force = false;
    bool progress = false; // machine-readable progress lines, for the game's settings button
    u32 jobs = 0;          // 0: one fewer than the processor's threads
};

int usage(const std::string& problem = {}) {
    if (!problem.empty()) std::fprintf(stderr, "%s\n\n", problem.c_str());
    std::fputs("usage: p2texup [options]\n"
               "\n"
               "Upscales the textures the game dumped (textures\\dump in the user folder) into a texture\n"
               "pack (textures\\replace\\upscaled), with FFmpeg's xBR scaler and light sharpening.\n"
               "\n"
               "  --user-dir <dir>   the game's user folder (default: userdata next to this program)\n"
               "  --scale <n>        2, 3 or 4 times the original size (default 4)\n"
               "  --sharpen <s>      sharpening from 0 (off) to 1 (default 0.25)\n"
               "  --force            redo textures that are already done\n"
               "  --jobs <n>         textures worked on at once (default: processor threads - 1)\n"
               "  --progress         also print 'progress <done> <total>' lines\n",
               problem.empty() ? stdout : stderr);
    return 2;
}

// A dumped texture's file name: 16 hexadecimal digits and ".png".
bool texture_name(const fs::path& file) {
    std::wstring ext = file.extension().wstring();
    for (wchar_t& ch : ext) ch = wchar_t(std::towlower(ch));
    const std::wstring stem = file.stem().wstring();
    return ext == L".png" && stem.size() == 16 && std::all_of(stem.begin(), stem.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; });
}

std::wstring lower(std::wstring text) {
    for (wchar_t& ch : text) ch = wchar_t(std::towlower(ch));
    return text;
}

bool read_png(const fs::path& path, texup::Rgba& image, std::string* error) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) {
        *error = "cannot read the file";
        return false;
    }
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        *error = std::string("not a readable PNG (") + stbi_failure_reason() + ")";
        return false;
    }
    image.width = u32(w);
    image.height = u32(h);
    image.pixels.assign(pixels, pixels + size_t(w) * size_t(h) * 4);
    stbi_image_free(pixels);
    return true;
}

// Written to a temporary name first: an interrupted run never leaves half a file.
bool write_png(const fs::path& path, const texup::Rgba& image) {
    std::vector<u8> bytes;
    auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(context);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    if (!stbi_write_png_to_func(append, &bytes, int(image.width), int(image.height), 4, image.pixels.data(), int(image.width) * 4))
        return false;
    fs::path partial = path;
    partial += L".partial";
    if (!write_file(partial, bytes.data(), bytes.size())) return false;
    std::error_code ec;
    fs::rename(partial, path, ec);
    if (ec) fs::remove(partial, ec);
    return !ec;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options opt;
    opt.user_dir = program_folder() / "userdata";
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : std::wstring(); };
        if (a == L"--user-dir") opt.user_dir = value();
        else if (a == L"--scale") opt.settings.scale = u32(std::wcstoul(value().c_str(), nullptr, 10));
        else if (a == L"--sharpen") opt.settings.sharpen = std::wcstof(value().c_str(), nullptr);
        else if (a == L"--force") opt.force = true;
        else if (a == L"--jobs") opt.jobs = u32(std::wcstoul(value().c_str(), nullptr, 10));
        else if (a == L"--progress") opt.progress = true;
        else if (a == L"--help" || a == L"-h") return usage();
        else return usage("unknown option '" + to_utf8(a.c_str()) + "'");
    }
    if (opt.settings.scale < 2 || opt.settings.scale > 4) return usage("--scale must be 2, 3 or 4");
    if (!(opt.settings.sharpen >= 0.0f && opt.settings.sharpen <= 1.0f)) return usage("--sharpen must be from 0 to 1");
    av_log_set_level(AV_LOG_ERROR);

    // Started by double-click, the console window closes with the program: keep the summary on
    // screen. Only then: run by the game (output into a pipe, hidden console) nobody could answer.
    DWORD console_users[2];
    const bool own_console = GetConsoleProcessList(console_users, 2) == 1 &&
                             GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR;
    auto finish = [&](int code) {
        if (own_console) {
            std::printf("\nPress Enter to close.");
            std::fflush(stdout);
            (void)std::getchar();
        }
        return code;
    };

    const fs::path dump = opt.user_dir / "textures" / "dump";
    const fs::path replace = opt.user_dir / "textures" / "replace";
    const fs::path out_dir = replace / gfx::textures::kUpscaledFolder;
    std::error_code ec;
    if (!fs::is_directory(dump, ec)) {
        std::printf("No dumped textures in %s.\nTurn on Dump textures in the game's settings (Video tab) and play for a while first.\n",
                    path_utf8(dump).c_str());
        if (opt.progress) std::printf("done 0 0 0 0\n");
        return finish(0);
    }

    // Replacements of the player's own, anywhere in textures/replace but the upscaler's folder.
    std::set<std::wstring> own;
    for (auto it = fs::recursive_directory_iterator(replace, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && lower(it->path().filename().wstring()) == lower(fs::path(gfx::textures::kUpscaledFolder).wstring()) &&
            it.depth() == 0) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && texture_name(it->path())) own.insert(lower(it->path().filename().wstring()));
    }

    struct Job {
        fs::path from, to;
    };
    std::vector<Job> jobs;
    u32 up_to_date = 0, kept = 0;
    for (const auto& entry : fs::directory_iterator(dump, ec)) {
        if (!entry.is_regular_file(ec) || !texture_name(entry.path())) continue;
        const fs::path to = out_dir / entry.path().filename();
        if (own.count(lower(entry.path().filename().wstring()))) {
            ++kept;
            continue;
        }
        if (!opt.force && fs::exists(to, ec) && fs::last_write_time(to, ec) >= fs::last_write_time(entry.path(), ec)) {
            ++up_to_date;
            continue;
        }
        jobs.push_back({entry.path(), to});
    }
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.from < b.from; });
    fs::create_directories(out_dir, ec);

    const u32 total = u32(jobs.size());
    std::printf("%u texture(s) to upscale %ux into %s (%u up to date, %u with a replacement of your own).\n", total,
                opt.settings.scale, path_utf8(out_dir).c_str(), up_to_date, kept);
    if (opt.progress) std::printf("progress 0 %u\n", total);
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    std::atomic<u32> next{0}, done{0}, failed{0};
    std::mutex print;
    auto worker = [&] {
        for (;;) {
            const u32 index = next.fetch_add(1);
            if (index >= total) return;
            const Job& job = jobs[index];
            std::string error;
            texup::Rgba in, out;
            bool ok = read_png(job.from, in, &error) && texup::upscale(in, opt.settings, out, &error);
            if (ok && !write_png(job.to, out)) {
                error = "cannot write " + path_utf8(job.to);
                ok = false;
            }
            const u32 count = done.fetch_add(1) + 1;
            std::lock_guard lock(print);
            if (!ok) {
                ++failed;
                std::fprintf(stderr, "%s: %s\n", path_utf8(job.from.filename()).c_str(), error.c_str());
            }
            if (opt.progress) std::printf("progress %u %u\n", count, total);
            else if (count == total || count % 25 == 0) std::printf("  %u of %u\n", count, total);
            std::fflush(stdout);
        }
    };
    u32 threads = opt.jobs ? opt.jobs : std::max(1u, std::thread::hardware_concurrency() - 1);
    threads = std::min(threads, std::max(total, 1u));
    std::vector<std::thread> pool;
    for (u32 t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const u32 bad = failed.load();
    std::printf("Upscaled %u texture(s) in %.1f s%s.\n", total - bad, seconds, bad ? format(", %u failed", bad).c_str() : "");
    if (total > bad) std::printf("The game uses them once the textures are reloaded (Reload in the Video tab) or at its next start.\n");
    if (opt.progress) std::printf("done %u %u %u %u\n", total - bad, up_to_date, kept, bad);
    std::fflush(stdout);
    return finish(bad ? 1 : 0);
}
