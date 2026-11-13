// sceSasCore: a 32-voice software synthesizer. Each voice plays ADPCM (VAG) or PCM data at a
// variable pitch through an ADSR envelope; the game asks for one "grain" of mixed output at a
// time and sends it to an audio channel itself.

#include "runtime/hle/hle.h"

#include <algorithm>

using namespace p2;
using namespace p2::kernel;

namespace {

constexpr u32 kVoices = 32;
constexpr s64 kEnvMax = 0x40000000;
constexpr u32 kPitchBase = 0x1000;
constexpr s32 kVolumeMax = 0x1000;

constexpr u32 kErrInvalidVoice = 0x80420010;
constexpr u32 kErrInvalidAddress = 0x80420005;
constexpr u32 kErrInvalidPitch = 0x80420012;
constexpr u32 kErrInvalidVolume = 0x80420018;
constexpr u32 kErrInvalidSize = 0x8042001A;
constexpr u32 kErrInvalidGrain = 0x80420001;

enum class Curve : u8 { LinearUp, LinearDown, LinearBent, ExpDownReverse, ExpDown, Direct };
enum class Stage : u8 { Off, Attack, Decay, Sustain, Release };

struct Envelope {
    Stage stage = Stage::Off;
    s64 height = 0;
    s32 attack_rate = 0, decay_rate = 0, sustain_rate = 0, release_rate = 0;
    Curve attack_curve = Curve::LinearUp, decay_curve = Curve::ExpDown, sustain_curve = Curve::LinearDown,
          release_curve = Curve::ExpDown;
    s32 sustain_level = 0;

    void walk(Curve curve, s32 rate) {
        switch (curve) {
        case Curve::LinearUp: height += rate; break;
        case Curve::LinearDown: height -= rate; break;
        case Curve::LinearBent: height += height <= kEnvMax * 3 / 4 ? rate : rate / 4; break;
        case Curve::ExpDownReverse: height += ((kEnvMax - height) * u32(rate)) >> 32; break;
        case Curve::ExpDown: height -= std::max<s64>((height * u32(rate)) >> 32, 1); break;
        case Curve::Direct: height = rate; break;
        }
    }

    void step() {
        switch (stage) {
        case Stage::Off: break;
        case Stage::Attack:
            walk(attack_curve, attack_rate);
            if (height >= kEnvMax || attack_curve == Curve::Direct) {
                height = std::min(height, kEnvMax);
                stage = Stage::Decay;
            }
            break;
        case Stage::Decay:
            walk(decay_curve, decay_rate);
            if (height <= sustain_level || decay_curve == Curve::Direct) {
                height = std::max<s64>(height, sustain_level);
                stage = Stage::Sustain;
            }
            break;
        case Stage::Sustain:
            walk(sustain_curve, sustain_rate);
            if (height <= 0) {
                height = 0;
                stage = Stage::Off;
            }
            break;
        case Stage::Release:
            walk(release_curve, release_rate);
            if (height <= 0) {
                height = 0;
                stage = Stage::Off;
            }
            break;
        }
        height = std::clamp<s64>(height, 0, kEnvMax);
    }
};

struct Voice {
    enum class Source : u8 { None, Vag, Pcm, Noise } source = Source::None;
    bool playing = false;
    bool paused = false;

    // ADPCM stream
    u32 vag_addr = 0, vag_blocks = 0;
    bool vag_loop = false;
    u32 block = 0, loop_block = 0;
    bool loop_next = false, stream_end = false;
    s32 hist1 = 0, hist2 = 0;
    s16 decoded[28] = {};
    u32 decoded_pos = 28;

    // PCM stream
    u32 pcm_addr = 0;
    s32 pcm_samples = 0, pcm_loop = -1, pcm_pos = 0;

    u32 noise_state = 0x12345;

    // resampler
    u32 pitch = kPitchBase;
    u32 frac = 0;
    s32 prev = 0, cur = 0;

    s32 vol_left = kVolumeMax, vol_right = kVolumeMax;
    Envelope env;

