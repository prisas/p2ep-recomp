#include "lib/disc/image.h"

#include "lib/common/file.h"

#include <libchdr/chd.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

#include <windows.h>

namespace p2::disc {

namespace {

// --- plain ISO: memory-mapped -------------------------------------------------------------------------------

class RawImage final : public Image {
public:
    ~RawImage() override {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    bool open(const std::filesystem::path& path, std::string* error) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            if (error) *error = "cannot open image file";
            return false;
        }
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file_, &size) || size.QuadPart == 0) {
            if (error) *error = "image file is empty";
            return false;
        }
        size_ = u64(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_) view_ = static_cast<const u8*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!view_) {
            if (error) *error = "cannot map image file";
            return false;
        }
        return true;
    }

    u64 size() const override { return size_; }
    const char* format_name() const override { return "ISO"; }

    bool read(u64 offset, void* dst, size_t size) override {
        if (offset > size_ || size > size_ - offset) return false;
        std::memcpy(dst, view_ + offset, size);
        return true;
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const u8* view_ = nullptr;
    u64 size_ = 0;
};

// --- CHD: hunk-compressed, decoded on demand through a direct-mapped cache -----------------------------------

class ChdImage final : public Image {
public:
    ~ChdImage() override {
        if (chd_) chd_close(chd_); // also closes the FILE it was opened from
        else if (file_) std::fclose(file_);
    }

    bool open(const std::filesystem::path& path, std::string* error) {
        file_ = _wfopen(path.c_str(), L"rb");
        if (!file_) {
            if (error) *error = "cannot open image file";
            return false;
        }
        chd_error err = chd_open_file(file_, CHD_OPEN_READ, nullptr, &chd_);
        if (err != CHDERR_NONE) {
            if (error) *error = format("cannot read compressed image: %s", chd_error_string(err));
            chd_ = nullptr;
            return false;
        }
        const chd_header* header = chd_get_header(chd_);
        hunk_bytes_ = header->hunkbytes;
        size_ = header->logicalbytes;
        if (hunk_bytes_ == 0 || size_ == 0) {
            if (error) *error = "compressed image has an invalid header";
            return false;
        }
        hunk_count_ = u32((size_ + hunk_bytes_ - 1) / hunk_bytes_);

        // About 32 MiB of decoded data; always a power of two so the slot index is a mask.
        size_t slots = 64;
        while (slots * 2 * hunk_bytes_ <= (32u << 20)) slots *= 2;
        slot_mask_ = u32(slots - 1);
        slot_hunk_.assign(slots, ~0u);
        slot_data_.resize(slots * size_t(hunk_bytes_));
        return true;
    }

    u64 size() const override { return size_; }
    const char* format_name() const override { return "CHD"; }

    bool read(u64 offset, void* dst, size_t size) override {
        if (offset > size_ || size > size_ - offset) return false;
        u8* out = static_cast<u8*>(dst);
        std::lock_guard lock(mutex_);
        while (size > 0) {
            u32 hunk = u32(offset / hunk_bytes_);
            u32 within = u32(offset % hunk_bytes_);
            size_t chunk = std::min<size_t>(size, hunk_bytes_ - within);
            const u8* data = fetch(hunk);
            if (!data) return false;
            std::memcpy(out, data + within, chunk);
            out += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }

private:
    const u8* fetch(u32 hunk) {
        u32 slot = hunk & slot_mask_;
        u8* data = slot_data_.data() + size_t(slot) * hunk_bytes_;
        if (slot_hunk_[slot] == hunk) return data;
        slot_hunk_[slot] = ~0u;
        if (hunk >= hunk_count_ || chd_read(chd_, hunk, data) != CHDERR_NONE) return nullptr;
        slot_hunk_[slot] = hunk;
        return data;
    }

    std::mutex mutex_;
    FILE* file_ = nullptr;
    chd_file* chd_ = nullptr;
    u64 size_ = 0;
    u32 hunk_bytes_ = 0;
    u32 hunk_count_ = 0;
    u32 slot_mask_ = 0;
    std::vector<u32> slot_hunk_;
    std::vector<u8> slot_data_;
};

} // namespace

std::unique_ptr<Image> Image::open(const std::filesystem::path& path, std::string* error) {
    char magic[8] = {};
    {
        FILE* probe = _wfopen(path.c_str(), L"rb");
        if (!probe) {
            if (error) *error = "cannot open image file";
            return nullptr;
        }
        size_t got = std::fread(magic, 1, sizeof(magic), probe);
        std::fclose(probe);
        if (got != sizeof(magic)) {
            if (error) *error = "image file is too small";
            return nullptr;
        }
    }

    if (std::memcmp(magic, "MComprHD", 8) == 0) {
        auto image = std::make_unique<ChdImage>();
        if (!image->open(path, error)) return nullptr;
        return image;
    }
    if (std::memcmp(magic, "CISO", 4) == 0) {
        if (error) *error = "CSO images are not supported; convert the image to ISO or CHD";
        return nullptr;
    }

    auto image = std::make_unique<RawImage>();
    if (!image->open(path, error)) return nullptr;
    return image;
}

} // namespace p2::disc
