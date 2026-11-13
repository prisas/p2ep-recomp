// sceMpeg: movie playback.
//
// The game reads the movie file itself and passes 2048-byte packs of the program stream through a
// "ring buffer"; it then asks for one video picture and one audio frame at a time. Here every
// pack is demultiplexed on arrival into a host-side H.264 decoder and an ATRAC3plus frame queue,
// so the ring buffer in guest memory is only a hand-over area.

#include "runtime/hle/hle.h"
#include "runtime/media/codec.h"

#include <algorithm>
#include <vector>

using namespace p2;
using namespace p2::kernel;

namespace {

constexpr u32 kPackSize = 2048;
constexpr u32 kErrNoData = 0x80618001;
constexpr u32 kErrInvalidValue = 0x806101FE;
constexpr u32 kErrMpegNoMemory = 0x80610022;
constexpr u32 kHandleSize = 0x10000;
constexpr u32 kRingOverhead = 104;        // bookkeeping bytes per pack in the ring buffer
constexpr s64 kVideoFrameTicks = 3003;    // 29.97 pictures per second on a 90 kHz clock
constexpr u32 kAudioFrameSamples = 2048;

// Ring-buffer structure offsets.
constexpr u32 kRbPackets = 0, kRbRead = 4, kRbWritten = 8, kRbAvail = 12, kRbPackSize = 16, kRbData = 20, kRbCallback = 24,
              kRbCallbackArg = 28, kRbDataEnd = 32, kRbSema = 36, kRbMpeg = 40;

struct Movie {
    bool created = false;
    u32 ring = 0;

    media::VideoDecoder video;
    media::AudioDecoder audio;
    bool video_open = false;

    std::vector<u8> audio_es; // demultiplexed audio stream
    size_t audio_read = 0;

    s64 first_pts = -1;
    u64 video_frames = 0, audio_frames = 0;
    bool picture_ready = false; // a decoded picture is waiting to be handed out
    u64 bytes_received = 0;
    u32 stream_size = 0;        // from the file header; 0 when unknown

    void reset_streams() {
        if (video_open) video.reset();
        audio.reset();
        audio_es.clear();
        audio_read = 0;
        first_pts = -1;
        video_frames = audio_frames = 0;
        picture_ready = false;
        bytes_received = 0;
    }
};

Movie g_movie;

s64 read_pts(const u8* p) {
    return (s64((p[0] >> 1) & 7) << 30) | (s64(p[1]) << 22) | (s64(p[2] >> 1) << 15) | (s64(p[3]) << 7) | s64(p[4] >> 1);
}

// Splits one program-stream pack into its video and audio payloads.
void demux_pack(Movie& m, const u8* p) {
    u32 pos = 0;
    while (pos + 6 <= kPackSize) {
        if (p[pos] != 0 || p[pos + 1] != 0 || p[pos + 2] != 1) {
            ++pos;
            continue;
        }
        const u8 id = p[pos + 3];
        if (id == 0xBA) { // pack header
            if (pos + 14 > kPackSize) break;
            pos += 14 + (p[pos + 13] & 7);
            continue;
        }
        const u32 length = load_be16(p + pos + 4);
        const u32 end = std::min(pos + 6 + length, kPackSize);
        const bool is_video = id >= 0xE0 && id <= 0xEF, is_private = id == 0xBD;
        if ((is_video || is_private) && pos + 9 <= end) {
            const u32 flags = p[pos + 7] >> 6;
            u32 payload = pos + 9 + p[pos + 8];
            if ((flags & 2) && pos + 14 <= end && m.first_pts < 0 && is_video) m.first_pts = read_pts(p + pos + 9);
            if (payload < end) {
                if (is_video) {
                    m.video.push(p + payload, end - payload);
                } else if ((p[payload] & 0xF0) == 0x00 && payload + 4 <= end) {
                    // Private stream 1, sub-stream 0x0n: ATRAC3plus behind a four-byte header.
                    m.audio_es.insert(m.audio_es.end(), p + payload + 4, p + end);
                }
            }
        }
        pos = end;
    }
}

// Finds the next complete audio frame. Each starts with an 8-byte header: 0F D0, then the frame
// length in 8-byte units minus one in the low ten bits of the following 16.
bool next_audio_frame(Movie& m, const u8** data, u32* size, u32* channels) {
    while (m.audio_read + 8 <= m.audio_es.size()) {
        const u8* h = m.audio_es.data() + m.audio_read;
        if (h[0] != 0x0F || h[1] != 0xD0) {
            ++m.audio_read;
            continue;
        }
        const u32 frame_bytes = ((((h[2] & 3u) << 8) | h[3]) + 1) * 8;
        if (m.audio_read + 8 + frame_bytes > m.audio_es.size()) return false;
        *data = h + 8;
        *size = frame_bytes;
        *channels = ((h[2] >> 2) & 7) == 1 ? 1 : 2;
        return true;
    }
    return false;
}

void consume_audio_frame(Movie& m, u32 size) {
    m.audio_read += 8 + size;
    if (m.audio_read > (1u << 18)) {
        m.audio_es.erase(m.audio_es.begin(), m.audio_es.begin() + ptrdiff_t(m.audio_read));
        m.audio_read = 0;
    }
}

bool ensure_picture(Movie& m) {
    if (!m.picture_ready) m.picture_ready = m.video.next_frame();
    return m.picture_ready;
}

void write_au(u32 au, s64 pts) {
    if (!mem::valid(au, 24)) return;
    mem::w32(au + 0, u32(u64(pts) >> 32));
    mem::w32(au + 4, u32(pts));
    mem::w32(au + 8, u32(u64(pts) >> 32));
    mem::w32(au + 12, u32(pts));
}

u32 pending_packs(const Movie& m) {
    return u32((m.video.pending_bytes() + (m.audio_es.size() - m.audio_read) + kPackSize - 1) / kPackSize);
}

} // namespace