    void decode_block() {
        static constexpr s32 coef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        if (block >= vag_blocks) {
            stream_end = true;
            return;
        }
        if (loop_next) {
            loop_next = false;
            block = loop_block;
        }
        const u32 addr = vag_addr + block * 16;
        if (!mem::valid(addr, 16)) {
            stream_end = true;
            return;
        }
        const u8* p = mem::ptr(addr);
        const u32 shift = p[0] & 15, predictor = std::min<u32>(p[0] >> 4, 4);
        const u32 flags = p[1];
        if (flags == 7) { // end marker: nothing to play in this block
            stream_end = true;
            return;
        }
        if (flags & 4) loop_block = block; // loop start
        if ((flags & 1) && (flags & 2) && vag_loop) loop_next = true;
        else if ((flags & 1) && !(flags & 2)) stream_end_after_ = true;

        for (u32 i = 0; i < 28; ++i) {
            u32 nibble = (p[2 + i / 2] >> ((i & 1) * 4)) & 15;
            s32 sample = s32(s16(nibble << 12)) >> shift;
            sample += (hist1 * coef[predictor][0] + hist2 * coef[predictor][1]) >> 6;
            sample = std::clamp(sample, -32768, 32767);
            hist2 = hist1;
            hist1 = sample;
            decoded[i] = s16(sample);
        }
        decoded_pos = 0;
        ++block;
    }

    s32 next_sample() {
        switch (source) {
        case Source::Vag:
            if (decoded_pos >= 28) {
                if (stream_end_after_) stream_end = true;
                if (!stream_end) decode_block();
                if (stream_end) return 0;
            }
            return decoded[decoded_pos++];
        case Source::Pcm:
            if (pcm_pos >= pcm_samples) {
                if (pcm_loop >= 0 && pcm_loop < pcm_samples) {
                    pcm_pos = pcm_loop;
                } else {
                    stream_end = true;
                    return 0;
                }
            }
            if (!mem::valid(pcm_addr + u32(pcm_pos) * 2, 2)) {
                stream_end = true;
                return 0;
            }
            return s16(mem::r16(pcm_addr + u32(pcm_pos++) * 2));
        case Source::Noise:
            noise_state = noise_state * 1664525u + 1013904223u;
            return s16(noise_state >> 16);
        default:
            stream_end = true;
            return 0;
        }
    }

    void key_on() {
        block = 0;
        loop_block = 0;
        loop_next = false;
        stream_end = false;
        stream_end_after_ = false;
        hist1 = hist2 = 0;
        decoded_pos = 28;
        pcm_pos = 0;
        frac = 0;
        prev = cur = 0;
        env.height = 0;
        env.stage = Stage::Attack;
        playing = true;
        paused = false;
    }

    void key_off() {
        if (env.stage != Stage::Off) env.stage = Stage::Release;
    }

