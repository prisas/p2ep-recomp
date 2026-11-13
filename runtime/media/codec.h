#pragma once

#include "lib/common/types.h"

#include <memory>
#include <vector>

// Decoders for the two compressed formats the console's media libraries handle: ATRAC3 /
// ATRAC3plus audio and H.264 video. Both are thin wrappers over libavcodec.
namespace p2::media {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;

    // `plus` selects ATRAC3plus. `extradata` is the codec-specific tail of the WAVE format chunk
    // (required for plain ATRAC3, ignored for ATRAC3plus).
    bool open(bool plus, u32 channels, u32 sample_rate, u32 block_align, const u8* extradata, u32 extradata_size);
    bool is_open() const;

    // Decodes one frame of `size` bytes and appends interleaved stereo samples to `out`.
    // Returns the number of sample frames produced, or -1 on error.
    int decode(const u8* data, u32 size, std::vector<s16>& out);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool open();
    // Appends elementary-stream bytes.
    void push(const u8* data, size_t size);
    // Bytes pushed but not yet consumed by the decoder.
    size_t pending_bytes() const;
    // Tells the decoder no more data will come, so it releases its delayed frames.
    void end_of_stream();
    // Advances to the next picture in display order. False when more data is needed.
    bool next_frame();
    bool has_frame() const;
    u32 width() const;
    u32 height() const;
    // Converts the current picture to RGBA8 into `dst` (rows of `stride` bytes), cropped to w x h.
    void convert_rgba(u8* dst, u32 stride, u32 w, u32 h);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2::media
