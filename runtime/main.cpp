#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "lib/common/log.h"
#include "lib/psp/prx.h"
#include "runtime/audio/audio_out.h"
#include "runtime/cpu/recomp.h"
#include "runtime/cpu/rt.h"
#include "runtime/ge/ge.h"
#include "runtime/gfx/vk/vk_renderer.h"
#include "runtime/hle/hle_display.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"
#include "runtime/platform/config.h"
#include "runtime/platform/frame.h"
#include "runtime/platform/host.h"
#include "runtime/platform/window.h"
#include "runtime/ui/overlay.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

using namespace p2;

namespace {

constexpr const char* kExecutablePath = "PSP_GAME/SYSDIR/EBOOT.BIN";
constexpr u32 kMinRootStack = 0x40000;

std::unique_ptr<gfx::Renderer> g_renderer;

int usage() {
    std::puts("usage: p2ep <disc-image> [options]\n"
              "\n"
              "  --user-dir <dir>      where saves and settings are kept (default: ./userdata)\n"
              "  --scale <n>           rendering resolution multiplier, 1-10 (default: match the window)\n"
              "  --fullscreen          start in full screen\n"
              "  --skip-intro          start at the main menu this time, skipping notices, logos and movie\n"
              "  --headless            run without a window or audio device\n"
              "  --overlay             with --headless: draw the overlay and let scripted input answer dialogs\n"
              "  --unthrottled         do not pace the game to real time\n"
              "  --frames <n>          quit after n displayed frames\n"
              "  --dump-frames <dir>   write displayed frames as PNG files\n"
              "  --dump-every <n>      ... every nth frame (default 60)\n"
              "  --dump-textures <dir> write every decoded texture as a PNG file\n"
              "  --dump-audio <file>   record the game's audio output to a WAV file\n"
              "  --input <script>      scripted pad input: frame:hexbuttons,frame:hexbuttons,... (or @file)\n"
              "  --no-gpu              do not create a renderer (system-call testing only)\n"
              "  --log <channels>      comma-separated trace channels: hle,io,thread,mem,display,ge,audio,all\n"
              "  --log-file <path>     also write the log to a file");
    return 2;
}

// Reports a host fault in terms the guest can be debugged with.
LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && mem::watch_rearm()) return EXCEPTION_CONTINUE_EXECUTION;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const void* address = reinterpret_cast<const void*>(rec->ExceptionInformation[1]);
    if (rec->ExceptionInformation[0] == 1) { // a write: possibly to memory under a debugging watch
        bool hit = false;
        u32 where = 0;
        if (mem::watch_fault(address, &hit, &where)) {
            static u32 reported = 0;
            if (hit && reported < 200) {
                ++reported;
                LOG_INFO("watch", "write to %08x by %s", where, kernel::describe_current().c_str());
            }
            info->ContextRecord->EFlags |= 0x100; // single-step the store, then protect the page again
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    const char* kind = rec->ExceptionInformation[0] == 1 ? "write to" : "read from";
    u32 guest = 0;
    if (mem::host_to_guest(address, &guest))
        P2_FATAL("the game tried to %s unmapped guest address %08x", kind, guest);
    P2_FATAL("host access violation: %s %p (code at %p)", kind, address, rec->ExceptionAddress);
}

bool boot(std::string* error) {
    if (!host::open_image(host::options().image, error)) return false;

    std::vector<u8> executable;
    if (!host::iso().read_all(kExecutablePath, executable)) {
        *error = std::string("the image does not contain ") + kExecutablePath;
        return false;
    }
    const std::string digest = to_hex(sha256(executable.data(), executable.size()));
    if (digest != g_gen_module.sha256) {
        *error = "this build was generated from a different game executable than the one in the image.\n"
                 "  image:  " + digest + "\n  build:  " + g_gen_module.sha256 +
                 "\nRebuild with this image, or use the image the build was made from.";
        return false;
    }

    psp::Module module;
    if (!psp::load_prx(executable, g_gen_module.base, module, error)) return false;
    if (module.base != g_gen_module.base || module.end != g_gen_module.end) {
        *error = "the executable does not load where the generated code expects it";
        return false;
    }
    if (!mem::valid(module.base, u32(module.image.size()))) {
        *error = "the executable does not fit in guest memory";
        return false;
    }
    std::memcpy(mem::ptr(module.base), module.image.data(), module.image.size());

    if (!kernel::init(error)) return false;
    init_dispatch();

    // Everything from the start of user memory to the end of the module belongs to the module.
    kernel::reserve_user(mem::kUserBase, module.end, "module");
    kernel::ModuleInfo& info = kernel::module();
    auto object = std::make_unique<kernel::Object>();
    object->type = kernel::ObjType::Module;
    object->name = module.name;
    info.uid = kernel::add_object(std::move(object));
    info.name = module.name;
    info.base = module.base;
    info.end = module.end;
    info.gp = module.gp;
    info.sdk_version = module.sdk_version;

    // The root thread receives the executable's path as its argument, like on the console.
    const std::string path = std::string("disc0:/") + kExecutablePath;
    const u32 arg = kernel::alloc_kernel("boot path", 256);
    mem::write_string(arg, path, 256);

    const s32 priority = module.start_thread_priority ? s32(module.start_thread_priority) : 32;
    const u32 stack = module.start_thread_stack > kMinRootStack ? module.start_thread_stack : kMinRootStack;
    const u32 attr = module.start_thread_attr | kernel::kThreadAttrUser;
    u32 create_error = 0;
    kernel::Uid root = kernel::create_thread("root", g_gen_module.entry, priority, stack, attr, &create_error);
    if (!root) {
        *error = "cannot create the game's main thread";
        return false;
    }
    LOG_INFO("boot", "module '%s' at %08x-%08x, entry %08x, gp %08x", module.name.c_str(), module.base, module.end,
             g_gen_module.entry, module.gp);
    hle::display_init(host::on_frame);
    kernel::start_thread(root, u32(path.size()) + 1, arg);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    host::Options& opt = host::options();
    opt.user_dir = "userdata";
    opt.dump_every = 60;
    std::string channels, log_file, audio_dump;
    bool no_gpu = false, fullscreen = false, skip_intro = false;
    u32 scale = 0; // 0: use the saved setting

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--user-dir") opt.user_dir = value();
        else if (a == "--scale") scale = u32(std::strtoul(value().c_str(), nullptr, 10));
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--skip-intro") skip_intro = true;
        else if (a == "--headless") opt.headless = true;
        else if (a == "--overlay") opt.overlay = true;
        else if (a == "--show-menu") { // <frame>[:<tab>]
            const std::string spec = value();
            char* end = nullptr;
            opt.menu_frame = std::strtoull(spec.c_str(), &end, 10);
            opt.menu_tab = *end == ':' ? u32(std::strtoul(end + 1, nullptr, 10)) : 0;
        }
        else if (a == "--unthrottled") opt.unthrottled = true;
        else if (a == "--frames") opt.frame_limit = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-frames") opt.dump_dir = value();
        else if (a == "--dump-every") opt.dump_every = u32(std::strtoul(value().c_str(), nullptr, 10));
        else if (a == "--dump-from") opt.dump_from = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-textures") opt.texture_dump_dir = value();
        else if (a == "--dump-audio") audio_dump = value();
        else if (a == "--trace-frame") opt.trace_frame = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--log-from") opt.late_from = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-ram") { // <file>@<frame>
            const std::string spec = value();
            const size_t at = spec.rfind('@');
            opt.ram_dump = spec.substr(0, at);
            opt.ram_dump_frame = at == std::string::npos ? 0 : std::strtoull(spec.c_str() + at + 1, nullptr, 10);
        } else if (a == "--watch") { // <hex address>:<bytes>@<frame>
            const std::string spec = value();
            char* end = nullptr;
            opt.watch_addr = u32(std::strtoul(spec.c_str(), &end, 16));
            opt.watch_size = *end == ':' ? u32(std::strtoul(end + 1, &end, 10)) : 4;
            opt.watch_from = *end == '@' ? std::strtoull(end + 1, nullptr, 10) : 0;
        }
        else if (a == "--input") { // the script itself, or @<file> holding it
            std::string script = value();
            if (!script.empty() && script[0] == '@') {
                std::vector<u8> bytes;
                if (!read_file(script.substr(1), bytes)) {
                    std::fprintf(stderr, "cannot read the input script %s\n", script.c_str() + 1);
                    return 2;
                }
                script.assign(bytes.begin(), bytes.end());
            }
            host::set_input_script(script);
        }
        else if (a == "--no-gpu") no_gpu = true;
        else if (a == "--log") channels = value();
        else if (a == "--log-file") log_file = value();
        else if (a == "--help" || a == "-h") return usage();
        else if (!a.empty() && a[0] != '-' && opt.image.empty()) opt.image = a;
        else {
            std::fprintf(stderr, "unknown option '%s'\n\n", a.c_str());
            return usage();
        }
    }
    if (opt.image.empty()) return usage();

    if (opt.late_from) opt.late_channels = channels;
    else if (!channels.empty()) log::enable_channels(channels);
    if (!log_file.empty() && !log::open_file(log_file)) std::fprintf(stderr, "cannot open log file %s\n", log_file.c_str());
    log::set_fatal_hook(kernel::dump_state);
    AddVectoredExceptionHandler(1, fault_handler);

    // Settings come from the user folder; options given on the command line win for this run.
    host::load_settings(opt.user_dir / "settings.ini");
    host::Settings& settings = host::settings();
    settings.run_scale = std::min<u32>(scale, 10);
    settings.run_skip_intro = skip_intro;
    settings.run_unthrottled = opt.unthrottled;
    if (fullscreen) settings.fullscreen = true;

    std::string error;
    if (!mem::init(&error) || !host::window_init(&error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    audio::output_init();
    if (!no_gpu) {
        // The overlay needs somewhere to draw: the window, or an off-screen picture when asked for.
        constexpr u32 kOffscreenWidth = 960, kOffscreenHeight = 544;
        const bool overlay = (host::window_handle() || opt.overlay) && ui::init(host::window_handle(), kOffscreenWidth, kOffscreenHeight);
        gfx::RendererOptions renderer_options;
        renderer_options.sdl_window = host::window_handle();
        renderer_options.scale = std::max<u32>(settings.run_scale ? settings.run_scale : settings.scale, 1);
        renderer_options.vsync = settings.vsync && !settings.run_unthrottled;
        renderer_options.overlay = overlay;
        if (overlay && !host::window_handle()) {
            renderer_options.offscreen_width = kOffscreenWidth;
            renderer_options.offscreen_height = kOffscreenHeight;
        }
        renderer_options.cache_dir = opt.user_dir / "cache";
        g_renderer = gfx::create_vulkan_renderer(renderer_options, &error);
        if (!g_renderer) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        host::set_renderer(g_renderer.get());
        ui::attach(g_renderer.get());
    }
    ge::init(g_renderer.get());
    if (!audio_dump.empty() && !audio::output_record(audio_dump)) std::fprintf(stderr, "cannot write %s\n", audio_dump.c_str());
    if (!boot(&error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const u64 run_started = ge::host_ticks();
    kernel::run();
    const ge::Stats& gs = ge::stats();
    {
        // Where the host time went: display-list processing, presentation, and everything else
        // (the translated game code and the system-call layer).
        const double rate = double(ge::host_ticks_per_second());
        const double total = double(ge::host_ticks() - run_started) / rate;
        const double lists = double(gs.ticks) / rate, present = double(host::present_ticks()) / rate;
        const double game_seconds = double(kernel::now_us()) / 1e6;
        LOG_INFO("perf", "%.2f s of host time for %.2f s of game time (%.1fx): game code %.2f s, graphics %.2f s, presentation %.2f s",
                 total, game_seconds, total > 0.0 ? game_seconds / total : 0.0, total - lists - present, lists, present);
    }
    LOG_INFO("boot", "stopped after %llu frame(s), %.2f s of game time; %llu display lists, %llu draws, %llu vertices",
             static_cast<unsigned long long>(host::frames_shown()), double(kernel::now_us()) / 1e6,
             static_cast<unsigned long long>(gs.lists), static_cast<unsigned long long>(gs.draws),
             static_cast<unsigned long long>(gs.vertices));
    host::set_renderer(nullptr);
    g_renderer.reset();
    ui::shutdown();
    audio::output_shutdown();
    host::window_shutdown();
    return kernel::exit_code();
}
