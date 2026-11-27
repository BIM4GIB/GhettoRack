#include "engine.h"
#include <algorithm>

namespace dk
{
namespace
{
constexpr float kPi         = 3.14159265358979f;
constexpr float kFadeStep   = 1.f / (0.002f * kRate); // 2 ms: start, stop and jumps without a click
constexpr float kScratchTau = 0.005f * kRate;         // scratching: the hand's place is smoothed twice, this long each,
constexpr float kScratchIn  = 0.135f;                 // (per chunk, the same 5 ms) so ticks that come in lumps give a steady speed
constexpr float kSlew       = 0.087f;                 // per chunk: speed settles in about 8 ms
constexpr float kBendDecay  = 0.9952f;                // per chunk: a nudge fades in about 150 ms
constexpr float kNudge      = 0.004f;                 // speed offset one jog tick adds
constexpr float kPeakDecay  = 0.992f;                 // per chunk: meters fall in about 90 ms
constexpr float kEqLoHz = 250.f, kEqHiHz = 2500.f;

inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/** One frame at a fractional place: 4-point Hermite. Outside the track is silence. */
inline void Read(const Track* t, double pos, float& l, float& r)
{
    const int64_t  i = (int64_t)std::floor(pos);
    const float    f = (float)(pos - (double)i);
    const int64_t  n = t->frames;
    const int16_t* p = t->pcm;
    if(f == 0.f) // on a frame: the frame itself, untouched
    {
        if(i < 0 || i >= n)
            l = r = 0;
        else
            l = p[2 * i] * (1.f / 32768.f), r = p[2 * i + 1] * (1.f / 32768.f);
        return;
    }
    float          a[4][2];
    for(int k = 0; k < 4; k++)
    {
        int64_t j = i - 1 + k;
        if(j < 0 || j >= n)
            a[k][0] = a[k][1] = 0;
        else
            a[k][0] = p[2 * j], a[k][1] = p[2 * j + 1];
    }
    float o[2];
    for(int c = 0; c < 2; c++)
    {
        float c0 = a[1][c];
        float c1 = 0.5f * (a[2][c] - a[0][c]);
        float c2 = a[0][c] - 2.5f * a[1][c] + 2.f * a[2][c] - 0.5f * a[3][c];
        float c3 = 0.5f * (a[3][c] - a[0][c]) + 1.5f * (a[1][c] - a[2][c]);
        o[c]     = ((c3 * f + c2) * f + c1) * f + c0;
    }
    l = o[0] * (1.f / 32768.f);
    r = o[1] * (1.f / 32768.f);
}
} // namespace

void Biquad::Butter(bool high, float hz)
{
    const float w = 2.f * kPi * hz / kRate, c = std::cos(w), s = std::sin(w);
    const float alpha = s / (2.f * 0.70710678f), a0 = 1.f + alpha;
    if(high)
        b0 = b2 = (1.f + c) * 0.5f / a0, b1 = -(1.f + c) / a0;
    else
        b0 = b2 = (1.f - c) * 0.5f / a0, b1 = (1.f - c) / a0;
    a1 = -2.f * c / a0;
    a2 = (1.f - alpha) / a0;
}

void Eq3::Init(float lo_hz, float hi_hz)
{
    for(int i = 0; i < 2; i++)
    {
        lp1[i].Butter(false, lo_hz), hp1[i].Butter(true, lo_hz);
        lp2[i].Butter(false, hi_hz), hp2[i].Butter(true, hi_hz);
        alp[i].Butter(false, hi_hz), ahp[i].Butter(true, hi_hz);
    }
}

void Svf::Set(float hz, float q)
{
    const float g = std::tan(kPi * Clamp(hz, 10.f, 0.45f * kRate) / kRate);
    k             = 1.f / q;
    a1            = 1.f / (1.f + g * (g + k));
    a2            = g * a1;
    a3            = g * a2;
}

float TrimGain(float k)
{
    k = Clamp(k, 0, 1);
    return k < 0.5f ? 4.f * k * k : 1.f + (k - 0.5f) * 6.f;
}
float EqGain(float k)
{
    k = Clamp(k, 0, 1);
    return k < 0.5f ? 4.f * k * k : 1.f + (k - 0.5f) * 2.f;
}
float FaderGain(float k)
{
    k = Clamp(k, 0, 1);
    return k * k;
}
float OutGain(float k)
{
    k = Clamp(k, 0, 1) * 1.25f;
    return k * k;
}
float SoftClip(float x)
{
    const float a = std::fabs(x);
    if(a <= 0.9f)
        return x;
    const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.f);
    return x < 0 ? -y : y;
}

