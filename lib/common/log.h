#pragma once

#include "lib/common/types.h"

#include <string>

namespace p2::log {

enum class Level : u8 { Info, Warn, Error, Off };

// Channels can be enabled individually (for example "hle,ge") on top of the global level.
Level level();
void enable_channels(const std::string& comma_separated);
bool channel_enabled(const char* channel);
bool open_file(const std::string& path);

void write(Level level, const char* channel, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Called by fatal() before the process exits; used by the runtime to dump guest state.
void set_fatal_hook(void (*hook)());

} // namespace p2::log

#define LOG_AT(lvl, ch, ...)                                                                                 \
    do {                                                                                                     \
        if (P2_UNLIKELY(static_cast<int>(lvl) >= static_cast<int>(::p2::log::level())))                     \
            ::p2::log::write(lvl, ch, __VA_ARGS__);                                                          \
    } while (0)

#define LOG_INFO(ch, ...) LOG_AT(::p2::log::Level::Info, ch, __VA_ARGS__)
#define LOG_WARN(ch, ...) LOG_AT(::p2::log::Level::Warn, ch, __VA_ARGS__)
#define LOG_ERROR(ch, ...) LOG_AT(::p2::log::Level::Error, ch, __VA_ARGS__)

// Verbose per-channel tracing that is off unless the channel was requested explicitly.
#define LOG_CH(ch, ...)                                                                                      \
    do {                                                                                                     \
        if (P2_UNLIKELY(::p2::log::channel_enabled(ch)))                                                     \
            ::p2::log::write(::p2::log::Level::Info, ch, __VA_ARGS__);                                       \
    } while (0)

#define P2_FATAL(...) ::p2::log::fatal(__VA_ARGS__)
#define P2_ASSERT(cond)                                                                                      \
    do {                                                                                                     \
        if (P2_UNLIKELY(!(cond)))                                                                            \
            ::p2::log::fatal("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);                     \
    } while (0)
