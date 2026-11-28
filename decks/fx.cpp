#include "fx.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dk
{
namespace
{
constexpr float  kTwoPi   = 6.28318530718f;
constexpr size_t kLine    = 2 * kRate + 4096; // the echo's longest: 2 s
constexpr float  kRamp    = 1.f / (0.010f * kRate);
constexpr float  kEchoFb  = 0.55f;            // each repeat a little over half the one before
constexpr int    kCombLen[4] = {1557, 1617, 1491, 1422}, kAllLen[2] = {556, 441}; // Freeverb's, 23 further on the right
constexpr int    kSpread  = 23;

inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
/** 0..1 round to 0..1..0 */
inline float Tri(double x)
{
    return (float)(x < 0.5 ? 2 * x : 2 - 2 * x);
}
} // namespace

const char* FxName(int kind)
{
    static const char* n[kFxCount] = {"HPF", "LPF", "FLANGER", "ECHO", "REVERB", "PHASER"};
    return kind >= 0 && kind < kFxCount ? n[kind] : "-";
}

void Fx::Init()
{
    for(int c = 0; c < 2; c++)
    {
        dl_[c].assign(kLine, 0.f);
        for(int i = 0; i < 4; i++)
            comb_[c][i].b.assign((size_t)kCombLen[i] + c * kSpread, 0.f);
        for(int i = 0; i < 2; i++)
            all_[c][i].b.assign((size_t)kAllLen[i] + c * kSpread, 0.f);
    }
}

/** Another effect: start it clean. The long delay line is not wiped (2 s of it a side, in the
 *  audio thread): filled_ says how much of it is this effect's, and the rest reads as silence. */
void Fx::Clear(int kind)
{
    kind_ = kind;
    in_ = mix_ = 0;
    lfo_       = 0;
    filled_    = 0;
    time_      = 0;
    for(int c = 0; c < 2; c++)
    {
        svf_[c].ic1 = svf_[c].ic2 = 0;
        lp_[c] = pfb_[c] = 0;
        for(float& a : ap_[c])
            a = 0;
        if(kind == kFxReverb)
        {
            for(auto& l : comb_[c])
                std::fill(l.b.begin(), l.b.end(), 0.f), l.lp = 0, l.at = 0;
            for(auto& l : all_[c])
                std::fill(l.b.begin(), l.b.end(), 0.f), l.at = 0;
        }
    }
}

void Fx::Run(float* l, float* r, int frames, bool on, int kind, float wet, float beats, double period)
{
    if(dl_[0].empty() || kind < 0 || kind >= kFxCount)
        return;
    if(kind != kind_)
    {
        if(!on && in_ <= 0 && mix_ <= 0 && kind_ >= 0 && kind_ != kFxEcho && kind_ != kFxReverb)
            kind_ = kind; // nothing of the old one left to hear
        else if(on || kind_ < 0)
            Clear(kind);
        /* else: the old one rings out first (an echo or a reverb switched off, then another chosen) */
    }
    const int k = kind_;
    if(k < 0)
        return;
    wet    = Clamp(wet, 0, 1);
    beats  = Clamp(beats, 1.f / 16, 8);
    period = period > 100 ? period : 0.5 * kRate;
    const double sweep = beats * period; // frames a turn of the LFO, or the echo's delay
    const bool   tail  = k == kFxEcho || k == kFxReverb;
    const float  tgt   = on && k == kind ? 1.f : 0.f;

    if(!tail && tgt == 0 && mix_ <= 0)
        return; // off and faded out: the sound passes untouched
    float* ch[2] = {l, r};

    if(k == kFxHpf || k == kFxLpf)
    {
        /* a resonant filter going up and down between two corners, once every `beats` */
        for(int i = 0; i < frames; i += 16)
        {
            const int   n  = std::min(16, frames - i);
            const float t  = Tri(lfo_);
            const float hz = k == kFxHpf ? 80.f * std::pow(6000.f / 80.f, t) : 16000.f * std::pow(250.f / 16000.f, t);
            svf_[0].Set(hz, 2.5f);
            svf_[1].Set(hz, 2.5f);
            for(int j = i; j < i + n; j++)
            {
                mix_ += Clamp(tgt - mix_, -kRamp, kRamp);
                const float g = mix_ * wet;
                for(int c = 0; c < 2; c++)
                {
                    float lp, hp;
                    svf_[c].Run(ch[c][j], lp, hp);
                    ch[c][j] += g * ((k == kFxHpf ? hp : lp) - ch[c][j]);
                }
            }
            lfo_ += n / sweep;
            lfo_ -= std::floor(lfo_);
        }
        return;
    }
    if(k == kFxPhaser)
    {
        for(int i = 0; i < frames; i += 16)
        {
            const int   n  = std::min(16, frames - i);
            const float hz = 300.f * std::pow(10.f, Tri(lfo_)); // 300 Hz to 3 kHz
            const float w  = std::tan(kTwoPi * 0.5f * hz / kRate), a = (w - 1) / (w + 1);
            for(int j = i; j < i + n; j++)
            {
                mix_ += Clamp(tgt - mix_, -kRamp, kRamp);
                const float g = mix_ * wet * 0.5f; // half and half is the deepest notch
                for(int c = 0; c < 2; c++)
                {
                    float x = ch[c][j] + 0.5f * pfb_[c];
                    for(float& z : ap_[c]) // six first-order all-passes
                    {
                        const float y = a * x + z;
                        z             = x - a * y;
                        x             = y;
                    }
                    pfb_[c] = x;
                    ch[c][j] += g * (x - ch[c][j]);
                }
            }
            lfo_ += n / sweep;
            lfo_ -= std::floor(lfo_);
        }
        return;
    }
    if(k == kFxFlanger)
    {
        for(int j = 0; j < frames; j++)
        {
            mix_ += Clamp(tgt - mix_, -kRamp, kRamp);
            const float g   = mix_ * wet * 0.7f; // the delayed sound added to the whole dry one: that makes the comb
            const float del = (0.001f + 0.005f * Tri(lfo_)) * kRate; // 1 to 6 ms
            const float rp  = (float)dw_ - del + (float)kLine;
            const size_t i0 = (size_t)rp % kLine, i1 = (i0 + 1) % kLine;
            const float  f  = rp - std::floor(rp);
            for(int c = 0; c < 2; c++)
            {
                const float d = filled_ > (size_t)del + 2 ? dl_[c][i0] + f * (dl_[c][i1] - dl_[c][i0]) : 0.f;
                dl_[c][dw_]   = ch[c][j] + 0.6f * d;
                ch[c][j] += g * d;
            }
            dw_ = (dw_ + 1) % kLine;
            if(filled_ < kLine)
                filled_++;
            lfo_ += 1.0 / sweep;
            lfo_ -= std::floor(lfo_);
        }
        return;
    }
    if(k == kFxEcho)
    {
        double want = sweep;
        while(want > 2.0 * kRate)
            want *= 0.5; // longer than the line: the next shorter that fits
        if(time_ <= 0)
            time_ = (float)want;
        for(int j = 0; j < frames; j++)
        {
            in_ += Clamp(tgt - in_, -kRamp, kRamp);
            time_ += Clamp((float)want - time_, -2.f, 2.f); // another length: glide there (it bends for a moment, as a tape echo does)
            const float  rp = (float)dw_ - time_ + (float)kLine;
            const size_t i0 = (size_t)rp % kLine, i1 = (i0 + 1) % kLine;
            const float  f  = rp - std::floor(rp);
            for(int c = 0; c < 2; c++)
            {
                float d = filled_ > (size_t)time_ + 2 ? dl_[c][i0] + f * (dl_[c][i1] - dl_[c][i0]) : 0.f;
                lp_[c] += 0.35f * (d - lp_[c]);
                dl_[c][dw_] = ch[c][j] * in_ + kEchoFb * lp_[c];
                ch[c][j] += wet * d;
            }
            dw_ = (dw_ + 1) % kLine;
            if(filled_ < kLine)
                filled_++;
        }
        return;
    }
    /* reverb: the room is larger with more beats */
    const float room = 0.72f + 0.25f * Clamp(std::log2(beats * 16) / 7.f, 0, 1), damp = 0.3f;
    for(int j = 0; j < frames; j++)
    {
        in_ += Clamp(tgt - in_, -kRamp, kRamp);
        const float x = (ch[0][j] + ch[1][j]) * 0.015f * in_;
        for(int c = 0; c < 2; c++)
        {
            float out = 0;
            for(auto& q : comb_[c])
            {
                const float y = q.b[q.at];
                q.lp          = y * (1 - damp) + q.lp * damp;
                q.b[q.at]     = x + q.lp * room;
                q.at          = q.at + 1 < q.b.size() ? q.at + 1 : 0;
                out += y;
            }
            for(auto& q : all_[c])
            {
                const float y = q.b[q.at];
                q.b[q.at]     = out + y * 0.5f;
                q.at          = q.at + 1 < q.b.size() ? q.at + 1 : 0;
                out           = y - out;
            }
            ch[c][j] += wet * 3.f * out;
        }
    }
}
} // namespace dk
