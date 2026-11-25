/** Desktop test: the 303 inside the engine. Drums on tracks 1-5, a 303
 *  line with slides and accents on track 6, a second 303 on track 7 played
 *  live with overlapping keys (legato), recording into the pattern.
 *  Writes acid_demo.wav and checks counts, levels and recorded steps.
 *  Build: see the end of mac/build.sh (same DSP library). */
#include "engine.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace ab;

static void WriteWav(const char* path, const std::vector<float>& l,
                     const std::vector<float>& r, int sr)
{
    FILE*    f    = fopen(path, "wb");
    uint32_t n    = l.size(), data = n * 4;
    auto     w32  = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto     w16  = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    w32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(2); w32(sr); w32(sr * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f);
    w32(data);
    for(uint32_t i = 0; i < n; i++)
    {
        int16_t a = (int16_t)lrintf(l[i] * 32767.f), b = (int16_t)lrintf(r[i] * 32767.f);
        fwrite(&a, 2, 1, f);
        fwrite(&b, 2, 1, f);
    }
    fclose(f);
}

int main()
{
    const float  sr = 48000.f;
    const size_t block = 48;
    const uint32_t slot_cap = 1024 * 1024;
    static std::vector<int16_t> slots(slot_cap * kTracks);
    static std::vector<float>   dl(96000), dr(96000), cl(192000), cr(192000);
    static daisysp::ReverbSc    reverb;
    Memory mem;
    for(int t = 0; t < kTracks; t++)
        mem.slot_mem[t] = slots.data() + t * slot_cap;
    mem.slot_capacity = slot_cap;
    mem.delay_l = dl.data(), mem.delay_r = dr.data(), mem.delay_len = dl.size();
    mem.capture_l = cl.data(), mem.capture_r = cr.data(), mem.capture_len = cl.size();
    mem.reverb = &reverb;

    static Engine e;
    e.Init(sr, mem);
    e.RenderBuiltInKit();
    e.SetSynth(5, true);
    e.SetSynth(6, true);

    for(int s : {0, 4, 8, 12})
        e.SetStep(0, 0, s, kStepAccent);
    e.SetStep(0, 1, 4, kStepAccent);
    e.SetStep(0, 1, 12, kStepAccent);
    for(int s = 0; s < 16; s += 2)
        e.SetStep(0, 2, s, kStepOn);

    /* 303 line on track 6: offsets from C2 */
    const int  notes[16] = {0, 12, 0, 3, 0, 0, 15, 12, 0, 7, 10, 12, 0, 0, 24, 19};
    const int  on[16]    = {2, 1, 1, 1, 0, 2, 1, 2, 1, 1, 1, 2, 0, 1, 2, 1};
    const bool sl[16]    = {0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1};
    for(int s = 0; s < 16; s++)
    {
        e.SetStep(0, 5, s, on[s]);
        e.SetNote(0, 5, s, 36 + notes[s]);
        e.SetSlide(0, 5, s, sl[s]);
    }
    e.track[5].vol = 0.8f;
    e.track[5].dly = 0.25f;
    e.track[6].acid.wave   = 1.f;
    e.track[6].acid.cutoff = 0.5f;
    e.track[6].pan         = 0.7f;
    e.global.bpm = 128.f;

    const float  bar = sr * 60.f / 128.f * 4.f;
    const size_t total = (size_t)(bar * 8) - 480; // stop before bar 9 starts
    std::vector<float> L(total), R(total);
    std::vector<float> l1(block), r1(block);

    uint32_t t5 = e.TriggerCount(5);
    e.Process(l1.data(), r1.data(), block); // picks up SetSynth
    e.Play();

    /* Live keys on track 7 during bars 3-4, recording on: G2 held, then
       A#2 pressed over it (slide), released, back to G2, all up */
    struct Ev { float bar; int note; bool on; } evs[] = {
        {2.0f, 43, true}, {2.25f, 46, true}, {2.5f, 46, false}, {2.75f, 43, false},
        {3.0f, 48, true}, {3.1f, 48, false}};
    size_t ev = 0;
    bool   rec_on = false;
    double peak = 0, sum6 = 0;
    for(size_t pos = 0; pos + block <= total; pos += block)
    {
        float b = pos / bar;
        if(!rec_on && b >= 2.f)
        {
            e.SetRecord(true);
            rec_on = true;
        }
        if(rec_on && b >= 3.5f)
            e.SetRecord(false);
        while(ev < sizeof(evs) / sizeof(evs[0]) && b >= evs[ev].bar)
        {
            e.LiveNote(6, evs[ev].note, evs[ev].on);
            ev++;
        }
        e.Process(&L[pos], &R[pos], block);
        for(size_t i = pos; i < pos + block; i++)
            peak = fmax(peak, fmax(fabs(L[i]), fabs(R[i])));
    }
    uint32_t fired = e.TriggerCount(5) - t5;
    int rec_steps = 0;
    for(int s = 0; s < 16; s++)
        if(e.GetStep(0, 6, s))
        {
            rec_steps++;
            printf("recorded track 7 step %d note %d\n", s + 1, e.GetNote(0, 6, s));
        }
    printf("track 6 notes fired %u (expect %d), peak %.3f, limiter %u, nan %u\n", fired,
           14 * 8, peak, e.limit_count, e.nan_count);
    WriteWav("acid_demo.wav", L, R, (int)sr);

    /* overload shedding: with both 303s left out their notes still fire (the
       sequencer runs on) but the mix is quieter; taken down again they are back */
    auto run = [&](int blocks) {
        double sq = 0;
        std::vector<float> l(block), r(block);
        for(int i = 0; i < blocks; i++)
        {
            e.Process(l.data(), r.data(), block);
            for(size_t k = 0; k < block; k++)
                sq += l[k] * l[k] + r[k] * r[k];
        }
        return sqrt(sq / (blocks * block * 2));
    };
    for(int t = 0; t < 5; t++)
        for(int st = 0; st < 16; st++)
            e.SetStep(0, t, st, kStepOff); // 303s alone
    run(2000);
    uint32_t f0 = e.TriggerCount(5);
    double with = run(4000);
    e.shed = 2;
    uint32_t f1 = e.TriggerCount(5);
    double without = run(4000);
    uint32_t f2 = e.TriggerCount(5);
    e.shed = 0;
    run(200);
    double back = run(4000);
    bool shed_ok = e.ShedMax() == 3 && with > 0.01 && without < with * 0.02 && back > with * 0.5
                   && f2 - f1 > 0 && (f2 - f1) * 10 >= (f1 - f0) * 8; // the windows don't fall on bar lines
    printf("shed: 303s alone rms %.4f, shed %.6f, back %.4f; notes fired %u then %u, ShedMax %d: %s\n", with, without,
           back, f1 - f0, f2 - f1, e.ShedMax(), shed_ok ? "ok" : "WRONG");
    bool ok = fired == 14 * 8 && peak > 0.2 && e.nan_count == 0 && rec_steps >= 2 && shed_ok;
    printf(ok ? "OK\n" : "FAIL\n");
    (void)sum6;
    return ok ? 0 : 1;
}
