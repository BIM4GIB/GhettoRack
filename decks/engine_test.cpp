/** Desktop test: the deck engine (engine.h) and the Numark map (numark.h),
 *  with tracks made in memory. No sound card, no files.
 *  Build: g++ -std=c++17 -O2 engine_test.cpp engine.cpp -o build/engine_test */
#include "engine.h"
#include "numark.h"
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
        CHECK(len == 3 * 12, "first lights: %d bytes", len);
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

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
