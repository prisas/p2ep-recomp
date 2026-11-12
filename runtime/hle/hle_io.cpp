// IoFileMgrForUser: files on the disc image, files on the memory stick (a host folder) and the
// standard streams. Asynchronous operations run to completion at once; only their result is
// delivered a moment later, so loading never waits for an imaginary drive.

#include "runtime/hle/hle.h"
#include "runtime/platform/host.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

using namespace p2;
using namespace p2::kernel;

namespace {

constexpr u32 kOpenRead = 0x0001, kOpenWrite = 0x0002, kOpenAppend = 0x0100, kOpenCreate = 0x0200,
              kOpenTruncate = 0x0400, kOpenExclusive = 0x0800;
constexpr int kFirstFd = 3, kMaxFd = 64;
constexpr u64 kAsyncDelayUs = 100;

enum class Device { None, Disc, MemoryStick, Unknown };

struct File {
    enum class Kind { Disc, Host, Directory, Failed } kind = Kind::Failed;
    std::string path;
    // disc
    u64 image_offset = 0;
    u64 size = 0;
    u32 start_lba = 0;
    u64 pos = 0;
    // host
    FILE* host = nullptr;
    // asynchronous state
    bool async_pending = false;
    bool async_done = false;
    s64 async_result = 0;
    std::vector<std::pair<Uid, u32>> async_waiters;