HLE(sceMpegInit) { RET(0); }
HLE(sceMpegFinish) { RET(0); }
HLE(sceMpegQueryMemSize) { RET(kHandleSize); }

HLE(sceMpegRingbufferQueryMemSize) { RET(ARG(0) * (kPackSize + kRingOverhead)); }

// (ring, packs, data, size, callback, callback argument)
HLE(sceMpegRingbufferConstruct) {
    u32 ring = ARG(0), packs = ARG(1), data = ARG(2), size = ARG(3);
    if (!mem::valid(ring, 48)) RET(kErrIllegalAddr);
    if (size < packs * (kPackSize + kRingOverhead)) RET(kErrMpegNoMemory);
    std::memset(mem::ptr(ring), 0, 48);
    mem::w32(ring + kRbPackets, packs);
    mem::w32(ring + kRbPackSize, kPackSize);
    mem::w32(ring + kRbData, data);
    mem::w32(ring + kRbCallback, ARG(4));
    mem::w32(ring + kRbCallbackArg, ARG(5));
    mem::w32(ring + kRbDataEnd, data + packs * kPackSize);
    RET(0);
}

HLE(sceMpegRingbufferDestruct) { RET(0); }

// (handle pointer, work area, size, ring, frame width, mode, top)
HLE(sceMpegCreate) {
    u32 handle_ptr = ARG(0), data = ARG(1), size = ARG(2), ring = ARG(3);
    if (!mem::valid(handle_ptr, 4) || !mem::valid(data, 32)) RET(kErrIllegalAddr);
    if (size < kHandleSize) RET(kErrMpegNoMemory);
    mem::w32(handle_ptr, data);
    std::memset(mem::ptr(data), 0, 32);
    std::memcpy(mem::ptr(data), "LIBMPEG", 8);
    std::memcpy(mem::ptr(data + 8), "001", 4);
    mem::w32(data + 12, 0xFFFFFFFFu);
    if (ring && mem::valid(ring, 48)) {
        mem::w32(data + 16, ring);
        mem::w32(data + 20, mem::r32(ring + kRbDataEnd));
        mem::w32(ring + kRbMpeg, handle_ptr);
    }
    g_movie.created = true;
    g_movie.ring = ring;
    if (!g_movie.video_open) g_movie.video_open = g_movie.video.open();
    g_movie.reset_streams();
    LOG_CH("media", "movie player created (ring buffer %08x)", ring);
    RET(0);
}

HLE(sceMpegDelete) {
    g_movie.created = false;
    g_movie.reset_streams();
    RET(0);
}

