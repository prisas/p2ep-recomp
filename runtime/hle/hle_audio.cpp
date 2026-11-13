// sceAudio: eight PCM output channels mixed at 44.1 kHz.
//
// A blocking output call queues its buffer and then waits until the data that was already queued
// has been played, which is how games pace their audio threads. Playback advances in guest time:
// a recurring event consumes a fixed number of frames from every channel and hands the mix to the
// host device.

#include "runtime/audio/audio_out.h"
#include "runtime/hle/hle.h"

#include <algorithm>
#include <vector>

using namespace p2;
using namespace p2::kernel;

namespace {

constexpr u32 kChannels = 8;
constexpr u32 kMixFrames = 256;
constexpr u32 kMaxVolume = 0x8000;
constexpr u32 kFormatMono = 0x10;

constexpr u32 kErrNotInit = 0x80260001;
constexpr u32 kErrBusy = 0x80260002;
constexpr u32 kErrInvalidChannel = 0x80260003;
constexpr u32 kErrNoChannels = 0x80260005;
constexpr u32 kErrBadSize = 0x80260006;
constexpr u32 kErrBadFormat = 0x80260007;
constexpr u32 kErrNotReserved = 0x80260008;
constexpr u32 kErrBadVolume = 0x8026000B;

struct Waiter {
    Uid thread;
    u32 serial;
    s32 frames_left;
};

struct Channel {
    bool reserved = false;
    u32 sample_count = 0;
    u32 format = 0;
    s32 left_volume = kMaxVolume, right_volume = kMaxVolume;
    std::vector<s16> queue; // interleaved stereo, volume already applied
    size_t read = 0;
    std::vector<Waiter> waiters;

    u32 queued_frames() const { return u32((queue.size() - read) / 2); }
};

Channel g_channels[kChannels];
bool g_mixer_running = false;
u64 g_mix_ticks = 0;
u64 g_mix_origin_us = 0;

void mix_event(u64) {
    s32 mix[kMixFrames * 2] = {};
    for (u32 ch = 0; ch < kChannels; ++ch) {
        Channel& c = g_channels[ch];
        u32 frames = std::min(c.queued_frames(), kMixFrames);
        if (frames) {
            const s16* src = c.queue.data() + c.read;
            for (u32 i = 0; i < frames * 2; ++i) mix[i] += src[i];
            c.read += size_t(frames) * 2;
            if (c.read == c.queue.size()) {
                c.queue.clear();
                c.read = 0;
            }
        }
        // The frames consumed count against every waiter; an empty queue releases them outright.
        for (size_t i = 0; i < c.waiters.size();) {
            Waiter& w = c.waiters[i];
            w.frames_left -= s32(kMixFrames);
            Thread* t = get<Thread>(w.thread);
            const bool live = t && t->wait_type == WaitType::Audio && t->wait_serial == w.serial;
            if (!live || w.frames_left <= 0 || c.queued_frames() == 0) {
                if (live) wake_thread(t, 0);
                c.waiters.erase(c.waiters.begin() + ptrdiff_t(i));
            } else {
                ++i;
            }
        }
    }

    s16 out[kMixFrames * 2];
    for (u32 i = 0; i < kMixFrames * 2; ++i) out[i] = s16(std::clamp(mix[i], -32768, 32767));
    audio::output_push(out, kMixFrames);

    // Schedule from the origin so rounding never accumulates into drift.
    ++g_mix_ticks;
    u64 next = g_mix_origin_us + g_mix_ticks * kMixFrames * 1000000ull / audio::kSampleRate;
    u64 now = now_us();
    schedule_event(next > now ? next - now : 1, mix_event, 0);
}

void ensure_mixer() {
    if (g_mixer_running) return;
    g_mixer_running = true;
    g_mix_origin_us = now_us();
    g_mix_ticks = 0;
    schedule_event(kMixFrames * 1000000ull / audio::kSampleRate, mix_event, 0);
}

// Queues one buffer. Returns the number of frames that were already waiting to play.
u32 enqueue(Channel& c, u32 buffer, s32 left, s32 right) {
    const u32 pending = c.queued_frames();
    if (!buffer) return pending;
    const u32 frames = c.sample_count;
    const u32 bytes = frames * (c.format == kFormatMono ? 2 : 4);
    if (!mem::valid(buffer, bytes)) return pending;
    const size_t at = c.queue.size();
    c.queue.resize(at + size_t(frames) * 2);
    s16* dst = c.queue.data() + at;
    const u8* src = mem::ptr(buffer);
    if (c.format == kFormatMono) {
        for (u32 i = 0; i < frames; ++i) {
            s32 s = load_le<s16>(src + i * 2);
            dst[i * 2] = s16(std::clamp((s * left) >> 15, -32768, 32767));
            dst[i * 2 + 1] = s16(std::clamp((s * right) >> 15, -32768, 32767));
        }
    } else {
        for (u32 i = 0; i < frames; ++i) {
            dst[i * 2] = s16(std::clamp((s32(load_le<s16>(src + i * 4)) * left) >> 15, -32768, 32767));
            dst[i * 2 + 1] = s16(std::clamp((s32(load_le<s16>(src + i * 4 + 2)) * right) >> 15, -32768, 32767));
        }
    }
    return pending;
}

u32 output(u32 channel, s32 left, s32 right, u32 buffer, bool blocking) {
    if (channel >= kChannels) return kErrInvalidChannel;
    Channel& c = g_channels[channel];
    if (!c.reserved) return kErrNotInit;
    if (u32(left) > 0xFFFF || u32(right) > 0xFFFF) return kErrBadVolume;
    ensure_mixer();

    if (!blocking && c.queued_frames() > 0) return kErrBusy;
    const u32 pending = enqueue(c, buffer, left, right);
    if (blocking && pending > 0) {
        Thread* t = current_thread();
        if (!t) return kErrCanNotWait;
        c.waiters.push_back({t->uid, t->wait_serial + 1, s32(pending)});
        wait_current(WaitType::Audio, Uid(channel), 0, false);
    }
    return c.sample_count;
}

} // namespace

