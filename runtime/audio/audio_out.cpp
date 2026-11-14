#include "runtime/audio/audio_out.h"

#include "lib/common/log.h"
#include "runtime/platform/host.h"
#include "runtime/platform/window.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace p2::audio {

namespace {

SDL_AudioStream* g_stream = nullptr;
bool g_sdl_audio = false;
FILE* g_wav = nullptr;
u32 g_wav_frames = 0;
f32 g_volume = 1.0f;
bool g_muted = false;

constexpr int kBytesPerFrame = 4;
constexpr int kTargetQueueFrames = kSampleRate * 60 / 1000; // aim for about 60 ms buffered
constexpr int kMaxQueueFrames = kSampleRate * 200 / 1000;   // beyond this, drop instead of lagging

void wav_header(FILE* f, u32 frames) {
    const u32 data_bytes = frames * kBytesPerFrame;
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    store_le<u32>(h + 4, 36 + data_bytes);
    store_le<u32>(h + 24, kSampleRate);
    store_le<u32>(h + 28, kSampleRate * kBytesPerFrame);
    store_le<u16>(h + 32, kBytesPerFrame);
    store_le<u16>(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    store_le<u32>(h + 40, data_bytes);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), f);
}

} // namespace

void output_init() {
    if (host::options().headless) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_WARN("audio", "no audio subsystem: %s", SDL_GetError());
        return;
    }
    g_sdl_audio = true;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, int(kSampleRate)};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_stream) {
        LOG_WARN("audio", "cannot open an audio device: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
}

void output_shutdown() {
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
    if (g_sdl_audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_sdl_audio = false;
    if (g_wav) {
        wav_header(g_wav, g_wav_frames);
        std::fclose(g_wav);
        g_wav = nullptr;
    }
}

bool output_record(const std::string& wav_path) {
    g_wav = std::fopen(wav_path.c_str(), "wb");
    if (!g_wav) return false;
    g_wav_frames = 0;
    wav_header(g_wav, 0);
    return true;
}

void output_set_muted(bool muted) { g_muted = muted; }

void output_set_volume(f32 volume) {
    g_volume = volume;
    if (g_stream) SDL_SetAudioStreamGain(g_stream, volume);
}

void output_push(const s16* frames, u32 frame_count) {
    if (g_wav) {
        std::fwrite(frames, kBytesPerFrame, frame_count, g_wav);
        g_wav_frames += frame_count;
    }
    if (!g_stream || g_muted) return;

    const int queued = SDL_GetAudioStreamQueued(g_stream) / kBytesPerFrame;
    // Fast-forward produces audio faster than it can be played: keep only what fits.
    if (queued > kMaxQueueFrames) return;
    // Nudge the playback rate by up to half a percent so the queue hovers around its target.
    f32 error = f32(queued - kTargetQueueFrames) / f32(kTargetQueueFrames);
    if (error > 1.0f) error = 1.0f;
    if (error < -1.0f) error = -1.0f;
    SDL_SetAudioStreamFrequencyRatio(g_stream, 1.0f + 0.005f * error);
    SDL_PutAudioStreamData(g_stream, frames, int(frame_count) * kBytesPerFrame);
}

} // namespace p2::audio