Engine::Engine()
{
    for(auto& d : deck_)
        for(auto& e : d.eq)
            e.Init(kEqLoHz, kEqHiHz);
}

bool Engine::Post(const Ev& e)
{
    std::lock_guard<std::mutex> g(post_mu_);
    if(ev_.Push(e))
        return true;
    ev_dropped++;
    return false;
}

const Track* Engine::Trash()
{
    const Track* t = nullptr;
    return trash_.Pop(t) ? t : nullptr;
}

void Engine::Jump(Deck& d, double to)
{
    const double n = d.trk ? d.trk->frames : 0;
    to             = to < 0 ? 0 : (to > n ? n : to);
    if(d.fade <= 0 || d.jgain <= 0) // not sounding: go at once
    {
        d.pos = d.goal = d.goal_s = to;
        d.jump         = false;
        return;
    }
    d.jump    = true; // sounding: 2 ms down, go, 2 ms up
    d.jump_to = to;
}

void Engine::Handle(const Ev& e)
{
    if(e.deck < 0 || e.deck >= kDecks)
        return;
    Deck& d = deck_[e.deck];
    switch(e.type)
    {
        case kEvLoad:
            if(d.trk)
                trash_.Push(d.trk); // 16 places, emptied 50 times a second; a full ring would leak one track
            d.trk     = e.track;
            d.pos = d.cue = d.goal = d.goal_s = 0;
            d.playing = d.cue_held = d.jump = false;
            d.speed = d.bend_acc = d.fade = 0;
            d.jgain                       = 1;
            break;
        case kEvPlay:
        {
            if(!d.trk)
                break;
            bool on = e.a >= 2 ? !(d.playing) : e.a >= 1;
            if(on && d.pos >= d.trk->frames)
                break; // at the end: nothing to play
            d.playing = on;
            if(on)
                d.cue_held = false; // play pressed while cue is held: it keeps running
            break;
        }
        case kEvCue:
            if(!d.trk)
                break;
            if(e.a >= 1)
            {
                const double at = d.jump ? d.jump_to : d.pos;
                if(d.playing)
                {
                    d.playing = false; // playing: back to the cue point and wait
                    Jump(d, d.cue);
                }
                else if(std::fabs(at - d.cue) >= 1.0)
                    d.cue = at; // stopped somewhere else: this is the cue point now
                else
                    d.cue_held = true; // stopped on it: plays while held
            }
            else if(d.cue_held)
            {
                d.cue_held = false;
                Jump(d, d.cue);
            }
            break;
        case kEvSeek: Jump(d, e.d); break;
        case kEvSeekRel: Jump(d, (d.jump ? d.jump_to : d.pos) + e.d); break;
        case kEvTouch:
            d.touched = e.a >= 1;
            d.goal = d.goal_s = d.pos;
            break;
        case kEvJog:
            if(d.touched && knobs[e.deck].scratch.load(std::memory_order_relaxed) > 0.5f)
                d.goal += e.a * kJogFramesPerTick;
            else
                d.bend_acc = Clamp(d.bend_acc + e.a * kNudge, -0.5f, 0.5f);
            break;
        case kEvBend: d.bend_btn = Clamp(e.a, -0.5f, 0.5f); break;
        default: break;
    }
}

/** One deck, `frames` (up to kChunk) frames: off the record, through trim,
 *  EQ and filter. What comes out is before the fader. */
