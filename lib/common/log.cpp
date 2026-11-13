#include "lib/common/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace p2::log {

namespace {

std::atomic<Level> g_level{Level::Info};
std::atomic<bool> g_any_channel{false};
std::vector<std::string> g_channels;
bool g_all_channels = false;
std::mutex g_mutex;
FILE* g_file = nullptr;
void (*g_fatal_hook)() = nullptr;

const char* level_tag(Level level) {
    switch (level) {
    case Level::Trace: return "T";
    case Level::Debug: return "D";
    case Level::Info: return "I";
    case Level::Warn: return "W";
    case Level::Error: return "E";
    default: return "?";
    }
}

void emit(Level level, const char* channel, const char* fmt, va_list args) {
    char buffer[2048];
    int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (n < 0) return;
    std::lock_guard lock(g_mutex);
    // With a log file open, routine messages go only there (the console is unbuffered and would
    // slow a verbose trace to a crawl); warnings and errors always reach the console too.
    if (!g_file || level >= Level::Warn) std::fprintf(stderr, "[%s %s] %s\n", level_tag(level), channel, buffer);
    if (g_file) {
        std::fprintf(g_file, "[%s %s] %s\n", level_tag(level), channel, buffer);
        if (level >= Level::Warn) std::fflush(g_file);
    }
}

} // namespace

void set_level(Level level) { g_level.store(level, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

void enable_channels(const std::string& comma_separated) {
    std::lock_guard lock(g_mutex);
    size_t start = 0;
    while (start <= comma_separated.size()) {
        size_t end = comma_separated.find(',', start);
        if (end == std::string::npos) end = comma_separated.size();
        std::string name = comma_separated.substr(start, end - start);
        if (name == "all") g_all_channels = true;
        else if (!name.empty()) g_channels.push_back(name);
        start = end + 1;
    }
    g_any_channel.store(g_all_channels || !g_channels.empty(), std::memory_order_relaxed);
}

bool channel_enabled(const char* channel) {
    if (!g_any_channel.load(std::memory_order_relaxed)) return false;
    if (g_all_channels) return true;
    for (const auto& name : g_channels)
        if (name == channel) return true;
    return false;
}

bool open_file(const std::string& path) {
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = std::fopen(path.c_str(), "w");
    if (g_file) std::setvbuf(g_file, nullptr, _IOFBF, 1 << 20);
    return g_file != nullptr;
}

void write(Level level, const char* channel, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(level, channel, fmt, args);
    va_end(args);
}

void set_fatal_hook(void (*hook)()) { g_fatal_hook = hook; }

void fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(Level::Error, "fatal", fmt, args);
    va_end(args);
    if (auto hook = g_fatal_hook) {
        g_fatal_hook = nullptr; // a failing hook must not recurse
        hook();
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_file) std::fflush(g_file);
        std::fflush(stderr);
    }
    std::_Exit(3);
}

} // namespace p2::log