// (handle, header buffer, &offset): where the stream data starts in the file.
HLE(sceMpegQueryStreamOffset) {
    u32 buffer = ARG(1), out = ARG(2);
    if (!mem::valid(buffer, 16) || !mem::valid(out, 4)) RET(kErrIllegalAddr);
    const u8* h = mem::ptr(buffer);
    if (std::memcmp(h, "PSMF", 4) != 0) {
        mem::w32(out, 0);
        RET(kErrInvalidValue);
    }
    u32 offset = load_be32(h + 8);
    if (offset == 0 || (offset & 0x7FF)) {
        mem::w32(out, 0);
        RET(kErrInvalidValue);
    }
    g_movie.stream_size = load_be32(h + 12);
    mem::w32(out, offset);
    RET(0);
}

// (header buffer, &size)
HLE(sceMpegQueryStreamSize) {
    u32 buffer = ARG(0), out = ARG(1);
    if (!mem::valid(buffer, 16) || !mem::valid(out, 4)) RET(kErrIllegalAddr);
    u32 size = load_be32(mem::ptr(buffer) + 12);
    if (size & 0x7FF) {
        mem::w32(out, 0);
        RET(kErrInvalidValue);
    }
    g_movie.stream_size = size;
    mem::w32(out, size);
    RET(0);
}

HLE(sceMpegRegistStream) { RET(0x10 + ARG(1) * 16 + (ARG(2) & 15)); }
HLE(sceMpegUnRegistStream) { RET(0); }
HLE(sceMpegMallocAvcEsBuf) { RET(1); }
HLE(sceMpegFreeAvcEsBuf) { RET(0); }

HLE(sceMpegQueryAtracEsSize) {
    if (mem::valid(ARG(1), 4)) mem::w32(ARG(1), 2112);
    if (mem::valid(ARG(2), 4)) mem::w32(ARG(2), kAudioFrameSamples * 4);
    RET(0);
}

// (handle, elementary-stream buffer, access unit)
HLE(sceMpegInitAu) {
    u32 es = ARG(1), au = ARG(2);
    if (!mem::valid(au, 24)) RET(kErrIllegalAddr);
    write_au(au, 0);
    mem::w32(au + 16, es);
    mem::w32(au + 20, es == 1 ? kPackSize : 2112);
    RET(0);
}

HLE(sceMpegRingbufferAvailableSize) {
    u32 ring = ARG(0);
    if (!mem::valid(ring, 48)) RET(kErrIllegalAddr);
    u32 packs = mem::r32(ring + kRbPackets), pending = pending_packs(g_movie);
    RET(pending >= packs ? 0 : packs - pending);
}

// (ring, packs to read, packs of room): asks the game's callback to read packs into the buffer.
HLE(sceMpegRingbufferPut) {
    u32 ring = ARG(0);
    s32 wanted = s32(ARG(1)), room = s32(ARG(2));
    if (!mem::valid(ring, 48)) RET(kErrIllegalAddr);
    Thread* t = current_thread();
    if (!t) RET(kErrIllegalContext);
    const u32 packs = mem::r32(ring + kRbPackets), data = mem::r32(ring + kRbData);
    s32 count = std::min({wanted, room, s32(packs)});
    if (count <= 0) RET(0);

    u32 read = call_guest_on_thread(t, mem::r32(ring + kRbCallback), data, u32(count), mem::r32(ring + kRbCallbackArg));
    if (s32(read) <= 0) RET(read);
    read = std::min<u32>(read, u32(count));
    if (!mem::valid(data, read * kPackSize)) RET(kErrIllegalAddr);
    Movie& mv = g_movie;
    for (u32 i = 0; i < read; ++i) demux_pack(mv, mem::ptr(data + i * kPackSize));
    mv.bytes_received += u64(read) * kPackSize;
    if (mv.stream_size && mv.bytes_received >= mv.stream_size) mv.video.end_of_stream();
    mem::w32(ring + kRbWritten, mem::r32(ring + kRbWritten) + read);
    mem::w32(ring + kRbRead, mem::r32(ring + kRbRead) + read);
    LOG_CH("media", "ring buffer put: %u pack(s), %u pending", read, pending_packs(mv));
    RET(read);
}

