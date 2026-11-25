/** Desktop test for the ApcBox engine.
 *  Plays a 16-bar demo that exercises the sequencer, mixer and every
 *  performance effect, writes demo.wav, and checks the output.
 */
#include "engine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

using namespace ab;

static void WriteWav(const char* path, const std::vector<float>& l,
                     const std::vector<float>& r, int sr)
{
    FILE*    f    = fopen(path, "wb");
    uint32_t n    = l.size();
    uint32_t data = n * 4;
    auto     w32  = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto     w16  = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    w32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(2);
    w32(sr);
    w32(sr * 4);
    w16(4);
    w16(16);
    fwrite("data", 1, 4, f);
    w32(data);
    for(uint32_t i = 0; i < n; i++)
    {
        int16_t a = (int16_t)lrintf(l[i] * 32767.f);
        int16_t b = (int16_t)lrintf(r[i] * 32767.f);
        fwrite(&a, 2, 1, f);
        fwrite(&b, 2, 1, f);
    }
    fclose(f);
}

int main()
{
    const float  sr    = 48000.f;
    const size_t block = 48;

    /* Same memory layout the firmware uses */
    const uint32_t slot_cap = 3 * 1024 * 1024;
    static std::vector<int16_t> slots(slot_cap * kTracks);
    static std::vector<float>   dl(96000), dr(96000), cl(192000), cr(192000);
    static ab::Reverb    reverb;

    Memory mem;
    for(int t = 0; t < kTracks; t++)
        mem.slot_mem[t] = slots.data() + t * slot_cap;
    mem.slot_capacity = slot_cap;
    mem.delay_l       = dl.data();
    mem.delay_r       = dr.data();
    mem.delay_len     = dl.size();
    mem.capture_l     = cl.data();
    mem.capture_r     = cr.data();
    mem.capture_len   = cl.size();
    mem.reverb        = &reverb;

    static Engine e;
    e.Init(sr, mem);
    e.RenderBuiltInKit();

    /* Pattern 1 */
    int p = 0;
    for(int s : {0, 4, 8, 12})
        e.SetStep(p, 0, s, kStepAccent);
    e.SetStep(p, 0, 10, kStepOn);
    e.SetStep(p, 1, 4, kStepAccent);
    e.SetStep(p, 1, 12, kStepAccent);
    for(int s = 0; s < 16; s += 2)
        if(s != 14)
            e.SetStep(p, 2, s, (s % 4) ? kStepAccent : kStepOn);
    e.SetStep(p, 3, 14, kStepOn);
    e.SetStep(p, 4, 12, kStepOn);
    e.SetStep(p, 6, 3, kStepOn);
    e.SetStep(p, 6, 7, kStepOn);
    e.SetStep(p, 6, 13, kStepOn);
    /* Pattern 2: pattern 1 plus toms and cowbell */
    e.CopyPattern(0, 1);
    e.SetStep(1, 5, 11, kStepOn);
    e.SetStep(1, 5, 15, kStepAccent);
    e.SetStep(1, 7, 2, kStepOn);
    e.SetStep(1, 7, 9, kStepOn);
    e.SetStep(1, 0, 7, kStepOn);

    e.global.bpm   = 124.f;
    e.global.swing = 0.22f;
    e.track[2].pan = 0.35f;
    e.track[3].pan = 0.65f;
    e.track[6].pan = 0.7f;
    e.track[6].rev = 0.6f;
    e.track[4].rev = 0.55f;
    e.track[4].dly = 0.5f;
    e.track[7].dly = 0.6f;
    e.track[7].pan = 0.3f;
    e.track[2].vol = 0.6f;
    e.track[3].vol = 0.6f;
    e.track[1].rev = 0.3f;
    e.global.dly_time = 0.55f; // dotted 1/8

    const float bar_samples = sr * 60.f / 124.f * 4.f;
    const int   bars        = 16;
    const size_t total      = (size_t)(bar_samples * bars) + (size_t)sr * 2;
    std::vector<float> L(total), R(total);

    uint32_t kick_count0 = e.TriggerCount(0);
    e.Play();

    double   proc_secs = 0.0;
    size_t   pos       = 0;
    int      fx_on     = -1;
    while(pos + block <= total)
    {
        float t_bar = pos / bar_samples; // position in bars
        int   bar   = (int)t_bar;
        float beat  = (t_bar - bar) * 4.f;

        /* Scripted performance */
        int want_fx = -1;
        if(bar == 3 && beat >= 3.f)
            want_fx = kFxRepeat16;
        if(bar == 4 && beat >= 3.f && beat < 3.5f)
            want_fx = kFxRepeat8;
        if(bar == 4 && beat >= 3.5f)
            want_fx = kFxRepeat32;
        if(bar == 5 && beat >= 2.f)
            want_fx = kFxTapeStop;
        if(bar == 7 && beat >= 3.f)
            want_fx = kFxReverse;
        if(bar == 8)
            want_fx = kFxCrush;
        if(bar == 9)
            want_fx = kFxGate;
        if(want_fx != fx_on)
        {
            if(fx_on >= 0)
                e.SetPerfFx(fx_on, false);
            if(want_fx >= 0)
                e.SetPerfFx(want_fx, true);
            fx_on = want_fx;
        }

        /* DJ filter sweep in bars 1-2: low-pass down, back, then high-pass */
        if(bar == 1)
            e.global.xfader = 0.5f - 0.45f * sinf(3.14159f * beat / 4.f);
        else if(bar == 2)
            e.global.xfader = 0.5f + 0.4f * sinf(3.14159f * beat / 4.f);
        else
            e.global.xfader = 0.5f;

        if(bar == 10 && beat < 0.01f)
            e.QueuePattern(1);
        /* Snare roll on the last beat of bar 11 */
        e.SetRoll(1, bar == 11 && beat >= 3.f);
        /* Live hits with record on in bar 12 */
        e.SetRecord(bar == 12);
        if(bar == 12 && fmodf(beat, 1.f) > 0.52f && fmodf(beat, 1.f) < 0.53f)
            e.TriggerLive(7);
        /* Bars 13-15: track filter and drive on the kick, delay feedback up */
        if(bar >= 13)
        {
            e.track[0].drive  = 0.5f;
            e.track[1].filter = 0.7f;
            e.global.dly_fb   = 0.6f;
            e.track[1].dly    = 0.4f;
        }
        if(bar >= bars)
            e.Stop();

        auto t0 = std::chrono::steady_clock::now();
        e.Process(&L[pos], &R[pos], block);
        auto t1 = std::chrono::steady_clock::now();
        proc_secs += std::chrono::duration<double>(t1 - t0).count();
        pos += block;
    }

    /* Checks */
    int    bad  = 0;
    float  peak = 0.f;
    double sum  = 0.0;
    for(size_t i = 0; i < pos; i++)
    {
        if(!std::isfinite(L[i]) || !std::isfinite(R[i]))
            bad++;
        peak = fmaxf(peak, fmaxf(fabsf(L[i]), fabsf(R[i])));
        sum += L[i] * L[i] + R[i] * R[i];
    }
    printf("samples %zu, non-finite %d, peak %.3f, rms %.3f\n",
           pos, bad, peak, sqrt(sum / (2.0 * pos)));
    printf("kick triggers %u (expect about %d)\n",
           e.TriggerCount(0) - kick_count0, 16 * 5 + 1);
    printf("pattern now %d, pattern 1 tr7 steps:", e.CurrentPattern());
    for(int s = 0; s < 16; s++)
        printf("%d", e.GetStep(1, 7, s));
    printf("\n");

    for(int b = 0; b < bars + 1; b++)
    {
        size_t a = (size_t)(b * bar_samples), z = (size_t)((b + 1) * bar_samples);
        if(z > pos)
            z = pos;
        double s2 = 0;
        float  pk = 0;
        for(size_t i = a; i < z; i++)
        {
            s2 += L[i] * L[i];
            pk = fmaxf(pk, fabsf(L[i]));
        }
        printf("bar %2d rms %.3f peak %.3f\n", b, sqrt(s2 / (z - a)), pk);
    }
    printf("limiter >1dB on %.2f%% of samples\n", 100.0 * e.limit_count / pos);
    printf("host: %.1f%% of real time\n", 100.0 * proc_secs / (pos / sr));

    L.resize(pos);
    R.resize(pos);
    WriteWav("demo.wav", L, R, (int)sr);

    /* Stress: every step on every track, max resonance near the top of
       the filter range, full drive, everything at full level */
    static Engine s;
    s.Init(sr, mem);
    s.RenderBuiltInKit();
    for(int tr = 0; tr < kTracks; tr++)
    {
        for(int st = 0; st < kSteps; st++)
            s.SetStep(0, tr, st, kStepAccent);
        s.track[tr].vol    = 1.f;
        s.track[tr].filter = 0.44f + 0.01f * tr;
        s.track[tr].reso   = 1.f;
        s.track[tr].drive  = 1.f;
        s.track[tr].rev    = 1.f;
        s.track[tr].dly    = 1.f;
    }
    s.global.master    = 1.f;
    s.global.drive     = 1.f;
    s.global.dly_fb    = 1.f;
    s.global.rev_size  = 1.f;
    s.global.fx_return = 1.f;
    s.global.bpm       = 200.f;
    s.Play();
    int   sbad = 0;
    float spk  = 0.f;
    std::vector<float> a(block), b(block);
    for(int blk = 0; blk < 48000 * 20 / (int)block; blk++)
    {
        s.global.xfader = (blk / 2000) % 2 ? 0.44f : 0.99f;
        s.Process(a.data(), b.data(), block);
        for(size_t i = 0; i < block; i++)
        {
            if(!std::isfinite(a[i]) || !std::isfinite(b[i]))
                sbad++;
            spk = fmaxf(spk, fabsf(a[i]));
        }
    }
    printf("stress: non-finite %d, peak %.3f, nan resets %u\n", sbad, spk, s.nan_count);
    return (bad || sbad) ? 1 : 0;
}
