/** Controllers plugged into the T95, reached through the bridge (v13.5).
 *
 *  The bridge announces each MIDI port (ESC N p name), forwards its MIDI as
 *  complete messages (ESC M p hex) and sends a keepalive (ESC K). This file
 *  decides what each port is and where its MIDI goes:
 *
 *    APC40     answers the identity request with Akai (47) / APC40 (73).
 *              It drives the box exactly like one on the Daisy's own USB-A,
 *              which always wins when both are plugged in. Gets the mode
 *              switch and the LEDs back as "X p hex" lines.
 *    keyboard  anything else (LPK25, MPK249...): notes play the selected
 *              track (BoxKeys). Its MIDI shows as "I p hex".
 *
 *  Keepalive missing for 2 s = the bridge is gone: every T95 port is
 *  dropped and anything held is released.
 *
 *  Lines out: X (must arrive: retried until there's room), I, P, Y.
 *  No Daisy-only code: main.cpp gives it a send function, tests a fake one.
 */
#pragma once
#include "box.h"
#include "hostcmd.h"
#include <cstdio>
#include <cstring>

class T95Ports
{
  public:
    static constexpr int      kPorts     = 8; // 1..8
    static constexpr uint32_t kKeepalive = 2000;
    static constexpr uint32_t kIdentify  = 1000; // no identity reply by then: decide by name
    static constexpr int      kVersion   = 5;    // the "P" line's control version (2: ESC U, 3: ESC F, 4: page actions, 5: sets)

    /** send: queue one line; false = no room right now (nothing queued) */
    using SendFn = bool (*)(const char* line);
    void Init(SendFn send) { send_ = send; }

    void OnCmd(const HostCmd& c, HostCmd::Cmd cmd, uint32_t now)
    {
        last_frame_ = now;
        heard_      = true;
        switch(cmd)
        {
            case HostCmd::kKeepalive: break;
            case HostCmd::kPing:
            {
                char l[8 + HostCmd::kMaxText];
                snprintf(l, sizeof(l), "Y %s", c.Text());
                send_(l);
                break;
            }
            case HostCmd::kName:
                if(strcmp(c.Text(), "-") == 0)
                    Detach(c.Port(), now);
                else
                    Attach(c.Port(), c.Text(), now);
                break;
            case HostCmd::kMidi: Midi(c.Port(), c.Midi(), c.MidiLen(), now); break;
            case HostCmd::kFile: ab::BoxFileFrame(c.Text(), now); break;
            case HostCmd::kUi:
                if(!ab::BoxUiCommand(c.Text(), now))
                    ab::BoxLog("T95: unknown UI action '%s'", c.Text());
                break;
            default: break;
        }
    }

    /** Main loop. direct_up: an APC40 is running on the Daisy's own USB-A */
    void Poll(uint32_t now, bool direct_up)
    {
        if(heard_ && Since(now, last_frame_) > (int32_t)kKeepalive)
        {
            heard_ = false;
            ab::BoxUiGone(); // FX held from a page
            if(any_attached())
            {
                ab::BoxLog("T95 bridge silent for 2 s: dropping its controllers");
                for(int p = 1; p <= kPorts; p++)
                    Detach(p, now);
            }
        }
        for(int p = 1; p <= kPorts; p++)
        {
            Port& o = port_[p];
            if(!o.attached)
                continue;
            static const uint8_t kInquiry[6] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
            if(!o.inquiry_sent && SendHex(p, kInquiry, 6))
                o.inquiry_sent = true;
            if(o.kind == kUnknown && Since(now, o.attach_time) > (int32_t)kIdentify)
            {
                o.kind   = LooksLikeApc(o.name) ? kApc : kKeys;
                changed_ = true;
                ab::BoxLog("T95 port %d (%s): no identity reply, treating it as %s", p,
                           o.name, o.kind == kApc ? "an APC40" : "a keyboard");
            }
        }

        /* Who drives the box: the APC40 on the Daisy, else the first one on the T95 */
        int want = direct_up ? 0 : -1;
        for(int p = 1; want < 0 && p <= kPorts; p++)
            if(port_[p].attached && port_[p].kind == kApc)
                want = p;
        if(want != active_)
        {
            if(active_ > 0)
                ab::BoxControllerGone();
            ab::BoxInvalidateLeds();
            if(want > 0)
            {
                port_[want].intro_step = 0;
                port_[want].mode_done  = false;
                ab::BoxLog("APC40 on T95 port %d drives the box", want);
            }
            active_  = want;
            changed_ = true;
        }
        if(active_ > 0)
            Drive(port_[active_], active_, now);

        if(changed_ || Since(now, p_time_) >= 1000)
        {
            char l[48];
            snprintf(l, sizeof(l), "P %d %02x %02x %02x %d", kVersion, Mask(-1), Mask(kApc),
                     Mask(kKeys), active_);
            if(send_(l))
            {
                changed_ = false;
                p_time_  = now;
            }
        }
    }

