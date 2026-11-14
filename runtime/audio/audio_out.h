#pragma once

#include "lib/common/types.h"

#include <string>

// Host audio device. The guest mixes at 44.1 kHz stereo in guest time; this hands the result to
// the sound card and keeps the two clocks from drifting apart.
namespace p2::audio {

constexpr u32 kSampleRate = 44100;

// Opens the default playback device unless running headless. Never fails hard: without a device
// the game simply runs silent.
void output_init();
void output_shutdown();

// Queues interleaved stereo frames for playback.
void output_push(const s16* frames, u32 frame_count);

// Also write everything pushed to a WAV file (for unattended verification).
bool output_record(const std::string& wav_path);

void output_set_volume(f32 volume);

// While muted, pushed audio is dropped instead of played (recordings still receive it).
void output_set_muted(bool muted);

} // namespace p2::audio