    // Produces one output sample (before volume), advancing the source by the pitch ratio.
    s32 render() {
        frac += pitch;
        while (frac >= kPitchBase) {
            frac -= kPitchBase;
            prev = cur;
            cur = next_sample();
        }
        env.step();
        if (stream_end || env.stage == Stage::Off) {
            playing = false;
            env.stage = Stage::Off;
            env.height = 0;
            return 0;
        }
        s32 sample = prev + (((cur - prev) * s32(frac)) >> 12);
        return s32((s64(sample) * (env.height >> 15)) >> 15);
    }

private:
    bool stream_end_after_ = false;
};

struct Sas {
    u32 grain = 256;
    u32 output_mode = 0;
    Voice voices[kVoices];
};

Sas g_sas;

// --- packed ("simple") envelope parameters --------------------------------------------------------------------

s32 simple_rate(u32 n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    s32 rate = s32(((7 - (n & 3)) << 26) >> (n >> 2));
    return rate ? rate : 1;
}

s32 exponent_rate(u32 n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    s32 rate = s32(((7 - (n & 3)) << 24) >> (n >> 2));
    return rate ? rate : 1;
}

void set_simple_envelope(Envelope& e, u32 env1, u32 env2) {
    e.attack_rate = simple_rate(env1 >> 8);
    e.attack_curve = (env1 & 0x8000) ? Curve::LinearBent : Curve::LinearUp;
    u32 decay = (env1 >> 4) & 15;
    e.decay_rate = decay == 0 ? 0x7FFFFFFF : s32(0x80000000u >> decay);
    e.decay_curve = Curve::ExpDown;
    e.sustain_level = s32(((env1 & 15) + 1) << 26);

    switch ((env2 >> 14) & 3) {
    case 0: e.sustain_curve = Curve::LinearUp; break;
    case 1: e.sustain_curve = Curve::LinearDown; break;
    case 2: e.sustain_curve = Curve::LinearBent; break;
    default: e.sustain_curve = Curve::ExpDown; break;
    }
    e.sustain_rate = e.sustain_curve == Curve::ExpDown ? exponent_rate(env2 >> 6) : simple_rate(env2 >> 6);

    u32 release = env2 & 31;
    if (env2 & 0x20) {
        e.release_curve = Curve::ExpDown;
        e.release_rate = release == 31 ? 0 : (release == 0 ? 0x7FFFFFFF : s32(0x80000000u >> release));
    } else {
        e.release_curve = Curve::LinearDown;
        e.release_rate = release == 31 ? 0 : (release == 30 ? 0x40000000 : (release == 29 ? 1 : s32(0x10000000u >> release)));
    }
}

Curve curve_from_mode(u32 mode, Curve fallback) {
    switch (mode) {
    case 0: return Curve::LinearUp;
    case 1: return Curve::LinearDown;
    case 2: return Curve::LinearBent;
    case 3: return Curve::ExpDownReverse;
    case 4: return Curve::ExpDown;
    case 5: return Curve::Direct;
    default: return fallback;
    }
}

void mix(u32 out, u32 in, s32 in_left, s32 in_right) {
    const u32 frames = g_sas.grain;
    if (!mem::valid(out, frames * 4)) return;
    for (u32 i = 0; i < frames; ++i) {
        s32 left = 0, right = 0;
        if (in) {
            left = (s32(s16(mem::r16(in + i * 4))) * in_left) >> 12;
            right = (s32(s16(mem::r16(in + i * 4 + 2))) * in_right) >> 12;
        }
        for (Voice& v : g_sas.voices) {
            if (!v.playing || v.paused) continue;
            s32 s = v.render();
            left += (s * v.vol_left) >> 12;
            right += (s * v.vol_right) >> 12;
        }
        mem::w16(out + i * 4, u16(s16(std::clamp(left, -32768, 32767))));
        mem::w16(out + i * 4 + 2, u16(s16(std::clamp(right, -32768, 32767))));
    }
}

} // namespace

HLE(__sceSasInit) {
    u32 grain = ARG(1), voices = ARG(2);
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) RET(kErrInvalidGrain);
    g_sas = Sas{};
    g_sas.grain = grain;
    g_sas.output_mode = ARG(3);
    LOG_CH("audio", "synthesizer init: grain %u, %u voices, mode %u, %u Hz", grain, voices, ARG(3), ARG(4));
    RET(0);
}

HLE(__sceSasCore) {
    mix(ARG(1), 0, 0, 0);
    RET(0);
}

HLE(__sceSasCoreWithMix) {
    mix(ARG(1), ARG(1), s32(ARG(2)), s32(ARG(3)));
    RET(0);
}

HLE(__sceSasSetVoice) {
    u32 voice = ARG(1), addr = ARG(2), loop = ARG(4);
    s32 size = s32(ARG(3));
    if (voice >= kVoices) RET(kErrInvalidVoice);
    if (size <= 0 || (size & 0xF)) RET(kErrInvalidSize);
    if (!mem::valid(addr, u32(size))) RET(kErrInvalidAddress);
    Voice& v = g_sas.voices[voice];
    v.source = Voice::Source::Vag;
    v.vag_addr = addr;
    v.vag_blocks = u32(size) / 16;
    v.vag_loop = loop != 0;
    RET(0);
}

HLE(__sceSasSetVoicePCM) {
    u32 voice = ARG(1), addr = ARG(2);
    s32 samples = s32(ARG(3)), loop = s32(ARG(4));
    if (voice >= kVoices) RET(kErrInvalidVoice);
    if (samples <= 0 || samples > 0x10000) RET(kErrInvalidSize);
    if (!mem::valid(addr, u32(samples) * 2)) RET(kErrInvalidAddress);
    Voice& v = g_sas.voices[voice];
    v.source = Voice::Source::Pcm;
    v.pcm_addr = addr;
    v.pcm_samples = samples;
    v.pcm_loop = loop;
    RET(0);
}

HLE(__sceSasSetNoise) {
    u32 voice = ARG(1);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    g_sas.voices[voice].source = Voice::Source::Noise;
    RET(0);
}

HLE(__sceSasSetPitch) {
    u32 voice = ARG(1), pitch = ARG(2);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    if (pitch == 0 || pitch > 0x4000) RET(kErrInvalidPitch);
    g_sas.voices[voice].pitch = pitch;
    RET(0);
}

