// sceAtrac3plus: streamed ATRAC3 / ATRAC3plus music.
//
// The game hands over a buffer holding the start of a RIFF/WAVE file and keeps topping it up as
// playback consumes it. Every byte the game supplies is copied to a host-side image of the file,
// so decoding and looping never depend on what currently sits in the game's buffer.

#include "runtime/hle/hle.h"
#include "runtime/media/codec.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace p2;
using namespace p2::kernel;

namespace {

constexpr u32 kMaxIds = 6;

constexpr u32 kErrNoAtracId = 0x80630003;
constexpr u32 kErrBadAtracId = 0x80630005;
constexpr u32 kErrUnknownFormat = 0x80630006;
constexpr u32 kErrSizeTooSmall = 0x80630011;
constexpr u32 kErrBadSample = 0x80630015;
constexpr u32 kErrAddDataTooBig = 0x80630018;
constexpr u32 kErrNoLoopInfo = 0x80630021;
constexpr u32 kErrSecondBufferNotNeeded = 0x80630022;
constexpr u32 kErrBufferIsEmpty = 0x80630023;
constexpr u32 kErrAllDataDecoded = 0x80630024;

constexpr s32 kRemainAllInMemory = -1; // the whole file was supplied; nothing more to add

struct Track {
    bool used = false;
    bool plus = false;
    u32 channels = 2, sample_rate = 44100, block_align = 0;
    u32 samples_per_frame = 2048;
    u32 data_offset = 0, data_size = 0, file_size = 0;
    s32 first_sample_offset = 0; // decoder delay to discard at the start
    s32 end_sample = 0;          // last valid sample index
    s32 loop_start = -1, loop_end = -1;
    s32 loop_count = 0; // -1: forever

    // What the game gave us.
    u32 buffer_addr = 0, buffer_size = 0;
    std::vector<u8> file;  // host image of the file
    u32 file_filled = 0;   // bytes of `file` that hold real data, counted from the start

    // Playback position.
    s32 position = 0;      // next sample to deliver
    u32 next_frame = 0;    // next frame index to feed the decoder
    s32 discard = 0;       // decoded samples still to drop (delay or loop pre-roll)
    std::vector<s16> pcm;  // decoded but not yet delivered, interleaved stereo
    bool finished = false;

    media::AudioDecoder decoder;

    u32 frame_count() const { return block_align ? data_size / block_align : 0; }
    u32 frames_available() const {
        if (file_filled <= data_offset) return 0;
        return std::min(frame_count(), (file_filled - data_offset) / block_align);
    }
    bool fully_loaded() const { return file_filled >= file_size; }
};

Track g_tracks[kMaxIds];

Track* track(u32 id) { return id < kMaxIds && g_tracks[id].used ? &g_tracks[id] : nullptr; }

// Parses the WAVE header at the start of the supplied data.
u32 parse_header(Track& t, const u8* d, u32 size) {
    if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return kErrUnknownFormat;
    t.file_size = load_le<u32>(d + 4) + 8;
    bool have_fmt = false, have_data = false;
    u8 extradata[32] = {};
    u32 extradata_size = 0;
    s32 fact_samples = -1, fact_extra = -1;

    u32 pos = 12;
    while (pos + 8 <= size) {
        const u32 chunk_size = load_le<u32>(d + pos + 4);
        const u8* body = d + pos + 8;
        const u32 body_avail = std::min(chunk_size, size - (pos + 8));
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && body_avail >= 16) {
            const u16 tag = load_le<u16>(body);
            t.channels = load_le<u16>(body + 2);
            t.sample_rate = load_le<u32>(body + 4);
            t.block_align = load_le<u16>(body + 12);
            if (tag == 0x0270) {
                t.plus = false;
                t.samples_per_frame = 1024;
                if (body_avail >= 18 + 14) {
                    extradata_size = 14;
                    std::memcpy(extradata, body + 18, 14);
                }
            } else if (tag == 0xFFFE) {
                t.plus = true;
                t.samples_per_frame = 2048;
            } else {
                return kErrUnknownFormat;
            }
            have_fmt = true;
        } else if (std::memcmp(d + pos, "fact", 4) == 0 && body_avail >= 4) {
            fact_samples = s32(load_le<u32>(body));
            if (body_avail >= 12) fact_extra = s32(load_le<u32>(body + 8));
            else if (body_avail >= 8) fact_extra = s32(load_le<u32>(body + 4));
        } else if (std::memcmp(d + pos, "smpl", 4) == 0 && body_avail >= 60) {
            if (load_le<u32>(body + 28) > 0) {
                t.loop_start = s32(load_le<u32>(body + 44));
                t.loop_end = s32(load_le<u32>(body + 48));
            }
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            t.data_offset = pos + 8;
            t.data_size = chunk_size;
            have_data = true;
            break;
        }
        pos += 8 + chunk_size + (chunk_size & 1);
    }
    if (!have_fmt || !have_data || t.block_align == 0 || t.channels == 0 || t.channels > 2) return kErrUnknownFormat;
    t.file_size = std::max(t.file_size, t.data_offset + t.data_size);

