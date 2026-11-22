/** Text link to the Mac over the Seed's onboard USB-C (USB serial).
 *
 *  Replaces libDaisy's Logger, which reuses its 128-byte buffer while a
 *  transfer is still in flight. Lines go into a ring buffer and are sent
 *  in chunks from two alternating buffers: a buffer is only refilled after
 *  a later transmit has been accepted, which means it has been delivered.
 *
 *  Received bytes go from the USB interrupt into a small ring; Poll() in the
 *  main loop parses them with HostCmd (hostcmd.h). ESC R and ESC BOOT are
 *  handled here, the T95 control frames go to the handler set with OnCmd.
 */
#pragma once
#include "hostcmd.h"
#include "daisy_seed.h"
#include "usbdev.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

class Link
{
  public:
    void Init()
    {
        usbdev::SetRx(Rx);
        usbdev::Init(); // serial + USB audio on the built-in port
    }

    void Printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char    line[300];
        va_list va;
        va_start(va, fmt);
        int n = vsnprintf(line, sizeof(line) - 1, fmt, va);
        va_end(va);
        if(n < 0)
            return;
        if(n > (int)sizeof(line) - 2)
            n = sizeof(line) - 2;
        line[n++] = '\n';
        Put(line, n);
    }

    using CmdFn = void (*)(const HostCmd& c, HostCmd::Cmd cmd);
    void OnCmd(CmdFn fn) { on_cmd_ = fn; }

    /** Main loop: parse what the host sent since last time */
    void Poll()
    {
        while(rx_r_ != rx_w_)
        {
            uint8_t b = rx_[rx_r_ % kRx];
            rx_r_     = rx_r_ + 1;
            HostCmd::Cmd c = cmd_.Feed(b);
            switch(c)
            {
                case HostCmd::kNone: break;
                case HostCmd::kResync: resync_ = true; break;
                case HostCmd::kBoot: boot_ = true; break; // flash script
                default:
                    if(on_cmd_)
                        on_cmd_(cmd_, c);
                    break;
            }
        }
    }

    uint32_t RxDropped() const { return rx_dropped_; }

    /** A line that must not be lost: false (and nothing queued) if there's
     *  no room, so the caller can try again later */
    bool TryLine(const char* s)
    {
        size_t   n    = strlen(s);
        uint32_t used = (head_ + kRing - tail_) % kRing;
        if(used + n + 1 >= kRing - 1)
            return false;
        Put(s, (int)n);
        Put("\n", 1);
        return true;
    }

    /** One raw line of any length (no formatting), newline added */
    void Line(const char* s)
    {
        size_t n = strlen(s);
        uint32_t used = (head_ + kRing - tail_) % kRing;
        if(used + n + 1 >= kRing - 1)
        {
            dropped_++;
            return;
        }
        Put(s, (int)n);
        Put("\n", 1);
    }

    /** Main loop: hand the next chunk to USB if the last one went out */
    void Flush()
    {
        if(!usbdev::SerialReady())
            return;
        if(pending_ == 0)
        {
            while(pending_ < kChunk && tail_ != head_)
            {
                last_out_             = ring_[tail_];
                tx_[cur_][pending_++] = last_out_;
                tail_                 = (tail_ + 1) % kRing;
            }
        }
        if(pending_ == 0)
            return;
        if(usbdev::SerialSend(tx_[cur_], (uint16_t)pending_))
        {
            cur_ ^= 1;
            pending_ = 0;
        }
    }

    /** True once after the Mac asked for a full resend */
    bool TakeResync()
    {
        if(!resync_)
            return false;
        resync_ = false;
        // Stale queued lines are useless now, but if the start of a line has
        // already gone out, its rest must follow or the next line gets glued
        // onto it. Keep that remainder, moved up against head_.
        uint32_t len = 0;
        if(last_out_ != '\n')
        {
            uint32_t i = tail_;
            while(i != head_ && ring_[i] != '\n')
                i = (i + 1) % kRing;
            if(i != head_)
                len = (i + kRing - tail_) % kRing + 1;
        }
        uint32_t to = (head_ + kRing - len) % kRing;
        for(uint32_t k = len; k-- > 0;) // backwards: to is at or after tail_
            ring_[(to + k) % kRing] = ring_[(tail_ + k) % kRing];
        tail_ = to;
        return true;
    }

    uint32_t Dropped() const { return dropped_; }

    bool BootRequested() const { return boot_; }

  private:
    static constexpr uint32_t kRing  = 16384;
    static constexpr uint32_t kChunk = 2048;

    void Put(const char* s, int n)
    {
        uint32_t used = (head_ + kRing - tail_) % kRing;
        if(used + n >= kRing - 1)
        {
            dropped_++; // nobody reading: drop rather than block
            return;
        }
        for(int i = 0; i < n; i++)
        {
            ring_[head_] = s[i];
            head_        = (head_ + 1) % kRing;
        }
    }

    /* USB interrupt: just store the bytes */
    static void Rx(const uint8_t* buf, uint32_t len)
    {
        for(uint32_t i = 0; i < len; i++)
        {
            if(rx_w_ - rx_r_ >= kRx)
            {
                rx_dropped_ = rx_dropped_ + 1;
                continue;
            }
            rx_[rx_w_ % kRx] = buf[i];
            __sync_synchronize();
            rx_w_ = rx_w_ + 1;
        }
    }

    static constexpr uint32_t  kRx = 4096;
    static uint8_t             rx_[kRx];
    static volatile uint32_t   rx_w_, rx_r_, rx_dropped_;
    static HostCmd             cmd_;
    CmdFn                      on_cmd_ = nullptr;

    char                 ring_[kRing];
    uint32_t             head_ = 0, tail_ = 0;
    uint8_t              tx_[2][kChunk];
    int                  cur_     = 0;
    uint32_t             pending_ = 0;
    uint32_t             dropped_ = 0;
    char                 last_out_ = '\n'; // last byte handed to USB
    static volatile bool resync_;
    static volatile bool boot_;
};