void Engine::RunDeck(int n, int frames, float* l, float* r)
{
    Deck&            d  = deck_[n];
    const DeckKnobs& k  = knobs[n];
    const auto       rx = std::memory_order_relaxed;

    const bool scratch = d.touched && k.scratch.load(rx) > 0.5f;
    const bool run     = d.trk && (d.playing || d.cue_held);
    if(!d.trk)
        d.speed = 0;
    else if(scratch)
    {
        d.goal_s += (d.goal - d.goal_s) * kScratchIn;
        d.speed = Clamp((float)((d.goal_s - d.pos) / kScratchTau), -16.f, 16.f);
    }
    else
    {
        const float v = run ? (1.f + k.rate.load(rx)) * (1.f + d.bend_acc + d.bend_btn) : 0.f;
        d.speed += (v - d.speed) * kSlew;
        if(std::fabs(v - d.speed) < 1e-4f)
            d.speed = v;
        if(d.speed == 1.f)
            d.pos = std::floor(d.pos + 0.5); // plain playing: on the frames, so nothing is lost to interpolation
    }
    d.bend_acc *= kBendDecay;
    const float fade_tgt = std::fabs(d.speed) > 1e-3f ? 1.f : 0.f;

    /* filter knob: a dead band at the centre, then low-pass to the left, high-pass to the right */
    d.filt += (Clamp(k.filter.load(rx), 0, 1) - d.filt) * 0.2f;
    const float away = std::fabs(d.filt - 0.5f);
    const float wet  = Clamp((away - 0.02f) / 0.03f, 0, 1);
    const float t    = Clamp((away - 0.02f) / 0.48f, 0, 1);
    const bool  low  = d.filt < 0.5f;
    const float hz   = low ? 18000.f * std::pow(80.f / 18000.f, t) : 20.f * std::pow(8000.f / 20.f, t);
    d.svf[0].Set(hz, 1.25f);
    d.svf[1].Set(hz, 1.25f);

    const float inv    = 1.f / frames;
    const float t_trim = TrimGain(k.gain.load(rx)), t_lo = EqGain(k.lo.load(rx));
    const float t_mid = EqGain(k.mid.load(rx)), t_hi = EqGain(k.hi.load(rx));
    const float s_trim = (t_trim - d.g_trim) * inv, s_lo = (t_lo - d.g_lo) * inv;
    const float s_mid = (t_mid - d.g_mid) * inv, s_hi = (t_hi - d.g_hi) * inv;
    const double end = d.trk ? d.trk->frames : 0;
    float        pk  = 0;

    for(int i = 0; i < frames; i++)
    {
        if(d.jump)
        {
            d.jgain -= kFadeStep;
            if(d.jgain <= 0)
            {
                d.jgain = 0;
                d.pos = d.goal = d.goal_s = d.jump_to;
                d.jump         = false;
                if(!run && !scratch)
                    d.speed = 0; // stopped: stay exactly there
            }
        }
        else if(d.jgain < 1)
            d.jgain = std::min(1.f, d.jgain + kFadeStep);
        d.fade += Clamp(fade_tgt - d.fade, -kFadeStep, kFadeStep);

        const float g  = d.fade * d.jgain;
        float       sl = 0, sr = 0;
        if(d.trk && g > 0)
        {
            Read(d.trk, d.pos, sl, sr);
            sl *= g, sr *= g;
        }
        d.pos += d.speed;
        if(d.pos >= end)
        {
            d.pos = end;
            if(!scratch)
                d.playing = d.cue_held = false; // ran off the end
        }
        else if(d.pos < 0)
            d.pos = 0;

        d.g_trim += s_trim, d.g_lo += s_lo, d.g_mid += s_mid, d.g_hi += s_hi;
        float yl = d.eq[0].Run(sl * d.g_trim, d.g_lo, d.g_mid, d.g_hi);
        float yr = d.eq[1].Run(sr * d.g_trim, d.g_lo, d.g_mid, d.g_hi);
        float lp, hp;
        d.svf[0].Run(yl, lp, hp);
        yl += wet * ((low ? lp : hp) - yl);
        d.svf[1].Run(yr, lp, hp);
        yr += wet * ((low ? lp : hp) - yr);
        l[i] = yl, r[i] = yr;
        pk = std::max(pk, std::max(std::fabs(yl), std::fabs(yr)));
    }
    d.g_trim = t_trim, d.g_lo = t_lo, d.g_mid = t_mid, d.g_hi = t_hi;
    d.peak   = std::max(d.peak * kPeakDecay, pk);
}