// (handle, stream, access unit, &attribute)
HLE(sceMpegGetAvcAu) {
    Movie& mv = g_movie;
    if (!ensure_picture(mv)) RET(kErrNoData);
    const s64 pts = (mv.first_pts < 0 ? 0 : mv.first_pts) + s64(mv.video_frames) * kVideoFrameTicks;
    write_au(ARG(2), pts);
    if (mem::valid(ARG(3), 4)) mem::w32(ARG(3), 1);
    RET(0);
}

// (handle, access unit, &picture buffer pointer, &status)
HLE(sceMpegAvcDecodeYCbCr) {
    Movie& mv = g_movie;
    const bool ok = ensure_picture(mv);
    if (ok) {
        mv.picture_ready = false; // handed out; the decoder keeps it as the current picture
        ++mv.video_frames;
    }
    if (mem::valid(ARG(3), 4)) mem::w32(ARG(3), ok ? 1 : 0);
    RET(0);
}

HLE(sceMpegAvcDecodeStopYCbCr) {
    if (mem::valid(ARG(2), 4)) mem::w32(ARG(2), 0);
    RET(0);
}

HLE(sceMpegAvcDecodeFlush) { RET(0); }

HLE(sceMpegAvcQueryYCbCrSize) {
    u32 width = ARG(2), height = ARG(3);
    if (!mem::valid(ARG(4), 4)) RET(kErrIllegalAddr);
    if ((width & 15) || (height & 15) || width > 720 || height > 576) RET(kErrInvalidValue);
    mem::w32(ARG(4), (width / 2) * (height / 2) * 6 + 128);
    RET(0);
}

HLE(sceMpegAvcInitYCbCr) { RET(0); }

// (handle, picture buffer, &range {x, y, width, height}, frame width, destination)
HLE(sceMpegAvcCsc) {
    Movie& mv = g_movie;
    u32 range = ARG(2), frame_width = ARG(3), dest = ARG(4);
    if (!mem::valid(range, 16)) RET(kErrIllegalAddr);
    const u32 x = mem::r32(range), y = mem::r32(range + 4), w = mem::r32(range + 8), h = mem::r32(range + 12);
    if (w == 0 || h == 0 || w > 720 || h > 576 || frame_width < w) RET(kErrInvalidValue);
    const u32 start = dest + (y * frame_width + x) * 4;
    if (!mem::valid(start, ((h - 1) * frame_width + w) * 4)) RET(kErrIllegalAddr);
    if (mv.video.has_frame()) mv.video.convert_rgba(mem::ptr(start), frame_width * 4, w, h);
    RET(0);
}

// (handle, stream, access unit, &attribute)
HLE(sceMpegGetAtracAu) {
    Movie& mv = g_movie;
    const u8* data;
    u32 size, channels;
    if (!next_audio_frame(mv, &data, &size, &channels)) RET(kErrNoData);
    const s64 pts = (mv.first_pts < 0 ? 0 : mv.first_pts) + s64(mv.audio_frames * kAudioFrameSamples * 90000 / 44100);
    write_au(ARG(2), pts);
    if (mem::valid(ARG(3), 4)) mem::w32(ARG(3), 0);
    RET(0);
}

// (handle, access unit, PCM buffer, init)
HLE(sceMpegAtracDecode) {
    Movie& mv = g_movie;
    u32 out = ARG(2);
    if (!mem::valid(out, kAudioFrameSamples * 4)) RET(kErrIllegalAddr);
    std::memset(mem::ptr(out), 0, kAudioFrameSamples * 4);
    const u8* data;
    u32 size, channels;
    if (next_audio_frame(mv, &data, &size, &channels)) {
        if (!mv.audio.is_open()) mv.audio.open(true, channels, 44100, size, nullptr, 0);
        std::vector<s16> pcm;
        if (mv.audio.decode(data, size, pcm) > 0)
            std::memcpy(mem::ptr(out), pcm.data(), std::min<size_t>(pcm.size() * 2, kAudioFrameSamples * 4));
        consume_audio_frame(mv, size);
        ++mv.audio_frames;
    }
    RET(0);
}

HLE(sceMpegFlushAllStream) {
    g_movie.reset_streams();
    RET(0);
}
