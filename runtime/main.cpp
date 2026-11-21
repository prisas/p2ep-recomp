#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "lib/common/log.h"
#include "lib/psp/prx.h"
#include "runtime/audio/audio_out.h"
#include "runtime/cpu/recomp.h"
#include "runtime/cpu/rt.h"
#include "runtime/ge/ge.h"
#include "runtime/gfx/texture_pack.h"
#include "runtime/gfx/vk/vk_renderer.h"
#include "runtime/hle/hle_display.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"
#include "runtime/platform/config.h"
#include "runtime/platform/control.h"
#include "runtime/platform/frame.h"
#include "runtime/platform/host.h"
#include "runtime/platform/upscale_job.h"
#include "runtime/platform/window.h"
#include "runtime/ui/launcher.h"
#include "runtime/ui/overlay.h"

#include <SDL3/SDL_messagebox.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

using namespace p2;

namespace {

constexpr const char* kTitle = "Persona 2: Eternal Punishment";
constexpr const char* kExecutablePath = "PSP_GAME/SYSDIR/EBOOT.BIN";
constexpr u32 kMinRootStack = 0x40000;

std::unique_ptr<gfx::Renderer> g_renderer;
bool g_console = false; // messages written to the standard streams reach someone

// The program is a windowed one, without a console of its own. Started from a terminal it writes
// its messages there; with its output redirected, they go where they were sent.
void connect_console() {
    auto usable = [](DWORD which) {
        const HANDLE handle = GetStdHandle(which);
        return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    const bool out = usable(STD_OUTPUT_HANDLE), err = usable(STD_ERROR_HANDLE);
    g_console = out || err;
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS)) return;
    g_console = true;
    FILE* stream = nullptr;
    if (!out) freopen_s(&stream, "CONOUT$", "w", stdout);
    if (!err) freopen_s(&stream, "CONOUT$", "w", stderr);
}

// Without a console, a player would never see a message on the standard streams: show it.
void show_message(const std::string& text) {
    if (!g_console) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, text.c_str(), static_cast<SDL_Window*>(host::window_handle()));
}

void report_error(const std::string& text) {
    std::fprintf(stderr, "error: %s\n", text.c_str());
    show_message(text);
}