    void ForceReport() { changed_ = true; }

    /** An APC40 on the T95 is driving the box (S line "APC40 up") */
    bool ApcUp() const { return active_ > 0 && port_[active_].mode_done; }
    bool AnyAttached() const { return any_attached(); }
    int  Active() const { return active_; }
    uint32_t Unattached() const { return unattached_; } // M frames for unknown ports

  private:
    /* Signed: frames are stamped when they're parsed, which on the Daisy can
       be a millisecond or two after the main loop read `now`. Unsigned
       now - t then wrapped to ~49 days and dropped every T95 controller
       (v13.5 on the rig, 2026-11-22). */
    static int32_t Since(uint32_t now, uint32_t t) { return (int32_t)(now - t); }

    enum Kind : uint8_t
    {
        kUnknown,
        kApc,
        kKeys,
    };
    struct Port
    {
        bool     attached     = false;
        char     name[HostCmd::kMaxText + 1];
        Kind     kind         = kUnknown;
        uint32_t attach_time  = 0;
        bool     inquiry_sent = false;
        int      devid        = -1; // APC40 device id from its identity reply
        int      intro_step   = 0;
        uint32_t intro_time   = 0;
        bool     mode_done    = false;
        uint32_t mode_time    = 0;
    };

    static bool LooksLikeApc(const char* n)
    {
        for(const char* s = n; s[0] && s[1] && s[2]; s++)
            if((s[0] | 32) == 'a' && (s[1] | 32) == 'p' && (s[2] | 32) == 'c')
                return true;
        return false;
    }

    bool any_attached() const
    {
        for(int p = 1; p <= kPorts; p++)
            if(port_[p].attached)
                return true;
        return false;
    }

    int Mask(int kind) const
    {
        int m = 0;
        for(int p = 1; p <= kPorts; p++)
            if(port_[p].attached && (kind < 0 || port_[p].kind == kind))
                m |= 1 << (p - 1);
        return m;
    }

    bool SendHex(int p, const uint8_t* d, int n, char tag = 'X')
    {
        char l[8 + 2 * HostCmd::kMaxMidi];
        int  k = snprintf(l, sizeof(l), "%c %d ", tag, p);
        for(int i = 0; i < n && k + 3 < (int)sizeof(l); i++)
            k += snprintf(l + k, sizeof(l) - k, "%02x", d[i]);
        return send_(l);
    }

    void Attach(int p, const char* name, uint32_t now)
    {
        Port& o = port_[p];
        if(o.attached && strcmp(o.name, name) == 0)
            return; // the bridge re-announces after a Daisy reconnect
        if(o.attached)
            Detach(p, now);
        o              = Port();
        o.attached     = true;
        o.attach_time  = now;
        snprintf(o.name, sizeof(o.name), "%s", name);
        changed_ = true;
        ab::BoxLog("T95 port %d: %s", p, o.name);
    }

    void Detach(int p, uint32_t now)
    {
        (void)now;
        Port& o = port_[p];
        if(!o.attached)
            return;
        if(o.kind == kKeys)
            ab::BoxKeysGone();
        if(p == active_)
        {
            ab::BoxControllerGone();
            ab::BoxInvalidateLeds();
            active_ = -1; // Poll picks the next one
        }
        ab::BoxLog("T95 port %d: %s gone", p, o.name);
        o        = Port();
        changed_ = true;
    }

