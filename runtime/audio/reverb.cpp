#include "runtime/audio/reverb.h"

#include <algorithm>
#include <cmath>

namespace p2::audio {

namespace {

// Comb and all-pass delays at 44.1 kHz for a room of size 1; chosen to share no common factors so
// their echoes do not line up. The right channel's are slightly longer for stereo width.
constexpr u32 kCombDelays[8] = {1123, 1201, 1279, 1361, 1433, 1499, 1567, 1637};
constexpr u32 kAllpassDelays[4] = {547, 433, 337, 229};
constexpr u32 kStereoSpread = 29;
constexpr f32 kAllpassGain = 0.5f;

struct Preset {
    f32 size;        // scales every delay of the network
    f32 decay;       // seconds for the tail to fall by 60 dB
    f32 damp;        // high-frequency loss per pass through a comb, 0 (none) to 1
    f32 predelay_ms; // gap before the reverberation starts
    f32 level;       // output level relative to the other types
};

// Smaller spaces decay faster and sound drier; the pipe is small, bright and resonant.
constexpr Preset kPresets[] = {
    /* room          */ {0.55f, 0.6f, 0.35f, 5.0f, 0.80f},
    /* studio small  */ {0.60f, 0.8f, 0.30f, 8.0f, 0.85f},
    /* studio medium */ {0.75f, 1.1f, 0.28f, 12.0f, 0.90f},
    /* studio large  */ {0.90f, 1.5f, 0.25f, 18.0f, 0.95f},
    /* hall          */ {1.10f, 2.4f, 0.30f, 25.0f, 1.00f},
    /* space         */ {1.20f, 3.2f, 0.20f, 30.0f, 1.00f},
    /* echo          */ {0.00f, 0.0f, 0.00f, 0.0f, 0.00f},
    /* delay         */ {0.00f, 0.0f, 0.00f, 0.0f, 0.00f},
    /* pipe          */ {0.30f, 1.0f, 0.05f, 2.0f, 0.75f},
};

// Overall output level of the reverberant network: a sustained sound at full effect volume comes
// back roughly 6 to 10 dB quieter than it went in, the larger spaces louder than the small ones.
constexpr f32 kNetworkGain = 0.36f;

} // namespace

void Reverb::Delay::resize(u32 length) {
    line.assign(std::max<u32>(length, 1), 0.0f);
    pos = 0;
}

f32 Reverb::Comb::tick(f32 in) {
    const f32 out = delay.read();
    state = out * (1.0f - damp) + state * damp; // one-pole low-pass in the loop: highs die first
    delay.write_advance(in + state * feedback);
    return out;
}

f32 Reverb::Allpass::tick(f32 in) {
    const f32 delayed = delay.read();
    const f32 out = delayed - in * kAllpassGain;
    delay.write_advance(in + delayed * kAllpassGain);
    return out;
}

Reverb::Reverb(u32 sample_rate) : rate_(sample_rate) {}

void Reverb::configure(s32 type, s32 delay, s32 feedback) {
    type = type < kOff || type > kPipe ? kOff : type;
    delay = std::clamp(delay, 0, 127);
    feedback = std::clamp(feedback, 0, 127);
    if (type == type_ && delay == delay_param_ && feedback == feedback_param_) return;
    const bool new_type = type != type_;
    const bool new_delay = delay != delay_param_;
    type_ = type;
    delay_param_ = delay;
    feedback_param_ = feedback;
    if (new_type || (echoing_ && new_delay)) build();
    // Feedback alone can change on the fly.
    if (echoing_) echo_feedback_ = (type_ == kSpace ? 0.5f : 0.85f) * f32(feedback_param_) / 127.0f;
}

void Reverb::build() {
    const f32 scale = f32(rate_) / 44100.0f;
    reverberant_ = type_ != kOff && type_ != kEcho && type_ != kDelay;
    echoing_ = type_ == kEcho || type_ == kDelay || type_ == kSpace;
    if (type_ == kOff) {
        clear();
        return;
    }
    const Preset& p = kPresets[type_];
    wet_gain_ = kNetworkGain * p.level;
    predelay_.resize(u32(p.predelay_ms * 0.001f * f32(rate_)) + 1);

    for (int ch = 0; ch < 2; ++ch) {
        Channel& c = channels_[ch];
        const u32 spread = ch ? kStereoSpread : 0;
        for (int i = 0; i < 8; ++i) {
            Comb& comb = c.combs[size_t(i)];
            const u32 length = std::max<u32>(u32(f32(kCombDelays[i] + spread) * p.size * scale), 16);
            comb.delay.resize(reverberant_ ? length : 1);
            // Each comb's feedback is set so that it alone decays by 60 dB in the preset's time.
            comb.feedback = reverberant_ ? std::pow(10.0f, -3.0f * f32(length) / (f32(rate_) * p.decay)) : 0.0f;
            comb.damp = p.damp;
            comb.state = 0.0f;
        }
        for (int i = 0; i < 4; ++i) {
            const u32 length = std::max<u32>(u32(f32(kAllpassDelays[i] + spread) * std::max(p.size, 0.3f) * scale), 8);
            c.allpasses[size_t(i)].delay.resize(reverberant_ ? length : 1);
        }
        // The echo types repeat after up to about half a second; the left and right repeats are
        // offset a little so the echo is wide rather than centred.
        const f32 seconds = 0.02f + 0.48f * f32(delay_param_) / 127.0f;
        c.echo.resize(echoing_ ? u32(seconds * f32(rate_)) + (ch ? u32(0.011f * f32(rate_)) : 0u) + 1 : 1);
        c.echo_state = 0.0f;
    }
    echo_damp_ = type_ == kDelay ? 0.0f : 0.35f;
    echo_level_ = type_ == kSpace ? 0.25f : 0.5f;
    echo_feedback_ = (type_ == kSpace ? 0.5f : 0.85f) * f32(feedback_param_) / 127.0f;
}

void Reverb::clear() {
    std::fill(predelay_.line.begin(), predelay_.line.end(), 0.0f);
    for (Channel& c : channels_) {
        for (Comb& comb : c.combs) {
            std::fill(comb.delay.line.begin(), comb.delay.line.end(), 0.0f);
            comb.state = 0.0f;
        }
        for (Allpass& ap : c.allpasses) std::fill(ap.delay.line.begin(), ap.delay.line.end(), 0.0f);
        std::fill(c.echo.line.begin(), c.echo.line.end(), 0.0f);
        c.echo_state = 0.0f;
    }
}

void Reverb::process(f32 in_left, f32 in_right, f32& out_left, f32& out_right) {
    out_left = out_right = 0.0f;
    if (type_ == kOff) return;
    const f32 inputs[2] = {in_left, in_right};
    f32* outputs[2] = {&out_left, &out_right};

    if (reverberant_) {
        // The network is fed a mono sum after the pre-delay and decorrelated per channel.
        const f32 mono = predelay_.read();
        predelay_.write_advance((in_left + in_right) * 0.5f);
        for (int ch = 0; ch < 2; ++ch) {
            Channel& c = channels_[ch];
            f32 sum = 0.0f;
            for (Comb& comb : c.combs) sum += comb.tick(mono);
            sum *= 1.0f / 8.0f;
            for (Allpass& ap : c.allpasses) sum = ap.tick(sum);
            *outputs[ch] += sum * wet_gain_;
        }
    }
    if (echoing_) {
        for (int ch = 0; ch < 2; ++ch) {
            Channel& c = channels_[ch];
            const f32 delayed = c.echo.read();
            c.echo_state = delayed * (1.0f - echo_damp_) + c.echo_state * echo_damp_;
            c.echo.write_advance(inputs[ch] + c.echo_state * echo_feedback_);
            *outputs[ch] += delayed * echo_level_;
        }
    }
}

} // namespace p2::audio
