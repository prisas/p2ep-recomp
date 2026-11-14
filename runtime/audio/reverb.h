#pragma once

#include "lib/common/types.h"

#include <array>
#include <vector>

// The reverb of the console's software synthesizer. The game selects one of nine effect types and,
// for the echo-like types, a delay time and feedback amount; the synthesizer sends its voice mix
// through the effect and adds the result to its output.
//
// This is an algorithmic reverb (parallel damped comb filters into series all-pass filters, plus a
// feedback delay for the echo types) tuned per effect type for a similar character and level. It
// is not a sample-exact model of the console's effect unit.
namespace p2::audio {

class Reverb {
public:
    enum Type : s32 {
        kOff = -1,
        kRoom = 0,
        kStudioSmall = 1,
        kStudioMedium = 2,
        kStudioLarge = 3,
        kHall = 4,
        kSpace = 5,
        kEcho = 6,
        kDelay = 7,
        kPipe = 8,
    };

    explicit Reverb(u32 sample_rate = 44100);

    // `delay` and `feedback` are the game's 0-127 parameters; only the echo-like types use them.
    // Changing the type clears the effect's memory; changing only the parameters does not.
    void configure(s32 type, s32 delay, s32 feedback);
    bool active() const { return type_ != kOff; }
    void clear();

    // One stereo frame in (16-bit range), the effect's output out (same scale, not yet attenuated by
    // the game's effect volume).
    void process(f32 in_left, f32 in_right, f32& out_left, f32& out_right);

private:
    struct Delay {
        std::vector<f32> line;
        u32 pos = 0;
        void resize(u32 length);
        f32 read() const { return line[pos]; }
        void write_advance(f32 v) {
            line[pos] = v;
            pos = pos + 1 == line.size() ? 0 : pos + 1;
        }
    };
    struct Comb {
        Delay delay;
        f32 feedback = 0.0f, damp = 0.0f, state = 0.0f;
        f32 tick(f32 in);
    };
    struct Allpass {
        Delay delay;
        f32 tick(f32 in);
    };
    struct Channel {
        std::array<Comb, 8> combs;
        std::array<Allpass, 4> allpasses;
        Delay echo;
        f32 echo_state = 0.0f;
    };

    void build();

    u32 rate_;
    s32 type_ = kOff;
    s32 delay_param_ = 0, feedback_param_ = 0;
    bool reverberant_ = false;   // the comb/all-pass network is in use
    bool echoing_ = false;       // the feedback delay is in use
    f32 echo_feedback_ = 0.0f, echo_damp_ = 0.0f, echo_level_ = 0.0f;
    f32 wet_gain_ = 0.0f;
    Delay predelay_;
    Channel channels_[2];
};

} // namespace p2::audio