HLE(__sceSasSetVolume) {
    u32 voice = ARG(1);
    s32 left = s32(ARG(2)), right = s32(ARG(3));
    if (voice >= kVoices) RET(kErrInvalidVoice);
    if (std::abs(left) > kVolumeMax || std::abs(right) > kVolumeMax) RET(kErrInvalidVolume);
    g_sas.voices[voice].vol_left = left;
    g_sas.voices[voice].vol_right = right;
    RET(0);
}

HLE(__sceSasSetADSR) {
    u32 voice = ARG(1), flags = ARG(2);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    Envelope& e = g_sas.voices[voice].env;
    if (flags & 1) e.attack_rate = s32(ARG(3));
    if (flags & 2) e.decay_rate = s32(ARG(4));
    if (flags & 4) e.sustain_rate = s32(ARG(5));
    if (flags & 8) e.release_rate = s32(ARG(6));
    RET(0);
}

HLE(__sceSasSetADSRmode) {
    u32 voice = ARG(1), flags = ARG(2);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    Envelope& e = g_sas.voices[voice].env;
    if (flags & 1) e.attack_curve = curve_from_mode(ARG(3), e.attack_curve);
    if (flags & 2) e.decay_curve = curve_from_mode(ARG(4), e.decay_curve);
    if (flags & 4) e.sustain_curve = curve_from_mode(ARG(5), e.sustain_curve);
    if (flags & 8) e.release_curve = curve_from_mode(ARG(6), e.release_curve);
    RET(0);
}

HLE(__sceSasSetSL) {
    u32 voice = ARG(1);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    g_sas.voices[voice].env.sustain_level = s32(ARG(2));
    RET(0);
}

HLE(__sceSasSetSimpleADSR) {
    u32 voice = ARG(1);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    set_simple_envelope(g_sas.voices[voice].env, ARG(2) & 0xFFFF, ARG(3) & 0xFFFF);
    RET(0);
}

HLE(__sceSasSetKeyOn) {
    u32 voice = ARG(1);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    g_sas.voices[voice].key_on();
    RET(0);
}

HLE(__sceSasSetKeyOff) {
    u32 voice = ARG(1);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    g_sas.voices[voice].key_off();
    RET(0);
}

HLE(__sceSasSetPause) {
    u32 bits = ARG(1);
    bool pause = ARG(2) != 0;
    for (u32 i = 0; i < kVoices; ++i)
        if (bits & (1u << i)) g_sas.voices[i].paused = pause;
    RET(0);
}

HLE(__sceSasGetPauseFlag) {
    u32 bits = 0;
    for (u32 i = 0; i < kVoices; ++i)
        if (g_sas.voices[i].paused) bits |= 1u << i;
    RET(bits);
}

HLE(__sceSasGetEndFlag) {
    u32 bits = 0;
    for (u32 i = 0; i < kVoices; ++i)
        if (!g_sas.voices[i].playing) bits |= 1u << i;
    RET(bits);
}

HLE(__sceSasGetEnvelopeHeight) {
    u32 voice = ARG(1);
    if (voice >= kVoices) RET(kErrInvalidVoice);
    RET(u32(g_sas.voices[voice].env.height));
}

HLE(__sceSasGetAllEnvelopeHeights) {
    u32 out = ARG(1);
    if (!mem::valid(out, kVoices * 4)) RET(kErrInvalidAddress);
    for (u32 i = 0; i < kVoices; ++i) mem::w32(out + i * 4, u32(g_sas.voices[i].env.height));
    RET(0);
}

// Reverb is accepted but not rendered.
HLE(__sceSasRevType) { RET(0); }
HLE(__sceSasRevParam) { RET(0); }
HLE(__sceSasRevEVOL) { RET(0); }
HLE(__sceSasRevVON) { RET(0); }

HLE(__sceSasSetOutputmode) {
    g_sas.output_mode = ARG(1);
    RET(0);
}
HLE(__sceSasGetOutputmode) { RET(g_sas.output_mode); }

HLE(__sceSasSetGrain) {
    u32 grain = ARG(1);
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) RET(kErrInvalidGrain);
    g_sas.grain = grain;
    RET(0);
}
HLE(__sceSasGetGrain) { RET(g_sas.grain); }
