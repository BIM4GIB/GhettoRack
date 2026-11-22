/** Commands from the host over the USB serial port (v13.3 on).
 *
 *  Every command starts with ESC (0x1B). The Daisy never sends ESC, so if
 *  the host's serial port echoes the Daisy's own output back (the Linux
 *  default until something sets -echo), nothing in it can trigger a command.
 *  Before v13.3 any single 'R' or 'B' byte did, and echoed report lines are
 *  full of both: that dropped a Daisy into its bootloader on the T95.
 *
 *    ESC R      send a full state dump
 *    ESC BOOT   reboot into the Daisy bootloader (flash scripts)
 *
 *  Bare bytes are ignored. Older firmware ignores the ESC and still acts on
 *  the 'R' or 'B' after it, so hosts can send these to any version.
 *  Other letters after ESC are reserved for the framed control protocol.
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
    };

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
                if(b == 'R')
                {
                    st_ = kIdle;
                    return kResync;
                }
                if(b == 'B')
                {
                    st_    = kBootWord;
                    match_ = 1;
                    return kNone;
                }
                st_ = kIdle;
                return kNone;
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
            default: return kNone;
        }
    }

    static constexpr uint8_t kEsc = 0x1B;

  private:
    static constexpr const char* kBootText = "BOOT";
    enum State : uint8_t
    {
        kIdle,
        kAfterEsc,
        kBootWord,
    };
    State   st_    = kIdle;
    uint8_t match_ = 0;
};
