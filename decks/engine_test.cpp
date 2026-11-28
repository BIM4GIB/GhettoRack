/** Desktop test: the deck engine (engine.h) and the Numark map (numark.h),
 *  with tracks made in memory. No sound card, no files.
 *  Build: g++ -std=c++17 -O2 engine_test.cpp engine.cpp -o build/engine_test */
#include "engine.h"
#include "numark.h"
#include "tempo.h"
#include <chrono>
#include <cstdio>
#include <vector>

using namespace dk;

static int fails = 0, checks = 0;
#define CHECK(c, ...) \
    do \
    { \
        checks++; \
        if(!(c)) \
        { \
            fails++; \
            printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while(0)

struct Tune : Track
{
    std::vector<int16_t> data;
};
static uint32_t next_id = 1;

static Tune* Sine(double hz, double secs, double amp = 0.5)
{
    Tune* t   = new Tune;
    t->frames = (uint32_t)(secs * kRate);
    t->data.resize((size_t)t->frames * 2);
    for(uint32_t i = 0; i < t->frames; i++)
    {
        int16_t v          = (int16_t)(amp * 32767 * std::sin(2 * M_PI * hz * i / kRate));
        t->data[2 * i]     = v;
        t->data[2 * i + 1] = v;
    }
    t->pcm = t->data.data();
    t->id  = next_id++;
    return t;
}

static std::vector<float> Render(Engine& e, double secs, int block = 128)
{
    int                n = (int)(secs * kRate);
    std::vector<float> out((size_t)n * kOuts);
    for(int off = 0; off < n; off += block)
        e.Process(out.data() + (size_t)off * kOuts, std::min(block, n - off));
    return out;
}
static double Rms(const std::vector<float>& b, int ch, double from_s = 0, double to_s = 1e9)
{
    size_t n = b.size() / kOuts, a = (size_t)(from_s * kRate), z = std::min(n, (size_t)(to_s * kRate));
    double s = 0;
    for(size_t i = a; i < z; i++)
        s += (double)b[i * kOuts + ch] * b[i * kOuts + ch];
    return z > a ? std::sqrt(s / (z - a)) : 0;
}
static double Db(double x)
{
    return 20 * std::log10(x + 1e-12);
}
static double Hz(const std::vector<float>& b, int ch, double from_s, double to_s)
{
    size_t a = (size_t)(from_s * kRate), z = (size_t)(to_s * kRate);
    int    n = 0;
    for(size_t i = a + 1; i < z; i++)
        if(b[(i - 1) * kOuts + ch] < 0 && b[i * kOuts + ch] >= 0)
            n++;
    return n / (to_s - from_s);
}
static double MaxStep(const std::vector<float>& b, int ch)
{
    double m = 0;
    for(size_t i = 1; i < b.size() / kOuts; i++)
        m = std::max(m, (double)std::fabs(b[i * kOuts + ch] - b[(i - 1) * kOuts + ch]));
    return m;
}
/** A dance track of sorts: a kick on every beat, a hat between, a bass note, noise over it all */
static Tune* Beat(double bpm, double secs, double first_s = 0.1, double swing_ms = 0, bool hats = true)
{
    Tune* t   = new Tune;
    t->frames = (uint32_t)(secs * kRate);
    t->data.assign((size_t)t->frames * 2, 0);
    std::vector<float> m(t->frames, 0.f);
    const double       beat = 60.0 / bpm * kRate;
    uint32_t           rnd  = 12345;
    auto               noise = [&]() { rnd = rnd * 1664525u + 1013904223u; return (int32_t)(rnd >> 8) / 8388608.f - 1.f; };
    for(int k = 0;; k++)
    {
        const double at = first_s * kRate + k * beat + (k & 1 ? swing_ms * 0.001 * kRate : 0);
        if(at + 1 >= t->frames)
            break;
        for(int i = 0; i < 0.18 * kRate && (size_t)(at + i) < m.size(); i++) // kick: 110 Hz falling to 50, 180 ms
        {
            const double x = i / (double)kRate;
            m[(size_t)at + i] += 0.6f * (float)(std::exp(-x * 22) * std::sin(2 * M_PI * (50 * x + 60 * (1 - std::exp(-x * 30)) / 30)));
        }
        if(hats)
            for(int i = 0; i < 0.03 * kRate && (size_t)(at + beat / 2 + i) < m.size(); i++)
                m[(size_t)(at + beat / 2) + i] += 0.12f * noise() * (float)std::exp(-i / (0.006 * kRate));
    }
    for(uint32_t i = 0; i < t->frames; i++)
    {
        const float v = m[i] + 0.1f * (float)std::sin(2 * M_PI * 55.0 * i / kRate) + 0.01f * noise();
        t->data[2 * i] = t->data[2 * i + 1] = (int16_t)(32767 * std::max(-1.f, std::min(1.f, v)));
    }
    t->pcm = t->data.data();
    t->id  = next_id++;
    return t;
}

static void Post(Engine& e, uint8_t type, int deck, float a = 0, double d = 0, const Track* t = nullptr)
{
    Ev ev;
    ev.type = type, ev.deck = (int8_t)deck, ev.a = a, ev.d = d, ev.track = t;
    e.Post(ev);
}
static void Free(Engine& e)
{
    while(const Track* t = e.Trash())
        delete static_cast<const Tune*>(t);
}

struct Host : NumarkHost
{
    int  browsed = 0, pressed = 0, loaded = -1;
    void Browse(int s, bool) override { browsed += s; }
    void BrowsePress(bool) override { pressed++; }
    void LoadDeck(int d) override { loaded = d; }
};
static void Midi(Numark& n, int a, int b, int c)
{
    uint8_t m[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c};
    n.Feed(m, 3);
}

int main()
{
    const double kSineRms = 0.5 / std::sqrt(2.0);

    { /* nothing loaded: silence, and play does nothing */
        Engine e;
        Post(e, kEvPlay, 0, 1);
        auto b = Render(e, 0.2);
        CHECK(Rms(b, 0) == 0 && Rms(b, 2) == 0, "silence expected");
        CHECK(e.state[0].playing == 0, "playing with no track");
    }

    { /* flat when every knob is at its middle: level in = level out, at any pitch */
        for(double hz : {40.0, 120.0, 250.0, 700.0, 2500.0, 6000.0, 15000.0})
        {
            Engine e;
            Post(e, kEvLoad, 0, 0, 0, Sine(hz, 2));
            Post(e, kEvPlay, 0, 1);
            auto   b  = Render(e, 1.5);
            double db = Db(Rms(b, 0, 0.5, 1.5) / kSineRms);
            CHECK(std::fabs(db) < 0.3, "%.0f Hz comes out %.2f dB off", hz, db);
            CHECK(std::fabs(Rms(b, 0, 0.5, 1.5) - Rms(b, 1, 0.5, 1.5)) < 1e-6, "left and right differ");
            CHECK(Rms(b, 2, 0.5, 1.5) < 1e-6, "headphones without PFL");
        }
    }

    { /* speed: the place moves one frame a frame; the pitch slider changes it */
        Engine e;
        Post(e, kEvLoad, 0, 0, 0, Sine(1000, 10));
        Post(e, kEvPlay, 0, 1);
        Render(e, 1.0);
        double p0 = e.state[0].pos;
        auto   b  = Render(e, 1.0);
        CHECK(std::fabs(e.state[0].pos - p0 - kRate) < 1.0, "moved %.1f in a second", e.state[0].pos - p0);
        CHECK(std::fabs(Hz(b, 0, 0, 1) - 1000) < 2, "%.1f Hz", Hz(b, 0, 0, 1));
        e.knobs[0].rate = 0.08f;
        Render(e, 0.2);
        b = Render(e, 1.0);
        CHECK(std::fabs(Hz(b, 0, 0, 1) - 1080) < 2, "+8%%: %.1f Hz", Hz(b, 0, 0, 1));
        e.knobs[0].rate = -0.5f;
        Render(e, 0.2);
        b = Render(e, 1.0);
        CHECK(std::fabs(Hz(b, 0, 0, 1) - 500) < 2, "-50%%: %.1f Hz", Hz(b, 0, 0, 1));
    }

    { /* start, stop and a jump make no click (a 50 Hz tone: any step shows) */
        Engine e;
        Post(e, kEvLoad, 0, 0, 0, Sine(50, 10, 0.8));
        Post(e, kEvSeek, 0, 0, 0.005 * kRate); // start at the top of the wave
        Post(e, kEvPlay, 0, 1);
        auto b = Render(e, 0.5);
        Post(e, kEvSeek, 0, 0, 3.005 * kRate);
        auto c = Render(e, 0.5);
        Post(e, kEvPlay, 0, 0);
        auto   d    = Render(e, 0.5);
        double tone = 0.8 * 2 * M_PI * 50 / kRate; // the steepest the tone itself gets
        CHECK(MaxStep(b, 0) < 4 * tone, "start: step %.4f", MaxStep(b, 0));
        CHECK(MaxStep(c, 0) < 4 * tone, "jump: step %.4f", MaxStep(c, 0));
        CHECK(MaxStep(d, 0) < 4 * tone, "stop: step %.4f", MaxStep(d, 0));
        CHECK(Rms(d, 0, 0.3, 0.5) < 1e-5, "stopped but not silent");
        CHECK(std::fabs(e.state[0].pos - 3.505 * kRate) < 0.02 * kRate, "after the jump at %.3f s", e.state[0].pos / kRate);
    }

    { /* cue, as on a CD deck */
        Engine e;
        Post(e, kEvLoad, 0, 0, 0, Sine(440, 20));
        Post(e, kEvSeek, 0, 0, 2.0 * kRate);
        Render(e, 0.05);
        Post(e, kEvCue, 0, 1); // stopped away from the cue point: sets it
        Post(e, kEvCue, 0, 0);
        Render(e, 0.05);
        CHECK(e.state[0].cue == 2.0 * kRate, "cue at %.3f", e.state[0].cue / kRate);
        CHECK(e.state[0].playing == 0 && e.state[0].pos == 2.0 * kRate, "setting a cue moved the deck");
        Post(e, kEvCue, 0, 1); // on it: plays while held
        auto b = Render(e, 0.5);
        CHECK(e.state[0].cue_held == 1 && Rms(b, 0, 0.1, 0.5) > 0.3, "no sound while cue is held");
        Post(e, kEvCue, 0, 0); // let go: back and silent
        b = Render(e, 0.3);
        CHECK(e.state[0].pos == 2.0 * kRate && Rms(b, 0, 0.1, 0.3) < 1e-5, "let go: at %.3f", e.state[0].pos / kRate);
        Post(e, kEvCue, 0, 1); // held, then play: keeps going after letting go
        Render(e, 0.2);
        Post(e, kEvPlay, 0, 2);
        Post(e, kEvCue, 0, 0);
        Render(e, 0.3);
        CHECK(e.state[0].playing == 1 && e.state[0].pos > 2.4 * kRate, "play during cue did not latch");
        Post(e, kEvCue, 0, 1); // playing: back to the cue point, stopped
        Post(e, kEvCue, 0, 0);
        Render(e, 0.3);
        CHECK(e.state[0].playing == 0 && e.state[0].pos == 2.0 * kRate, "cue while playing: at %.3f", e.state[0].pos / kRate);
    }

    { /* EQ: a band at 0 is gone, the others stay */
        for(int band = 0; band < 3; band++)
        {
            double hz[3] = {60, 800, 9000}, got[3];
            for(int f = 0; f < 3; f++)
            {
                Engine e;
                (band == 0 ? e.knobs[0].lo : band == 1 ? e.knobs[0].mid : e.knobs[0].hi) = 0.f;
                Post(e, kEvLoad, 0, 0, 0, Sine(hz[f], 2));
                Post(e, kEvPlay, 0, 1);
                auto b = Render(e, 1.5);
                got[f] = Db(Rms(b, 0, 0.5, 1.5) / kSineRms);
            }
            for(int f = 0; f < 3; f++)
                if(f == band)
                    CHECK(got[f] < -30, "band %d killed, %.0f Hz still at %.1f dB", band, hz[f], got[f]);
                else
                    CHECK(std::fabs(got[f]) < 1.5, "band %d killed, %.0f Hz moved %.1f dB", band, hz[f], got[f]);
        }
        Engine e;
        e.knobs[0].lo = 1.f;
        Post(e, kEvLoad, 0, 0, 0, Sine(60, 2, 0.25));
        Post(e, kEvPlay, 0, 1);
        auto b = Render(e, 1.5);
        CHECK(std::fabs(Db(Rms(b, 0, 0.5, 1.5) / (0.25 / std::sqrt(2.0))) - 6.0) < 0.5, "bass full up is not +6 dB");
    }

    { /* filter: left takes the top away, right the bottom, the middle nothing */
        auto level = [&](float knob, double hz) {
            Engine e;
            e.knobs[0].filter = knob;
            Post(e, kEvLoad, 0, 0, 0, Sine(hz, 2));
            Post(e, kEvPlay, 0, 1);
            auto b = Render(e, 1.5);
            return Db(Rms(b, 0, 0.5, 1.5) / kSineRms);
        };
        CHECK(level(0.1f, 8000) < -24 && std::fabs(level(0.1f, 60)) < 1.5, "low-pass: 8k %.1f, 60 Hz %.1f", level(0.1f, 8000), level(0.1f, 60));
        CHECK(level(0.9f, 60) < -24 && std::fabs(level(0.9f, 8000)) < 1.5, "high-pass: 60 Hz %.1f, 8k %.1f", level(0.9f, 60), level(0.9f, 8000));
        CHECK(std::fabs(level(0.51f, 8000)) < 0.01 && std::fabs(level(0.49f, 60)) < 0.01, "dead band is not dead");
    }

    { /* crossfader, faders, headphones */
        Engine e;
        Post(e, kEvLoad, 0, 0, 0, Sine(300, 5));
        Post(e, kEvLoad, 1, 0, 0, Sine(3000, 5));
        Post(e, kEvPlay, 0, 1);
        Post(e, kEvPlay, 1, 1);
        e.xfader = 0.f;
        auto b   = Render(e, 1.0);
        CHECK(std::fabs(Hz(b, 0, 0.5, 1) - 300) < 3, "crossfader left: %.0f Hz", Hz(b, 0, 0.5, 1));
        CHECK(std::fabs(Db(Rms(b, 0, 0.5, 1) / kSineRms)) < 0.3, "crossfader left: deck 1 not full");
        e.xfader = 1.f;
        b        = Render(e, 1.0);
        CHECK(std::fabs(Hz(b, 0, 0.5, 1) - 3000) < 3, "crossfader right: %.0f Hz", Hz(b, 0, 0.5, 1));
        e.xfader          = 0.f;
        e.knobs[0].volume = 0.f;
        e.knobs[0].pfl    = 1.f;
        b                 = Render(e, 1.0);
        CHECK(Rms(b, 0, 0.5, 1) < 1e-5, "fader down but in the master");
        CHECK(std::fabs(Db(Rms(b, 2, 0.5, 1) / kSineRms)) < 0.3 && std::fabs(Hz(b, 3, 0.5, 1) - 300) < 3, "PFL: deck 1 not in the headphones");
        e.cue_mix = 1.f; // headphones on master: deck 1's fader is down, the crossfader shuts deck 2 out
        b         = Render(e, 1.0);
        CHECK(Rms(b, 2, 0.5, 1) < 1e-5, "cue mix at master still has the cue");
        e.master = 0.f;
        e.xfader = 0.5f, e.knobs[0].volume = 1.f;
        b        = Render(e, 1.0);
        CHECK(Rms(b, 0, 0.5, 1) < 1e-5 && Rms(b, 2, 0.5, 1) > 0.2, "master knob down: headphones must keep the mix");
    }

    { /* loud never passes full scale */
        Engine e;
        Post(e, kEvLoad, 0, 0, 0, Sine(100, 3, 0.99));
        Post(e, kEvLoad, 1, 0, 0, Sine(133, 3, 0.99));
        Post(e, kEvPlay, 0, 1);
        Post(e, kEvPlay, 1, 1);
        e.knobs[0].gain = e.knobs[1].gain = 1.f;
        e.master                          = 1.f;
        auto  b = Render(e, 2.0);
        float m = 0;
        for(float v : b)
        {
            m = std::max(m, std::fabs(v));
            if(v != v)
                m = 99;
        }
        CHECK(m <= 1.0f, "peak %.3f", m);
        CHECK(e.peak_l > 0.9f, "master meter %.2f", e.peak_l.load());
    }

    { /* scratch: the record follows the hand, and stands still under a still hand */
        Engine e;
        Post(e, kEvLoad, 0, 0, 0, Sine(440, 30));
        Post(e, kEvSeek, 0, 0, 5.0 * kRate);
        Post(e, kEvPlay, 0, 1);
        Render(e, 0.5);
        Post(e, kEvTouch, 0, 1);
        auto   b  = Render(e, 0.5);
        double p0 = e.state[0].pos;
        CHECK(Rms(b, 0, 0.2, 0.5) < 1e-5, "hand on the record: still sounding");
        CHECK(std::fabs(e.state[0].speed) < 1e-3, "hand on the record: speed %.3f", e.state[0].speed.load());
        for(int i = 0; i < 180; i++) // one turn forward in 1.8 s: as fast as it plays
        {
            for(int k = 0; k < 4; k++)
                Post(e, kEvJog, 0, i % 45 == 0 && k == 0 ? 5.f + 1.6f / 4 : 1.4f); // 1024 ticks in all, lumpy
            Render(e, 0.01);
        }
        double moved = e.state[0].pos - p0;
        (void)moved;
        Render(e, 0.1);
        moved = e.state[0].pos - p0;
        double ticks = 180 * (3 * 1.4) + (180 - 4) * 1.4 + 4 * 5.4;
        CHECK(std::fabs(moved - ticks * kJogFramesPerTick) < 2.0, "moved %.0f, hand %.0f", moved, ticks * kJogFramesPerTick);
        float lo = 0, hi = -99;
        for(int i = 0; i < 170; i++) // and back, 3.5 times as fast, the ticks in lumps a sound-card block apart
        {
            Post(e, kEvJog, 0, -6);
            b = Render(e, 128.0 / kRate);
            if(i > 40)
                lo = std::min(lo, e.state[0].speed.load()), hi = std::max(hi, e.state[0].speed.load());
        }
        const float want = -6 * (float)kJogFramesPerTick / 128;
        CHECK(lo > want * 1.1f && hi < want * 0.9f, "backwards: speed %.2f to %.2f, hand %.2f", hi, lo, want);
        CHECK(Rms(b, 0) > 0.1, "backwards: no sound");
        Post(e, kEvTouch, 0, 0); // let go: it plays on
        Render(e, 0.2);
        CHECK(std::fabs(e.state[0].speed - 1.f) < 0.01 && e.state[0].playing == 1, "let go: speed %.3f", e.state[0].speed.load());

        e.knobs[0].scratch = 0.f; // scratch off: the platter only nudges
        double q0 = e.state[0].pos;
        Post(e, kEvTouch, 0, 1);
        for(int i = 0; i < 20; i++)
        {
            Post(e, kEvJog, 0, 5);
            Render(e, 0.01);
        }
        Post(e, kEvTouch, 0, 0);
        Render(e, 1.8);
        double ahead = e.state[0].pos - q0 - 2.0 * kRate;
        CHECK(ahead > 0.01 * kRate && ahead < 0.2 * kRate, "nudge: %.0f frames ahead", ahead);
        CHECK(std::fabs(e.state[0].speed - 1.f) < 0.002, "nudge did not wear off: %.4f", e.state[0].speed.load());
    }

    { /* the ends of a track, and swapping one out */
        Engine e;
        Tune*  a = Sine(440, 0.5);
        Post(e, kEvLoad, 0, 0, 0, a);
        Post(e, kEvPlay, 0, 1);
        auto b = Render(e, 1.0);
        CHECK(e.state[0].playing == 0 && e.state[0].pos == a->frames, "did not stop at the end");
        CHECK(Rms(b, 0, 0.7, 1.0) < 1e-5, "sound after the end");
        Post(e, kEvPlay, 0, 1);
        Render(e, 0.1);
        CHECK(e.state[0].playing == 0, "play at the end");
        Post(e, kEvSeek, 0, 0, 100);
        Post(e, kEvTouch, 0, 1);
        Post(e, kEvJog, 0, -100);
        Render(e, 0.3);
        CHECK(e.state[0].pos == 0, "scratched past the start: %.1f", e.state[0].pos.load());
        Post(e, kEvTouch, 0, 0);
        CHECK(e.Trash() == nullptr, "trash before a swap");
        Tune* c = Sine(880, 0.5);
        Post(e, kEvLoad, 0, 0, 0, c);
        Render(e, 0.05);
        CHECK(e.state[0].track == c->id && e.state[0].pos == 0, "swap");
        const Track* old = e.Trash();
        CHECK(old == a, "the old track did not come back");
        delete static_cast<const Tune*>(old);
        Post(e, kEvLoad, 0);
        Render(e, 0.05);
        CHECK(e.state[0].frames == 0, "eject");
        Free(e);
    }

    { /* the Numark: what it sent the Pi, and what the map makes of it */
        Engine e;
        Host   h;
        Numark n(e, &h);
        Post(e, kEvLoad, 0, 0, 0, Sine(440, 20));
        Post(e, kEvLoad, 1, 0, 0, Sine(440, 20));
        Render(e, 0.05);
        Midi(n, 0x90, 0x00, 0x7F);
        Midi(n, 0x80, 0x00, 0x00);
        Render(e, 0.05);
        CHECK(e.state[0].playing == 1 && e.state[1].playing == 0, "play, deck 1");
        Midi(n, 0x90, 0x00, 0x7F);
        Render(e, 0.05);
        CHECK(e.state[0].playing == 0, "play again stops");
        Midi(n, 0xB1, 0x09, 0x40), Midi(n, 0xB1, 0x29, 0x00);
        CHECK(e.knobs[1].rate == 0.f, "pitch centre: %.4f", e.knobs[1].rate.load());
        Midi(n, 0xB1, 0x09, 0x7F), Midi(n, 0xB1, 0x29, 0x7F);
        CHECK(std::fabs(e.knobs[1].rate + 0.08f) < 1e-4, "pitch at the far end: %.4f", e.knobs[1].rate.load());
        Midi(n, 0xB1, 0x09, 0x00), Midi(n, 0xB1, 0x29, 0x00);
        CHECK(std::fabs(e.knobs[1].rate - 0.08f) < 1e-4, "pitch at the near end: %.4f", e.knobs[1].rate.load());
        Midi(n, 0x91, 0x2B, 0x7F);
        CHECK(std::fabs(e.knobs[1].rate - 0.16f) < 1e-4, "range 16%%: %.4f", e.knobs[1].rate.load());
        Midi(n, 0xBF, 0x08, 0x00);
        CHECK(e.xfader == 0.f, "crossfader");
        Midi(n, 0xB0, 0x19, 0x00), Midi(n, 0xB0, 0x1A, 0x7F), Midi(n, 0xB0, 0x1C, 0x40), Midi(n, 0xB0, 0x16, 0x7F);
        CHECK(e.knobs[0].lo == 0.f && e.knobs[0].filter == 1.f && e.knobs[0].gain == 1.f, "strip knobs");
        CHECK(std::fabs(e.knobs[0].volume - 64 / 127.f) < 1e-6, "fader");
        Midi(n, 0xBE, 0x23, 0x7F), Midi(n, 0xBE, 0x27, 0x7F), Midi(n, 0xBE, 0x2F, 0x00);
        CHECK(e.master == 1.f && e.cue_mix == 1.f && e.cue_gain == 0.f, "master section");
        Midi(n, 0x90, 0x1B, 0x7F), Midi(n, 0x80, 0x1B, 0x00);
        CHECK(e.knobs[0].pfl == 1.f, "PFL");
        Midi(n, 0x90, 0x07, 0x7F);
        CHECK(e.knobs[0].scratch == 0.f, "scratch off");
        Midi(n, 0x90, 0x07, 0x7F);
        Midi(n, 0xBF, 0x00, 0x01), Midi(n, 0xBF, 0x00, 0x7F), Midi(n, 0xBF, 0x00, 0x7E);
        CHECK(h.browsed == -2, "browse: %d", h.browsed);
        Midi(n, 0x9F, 0x03, 0x7F), Midi(n, 0x9F, 0x06, 0x7F);
        CHECK(h.loaded == 1 && h.pressed == 1, "load and browse press");
        /* platter: touch and turn scratches; with shift it searches */
        Render(e, 0.1);
        double before = e.state[0].pos;
        Midi(n, 0x90, 0x06, 0x7F);
        for(int i = 0; i < 50; i++)
        {
            Midi(n, 0xB0, 0x06, 0x05);
            Render(e, 0.005);
        }
        Render(e, 0.1);
        CHECK(std::fabs(e.state[0].pos - before - 250 * kJogFramesPerTick) < 2, "platter: moved %.0f", e.state[0].pos - before);
        before = e.state[0].pos;
        Midi(n, 0x80, 0x06, 0x00);
        Midi(n, 0x90, 0x20, 0x7F);
        Midi(n, 0xB0, 0x06, 0x7E); // two ticks back
        Midi(n, 0x80, 0x20, 0x00);
        Render(e, 0.05);
        CHECK(std::fabs(e.state[0].pos - (before - 0.1 * kRate)) < 2, "search: at %.0f", e.state[0].pos.load());
        /* lights */
        uint8_t out[256];
        int     len = n.Lights(out, sizeof(out));
        CHECK(len == 3 * (12 + 26), "first lights: %d bytes", len); // 5 and a meter a deck, 13 in each pad section
        CHECK(n.Lights(out, sizeof(out)) == 0, "lights sent twice");
        Midi(n, 0x91, 0x00, 0x7F);
        Render(e, 0.05);
        len = n.Lights(out, sizeof(out));
        bool play2 = false;
        for(int i = 0; i + 2 < len; i += 3)
            if(out[i] == 0x91 && out[i + 1] == 0x00 && out[i + 2] == 0x7F)
                play2 = true;
        CHECK(play2, "deck 2 play light");
        Free(e);
    }

    /* hot cues */
    {
        Engine e;
        e.knobs[0].volume = 1, e.master = 0.8f;
        Tune* t = Sine(1000, 20);
        Post(e, kEvLoad, 0, 0, 0, t);
        Post(e, kEvSeek, 0, 0, 5.0 * kRate);
        Render(e, 0.05);
        Post(e, kEvHot, 0, 2, 1); // empty, stopped: set here, nothing plays
        Post(e, kEvHot, 0, 2, 0);
        auto b = Render(e, 0.2);
        CHECK(e.state[0].hot[2] == 5.0 * kRate && e.state[0].hot[0] < 0, "hot cue 3 at %.3f s", e.state[0].hot[2] / kRate);
        CHECK(Rms(b, 0) < 1e-6 && e.state[0].pos == 5.0 * kRate, "setting a hot cue made sound or moved the deck");
        Post(e, kEvSeek, 0, 0, 1.0 * kRate);
        Render(e, 0.05);
        Post(e, kEvHot, 0, 2, 1); // set, stopped: plays from it while held
        b = Render(e, 0.5);
        CHECK(Rms(b, 0, 0.1, 0.5) > 0.3 && std::fabs(e.state[0].pos - 5.5 * kRate) < 0.02 * kRate, "held hot cue: at %.3f s", e.state[0].pos / kRate);
        CHECK(e.state[0].playing == 0, "a held hot cue is not play");
        Post(e, kEvHot, 0, 2, 0); // let go: back on it, stopped
        b = Render(e, 0.3);
        CHECK(e.state[0].pos == 5.0 * kRate && Rms(b, 0, 0.1, 0.3) < 1e-5, "let go: at %.3f s", e.state[0].pos / kRate);
        Post(e, kEvHot, 0, 2, 1); // held, then play: it runs on
        Render(e, 0.1);
        Post(e, kEvPlay, 0, 1);
        Post(e, kEvHot, 0, 2, 0);
        Render(e, 0.4);
        CHECK(e.state[0].playing == 1 && std::fabs(e.state[0].pos - 5.5 * kRate) < 0.02 * kRate, "play during a held hot cue: at %.3f s", e.state[0].pos / kRate);
        /* playing: there at once, no gap and no click (a 1 kHz tone into itself at another phase) */
        Post(e, kEvSeek, 0, 0, 10.0 * kRate + 11);
        Render(e, 0.3);
        Post(e, kEvHot, 0, 5, 1);
        Post(e, kEvHot, 0, 5, 0);
        Render(e, 1.0);
        const double tone = 0.5 * 2 * M_PI * 1000 / kRate;
        Post(e, kEvHot, 0, 2, 1);
        Post(e, kEvHot, 0, 2, 0);
        b = Render(e, 0.5);
        CHECK(std::fabs(e.state[0].pos - 5.5 * kRate) < 0.01 * kRate && e.state[0].playing == 1, "hot cue while playing: at %.3f s", e.state[0].pos / kRate);
        CHECK(MaxStep(b, 0) < 2.2 * tone, "hot cue while playing: step %.4f against %.4f", MaxStep(b, 0), tone);
        double low = 1;
        for(double x = 0; x < 0.02; x += 0.002)
            low = std::min(low, Rms(b, 0, x, x + 0.002));
        CHECK(low > 0.5 * kSineRms * 0.64, "hot cue while playing: a hole, %.3f", low);
        Post(e, kEvHotClear, 0, 2);
        Render(e, 0.05);
        CHECK(e.state[0].hot[2] < 0 && e.state[0].hot[5] >= 0, "clear took the wrong one");
        Post(e, kEvHotSet, 0, 7, 3.25 * kRate); // what the file remembered
        Post(e, kEvHotSet, 0, 6, 1e12);         // nonsense: not taken
        Render(e, 0.05);
        CHECK(e.state[0].hot[7] == std::floor(3.25 * kRate + 0.5) && e.state[0].hot[6] < 0, "restored hot cues");
        Tune* t2 = Sine(500, 5);
        Post(e, kEvPlay, 0, 0);
        Post(e, kEvLoad, 0, 0, 0, t2);
        Render(e, 0.05);
        CHECK(e.state[0].hot[5] < 0 && e.state[0].hot[7] < 0, "another track kept the hot cues");
        Free(e);
        Post(e, kEvLoad, 0);
        Render(e, 0.05);
        Free(e);
    }

    /* loops */
    {
        Engine e;
        e.knobs[0].volume = 1, e.master = 0.8f;
        Tune* t = Sine(1000, 30);
        t->bpm  = 120; // a beat is half a second
        Post(e, kEvLoad, 0, 0, 0, t);
        Post(e, kEvSeek, 0, 0, 2.0 * kRate);
        Post(e, kEvPlay, 0, 1);
        Render(e, 0.5);
        CHECK(e.state[0].bpm == 120, "the track's tempo: %.2f", e.state[0].bpm.load());
        Post(e, kEvLoopIn, 0);
        Render(e, 1.0);
        CHECK(e.state[0].loop_on == 0 && e.state[0].loop_in > 0 && e.state[0].loop_out < 0, "loop in alone");
        const double in = e.state[0].loop_in;
        Post(e, kEvLoopOut, 0);
        auto b = Render(e, 3.3);
        const double len = e.state[0].loop_out - in;
        CHECK(e.state[0].loop_on == 1 && std::fabs(len - kRate) < 200, "loop of %.3f s", len / kRate);
        CHECK(e.state[0].pos >= in && e.state[0].pos < in + len, "outside the loop: %.3f s", e.state[0].pos / kRate);
        CHECK(std::fabs(std::fmod(e.state[0].pos - in, len) - std::fmod(3.3 * kRate, len)) < 200, "the loop does not keep time");
        const double tone = 0.5 * 2 * M_PI * 1000 / kRate;
        CHECK(MaxStep(b, 0) < 2.2 * tone, "loop wrap: step %.4f against %.4f", MaxStep(b, 0), tone);
        CHECK(Rms(b, 0, 0.1, 3.3) > 0.9 * kSineRms * 0.64, "loop: holes in the sound");
        Post(e, kEvLoopScale, 0, 0.5f);
        Render(e, 1.0);
        CHECK(std::fabs(e.state[0].loop_out - in - len / 2) <= 1 && e.state[0].pos < in + len / 2, "halved");
        for(int i = 0; i < 20; i++)
            Post(e, kEvLoopScale, 0, 0.5f);
        Render(e, 0.2);
        CHECK(e.state[0].loop_out - in >= kLoopMin, "halved to %.0f frames", e.state[0].loop_out - in);
        b = Render(e, 0.5);
        CHECK(Rms(b, 0) > 0.8 * kSineRms * 0.64, "a very short loop: holes in the sound");
        for(int i = 0; i < 30; i++)
            Post(e, kEvLoopScale, 0, 2.f);
        Render(e, 0.2);
        CHECK(e.state[0].loop_out <= t->frames && e.state[0].loop_out - in > 10.0 * kRate, "doubled to %.2f s", (e.state[0].loop_out - in) / kRate);
        Post(e, kEvLoop, 0, 0); // out of it: the record runs on
        Render(e, 0.1);
        const double out = e.state[0].loop_out;
        Post(e, kEvSeek, 0, 0, out - 0.2 * kRate);
        Render(e, 0.6);
        CHECK(e.state[0].loop_on == 0 && e.state[0].pos > out, "left the loop but it wrapped");
        Post(e, kEvLoop, 0, 1); // into it again
        Render(e, 0.1);
        CHECK(e.state[0].loop_on == 1 && e.state[0].pos >= in && e.state[0].pos < in + 0.2 * kRate, "reloop: at %.3f s", e.state[0].pos / kRate);
        Post(e, kEvLoop, 0, 2);
        Render(e, 0.05);
        CHECK(e.state[0].loop_on == 0, "toggle off");
        /* by the beat */
        Post(e, kEvSeek, 0, 0, 20.0 * kRate);
        Render(e, 0.1);
        Post(e, kEvLoop, 0, 2, 2); // two beats from here
        Render(e, 0.05);
        CHECK(e.state[0].loop_on == 1 && std::fabs(e.state[0].loop_out - e.state[0].loop_in - 1.0 * kRate) <= 1, "2 beats at 120: %.3f s",
              (e.state[0].loop_out - e.state[0].loop_in) / kRate);
        Post(e, kEvGrid, 0, 0, 0); // tempo not known: no loop by the beat, the old one stays
        Post(e, kEvLoop, 0, 0);
        Post(e, kEvLoop, 0, 2, 4);
        Render(e, 0.05);
        CHECK(e.state[0].loop_on == 0 && e.state[0].bpm == 0, "a loop by the beat without a tempo");
        Post(e, kEvGrid, 0, 126, 1234);
        Render(e, 0.05);
        CHECK(e.state[0].bpm == 126 && e.state[0].beat0 == 1234, "grid");
        /* a platter in hand goes through the loop's end */
        Post(e, kEvPlay, 0, 0);
        Post(e, kEvLoad, 0);
        Render(e, 0.05);
        Free(e);
    }

    /* the Numark's pads and loop buttons */
    {
        Engine e;
        Host   h;
        Numark n(e, &h);
        Tune*  t = Sine(440, 30);
        t->bpm   = 120;
        Post(e, kEvLoad, 1, 0, 0, t);
        Post(e, kEvSeek, 1, 0, 4.0 * kRate);
        Render(e, 0.05);
        uint8_t out[256];
        n.Lights(out, sizeof(out));
        Midi(n, 0x95, 0x16, 0x7F); // deck 2, pad 3
        Midi(n, 0x95, 0x16, 0x00);
        Render(e, 0.05);
        CHECK(e.state[1].hot[2] == 4.0 * kRate && e.state[0].hot[2] < 0, "pad 3 on deck 2");
        int  len = n.Lights(out, sizeof(out));
        bool lit = false;
        for(int i = 0; i + 2 < len; i += 3)
            lit |= out[i] == 0x95 && out[i + 1] == 0x16 && out[i + 2] == 0x7F;
        CHECK(lit, "pad 3's light");
        Midi(n, 0x95, 0x0D, 0x7F); // AUTO LOOP mode: pads do nothing yet, their lights go, the mode's light comes
        Midi(n, 0x95, 0x14, 0x7F);
        Midi(n, 0x95, 0x14, 0x00);
        Render(e, 0.05);
        len       = n.Lights(out, sizeof(out));
        bool mode = false, off = false;
        for(int i = 0; i + 2 < len; i += 3)
        {
            mode |= out[i] == 0x95 && out[i + 1] == 0x0D && out[i + 2] == 0x7F;
            off |= out[i] == 0x95 && out[i + 1] == 0x16 && out[i + 2] == 0x01;
        }
        CHECK(e.state[1].hot[0] < 0 && mode && off, "AUTO LOOP mode");
        Midi(n, 0x95, 0x00, 0x7F); // CUE mode again
        Midi(n, 0x95, 0x1E, 0x7F); // shift + pad 3: cleared
        Render(e, 0.05);
        CHECK(e.state[1].hot[2] < 0, "shift + pad clears");
        Midi(n, 0x95, 0x40, 0x7F); // LOOP with none: 4 beats from here
        Render(e, 0.05);
        CHECK(e.state[1].loop_on == 1 && std::fabs(e.state[1].loop_out - e.state[1].loop_in - 2.0 * kRate) <= 1, "LOOP: 4 beats");
        len       = n.Lights(out, sizeof(out));
        bool loop = false;
        for(int i = 0; i + 2 < len; i += 3)
            loop |= out[i] == 0x95 && out[i + 1] == 0x40 && out[i + 2] == 0x7F;
        CHECK(loop, "LOOP's light");
        Midi(n, 0x95, 0x34, 0x7F);
        Render(e, 0.05);
        CHECK(std::fabs(e.state[1].loop_out - e.state[1].loop_in - 1.0 * kRate) <= 1, "1/2");
        Midi(n, 0x95, 0x35, 0x7F);
        Midi(n, 0x95, 0x35, 0x7F);
        Render(e, 0.05);
        CHECK(std::fabs(e.state[1].loop_out - e.state[1].loop_in - 4.0 * kRate) <= 1, "x2");
        Midi(n, 0x95, 0x40, 0x7F);
        Render(e, 0.05);
        CHECK(e.state[1].loop_on == 0, "LOOP off");
        Midi(n, 0x91, 0x00, 0x7F); // play, then loop in and out by hand
        Render(e, 0.5);
        Midi(n, 0x95, 0x36, 0x7F);
        Render(e, 0.75);
        Midi(n, 0x95, 0x37, 0x7F);
        Render(e, 0.1);
        CHECK(e.state[1].loop_on == 1 && std::fabs(e.state[1].loop_out - e.state[1].loop_in - 0.75 * kRate) < 300, "loop in, loop out: %.3f s",
              (e.state[1].loop_out - e.state[1].loop_in) / kRate);
        Midi(n, 0x95, 0x40, 0x7F);
        Render(e, 1.0);
        Midi(n, 0x95, 0x41, 0x7F); // shift + LOOP: into it again
        Render(e, 0.05);
        CHECK(e.state[1].loop_on == 1 && e.state[1].pos < e.state[1].loop_out, "shift + LOOP");
        uint8_t hello[32];
        CHECK(Numark::Hello(hello) == 19 && hello[10] == 0x03 && hello[11] == 0xF7, "hello: fader cuts handed to the controller");
        Midi(n, 0x91, 0x00, 0x7F);
        Post(e, kEvLoad, 1);
        Render(e, 0.05);
        Free(e);
    }

    /* tempo */
    {
        const struct
        {
            double bpm, secs, first, want;
        } cases[] = {{128, 90, 0.1, 128}, {120, 60, 0.37, 120}, {146, 120, 0.02, 146}, {174, 60, 0.2, 87}, {133.33, 180, 0.25, 133.33},
                     {100, 45, 0.05, 100}, {125.5, 240, 0.3, 125.5}, {140, 20, 0.11, 140}};
        for(const auto& c : cases)
        {
            Tune* t  = Beat(c.bpm, c.secs, c.first);
            auto  t0 = std::chrono::steady_clock::now();
            Tempo r  = FindTempo(t->pcm, t->frames);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            CHECK(std::fabs(r.bpm - c.want) < 0.02, "%.2f bpm, %.0f s: found %.3f (sure %.1f, %.0f ms)", c.bpm, c.secs, r.bpm, r.sure, ms);
            /* a beat line within 6 ms of a kick, all the way through: the last one too */
            /* (at half the tempo either set of kicks is as good: measured against the kicks' own spacing) */
            const double period = 60.0 / c.bpm * kRate, kick = period;
            double       worst = 0;
            for(double k : {0.0, std::floor((c.secs - 1) * kRate / kick)}) // kick: every beat the answer counts
            {
                const double at  = c.first * kRate + k * kick;
                const double off = std::fmod(at - r.beat0 + period * 1000.5, period) - period / 2;
                worst            = std::max(worst, std::fabs(off));
            }
            CHECK(r.bpm > 0 && worst < 0.006 * kRate, "%.2f bpm: beat line %.1f ms off a kick", c.bpm, worst * 1000 / kRate);
            if(c.secs >= 240)
                printf("  tempo of a %.0f s track: %.0f ms\n", c.secs, ms);
            delete t;
        }
        Tune* s = Sine(440, 30); // no beat at all
        CHECK(FindTempo(s->pcm, s->frames).bpm == 0, "a steady tone has a tempo: %.2f", FindTempo(s->pcm, s->frames).bpm);
        delete s;
        Tune* nz = Sine(440, 30, 0); // noise: plenty going on, no beat in it
        uint32_t rnd = 99;
        for(auto& v : nz->data)
            rnd = rnd * 1664525u + 1013904223u, v = (int16_t)((int32_t)(rnd >> 16) - 32768) / 4;
        CHECK(FindTempo(nz->pcm, nz->frames).bpm == 0, "noise has a tempo: %.2f (sure %.2f)", FindTempo(nz->pcm, nz->frames).bpm, FindTempo(nz->pcm, nz->frames).sure);
        delete nz;
        Tune* z = Sine(440, 30, 0);
        CHECK(FindTempo(z->pcm, z->frames).bpm == 0, "silence has a tempo");
        delete z;
        Tune* sh = Beat(128, 4);
        CHECK(FindTempo(sh->pcm, sh->frames).bpm == 0, "4 seconds are enough for a tempo");
        delete sh;
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
