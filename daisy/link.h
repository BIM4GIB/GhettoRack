/** Text link to the Mac over the Seed's onboard USB-C (USB serial).
 *
 *  Replaces libDaisy's Logger, which reuses its 128-byte buffer while a
 *  transfer is still in flight. Lines go into a ring buffer and are sent
 *  in chunks from two alternating buffers: a buffer is only refilled after
 *  a later transmit has been accepted, which means it has been delivered.
 *
 *  Anything received from the Mac containing 'R' asks for a full resend
 *  of the state (the UI sends it when it connects).
 */
#pragma once
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
                tx_[cur_][pending_++] = ring_[tail_];
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
        tail_   = head_; // stale queued lines are useless now
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

    static void Rx(const uint8_t* buf, uint32_t len)
    {
        for(uint32_t i = 0; i < len; i++)
        {
            if(buf[i] == 'R')
                resync_ = true;
            if(buf[i] == 'B') // flash script: go to the bootloader
                boot_ = true;
        }
    }

    char                 ring_[kRing];
    uint32_t             head_ = 0, tail_ = 0;
    uint8_t              tx_[2][kChunk];
    int                  cur_     = 0;
    uint32_t             pending_ = 0;
    uint32_t             dropped_ = 0;
    static volatile bool resync_;
    static volatile bool boot_;
};


