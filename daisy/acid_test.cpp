/** Checks acid.h against the Pocket 303 double original (voice.h).
 *  Build: g++ -O2 -I. -I<uhntiss>/m4l/pocket303/proto acid_test.cpp
 *  Plays a 2-bar line with accents, slides and both waves, compares the
 *  outputs sample by sample and writes both to wav for listening. */
#include "acid.h"
#include "voice.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

static void WriteWav(const char* path, const std::vector<float>& x, int sr)
{
    FILE*    f    = fopen(path, "wb");
    uint32_t n    = x.size(), data = n * 2;
    auto     w32  = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto     w16  = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    w32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(1); w32(sr); w32(sr * 2); w16(2); w16(16);
    fwrite("data", 1, 4, f);
    w32(data);
    for(float s : x)
    {
        float c = s > 1.f ? 1.f : (s < -1.f ? -1.f : s);
        int16_t v = (int16_t)(c * 32767.f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

struct Step { int on, note, acc, slide; };

int main()
{
    const int   sr   = 48000;
    const float step = sr * 15.f / 128.f; // 128 bpm sixteenths
    /* A classic-ish line: offsets from C2 (36) */
    const Step line[16] = {{1, 0, 1, 0},  {1, 12, 0, 0}, {1, 0, 0, 1},  {1, 3, 0, 0},
                           {0, 0, 0, 0},  {1, 0, 1, 0},  {1, 15, 0, 1}, {1, 12, 1, 0},
                           {1, 0, 0, 0},  {1, 7, 0, 1},  {1, 10, 0, 1}, {1, 12, 1, 0},
                           {0, 0, 0, 0},  {1, 0, 0, 0},  {1, 24, 1, 0}, {1, 19, 0, 1}};

    double worst = 0.0, err2 = 0.0, sig2 = 0.0;
    std::vector<float> out_ref, out_port;
    for(int pass = 0; pass < 2; pass++) // saw, then square with other knobs
    {
        ab::AcidParams ap;
        if(pass == 1)
        {
            ap.wave = 1.f, ap.cutoff = 0.16f, ap.reso = 0.9f, ap.envmod = 0.2f;
            ap.decay = 0.79f, ap.accent = 0.55f, ap.slide = 0.6f;
        }
        Params p = {ap.cutoff, ap.reso, ap.envmod, ap.decay, ap.accent, ap.tune,
                    ap.slide, 1.0, ap.wave};
        Voice v;
        voice_init(&v);
        ab::AcidVoice a;
        a.Init((float)sr);
        a.SetParams(ap);

        double in1 = 60, in2 = 0, in3 = 0, in4 = 0, in5 = 0;
        bool   tie = false;
        int    gate_left = 0;
        int    total = (int)(step * 32);
        int    next = 0, s = 0;
        for(int i = 0; i < total; i++)
        {
            if(i >= next)
            {
                const Step& st = line[s % 16];
                if(st.on)
                {
                    int n = 36 + st.note;
                    in1 = n, in2 = 1, in3 = st.acc, in4 = tie ? 1 : 0, in5 += 1;
                    a.Trigger(n, st.acc, tie);
                    gate_left = st.slide ? 0x7fffffff : (int)(step * 0.5f);
                    tie       = st.slide;
                }
                else
                {
                    in2 = 0, tie = false, gate_left = 0;
                    a.SetGate(false);
                }
                s++;
                next = (int)(s * step);
            }
            if(gate_left > 0 && --gate_left == 0)
            {
                in2 = 0;
                a.SetGate(false);
            }
            double led;
            double r = voice_tick(&v, in1, in2, in3, in4, in5, &p, sr, &led);
            float  q = a.Process();
            double e = fabs(r - q);
            if(e > worst)
                worst = e;
            err2 += e * e;
            sig2 += r * r;
            out_ref.push_back((float)r * 0.5f);
            out_port.push_back(q * 0.5f);
        }
    }
    double snr = 10.0 * log10(sig2 / (err2 + 1e-30));
    /* The ladder sits near self-oscillation, so tiny rounding differences
       move the waveform's fine detail; what you hear is the envelope and
       spectrum. Compare loudness in 5 ms windows instead. */
    double env_worst = 0, env_sum = 0;
    int    env_n = 0;
    for(size_t w = 0; w + 240 <= out_ref.size(); w += 240)
    {
        double a = 0, b = 0;
        for(size_t i = w; i < w + 240; i++)
            a += out_ref[i] * out_ref[i], b += out_port[i] * out_port[i];
        if(a < 1e-6 * 240) // below -60 dB: skip silence
            continue;
        double d = fabs(10.0 * log10((b + 1e-12) / a));
        env_worst = d > env_worst ? d : env_worst;
        env_sum += d;
        env_n++;
    }
    printf("loudness in 5 ms windows: mean diff %.3f dB, worst %.2f dB (%d windows)\n",
           env_sum / env_n, env_worst, env_n);
    printf("samples %zu  peak ref %.3f  worst abs error %.6f  SNR %.1f dB\n", out_ref.size(),
           [&] { float m = 0; for(float x : out_ref) m = fmaxf(m, fabsf(x * 2)); return m; }(),
           worst, snr);
    WriteWav("acid_ref.wav", out_ref, sr);
    WriteWav("acid_port.wav", out_port, sr);

    /* Speed, desktop only: relative cost of the two */
    ab::AcidVoice a;
    a.Init((float)sr);
    a.Trigger(48, false, false);
    auto  t0 = std::chrono::steady_clock::now();
    float acc = 0.f;
    for(int i = 0; i < sr * 20; i++)
        acc += a.Process();
    auto t1 = std::chrono::steady_clock::now();
    Voice v;
    voice_init(&v);
    Params p = {0.3, 0.79, 0.67, 0.2, 0.79, 0.5, 0.3, 1.0, 0};
    double acc2 = 0, led;
    for(int i = 0; i < sr * 20; i++)
        acc2 += voice_tick(&v, 48, 1, 0, 0, 1, &p, sr, &led);
    auto t2 = std::chrono::steady_clock::now();
    printf("20 s of audio: port %.1f ms, original %.1f ms (%g %g)\n",
           std::chrono::duration<double, std::milli>(t1 - t0).count(),
           std::chrono::duration<double, std::milli>(t2 - t1).count(), acc, acc2);
    return (env_sum / env_n < 0.1 && env_worst < 1.0) ? 0 : 1;
}