    void Midi(int p, const uint8_t* d, int n, uint32_t now)
    {
        Port& o = port_[p];
        if(!o.attached)
        {
            unattached_++;
            return;
        }
        if(d[0] == 0xF0)
        {
            Sysex(p, o, d, n);
            return;
        }
        if(d[0] < 0x80 || d[0] >= 0xF0)
            return; // the bridge sends complete channel messages only
        int type = (d[0] >> 4) - 8; // 0 note off, 1 on, 2 poly AT, 3 CC, 4 PC, 5 AT, 6 bend
        int ch   = d[0] & 15;
        int d0   = n > 1 ? d[1] : 0;
        int d1   = n > 2 ? d[2] : 0;
        if(o.kind == kUnknown && !LooksLikeApc(o.name))
        {
            o.kind   = kKeys; // playing before it answered: a keyboard
            changed_ = true;
        }
        if(o.kind == kApc)
        {
            if(p == active_)
                ab::BoxMidi(type, ch, d0, d1, now);
            return; // an APC40 that isn't driving: ignored
        }
        if(o.kind == kKeys)
        {
            SendHex(p, d, n, 'I');
            ab::BoxKeys(type, ch, d0, d1, now);
        }
    }

    void Sysex(int p, Port& o, const uint8_t* d, int n)
    {
        /* F0 7E ch 06 02 mfr family.. : identity reply */
        if(n >= 7 && d[1] == 0x7E && d[3] == 0x06 && d[4] == 0x02)
        {
            if(d[5] == 0x47 && d[6] == 0x73 && n >= 14)
            {
                o.kind  = kApc;
                o.devid = d[13];
                ab::BoxLog("T95 port %d: APC40, device id %d, firmware %d.%d.%d.%d", p, d[13],
                           d[9], d[10], d[11], d[12]);
            }
            else if(o.kind == kUnknown)
            {
                o.kind = kKeys;
                ab::BoxLog("T95 port %d: identity %02X %02X: keyboard", p, d[5], n > 6 ? d[6] : 0);
            }
            changed_ = true;
            return;
        }
        SendHex(p, d, n, 'I');
    }

    /* The active APC40 on the T95: mode switch first, then LEDs */
    void Drive(Port& o, int p, uint32_t now)
    {
        if(!o.mode_done)
        {
            if(Since(now, o.intro_time) < 60) // a gap between SysEx messages, as on USB-A
                return;
            /* Known device id: one intro. Otherwise the broadcast and the two
               ids these units usually have, like the USB-A path */
            static const int kIds[3] = {0x7F, 0x00, 0x01};
            int              count   = o.devid >= 0 ? 1 : 3;
            if(o.intro_step < count)
            {
                uint8_t id   = (uint8_t)(o.devid >= 0 ? o.devid : kIds[o.intro_step]);
                uint8_t m[12] = {0xF0, 0x47, id, 0x73, 0x60, 0x00, 0x04, 0x42, 0x01, 0x01, 0x01, 0xF7};
                if(SendHex(p, m, 12))
                {
                    o.intro_step++;
                    o.intro_time = now;
                }
                return;
            }
            o.mode_done = true;
            o.mode_time = now;
            ab::BoxInvalidateLeds(); // the mode switch resets the LEDs
            changed_ = true;
            return;
        }
        if(Since(now, o.mode_time) < 100)
            return;
        /* Up to 16 changed LEDs per line; marked sent once the line is queued */
        int          n;
        ab::LedSlot* leds = ab::BoxLeds(&n);
        uint8_t      msg[48];
        int          idx[16];
        int          cnt = 0;
        for(int i = 0; i < n && cnt < 16; i++)
        {
            if(leds[i].want == leds[i].sent)
                continue;
            msg[3 * cnt]     = leds[i].status;
            msg[3 * cnt + 1] = leds[i].d1;
            msg[3 * cnt + 2] = leds[i].want;
            idx[cnt++]       = i;
        }
        if(cnt && SendHex(p, msg, 3 * cnt))
            for(int k = 0; k < cnt; k++)
                leds[idx[k]].sent = msg[3 * k + 2];
    }

    SendFn   send_       = nullptr;
    Port     port_[kPorts + 1];
    int      active_     = -1; // -1 none, 0 Daisy USB-A, 1..8 T95 port
    bool     changed_    = true;
    uint32_t p_time_     = 0;
    uint32_t last_frame_ = 0;
    bool     heard_      = false; // a frame since the last silence
    uint32_t unattached_ = 0;
};
