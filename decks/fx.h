/** The effect a deck's sound can go through, between its filter and its fader:
 *  one of six, chosen for both decks at once as on the Numark (its six FX
 *  buttons), each deck switching it in with its paddle. Two knobs for all of
 *  them: how much (wet), and beats: how long a sweep or an echo is, by the
 *  deck's tempo as it plays (120 when that is not known).
 *
 *  Switched off, the sweeping ones (filters, flanger, phaser) fade out over
 *  10 ms; echo and reverb stop taking sound in and ring out.
 *
 *  Audio thread only, after Init(). Nothing here allocates once it runs. */
#pragma once
#include "engine_parts.h"
#include <vector>

namespace dk
{
enum FxKind { kFxHpf, kFxLpf, kFxFlanger, kFxEcho, kFxReverb, kFxPhaser, kFxCount };
const char* FxName(int kind);

class Fx
{
  public:
    void Init();
    /** frames of l and r, in place. period: frames a beat at the deck's speed now. */
    void Run(float* l, float* r, int frames, bool on, int kind, float wet, float beats, double period);

  private:
    void Clear(int kind);

    int    kind_ = -1;
    float  in_ = 0, mix_ = 0; // ramped: what goes in (echo, reverb), how much of the swept sound is heard
    double lfo_ = 0;          // 0..1, one turn a sweep
    Svf    svf_[2];
    /* flanger and echo: one delay line a side */
    std::vector<float> dl_[2];
    size_t             dw_ = 0, filled_ = 0;
    float              lp_[2] = {0, 0}; // the echo's repeats lose their top
    float              time_ = 0;       // the echo's delay as it is now, in frames: it glides to a new length
    /* phaser */
    float ap_[2][6] = {{0}, {0}}, pfb_[2] = {0, 0};
    /* reverb: 4 combs and 2 all-passes a side */
    struct Line
    {
        std::vector<float> b;
        size_t             at = 0;
        float              lp = 0;
    };
    Line comb_[2][4], all_[2][2];
};
} // namespace dk