    // The codecs delay their output; the header's extra field says how much the encoder added on top.
    const s32 codec_delay = t.plus ? 368 : 69;
    t.first_sample_offset = (fact_extra >= 0 ? fact_extra : 0) + codec_delay;
    const s32 total = fact_samples >= 0 ? fact_samples : s32(t.frame_count() * t.samples_per_frame) - t.first_sample_offset;
    t.end_sample = total - 1;
    if (t.loop_start >= 0) {
        // Loop points in the file count from the start of the stream, delay included.
        t.loop_start -= t.first_sample_offset;
        t.loop_end -= t.first_sample_offset;
        if (t.loop_start < 0 || t.loop_end <= t.loop_start) t.loop_start = t.loop_end = -1;
    }
    if (!t.decoder.open(t.plus, t.channels, t.sample_rate, t.block_align, extradata, extradata_size)) return kErrUnknownFormat;
    return 0;
}

// Positions the decoder so the next delivered sample is `sample`.
void seek_to(Track& t, s32 sample) {
    const s32 absolute = sample + t.first_sample_offset;
    s32 frame = absolute / s32(t.samples_per_frame);
    const s32 preroll = std::min(frame, 2); // overlap frames need their predecessors
    frame -= preroll;
    t.next_frame = u32(frame);
    t.discard = absolute - frame * s32(t.samples_per_frame);
    t.position = sample;
    t.pcm.clear();
    t.finished = false;
    t.decoder.reset();
}

// Decodes until at least one sample is ready. Returns false when the needed data has not arrived.
bool fill_pcm(Track& t) {
    while (t.pcm.empty()) {
        std::vector<s16> decoded;
        if (t.next_frame >= t.frame_count()) {
            // Past the last frame of data. The header's sample count can end a little beyond it (the
            // start offset already takes up part of the first frames), so what is left up to the end
            // or the loop point is silence. Without it such a track never finishes, and the game's
            // player thread retries without end and without ever letting another thread run.
            decoded.assign(size_t(t.samples_per_frame) * 2, 0);
            ++t.next_frame;
        } else {
            if (t.next_frame >= t.frames_available()) return false;
            const u8* frame = t.file.data() + t.data_offset + size_t(t.next_frame) * t.block_align;
            int n = t.decoder.decode(frame, t.block_align, decoded);
            ++t.next_frame;
            if (n <= 0) decoded.assign(size_t(t.samples_per_frame) * 2, 0); // keep time moving over a bad frame
        }
        s32 frames = s32(decoded.size() / 2);
        s32 drop = std::min(t.discard, frames);
        t.discard -= drop;
        t.pcm.insert(t.pcm.end(), decoded.begin() + ptrdiff_t(drop) * 2, decoded.end());
    }
    return true;
}

s32 remaining_frames(const Track& t) {
    // Once the host image is complete nothing more is ever needed, loops included. Reporting the
    // track as wholly in memory stops the game from issuing empty top-up reads, which its own
    // stop logic would then wait on forever.
    if (t.fully_loaded()) return kRemainAllInMemory;
    s32 ahead = s32(t.frames_available()) - s32(t.next_frame);
    return std::max(ahead, 0);
}

} // namespace

