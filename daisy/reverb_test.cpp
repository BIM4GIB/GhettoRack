/** Desktop test: ab::Reverb (reverb.h) against DaisySP's ReverbSc, which it is
 *  a repacked copy of. Same input, same settings: the output must be the same
 *  sample for sample.
 *  Build: g++ -std=c++17 -I../lib/DaisySP/DaisySP-LGPL/Source/Effects reverb_test.cpp \
 *         ../lib/DaisySP/DaisySP-LGPL/Source/Effects/reverbsc.cpp -o hostbuild/reverb_test */
#include "reverb.h"
#include "reverbsc.h"
#include <cstdio>

int main()
{
    static daisysp::ReverbSc lib;
    static ab::Reverb        mine;
    int                      ra = lib.Init(48000.f), rb = mine.Init(48000.f);
    uint32_t                 seed = 12345;
    long                     diff = 0;
    double                   sum  = 0;
    const long               n    = 48000 * 20;
    for(long i = 0; i < n; i++)
    {
        if(i == 48000 * 6)
        {
            lib.SetFeedback(0.7f), mine.SetFeedback(0.7f);
            lib.SetLpFreq(3000.f), mine.SetLpFreq(3000.f);
        }
        seed     = seed * 1664525u + 1013904223u;
        float in = (i % 24000) < 2000 ? ((int)(seed >> 16) - 32768) / 65536.f : 0.f; // bursts of noise, then tails
        float a1, a2, b1, b2;
        lib.Process(in, in * 0.5f, &a1, &a2);
        mine.Process(in, in * 0.5f, &b1, &b2);
        if(a1 != b1 || a2 != b2)
            diff++;
        sum += a1 * a1;
    }
    bool ok = ra == 0 && rb == 0 && diff == 0 && sum > 1.0;
    printf("reverb: library %zu bytes, ours %zu bytes; %ld samples, %ld differ, energy %.1f: %s\n", sizeof(lib),
           sizeof(mine), n, diff, sum, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
