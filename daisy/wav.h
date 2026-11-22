/** Streaming WAV reader: converts any common WAV into mono int16 a chunk
 *  at a time, so the main loop can keep servicing USB between chunks.
 *
 *  Supports PCM 8/16/24/32-bit, 32-bit float, WAVE_FORMAT_EXTENSIBLE,
 *  1-8 channels (averaged to mono), any sample rate.
 *  Platform independent: the caller supplies read/skip functions.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <stdint.h>
#include <string.h>

namespace ab
{
struct WavSource
{
    void* ctx;
    /** Returns bytes read, 0 at end, negative on error */
    int32_t (*read)(void* ctx, void* dst, uint32_t n);
    /** Skips n bytes forward, false on error */
    bool (*skip)(void* ctx, uint32_t n);
};

class WavStream
{
  public:
    /** Parses the header. False if the file isn't a WAV we can play. */
    bool Begin(const WavSource& src)
    {
        src_     = src;
        written_ = 0;
        done_    = false;
        uint8_t h[12];
        if(!ReadAll(h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
            return false;

        bool have_fmt = false, have_data = false;
        for(int guard = 0; guard < 64; guard++)
        {
            uint8_t ch[8];
            if(!ReadAll(ch, 8))
                return false;
            uint32_t size = U32(ch + 4);
            if(!memcmp(ch, "fmt ", 4))
            {
                uint8_t  f[40] = {};
                uint32_t take  = size < 40 ? size : 40;
                if(take < 16 || !ReadAll(f, take))
                    return false;
                if(size > take && !src_.skip(src_.ctx, size - take + (size & 1)))
                    return false;
                else if(size == take && (size & 1) && !src_.skip(src_.ctx, 1))
                    return false;
                format_   = U16(f);
                channels_ = U16(f + 2);
                rate_     = U32(f + 4);
                align_    = U16(f + 12);
                bits_     = U16(f + 14);
                if(format_ == 0xFFFE && take >= 26)
                    format_ = U16(f + 24); // sub-format GUID starts with the tag
                have_fmt = true;
            }
            else if(!memcmp(ch, "data", 4))
            {
                if(!have_fmt)
                    return false;
                data_left_ = (size == 0 || size == 0xFFFFFFFFu) ? 0xFFFFFFFFu : size;
                have_data  = true;
                break;
            }
            else
            {
                if(!src_.skip(src_.ctx, size + (size & 1)))
                    return false;
            }
        }
        if(!have_fmt || !have_data || channels_ < 1 || channels_ > 8 || rate_ < 4000 || rate_ > 384000)
            return false;
        bool pcm   = format_ == 1 && (bits_ == 8 || bits_ == 16 || bits_ == 24 || bits_ == 32);
        bool flt   = format_ == 3 && bits_ == 32;
        if(!pcm && !flt)
            return false;
        if(align_ != channels_ * (bits_ / 8))
            align_ = channels_ * (bits_ / 8);
        return true;
    }

    /** Converts one chunk into dst (capacity in samples). Returns true
     *  while there is more to read. scratch must hold several frames. */
    bool Step(uint8_t* scratch, uint32_t scratch_size, int16_t* dst, uint32_t cap)
    {
        if(done_)
            return false;
        uint32_t frames_room = cap > written_ ? cap - written_ : 0;
        uint32_t bytes       = scratch_size - (scratch_size % align_);
        if(bytes > data_left_)
            bytes = data_left_ - (data_left_ % align_);
        if(bytes > frames_room * align_)
            bytes = frames_room * align_;
        if(bytes == 0)
        {
            done_ = true;
            return false;
        }
        int32_t n = src_.read(src_.ctx, scratch, bytes);
        if(n <= 0)
        {
            done_ = true;
            return false;
        }
        uint32_t frames = (uint32_t)n / align_;
        Convert(scratch, frames, dst + written_);
        written_ += frames;
        if(data_left_ != 0xFFFFFFFFu)
            data_left_ -= (uint32_t)n;
        if((uint32_t)n < bytes || data_left_ < align_ || written_ >= cap)
            done_ = true;
        return !done_;
    }

    uint32_t Written() const { return written_; }
    /** Frames in the file (after Begin); 0xFFFFFFFF if the header doesn't say */
    uint32_t TotalFrames() const
    {
        return data_left_ == 0xFFFFFFFFu ? 0xFFFFFFFFu : written_ + data_left_ / align_;
    }
    uint32_t SampleRate() const { return rate_; }
    uint16_t Channels() const { return channels_; }
    uint16_t Bits() const { return bits_; }
    bool     Done() const { return done_; }

  private:
    static uint16_t U16(const uint8_t* p) { return p[0] | (p[1] << 8); }
    static uint32_t U32(const uint8_t* p)
    {
        return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    bool ReadAll(void* dst, uint32_t n)
    {
        return src_.read(src_.ctx, dst, n) == (int32_t)n;
    }

    /** One channel sample as a 16-bit value in an int32 */
    inline int32_t Sample16(const uint8_t* p) const
    {
        switch(bits_)
        {
            case 8: return ((int32_t)p[0] - 128) << 8;
            case 16: return (int16_t)(p[0] | (p[1] << 8));
            case 24: return ((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24))) >> 16;
            default:
            {
                if(format_ == 3)
                {
                    float f;
                    memcpy(&f, p, 4);
                    if(!(f == f))
                        f = 0.f; // NaN
                    if(f > 1.f)
                        f = 1.f;
                    if(f < -1.f)
                        f = -1.f;
                    return (int32_t)(f * 32767.f);
                }
                return ((int32_t)U32(p)) >> 16;
            }
        }
    }

    void Convert(const uint8_t* src, uint32_t frames, int16_t* out) const
    {
        const uint32_t bps = bits_ / 8;
        for(uint32_t i = 0; i < frames; i++)
        {
            int32_t        sum = 0;
            const uint8_t* f   = src + i * align_;
            for(uint32_t c = 0; c < channels_; c++)
                sum += Sample16(f + c * bps);
            sum /= (int32_t)channels_;
            if(sum > 32767)
                sum = 32767;
            if(sum < -32768)
                sum = -32768;
            out[i] = (int16_t)sum;
        }
    }

    WavSource src_{};
    uint16_t  format_ = 0, channels_ = 0, bits_ = 0, align_ = 1;
    uint32_t  rate_ = 0, data_left_ = 0, written_ = 0;
    bool      done_ = true;
};

} // namespace ab
