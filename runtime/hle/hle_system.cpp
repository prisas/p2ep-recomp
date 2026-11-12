// Small system libraries: sceUmdUser, scePower, sceRtc, sceImpose and the module-loading part of
// sceUtility.

#include "runtime/hle/hle.h"

#include <chrono>
#include <ctime>

using namespace p2;
using namespace p2::kernel;

namespace p2::hle {
u64 wall_clock_us();
}

namespace {

constexpr u32 kUmdPresent = 0x02, kUmdReady = 0x10, kUmdReadable = 0x20;
constexpr u32 kUmdState = kUmdPresent | kUmdReady | kUmdReadable;

// Microseconds between 0001-01-01 and the Unix epoch; RTC ticks count from year 1.
constexpr u64 kRtcUnixOffsetUs = 62135596800000000ull;

s64 days_from_civil(s64 y, u32 m, u32 d) {
    y -= m <= 2;
    const s64 era = (y >= 0 ? y : y - 399) / 400;
    const u32 yoe = u32(y - era * 400);
    const u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + s64(doe) - 719468;
}

} // namespace

// --- sceUmdUser -----------------------------------------------------------------------------------------------
// The disc is always present, spun up and readable.

HLE(sceUmdCheckMedium) { RET(1); }
HLE(sceUmdActivate) { RET(0); }
HLE(sceUmdGetDriveStat) { RET(kUmdState); }
HLE(sceUmdWaitDriveStat) { RET(0); }
HLE(sceUmdWaitDriveStatCB) {
    run_pending_callbacks();
    RET(0);
}
HLE(sceUmdCancelWaitDriveStat) { RET(0); }

// --- scePower -------------------------------------------------------------------------------------------------

HLE(scePowerRegisterCallback) { RET(0); }
HLE(scePowerSetClockFrequency350) { RET(0); }

// --- sceRtc ---------------------------------------------------------------------------------------------------

HLE(sceRtcGetCurrentClockLocalTime) {
    u32 out = ARG(0);
    if (!mem::valid(out, 16)) RET(kErrIllegalAddr);
    u64 us = hle::wall_clock_us();
    std::time_t seconds = std::time_t(us / 1000000);
    std::tm local{};
    localtime_s(&local, &seconds);
    mem::w16(out + 0, u16(local.tm_year + 1900));
    mem::w16(out + 2, u16(local.tm_mon + 1));
    mem::w16(out + 4, u16(local.tm_mday));
    mem::w16(out + 6, u16(local.tm_hour));
    mem::w16(out + 8, u16(local.tm_min));
    mem::w16(out + 10, u16(local.tm_sec));
    mem::w32(out + 12, u32(us % 1000000));
    RET(0);
}

HLE(sceRtcGetTick) {
    u32 in = ARG(0), out = ARG(1);
    if (!mem::valid(in, 16) || !mem::valid(out, 8)) RET(kErrIllegalAddr);
    s64 days = days_from_civil(mem::r16(in), mem::r16(in + 2), mem::r16(in + 4));
    s64 seconds = days * 86400 + s64(mem::r16(in + 6)) * 3600 + s64(mem::r16(in + 8)) * 60 + s64(mem::r16(in + 10));
    u64 tick = u64(seconds * 1000000 + s64(mem::r32(in + 12))) + kRtcUnixOffsetUs;
    mem::w64(out, tick);
    RET(0);
}

// --- sceImpose ------------------------------------------------------------------------------------------------

HLE(sceImposeSetLanguageMode) { RET(0); }

// --- sceUtility: system module loading ------------------------------------------------------------------------
// Codec and network modules are part of this runtime, so loading one always succeeds.

HLE(sceUtilityLoadModule) {
    LOG_CH("hle", "load system module %04x", ARG(0));
    RET(0);
}