    ~File() {
        if (host) std::fclose(host);
    }
};

std::unique_ptr<File> g_files[kMaxFd];
std::string g_cwd = "disc0:/PSP_GAME/USRDIR";

File* file_at(u32 fd) { return fd >= u32(kFirstFd) && fd < u32(kMaxFd) ? g_files[fd].get() : nullptr; }

int alloc_fd(std::unique_ptr<File> file) {
    for (int fd = kFirstFd; fd < kMaxFd; ++fd) {
        if (!g_files[fd]) {
            g_files[fd] = std::move(file);
            return fd;
        }
    }
    return -1;
}

// Splits "device:/path" (or a path relative to the working directory) into device and path.
Device parse_path(const std::string& raw, std::string* rest) {
    std::string full = raw;
    if (full.find(':') == std::string::npos) full = g_cwd + (full.empty() || full[0] == '/' ? "" : "/") + full;
    size_t colon = full.find(':');
    std::string dev = full.substr(0, colon);
    std::transform(dev.begin(), dev.end(), dev.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    *rest = full.substr(colon + 1);
    while (!rest->empty() && ((*rest)[0] == '/' || (*rest)[0] == '\\')) rest->erase(0, 1);
    if (dev == "disc0" || dev == "umd0" || dev == "umd1" || dev == "umd") return Device::Disc;
    if (dev == "ms0" || dev == "fatms0" || dev == "ms") return Device::MemoryStick;
    return Device::Unknown;
}

// Maps a memory-stick path to the host folder, refusing anything that would escape it.
bool host_path(const std::string& rest, std::filesystem::path* out) {
    std::filesystem::path result = host::memory_stick_dir();
    size_t pos = 0;
    while (pos <= rest.size()) {
        size_t end = rest.find_first_of("/\\", pos);
        if (end == std::string::npos) end = rest.size();
        std::string part = rest.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find(':') != std::string::npos) return false;
        result /= part;
    }
    *out = result;
    return true;
}

// "sce_lbn0x1234_size0x5678": a raw extent of the disc.
bool parse_raw_extent(const std::string& rest, u32* lba, u32* size) {
    unsigned l = 0, s = 0;
    if (std::sscanf(rest.c_str(), "sce_lbn%x_size%x", &l, &s) != 2) return false;
    *lba = l;
    *size = s;
    return true;
}

u32 open_file(const std::string& raw, u32 flags, std::unique_ptr<File>* out) {
    std::string rest;
    Device device = parse_path(raw, &rest);
    auto file = std::make_unique<File>();
    file->path = raw;

    if (device == Device::Disc) {
        if (flags & kOpenWrite) return hle::kErrNoEnt;
        u32 lba, size;
        if (parse_raw_extent(rest, &lba, &size)) {
            file->image_offset = u64(lba) * disc::kSectorSize;
            file->size = size;
            file->start_lba = lba;
        } else if (rest.empty()) { // the whole disc
            file->size = host::image()->size();
        } else {
            const disc::IsoEntry* entry = host::iso().find(rest);
            if (!entry || entry->is_dir) return hle::kErrNoEnt;
            file->image_offset = u64(entry->lba) * disc::kSectorSize;
            file->size = entry->size;
            file->start_lba = entry->lba;
        }
        file->kind = File::Kind::Disc;
    } else if (device == Device::MemoryStick) {
        std::filesystem::path path;
        if (!host_path(rest, &path)) return hle::kErrNoEnt;
        std::error_code ec;
        bool exists = std::filesystem::is_regular_file(path, ec);
        if (!exists && !(flags & kOpenCreate)) return hle::kErrNoEnt;
        if (exists && (flags & kOpenCreate) && (flags & kOpenExclusive)) return hle::kErrExist;
        const wchar_t* mode = L"rb";
        if (flags & kOpenWrite) {
            if (!exists || (flags & kOpenTruncate)) mode = L"w+b";
            else mode = L"r+b";
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        file->host = _wfopen(path.c_str(), mode);
        if (!file->host) return hle::kErrNoEnt;
        if (flags & kOpenAppend) _fseeki64(file->host, 0, SEEK_END);
        file->kind = File::Kind::Host;
    } else {
        LOG_WARN("io", "open of unsupported device path '%s'", raw.c_str());
        return hle::kErrNoDev;
    }
    *out = std::move(file);
    return 0;
}

s64 read_file(File* f, u32 buffer, u32 size) {
    if (size == 0) return 0;
    if (!mem::valid(buffer, size)) return s64(s32(kErrIllegalAddr));
    if (f->kind == File::Kind::Disc) {
        if (f->pos >= f->size) return 0;
        u32 count = u32(std::min<u64>(size, f->size - f->pos));
        if (!host::image()->read(f->image_offset + f->pos, mem::ptr(buffer), count)) {
            LOG_ERROR("io", "disc read failed at %llu (+%u)", static_cast<unsigned long long>(f->image_offset + f->pos), count);
            return s64(s32(kErrError));
        }
        f->pos += count;
        return count;
    }
    if (f->kind == File::Kind::Host) return s64(std::fread(mem::ptr(buffer), 1, size, f->host));
    return s64(s32(hle::kErrBadF));
}

s64 seek_file(File* f, s64 offset, u32 whence) {
    if (f->kind == File::Kind::Disc) {
        s64 base = whence == 0 ? 0 : (whence == 1 ? s64(f->pos) : s64(f->size));
        s64 target = base + offset;
        if (target < 0) return s64(s32(hle::kErrInval));
        f->pos = u64(target);
        return target;
    }
    if (f->kind == File::Kind::Host) {
        if (_fseeki64(f->host, offset, whence == 0 ? SEEK_SET : (whence == 1 ? SEEK_CUR : SEEK_END)) != 0)
            return s64(s32(hle::kErrInval));
        return _ftelli64(f->host);
    }
    return s64(s32(hle::kErrBadF));
}

void async_complete(u64 fd) {
    File* f = file_at(u32(fd));
    if (!f || !f->async_pending) return;
    f->async_done = true;
    for (auto [uid, serial] : f->async_waiters) {
        Thread* t = get<Thread>(uid);
        if (t && t->wait_type == WaitType::Io && t->wait_id == Uid(fd) && t->wait_serial == serial) wake_thread(t, 0);
    }
    f->async_waiters.clear();
}

void start_async(u32 fd, s64 result) {
    File* f = file_at(fd);
    f->async_pending = true;
    f->async_done = false;
    f->async_result = result;
    schedule_event(kAsyncDelayUs, async_complete, fd);
}

void write_stat_time(u8* out) {
    // { u16 year, month, day, hour, minute, second; u32 microsecond }
    store_le<u16>(out + 0, 2012);
    store_le<u16>(out + 2, 1);
    store_le<u16>(out + 4, 1);
}

} // namespace

HLE(sceIoOpen) {
    std::string path = mem::read_string(ARG(0));
    std::unique_ptr<File> file;
    u32 error = open_file(path, ARG(1), &file);
    int fd = error ? -1 : alloc_fd(std::move(file));
    LOG_CH("io", "open '%s' flags %x -> %d", path.c_str(), ARG(1), error ? s32(error) : fd);
    if (error) RET(error);
    RET(fd < 0 ? hle::kErrMFile : u32(fd));
}

HLE(sceIoOpenAsync) {
    std::string path = mem::read_string(ARG(0));
    std::unique_ptr<File> file;
    u32 error = open_file(path, ARG(1), &file);
    if (error) {
        // The call still hands out a descriptor; the failure is reported as the asynchronous result.
        file = std::make_unique<File>();
        file->path = path;
    }
    int fd = alloc_fd(std::move(file));
    LOG_CH("io", "open async '%s' flags %x -> fd %d, result %d", path.c_str(), ARG(1), fd, error ? s32(error) : fd);
    if (fd < 0) RET(hle::kErrMFile);
    start_async(u32(fd), error ? s64(s32(error)) : s64(fd));
    RET(fd);
}

HLE(sceIoClose) {
    u32 fd = ARG(0);
    if (!file_at(fd)) RET(hle::kErrBadF);
    LOG_CH("io", "close %u", fd);
    g_files[fd].reset();
    RET(0);
}

HLE(sceIoRead) {
    File* f = file_at(ARG(0));
    if (!f) RET(hle::kErrBadF);
    s64 n = read_file(f, ARG(1), ARG(2));
    LOG_CH("io", "read fd %u size %x -> %lld", ARG(0), ARG(2), static_cast<long long>(n));
    RET(u32(n));
}

HLE(sceIoReadAsync) {
    File* f = file_at(ARG(0));
    if (!f) RET(hle::kErrBadF);
    if (f->async_pending) RET(hle::kErrAsyncBusy);
    s64 n = read_file(f, ARG(1), ARG(2));
    LOG_CH("io", "read async fd %u size %x -> %lld", ARG(0), ARG(2), static_cast<long long>(n));
    start_async(ARG(0), n);
    RET(0);
}

HLE(sceIoWrite) {
    u32 fd = ARG(0), buffer = ARG(1), size = ARG(2);
    if (size && !mem::valid(buffer, size)) RET(kErrIllegalAddr);
    if (fd == 1 || fd == 2) {
        std::string text(reinterpret_cast<const char*>(mem::ptr(buffer)), size);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (!text.empty()) LOG_INFO("guest", "%s", text.c_str());
        RET(size);
    }
    File* f = file_at(fd);
    if (!f || f->kind != File::Kind::Host) RET(hle::kErrBadF);
    RET(u32(std::fwrite(mem::ptr(buffer), 1, size, f->host)));
}

HLE(sceIoLseek) {
    File* f = file_at(ARG(0));
    if (!f) RET64(u64(s64(s32(hle::kErrBadF))));
    RET64(u64(seek_file(f, s64(ARG64(2)), ARG(4))));
}

HLE(sceIoLseekAsync) {
    File* f = file_at(ARG(0));
    if (!f) RET(hle::kErrBadF);
    if (f->async_pending) RET(hle::kErrAsyncBusy);
    start_async(ARG(0), seek_file(f, s64(ARG64(2)), ARG(4)));
    RET(0);
}

HLE(sceIoPollAsync) {
    File* f = file_at(ARG(0));
    if (!f) RET(hle::kErrBadF);
    if (!f->async_pending) RET(hle::kErrNoAsync);
    if (!f->async_done) RET(1);
    if (mem::valid(ARG(1), 8)) mem::w64(ARG(1), u64(f->async_result));
    f->async_pending = false;
    if (f->kind == File::Kind::Failed) g_files[ARG(0)].reset(); // a failed open leaves nothing behind
    RET(0);
}

HLE(sceIoWaitAsyncCB) {
    u32 fd = ARG(0), result_ptr = ARG(1);
    File* f = file_at(fd);
    if (!f) RET(hle::kErrBadF);
    if (!f->async_pending) RET(hle::kErrNoAsync);
    if (!f->async_done) {
        Thread* t = current_thread();
        if (!t) RET(kErrCanNotWait);
        f->async_waiters.push_back({t->uid, t->wait_serial + 1});
        wait_current(WaitType::Io, Uid(fd), 0, true);
        f = file_at(fd);
        if (!f) RET(hle::kErrBadF);
    }
    if (mem::valid(result_ptr, 8)) mem::w64(result_ptr, u64(f->async_result));
    f->async_pending = false;
    if (f->kind == File::Kind::Failed) g_files[fd].reset();
    RET(0);
}

HLE(sceIoGetstat) {
    std::string path = mem::read_string(ARG(0)), rest;
    u32 out = ARG(1);
    if (!mem::valid(out, 88)) RET(kErrIllegalAddr);
    Device device = parse_path(path, &rest);
    bool is_dir = false;
    u64 size = 0;
    u32 lba = 0;
    if (device == Device::Disc) {
        const disc::IsoEntry* entry = host::iso().find(rest);
        if (!entry) RET(hle::kErrNoEnt);
        is_dir = entry->is_dir;
        size = entry->size;
        lba = entry->lba;
    } else if (device == Device::MemoryStick) {
        std::filesystem::path host;
        std::error_code ec;
        if (!host_path(rest, &host) || !std::filesystem::exists(host, ec)) RET(hle::kErrNoEnt);
        is_dir = std::filesystem::is_directory(host, ec);
        if (!is_dir) size = std::filesystem::file_size(host, ec);
    } else {
        RET(hle::kErrNoDev);
    }
    u8 stat[88] = {};
    store_le<u32>(stat + 0, (is_dir ? 0x1000u : 0x2000u) | 0x16Du);
    store_le<u32>(stat + 4, is_dir ? 0x10u : 0x20u);
    store_le<u64>(stat + 8, size);
    write_stat_time(stat + 16);
    write_stat_time(stat + 32);
    write_stat_time(stat + 48);
    store_le<u32>(stat + 64, lba);
    std::memcpy(mem::ptr(out), stat, sizeof(stat));
    LOG_CH("io", "getstat '%s' -> %s, %llu bytes", path.c_str(), is_dir ? "directory" : "file",
           static_cast<unsigned long long>(size));
    RET(0);
}

HLE(sceIoDopen) {
    std::string path = mem::read_string(ARG(0)), rest;
    Device device = parse_path(path, &rest);
    bool found = false;
    if (device == Device::Disc) {
        const disc::IsoEntry* entry = host::iso().find(rest);
        found = entry && entry->is_dir;
    } else if (device == Device::MemoryStick) {
        std::filesystem::path host;
        std::error_code ec;
        found = host_path(rest, &host) && std::filesystem::is_directory(host, ec);
    }
    LOG_CH("io", "dopen '%s' -> %s", path.c_str(), found ? "ok" : "missing");
    if (!found) RET(hle::kErrNoEnt);
    auto dir = std::make_unique<File>();
    dir->kind = File::Kind::Directory;
    dir->path = path;
    int fd = alloc_fd(std::move(dir));
    RET(fd < 0 ? hle::kErrMFile : u32(fd));
}

HLE(sceIoDclose) {
    u32 fd = ARG(0);
    File* f = file_at(fd);
    if (!f || f->kind != File::Kind::Directory) RET(hle::kErrBadF);
    g_files[fd].reset();
    RET(0);
}

HLE(sceIoRename) {
    std::string from = mem::read_string(ARG(0)), to = mem::read_string(ARG(1)), rest_from, rest_to;
    if (parse_path(from, &rest_from) != Device::MemoryStick) RET(hle::kErrNoEnt);
    // The destination is usually given relative to the source's directory.
    std::filesystem::path host_from, host_to;
    if (!host_path(rest_from, &host_from)) RET(hle::kErrNoEnt);
    if (to.find(':') != std::string::npos) {
        if (parse_path(to, &rest_to) != Device::MemoryStick || !host_path(rest_to, &host_to)) RET(hle::kErrNoEnt);
    } else {
        std::string leaf = to.substr(to.find_last_of("/\\") == std::string::npos ? 0 : to.find_last_of("/\\") + 1);
        host_to = host_from.parent_path() / leaf;
    }
    std::error_code ec;
    std::filesystem::rename(host_from, host_to, ec);
    LOG_CH("io", "rename '%s' -> '%s': %s", from.c_str(), to.c_str(), ec ? "failed" : "ok");
    RET(ec ? hle::kErrNoEnt : 0);
}

HLE(sceIoDevctl) {
    std::string device = mem::read_string(ARG(0), 32);
    u32 cmd = ARG(1), in = ARG(2), in_len = ARG(3), out = ARG(4), out_len = ARG(5);
    LOG_CH("io", "devctl '%s' cmd %08x in %08x/%u out %08x/%u", device.c_str(), cmd, in, in_len, out, out_len);
    switch (cmd) {
    case 0x01F20001: // disc type
        if (mem::valid(out, 8)) {
            mem::w32(out, 0xFFFFFFFFu);
            mem::w32(out + 4, 0x10); // game disc
        }
        RET(0);
    case 0x01F20002: // current sector
    case 0x01F20003: // last sector
        if (mem::valid(out, 4)) mem::w32(out, cmd == 0x01F20003 ? u32(host::image()->size() / disc::kSectorSize) - 1 : 0);
        RET(0);
    case 0x01F100A3: // seek
    case 0x01F100A4: // prepare read
    case 0x01F300A5: // prepare cache
    case 0x01F010DB: // wake the drive
        RET(0);
    case 0x02025806: // memory stick inserted?
        if (mem::valid(out, 4)) mem::w32(out, 1);
        RET(0);
    case 0x02025801: // memory stick driver status
        if (mem::valid(out, 4)) mem::w32(out, 4);
        RET(0);
    case 0x02425823: { // free space
        u32 info = mem::valid(in, 4) ? mem::r32(in) : 0;
        if (mem::valid(info, 20)) {
            const u32 cluster_sectors = 0x40, sector_size = 0x200;
            const u32 clusters = u32((u64(1) << 30) / (cluster_sectors * sector_size)); // report 1 GiB free
            mem::w32(info + 0, clusters);  // total clusters
            mem::w32(info + 4, clusters);  // free clusters
            mem::w32(info + 8, clusters);
            mem::w32(info + 12, sector_size);
            mem::w32(info + 16, cluster_sectors);
        }
        RET(0);
    }
    case 0x02415821: // register / unregister memory stick callbacks
    case 0x02415822:
    case 0x02015804:
    case 0x02015805:
        RET(0);
    default:
        LOG_WARN("io", "devctl '%s': unknown command %08x", device.c_str(), cmd);
        RET(0);
    }
}

HLE(sceIoIoctl) {
    File* f = file_at(ARG(0));
    u32 cmd = ARG(1), in = ARG(2), in_len = ARG(3), out = ARG(4), out_len = ARG(5);
    LOG_CH("io", "ioctl fd %u cmd %08x in %08x/%u out %08x/%u", ARG(0), cmd, in, in_len, out, out_len);
    if (!f) RET(hle::kErrBadF);
    switch (cmd) {
    case 0x01020003: // sector size
        if (mem::valid(out, 4)) mem::w32(out, disc::kSectorSize);
        RET(0);
    case 0x01020004: // current position
        if (out_len >= 8 && mem::valid(out, 8)) mem::w64(out, f->pos);
        else if (mem::valid(out, 4)) mem::w32(out, u32(f->pos));
        RET(0);
    case 0x01010005: // seek
        if (in_len >= 16 && mem::valid(in, 16)) seek_file(f, s64(mem::r64(in)), mem::r32(in + 12));
        else if (mem::valid(in, 4)) seek_file(f, s64(mem::r32(in)), 0);
        RET(0);
    case 0x01020006: // first sector
        if (mem::valid(out, 4)) mem::w32(out, f->start_lba);
        RET(0);
    case 0x01020007: // length in bytes
        if (mem::valid(out, 8)) mem::w64(out, f->size);
        RET(0);
    default:
        LOG_WARN("io", "ioctl on '%s': unknown command %08x", f->path.c_str(), cmd);
        RET(0);
    }
}
