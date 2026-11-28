/** The Numark Mixtrack Pro FX on the decks: what each control does, and its lights.
 *
 *  This is the one place that knows the controller's MIDI. Feed() takes one
 *  complete message from it; Lights() gives the messages that bring its
 *  lights in step with the engine. Nothing here opens a device: the bytes
 *  come and go through whoever owns the port (the bridge).
 *
 *  The numbers were read off the controller on the Pi (its answer to a state
 *  request, 2026-11-27) and agree with Mixxx's published mapping for it,
 *  which is where the ones a state request does not show (buttons, jog,
 *  lights) come from. Lights: 0x7F on, 0x01 dim.
 *
 *  Channels: 1, 2 = deck transport and strip; 5, 6 = pads, their mode buttons
 *  and the loop buttons; 9, 10 = FX; 15 = master section; 16 = browse.
 *
 *  SYNC: the deck takes the other deck's tempo (tempo only: the beats are
 *  lined up by hand, with the platter) until SYNC is pressed again; shift +
 *  SYNC ends it too. Refused while either track's tempo is not known.
 *
 *  FX: six buttons choose the effect for both decks (HPF, LPF, FLANGER on
 *  channel 9 as notes 0 to 2, ECHO, REVERB, PHASER on channel 10 as notes 3
 *  to 5: the order is taken from the panel, the mapping only says there are
 *  six). A deck's paddle switches it in (up: stays, down: while held; CC 3 on
 *  channel 9 / 10, value 0 off). The wet / dry knob is CC 4 on channel 9, the
 *  beats knob CC 5 there, turning one way doubles and the other halves.
 *
 *  Pads: four mode buttons a deck. CUE: a pad sets its hot cue where the
 *  record is, or goes to it; shift + pad clears it. AUTO LOOP: a pad makes a
 *  loop of 1/16 to 8 beats from here (or changes the running one's length),
 *  the lit pad ends it; shift + pad loops while held, and let go the record
 *  is where it would have been. SAMPLE does nothing yet. FADER CUTS is the
 *  controller's own doing (it cuts its own sound, the engine hears nothing of
 *  it). Loop buttons: LOOP ends the running loop, or makes a new one from
 *  here, as long as the last (4 beats at first; needs the tempo); shift +
 *  LOOP goes into the last loop again; 1/2 and x2 its length (or, with none
 *  running, what LOOP makes next); shift + 1/2 loop in, shift + x2 loop out. */
#pragma once
#include "engine.h"
#include <algorithm>

namespace dk
{
struct NumarkHost
{
    virtual void Browse(int steps, bool shift) = 0; // the browse knob turned
    virtual void BrowsePress(bool shift)       = 0;
    virtual void LoadDeck(int deck)            = 0; // a LOAD button
    virtual ~NumarkHost() {}
};

class Numark
{
  public:
    static constexpr float kRanges[3] = {0.08f, 0.16f, 0.5f}; // pitch slider, end to end
    static constexpr float kBend      = 0.04f;                // the pitch-bend buttons

    Numark(Engine& e, NumarkHost* h) : e_(e), host_(h) { ForgetLights(); }

    enum PadMode { kPadCue, kPadLoop, kPadCuts, kPadSample };

    int  range[kDecks] = {0, 0}; // index into kRanges
    int  pads[kDecks]  = {kPadCue, kPadCue};
    bool shift         = false;

    /** What to send when the controller appears: stop its demo, then ask for
     *  where every knob and fader stands. Returns the number of bytes. */
    static int Hello(uint8_t* out)
    {
        static const uint8_t m[] = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7,        // out of its demo
                                    0xF0, 0x00, 0x20, 0x7F, 0x03, 0xF7,        // FADER CUTS: its own to do
                                    0xF0, 0x00, 0x20, 0x7F, 0x03, 0x01, 0xF7}; // where is everything?
        for(unsigned i = 0; i < sizeof(m); i++)
            out[i] = m[i];
        return sizeof(m);
    }
    /** And when the module is left: back to its demo */
    static int Bye(uint8_t* out)
    {
        static const uint8_t m[] = {0xF0, 0x00, 0x20, 0x7F, 0x02, 0xF7};
        for(unsigned i = 0; i < sizeof(m); i++)
            out[i] = m[i];
        return sizeof(m);
    }

