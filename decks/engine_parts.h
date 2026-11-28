/** Filters the engine and its effects share */
#pragma once

namespace dk
{
constexpr int kRate = 44100;

struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline float Run(float x)
    {
        float y = b0 * x + z1;
        z1      = b1 * x - a1 * y + z2;
        z2      = b2 * x - a2 * y;
        return y;
    }
    void Butter(bool high, float hz);
    void CopyCoefs(const Biquad& o) { b0 = o.b0, b1 = o.b1, b2 = o.b2, a1 = o.a1, a2 = o.a2; }
};

/** Three bands that add up to the input when all three are at 1: two
 *  4th-order Linkwitz-Riley splits, the low band passed through the upper
 *  split as well so the three stay in phase. A band at 0 is gone (kill). */
struct Eq3
{
    Biquad lp1[2], hp1[2], lp2[2], hp2[2], alp[2], ahp[2];
    void   Init(float lo_hz, float hi_hz);
    inline float Run(float x, float gl, float gm, float gh)
    {
        float lo   = lp1[1].Run(lp1[0].Run(x));
        float rest = hp1[1].Run(hp1[0].Run(x));
        float low  = alp[1].Run(alp[0].Run(lo)) + ahp[1].Run(ahp[0].Run(lo));
        float mid  = lp2[1].Run(lp2[0].Run(rest));
        float high = hp2[1].Run(hp2[0].Run(rest));
        return gl * low + gm * mid + gh * high;
    }
};

/** State-variable filter (trapezoidal), low-pass and high-pass from one core */
struct Svf
{
    float ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1;
    void  Set(float hz, float q);
    inline void Run(float x, float& lp, float& hp)
    {
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1      = 2 * v1 - ic1;
        ic2      = 2 * v2 - ic2;
        lp       = v2;
        hp       = x - k * v1 - v2;
    }
};

} // namespace dk
