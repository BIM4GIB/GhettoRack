#include "engine.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#ifdef AB_HOST_DEBUG
#include <cstdio>
#endif

using namespace daisysp;

namespace ab
{
namespace
{
constexpr float kHalfPi = 1.57079633f;
constexpr float kTwoPi  = 6.28318531f;
constexpr float kSqrt2  = 1.41421356f;

/** Delay times in sixteenth notes: 1/16, 1/8, dotted 1/8, 1/4, dotted 1/4, 1/2 */
const float kDelaySteps[6] = {1.f, 2.f, 3.f, 4.f, 6.f, 8.f};
/** Beat repeat lengths in sixteenth notes for 1/4, 1/8, 1/16, 1/32 */
const float kRepeatSteps[4] = {4.f, 2.f, 1.f, 0.5f};

inline float Clamp01(float x)
{
    return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
}

inline uint32_t XorShift(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

inline float RandF(uint32_t& s)
{
    return (XorShift(s) >> 8) * (1.f / 16777216.f);
}

/** Makeup gain so drive changes colour more than level */
inline float DriveComp(float g)
{
    return SoftClip(0.7f) / SoftClip(0.7f * g);
}

/** Knob position to filter mode and cutoff. Centre band is "off". */
inline int FilterMap(float x, float& freq)
{
    if(x < 0.46f)
    {
        freq = 40.f * powf(2.f, (x / 0.46f) * 9.f); // 40 Hz .. 20 kHz
        return 1;
    }
    if(x > 0.54f)
    {
        freq = 20.f * powf(2.f, ((x - 0.54f) / 0.46f) * 9.f); // 20 Hz .. 10 kHz
        return 2;
    }
    freq = 0.f;
    return 0;
}
} // namespace

/* ------------------------------------------------------------------ */

void Engine::Init(float sample_rate, const Memory& mem)
{
    sr_            = sample_rate;
    slot_capacity_ = mem.slot_capacity;

    for(int t = 0; t < kTracks; t++)
    {
        slots_[t].data     = mem.slot_mem[t];
        home_[t]           = mem.slot_mem[t];
        slots_[t].capacity = mem.slot_capacity;
        slots_[t].length   = 0;
        slots_[t].rate     = 1.f;
        filt_[t].Init(sr_);
        filt_[t].SetDrive(0.f); // the cubic drive term can blow up at high resonance
        last_filter_[t] = -1.f;
        last_reso_[t]   = -1.f;
        skip_once_[t]   = -1;
        pitch_ratio_[t] = 1.f;
        drive_gain_[t]  = 1.f;
        drive_comp_[t]  = 1.f;
        voices_[t][0]   = Voice();
        voices_[t][1]   = Voice();
        acid_[t].Init(sr_);
        acid_last_[t] = kAcidDefaultNote;
    }

    dly_l_   = mem.delay_l;
    dly_r_   = mem.delay_r;
    dly_len_ = mem.delay_len;
    if(dly_l_ && dly_r_)
    {
        memset(dly_l_, 0, sizeof(float) * dly_len_);
        memset(dly_r_, 0, sizeof(float) * dly_len_);
    }
    dly_w_ = 0;

    cap_l_   = mem.capture_l;
    cap_r_   = mem.capture_r;
    cap_len_ = mem.capture_len;
    if(cap_l_ && cap_r_)
    {
        memset(cap_l_, 0, sizeof(float) * cap_len_);
        memset(cap_r_, 0, sizeof(float) * cap_len_);
    }
    cap_w_ = 0;

#ifdef USE_DAISYSP_LGPL
    reverb_ = mem.reverb;
    if(reverb_)
        reverb_->Init(sr_);
#endif

    dj_l_.Init(sr_);
    dj_r_.Init(sr_);
    dj_l_.SetDrive(0.f);
    dj_r_.SetDrive(0.f);
    crush_l_.Init();
    crush_r_.Init();
    crush_l_.SetDownsampleFactor(0.22f);
    crush_r_.SetDownsampleFactor(0.22f);
    crush_l_.SetBitsToCrush(10);
    crush_r_.SetBitsToCrush(10);

    cur_step_len_     = BaseStepLength();
    to_next_step_     = cur_step_len_;
    dly_target_       = 4.f * cur_step_len_;
    dly_time_smooth_  = dly_target_;
    master_gain_.cur  = 0.f;
    fx_return_.cur    = 0.f;
}

/* ------------------------------------------------------------------ */
/* Main loop side                                                      */

void Engine::Play()
{
    play_req_ = true;
}

void Engine::Stop()
{
    stop_req_ = true;
}

void Engine::Panic()
{
    panic_req_ = true;
}

void Engine::TriggerLive(int t)
{
    if(t >= 0 && t < kTracks)
        live_req_[t] = 1;
}

void Engine::Audition(int t)
{
    if(t >= 0 && t < kTracks)
        live_req_[t] = 2;
}

void Engine::ClearTrack(int pat, int t)
{
    for(int s = 0; s < kSteps; s++)
    {
        patterns_[pat][t][s] = kStepOff;
        notes_[pat][t][s]    = 0;
    }
}

void Engine::CopyPattern(int from, int to)
{
    if(from == to)
        return;
    memcpy(patterns_[to], patterns_[from], sizeof(patterns_[0]));
    memcpy(notes_[to], notes_[from], sizeof(notes_[0]));
}

void Engine::CopyTrack(int from, int to, int t)
{
    if(from == to || t < 0 || t >= kTracks)
        return;
    memcpy(patterns_[to][t], patterns_[from][t], kSteps);
    memcpy(notes_[to][t], notes_[from][t], kSteps);
}

void Engine::ClearPattern(int pat)
{
    memset(patterns_[pat], 0, sizeof(patterns_[0]));
    memset(notes_[pat], 0, sizeof(notes_[0]));
}

void Engine::SavePattern(int pat, uint8_t* dst) const
{
    memcpy(dst, patterns_[pat], sizeof(patterns_[0]));
    memcpy(dst + sizeof(patterns_[0]), notes_[pat], sizeof(notes_[0]));
}

void Engine::RestorePattern(int pat, const uint8_t* src)
{
    memcpy(patterns_[pat], src, sizeof(patterns_[0]));
    memcpy(notes_[pat], src + sizeof(patterns_[0]), sizeof(notes_[0]));
}

void Engine::LiveNote(int t, int note, bool on)
{
    if(t < 0 || t >= kTracks)
        return;
    uint32_t w = note_w_;
    if(w - note_r_ >= (uint32_t)kNoteQueue)
        return; // audio isn't running: drop
    note_q_[w % kNoteQueue] = {(int8_t)t, (int8_t)note, on, 0};
    __sync_synchronize();
    note_w_ = w + 1;
}

void Engine::KeyNote(int t, int note, int vel, bool on)
{
    if(t < 0 || t >= kTracks || note < 0 || note > 127)
        return;
    uint32_t w = note_w_;
    if(w - note_r_ >= (uint32_t)kNoteQueue)
        return;
    vel = vel < 1 ? 1 : (vel > 127 ? 127 : vel);
    note_q_[w % kNoteQueue] = {(int8_t)t, (int8_t)note, on, (uint8_t)vel};
    __sync_synchronize();
    note_w_ = w + 1;
}

bool Engine::PatternEmpty(int pat) const
{
    for(int t = 0; t < kTracks; t++)
        for(int s = 0; s < kSteps; s++)
            if(patterns_[pat][t][s])
                return false;
    return true;
}

void Engine::QueuePattern(int pat)
{
    if(pat < 0 || pat >= kPatterns)
        return;
    if(!playing_)
    {
        pattern_        = pat;
        queued_pattern_ = -1;
    }
    else
    {
        queued_pattern_ = (pat == pattern_) ? -1 : pat;
    }
}

/* ------------------------------------------------------------------ */
/* Built-in kit: synthesized once into the sample slots                */

/* Each sound is a small spec rendered by one of a few synth recipes,
 * then a per-kit colour stage (drive, bit depth, sample-and-hold, tone).
 * Rendered twice: once to measure the peak, once to write it at full
 * resolution. The noise sources are reseeded so both passes match. */
namespace
{
enum VType : uint8_t
{
    V_BDA,   // analog bass drum: freq, tone, decay, fm
    V_BDS,   // synthetic bass drum: freq, tone, decay, dirt
    V_SDS,   // synthetic snare: freq, decay, snappy, fm
    V_SDA,   // analog snare: freq, tone, decay, snappy
    V_HAT,   // hi-hat: freq, tone, decay, noisiness
    V_CLAP,  // clap: band freq, tail seconds, burst spacing, resonance
    V_SWEEP, // pitched sweep: start Hz, end Hz, sweep seconds, decay seconds
    V_NOISE, // filtered noise: band freq, resonance, attack s, decay s
    V_BELL,  // two squares into a band-pass: f1, f2, band freq, decay s
    V_RIM,   // two pings + click: f1, f2, decay s, click amount
};

struct VSpec
{
    VType type;
    float a, b, c, d;
    float secs;
};

struct KitSpec
{
    const char* name;
    const char* sound[kKitSounds];
    VSpec       v[kKitSounds];
    float       drive; // 1 = clean
    int         bits;  // 16 = clean
    int         hold;  // 1 = clean, n = sample-and-hold every n samples
    float       tone;  // 1 = open, smaller = darker one-pole low-pass
};

const KitSpec kKits[] = {
    {"808",
     {"kick", "snare", "closed hat", "open hat", "clap", "tom", "rim", "cowbell"},
     {{V_BDA, 48, .45f, .65f, .38f, .9f},
      {V_SDS, 190, .5f, .7f, .2f, .5f},
      {V_HAT, 3200, .65f, .35f, .4f, .12f},
      {V_HAT, 3200, .65f, .85f, .4f, .9f},
      {V_CLAP, 1150, .09f, .01f, .35f, .6f},
      {V_BDA, 105, .6f, .55f, .6f, .8f},
      {V_RIM, 1720, 460, .006f, .4f, .1f},
      {V_BELL, 540, 800, 2600, .11f, .5f}},
     1.f, 16, 1, 1.f},
    {"909",
     {"kick", "snare", "closed hat", "open hat", "clap", "tom", "rim", "ride"},
     {{V_BDS, 55, .5f, .45f, .3f, .7f},
      {V_SDA, 220, .6f, .4f, .8f, .45f},
      {V_HAT, 4800, .85f, .25f, .8f, .1f},
      {V_HAT, 4800, .85f, .75f, .8f, .7f},
      {V_CLAP, 1500, .12f, .008f, .3f, .6f},
      {V_BDS, 140, .6f, .4f, .1f, .6f},
      {V_RIM, 2100, 600, .005f, .6f, .08f},
      {V_NOISE, 7000, .2f, .001f, .35f, .9f}},
     1.2f, 16, 1, 1.f},
    {"lofi",
     {"kick", "snare", "closed hat", "open hat", "clap", "tom", "rim", "shaker"},
     {{V_BDA, 52, .3f, .55f, .3f, .7f},
      {V_SDA, 180, .3f, .5f, .6f, .45f},
      {V_HAT, 2600, .4f, .3f, .3f, .1f},
      {V_HAT, 2600, .4f, .7f, .3f, .6f},
      {V_CLAP, 1000, .1f, .011f, .3f, .5f},
      {V_SWEEP, 160, 90, .05f, .25f, .6f},
      {V_RIM, 1400, 380, .008f, .3f, .1f},
      {V_NOISE, 6000, .3f, .02f, .06f, .25f}},
     1.6f, 9, 3, .4f},
    {"electro",
     {"kick", "snare", "closed hat", "open hat", "clap", "zap", "blip", "laser"},
     {{V_BDS, 45, .3f, .7f, .6f, .9f},
      {V_SDS, 240, .35f, .9f, .5f, .4f},
      {V_HAT, 7000, .9f, .2f, .9f, .08f},
      {V_HAT, 7000, .9f, .6f, .9f, .5f},
      {V_CLAP, 1300, .25f, .012f, .4f, .8f},
      {V_SWEEP, 2400, 120, .03f, .15f, .4f},
      {V_SWEEP, 2000, 1990, .5f, .03f, .12f},
      {V_SWEEP, 5000, 300, .08f, .2f, .5f}},
     1.3f, 16, 1, 1.f},
    {"minimal",
     {"kick", "click snare", "tick", "open tick", "clap", "wood", "click", "shaker"},
     {{V_BDA, 58, .2f, .3f, .15f, .4f},
      {V_NOISE, 3000, .4f, .001f, .05f, .2f},
      {V_NOISE, 9000, .5f, .0005f, .015f, .06f},
      {V_NOISE, 8000, .4f, .001f, .12f, .4f},
      {V_CLAP, 1800, .05f, .006f, .5f, .3f},
      {V_SWEEP, 900, 800, .01f, .03f, .15f},
      {V_RIM, 3000, 1200, .002f, .8f, .05f},
      {V_NOISE, 7000, .3f, .03f, .05f, .2f}},
     1.f, 16, 1, 1.f},
    {"industrial",
     {"kick", "snare", "metal tick", "metal open", "clap", "drop", "hit", "metal"},
     {{V_BDS, 40, .7f, .6f, 1.f, .8f},
      {V_SDA, 160, .8f, .6f, 1.f, .6f},
      {V_BELL, 3100, 4700, 8000, .03f, .12f},
      {V_BELL, 3100, 4700, 7000, .3f, .8f},
      {V_CLAP, 900, .2f, .015f, .6f, .6f},
      {V_SWEEP, 300, 60, .1f, .4f, .9f},
      {V_RIM, 800, 2900, .02f, 1.f, .2f},
      {V_BELL, 1250, 1789, 3000, .4f, .9f}},
     3.5f, 12, 1, 1.f},
};
constexpr int kNumKits = sizeof(kKits) / sizeof(kKits[0]);

/** One pass over a sound. Writes int16 when out != nullptr, at `gain`
 *  relative to the raw signal. Returns the raw (pre-gain) peak, or the
 *  written peak in int16 units when writing. */
float RenderPass(const KitSpec& kit, const VSpec& v, float sr, uint32_t n,
                 float gain, int16_t* out, uint32_t seed)
{
    srand(seed);
    uint32_t noise = seed * 2654435761u + 1;

    AnalogBassDrum     bda;
    SyntheticBassDrum  bds;
    SyntheticSnareDrum sds;
    AnalogSnareDrum    sda;
    HiHat<>            hh;
    Svf                bp;

    switch(v.type)
    {
        case V_BDA:
            bda.Init(sr);
            bda.SetFreq(v.a);
            bda.SetTone(v.b);
            bda.SetDecay(v.c);
            bda.SetAccent(0.9f);
            bda.SetAttackFmAmount(v.d);
            bda.SetSelfFmAmount(v.d);
            break;
        case V_BDS:
            bds.Init(sr);
            bds.SetFreq(v.a);
            bds.SetTone(v.b);
            bds.SetDecay(v.c);
            bds.SetDirtiness(v.d);
            bds.SetFmEnvelopeAmount(0.6f);
            bds.SetFmEnvelopeDecay(0.3f);
            bds.SetAccent(0.9f);
            break;
        case V_SDS:
            sds.Init(sr);
            sds.SetFreq(v.a);
            sds.SetDecay(v.b);
            sds.SetSnappy(v.c);
            sds.SetFmAmount(v.d);
            sds.SetAccent(0.8f);
            break;
        case V_SDA:
            sda.Init(sr);
            sda.SetFreq(v.a);
            sda.SetTone(v.b);
            sda.SetDecay(v.c);
            sda.SetSnappy(v.d);
            sda.SetAccent(0.8f);
            break;
        case V_HAT:
            hh.Init(sr);
            hh.SetFreq(v.a);
            hh.SetTone(v.b);
            hh.SetDecay(v.c);
            hh.SetNoisiness(v.d);
            hh.SetAccent(0.9f);
            break;
        case V_CLAP:
        case V_NOISE:
        case V_BELL:
            bp.Init(sr);
            bp.SetDrive(0.f);
            bp.SetFreq(v.type == V_BELL ? v.c : v.a);
            bp.SetRes(v.type == V_CLAP ? v.d : (v.type == V_NOISE ? v.b : 0.25f));
            break;
        default: break;
    }

    float peak = 0.f, ph1 = 0.f, ph2 = 0.f, held = 0.f, lp = 0.f;
    float sweep_ph = 0.f;
    for(uint32_t i = 0; i < n; i++)
    {
        float t    = i / sr;
        bool  trig = (i == 0);
        float x    = 0.f;
        switch(v.type)
        {
            case V_BDA: x = bda.Process(trig); break;
            case V_BDS: x = bds.Process(trig); break;
            case V_SDS: x = sds.Process(trig); break;
            case V_SDA: x = sda.Process(trig); break;
            case V_HAT: x = hh.Process(trig); break;
            case V_CLAP:
            {
                float w   = RandF(noise) * 2.f - 1.f;
                float env = 0.f;
                if(t < 3.f * v.c)
                    env = expf(-fmodf(t, v.c) / 0.0025f);
                else
                    env = 0.9f * expf(-(t - 3.f * v.c) / v.b);
                bp.Process(w * env);
                x = bp.Band();
                break;
            }
            case V_SWEEP:
            {
                float f = v.b + (v.a - v.b) * expf(-t / v.c);
                sweep_ph += f / sr;
                if(sweep_ph >= 1.f)
                    sweep_ph -= 1.f;
                x = sinf(kTwoPi * sweep_ph) * expf(-t / v.d);
                if(t < 0.0015f)
                    x += (RandF(noise) * 2.f - 1.f) * 0.3f * (1.f - t / 0.0015f);
                break;
            }
            case V_NOISE:
            {
                float w   = RandF(noise) * 2.f - 1.f;
                float att = v.c > 0.f && t < v.c ? t / v.c : 1.f;
                float env = att * expf(-(t > v.c ? t - v.c : 0.f) / v.d);
                bp.Process(w * env);
                x = bp.Band();
                break;
            }
            case V_BELL:
            {
                ph1 += v.a / sr;
                ph2 += v.b / sr;
                if(ph1 >= 1.f)
                    ph1 -= 1.f;
                if(ph2 >= 1.f)
                    ph2 -= 1.f;
                float sq  = (ph1 < 0.5f ? 1.f : -1.f) + (ph2 < 0.5f ? 1.f : -1.f);
                float env = 0.65f * expf(-t / 0.012f) + 0.35f * expf(-t / v.d);
                bp.Process(sq * env);
                x = bp.Band();
                break;
            }
            case V_RIM:
            {
                float p1 = sinf(kTwoPi * v.a * t) * expf(-t / v.c);
                float p2 = sinf(kTwoPi * v.b * t) * expf(-t / (2.f * v.c));
                float c  = (RandF(noise) * 2.f - 1.f) * expf(-t / 0.0015f);
                x        = 0.7f * p1 + 0.5f * p2 + v.d * 0.5f * c;
                break;
            }
        }

        if(!out)
        {
            float a = fabsf(x);
            if(a > peak)
                peak = a;
            continue;
        }

        /* Kit colour, on a signal normalized to about +-1 */
        float y = x * gain;
        if(kit.drive > 1.f)
            y = SoftClip(y * kit.drive) / SoftClip(kit.drive);
        if(kit.hold > 1)
        {
            if(i % kit.hold == 0)
                held = y;
            y = held;
        }
        if(kit.bits < 16)
        {
            float q = (float)(1 << (kit.bits - 1));
            y       = roundf(y * q) / q;
        }
        if(kit.tone < 1.f)
        {
            lp += (y - lp) * kit.tone;
            y = lp;
        }
        float s = y * 29000.f;
        if(s > 32767.f)
            s = 32767.f;
        if(s < -32767.f)
            s = -32767.f;
        out[i] = (int16_t)s;
        if(fabsf(s) > peak)
            peak = fabsf(s);
    }
    return peak;
}
} // namespace

int Engine::NumBuiltInKits()
{
    return kNumKits;
}

const char* Engine::BuiltInKitName(int kit)
{
    return (kit >= 0 && kit < kNumKits) ? kKits[kit].name : "?";
}

const char* Engine::BuiltInSoundName(int kit, int track)
{
    if(kit < 0 || kit >= kNumKits || track < 0 || track >= kTracks)
        return "?";
    return kKits[kit].sound[track % kKitSounds]; // page 2 repeats the kit
}

void Engine::RenderBuiltInKit(int kit)
{
    for(int t = 0; t < kTracks; t++)
        RenderBuiltInSound(t, kit);
}

void Engine::RenderBuiltInSound(int t, int kit)
{
    if(t < 0 || t >= kTracks)
        return;
    Slot& sl = slots_[t];
    if(!sl.data || !sl.capacity)
        return;
    sl.length  = 0; // hide while writing
    uint32_t n = RenderBuiltInTo(t, kit, sl.data, sl.capacity);
    sl.rate    = 1.f;
    sl.length  = n;
}

uint32_t Engine::RenderBuiltInTo(int t, int kit, int16_t* dst, uint32_t cap)
{
    if(t < 0 || t >= kTracks || !dst || cap < 64)
        return 0;
    if(kit < 0 || kit >= kNumKits)
        kit = 0;
    const KitSpec& k = kKits[kit];
    const VSpec&   v = k.v[t % kKitSounds];
    uint32_t       n = (uint32_t)(v.secs * sr_);
    if(n > cap)
        n = cap;
    if(n < 64)
        n = 64;

    uint32_t seed   = 1234u + 77u * (uint32_t)kit + (uint32_t)t;
    float    raw_pk = RenderPass(k, v, sr_, n, 0.f, nullptr, seed);
    float    gain   = raw_pk > 1e-9f ? 1.f / raw_pk : 1.f;
    RenderPass(k, v, sr_, n, gain, dst, seed);

    /* Fade the last 5 ms so nothing clicks at the end */
    uint32_t fade = (uint32_t)(0.005f * sr_);
    if(fade > n)
        fade = n;
    for(uint32_t i = 0; i < fade; i++)
    {
        uint32_t j = n - 1 - i;
        dst[j] = (int16_t)(dst[j] * (float)i / fade);
    }

    /* Normalize to about -1 dBFS after the colour stage */
    int32_t peak = 1;
    for(uint32_t i = 0; i < n; i++)
    {
        int32_t a = dst[i] < 0 ? -dst[i] : dst[i];
        if(a > peak)
            peak = a;
    }
    float g = 29000.f / peak;
    for(uint32_t i = 0; i < n; i++)
        dst[i] = (int16_t)(dst[i] * g);
    return n;
}

/* ------------------------------------------------------------------ */
/* Audio side                                                           */

float Engine::BaseStepLength() const
{
    float bpm = global.bpm;
    if(bpm < 60.f)
        bpm = 60.f;
    if(bpm > 200.f)
        bpm = 200.f;
    return sr_ * 15.f / bpm; // one sixteenth
}

float Engine::StepLength(int s) const
{
    float sw = Clamp01(global.swing) * 0.5f; // up to 75 % swing
    float b  = BaseStepLength();
    return (s & 1) ? b * (1.f - sw) : b * (1.f + sw);
}

void Engine::HandleRequests()
{
    if(panic_req_)
    {
        panic_req_ = false;
        for(int t = 0; t < kTracks; t++)
        {
            voices_[t][0].active = false;
            voices_[t][1].active = false;
            acid_[t].Reset();
            acid_tie_[t]  = false;
            acid_gate_[t] = 0;
            live_n_[t]    = 0;
        }
        dly_kill_       = dly_len_;
        fx_mix_         = 0.f;
        fx_mix_target_  = 0.f;
        cap_mode_       = kCapNone;
        cap_writing_    = true;
        cap_fx_         = -1;
        click_env_      = 0.f;
    }
    if(stop_req_)
    {
        stop_req_       = false;
        playing_        = false;
        step_           = 0;
        queued_pattern_ = -1;
        if(lr_mode_ == kRecShot)
            ; // a one-shot doesn't care about the transport
        else if(lr_state_ == 2 && lr_mode_ == kRecOverdub)
        {
            lr_state_      = 0;
            lr_done_track_ = lr_track_;
            lr_done_bars_  = lr_bars_done_;
            lr_done_mode_  = kRecOverdub;
            lr_done_       = true;
        }
        else if(lr_state_ == 2) // stopping mid-take keeps the whole bars so far
        {
            if(lr_bars_done_ > 0)
            {
                lr_pos_ = (uint32_t)(lr_bars_done_ * kSteps * BaseStepLength());
                LiveRecFinish();
            }
            else
            {
                if(!LiveRecRestore())
                    slots_[lr_track_].length = 0;
                lr_state_ = 0;
            }
        }
        else if(lr_state_ == 1)
        {
            LiveRecRestore();
            lr_state_ = 0;
        }
        for(int t = 0; t < kTracks; t++) // let 303 notes end, keep live keys
            if(live_n_[t] == 0)
            {
                acid_[t].SetGate(false);
                acid_tie_[t]  = false;
                acid_gate_[t] = 0;
            }
    }
    if(nan_reset_)
    {
        /* Something went non-finite: clear every recursive state so it
           can't stay stuck silent */
        nan_reset_ = false;
        for(int t = 0; t < kTracks; t++)
        {
            filt_[t].Init(sr_);
            filt_[t].SetDrive(0.f);
            last_filter_[t] = -1.f;
            acid_[t].Reset();
            acid_gate_[t] = 0;
        }
        dj_l_.Init(sr_);
        dj_r_.Init(sr_);
        dj_l_.SetDrive(0.f);
        dj_r_.SetDrive(0.f);
        last_dj_ = -1.f;
        dly_fb_lp_l_ = dly_fb_lp_r_ = 0.f;
        dly_kill_    = dly_len_;
        lim_env_     = 0.f;
        click_env_   = 0.f;
#ifdef USE_DAISYSP_LGPL
        if(reverb_)
            reverb_->Init(sr_);
#endif
        if(cap_l_ && cap_r_)
        {
            cap_mode_   = kCapNone;
            fx_mix_     = 0.f;
            fx_mix_target_ = 0.f;
            memset(cap_l_, 0, sizeof(float) * cap_len_);
            memset(cap_r_, 0, sizeof(float) * cap_len_);
        }
    }
    for(int t = 0; t < kTracks; t++)
    {
        if(loop_clear_req_[t])
        {
            loop_clear_req_[t] = false;
            loop_on_[t]        = false;
        }
        if(loop_set_req_[t])
        {
            loop_set_req_[t]   = false;
            rl_len_[t]         = slots_[t].length;
            loop_bars_[t]      = loop_set_bars_[t];
            loop_bpm_[t]       = loop_set_bpm_[t] > 0.f ? loop_set_bpm_[t] : global.bpm;
            loop_rate_[t]      = 1.f;
            loop_start_bar_[t] = bar_count_ + 1; // from the top on the next bar line
            loop_off_[t]       = loop_set_off_[t] < kSteps ? loop_set_off_[t] : 0;
            rl_pos_[t]         = (double)rl_len_[t];
            tail_[t]           = 0;
            loop_on_[t]        = loop_bars_[t] > 0 && rl_len_[t] > 0;
        }
    }
    if(swap_ && !playing_)
        ApplySwap(); // stopped: no bar line to wait for
    if(lr_req_)
    {
        uint8_t r = lr_req_;
        lr_req_   = 0;
        if(r == 1 && lr_state_ == 0)
        {
            int t = lr_req_track_;
            if(t >= 0 && t < kTracks)
            {
                lr_prev_loop_  = loop_on_[t];
                lr_prev_synth_ = synth_[t];
                lr_track_  = t;
                lr_mode_   = kRecLoop;
                lr_state_  = 1;
                lr_stop_   = false;
                lr_wait_   = 0;
                loop_on_[t] = false;
                synth_[t]  = false; // a 303 track becomes a sample track
                voices_[t][0].active = voices_[t][1].active = false;
                LiveRecBuffer(t);
                if(!playing_)
                {
                    play_req_ = true; // start now: the bar line is right away
                    if(lr_countin)
                        lr_wait_ = 2; // the bar line it starts on, then the one after the count
                }
            }
        }
        else if(r == 4 && lr_state_ == 0)
        {
            int t = lr_req_track_;
            if(t >= 0 && t < kTracks && loop_on_[t] && fabsf(loop_rate_[t] - 1.f) < 0.002f)
            {
                lr_track_ = t;
                lr_mode_  = kRecOverdub;
                lr_state_ = 1;
                lr_stop_  = false;
                lr_wait_  = 0;
                if(!playing_)
                    play_req_ = true;
            }
        }
        else if(r == 5 && lr_state_ == 0)
        {
            int t = lr_req_track_;
            if(t >= 0 && t < kTracks)
            {
                lr_prev_loop_  = loop_on_[t];
                lr_prev_synth_ = synth_[t];
                lr_track_   = t;
                lr_mode_    = kRecShot;
                lr_state_   = 1;
                lr_stop_    = false;
                loop_on_[t] = false;
                synth_[t]   = false;
                voices_[t][0].active = voices_[t][1].active = false;
                LiveRecBuffer(t);
            }
        }
        else if(r == 2 && lr_state_ == 2 && lr_mode_ == kRecShot)
            ShotFinish();
        else if(r == 2 && lr_state_ == 2 && lr_mode_ == kRecOverdub)
        {
            lr_state_      = 0; // it's already in the loop
            lr_done_track_ = lr_track_;
            lr_done_bars_  = lr_bars_done_;
            lr_done_mode_  = kRecOverdub;
            lr_done_       = true;
        }
        else if(r == 2 && lr_state_ == 2)
            lr_stop_ = true;
        else if(r == 2 && lr_state_ == 1)
        {
            LiveRecRestore();
            lr_state_ = 0; // stop before it started = cancel
        }
        else if(r == 3)
        {
            if(lr_state_ != 0 && !LiveRecRestore() && lr_state_ == 2 && lr_track_ >= 0
               && lr_mode_ != kRecOverdub)
                slots_[lr_track_].length = 0; // half a take is no use
            lr_state_ = 0;
        }
    }
    if(play_req_)
    {
        play_req_ = false;
        if(!playing_)
        {
            bar_count_ = 0xFFFFFFFFu; // step 0 makes it bar 0
            for(int t = 0; t < kTracks; t++)
            {
                loop_start_bar_[t] = 0;
                if(loop_on_[t] && loop_off_[t]) // starts mid-bar: its tail fills the bar up to there
                {
                    double back = (double)loop_off_[t] * BaseStepLength() * loop_rate_[t];
                    rl_pos_[t]  = back < (double)rl_len_[t] ? (double)rl_len_[t] - back : 0.0;
                }
            }
            playing_      = true;
            clock_step_   = kSteps - 1;
            to_next_step_ = 0.f; // fires step 0 on the next sample
            since_step_   = 0;
            if(queued_pattern_ >= 0)
            {
                pattern_        = queued_pattern_;
                queued_pattern_ = -1;
            }
            for(int t = 0; t < kTracks; t++)
                skip_once_[t] = -1;
        }
    }
    for(int t = 0; t < kTracks; t++)
    {
        if(synth_[t] != synth_prev_[t])
        {
            synth_prev_[t] = synth_[t];
            acid_[t].Reset();
            acid_tie_[t]  = false;
            acid_gate_[t] = 0;
            live_n_[t]    = 0;
            voices_[t][0].active = false;
            voices_[t][1].active = false;
        }
        if(lock_req_[t] && !locked_[t])
        {
            voices_[t][0].active = false;
            voices_[t][1].active = false;
            locked_[t]           = true;
        }
        else if(!lock_req_[t] && locked_[t])
        {
            locked_[t] = false;
        }
    }
}

void Engine::UpdateBlockParams(size_t size)
{
    bool any_solo = false;
    for(int t = 0; t < kTracks; t++)
    {
        if(mute_latch_ || !playing_ || quant_mute == 0)
        {
            mute_eff_[t] = track[t].mute;
            solo_eff_[t] = track[t].solo;
        }
        any_solo |= solo_eff_[t];
    }
    mute_latch_ = false;
    if(fx_latch_ || !playing_ || quant_fx == 0)
        for(int f = 0; f < kNumPerfFx; f++)
            fx_held_[f] = fx_want_[f];
    fx_latch_ = false;

    /* Crossfader: tracks on A fade out to the right, tracks on B fade out
       to the left, both full in the middle; AB tracks ignore it */
    const float xf      = Clamp01(global.xfader);
    const float side[3] = {1.f, fminf(1.f, 2.f * (1.f - xf)), fminf(1.f, 2.f * xf)};

    /* Set transition over the last bar of the old set */
    const float tph  = SwapPhase();
    const bool  echo = swap_state_ == 2 && trans_ == kTransEcho;
    float       dry  = 1.f;
    if(echo && tph > 0.5f)
        dry = 1.f - (tph - 0.5f) * 2.f;
    trans_tape_ = swap_state_ == 2 && trans_ == kTransTape && tph >= 0.5f;

    for(int t = 0; t < kTracks; t++)
    {
        const TrackParams& p = track[t];

        const float rg = side[p.xfade < 3 ? p.xfade : 0];
        float g = p.vol * p.vol * rg * dry;
        if(mute_eff_[t] || (any_solo && !solo_eff_[t]))
            g = 0.f;
        float pan = Clamp01(p.pan) * kHalfPi;
        gain_l_[t].Set(cosf(pan) * kSqrt2 * g, size);
        gain_r_[t].Set(sinf(pan) * kSqrt2 * g, size);
        send_rev_[t].Set(p.rev * p.rev * rg, size);
        float ds = p.dly * p.dly;
        if(echo && !(mute_eff_[t] || (any_solo && !solo_eff_[t])))
            ds = fmaxf(ds, 0.7f * tph * p.vol);
        send_dly_[t].Set(ds * rg, size);

        if(synth_prev_[t])
        {
            acid_[t].SetParams(p.acid);
            acid_[t].SetBend(bend_st[t]);
        }
        if(loop_on_[t]) // follows tempo changes (varispeed)
            loop_rate_[t] = loop_bpm_[t] > 0.f ? global.bpm / loop_bpm_[t] : 1.f;

        float st        = roundf((Clamp01(p.pitch) - 0.5f) * 24.f);
        pitch_ratio_[t] = powf(2.f, (st + bend_st[t]) / 12.f) * slots_[t].rate;

        float d = Clamp01(p.drive);
        if(d < 0.02f)
        {
            drive_gain_[t] = 1.f;
            drive_comp_[t] = 1.f;
        }
        else
        {
            drive_gain_[t] = 1.f + d * d * 24.f;
            drive_comp_[t] = DriveComp(drive_gain_[t]);
        }

        if(p.filter != last_filter_[t] || p.reso != last_reso_[t])
        {
            last_filter_[t] = p.filter;
            last_reso_[t]   = p.reso;
            float freq;
            int   mode  = FilterMap(Clamp01(p.filter), freq);
            filt_on_[t] = mode != 0;
            filt_hp_[t] = mode == 2;
            if(mode)
            {
                filt_[t].SetFreq(freq);
                filt_[t].SetRes(Clamp01(p.reso) * 0.92f);
            }
        }
    }

    int di = (int)(Clamp01(global.dly_time) * 6.f);
    if(di > 5)
        di = 5;
    dly_target_ = kDelaySteps[di] * BaseStepLength();
    if(dly_target_ > (float)dly_len_ - 4.f)
        dly_target_ = (float)dly_len_ - 4.f;
    dly_fb_ = Clamp01(global.dly_fb) * 0.92f;
    if(echo)
        dly_fb_ = fmaxf(dly_fb_, 0.5f + 0.35f * tph);
    if(echo_post_ > 0.f) // the old set's echoes ring out over a bar or two
    {
        dly_fb_ = fmaxf(dly_fb_, 0.85f * echo_post_);
        echo_post_ -= (float)size / (2.f * kSteps * BaseStepLength());
    }

#ifdef USE_DAISYSP_LGPL
    if(reverb_)
    {
        float tone = Clamp01(global.rev_tone);
        reverb_->SetFeedback(0.6f + Clamp01(global.rev_size) * 0.37f);
        reverb_->SetLpFreq(800.f + tone * tone * 15000.f);
    }
#endif

    fx_return_.Set(Clamp01(global.fx_return), size);
    float m = Clamp01(global.master);
    master_gain_.Set(m * m * 1.3f, size);

    float md = Clamp01(global.drive);
    if(md < 0.02f)
    {
        master_drive_      = 1.f;
        master_drive_comp_ = 1.f;
    }
    else
    {
        master_drive_      = 1.f + md * md * 12.f;
        master_drive_comp_ = DriveComp(master_drive_);
    }

    float djf = global.djfilter;
    if(swap_state_ == 2 && trans_ == kTransFilter)
        djf = djf + (0.04f - djf) * tph * (2.f - tph); // closes, faster at first
    if(djf != last_dj_)
    {
        last_dj_ = djf;
        float freq;
        dj_mode_ = FilterMap(Clamp01(djf), freq);
        if(dj_mode_)
        {
            dj_l_.SetFreq(freq);
            dj_r_.SetFreq(freq);
            dj_l_.SetRes(0.35f);
            dj_r_.SetRes(0.35f);
        }
    }
}

void Engine::StartVoice(int t, float vel, float rate)
{
    Slot& sl = slots_[t];
    if(locked_[t] || lock_req_[t])
        return;
    uint32_t len = sl.length;
    if(len < 4 || !sl.data)
        return;

    /* Fade the old voice out over 2 ms instead of cutting it */
    Voice& old = voices_[t][cur_voice_[t]];
    if(old.active)
        old.fade_step = 1.f / (0.002f * sr_);

    cur_voice_[t] ^= 1;
    Voice&             v = voices_[t][cur_voice_[t]];
    const TrackParams& p = track[t];

    uint32_t off = (uint32_t)(Clamp01(p.start) * 0.95f * len);
    v.dir        = p.reverse >= 0.5f ? -1 : 1;
    v.idx        = v.dir > 0 ? (int32_t)off : (int32_t)(len - 1 - off);
    v.frac       = 0.f;
    v.amp        = 1.f;
    v.fade       = 1.f;
    v.fade_step  = 0.f;
    v.gain       = vel;
    v.rate       = rate;
    /* Reverse playback or a start offset lands mid-waveform: soften it */
    if(off > 0 || v.dir < 0)
    {
        v.att      = 0.f;
        v.att_step = 1.f / (0.001f * sr_);
    }
    else
    {
        v.att      = 1.f;
        v.att_step = 0.f;
    }
    float d = Clamp01(p.decay);
    v.decay = d > 0.98f ? 1.f : expf(-1.f / (sr_ * 0.01f * powf(400.f, d)));
    v.active = true;

    tail_[t] = (uint32_t)(0.08f * sr_);
    trig_count_[t]++;
}

inline float Engine::RenderVoice(Voice& v, const Slot& sl, float inc)
{
    const int32_t len = (int32_t)sl.length;
    const int32_t i0  = v.idx;
    const int32_t i1  = i0 + v.dir;
    if(i0 < 0 || i0 >= len || i1 < 0 || i1 >= len)
    {
        v.active = false;
        return 0.f;
    }
    float s0 = sl.data[i0];
    float s1 = sl.data[i1];
    float s  = (s0 + (s1 - s0) * v.frac) * (1.f / 32768.f);

    v.frac += inc * v.rate;
    int32_t adv = (int32_t)v.frac;
    v.frac -= (float)adv;
    v.idx += adv * v.dir;

    float g = v.amp * v.gain;
    v.amp *= v.decay;
    if(v.att < 1.f)
    {
        g *= v.att;
        v.att += v.att_step;
    }
    if(v.fade_step > 0.f)
    {
        v.fade -= v.fade_step;
        if(v.fade <= 0.f)
        {
            v.active = false;
            return 0.f;
        }
        g *= v.fade;
    }
    if(v.amp < 0.0003f)
        v.active = false;
    return s * g;
}

void Engine::LiveHit(int t, bool record)
{
    if(loop_on_[t] || (lr_state_ != 0 && lr_track_ == t))
        return; // loops follow the bar; pads don't retrigger them
    if(synth_prev_[t])
    {
        /* A pad or audition on a 303 track: its last note, one step long */
        if(live_n_[t])
            return;
        acid_[t].Trigger(acid_last_[t], false, false);
        acid_gate_[t] = (int32_t)cur_step_len_;
        acid_tie_[t]  = false;
        trig_count_[t]++;
    }
    else
        StartVoice(t, 0.9f);
    if(record)
        RecordHit(t);
}

void Engine::RecordHit(int t)
{
    if(playing_ && record_)
    {
        /* Quantize to the nearest step. If that is the next step, write it
           and skip it once so the hit isn't played twice. */
        int target = clock_step_;
        if((float)since_step_ > cur_step_len_ * 0.5f)
        {
            target        = (clock_step_ + 1) % kSteps;
            skip_once_[t] = target;
        }
        if(patterns_[pattern_][t][target] == kStepOff)
            patterns_[pattern_][t][target] = kStepOn;
    }
}

void Engine::Click(bool downbeat)
{
    float w     = kTwoPi * (downbeat ? 1800.f : 1200.f) / sr_;
    click_coef_ = 2.f * cosf(w);
    click_y1_   = sinf(w);
    click_y2_   = 0.f;
    click_env_  = downbeat ? 0.4f : 0.25f;
}

void Engine::LiveRecStep(int s)
{
    if(s == 0)
        bar_count_++;
    for(int t = 0; t < kTracks; t++)
        if(loop_on_[t] && loop_bars_[t] > 0 && s == loop_off_[t]
           && (bar_count_ - loop_start_bar_[t]) % (uint32_t)loop_bars_[t] == 0)
            rl_pos_[t] = 0.0; // back to the top on its bar
    if(lr_mode_ == kRecShot)
        return;
    if(lr_state_ == 1)
    {
        if(lr_wait_ > 0)
        {
            if(s == 0)
                lr_wait_--;
            if(lr_wait_ > 0)
                return;
        }
        if(s != 0 && !(quant_rec == 1 && (s % 4) == 0))
            return;
        if(lr_mode_ == kRecOverdub && !loop_on_[lr_track_])
        {
            lr_state_ = 0; // the loop went away meanwhile
            return;
        }
        lr_state_     = 2;
        lr_pos_       = 0;
        lr_bars_done_ = 0;
        lr_off_       = s;
        return;
    }
    if(lr_state_ == 2 && s == lr_off_)
    {
        lr_bars_done_ = lr_bars_done_ + 1;
        const bool full = lr_len_bars && lr_bars_done_ >= lr_len_bars;
        if(lr_mode_ == kRecOverdub)
        {
            if(full)
            {
                lr_state_      = 0;
                lr_done_track_ = lr_track_;
                lr_done_bars_  = lr_bars_done_;
                lr_done_mode_  = kRecOverdub;
                lr_done_       = true;
            }
            return;
        }
        uint32_t bar_len = (uint32_t)(kSteps * BaseStepLength());
        uint32_t cap     = slots_[lr_track_].capacity;
        if(lr_stop_ || full || (uint32_t)(lr_bars_done_ + 1) * bar_len > cap)
            LiveRecFinish();
    }
}

/** A take starts on track t: point its slot at the buffer asked for. If that
 *  isn't where the track's sound is, the sound stays there untouched. */
void Engine::LiveRecBuffer(int t)
{
    Slot&    s   = slots_[t];
    int16_t* buf = lr_req_buf_;
    lr_swapped_  = false;
    if(!buf || buf == s.data)
        return;
    lr_prev_data_ = s.data;
    lr_prev_cap_  = s.capacity;
    lr_prev_len_  = s.length;
    s.length      = 0;
    __sync_synchronize();
    s.data     = buf;
    s.capacity = lr_req_cap_;
    lr_swapped_ = true;
}

/** A take that isn't kept: the track is back on what it held (true), if the
 *  take had a buffer of its own */
bool Engine::LiveRecRestore()
{
    if(!lr_swapped_ || lr_track_ < 0)
        return false;
    Slot& s     = slots_[lr_track_];
    lr_swapped_ = false;
    s.length    = 0;
    __sync_synchronize();
    s.data     = lr_prev_data_;
    s.capacity = lr_prev_cap_;
    __sync_synchronize();
    s.length = lr_prev_len_;
    /* a loop comes back in from its top on its bar, a 303 track is one again */
    const int t = lr_track_;
    synth_[t]   = lr_prev_synth_;
    if(lr_prev_loop_ && loop_bars_[t] > 0 && rl_len_[t] > 0 && rl_len_[t] <= lr_prev_len_)
    {
        rl_pos_[t]  = (double)rl_len_[t];
        tail_[t]    = 0;
        loop_on_[t] = true;
    }
    return true;
}

/** A one-shot take ends: a short fade so it doesn't click */
void Engine::ShotFinish()
{
    lr_swapped_ = false; // it's kept
    const int t = lr_track_;
    Slot&     s = slots_[t];
    uint32_t  n = lr_pos_, f = n < 240 ? n : 240;
    for(uint32_t i = 0; i < f; i++)
        s.data[n - 1 - i] = (int16_t)((int32_t)s.data[n - 1 - i] * (int32_t)i / (int32_t)f);
    s.rate         = 1.f;
    loop_on_[t]    = false;
    loop_off_[t]   = 0;
    __sync_synchronize();
    s.length       = n;
    lr_state_      = 0;
    lr_done_track_ = t;
    lr_done_bars_  = 0;
    lr_done_mode_  = kRecShot;
    lr_done_       = true;
}

void Engine::LiveRecFinish()
{
    lr_swapped_ = false; // it's kept
    const int t = lr_track_;
    Slot&     s = slots_[t];
    s.rate      = 1.f;
    s.length    = lr_pos_;
    rl_len_[t]       = lr_pos_;
    loop_bars_[t]      = lr_bars_done_;
    loop_bpm_[t]       = global.bpm;
    loop_rate_[t]      = 1.f;
    loop_start_bar_[t] = bar_count_;
    loop_off_[t]       = (uint8_t)lr_off_;
    rl_pos_[t]       = 0.0;
    tail_[t]           = 0;
    loop_on_[t]        = true;
    lr_state_          = 0;
    lr_done_track_     = t;
    lr_done_bars_      = lr_bars_done_;
    lr_done_mode_      = kRecLoop;
    lr_done_           = true;
}

void Engine::ApplySwap()
{
    const SetSwap* sw = swap_;
    if(!sw)
        return;
    memcpy(patterns_, sw->patterns, sizeof(patterns_));
    memcpy(notes_, sw->notes, sizeof(notes_));
    for(int t = 0; t < kTracks; t++)
    {
        track[t] = sw->tracks[t];
        if(sw->replace[t])
        {
            voices_[t][0].active = voices_[t][1].active = false;
            Slot& s   = slots_[t];
            s.length  = 0;
            __sync_synchronize();
            s.data     = sw->data[t] ? sw->data[t] : home_[t];
            s.capacity = sw->data[t] ? sw->length[t] : slot_capacity_;
            s.rate     = sw->rate[t];
            __sync_synchronize();
            s.length = sw->data[t] ? sw->length[t] : 0;
            if(lr_state_ != 0 && lr_track_ == t)
            {
                lr_state_   = 0;
                lr_swapped_ = false;
            }
        }
        synth_[t] = sw->synth[t];
        if(sw->loop_bars[t] && slots_[t].length)
        {
            rl_len_[t]         = slots_[t].length;
            loop_bars_[t]      = sw->loop_bars[t];
            loop_bpm_[t]       = sw->loop_bpm[t] > 0.f ? sw->loop_bpm[t] : sw->global.bpm;
            loop_rate_[t]      = 1.f;
            loop_start_bar_[t] = bar_count_ + 1;
            loop_off_[t]       = 0;
            rl_pos_[t]         = (double)rl_len_[t];
            tail_[t]           = 0;
            loop_on_[t]        = true;
        }
        else
            loop_on_[t] = false;
        live_n_[t] = 0;
    }
    global          = sw->global;
    pattern_        = sw->pattern;
    queued_pattern_ = -1;
    if(trans_ == kTransEcho)
        echo_post_ = 1.f;
    trans_      = kTransCut;
    trans_tape_ = false;
    swap_       = nullptr;
    swap_state_ = 0;
    __sync_synchronize();
    swap_done_ = true;
}

/** 0..1 through the transition bar */
float Engine::SwapPhase() const
{
    if(swap_state_ != 2)
        return 0.f;
    float p = ((float)step_ + fminf(1.f, (float)since_step_ / cur_step_len_)) / (float)kSteps;
    return p < 0.f ? 0.f : (p > 1.f ? 1.f : p);
}

void Engine::OnStep(int s)
{
    if(playing_)
    {
        if(s == 0 && swap_)
        {
            if(swap_state_ == 2 || swap_->transition == kTransCut)
                ApplySwap();
            else if(swap_state_ == 1)
            {
                swap_state_ = 2; // this bar is the transition
                trans_      = swap_->transition;
            }
        }
        LiveRecStep(s);
        if(queued_pattern_ >= 0
           && (s == 0 || quant_pat == 2 || (quant_pat == 1 && (s % 4) == 0)))
        {
            pattern_        = queued_pattern_;
            queued_pattern_ = -1;
        }
        step_ = s;
        for(int t = 0; t < kTracks; t++)
        {
            if(skip_once_[t] == s)
            {
                skip_once_[t] = -1;
                continue;
            }
            if(roll_[t])
                continue; // the roll below covers it
            if(synth_prev_[t])
            {
                AcidStep(t, s);
                continue;
            }
            if(loop_on_[t] || (lr_state_ != 0 && lr_track_ == t))
                continue; // a recorded loop plays by the bar, not by steps
            uint8_t v = patterns_[pattern_][t][s];
            if(!v)
                continue;
            float prob = track[t].prob;
            if(prob < 0.999f && RandF(rng_) >= prob)
                continue;
            StartVoice(t, v == kStepAccent ? 1.f : 0.7f);
        }
        if(quant_mute == 0 || (s % 4 == 0 && quant_mute == 1) || s == 0)
            mute_latch_ = true;
        if(quant_fx <= 1 || s % 4 == 0)
            fx_latch_ = true;
        if((global.metronome || (lr_countin && lr_state_ == 1 && lr_mode_ != kRecShot)) && (s % 4) == 0)
            Click(s == 0);
    }
    for(int t = 0; t < kTracks; t++)
        if(roll_[t])
        {
            if(synth_prev_[t])
            {
                if(live_n_[t])
                    continue;
                acid_[t].Trigger(acid_last_[t], false, false);
                acid_gate_[t] = (int32_t)(cur_step_len_ * 0.5f);
                acid_tie_[t]  = false;
                trig_count_[t]++;
            }
            else
                StartVoice(t, 0.8f);
        }
}

/* ---- 303 sequencing, like the original: the gate is open for half a
   step, a slide step holds it into the next note, which then glides
   instead of retriggering ---- */
void Engine::AcidStep(int t, int s)
{
    if(live_n_[t])
        return; // keys held: they own the voice
    uint8_t v    = patterns_[pattern_][t][s];
    float   prob = track[t].prob;
    if(v && prob < 0.999f && RandF(rng_) >= prob)
        v = kStepOff;
    if(!v)
    {
        if(acid_tie_[t]) // the slide had nothing to go to: let go
        {
            acid_[t].SetGate(false);
            acid_gate_[t] = 0;
        }
        acid_tie_[t] = false;
        return;
    }
    int  note  = GetNote(pattern_, t, s);
    bool slide = GetSlide(pattern_, t, s);
    acid_[t].Trigger(note, v == kStepAccent, acid_tie_[t]);
    acid_gate_[t] = slide ? -1 : (int32_t)(cur_step_len_ * 0.5f);
    if(acid_gate_[t] == 0)
        acid_gate_[t] = 1;
    acid_tie_[t]  = slide;
    acid_last_[t] = note;
    trig_count_[t]++;
}

void Engine::AcidRecord(int t, int note)
{
    int target = clock_step_;
    if((float)since_step_ > cur_step_len_ * 0.5f)
    {
        target        = (clock_step_ + 1) % kSteps;
        skip_once_[t] = target;
    }
    if(patterns_[pattern_][t][target] == kStepOff)
        patterns_[pattern_][t][target] = kStepOn;
    SetNote(pattern_, t, target, note);
}

void Engine::AcidLiveEvents()
{
    while(note_r_ != note_w_)
    {
        NoteEvent e = note_q_[note_r_ % kNoteQueue];
        note_r_     = note_r_ + 1;
        int t       = e.track;
        if(!synth_prev_[t])
        {
            /* Keyboard on a sample track: one-shot, pitched; note off does nothing */
            if(e.vel && e.on && !loop_on_[t] && !(lr_state_ != 0 && lr_track_ == t))
            {
                StartVoice(t, 0.25f + 0.75f * (e.vel / 127.f), exp2f((e.note - 60) / 12.f));
                RecordHit(t);
            }
            continue;
        }
        int8_t* stack = live_stack_[t];
        int&    n     = live_n_[t];
        if(e.on)
        {
            if(n >= 8)
                continue;
            bool legato = n > 0;
            stack[n++]  = e.note;
            acid_[t].Trigger(e.note, e.vel >= 100, legato); // keyboard: hard = accent
            acid_gate_[t] = -1;
            acid_tie_[t]  = false;
            acid_last_[t] = e.note;
            trig_count_[t]++;
            if(record_ && playing_)
                AcidRecord(t, e.note);
        }
        else
        {
            int j = 0;
            while(j < n && stack[j] != e.note)
                j++;
            if(j == n)
                continue;
            bool was_top = (j == n - 1);
            for(; j < n - 1; j++)
                stack[j] = stack[j + 1];
            n--;
            if(n > 0)
            {
                if(was_top) // back to the key still held, gliding
                {
                    acid_[t].SetNote(stack[n - 1]);
                    acid_last_[t] = stack[n - 1];
                }
            }
            else
            {
                acid_[t].SetGate(false);
                acid_gate_[t] = 0;
            }
        }
    }
}

void Engine::FxPress(int f)
{
    if(f <= kFxRepeat32)
    {
        uint32_t len = (uint32_t)(kRepeatSteps[f] * BaseStepLength());
        if(len >= cap_len_)
            len = cap_len_ - 1;
        if(len < 64)
            len = 64;
        if(cap_mode_ == kCapRepeat && fx_mix_target_ > 0.f)
        {
            /* Already repeating: shorten the loop we have, or keep
               capturing if the first pass is still running. */
            if(cap_have_ >= len)
            {
                loop_len_    = len;
                loop_pos_    = loop_pos_ % len;
                cap_writing_ = false;
            }
            else if(cap_writing_)
            {
                loop_len_ = len;
            }
            else
            {
                loop_start_  = cap_w_;
                loop_pos_    = 0;
                loop_len_    = len;
                cap_have_    = 0;
                cap_writing_ = true;
            }
        }
        else
        {
            cap_mode_    = kCapRepeat;
            loop_start_  = cap_w_;
            loop_pos_    = 0;
            loop_len_    = len;
            cap_have_    = 0;
            cap_writing_ = true;
        }
        cap_fx_        = f;
        fx_mix_target_ = 1.f;
    }
    else if(f == kFxTapeStop)
    {
        cap_mode_      = kCapTape;
        cap_writing_   = true;
        tape_pos_      = cap_w_;
        tape_rate_     = 1.f;
        tape_dec_      = 1.f / (8.f * BaseStepLength()); // half a bar
        cap_fx_        = f;
        fx_mix_target_ = 1.f;
    }
    else if(f == kFxReverse)
    {
        cap_mode_      = kCapReverse;
        cap_writing_   = false;
        tape_pos_      = (cap_w_ + cap_len_ - 1) % cap_len_;
        cap_fx_        = f;
        fx_mix_target_ = 1.f;
    }
}

void Engine::FxRelease(int f)
{
    if(f == cap_fx_)
    {
        fx_mix_target_ = 0.f;
        cap_fx_        = -1;
    }
}

void Engine::PerfFxEdges()
{
    if(!cap_l_ || !cap_r_)
        return;
    for(int f = 0; f < kNumPerfFx; f++)
    {
        bool h = fx_held_[f] || (f == kFxTapeStop && trans_tape_);
        if(h != fx_prev_[f])
        {
            fx_prev_[f] = h;
            if(h)
                FxPress(f);
            else
                FxRelease(f);
        }
    }
}

float Engine::CaptureRead(const float* buf, double pos) const
{
    uint32_t i0 = (uint32_t)pos;
    float    fr = (float)(pos - (double)i0);
    if(i0 >= cap_len_)
        i0 -= cap_len_;
    uint32_t i1 = i0 + 1;
    if(i1 >= cap_len_)
        i1 = 0;
    return buf[i0] + (buf[i1] - buf[i0]) * fr;
}

void Engine::ProcessPerfFx(float& l, float& r)
{
    if(!cap_l_ || !cap_r_)
        return;

    if(cap_writing_)
    {
        cap_l_[cap_w_] = l;
        cap_r_[cap_w_] = r;
        if(++cap_w_ >= cap_len_)
            cap_w_ = 0;
        if(cap_mode_ == kCapRepeat && cap_have_ < cap_len_)
            cap_have_++;
    }

    if(cap_mode_ != kCapNone)
    {
        float fl = l, fr = r;
        switch(cap_mode_)
        {
            case kCapRepeat:
            {
                uint32_t p = loop_start_ + loop_pos_;
                if(p >= cap_len_)
                    p -= cap_len_;
                fl = cap_l_[p];
                fr = cap_r_[p];
                /* 1 ms window at the loop edges */
                uint32_t e = loop_pos_ < loop_len_ - 1 - loop_pos_
                                 ? loop_pos_
                                 : loop_len_ - 1 - loop_pos_;
                if(e < 48)
                {
                    float w = e * (1.f / 48.f);
                    fl *= w;
                    fr *= w;
                }
                if(++loop_pos_ >= loop_len_)
                {
                    loop_pos_    = 0;
                    cap_writing_ = false; // loop captured, freeze it
                }
                break;
            }
            case kCapTape:
            {
                fl  = CaptureRead(cap_l_, tape_pos_);
                fr  = CaptureRead(cap_r_, tape_pos_);
                float g = tape_rate_ * 4.f;
                if(g < 1.f)
                {
                    fl *= g;
                    fr *= g;
                }
                tape_pos_ += tape_rate_;
                if(tape_pos_ >= cap_len_)
                    tape_pos_ -= cap_len_;
                tape_rate_ -= tape_dec_;
                if(tape_rate_ < 0.f)
                    tape_rate_ = 0.f;
                break;
            }
            case kCapReverse:
            {
                uint32_t p = (uint32_t)tape_pos_;
                fl         = cap_l_[p];
                fr         = cap_r_[p];
                tape_pos_ -= 1.0;
                if(tape_pos_ < 0.0)
                    tape_pos_ += cap_len_;
                break;
            }
            default: break;
        }

        /* 5 ms crossfade between live and the effect */
        const float xf = 1.f / (0.005f * sr_);
        if(fx_mix_ < fx_mix_target_)
        {
            fx_mix_ += xf;
            if(fx_mix_ > fx_mix_target_)
                fx_mix_ = fx_mix_target_;
        }
        else if(fx_mix_ > fx_mix_target_)
        {
            fx_mix_ -= xf;
            if(fx_mix_ < fx_mix_target_)
                fx_mix_ = fx_mix_target_;
        }
        l += (fl - l) * fx_mix_;
        r += (fr - r) * fx_mix_;

        if(fx_mix_ <= 0.f && fx_mix_target_ <= 0.f)
        {
            cap_mode_    = kCapNone;
            cap_writing_ = true;
        }
    }

    /* Bitcrush */
    float ct = fx_held_[kFxCrush] ? 1.f : 0.f;
    crush_mix_ += (ct - crush_mix_) * 0.005f;
    if(crush_mix_ > 0.001f)
    {
        float cl = crush_l_.Process(l);
        float cr = crush_r_.Process(r);
        l += (cl - l) * crush_mix_;
        r += (cr - r) * crush_mix_;
    }

    /* 1/16 trance gate */
    float gt = 1.f;
    if(fx_held_[kFxGate])
        gt = ((float)since_step_ < cur_step_len_ * 0.5f) ? 1.f : 0.f;
    gate_gain_ += (gt - gate_gain_) * 0.01f;
    l *= gate_gain_;
    r *= gate_gain_;
}

void Engine::Process(float* out_l, float* out_r, size_t size)
{
    HandleRequests();
    UpdateBlockParams(size);
    PerfFxEdges();

    for(int t = 0; t < kTracks; t++)
    {
        uint8_t req = live_req_[t];
        if(req)
        {
            live_req_[t] = 0;
            LiveHit(t, req == 1);
        }
    }
    AcidLiveEvents();

    /* ---- following an outside clock: compare, then nudge or jump ---- */
    if(ext_req_)
    {
        ext_req_ = false;
        if(playing_)
        {
            const float b   = BaseStepLength();
            const float bar = b * kSteps;
            // where we are in the bar, in samples (real time since the bar line, swing included)
            float here = (float)(clock_step_ / 2) * 2.f * b + ((clock_step_ & 1) ? StepLength(clock_step_ - 1) : 0.f)
                         + (cur_step_len_ - to_next_step_);
            float there = ext_pos_ * b;
            float err   = there - here; // + = we are behind
            while(err > bar * 0.5f)
                err -= bar;
            while(err < -bar * 0.5f)
                err += bar;
            if(fabsf(err) > b * 0.5f)
            {
                // far off (start, locate, a tempo jump): go there
                while(there >= bar)
                    there -= bar;
                while(there < 0.f)
                    there += bar;
                float acc = 0.f;
                int   s   = 0;
                for(; s < kSteps - 1; s++)
                {
                    float l = StepLength(s);
                    if(there < acc + l)
                        break;
                    acc += l;
                }
                clock_step_   = s;
                cur_step_len_ = StepLength(s);
                to_next_step_ = acc + cur_step_len_ - there;
                since_step_   = (uint32_t)(there - acc);
                ext_err_      = 0.f;
                ext_slew_     = 0.f;
            }
            else
            {
                ext_err_ += 0.25f * (err - ext_err_);
                // take a tenth of the error out per sixteenth: gone in a second or two, tempo moves < 1 %
                float slew = ext_err_ * 0.1f / b;
                if(slew > 0.01f)
                    slew = 0.01f;
                if(slew < -0.01f)
                    slew = -0.01f;
                ext_slew_ = slew;
            }
        }
    }

    for(size_t i = 0; i < size; i++)
    {
        /* ---- clock ---- */
        to_next_step_ -= 1.f + ext_slew_;
        since_step_++;
        if(to_next_step_ <= 0.f)
        {
            clock_step_ = (clock_step_ + 1) % kSteps;
            cur_step_len_ = StepLength(clock_step_);
            to_next_step_ += cur_step_len_;
            since_step_ = 0;
            OnStep(clock_step_);
        }

        /* ---- audio input: meter, live recording ---- */
        float in = in_l_ ? in_l_[i] * in_gain : 0.f;
        {
            float a = fabsf(in);
            in_peak_ = a > in_peak_ ? a : in_peak_ * 0.99995f;
            if(lr_state_ == 1 && lr_mode_ == kRecShot && a > 0.02f)
            {
                lr_state_ = 2; // a one-shot starts with the sound
                lr_pos_   = 0;
            }
        }
        if(lr_state_ == 2 && lr_mode_ != kRecOverdub)
        {
            Slot& s = slots_[lr_track_];
            if(lr_pos_ < s.capacity)
            {
                float v       = in * 32767.f;
                v             = v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v);
                s.data[lr_pos_++] = (int16_t)v;
            }
            else if(lr_mode_ == kRecShot)
                ShotFinish(); // the slot is full
        }

        /* ---- tracks ---- */
        float L = 0.f, R = 0.f, rev_l = 0.f, rev_r = 0.f, dly_in = 0.f;
        for(int t = 0; t < kTracks; t++)
        {
            float mono = 0.f;
            if(synth_prev_[t])
            {
                /* 303: its own filter and drive, no sample chain */
                AcidVoice& a = acid_[t];
                if(acid_gate_[t] > 0 && --acid_gate_[t] == 0)
                    a.SetGate(false);
                if(a.Idle())
                {
                    a.Sleep();
                    gain_l_[t].Next(); // keep the ramps in step
                    gain_r_[t].Next();
                    send_rev_[t].Next();
                    send_dly_[t].Next();
                    continue;
                }
                mono = a.Process();
            }
            else
            {
                Voice* vv  = voices_[t];
                if(loop_on_[t])
                {
                    /* recorded loop: locked to the bar, not to steps */
                    double p = rl_pos_[t];
                    /* (never past the memory the slot has now: a load may have
                       moved it a block before the loop is told to stop) */
                    if(!playing_ || p >= (double)rl_len_[t] - 1.0
                       || p >= (double)slots_[t].capacity - 1.0)
                    {
                        if(!filt_on_[t] || tail_[t] == 0)
                            continue;
                        tail_[t]--;
                    }
                    else
                    {
                        int16_t* d  = slots_[t].data;
                        uint32_t k  = (uint32_t)p;
                        float    fr = (float)(p - (double)k);
                        mono = (d[k] + (d[k + 1] - d[k]) * fr) * (1.f / 32768.f);
                        if(lr_state_ == 2 && lr_mode_ == kRecOverdub && lr_track_ == t)
                        {
                            float v = (float)d[k] + in * 32767.f; // on top, in place
                            d[k]    = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
                        }
                        rl_pos_[t] = p + loop_rate_[t];
                        tail_[t]     = (uint32_t)(sr_ * 0.5f);
                    }
                }
                else
                {
                    bool any = vv[0].active || vv[1].active;
                    if(!any)
                    {
                        if(!filt_on_[t] || tail_[t] == 0)
                            continue;
                        tail_[t]--;
                    }
                    float inc = pitch_ratio_[t];
                    if(vv[0].active)
                        mono += RenderVoice(vv[0], slots_[t], inc);
                    if(vv[1].active)
                        mono += RenderVoice(vv[1], slots_[t], inc);
                }

                if(drive_gain_[t] > 1.f)
                    mono = SoftClip(mono * drive_gain_[t]) * drive_comp_[t];
                if(filt_on_[t])
                {
                    filt_[t].Process(mono);
                    mono = filt_hp_[t] ? filt_[t].High() : filt_[t].Low();
                }
            }
            float gl = gain_l_[t].Next();
            float gr = gain_r_[t].Next();
            float sl = mono * gl;
            float sr = mono * gr;
            L += sl;
            R += sr;
            {
                float a = fabsf(sl), b = fabsf(sr);
                if(b > a)
                    a = b;
                if(a > peak_[t])
                    peak_[t] = a;
            }
            float rs = send_rev_[t].Next();
            float ds = send_dly_[t].Next();
            rev_l += sl * rs;
            rev_r += sr * rs;
            dly_in += (sl + sr) * 0.5f * ds;
        }

        /* ---- ping-pong delay ---- */
        float dl = 0.f, dr = 0.f;
        if(dly_l_ && dly_r_)
        {
            dly_time_smooth_ += (dly_target_ - dly_time_smooth_) * 0.0005f;
            float rp = (float)dly_w_ - dly_time_smooth_;
            if(rp < 0.f)
                rp += (float)dly_len_;
            uint32_t r0 = (uint32_t)rp;
            float    fr = rp - (float)r0;
            if(r0 >= dly_len_)
                r0 = 0;
            uint32_t r1 = r0 + 1;
            if(r1 >= dly_len_)
                r1 = 0;
            dl = dly_l_[r0] + (dly_l_[r1] - dly_l_[r0]) * fr;
            dr = dly_r_[r0] + (dly_r_[r1] - dly_r_[r0]) * fr;

            /* Damping in the loop so repeats get darker */
            dly_fb_lp_l_ += (dl - dly_fb_lp_l_) * 0.35f;
            dly_fb_lp_r_ += (dr - dly_fb_lp_r_) * 0.35f;
            float wl = dly_in + dly_fb_lp_r_ * dly_fb_;
            float wr = dly_fb_lp_l_ * dly_fb_;
            if(dly_kill_)
            {
                dly_kill_--;
                wl = wr = 0.f;
                dl = dr = 0.f;
            }
            dly_l_[dly_w_] = SoftClip(wl);
            dly_r_[dly_w_] = SoftClip(wr);
            if(++dly_w_ >= dly_len_)
                dly_w_ = 0;
        }

        /* ---- reverb ---- */
        float rl = 0.f, rr = 0.f;
#ifdef USE_DAISYSP_LGPL
        if(reverb_)
            reverb_->Process(rev_l, rev_r, &rl, &rr);
#endif
        float fxr = fx_return_.Next();
        L += (rl + dl) * fxr;
        R += (rr + dr) * fxr;

        /* ---- performance effects ---- */
        ProcessPerfFx(L, R);

        /* ---- DJ filter on the crossfader ---- */
        if(dj_mode_)
        {
            dj_l_.Process(L);
            dj_r_.Process(R);
            if(dj_mode_ == 1)
            {
                L = dj_l_.Low();
                R = dj_r_.Low();
            }
            else
            {
                L = dj_l_.High();
                R = dj_r_.High();
            }
        }

        /* hear the input while recording, or with monitoring on */
        if(input_monitor || lr_state_ != 0)
        {
            L += in;
            R += in;
        }

        /* ---- master ---- */
        if(master_drive_ > 1.f)
        {
            L = SoftClip(L * master_drive_) * master_drive_comp_;
            R = SoftClip(R * master_drive_) * master_drive_comp_;
        }
        float mg = master_gain_.Next();
        L *= mg;
        R *= mg;

        if(click_env_ > 0.0001f)
        {
            float y   = click_coef_ * click_y1_ - click_y2_;
            click_y2_ = click_y1_;
            click_y1_ = y;
            L += y * click_env_;
            R += y * click_env_;
            click_env_ *= 0.9985f;
        }

        if(!(L == L) || !(R == R) || fabsf(L) > 1e6f || fabsf(R) > 1e6f)
        {
            L = R      = 0.f;
            if(!nan_reset_)
                nan_count++;
            nan_reset_ = true;
        }

        /* Peak limiter, instant attack, ~100 ms release */
        float pk = fabsf(L) > fabsf(R) ? fabsf(L) : fabsf(R);
        if(pk > lim_env_)
            lim_env_ = pk;
        else
            lim_env_ += (pk - lim_env_) * 0.0002f;
        if(lim_env_ > 0.95f)
        {
            float g = 0.95f / lim_env_;
            if(g < 0.89f)
                limit_count++; // more than 1 dB of gain reduction
            L *= g;
            R *= g;
        }
        if(L > 1.f)
            L = 1.f;
        if(L < -1.f)
            L = -1.f;
        if(R > 1.f)
            R = 1.f;
        if(R < -1.f)
            R = -1.f;
        out_l[i] = L;
        out_r[i] = R;
        if(fabsf(L) > peak_[kTracks])
            peak_[kTracks] = fabsf(L);
        if(fabsf(R) > peak_[kTracks + 1])
            peak_[kTracks + 1] = fabsf(R);
    }
}

} // namespace ab