HLE(sceAudioChReserve) {
    s32 channel = s32(ARG(0));
    u32 samples = ARG(1), format = ARG(2);
    if (channel < 0) {
        channel = -1;
        for (s32 i = s32(kChannels) - 1; i >= 0; --i) {
            if (!g_channels[i].reserved) {
                channel = i;
                break;
            }
        }
        if (channel < 0) RET(kErrNoChannels);
    }
    if (u32(channel) >= kChannels) RET(kErrInvalidChannel);
    if ((samples & 63) || samples == 0 || samples > 65472) RET(kErrBadSize);
    if (format != 0 && format != kFormatMono) RET(kErrBadFormat);
    Channel& ch = g_channels[channel];
    if (ch.reserved) RET(kErrInvalidChannel);
    ch = Channel{};
    ch.reserved = true;
    ch.sample_count = samples;
    ch.format = format;
    LOG_CH("audio", "reserve channel %d: %u samples, %s", channel, samples, format ? "mono" : "stereo");
    RET(channel);
}

HLE(sceAudioChRelease) {
    u32 channel = ARG(0);
    if (channel >= kChannels) RET(kErrInvalidChannel);
    Channel& ch = g_channels[channel];
    if (!ch.reserved) RET(kErrNotReserved);
    for (const Waiter& w : ch.waiters) {
        Thread* t = get<Thread>(w.thread);
        if (t && t->wait_type == WaitType::Audio && t->wait_serial == w.serial) wake_thread(t, 0);
    }
    ch = Channel{};
    RET(0);
}

HLE(sceAudioOutputBlocking) { RET(output(ARG(0), s32(ARG(1)), s32(ARG(1)), ARG(2), true)); }
HLE(sceAudioOutputPannedBlocking) { RET(output(ARG(0), s32(ARG(1)), s32(ARG(2)), ARG(3), true)); }
HLE(sceAudioOutputPanned) { RET(output(ARG(0), s32(ARG(1)), s32(ARG(2)), ARG(3), false)); }

HLE(sceAudioChangeChannelConfig) {
    u32 channel = ARG(0), format = ARG(1);
    if (channel >= kChannels) RET(kErrInvalidChannel);
    if (!g_channels[channel].reserved) RET(kErrNotReserved);
    if (format != 0 && format != kFormatMono) RET(kErrBadFormat);
    g_channels[channel].format = format;
    RET(0);
}

HLE(sceAudioChangeChannelVolume) {
    u32 channel = ARG(0);
    if (channel >= kChannels) RET(kErrInvalidChannel);
    if (!g_channels[channel].reserved) RET(kErrNotReserved);
    g_channels[channel].left_volume = s32(ARG(1));
    g_channels[channel].right_volume = s32(ARG(2));
    RET(0);
}

HLE(sceAudioSetChannelDataLen) {
    u32 channel = ARG(0), samples = ARG(1);
    if (channel >= kChannels) RET(kErrInvalidChannel);
    if (!g_channels[channel].reserved) RET(kErrNotReserved);
    if ((samples & 63) || samples == 0 || samples > 65472) RET(kErrBadSize);
    g_channels[channel].sample_count = samples;
    RET(0);
}

HLE(sceAudioGetChannelRestLength) {
    u32 channel = ARG(0);
    if (channel >= kChannels) RET(kErrInvalidChannel);
    RET(g_channels[channel].queued_frames());
}
