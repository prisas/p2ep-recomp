#pragma once

#include "lib/common/types.h"

#include <filesystem>
#include <memory>
#include <string>

namespace p2::disc {

constexpr u32 kSectorSize = 2048;

// Random-access view of a disc image. Plain ISO files and CHD-compressed images are supported.
class Image {
public:
    virtual ~Image() = default;

    virtual u64 size() const = 0;
    virtual const char* format_name() const = 0;

    // Thread-safe. Returns false when the range is out of bounds or the image is damaged.
    virtual bool read(u64 offset, void* dst, size_t size) = 0;

    bool read_sectors(u32 lba, u32 count, void* dst) {
        return read(u64(lba) * kSectorSize, dst, size_t(count) * kSectorSize);
    }

    static std::unique_ptr<Image> open(const std::filesystem::path& path, std::string* error);
};

} // namespace p2::disc