    void Feed(const uint8_t* m, int n)
    {
        if(n < 3 || m[0] < 0x80 || m[0] >= 0xF0)
            return;
        const int  type = m[0] & 0xF0, ch = m[0] & 15, a = m[1], v = m[2];
        const bool down = type == 0x90 && v > 0;
        const bool up   = type == 0x80 || (type == 0x90 && v == 0);
        if(type == 0xB0)
            return Knob(ch, a, v);
        if(!down && !up)
            return;
        if(ch == 15)
        {
            if(!down || !host_)
                return;
            if(a == 0x02 || a == 0x03)
                host_->LoadDeck(a - 0x02);
            else if(a == 0x06 || a == 0x07)
                host_->BrowsePress(a == 0x07);
            return;
        }
        if(ch == 4 || ch == 5)
            return Pads(ch - 4, a, down);
        if(ch == 8 || ch == 9)
        {
            if(down && a <= 0x05)
                e_.fx_kind = a; // (0x09, TAP, is not used)
            return;
        }
        if(ch > 1)
            return;
        const int d = ch;
        switch(a)
        {
            case 0x00: // play
                if(down)
                    Post(kEvPlay, d, 2);
                break;
            case 0x04: // shift + play: from the cue point, playing
                if(down)
                {
                    Post(kEvSeek, d, 0, e_.state[d].cue.load());
                    Post(kEvPlay, d, 1);
                }
                break;
            case 0x01: Post(kEvCue, d, down ? 1 : 0); break;
            case 0x05: // shift + cue: to the start, stopped
                if(down)
                {
                    Post(kEvPlay, d, 0);
                    Post(kEvSeek, d, 0, 0);
                }
                break;
            case 0x06: Post(kEvTouch, d, down ? 1 : 0); break;
            case 0x07: // scratch on / off
                if(down)
                    e_.knobs[d].scratch = e_.knobs[d].scratch.load() > 0.5f ? 0.f : 1.f;
                break;
            case 0x0B: Post(kEvBend, d, down ? kBend : 0); break;
            case 0x0C: Post(kEvBend, d, down ? -kBend : 0); break;
            case 0x2B: // shift + pitch bend: the slider's range
            case 0x2C:
                if(down)
                {
                    range[d] = (range[d] + (a == 0x2B ? 1 : 2)) % 3;
                    SetRate(d);
                }
                break;
            case 0x1B: // headphones
                if(down)
                    e_.knobs[d].pfl = e_.knobs[d].pfl.load() > 0.5f ? 0.f : 1.f;
                break;
            case 0x20: shift = down; break;
            case 0x02: // SYNC: on (the other deck's goes off: one follows, one leads), or off again
                if(down)
                {
                    const bool on = e_.knobs[d].sync.load() < 0.5f;
                    if(on && (e_.state[d].bpm.load() <= 0 || e_.state[1 - d].bpm.load() <= 0))
                        break; // a tempo is missing: nothing to match
                    e_.knobs[d].sync = on ? 1.f : 0.f;
                    if(on)
                        e_.knobs[1 - d].sync = 0.f;
                }
                break;
            case 0x03: // shift + SYNC
                if(down)
                    e_.knobs[d].sync = 0.f;
                break;
            default: break;
        }
    }

