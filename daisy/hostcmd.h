/** Commands from the host over the USB serial port (v13.3 on).
 *
 *  Every command starts with ESC (0x1B). The Daisy never sends ESC, so if
 *  the host's serial port echoes the Daisy's own output back (the Linux
 *  default until something sets -echo), nothing in it can trigger a command.
 *  Before v13.3 any single 'R' or 'B' byte did, and echoed report lines are
 *  full of both: that dropped a Daisy into its bootloader on the T95.
 *
 *    ESC R                 send a full state dump
 *    ESC BOOT              reboot into the Daisy bootloader (flash scripts)
 *    ESC K                 keepalive from the T95 bridge (v13.5)
 *    ESC M p hex... \n     MIDI from T95 port p (1-8): one complete message,
 *                          bytes as hex digits, spaces ignored (v13.5)
 *    ESC N p name \n       T95 port p has a controller called name; "-" = gone
 *    ESC P id \n           ping: the Daisy answers "Y id" (id: up to 16 chars)
 *
 *  Bytes outside a command are ignored. Inside a framed command an ESC
 *  starts over, and the payload is text, so a corrupted frame can't become
 *  another command. Older firmware ignores the ESC and acts on the 'R' or
 *  'B' after it, so hosts can send ESC R / ESC BOOT to any version; the
 *  framed commands must only be sent once the Daisy reports "P" (v13.5 on).
 *  No Daisy-only code here: desktop tests include it.
 */
#pragma once
#include <cstdint>

class HostCmd
{
  public:
    enum Cmd : uint8_t
    {
        kNone,
        kResync,
        kBoot,
        kKeepalive,
        kMidi,
        kName,
        kPing,
    };

    static constexpr int     kMaxMidi = 128; // bytes per M frame
    static constexpr int     kMaxText = 32;  // name / ping id
    static constexpr uint8_t kEsc     = 0x1B;

    /** Feed one received byte; returns the command it completes, if any */
    Cmd Feed(uint8_t b)
    {
        if(b == kEsc)
        {
            st_ = kAfterEsc;
            return kNone;
        }
        switch(st_)
        {
            case kAfterEsc:
                st_ = kIdle;
                switch(b)
                {
                    case 'R': return kResync;
                    case 'K': return kKeepalive;
                    case 'B':
                        st_    = kBootWord;
                        match_ = 1;
                        return kNone;
                    case 'M':
                    case 'N':
                        cmd_ = b == 'M' ? kMidi : kName;
                        st_  = kPort;
                        return kNone;
                    case 'P':
                        cmd_  = kPing;
                        st_   = kText;
                        tlen_ = 0;
                        return kNone;
                    default: return kNone;
                }
            case kBootWord:
                if(b != (uint8_t)kBootText[match_])
                {
                    st_ = kIdle;
                    return kNone;
                }
                if(kBootText[++match_] == 0)
                {
                    st_ = kIdle;
                    return kBoot;
                }
                return kNone;
            case kPort:
                if(b < '1' || b > '8')
                {
                    st_ = kIdle;
                    return kNone;
                }
                port_ = b - '0';
                len_  = 0;
                tlen_ = 0;
                nib_  = -1;
                st_   = cmd_ == kMidi ? kHex : kText;
                return kNone;
            case kHex:
            {
                if(b == '\n')
                {
                    st_ = kIdle;
                    return nib_ < 0 && len_ > 0 ? kMidi : kNone;
                }
                if(b == ' ' || b == '\r')
                    return kNone;
                int v = Hex(b);
                if(v < 0 || (nib_ < 0 && len_ >= kMaxMidi))
                {
                    st_ = kIdle; // not hex, or too long: drop the frame
                    return kNone;
                }
                if(nib_ < 0)
                    nib_ = v;
                else
                {
                    midi_[len_++] = (uint8_t)(nib_ << 4 | v);
                    nib_          = -1;
                }
                return kNone;
            }
            case kText:
                if(b == '\n')
                {
                    st_          = kIdle;
                    text_[tlen_] = 0;
                    return tlen_ > 0 ? cmd_ : kNone;
                }
                if(b == '\r')
                    return kNone;
                if(b < 0x20 || b > 0x7E || tlen_ >= kMaxText)
                {
                    st_ = kIdle;
                    return kNone;
                }
                if(!(tlen_ == 0 && b == ' ')) // "N1 name", "P id": skip leading spaces
                    text_[tlen_++] = (char)b;
                return kNone;
            default: return kNone;
        }
    }

    int            Port() const { return port_; }
    const uint8_t* Midi() const { return midi_; }
    int            MidiLen() const { return len_; }
    const char*    Text() const { return text_; } // name or ping id

  private:
    static int Hex(uint8_t b)
    {
        if(b >= '0' && b <= '9')
            return b - '0';
        if(b >= 'a' && b <= 'f')
            return b - 'a' + 10;
        if(b >= 'A' && b <= 'F')
            return b - 'A' + 10;
        return -1;
    }
    static constexpr const char* kBootText = "BOOT";
    enum State : uint8_t
    {
        kIdle,
        kAfterEsc,
        kBootWord,
        kPort,
        kHex,
        kText,
    };
    State   st_    = kIdle;
    Cmd     cmd_   = kNone;
    uint8_t match_ = 0;
    int     port_  = 0;
    int     len_   = 0;
    int     tlen_  = 0;
    int     nib_   = -1;
    uint8_t midi_[kMaxMidi];
    char    text_[kMaxText + 1] = {};
};