int usage(const std::string& problem = {}) {
    const std::string text = (problem.empty() ? "" : problem + "\n\n") +
        "usage: p2ep [disc-image] [options]\n"
              "\n"
              "Without a disc image, the launcher opens: choose the image and the settings there.\n"
              "\n"
              "  --user-dir <dir>      where saves and settings are kept (default: userdata next to p2ep.exe)\n"
              "  --scale <n>           rendering resolution multiplier, 1-10 (default: match the window)\n"
              "  --fullscreen          start in full screen\n"
              "  --window-size <WxH>   window size in pixels this time, for example 1920x1088\n"
              "  --skip-intro          start at the main menu this time, skipping notices, logos and movie\n"
              "  --headless            run without a window or audio device\n"
              "  --overlay             with --headless: draw the overlay and let scripted input answer dialogs\n"
              "  --unthrottled         do not pace the game to real time\n"
              "  --frames <n>          quit after n displayed frames\n"
              "  --dump-frames <dir>   write displayed frames as PNG files\n"
              "  --dump-every <n>      ... every nth frame (default 60)\n"
              "  --dump-textures       dump textures this time (to <user dir>/textures/dump), whatever the setting\n"
              "  --dump-audio <file>   record the game's audio output to a WAV file\n"
              "  --input <script>      scripted pad input: frame:hexbuttons,frame:hexbuttons,... (or @file)\n"
              "  --no-gpu              do not create a renderer (system-call testing only)\n"
              "  --log <channels>      comma-separated trace channels: hle,io,thread,mem,display,ge,audio,all\n"
              "  --log-file <path>     also write the log to a file\n";
    std::fputs(text.c_str(), problem.empty() ? stdout : stderr);
    if (!g_console) SDL_ShowSimpleMessageBox(problem.empty() ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, kTitle, text.c_str(), nullptr);
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

// Opens the disc image and reads the game's executable from it, making sure it is the one this
// build was made from. On failure `problem` says what is wrong, in words for the player.
bool open_game(const std::filesystem::path& image, std::vector<u8>* executable, std::string* problem) {
    std::string error;
    if (!host::open_image(image, &error)) {
        *problem = "This file cannot be read as a disc image: " + error + ".";
        return false;
    }
    std::vector<u8> bytes;
    if (!host::iso().read_all(kExecutablePath, bytes)) {
        *problem = "This disc image does not hold a PSP game.";
        return false;
    }
    const std::string digest = to_hex(sha256(bytes.data(), bytes.size()));
    if (digest != g_gen_module.sha256) {
        LOG_INFO("boot", "the image's executable is %s; this build was made from %s", digest.c_str(), g_gen_module.sha256);
        *problem = "This disc image holds a different game, or another version of it, than the one this build was made "
                   "from.";
        return false;
    }
    if (executable) *executable = std::move(bytes);
    return true;
}

bool check_image(const std::filesystem::path& image, std::string* problem) { return open_game(image, nullptr, problem); }

bool boot(std::string* error) {
    std::vector<u8> executable;
    if (!open_game(host::options().image, &executable, error)) return false;

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

int wmain(int argc, wchar_t** wargv) {
    connect_console();
    // Holding Shift while the program starts asks for the launcher even when it is switched off.
    const bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) argv.push_back(to_utf8(wargv[i]));

    host::Options& opt = host::options();
    opt.user_dir = program_folder() / "userdata";
    opt.dump_every = 60;
    std::string channels, log_file, audio_dump;
    bool no_gpu = false, fullscreen = false, skip_intro = false, dump_textures = false;
    u32 window_w = 0, window_h = 0; // 0: the saved setting
    u32 scale = 0; // 0: use the saved setting

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[size_t(i)];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[size_t(++i)] : std::string(); };
        if (a == "--user-dir") opt.user_dir = utf8_path(value());
        else if (a == "--scale") scale = u32(std::strtoul(value().c_str(), nullptr, 10));
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--window-size") { // <width>x<height>, in pixels
            const std::string spec = value();
            char* end = nullptr;
            window_w = u32(std::strtoul(spec.c_str(), &end, 10));
            window_h = (*end == 'x' || *end == 'X') ? u32(std::strtoul(end + 1, nullptr, 10)) : 0;
            if (window_w < 320 || window_h < 180) return usage("--window-size wants <width>x<height>, at least 320x180.");
        }
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
        else if (a == "--control") host::control::init(utf8_path(value())); // see runtime/platform/control.h
        else if (a == "--frames") opt.frame_limit = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-frames") opt.dump_dir = utf8_path(value());
        else if (a == "--dump-every") opt.dump_every = u32(std::strtoul(value().c_str(), nullptr, 10));
        else if (a == "--dump-from") opt.dump_from = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-textures") dump_textures = true;
        else if (a == "--dump-audio") audio_dump = value();
        else if (a == "--trace-frame") opt.trace_frame = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--log-from") opt.late_from = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-ram") { // <file>@<frame>[:<to frame>:<every>]
            const std::string spec = value();
            const size_t at = spec.rfind('@');
            opt.ram_dump = utf8_path(spec.substr(0, at));
            char* end = nullptr;
            opt.ram_dump_frame = at == std::string::npos ? 0 : std::strtoull(spec.c_str() + at + 1, &end, 10);
            if (end && *end == ':') {
                opt.ram_dump_to = std::strtoull(end + 1, &end, 10);
                opt.ram_dump_step = *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 0;
            }
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
                if (!read_file(utf8_path(script.substr(1)), bytes)) {
                    report_error("cannot read the input script " + script.substr(1));
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
        else if (!a.empty() && a[0] != '-' && opt.image.empty()) opt.image = utf8_path(a);
        else return usage("Unknown option '" + a + "'.");
    }
    // The launcher needs the window and the renderer.
    if (opt.image.empty() && (opt.headless || no_gpu)) return usage("A disc image is needed with --headless or --no-gpu.");

    if (opt.late_from) opt.late_channels = channels;
    else if (!channels.empty()) log::enable_channels(channels);
    if (!log_file.empty() && !log::open_file(log_file)) std::fprintf(stderr, "cannot open log file %s\n", log_file.c_str());
    log::set_fatal_hook(kernel::dump_state);
    log::set_fatal_report([](const char* message) { show_message(std::string("The game stopped because of an error:\n\n") + message); });
    AddVectoredExceptionHandler(1, fault_handler);

    // Settings come from the user folder; options given on the command line win for this run.
    host::load_settings(opt.user_dir / "settings.ini");
    host::Settings& settings = host::settings();
    settings.run_scale = std::min<u32>(scale, 10);
    settings.run_skip_intro = skip_intro;
    settings.run_dump_textures = dump_textures;
    settings.run_window_width = window_w;
    settings.run_window_height = window_h;
    gfx::textures::init(opt.user_dir, program_folder() / "texturepacks");
    settings.run_unthrottled = opt.unthrottled;
    if (fullscreen) settings.fullscreen = true;

    std::string error;
    if (!mem::init(&error) || !host::window_init(&error)) {
        report_error(error);
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
            report_error(error);
            return 1;
        }
        host::set_renderer(g_renderer.get());
        ui::attach(g_renderer.get());
    }
    auto shut_down = [] {
        host::upscale::stop(); // an upscaling run started from the settings
        host::set_renderer(nullptr);
        g_renderer.reset();
        gfx::textures::shutdown();
        ui::shutdown();
        audio::output_shutdown();
        host::window_shutdown();
    };

    if (opt.image.empty()) {
        // Started without an image: the launcher. Only when the player switched it off, the image
        // last chosen still passes the check and Shift is not held does the game start at once.
        const std::filesystem::path last = utf8_path(settings.image);
        std::string problem;
        const bool playable = !last.empty() && check_image(last, &problem);
        if (playable && !settings.show_launcher && !shift_held) opt.image = last;
        else opt.image = ui::run_launcher(*g_renderer, check_image, last, problem);
        if (opt.image.empty()) { // the player closed the launcher
            shut_down();
            return 0;
        }
    }

    ge::init(g_renderer.get());
    if (!audio_dump.empty() && !audio::output_record(audio_dump)) std::fprintf(stderr, "cannot write %s\n", audio_dump.c_str());
    if (!boot(&error)) {
        report_error(error);
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
    shut_down();
    return kernel::exit_code();
}
