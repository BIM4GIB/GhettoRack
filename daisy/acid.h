/** Pocket 303 voice for the ApcBox engine.
 *
 *  Float port of UhnTiss m4l/pocket303/proto/voice.h (MIT, same author):
 *  polyBLEP saw/square, RC slide on the note, decay-only MEG, accent
 *  sweep, snap VCA, 2x oversampled 4-pole ladder with tanh input stage.
 *  Same math and constants; the changes are only for speed on the
 *  Cortex-M7:
 *   - float instead of double (the oscillator phase stays double: it
 *     accumulates, and float drift there is the biggest difference),
 *   - the exp(-T/tau) coefficients are worked out once per block
 *     (they only depend on knobs), not once per sample,
 *   - pow/exp/tanh go through a fast exp2 (about 1e-6 relative error).
 *  acid_test.cpp renders this against the double original.
 *
 *  Note events replace the gen~ trig counter: Trigger() marks a new note
 *  for the next sample, exactly like the counter changing.
 */
#pragma once
#include <cmath>
#include <cstdint>

namespace ab
{
/** The Pocket 303 panel, as gen~ 0..1 values (Volume is the track fader) */
struct AcidParams
{
    float cutoff = 0.30f; // "Acid Trojan", the device's default sound
    float reso   = 0.79f;
    float envmod = 0.67f;
    float decay  = 0.20f;
    float accent = 0.79f;
    float tune   = 0.5f;  // 0.5 = 0 st, 0..1 = -12..+12
    float slide  = 0.30f; // glide time
    float wave   = 0.f;   // >= 0.5 = square
};

/** 2^x, |x| < 100. Splits x into integer and [-0.5, 0.5] fraction. */
inline float FastExp2(float x)
{
    float fl = floorf(x + 0.5f);
    float f  = x - fl;
    float p  = 1.f
              + f
                    * (0.69314718f
                       + f
                             * (0.24022651f
                                + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union
    {
        int32_t i;
        float   v;
    } u;
    u.i = ((int32_t)fl + 127) << 23;
    return p * u.v;
}

inline float FastExp(float x)
{
    return FastExp2(x * 1.44269504f);
}

inline float FastTanh(float x)
{
    if(x > 9.f)
        return 1.f;
    if(x < -9.f)
        return -1.f;
    float e = FastExp2(x * 2.88539008f); // e^(2x)
    return 1.f - 2.f / (e + 1.f);
}

/** inv_dt = 1 / dt, worked out once per sample by the caller */
inline float PolyBlep(float t, float dt, float inv_dt)
{
    if(t < dt)
    {
        float tt = t * inv_dt;
        return tt + tt - tt * tt - 1.f;
    }
    if(t > 1.f - dt)
    {
        float tt = (t - 1.f) * inv_dt;
        return tt * tt + tt + tt + 1.f;
    }
    return 0.f;
}

class AcidVoice
{
  public:
    void Init(float sr)
    {
        sr_ = sr;
        T_  = 1.f / sr;
        Reset();
        acc_raw_mul_ = expf(-T_ / 0.2f);
        acc_env_k_   = 1.f - expf(-T_ / 0.012f);
        vca_hold_    = expf(-T_ / 3.f);
        vca_att_     = 1.f - expf(-T_ / 0.003f);
        vca_rel_     = 1.f - expf(-T_ / 0.010f);
        meg_acc_mul_ = expf(-T_ / 0.2f);
        SetParams(AcidParams());
    }

    /** Clears the sound state, keeps the knobs */
    void Reset()
    {
        ph_ = 0.f;
        pitch_s_ = 60.f;
        acc_note_ = meg_ = acc_raw_ = acc_env_ = vca_lvl_ = vca_ = 0.f;
        s1_ = s2_ = s3_ = s4_ = fb_ = hp_x_ = hp_y_ = 0.f;
        note_ = 60.f;
        gate_ = false;
        trig_ = legato_ = accent_ = false;
    }

    /** Knobs: once per audio block */
    void SetParams(const AcidParams& p)
    {
        float slide_tau = 0.012f * powf(25.f, p.slide);
        slide_k_        = 1.f - expf(-T_ / slide_tau);
        meg_tau_        = 0.03f * powf(80.f, p.decay);
        meg_mul_        = expf(-T_ / meg_tau_);
        envmod_         = p.envmod;
        accent_amt_     = p.accent;
        tune_st_        = (p.tune - 0.5f) * 24.f;
        square_         = p.wave > 0.5f;
        cut_hz_         = 150.f * powf(2.f, p.cutoff * 5.f);
        k_              = p.reso * 4.f;
        makeup_         = 1.f + 0.25f * k_;
    }

    /** New note on the next sample. legato = slide into it from the
     *  previous one (no retrigger, the pitch glides). */
    void Trigger(int note, bool accent, bool legato)
    {
        note_   = (float)note;
        accent_ = accent;
        legato_ = legato;
        trig_   = true;
        gate_   = true;
    }

    /** Changes the target note without a new trigger (glides) */
    void SetNote(int note) { note_ = (float)note; }

    void SetGate(bool on) { gate_ = on; }
    bool Gate() const { return gate_; }

    /** Nothing audible and nothing coming: Process can be skipped */
    bool Idle() const
    {
        return !gate_ && !trig_ && vca_ < 1e-6f && vca_lvl_ == 0.f;
    }

    /** Skipped while idle: the envelopes that would have decayed to
     *  nothing by the next note are cleared instead */
    void Sleep()
    {
        acc_raw_ = acc_env_ = 0.f;
        vca_     = 0.f;
    }

    float Process()
    {
        /* ---- note events ---- */
        const bool newtrig = trig_;
        trig_              = false;
        const bool retrig  = newtrig && !legato_;
        float acc_note     = newtrig ? (accent_ ? 1.f : 0.f) : acc_note_;

        /* ---- slide ---- */
        float pitch_s = retrig ? note_ : pitch_s_ + (note_ - pitch_s_) * slide_k_;
        float semis   = pitch_s + tune_st_;
        float freq    = 440.f * FastExp2((semis - 69.f) * (1.f / 12.f));

        /* ---- oscillator ---- */
        float inc = freq * T_;
        double phd = ph_ + (double)inc;
        phd -= floor(phd);
        float ph = (float)phd;
        float inv = 1.f / inc;
        float osc;
        if(square_)
        {
            float ph2 = ph + 0.5f;
            ph2 -= floorf(ph2);
            osc = ((ph < 0.5f) ? 1.f : -1.f) + PolyBlep(ph, inc, inv) - PolyBlep(ph2, inc, inv);
        }
        else
            osc = 2.f * ph - 1.f - PolyBlep(ph, inc, inv);

        /* ---- MEG; accent forces the fast decay ---- */
        float meg_mul = (acc_note > 0.5f && meg_tau_ > 0.2f) ? meg_acc_mul_ : meg_mul_;
        float meg     = (retrig ? 1.f : meg_) * meg_mul;

        /* ---- accent sweep ---- */
        float acc_raw = ((retrig && acc_note > 0.5f) ? 1.f : acc_raw_) * acc_raw_mul_;
        float acc_env = acc_env_ + (acc_raw * accent_amt_ - acc_env_) * acc_env_k_;

        /* ---- VCA ---- */
        float vca_lvl = retrig ? 1.f : vca_lvl_;
        vca_lvl       = gate_ ? vca_lvl * vca_hold_ : 0.f;
        float vca_c   = (vca_lvl > vca_) ? vca_att_ : vca_rel_;
        float vca     = vca_ + (vca_lvl - vca_) * vca_c;
        float amp     = vca * (1.f + 0.7f * acc_env);

        /* ---- cutoff ---- */
        float fc = cut_hz_ * FastExp2(meg * envmod_ * 2.4f + acc_env * 0.5f);
        fc       = fc > 8000.f ? 8000.f : (fc < 20.f ? 20.f : fc);

        /* ---- ladder, 2x oversampled ---- */
        float g  = 1.f - FastExp(-3.14159265f * fc * T_); // 2 pi fc / (2 sr)
        float s1 = s1_, s2 = s2_, s3 = s3_, s4 = s4_, fb = fb_;
        for(int i = 0; i < 2; i++)
        {
            float u   = osc * 0.5f - k_ * fb;
            float x   = 2.f * FastTanh(0.5f * u);
            s1        = s1 + g * (x - s1);
            s2        = s2 + g * (s1 - s2);
            s3        = s3 + g * (s2 - s3);
            float s4n = s4 + g * (s3 - s4);
            fb        = 0.5f * (s4n + s4);
            s4        = s4n;
        }

        /* ---- output ---- */
        float y  = FastTanh(s4 * makeup_);
        float hp = y - hp_x_ + 0.997f * hp_y_;

        acc_note_ = acc_note;
        pitch_s_  = pitch_s;
        ph_       = phd;
        meg_      = meg;
        acc_raw_  = acc_raw;
        acc_env_  = acc_env;
        vca_lvl_  = vca_lvl;
        vca_      = vca;
        s1_ = s1, s2_ = s2, s3_ = s3, s4_ = s4, fb_ = fb;
        hp_x_ = y;
        hp_y_ = hp;
        return hp * amp;
    }

  private:
    float sr_ = 48000.f, T_ = 1.f / 48000.f;

    // knobs (per block)
    float slide_k_ = 0.f, meg_tau_ = 0.f, meg_mul_ = 0.f, envmod_ = 0.f;
    float accent_amt_ = 0.f, tune_st_ = 0.f, cut_hz_ = 150.f, k_ = 0.f, makeup_ = 1.f;
    bool  square_ = false;

    // fixed coefficients
    float acc_raw_mul_ = 0.f, acc_env_k_ = 0.f, vca_hold_ = 0.f, vca_att_ = 0.f;
    float vca_rel_ = 0.f, meg_acc_mul_ = 0.f;

    // inputs
    float note_ = 60.f;
    bool  gate_ = false, trig_ = false, legato_ = false, accent_ = false;

    // state
    double ph_; float pitch_s_, acc_note_, meg_, acc_raw_, acc_env_, vca_lvl_, vca_;
    float s1_, s2_, s3_, s4_, fb_, hp_x_, hp_y_;
};

} // namespace ab
