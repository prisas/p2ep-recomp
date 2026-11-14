#include "runtime/audio/reverb.h"
#include "tests/test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace p2;
using audio::Reverb;

namespace {

constexpr u32 kRate = 44100;

struct Run {
    std::vector<f32> left, right;
};

// Feeds `input(i)` (the same to both channels) for `frames` frames.
template <class Input>
Run run(Reverb& r, u32 frames, Input input) {
    Run out;
    out.left.resize(frames);
    out.right.resize(frames);
    for (u32 i = 0; i < frames; ++i) r.process(input(i), input(i), out.left[i], out.right[i]);
    return out;
}

f32 rms(const std::vector<f32>& v, size_t from, size_t to) {
    double sum = 0.0;
    for (size_t i = from; i < to; ++i) sum += double(v[i]) * v[i];
    return f32(std::sqrt(sum / double(to - from)));
}

f32 peak(const std::vector<f32>& v, size_t from, size_t to) {
    f32 m = 0.0f;
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(v[i]));
    return m;
}

f32 noise(u32 i) {
    u32 x = i * 2654435761u + 12345u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (f32(x & 0xFFFF) - 32768.0f) / 32768.0f * 8000.0f;
}

} // namespace

TEST(reverb_off_and_silence) {
    Reverb off;
    off.configure(Reverb::kOff, 0, 0);
    Run a = run(off, 1000, noise);
    CHECK_EQ(peak(a.left, 0, 1000), 0.0f);

    for (s32 type = Reverb::kRoom; type <= Reverb::kPipe; ++type) {
        Reverb r;
        r.configure(type, 64, 64);
        Run s = run(r, kRate, [](u32) { return 0.0f; });
        CHECK_EQ(peak(s.left, 0, kRate), 0.0f);
        CHECK_EQ(peak(s.right, 0, kRate), 0.0f);
    }
}

// Every type, at the strongest settings, dies away after the sound stops instead of ringing on.
TEST(reverb_tails_decay) {
    for (s32 type = Reverb::kRoom; type <= Reverb::kPipe; ++type) {
        Reverb r;
        r.configure(type, 64, 127);
        const u32 burst = kRate / 10, total = kRate * 20;
        Run out = run(r, total, [&](u32 i) { return i < burst ? noise(i) : 0.0f; });
        const f32 loud = peak(out.left, 0, kRate * 2);
        const f32 last = std::max(peak(out.left, total - kRate / 10, total), peak(out.right, total - kRate / 10, total));
        CHECK(loud > 100.0f);       // there is an effect at all
        CHECK(last < loud * 1e-3f); // and after 20 s it is at least 60 dB down
        for (f32 v : out.left) CHECK(std::isfinite(v));
    }
}

// A sustained sound comes back from the reverberant types at a similar, moderate level, and with
// different left and right signals (the effect is wide, not mono).
TEST(reverb_level_and_width) {
    const bool show = std::getenv("P2_SHOW_REVERB_LEVELS") != nullptr;
    for (s32 type = Reverb::kRoom; type <= Reverb::kPipe; ++type) {
        Reverb r;
        r.configure(type, 64, 64);
        const u32 frames = kRate * 4;
        Run out = run(r, frames, noise);
        const f32 in = 8000.0f / std::sqrt(3.0f);
        const f32 ratio = rms(out.left, frames - kRate, frames) / in;
        if (show) std::printf("type %d: output/input %.3f\n", type, double(ratio));
        CHECK(ratio > 0.15f);
        CHECK(ratio < 0.8f);
        double difference = 0.0;
        for (u32 i = frames - kRate; i < frames; ++i) difference += std::fabs(out.left[i] - out.right[i]);
        CHECK(difference > 0.0);
    }
}

// The echo types repeat the sound after the delay the game asks for.
TEST(reverb_echo_timing) {
    Reverb r;
    r.configure(Reverb::kDelay, 127, 0);
    Run out = run(r, kRate, [](u32 i) { return i == 0 ? 10000.0f : 0.0f; });
    size_t at = 0;
    for (size_t i = 0; i < out.left.size(); ++i)
        if (std::fabs(out.left[i]) > 1000.0f) {
            at = i;
            break;
        }
    // 0.02 s + 0.48 s at the longest setting.
    CHECK(at > size_t(kRate * 0.49));
    CHECK(at < size_t(kRate * 0.51));
}

// Changing only the feedback keeps the effect running; changing the type starts it from silence.
TEST(reverb_reconfigure) {
    Reverb r;
    r.configure(Reverb::kHall, 0, 0);
    run(r, kRate / 2, noise);
    r.configure(Reverb::kRoom, 0, 0);
    Run after = run(r, 64, [](u32) { return 0.0f; });
    CHECK_EQ(peak(after.left, 0, 64), 0.0f);
}
