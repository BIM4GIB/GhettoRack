/** GhettoRack decks: a two-deck DJ engine for the companion box.
 *
 *  Two decks play tracks held whole in memory, through a mixer (trim, 3-band
 *  EQ with kill, one-knob filter, fader, crossfader) to four outputs: master
 *  on 1/2, headphones (cue) on 3/4. 44.1 kHz, the rate of the Numark's card.
 *
 *  Threads: Process() is the audio thread and never waits, allocates or
 *  touches a file. Everything else talks to it through Post() (events, one
 *  lock on the sender's side) and the atomic knob values, and reads the
 *  atomic state back. A track is loaded elsewhere and handed over by
 *  pointer; the one it replaces comes back through Trash() to be freed.
 *
 *  No Linux, ALSA or file code here: this file and engine.cpp also build on
 *  the desk for the tests (engine_test.cpp). */
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace dk
{
constexpr int kRate  = 44100;
constexpr int kDecks = 2;
constexpr int kOuts  = 4;  // master L R, headphones L R
constexpr int kChunk = 32; // frames between knob and speed updates (0.7 ms)

/** One jog tick moves the record this far while scratching: the platter is
 *  taken as 1024 ticks a turn at 33 1/3 rpm (the figure Mixxx's mapping
 *  uses for this controller; not measured here). */
constexpr double kJogFramesPerTick = 1.8 * kRate / 1024.0;

struct Track
{
    const int16_t* pcm    = nullptr; // stereo, interleaved
    uint32_t       frames = 0;
    uint32_t       id     = 0;
};

enum EvType : uint8_t
{
    kEvLoad,    // track (nullptr: eject)
    kEvPlay,    // a: 0 stop, 1 play, 2 toggle
    kEvCue,     // a: 1 pressed, 0 let go
    kEvSeek,    // d: frame
    kEvSeekRel, // d: frames from here
    kEvTouch,   // a: 1 hand on the platter, 0 off
    kEvJog,     // a: ticks, signed
    kEvBend,    // a: speed offset while a pitch-bend button is held (0: let go)
};

struct Ev
{
    uint8_t      type  = 0;
    int8_t       deck  = 0;
    float        a     = 0;
    double       d     = 0;
    const Track* track = nullptr;
};

/** One writer, one reader, no locks */
template <typename T, int N>
class Ring
{
  public:
    bool Push(const T& v)
    {
        uint32_t w = w_.load(std::memory_order_relaxed);
        if(w - r_.load(std::memory_order_acquire) >= (uint32_t)N)
            return false;
        buf_[w % N] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& v)
    {
        uint32_t r = r_.load(std::memory_order_relaxed);
        if(r == w_.load(std::memory_order_acquire))
            return false;
        v = buf_[r % N];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

  private:
    T                     buf_[N];
    std::atomic<uint32_t> w_{0}, r_{0};
};

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

/** What a knob at 0..1 means. Centre is "as it comes" for all but the faders. */
float TrimGain(float knob);   // 0 off, 0.5 unity, 1 = x4 (+12 dB)
float EqGain(float knob);     // 0 kill, 0.5 unity, 1 = x2 (+6 dB)
float FaderGain(float knob);  // 0 off, 1 unity
float OutGain(float knob);    // master and headphones: 0 off, 0.8 unity, 1 = +3.9 dB
float SoftClip(float x);      // straight up to 0.9, never past 1

/** Knobs of one deck, all 0..1 unless said. Written by anyone, read by the audio thread. */
struct DeckKnobs
{
    std::atomic<float> rate{0};      // speed offset: 0.08 = 8% faster
    std::atomic<float> gain{0.5f};
    std::atomic<float> hi{0.5f}, mid{0.5f}, lo{0.5f};
    std::atomic<float> filter{0.5f}; // left of centre low-pass, right high-pass
    std::atomic<float> volume{1};
    std::atomic<float> pfl{0};       // 1: this deck is in the headphones
    std::atomic<float> scratch{1};   // 1: a touched platter scratches, 0: it only nudges
};

/** What a deck is doing, for the reader (written once a Process call) */
struct DeckState
{
    std::atomic<double>   pos{0}, cue{0}; // frames
    std::atomic<uint32_t> frames{0}, track{0};
    std::atomic<int>      playing{0}, cue_held{0}, touched{0};
    std::atomic<float>    speed{0}, peak{0}; // peak: before the fader, decaying
};

class Engine
{
  public:
    Engine();

    DeckKnobs          knobs[kDecks];
    std::atomic<float> xfader{0.5f}; // 0 = deck 1 only, 1 = deck 2 only; both full at centre
    std::atomic<float> master{0.8f};
    std::atomic<float> cue_mix{0};   // headphones: 0 cue only, 1 master only
    std::atomic<float> cue_gain{0.8f};

    DeckState          state[kDecks];
    std::atomic<float> peak_l{0}, peak_r{0}; // master, after the clipper, decaying
    std::atomic<uint32_t> ev_dropped{0};

    /** From any thread but the audio one. False: the queue was full. */
    bool Post(const Ev& e);
    /** The audio thread: `frames` frames of kOuts floats, interleaved */
    void Process(float* out, int frames);
    /** Tracks the audio thread is done with, one a call, or nullptr */
    const Track* Trash();

  private:
    struct Deck
    {
        const Track* trk = nullptr;
        double pos = 0, cue = 0, goal = 0, goal_s = 0, jump_to = 0;
        bool   playing = false, cue_held = false, touched = false, jump = false;
        float  speed = 0, bend_acc = 0, bend_btn = 0;
        float  fade = 0, jgain = 1;
        float  g_trim = 1, g_lo = 1, g_mid = 1, g_hi = 1, g_vol = 0, g_x = 1, g_pfl = 0;
        float  filt = 0.5f, peak = 0;
        Eq3    eq[2];
        Svf    svf[2];
    };
    void Handle(const Ev& e);
    void Jump(Deck& d, double to);
    void RunDeck(int n, int frames, float* l, float* r);

    Deck                         deck_[kDecks];
    Ring<Ev, 256>                ev_;
    Ring<const Track*, 16>       trash_;
    std::mutex                   post_mu_;
    float g_master_ = 0, g_cue_ = 0, g_mix_ = 0, pk_l_ = 0, pk_r_ = 0;
};

} // namespace dk
