#include "tools/texup/realesrgan.h"

#include "lib/common/file.h"

#include <set>
#include <vector>

#include <windows.h>

namespace p2::texup::realesrgan {

namespace {

constexpr const char* kAnimeVideo = "realesr-animevideov3";

bool is_black(const u8* p) { return p[0] < 8 && p[1] < 8 && p[2] < 8; }

} // namespace

std::filesystem::path default_program() { return program_folder() / "realesrgan" / "realesrgan-ncnn-vulkan.exe"; }

u32 native_scale(const std::string& model, u32 wanted) {
    if (model == kAnimeVideo && wanted >= 2 && wanted <= 4) return wanted;
    return 4;
}

std::vector<std::string> list_models(const std::filesystem::path& models_dir) {
    std::set<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(models_dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".param") continue;
        std::string name = entry.path().stem().string();
        if (name.rfind(kAnimeVideo, 0) == 0) name = kAnimeVideo; // -x2, -x3, -x4
        names.insert(name);
    }
    return {names.begin(), names.end()};
}

std::wstring command_line(const std::filesystem::path& program, const std::filesystem::path& input,
                          const std::filesystem::path& output, const std::string& model, u32 scale, int gpu) {
    const std::filesystem::path models = program.parent_path() / "models";
    std::wstring cmd = L"\"" + program.wstring() + L"\" -i \"" + input.wstring() + L"\" -o \"" + output.wstring() + L"\" -m \"" +
                       models.wstring() + L"\" -n " + utf8_path(model).wstring() + L" -s " + std::to_wstring(native_scale(model, scale)) +
                       L" -f png";
    if (gpu >= 0) cmd += L" -g " + std::to_wstring(gpu);
    return cmd;
}

bool black_result(const Rgba& source, const Rgba& result) {
    bool source_black = true;
    for (size_t i = 0; i + 3 < source.pixels.size() && source_black; i += 4)
        if (source.pixels[i + 3] && !is_black(&source.pixels[i])) source_black = false;
    if (source_black) return false;
    for (size_t i = 0; i + 3 < result.pixels.size(); i += 4)
        if (!is_black(&result.pixels[i])) return false;
    return true;
}

int run(const std::wstring& command, std::string* output) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return -1;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // The job ends the program when this one ends (or is ended by the game).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    // The child gets its output pipe and nothing else this process could pass on (such as the pipe
    // to the game).
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
    std::wstring cmd = command;
    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup.StartupInfo, &info);
    if (listed) DeleteProcThreadAttributeList(attributes);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        if (job) CloseHandle(job);
        return -1;
    }
    if (job) AssignProcessToJobObject(job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got)
        if (output) output->append(buffer, got);
    CloseHandle(read_end);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    if (job) CloseHandle(job);
    return int(code);
}

} // namespace p2::texup::realesrgan