void Engine::Process(float* out, int frames)
{
    const auto rx = std::memory_order_relaxed;
    Ev         e;
    while(ev_.Pop(e))
        Handle(e);

    for(int off = 0; off < frames; off += kChunk)
    {
        const int n = std::min(kChunk, frames - off);
        float     l[kDecks][kChunk], r[kDecks][kChunk];
        for(int d = 0; d < kDecks; d++)
            RunDeck(d, n, l[d], r[d]);

        const float inv = 1.f / n;
        const float x   = Clamp(xfader.load(rx), 0, 1);
        const float tx[kDecks] = {std::min(1.f, 2.f * (1.f - x)), std::min(1.f, 2.f * x)};
        float       sv[kDecks], sx[kDecks], sp[kDecks];
        for(int d = 0; d < kDecks; d++)
        {
            Deck& k = deck_[d];
            sv[d]   = (FaderGain(knobs[d].volume.load(rx)) - k.g_vol) * inv;
            sx[d]   = (tx[d] - k.g_x) * inv;
            sp[d]   = ((knobs[d].pfl.load(rx) > 0.5f ? 1.f : 0.f) - k.g_pfl) * inv;
        }
        const float t_master = OutGain(master.load(rx)), t_cue = OutGain(cue_gain.load(rx));
        const float t_mix = Clamp(cue_mix.load(rx), 0, 1);
        const float s_master = (t_master - g_master_) * inv, s_cue = (t_cue - g_cue_) * inv;
        const float s_mix = (t_mix - g_mix_) * inv;
        float*      o     = out + (size_t)off * kOuts;
        float       pl = 0, pr = 0;
        for(int i = 0; i < n; i++)
        {
            float ml = 0, mr = 0, cl = 0, cr = 0;
            for(int d = 0; d < kDecks; d++)
            {
                Deck& k = deck_[d];
                k.g_vol += sv[d], k.g_x += sx[d], k.g_pfl += sp[d];
                const float g = k.g_vol * k.g_x;
                ml += l[d][i] * g, mr += r[d][i] * g;
                cl += l[d][i] * k.g_pfl, cr += r[d][i] * k.g_pfl;
            }
            g_master_ += s_master, g_cue_ += s_cue, g_mix_ += s_mix;
            o[0] = SoftClip(ml * g_master_);
            o[1] = SoftClip(mr * g_master_);
            o[2] = SoftClip((cl + g_mix_ * (ml - cl)) * g_cue_); // headphones take the master before its knob
            o[3] = SoftClip((cr + g_mix_ * (mr - cr)) * g_cue_);
            pl   = std::max(pl, std::fabs(o[0]));
            pr   = std::max(pr, std::fabs(o[1]));
            o += kOuts;
        }
        for(int d = 0; d < kDecks; d++)
        {
            Deck& k = deck_[d];
            k.g_vol = FaderGain(knobs[d].volume.load(rx)), k.g_x = tx[d];
            k.g_pfl = knobs[d].pfl.load(rx) > 0.5f ? 1.f : 0.f;
        }
        g_master_ = t_master, g_cue_ = t_cue, g_mix_ = t_mix;
        pk_l_     = std::max(pk_l_ * kPeakDecay, pl);
        pk_r_     = std::max(pk_r_ * kPeakDecay, pr);
    }

    for(int d = 0; d < kDecks; d++)
    {
        const Deck& k = deck_[d];
        DeckState&  s = state[d];
        s.pos.store(k.jump ? k.jump_to : k.pos, rx);
        s.cue.store(k.cue, rx);
        s.frames.store(k.trk ? k.trk->frames : 0, rx);
        s.track.store(k.trk ? k.trk->id : 0, rx);
        s.playing.store(k.playing, rx);
        s.cue_held.store(k.cue_held, rx);
        s.touched.store(k.touched, rx);
        s.speed.store(k.speed, rx);
        s.peak.store(k.peak, rx);
    }
    peak_l.store(pk_l_, rx);
    peak_r.store(pk_r_, rx);
}

} // namespace dk