    /** Messages that bring the lights in step (3 bytes each). Call about 30
     *  times a second; only what changed is sent. Returns the number of bytes. */
    int Lights(uint8_t* out, int cap)
    {
        int n = 0;
        for(int d = 0; d < kDecks; d++)
        {
            const DeckState& s      = e_.state[d];
            const bool       loaded = s.frames.load() > 0;
            const bool       playing = s.playing.load() != 0;
            const bool       at_cue = loaded && !playing && std::fabs(s.pos.load() - s.cue.load()) < 1.0;
            const uint8_t    want[kLights] = {
                (uint8_t)(playing ? 0x7F : 0x01),
                (uint8_t)(s.cue_held.load() || at_cue ? 0x7F : 0x01),
                (uint8_t)(s.synced.load() ? 0x7F : 0x01),
                (uint8_t)(e_.knobs[d].scratch.load() > 0.5f ? 0x7F : 0x01),
                (uint8_t)(e_.knobs[d].pfl.load() > 0.5f ? 0x7F : 0x01),
            };
            static const uint8_t note[kLights] = {0x00, 0x01, 0x02, 0x07, 0x1B};
            for(int i = 0; i < kLights; i++)
                if(want[i] != sent_[d][i] && n + 3 <= cap)
                {
                    out[n++]    = 0x90 | d;
                    out[n++]    = note[i];
                    out[n++]    = want[i];
                    sent_[d][i] = want[i];
                }
            /* the strip's meter: 0..90, the level before the fader */
            const float   p     = std::min(1.f, s.peak.load());
            const uint8_t level = (uint8_t)(90.f * std::sqrt(p) + 0.5f);
            if(level != meter_[d] && n + 3 <= cap)
            {
                out[n++]  = 0xB0 | d;
                out[n++]  = 0x1F;
                out[n++]  = level;
                meter_[d] = level;
            }
            /* the pad section: the mode in use, in CUE the pads that hold a cue, and LOOP while one runs */
            uint8_t pw[kPadLights];
            for(int i = 0; i < 4; i++)
                pw[i] = pads[d] == i ? 0x7F : 0x01;
            for(int i = 0; i < kHot; i++)
                pw[4 + i] = (pads[d] == kPadCue && s.hot[i].load() >= 0)
                                    || (pads[d] == kPadLoop && s.loop_on.load() && std::fabs(s.beats.load() - kAuto[i]) < 1e-4f)
                                ? 0x7F
                                : 0x01;
            pw[12] = s.loop_on.load() ? 0x7F : 0x01;
            static const uint8_t pnote[kPadLights] = {0x00, 0x0D, 0x07, 0x0B, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x40};
            for(int i = 0; i < kPadLights; i++)
                if(pw[i] != psent_[d][i] && n + 3 <= cap)
                {
                    out[n++]     = 0x94 | d;
                    out[n++]     = pnote[i];
                    out[n++]     = pw[i];
                    psent_[d][i] = pw[i];
                }
        }
        const int kind = e_.fx_kind.load();
        for(int i = 0; i < kFxCount; i++) // the six FX buttons: the chosen one
        {
            const uint8_t want = i == kind ? 0x7F : 0x01;
            if(want != fsent_[i] && n + 3 <= cap)
            {
                out[n++]  = i < 3 ? 0x98 : 0x99;
                out[n++]  = (uint8_t)i;
                out[n++]  = want;
                fsent_[i] = want;
            }
        }
        return n;
    }
    /** The controller came (back): send every light again */
    void ForgetLights()
    {
        for(int d = 0; d < kDecks; d++)
        {
            for(int i = 0; i < kLights; i++)
                sent_[d][i] = 0xFF;
            for(int i = 0; i < kPadLights; i++)
                psent_[d][i] = 0xFF;
            meter_[d] = 0xFF;
        }
        for(auto& f : fsent_)
            f = 0xFF;
    }

  private:
    static constexpr int kLights = 5, kPadLights = 13;

    static constexpr float kAuto[kHot] = {1.f / 16, 1.f / 8, 1.f / 4, 1.f / 2, 1, 2, 4, 8}; // AUTO LOOP: beats a pad

