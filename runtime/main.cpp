#include "lib/common/file.h"
#include "lib/common/hash.h"
#include "lib/common/log.h"
#include "lib/psp/prx.h"
#include "runtime/cpu/recomp.h"
#include "runtime/cpu/rt.h"
#include "runtime/hle/hle_display.h"
#include "runtime/kernel/kernel.h"
#include "runtime/mem/memory.h"
#include "runtime/platform/host.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <windows.h>

using namespace p2;

namespace {

constexpr const char* kExecutablePath = "PSP_GAME/SYSDIR/EBOOT.BIN";
constexpr u32 kMinRootStack = 0x40000;

u64 g_frames = 0;

int usage() {
    std::puts("usage: p2ep <disc-image> [options]\n"
              "\n"
              "  --user-dir <dir>      where saves and settings are kept (default: ./userdata)\n"
              "  --headless            run without a window or audio device\n"
              "  --unthrottled         do not pace the game to real time\n"
              "  --frames <n>          quit after n displayed frames\n"
              "  --dump-frames <dir>   write displayed frames as PNG files\n"
              "  --dump-every <n>      ... every nth frame (default 60)\n"
              "  --log <channels>      comma-separated trace channels: hle,io,thread,mem,display,ge,audio,all\n"
              "  --log-file <path>     also write the log to a file\n"
              "  --verbose             include debug messages");
    return 2;
}

// Reports a host fault in terms the guest can be debugged with.
LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const void* address = reinterpret_cast<const void*>(rec->ExceptionInformation[1]);
    const char* kind = rec->ExceptionInformation[0] == 1 ? "write to" : "read from";
    u32 guest = 0;
    if (mem::host_to_guest(address, &guest))
        P2_FATAL("the game tried to %s unmapped guest address %08x", kind, guest);
    P2_FATAL("host access violation: %s %p (code at %p)", kind, address, rec->ExceptionAddress);
}

void frame_hook() {
    ++g_frames;
    const host::Options& opt = host::options();
    if (opt.frame_limit && g_frames >= opt.frame_limit) kernel::request_stop(0);
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
    hle::display_init(frame_hook);
    kernel::start_thread(root, u32(path.size()) + 1, arg);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    host::Options& opt = host::options();
    opt.user_dir = "userdata";
    opt.dump_every = 60;
    std::string channels, log_file;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--user-dir") opt.user_dir = value();
        else if (a == "--headless") opt.headless = true;
        else if (a == "--unthrottled") opt.unthrottled = true;
        else if (a == "--frames") opt.frame_limit = std::strtoull(value().c_str(), nullptr, 10);
        else if (a == "--dump-frames") opt.dump_dir = value();
        else if (a == "--dump-every") opt.dump_every = u32(std::strtoul(value().c_str(), nullptr, 10));
        else if (a == "--log") channels = value();
        else if (a == "--log-file") log_file = value();
        else if (a == "--verbose") verbose = true;
        else if (a == "--help" || a == "-h") return usage();
        else if (!a.empty() && a[0] != '-' && opt.image.empty()) opt.image = a;
        else {
            std::fprintf(stderr, "unknown option '%s'\n\n", a.c_str());
            return usage();
        }
    }
    if (opt.image.empty()) return usage();

    log::set_level(verbose ? log::Level::Debug : log::Level::Info);
    if (!channels.empty()) log::enable_channels(channels);
    if (!log_file.empty() && !log::open_file(log_file)) std::fprintf(stderr, "cannot open log file %s\n", log_file.c_str());
    log::set_fatal_hook(kernel::dump_state);
    AddVectoredExceptionHandler(1, fault_handler);

    std::string error;
    if (!mem::init(&error) || !boot(&error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    kernel::run();
    LOG_INFO("boot", "stopped after %llu frame(s), %.2f s of game time", static_cast<unsigned long long>(g_frames),
             double(kernel::now_us()) / 1e6);
    return kernel::exit_code();
}
