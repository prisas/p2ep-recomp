// p2texup: makes a texture pack from the textures the game dumped.
//
// Every <user dir>/textures/dump/<id>.png is upscaled to <user dir>/textures/replace/upscaled/<method>/
// <id>.png, where the game finds it: with FFmpeg's xBR (see upscaler.h) or with Real-ESRGAN (see
// realesrgan.h). Textures already done (the result is newer than the dump, and the folder's
// upscaler.ini shows the same settings) are skipped, and so are textures that have a replacement of
// their own elsewhere in textures/replace: hand-made work is never overridden.
//
// With --progress the output has lines for the game's settings button: "progress <done> <total>",
// "error <message>" when nothing can be done, and "done <upscaled> <up to date> <kept> <failed>".

#include "lib/common/file.h"
#include "runtime/gfx/texture_pack.h"
#include "tools/texup/realesrgan.h"
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
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using namespace p2;
namespace fs = std::filesystem;

namespace {

constexpr u32 kBatch = 128;     // textures per Real-ESRGAN run: progress shows, an interruption loses little
constexpr u32 kSmallestSide = 8; // smaller textures go through xBR: the network adds nothing there

struct Options {
    fs::path user_dir;
    fs::path realesrgan;   // the program; default realesrgan\realesrgan-ncnn-vulkan.exe next to this one
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
               "pack (textures\\replace\\upscaled\\<method>).\n"
               "\n"
               "  --user-dir <dir>    the game's user folder (default: userdata next to this program)\n"
               "  --method <m>        xbr (FFmpeg's xBR scaler, default) or realesrgan (Real-ESRGAN, on the GPU)\n"
               "  --scale <n>         2, 3 or 4 times the original size (default 4)\n"
               "  --sharpen <s>       xbr: sharpening from 0 (off) to 1 (default 0.25)\n"
               "  --model <name>      realesrgan: the model (default realesrgan-x4plus-anime)\n"
               "  --realesrgan <exe>  realesrgan: the program (default realesrgan\\realesrgan-ncnn-vulkan.exe\n"
               "                      next to this one)\n"
               "  --force             redo textures that are already done\n"
               "  --jobs <n>          textures worked on at once (default: processor threads - 1)\n"
               "  --progress          also print 'progress <done> <total>' lines\n",
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

// An opaque picture as an RGB PNG (Real-ESRGAN's input: an alpha channel would only cost it time).
bool write_png_rgb(const fs::path& path, const texup::Rgba& image) {
    std::vector<u8> rgb(size_t(image.width) * image.height * 3), bytes;
    for (size_t i = 0, o = 0; o < rgb.size(); i += 4, o += 3) std::copy_n(&image.pixels[i], 3, &rgb[o]);
    auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(context);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    return stbi_write_png_to_func(append, &bytes, int(image.width), int(image.height), 3, rgb.data(), int(image.width) * 3) &&
           write_file(path, bytes.data(), bytes.size());
}

// What made a set, as written to its upscaler.ini: a change means every texture is done again.
std::string settings_record(const texup::Settings& s) {
    std::string text = format("method = %s\nscale = %u\n", texup::method_name(s.method), s.scale);
    if (s.method == texup::Method::Xbr) append_format(text, "sharpen = %.3f\n", double(s.sharpen));
    else text += "model = " + s.model + "\n";
    return text;
}

// Textures directly in the upscaled folder date from before there were several methods: they were
// made with xBR, so they move to its folder.
void move_legacy(const fs::path& upscaled) {
    std::error_code ec;
    const fs::path xbr = upscaled / gfx::textures::kUpscalerXbr;
    u32 moved = 0;
    for (const auto& entry : fs::directory_iterator(upscaled, ec)) {
        if (!entry.is_regular_file(ec) || !texture_name(entry.path())) continue;
        fs::create_directories(xbr, ec);
        const fs::path to = xbr / entry.path().filename();
        if (fs::exists(to, ec) && fs::last_write_time(to, ec) >= fs::last_write_time(entry.path(), ec)) fs::remove(entry.path(), ec);
        else fs::rename(entry.path(), to, ec);
        if (!ec) ++moved;
    }
    if (moved) std::printf("Moved %u earlier xBR texture(s) into %s.\n", moved, path_utf8(xbr).c_str());
}

struct Job {
    fs::path from, to;
};

// Runs `work(i)` for i in [0, count) on `threads` threads.
void parallel(u32 count, u32 threads, const std::function<void(u32)>& work) {
    std::atomic<u32> next{0};
    auto worker = [&] {
        for (u32 i; (i = next.fetch_add(1)) < count;) work(i);
    };
    threads = std::min(threads, std::max(count, 1u));
    std::vector<std::thread> pool;
    for (u32 t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
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
        else if (a == L"--method") {
            const std::string m = to_utf8(value().c_str());
            if (!texup::parse_method(m, &opt.settings.method)) return usage("--method must be xbr or realesrgan");
        } else if (a == L"--scale") opt.settings.scale = u32(std::wcstoul(value().c_str(), nullptr, 10));
        else if (a == L"--sharpen") opt.settings.sharpen = std::wcstof(value().c_str(), nullptr);
        else if (a == L"--model") opt.settings.model = to_utf8(value().c_str());
        else if (a == L"--realesrgan") opt.realesrgan = value();
        else if (a == L"--force") opt.force = true;
        else if (a == L"--jobs") opt.jobs = u32(std::wcstoul(value().c_str(), nullptr, 10));
        else if (a == L"--progress") opt.progress = true;
        else if (a == L"--help" || a == L"-h") return usage();
        else return usage("unknown option '" + to_utf8(a.c_str()) + "'");
    }
    if (opt.settings.scale < 2 || opt.settings.scale > 4) return usage("--scale must be 2, 3 or 4");
    if (!(opt.settings.sharpen >= 0.0f && opt.settings.sharpen <= 1.0f)) return usage("--sharpen must be from 0 to 1");
    av_log_set_level(AV_LOG_ERROR);
    const bool use_realesrgan = opt.settings.method == texup::Method::RealEsrgan;

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
    auto give_up = [&](const std::string& message) {
        std::printf("%s\n", message.c_str());
        if (opt.progress) std::printf("error %s\ndone 0 0 0 0\n", message.c_str());
        std::fflush(stdout);
        return finish(1);
    };

    const fs::path dump = opt.user_dir / "textures" / "dump";
    const fs::path replace = opt.user_dir / "textures" / "replace";
    const fs::path upscaled = replace / gfx::textures::kUpscaledFolder;
    const fs::path out_dir = upscaled / texup::method_name(opt.settings.method);
    std::error_code ec;
    if (!fs::is_directory(dump, ec)) {
        std::printf("No dumped textures in %s.\nTurn on Dump textures in the game's settings (Video tab) and play for a while first.\n",
                    path_utf8(dump).c_str());
        if (opt.progress) std::printf("done 0 0 0 0\n");
        return finish(0);
    }

    // Real-ESRGAN: the program and the model must be there.
    const fs::path program = opt.realesrgan.empty() ? texup::realesrgan::default_program() : opt.realesrgan;
    if (use_realesrgan) {
        if (!fs::is_regular_file(program, ec))
            return give_up("Real-ESRGAN is not installed: " + path_utf8(program) + " is missing.");
        const auto models = texup::realesrgan::list_models(program.parent_path() / "models");
        if (std::find(models.begin(), models.end(), opt.settings.model) == models.end())
            return give_up("Real-ESRGAN has no model named " + opt.settings.model + ".");
    }

    fs::create_directories(upscaled, ec);
    move_legacy(upscaled);
    fs::create_directories(out_dir, ec);

    // A set made with other settings is done again.
    const std::string record = settings_record(opt.settings);
    const fs::path record_file = out_dir / "upscaler.ini";
    std::vector<u8> old_record;
    if (!opt.force && read_file(record_file, old_record) && std::string(old_record.begin(), old_record.end()) != record) {
        std::printf("The settings changed since %s was made: every texture is done again.\n", path_utf8(out_dir).c_str());
        opt.force = true;
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

    const u32 total = u32(jobs.size());
    std::printf("%u texture(s) to upscale %ux with %s into %s (%u up to date, %u with a replacement of your own).\n", total,
                opt.settings.scale, use_realesrgan ? ("Real-ESRGAN (" + opt.settings.model + ")").c_str() : "xBR",
                path_utf8(out_dir).c_str(), up_to_date, kept);
    if (opt.progress) std::printf("progress 0 %u\n", total);
    std::fflush(stdout);

    const auto started = std::chrono::steady_clock::now();
    std::atomic<u32> done{0}, failed{0};
    std::mutex print;
    // Records one finished texture (ok or not) and reports progress.
    auto finished = [&](const Job& job, bool ok, const std::string& error) {
        const u32 count = done.fetch_add(1) + 1;
        std::lock_guard lock(print);
        if (!ok) {
            ++failed;
            std::fprintf(stderr, "%s: %s\n", path_utf8(job.from.filename()).c_str(), error.c_str());
        }
        if (opt.progress) std::printf("progress %u %u\n", count, total);
        else if (count == total || count % 25 == 0) std::printf("  %u of %u\n", count, total);
        std::fflush(stdout);
    };
    auto upscale_xbr = [&](const Job& job, const texup::Rgba* in_memory) {
        std::string error;
        texup::Rgba in, out;
        texup::Settings xbr = opt.settings;
        xbr.method = texup::Method::Xbr;
        bool ok = (in_memory ? (in = *in_memory, true) : read_png(job.from, in, &error)) && texup::upscale(in, xbr, out, &error);
        if (ok && !write_png(job.to, out)) {
            error = "cannot write " + path_utf8(job.to);
            ok = false;
        }
        finished(job, ok, error);
    };
    const u32 threads = opt.jobs ? opt.jobs : std::max(1u, std::thread::hardware_concurrency() - 1);

    if (!use_realesrgan) {
        parallel(total, threads, [&](u32 i) { upscale_xbr(jobs[i], nullptr); });
    } else {
        // Real-ESRGAN works on folders: each batch is prepared as opaque pictures (transparent pixels
        // coloured from their surroundings), upscaled in one run, then finished here: resized when the
        // model's scale is larger than asked, and given back its alpha, upscaled with xBR.
        const fs::path work = fs::temp_directory_path() / format("p2texup-%lu", GetCurrentProcessId());
        const fs::path in_dir = work / "in";
        const u32 native = texup::realesrgan::native_scale(opt.settings.model, opt.settings.scale);
        u32 xbr_instead = 0;
        for (u32 first = 0; first < total; first += kBatch) {
            const u32 count = std::min(kBatch, total - first);
            fs::remove_all(work, ec);
            fs::create_directories(in_dir, ec);
            std::vector<texup::Rgba> sources(count);
            std::vector<char> queued(count, 0);
            parallel(count, threads, [&](u32 i) {
                const Job& job = jobs[first + i];
                std::string error;
                texup::Rgba& src = sources[i];
                if (!read_png(job.from, src, &error)) return finished(job, false, error);
                if (src.width < kSmallestSide || src.height < kSmallestSide) return upscale_xbr(job, &src);
                texup::Rgba prepared = src;
                if (texup::has_transparency(prepared)) texup::fill_transparent(prepared);
                if (!write_png_rgb(in_dir / job.from.filename(), prepared)) return finished(job, false, "cannot write a temporary file");
                queued[i] = 1;
            });
            if (std::none_of(queued.begin(), queued.end(), [](char q) { return q != 0; })) continue;

            // Some GPUs and drivers give an all-black picture for a few inputs (this build of the
            // program is known for it): those are tried again on the computer's other GPUs, and any
            // still black are done with xBR, so the set has every texture.
            std::vector<u32> pending;
            for (u32 i = 0; i < count; ++i)
                if (queued[i]) pending.push_back(i);
            for (int gpu = -1; !pending.empty(); ++gpu) {
                const fs::path round_in = gpu < 0 ? in_dir : work / format("in%d", gpu);
                const fs::path round_out = work / format("out%d", gpu + 1);
                fs::create_directories(round_out, ec);
                if (gpu >= 0) { // only the pictures still to do
                    fs::create_directories(round_in, ec);
                    for (u32 i : pending) fs::copy_file(in_dir / jobs[first + i].from.filename(), round_in / jobs[first + i].from.filename(), ec);
                }
                std::string output;
                const int code = texup::realesrgan::run(
                    texup::realesrgan::command_line(program, round_in, round_out, opt.settings.model, opt.settings.scale, gpu), &output);
                if (code != 0 && gpu >= 0) break; // no such GPU: the rest go to xBR
                std::string run_error;
                if (code != 0) {
                    run_error = format("Real-ESRGAN failed (exit code %d)", code);
                    const size_t eol = output.find_first_of("\r\n");
                    if (!output.empty()) run_error += ": " + output.substr(0, eol);
                }
                std::vector<char> black(pending.size(), 0);
                parallel(u32(pending.size()), threads, [&](u32 k) {
                    const u32 i = pending[k];
                    const Job& job = jobs[first + i];
                    if (!run_error.empty()) return finished(job, false, run_error);
                    const texup::Rgba& src = sources[i];
                    std::string error;
                    texup::Rgba result;
                    if (!read_png(round_out / job.from.filename(), result, &error))
                        return finished(job, false, "Real-ESRGAN gave no picture (" + error + ")");
                    if (result.width != src.width * native || result.height != src.height * native)
                        return finished(job, false, "Real-ESRGAN gave a picture of the wrong size");
                    if (texup::realesrgan::black_result(src, result)) {
                        black[k] = 1;
                        return;
                    }
                    const u32 w = src.width * opt.settings.scale, h = src.height * opt.settings.scale;
                    if (native != opt.settings.scale && !texup::resize(result, w, h, result, &error)) return finished(job, false, error);
                    if (texup::has_transparency(src)) {
                        std::vector<u8> alpha;
                        u32 aw = 0, ah = 0;
                        if (!texup::upscale_alpha(src, opt.settings.scale, alpha, aw, ah, &error)) return finished(job, false, error);
                        if (aw != w || ah != h) return finished(job, false, "colour and alpha came out at different sizes");
                        result = texup::merge_bgr0(w, h, texup::color_bgr0(result), &alpha);
                    }
                    if (!write_png(job.to, result)) return finished(job, false, "cannot write " + path_utf8(job.to));
                    finished(job, true, {});
                });
                std::vector<u32> still;
                for (size_t k = 0; k < pending.size(); ++k)
                    if (black[k]) still.push_back(pending[k]);
                pending.swap(still);
            }
            if (!pending.empty()) {
                xbr_instead += u32(pending.size());
                parallel(u32(pending.size()), threads, [&](u32 k) { upscale_xbr(jobs[first + pending[k]], &sources[pending[k]]); });
            }
        }
        fs::remove_all(work, ec);
        if (xbr_instead)
            std::printf("%u texture(s) came out black from Real-ESRGAN on every GPU (a graphics driver problem) and were done "
                        "with xBR instead.\n", xbr_instead);
    }

    write_file(record_file, record.data(), record.size());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const u32 bad = failed.load();
    std::printf("Upscaled %u texture(s) in %.1f s%s.\n", total - bad, seconds, bad ? format(", %u failed", bad).c_str() : "");
    if (total > bad) std::printf("The game uses them once the textures are reloaded (Reload in the Video tab) or at its next start.\n");
    if (opt.progress) std::printf("done %u %u %u %u\n", total - bad, up_to_date, kept, bad);
    std::fflush(stdout);
    return finish(bad ? 1 : 0);
}