HLE(sceAtracSetDataAndGetID) {
    u32 buffer = ARG(0), size = ARG(1);
    if (!mem::valid(buffer, size) || size < 64) RET(kErrSizeTooSmall);
    u32 id = kMaxIds;
    for (u32 i = 0; i < kMaxIds; ++i) {
        if (!g_tracks[i].used) {
            id = i;
            break;
        }
    }
    if (id == kMaxIds) RET(kErrNoAtracId);

    Track& t = g_tracks[id];
    t = Track{};
    u32 error = parse_header(t, mem::ptr(buffer), size);
    if (error) RET(error);
    t.used = true;
    t.buffer_addr = buffer;
    t.buffer_size = size;
    t.file.assign(t.file_size, 0);
    t.file_filled = std::min(size, t.file_size);
    std::memcpy(t.file.data(), mem::ptr(buffer), t.file_filled);
    seek_to(t, 0);
    LOG_CH("audio", "atrac %u: %s, %u ch, %u Hz, %u-byte frames, %d samples, loop %d-%d, file %u bytes (%u supplied)", id,
           t.plus ? "ATRAC3plus" : "ATRAC3", t.channels, t.sample_rate, t.block_align, t.end_sample + 1, t.loop_start, t.loop_end,
           t.file_size, size);
    LOG_CH("audio", "atrac %u: data at %u, %u bytes (%u frames, %u samples), first sample offset %d", id, t.data_offset,
           t.data_size, t.frame_count(), t.frame_count() * t.samples_per_frame, t.first_sample_offset);
    RET(id);
}

HLE(sceAtracReleaseAtracID) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    *t = Track{};
    RET(0);
}

// (id, out samples, &sample count, &finished, &remaining frames)
HLE(sceAtracDecodeData) {
    Track* t = track(ARG(0));
    u32 out = ARG(1), count_ptr = ARG(2), finish_ptr = ARG(3), remain_ptr = ARG(4);
    if (!t) RET(kErrBadAtracId);
    if (t->finished) {
        if (mem::valid(count_ptr, 4)) mem::w32(count_ptr, 0);
        if (mem::valid(finish_ptr, 4)) mem::w32(finish_ptr, 1);
        if (mem::valid(remain_ptr, 4)) mem::w32(remain_ptr, u32(remaining_frames(*t)));
        RET(kErrAllDataDecoded);
    }
    if (!fill_pcm(*t)) {
        // Like the firmware, still report the state: the remaining frames (none) tell the game to
        // supply more data, instead of it acting on the count from an earlier call.
        if (mem::valid(count_ptr, 4)) mem::w32(count_ptr, 0);
        if (mem::valid(finish_ptr, 4)) mem::w32(finish_ptr, 0);
        if (mem::valid(remain_ptr, 4)) mem::w32(remain_ptr, u32(remaining_frames(*t)));
        RET(kErrBufferIsEmpty);
    }

    // One call delivers at most one frame's worth, cut at the loop end or the end of the track.
    const bool looping = t->loop_start >= 0 && t->loop_count != 0;
    const s32 stop_at = looping ? t->loop_end : t->end_sample;
    s32 want = s32(t->samples_per_frame);
    // The first call is short so that later calls line up with frame boundaries.
    const s32 into_frame = (t->position + t->first_sample_offset) % s32(t->samples_per_frame);
    want -= into_frame;
    want = std::min(want, stop_at - t->position + 1);
    want = std::min(want, s32(t->pcm.size() / 2));
    want = std::max(want, 0);

    if (want > 0 && mem::valid(out, u32(want) * 4)) std::memcpy(mem::ptr(out), t->pcm.data(), size_t(want) * 4);
    t->pcm.erase(t->pcm.begin(), t->pcm.begin() + ptrdiff_t(want) * 2);
    t->position += want;

    u32 finished = 0;
    if (t->position > stop_at) {
        if (looping) {
            if (t->loop_count > 0) --t->loop_count;
            seek_to(*t, t->loop_start);
        } else {
            t->finished = true;
            finished = 1;
        }
    }
    if (mem::valid(count_ptr, 4)) mem::w32(count_ptr, u32(want));
    if (mem::valid(finish_ptr, 4)) mem::w32(finish_ptr, finished);
    if (mem::valid(remain_ptr, 4)) mem::w32(remain_ptr, u32(remaining_frames(*t)));
    RET(0);
}

HLE(sceAtracGetRemainFrame) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), u32(remaining_frames(*t)));
    RET(0);
}

