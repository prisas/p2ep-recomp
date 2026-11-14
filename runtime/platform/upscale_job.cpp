#include "runtime/platform/upscale_job.h"

#include "lib/common/file.h"
#include "lib/common/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace p2::host::upscale {

namespace {

std::filesystem::path tool_path() { return program_folder() / "p2texup.exe"; }

std::mutex g_mutex;
HANDLE g_process = nullptr;
std::thread g_reader;
std::atomic<bool> g_running{false};
std::atomic<u32> g_done{0}, g_total{0};
bool g_has_result = false; // guarded by g_mutex
Result g_result;

// Reads the program's output: "progress <done> <total>" while it works, "done <upscaled>
// <up to date> <kept> <failed>" at the end, and other lines meant for people (logged).
void read_output(HANDLE pipe, HANDLE process) {
    Result result;
    bool summary = false;
    std::string pending;
    char buffer[4096];
    DWORD got = 0;
    auto line = [&](const std::string& text) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(text.c_str(), "progress %u %u", &a, &b) == 2) {
            g_done = a;
            g_total = b;
        } else if (std::sscanf(text.c_str(), "done %u %u %u %u", &a, &b, &c, &d) == 4) {
            result.upscaled = a;
            result.up_to_date = b;
            result.kept = c;
            result.failed = d;
            summary = true;
        } else if (!text.empty()) {
            LOG_INFO("upscale", "%s", text.c_str());
        }
    };
    while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got) {
        pending.append(buffer, got);
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            std::string text = pending.substr(0, end);
            if (!text.empty() && text.back() == '\r') text.pop_back();
            line(text);
            pending.erase(0, end + 1);
        }
    }
    line(pending);
    CloseHandle(pipe);

    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    result.ok = summary && code == 0;
    std::lock_guard lock(g_mutex);
    g_result = result;
    g_has_result = true;
    g_running = false;
}

void join_reader() {
    if (g_reader.joinable()) g_reader.join();
    if (g_process) {
        CloseHandle(g_process);
        g_process = nullptr;
    }
}

} // namespace

bool available() {
    std::error_code ec;
    return std::filesystem::is_regular_file(tool_path(), ec);
}

bool start(const std::filesystem::path& user_dir, std::string* error) {
    if (g_running) return true;
    join_reader(); // the previous run's reader has finished: g_running is false

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        *error = "cannot create a pipe for the upscaler";
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // The child gets the pipe and nothing else this process could pass on (an open log file, say).
    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
    std::vector<u8> attributes_storage(attributes_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_storage.data());
    HANDLE inherited[] = {write_end};
    const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size) &&
                        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                                  nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = write_end;
    startup.lpAttributeList = listed ? attributes : nullptr;
    std::wstring command = L"\"" + tool_path().wstring() + L"\" --progress --user-dir \"" +
                           std::filesystem::absolute(user_dir).wstring() + L"\"";
    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                        &startup.StartupInfo, &info);
    if (listed) DeleteProcThreadAttributeList(attributes);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        *error = "cannot start p2texup.exe";
        return false;
    }
    CloseHandle(info.hThread);
    LOG_INFO("upscale", "upscaling the dumped textures of %s", path_utf8(user_dir).c_str());

    std::lock_guard lock(g_mutex);
    g_process = info.hProcess;
    g_done = 0;
    g_total = 0;
    g_has_result = false;
    g_running = true;
    g_reader = std::thread(read_output, read_end, info.hProcess);
    return true;
}

bool running() { return g_running; }

Progress progress() { return {g_done.load(), g_total.load()}; }

bool take_result(Result* result) {
    std::lock_guard lock(g_mutex);
    if (!g_has_result) return false;
    g_has_result = false;
    *result = g_result;
    return true;
}

void stop() {
    if (g_running && g_process) TerminateProcess(g_process, 1); // the reader then sees the pipe close
    join_reader();
}

} // namespace p2::host::upscale
