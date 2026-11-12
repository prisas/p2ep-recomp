// SysMemUserForUser, ModuleMgrForUser, LoadExecForUser, UtilsForUser, InterruptManager,
// sceSuspendForUser and the remaining pieces of Kernel_Library.

#include "runtime/hle/hle.h"

#include <chrono>
#include <ctime>

using namespace p2;
using namespace p2::kernel;

namespace {

Uid g_exit_callback = 0;
bool g_interrupts_enabled = true;

// Wall-clock time advances with guest time from the moment the game started.
u64 unix_time_us() {
    static const u64 start = u64(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
    return start + now_us();
}

u32 alloc_block(const std::string& name, AllocType type, u32 size, u32 addr) {
    u32 block_addr = alloc_user(name, type, size, addr);
    if (!block_addr) return kErrNoMemory;
    auto block = std::make_unique<MemBlock>();
    block->type = ObjType::MemBlock;
    block->name = name;
    block->addr = block_addr;
    block->size = size;
    return u32(add_object(std::move(block)));
}

// Expands the handful of printf conversions that appear in debug output, taking arguments from
// the registers after the format string.
std::string format_guest(Cpu* c, u32 first_arg) {
    std::string fmt = mem::read_string(ARG(first_arg - 4), 512);
    std::string out;
    u32 reg = first_arg + 1;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out.push_back(fmt[i]);
            continue;
        }
        size_t j = i + 1;
        while (j < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[j])) || fmt[j] == '-' || fmt[j] == '.' ||
                                  fmt[j] == 'l' || fmt[j] == 'h' || fmt[j] == '0'))
            ++j;
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        if (conv == '%') {
            out.push_back('%');
        } else {
            u32 value = reg < 12 ? c->r[reg] : 0;
            ++reg;
            char buf[64];
            switch (conv) {
            case 's': out += mem::read_string(value, 256); break;
            case 'd':
            case 'i': std::snprintf(buf, sizeof(buf), "%d", s32(value)); out += buf; break;
            case 'u': std::snprintf(buf, sizeof(buf), "%u", value); out += buf; break;
            case 'c': out.push_back(char(value)); break;
            default: std::snprintf(buf, sizeof(buf), "%x", value); out += buf; break;
            }
        }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

} // namespace

// --- SysMemUserForUser ----------------------------------------------------------------------------------------

HLE(sceKernelMaxFreeMemSize) { RET(user_max_free()); }
HLE(sceKernelTotalFreeMemSize) { RET(user_total_free()); }

HLE(sceKernelAllocPartitionMemory) {
    u32 partition = ARG(0), type = ARG(2), size = ARG(3), addr = ARG(4);
    std::string name = mem::read_string(ARG(1), 31);
    if (partition != 2 && partition != 6) RET(kErrIllegalArgument);
    if (type > 4) RET(kErrIllegalMemblock);
    if (size == 0) RET(kErrIllegalMemsize);
    RET(alloc_block(name, AllocType(type), size, addr));
}

HLE(sceKernelFreePartitionMemory) {
    MemBlock* block = get<MemBlock>(Uid(ARG(0)));
    if (!block) RET(kErrUnknownUid);
    free_user(block->addr);
    destroy_object(block->uid);
    RET(0);
}

HLE(sceKernelGetBlockHeadAddr) {
    MemBlock* block = get<MemBlock>(Uid(ARG(0)));
    RET(block ? block->addr : 0);
}

HLE(sceKernelAllocMemoryBlock) {
    u32 type = ARG(1), size = ARG(2);
    if (type > 1) RET(kErrIllegalMemblock);
    if (size == 0) RET(kErrIllegalMemsize);
    RET(alloc_block(mem::read_string(ARG(0), 31), AllocType(type), size, 0));
}

HLE(sceKernelFreeMemoryBlock) {
    MemBlock* block = get<MemBlock>(Uid(ARG(0)));
    if (!block) RET(kErrUnknownUid);
    free_user(block->addr);
    destroy_object(block->uid);
    RET(0);
}

HLE(sceKernelGetMemoryBlockAddr) {
    MemBlock* block = get<MemBlock>(Uid(ARG(0)));
    if (!block) RET(kErrUnknownUid);
    if (!mem::valid(ARG(1), 4)) RET(kErrIllegalAddr);
    mem::w32(ARG(1), block->addr);
    RET(0);
}

HLE(sceKernelSetCompilerVersion) {
    module().compiler_version = ARG(0);
    RET(0);
}