// (id, &write pointer, &writable bytes, &file offset to read from)
HLE(sceAtracGetStreamDataInfo) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    u32 writable = t->fully_loaded() ? 0 : std::min(t->buffer_size, t->file_size - t->file_filled);
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), t->buffer_addr);
    if (mem::valid(ARG(2), 4)) mem::w32(ARG(2), writable);
    if (mem::valid(ARG(3), 4)) mem::w32(ARG(3), t->file_filled);
    RET(0);
}

HLE(sceAtracAddStreamData) {
    Track* t = track(ARG(0));
    u32 bytes = ARG(1);
    if (!t) RET(kErrBadAtracId);
    if (bytes > t->buffer_size || bytes > t->file_size - t->file_filled) RET(kErrAddDataTooBig);
    if (bytes && mem::valid(t->buffer_addr, bytes)) {
        std::memcpy(t->file.data() + t->file_filled, mem::ptr(t->buffer_addr), bytes);
        t->file_filled += bytes;
    }
    RET(0);
}

HLE(sceAtracGetSecondBufferInfo) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), 0);
    if (mem::valid(ARG(2), 4)) mem::w32(ARG(2), 0);
    RET(kErrSecondBufferNotNeeded);
}

HLE(sceAtracSetSecondBuffer) { RET(track(ARG(0)) ? 0 : kErrBadAtracId); }

HLE(sceAtracGetNextDecodePosition) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    if (t->finished) RET(kErrAllDataDecoded);
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), u32(t->position));
    RET(0);
}

HLE(sceAtracGetSoundSample) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), u32(t->end_sample));
    if (mem::valid(ARG(2), 4)) mem::w32(ARG(2), u32(t->loop_start));
    if (mem::valid(ARG(3), 4)) mem::w32(ARG(3), u32(t->loop_end));
    RET(0);
}

HLE(sceAtracSetLoopNum) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    if (t->loop_start < 0) RET(kErrNoLoopInfo);
    t->loop_count = s32(ARG(1));
    RET(0);
}

// (id, sample, &info): where to put data so playback can restart at `sample`.
// info: two blocks of { write pointer, writable bytes, minimum bytes, file offset }.
HLE(sceAtracGetBufferInfoForResetting) {
    Track* t = track(ARG(0));
    s32 sample = s32(ARG(1));
    u32 info = ARG(2);
    if (!t) RET(kErrBadAtracId);
    if (sample < 0 || sample > t->end_sample) RET(kErrBadSample);
    if (!mem::valid(info, 32)) RET(kErrIllegalAddr);
    std::memset(mem::ptr(info), 0, 32);
    const u32 frame = u32(sample + t->first_sample_offset) / t->samples_per_frame;
    const u32 start = t->data_offset + (frame > 2 ? frame - 2 : 0) * t->block_align;
    mem::w32(info + 0, t->buffer_addr);
    if (t->fully_loaded()) { // everything is already here: nothing to supply
        mem::w32(info + 12, start);
    } else {
        u32 writable = std::min(t->buffer_size, t->file_size - start);
        mem::w32(info + 4, writable);
        mem::w32(info + 8, std::min(writable, t->block_align * 4));
        mem::w32(info + 12, start);
    }
    mem::w32(info + 16, t->buffer_addr);
    RET(0);
}

// (id, sample, bytes written to the first buffer, bytes written to the second)
HLE(sceAtracResetPlayPosition) {
    Track* t = track(ARG(0));
    s32 sample = s32(ARG(1));
    u32 written = ARG(2);
    if (!t) RET(kErrBadAtracId);
    if (sample < 0 || sample > t->end_sample) RET(kErrBadSample);
    if (!t->fully_loaded() && written) {
        const u32 frame = u32(sample + t->first_sample_offset) / t->samples_per_frame;
        const u32 start = t->data_offset + (frame > 2 ? frame - 2 : 0) * t->block_align;
        written = std::min(written, std::min(t->buffer_size, t->file_size - start));
        if (mem::valid(t->buffer_addr, written)) {
            std::memcpy(t->file.data() + start, mem::ptr(t->buffer_addr), written);
            // The host image is tracked as one contiguous run from the start; only extend it when
            // the new data joins that run.
            if (start <= t->file_filled) t->file_filled = std::max(t->file_filled, start + written);
        }
    }
    seek_to(*t, sample);
    RET(0);
}

HLE(sceAtracGetInternalErrorInfo) {
    Track* t = track(ARG(0));
    if (!t) RET(kErrBadAtracId);
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), 0);
    RET(0);
}