    void Pads(int d, int a, bool down)
    {
        const DeckState& s = e_.state[d];
        if(a >= 0x14 && a <= 0x1B) // a pad
        {
            const int i = a - 0x14;
            if(pads[d] == kPadCue)
                Post(kEvHot, d, (float)i, down ? 1 : 0);
            else if(pads[d] == kPadLoop && down) // that many beats; the pad of the loop that runs ends it
            {
                if(s.loop_on.load() && !s.roll.load() && std::fabs(s.beats.load() - kAuto[i]) < 1e-4f)
                    Post(kEvLoop, d, 0);
                else
                    Post(kEvLoop, d, 2, kAuto[i]);
            }
            return;
        }
        if(a >= 0x1C && a <= 0x23) // shift + pad
        {
            const int i = a - 0x1C;
            if(pads[d] == kPadCue && down)
                Post(kEvHotClear, d, (float)i);
            else if(pads[d] == kPadLoop) // a loop while it is held
                Post(kEvRoll, d, down ? 1 : 0, kAuto[i]);
            return;
        }
        if(!down)
            return;
        switch(a)
        {
            case 0x00: pads[d] = kPadCue; break;
            case 0x0D: pads[d] = kPadLoop; break;
            case 0x07: pads[d] = kPadCuts; break;
            case 0x0B: pads[d] = kPadSample; break;
            case 0x40: Post(kEvLoop, d, 2); break; // LOOP: off, or a new one from here
            case 0x41: Post(kEvLoop, d, 1); break; // shift + LOOP: into the last one again
            case 0x34: Post(kEvLoopScale, d, 0.5f); break;
            case 0x35: Post(kEvLoopScale, d, 2.f); break;
            case 0x36: Post(kEvLoopIn, d, 0); break; // shift + 1/2
            case 0x37: Post(kEvLoopOut, d, 0); break; // shift + x2
            default: break;
        }
    }

    void Post(uint8_t type, int deck, float a, double dd = 0)
    {
        Ev ev;
        ev.type = type, ev.deck = (int8_t)deck, ev.a = a, ev.d = dd;
        e_.Post(ev);
    }
    /** The pitch slider: 14 bits, 8192 the centre; towards the player is faster */
    void SetRate(int d)
    {
        const int v = (pitch_msb_[d] << 7) | pitch_lsb_[d];
        float     x = -(v - 8192) / 8192.f;
        if(std::fabs(x) < 0.004f)
            x = 0; // the centre click
        e_.knobs[d].rate = x * kRanges[range[d]];
    }
    void Knob(int ch, int a, int v)
    {
        const float x = v / 127.f;
        if(a == 0x08 && (ch == 1 || ch == 15))
        {
            e_.xfader = x;
            return;
        }
        if(ch == 8 || ch == 9) // the FX section
        {
            if(a == 0x03)
                e_.knobs[ch - 8].fx = v ? 1.f : 0.f; // the paddle: 1 up and staying, 2 down and held, 0 back in the middle
            else if(a == 0x04)
                e_.fx_wet = x;
            else if(a == 0x05)
                e_.fx_beats = std::max(1.f / 16, std::min(8.f, e_.fx_beats.load() * (v < 0x40 ? 2.f : 0.5f)));
            return;
        }
        if(ch == 14)
        {
            if(a == 0x23)
                e_.master = x;
            else if(a == 0x27)
                e_.cue_mix = x;
            else if(a == 0x2F)
                e_.cue_gain = x;
            return;
        }
        if(ch == 15)
        {
            if((a == 0x00 || a == 0x01) && host_)
                host_->Browse(v >= 64 ? v - 128 : v, a == 0x01);
            return;
        }
        if(ch > 1)
            return;
        DeckKnobs& k = e_.knobs[ch];
        switch(a)
        {
            case 0x06:
            {
                const int t = v >= 64 ? v - 128 : v;
                if(shift)
                    Post(kEvSeekRel, ch, 0, t * 0.05 * kRate); // shift + platter: search
                else
                    Post(kEvJog, ch, (float)t);
                break;
            }
            case 0x09: pitch_msb_[ch] = v, SetRate(ch); break;
            case 0x29: pitch_lsb_[ch] = v, SetRate(ch); break;
            case 0x16: k.gain = x; break;
            case 0x17: k.hi = x; break;
            case 0x18: k.mid = x; break;
            case 0x19: k.lo = x; break;
            case 0x1A: k.filter = x; break;
            case 0x1C: k.volume = x; break;
            default: break;
        }
    }

    Engine&     e_;
    NumarkHost* host_;
    int         pitch_msb_[kDecks] = {64, 64}, pitch_lsb_[kDecks] = {0, 0};
    uint8_t     sent_[kDecks][kLights], psent_[kDecks][kPadLights], meter_[kDecks], fsent_[kFxCount];
};

} // namespace dk