HLE(sceKernelSetCompiledSdkVersion606) {
    module().sdk_version = ARG(0);
    RET(0);
}

HLE(sceKernelPrintf) {
    LOG_INFO("guest", "%s", format_guest(c, 4).c_str());
    RET(0);
}

// --- ModuleMgrForUser -----------------------------------------------------------------------------------------

HLE(sceKernelGetModuleId) { RET(module().uid); }

HLE(sceKernelGetModuleIdByAddress) {
    u32 addr = ARG(0);
    RET(addr >= module().base && addr < module().end ? u32(module().uid) : kErrUnknownUid);
}

HLE(sceKernelStopModule) { RET(0); }
HLE(sceKernelUnloadModule) { RET(0); }

HLE(sceKernelStopUnloadSelfModuleWithStatus) {
    LOG_INFO("kernel", "the game unloaded itself (status %d)", s32(ARG(0)));
    request_stop(0);
    RET(0);
}

// --- LoadExecForUser ------------------------------------------------------------------------------------------

HLE(sceKernelExitGame) {
    LOG_INFO("kernel", "the game asked to exit");
    request_stop(0);
    RET(0);
}

HLE(sceKernelRegisterExitCallback) {
    g_exit_callback = Uid(ARG(0));
    RET(0);
}

// --- StdioForUser ---------------------------------------------------------------------------------------------

HLE(sceKernelStdin) { RET(0); }
HLE(sceKernelStdout) { RET(1); }
HLE(sceKernelStderr) { RET(2); }

// --- UtilsForUser ---------------------------------------------------------------------------------------------

HLE(sceKernelLibcTime) {
    u32 t = u32(unix_time_us() / 1000000);
    if (ARG(0) && mem::valid(ARG(0), 4)) mem::w32(ARG(0), t);
    RET(t);
}

HLE(sceKernelLibcClock) { RET(u32(now_us())); }

HLE(sceKernelLibcGettimeofday) {
    u64 us = unix_time_us();
    if (ARG(0) && mem::valid(ARG(0), 8)) {
        mem::w32(ARG(0), u32(us / 1000000));
        mem::w32(ARG(0) + 4, u32(us % 1000000));
    }
    if (ARG(1) && mem::valid(ARG(1), 8)) {
        mem::w32(ARG(1), 0);
        mem::w32(ARG(1) + 4, 0);
    }
    RET(0);
}

// Cache maintenance has nothing to do here: there is no cache between the game and its memory.
HLE(sceKernelDcacheWritebackAll) { RET(0); }
HLE(sceKernelDcacheWritebackRange) { RET(0); }

// --- InterruptManager -----------------------------------------------------------------------------------------

HLE(sceKernelRegisterSubIntrHandler) {
    LOG_CH("thread", "register sub-interrupt %u/%u handler %08x arg %08x", ARG(0), ARG(1), ARG(2), ARG(3));
    RET(register_sub_interrupt(ARG(0), ARG(1), ARG(2), ARG(3)));
}
HLE(sceKernelReleaseSubIntrHandler) { RET(release_sub_interrupt(ARG(0), ARG(1))); }
HLE(sceKernelEnableSubIntr) { RET(enable_sub_interrupt(ARG(0), ARG(1), true)); }

// --- Kernel_Library -------------------------------------------------------------------------------------------

// With every guest thread on one host thread and interrupts delivered only at scheduling points,
// masking interrupts just has to be reported consistently.
HLE(sceKernelCpuSuspendIntr) {
    u32 previous = g_interrupts_enabled ? 1 : 0;
    g_interrupts_enabled = false;
    RET(previous);
}

HLE(sceKernelCpuResumeIntr) {
    g_interrupts_enabled = ARG(0) != 0;
    RET(0);
}

HLE(sceKernelMemcpy) {
    u32 dst = ARG(0), src = ARG(1), size = ARG(2);
    if (size && mem::valid(dst, size) && mem::valid(src, size)) std::memmove(mem::ptr(dst), mem::ptr(src), size);
    RET(dst);
}

HLE(sceKernelMemset) {
    u32 dst = ARG(0), size = ARG(2);
    if (size && mem::valid(dst, size)) std::memset(mem::ptr(dst), int(ARG(1) & 0xFF), size);
    RET(dst);
}

// --- sceSuspendForUser ----------------------------------------------------------------------------------------

HLE(sceKernelPowerTick) { RET(0); }

namespace p2::hle {

Uid exit_callback() { return g_exit_callback; }
u64 wall_clock_us() { return unix_time_us(); }

} // namespace p2::hle
