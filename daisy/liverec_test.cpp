/** Desktop test: live sampling. Plays 1 bar, arms track 4, feeds a click
 *  on each beat plus a tone, stops after 2 bars, then checks the loop
 *  plays back with its clicks on the beat grid while the input is silent. */
#include "engine.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace ab;

int main()
{
    const float sr = 48000.f; const size_t B = 48;
    const uint32_t cap = 768 * 1024;
    static std::vector<int16_t> slots(cap * kTracks);
    static std::vector<float> dl(96000), dr(96000), cl(192000), cr(192000);
    static ab::Reverb rv;
    Memory m;
    for(int t = 0; t < kTracks; t++) m.slot_mem[t] = slots.data() + t * cap;
    m.slot_capacity = cap; m.delay_l = dl.data(); m.delay_r = dr.data(); m.delay_len = dl.size();
    m.capture_l = cl.data(); m.capture_r = cr.data(); m.capture_len = cl.size(); m.reverb = &rv;
    static Engine e;
    e.Init(sr, m);
    for(int t = 0; t < kTracks; t++) e.track[t].vol = 0.f; // only track 4 audible
    e.track[3].vol = 1.f; e.track[3].pan = 0.5f;
    e.global.bpm = 120.f; // bar = 2 s = 96000 samples, beat = 24000
    const uint32_t bar = 96000, beat = 24000;
    std::vector<float> L(B), R(B), in(B);
    uint64_t n = 0;
    auto run = [&](uint32_t samples, bool feed, std::vector<float>* rec = nullptr) {
        for(uint32_t k = 0; k < samples; k += B) {
            for(size_t i = 0; i < B; i++) {
                uint64_t s = n + i;
                float v = 0.f;
                if(feed) { v = 0.1f * sinf(2 * 3.14159f * 220.f * s / sr); if(s % beat < 48) v = 0.9f; }
                in[i] = v;
            }
            e.SetInput(in.data(), in.data());
            e.Process(L.data(), R.data(), B);
            if(rec) rec->insert(rec->end(), L.begin(), L.end());
            n += B;
        }
    };
    e.Process(L.data(), R.data(), B); // settle
    e.LiveRecArm(3);                  // stopped: starts the transport, records from bar 0
    n = 0;
    run(B, true);
    printf("state after arm: %d (2 = recording)\n", e.LiveRecState());
    run(bar + bar / 2, true);         // 1.5 bars in
    e.LiveRecStop();                  // ends at the end of bar 2
    run(bar / 2 + 4800, true);
    int t, bars;
    bool done = e.TakeLiveRecDone(&t, &bars);
    printf("done %d track %d bars %d loop %d\n", done, t + 1, bars, e.IsLoop(3));
    std::vector<float> out;
    uint64_t start = n;
    run(bar * 2, false, &out);        // input silent now: only the loop
    // find clicks in the output (big peaks)
    int on_grid = 0, clicks = 0;
    for(size_t i = 1; i < out.size(); i++)
        if(fabsf(out[i]) > 0.5f && fabsf(out[i - 1]) <= 0.5f) {
            clicks++;
            uint64_t pos = start + i;
            long off = (long)(pos % beat); if(off > (long)beat / 2) off -= beat;
            if(labs(off) < 200) on_grid++;
        }
    printf("clicks in 2 bars of playback: %d, on the beat grid: %d\n", clicks, on_grid);
    bool ok = done && bars == 2 && clicks >= 7 && on_grid == clicks;
    printf(ok ? "OK\n" : "FAIL\n");
    return ok ? 0 : 1;
}
