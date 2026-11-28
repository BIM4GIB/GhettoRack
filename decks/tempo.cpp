#include "tempo.h"
#include "engine.h"
#include <algorithm>
#include <cmath>
#include <vector>

/* How: the track becomes a line of "something started here" values, 345 a
 * second (onsets: the rise in loudness of the bass and of the top end). A
 * tempo is tried by laying a comb over that line, one tooth a beat, and
 * adding up what the teeth hit; the right tempo and phase hit the kicks.
 * First coarsely on 20 s pieces spread over the track (0.1 bpm steps: a comb
 * that is slightly off still fits a short piece), then finer on longer
 * stretches, last over the whole track, where a hundredth of a bpm shows. */

namespace dk
{
namespace
{
constexpr int   kHop  = 128;
constexpr float kFr   = (float)kRate / kHop; // onset values a second
/* The best tempo's fit over the middling one's: under this there is no beat to speak of.
 * Seen: noise 1.11, a slow swell 1.03, two real tracks 1.71 and 1.75, made-up kick tracks far more.
 * Two real tracks are not many: this number is a first guess. */
constexpr float kSure = 1.35f;

/** Mean of what a comb of this period hits in o[a..b), at its best phase. *at: that phase (index of a tooth). */
float Comb(const std::vector<float>& o, int a, int b, double period, double* at = nullptr)
{
    const int phases = (int)std::ceil(period);
    float     best   = -1;
    for(int ph = 0; ph < phases; ph++)
    {
        float sum = 0;
        int   n   = 0;
        for(double x = a + ph; x < b; x += period)
            sum += o[(int)(x + 0.5) < b ? (int)(x + 0.5) : b - 1], n++;
        if(n && sum / n > best)
        {
            best = sum / n;
            if(at)
                *at = a + ph;
        }
    }
    return best;
}
/** How well a tempo fits o[a..b): what a comb at the beat hits, and what one at half the beat
 *  hits. The second is what tells the beat from its neighbours: music with as much on the
 *  off-beat as on the beat (a bass between the kicks) fits a comb at 1 1/2 or 1 1/3 beats as
 *  well as one at the beat, but halve those and the teeth land between the notes. */
float Fit(const std::vector<float>& o, int a, int b, double period)
{
    return Comb(o, a, b, period) + Comb(o, a, b, period / 2);
}
} // namespace

Tempo FindTempo(const int16_t* pcm, uint32_t frames)
{
    Tempo     t;
    const int n = (int)(frames / kHop);
    if(n < (int)(kFr * 8)) // under 8 s: too little to say
        return t;

    /* onsets: per hop the energy of the bass (under 150 Hz) and of the top (the signal's own
       steps), on a log scale; what counts is how much each rose */
    std::vector<float> o(n);
    {
        Biquad lp;
        lp.Butter(false, 150.f);
        float  prev = 0, el = 0, eh = 0;
        double lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0; // the two hops before: a steady tone's energy wobbles hop to hop, three together do not
        for(int i = 0; i < n; i++)
        {
            double lo = 0, hi = 0;
            const int16_t* p = pcm + (size_t)i * kHop * 2;
            for(int k = 0; k < kHop; k++)
            {
                const float x = (p[2 * k] + p[2 * k + 1]) * (0.5f / 32768.f);
                const float b = lp.Run(x);
                lo += b * b;
                hi += (x - prev) * (x - prev);
                prev = x;
            }
            const float nl = std::log1p(2000.f * (float)((lo + 2 * lo1 + lo2) / (4 * kHop))), nh = std::log1p(2000.f * (float)((hi + 2 * hi1 + hi2) / (4 * kHop)));
            lo2 = lo1, hi2 = hi1, lo1 = lo, hi1 = hi;
            o[i] = i ? std::max(0.f, nl - el) + 0.5f * std::max(0.f, nh - eh) : 0;
            el = nl, eh = nh;
        }
    }
    /* a little width, so a tooth that is a hop off still hits */
    std::vector<float> s(n);
    for(int i = 0; i < n; i++)
    {
        float v = 3 * o[i];
        if(i > 0)
            v += 2 * o[i - 1];
        if(i > 1)
            v += o[i - 2];
        if(i + 1 < n)
            v += 2 * o[i + 1];
        if(i + 2 < n)
            v += o[i + 2];
        s[i] = v / 9;
    }
    double mean = 0;
    for(float v : s)
        mean += v;
    mean /= n;
    if(mean <= 1e-2)
        return t; // silence, or a sound that never starts anything

    /* 1: 20 s pieces, every 0.1 bpm */
    const int piece = std::min(n, (int)(kFr * 20));
    const int count = std::max(1, std::min(8, n / piece));
    std::vector<int> start(count);
    for(int w = 0; w < count; w++)
        start[w] = count == 1 ? 0 : (int)((0.1 + 0.8 * w / (count - 1)) * (n - piece));
    double             best_bpm = 0, best = -1;
    std::vector<float> all;
    for(double bpm = kBpmLo; bpm < kBpmHi; bpm += 0.1)
    {
        const double period = kFr * 60.0 / bpm;
        double       sum    = 0;
        for(int w = 0; w < count; w++)
            sum += Fit(s, start[w], start[w] + piece, period);
        all.push_back((float)sum);
        if(sum > best)
            best = sum, best_bpm = bpm;
    }
    std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
    t.sure = (float)(best / std::max(1e-9f, all[all.size() / 2]));
    if(t.sure < kSure)
        return t;

    /* 2: longer stretches, finer steps, each inside what the step before allows */
    const struct
    {
        float secs, span, step;
    } pass[] = {{60, 0.12f, 0.02f}, {180, 0.03f, 0.005f}, {3600, 0.008f, 0.001f}};
    for(const auto& p : pass)
    {
        const int len = std::min(n, (int)(kFr * p.secs));
        const int a   = (n - len) / 2;
        double    top = -1, top_bpm = best_bpm;
        for(double bpm = best_bpm - p.span; bpm <= best_bpm + p.span + 1e-9; bpm += p.step)
        {
            const double v = Fit(s, a, a + len, kFr * 60.0 / bpm);
            if(v > top)
                top = v, top_bpm = bpm;
        }
        if(top < 0.6 * best / count)
            break; // the longer stretch does not hold together (tempo moves, or a long break): keep what the pieces said
        best_bpm = top_bpm;
        if(len == n)
            break;
    }
    /* dance music is made on whole numbers far more often than not: within 0.03, take it */
    if(std::fabs(best_bpm - std::floor(best_bpm + 0.5)) < 0.03)
        best_bpm = std::floor(best_bpm + 0.5);

    /* where the beats are: the comb's best phase on the bass alone would be nicer, the whole line does */
    const double period = kFr * 60.0 / best_bpm;
    double       at     = 0;
    Comb(s, 0, n, period, &at);
    /* an onset value says "louder than the hop before": the hit is at that hop's start, give or take a hop */
    double first = std::fmod(at, period) * kHop;
    t.bpm        = (float)best_bpm;
    t.beat0      = first;
    return t;
}
} // namespace dk
