/** ApcBox: an 8-track sample groovebox on a Daisy Seed, played from an
 *  Akai APC40 mk1 over USB host. Samples load from a microSD card.
 *
 *  Hardware
 *   - APC40 on the USB-A socket wired to Seed pins 36 (D-) / 37 (D+),
 *     5V on VBUS from the breadboard supply, APC40 on its own adapter.
 *   - microSD on SDMMC1, 4-bit: pins 2-7 + 3V3 + GND, pull-ups on
 *     CMD and DAT0-3.
 *   - Audio out on pins 18/19.
 *
 *  The onboard USB-C streams the state to the monitor page
 *  (ui/apcbox-ui.html in Chrome).
 *
 *  The control logic lives in box.cpp, shared with the Mac build (mac/).
 *  This file is only the Daisy hardware: memory, USB host, LED sending,
 *  SD card, audio.
 *
 *  Control map: see the "Groovebox v1" tab of the build doc.
 */
#include "daisy_seed.h"
#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "usbh_midi.h"
#include "fatfs.h"
#include "engine.h"
#include "box.h"
#include "link.h"
#include "ports.h"
#include "diag.h"
#include "tft.h"
#include "usbdev.h"
#include <cstdarg>
#include <new>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

using namespace daisy;
using namespace ab;

extern "C"
{
    extern USBH_HandleTypeDef hUsbHostHS;
}

/* ================================================================== */
/* Hardware and memory                                                 */

DaisySeed      hw;
MidiUsbHandler midi;
USBHostHandle  usbHost;
CpuLoadMeter   cpu;
SdmmcHandler   sd;
FatFSInterface fsi;
Engine         engine;
Link           ui_link; // USB serial to the Mac UI
volatile bool  Link::resync_ = false;
volatile bool  Link::boot_   = false;
HostCmd        Link::cmd_;
uint8_t           Link::rx_[Link::kRx];
volatile uint32_t Link::rx_w_ = 0, Link::rx_r_ = 0, Link::rx_dropped_ = 0;

#define Log BoxLog

constexpr float    kSampleRate = 48000.f;
constexpr uint32_t kSlotCap    = 768u * 1024u;  // 16 s per track at 48k, 16 tracks
constexpr int      kSpareBlocks = 8;             // for loops longer than that: 32 s each
constexpr uint32_t kDelayLen   = 96000;              // 2 s
constexpr uint32_t kCaptureLen = 192000;             // 4 s

DSY_SDRAM_BSS int16_t slot_mem[kTracks][kSlotCap];
DSY_SDRAM_BSS float   dly_mem_l[kDelayLen];
DSY_SDRAM_BSS float   dly_mem_r[kDelayLen];
DSY_SDRAM_BSS float   cap_mem_l[kCaptureLen];
DSY_SDRAM_BSS float   cap_mem_r[kCaptureLen];
DSY_SDRAM_BSS alignas(8) uint8_t reverb_mem[sizeof(daisysp::ReverbSc)];

/** USB transmit buffers: must be DMA reachable, so D2 SRAM */
DMA_BUFFER_MEM_SECTION uint8_t usb_tx[2][64];

/** SD buffers: 32-byte aligned for cache maintenance, in AXI SRAM */
alignas(32) uint8_t sd_scratch[16384];
alignas(32) FIL     load_file;
alignas(32) FIL     proj_file;
alignas(32) FIL     save_file;
/* Where a set's sounds load while the old set keeps playing: what's left
   of the SDRAM after the slots (about 9 MB, ~95 s of mono audio) */
constexpr uint32_t   kStageCap = 4600 * 1024;
DSY_SDRAM_BSS int16_t stage_mem[kStageCap];
/* The looper's undo: what a track held before the last take (one slot) */
DSY_SDRAM_BSS int16_t undo_mem[kSlotCap];
/* Spare blocks of two slots each: a take the looper can't fit into a slot at
   the tempo (8 bars below about 117 BPM), and such loops loaded from a set */
DSY_SDRAM_BSS int16_t spare_mem[kSpareBlocks][2 * kSlotCap];
DIR                 dir;
FILINFO             fno;

T95Ports t95; // controllers on the T95, through the bridge

/* Main loop timing: MIDI from either USB port waits for the next pass, so a
   slow part of the loop is input latency. The slowest parts are logged. */
const char* const kLoopPart[8] = {"usb-a", "serial in", "box", "t95", "serial out", "oled", "tft", "sd"};
uint32_t          loop_part_max[8];
uint32_t          loop_max = 0, loop_mark = 0, loop_log_time = 0;
/* System::GetUs() wraps about every 18 s (a 32-bit tick counter divided
   down), not at 2^32 us: a pass across the wrap looked like 4277 s. Such
   impossible values are dropped. */
constexpr uint32_t kLoopSane = 10000000; // 10 s
inline void        LoopPart(int i)
{
    uint32_t u = System::GetUs();
    uint32_t d = u - loop_mark;
    if(d < kLoopSane && d > loop_part_max[i])
        loop_part_max[i] = d;
    loop_mark = u;
}
void LoopReport(uint32_t now, uint32_t start_us)
{
    uint32_t d = System::GetUs() - start_us;
    if(d < kLoopSane && d > loop_max)
        loop_max = d;
    if(now - loop_log_time < 10000)
        return;
    loop_log_time = now;
    if(loop_max > 5000) // only when a pass took more than 5 ms
    {
        int o[8] = {0, 1, 2, 3, 4, 5, 6, 7}; // parts, slowest first
        for(int i = 0; i < 8; i++)
            for(int j = i + 1; j < 8; j++)
                if(loop_part_max[o[j]] > loop_part_max[o[i]])
                {
                    int t = o[i];
                    o[i]  = o[j];
                    o[j]  = t;
                }
        int a = o[0], b = o[1], c = o[2];
        Log("loop: slowest pass %lu.%lu ms in 10 s; %s %lu.%lu, %s %lu.%lu, %s %lu.%lu",
            (unsigned long)(loop_max / 1000), (unsigned long)(loop_max % 1000 / 100), kLoopPart[a],
            (unsigned long)(loop_part_max[a] / 1000), (unsigned long)(loop_part_max[a] % 1000 / 100),
            kLoopPart[b], (unsigned long)(loop_part_max[b] / 1000),
            (unsigned long)(loop_part_max[b] % 1000 / 100), kLoopPart[c],
            (unsigned long)(loop_part_max[c] / 1000), (unsigned long)(loop_part_max[c] % 1000 / 100));
    }
    loop_max = 0;
    for(int i = 0; i < 8; i++)
        loop_part_max[i] = 0;
}
bool     SendLine(const char* l) { return ui_link.TryLine(l); }
void     OnHostCmd(const HostCmd& c, HostCmd::Cmd cmd) { t95.OnCmd(c, cmd, System::GetNow()); }

volatile bool midi_ready    = false;
bool          mode_sent     = false;
bool          sysex_pending = false; // true while sx_queue has entries

/* SysEx messages to send, each as its own USB transfer, already packed
   as USB-MIDI packets */
uint8_t sx_queue[8][64];
uint8_t sx_len[8];
int     sx_count = 0;

void QueueSysex(const uint8_t* msg, int n)
{
    if(sx_count >= 8)
        return;
    uint8_t* o   = sx_queue[sx_count];
    int      len = 0;
    for(int i = 0; i < n && len <= 60;)
    {
        int left = n - i;
        int take = left > 3 ? 3 : left;
        uint8_t cin = left > 3 ? 0x04 : (uint8_t)(0x04 + take); // 5/6/7 = end with 1/2/3
        o[len++]    = cin;
        for(int k = 0; k < 3; k++)
            o[len++] = k < take ? msg[i + k] : 0;
        i += take;
    }
    sx_len[sx_count++] = (uint8_t)len;
    sysex_pending      = true;
}

/** APC40 "introduction": switches it to a mode. mode 0x42 = Alternate
 *  Ableton Live mode, where the host controls every LED and the track
 *  select buttons send notes. */
void QueueIntro(uint8_t device_id)
{
    const uint8_t m[12] = {0xF0, 0x47, device_id, 0x73, 0x60, 0x00,
                           0x04, 0x42, 0x01, 0x01, 0x01, 0xF7};
    QueueSysex(m, 12);
}

void QueueInquiry()
{
    const uint8_t m[6] = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
    QueueSysex(m, 6);
}

/** SysEx from the APC40: log it, and answer a device inquiry reply with
 *  an introduction addressed to the ID it reported */
void HandleSysexIn(const uint8_t* d, int n)
{
    char hex[3 * 40 + 1];
    int  k = 0;
    for(int i = 0; i < n && i < 40; i++)
        k += snprintf(hex + k, sizeof(hex) - k, "%02X ", d[i]);
    hex[k] = 0;
    Log("sysex in (%d bytes): F0 %s", n, hex);
    /* d excludes F0/F7: 7E ch 06 02 47 73 00 19 v1 v2 v3 v4 devid ... */
    if(n >= 13 && d[0] == 0x7E && d[2] == 0x06 && d[3] == 0x02 && d[4] == 0x47
       && d[5] == 0x73)
    {
        Log("APC40 says: device id %d, firmware %d.%d.%d.%d", d[12], d[8], d[9],
            d[10], d[11]);
        QueueIntro(d[12]);
    }
}
uint32_t      ready_time    = 0;
uint32_t      mode_time     = 0;

/* One batch in flight at a time. The URB state is polled directly:
 *   IDLE = still in flight (the HAL sets IDLE when it submits)
 *   DONE = delivered, mark the batch sent
 *   NOTREADY / NYET = the APC40 NAKed it, resubmit the same bytes
 *   ERROR / STALL = give up on this batch and resend everything
 * libDaisy's USBH_MIDI_Transmit treats IDLE as free and never resubmits
 * after a NAK, which is what lost LED messages before. */
bool     tx_busy     = false;
uint32_t tx_start    = 0;
int      pending_len = 0;
bool     pending_sx  = false;
int      pending_n   = 0;
int16_t  pending_idx[16];
uint8_t  pending_val[16];

MIDI_HandleTypeDef* MidiClass()
{
    if(!midi_ready || !usbHost.IsActiveClass(USBH_MIDI_CLASS)
       || hUsbHostHS.pActiveClass == nullptr)
        return nullptr;
    return (MIDI_HandleTypeDef*)hUsbHostHS.pActiveClass->pData;
}

bool UsbMidiUp()
{
    return MidiClass() != nullptr;
}

void ResetTx()
{
    tx_busy     = false;
    pending_len = 0;
}

void LedFlush(uint32_t now)
{
    MIDI_HandleTypeDef* hm = MidiClass();
    if(!hm)
        return;
    int      num_leds;
    LedSlot* leds = BoxLeds(&num_leds);
    uint8_t* b = usb_tx[0];

    if(tx_busy)
    {
        USBH_URBStateTypeDef st = USBH_LL_GetURBState(&hUsbHostHS, hm->OutPipe);
        if(st == USBH_URB_DONE)
        {
            for(int k = 0; k < pending_n; k++)
                leds[pending_idx[k]].sent = pending_val[k];
            if(pending_sx)
            {
                /* drop the message just sent */
                for(int i = 1; i < sx_count; i++)
                {
                    memcpy(sx_queue[i - 1], sx_queue[i], sizeof(sx_queue[0]));
                    sx_len[i - 1] = sx_len[i];
                }
                sx_count--;
                if(sx_count == 0)
                {
                    sysex_pending = false;
                    mode_time     = now;
                    BoxInvalidateLeds(); // the mode switch resets the LEDs
                }
            }
            ResetTx();
        }
        else if(st == USBH_URB_NOTREADY || st == USBH_URB_NYET)
        {
            USBH_BulkSendData(&hUsbHostHS, b, pending_len, hm->OutPipe, 0);
        }
        else if(st == USBH_URB_ERROR || st == USBH_URB_STALL
                || now - tx_start > 250)
        {
            BoxInvalidateLeds();
            ResetTx();
        }
        return;
    }

    int len    = 0;
    pending_n  = 0;
    pending_sx = false;
    if(sysex_pending && sx_count > 0)
    {
        if(now - mode_time < 60) // a little gap between SysEx messages
            return;
        memcpy(b, sx_queue[0], sx_len[0]);
        len        = sx_len[0];
        pending_sx = true;
        mode_time  = now;
    }
    else if(now - mode_time > 100)
    {
        for(int i = 0; i < num_leds && pending_n < 16; i++)
        {
            LedSlot& l = leds[i];
            if(l.want == l.sent)
                continue;
            b[len++]               = (l.status & 0xF0) == 0xB0 ? 0x0B : 0x09;
            b[len++]               = l.status;
            b[len++]               = l.d1;
            b[len++]               = l.want;
            pending_idx[pending_n] = i;
            pending_val[pending_n] = l.want;
            pending_n++;
        }
    }
    if(len == 0)
        return;
    pending_len = len;
    if(USBH_BulkSendData(&hUsbHostHS, b, len, hm->OutPipe, 0) == USBH_OK)
    {
        tx_busy  = true;
        tx_start = now;
    }
}


/* ================================================================== */
/* 0.96" SSD1306 OLED on I2C1: SCL = pin 12 (D11), SDA = pin 13 (D12)    */

/* DMA-reachable (D2 SRAM, not cached) buffers for the OLED's I2C transfers */
uint8_t DMA_BUFFER_MEM_SECTION oled_cmd[4];
uint8_t DMA_BUFFER_MEM_SECTION oled_tx[129];

class Oled
{
  public:
    void Init()
    {
        I2CHandle::Config c;
        c.periph         = I2CHandle::Config::Peripheral::I2C_1;
        c.speed          = I2CHandle::Config::Speed::I2C_400KHZ;
        c.mode           = I2CHandle::Config::Mode::I2C_MASTER;
        c.pin_config.scl = Pin(PORTB, 8);
        c.pin_config.sda = Pin(PORTB, 9);
        i2c_.Init(c);
        Probe();
    }

    /** Main loop. One page (4 command bytes, then 128 pixel bytes) at a time,
     *  sent by DMA so the loop doesn't wait: blocking, a page took ~52 ms on
     *  the rig (v13.6 loop timing) and every MIDI message waited with it.
     *  If DMA fails or never finishes, it falls back to blocking sends. */
    void Poll(uint32_t now)
    {
        if(!present_)
        {
            /* Plugged in later? Try again every 10 s (a probe blocks ~20 ms) */
            if(now - last_probe_ > 10000)
                Probe();
            return;
        }
        if(step_ != kIdle)
        {
            if(!done_)
            {
                if(now - started_ > 200)
                {
                    Log("OLED: DMA didn't finish, sending blocking from now on");
                    use_dma_ = false;
                    step_    = kIdle;
                    page_    = -1;
                }
                return;
            }
            if(!ok_)
            {
                Lost();
                return;
            }
            if(step_ == kCmd)
            {
                Start(oled_tx, sizeof(oled_tx), kData, now);
                return;
            }
            step_ = kIdle; // a page is out
            if(++page_ >= 8)
                page_ = -1;
        }
        uint32_t       version;
        const uint8_t* fb = BoxScreen(&version);
        if(page_ < 0)
        {
            if(version == sent_version_)
                return;
            sent_version_ = version;
            memcpy(shadow_, fb, sizeof(shadow_)); // send one consistent frame
            page_ = 0;
        }
        oled_cmd[0] = 0x00;
        oled_cmd[1] = (uint8_t)(0xB0 + page_);
        oled_cmd[2] = 0x00;
        oled_cmd[3] = 0x10;
        oled_tx[0]  = 0x40;
        memcpy(oled_tx + 1, shadow_ + page_ * 128, 128);
        if(use_dma_)
        {
            Start(oled_cmd, sizeof(oled_cmd), kCmd, now);
            return;
        }
        if(Send(oled_cmd, sizeof(oled_cmd)) && Send(oled_tx, sizeof(oled_tx)))
        {
            if(++page_ >= 8)
                page_ = -1;
        }
        else
            Lost();
    }

    bool Present() const { return present_; }

  private:
    uint8_t addr_ = 0x3C; // 0x3D on some modules (address jumper)
    bool    told_ = false;

    enum Step : uint8_t
    {
        kIdle,
        kCmd,
        kData,
    };

    void Start(uint8_t* d, uint16_t n, Step step, uint32_t now)
    {
        done_    = false;
        step_    = step;
        started_ = now;
        if(i2c_.TransmitDma(addr_, d, n, DmaDone, this) != I2CHandle::Result::OK)
        {
            Log("OLED: DMA refused, sending blocking from now on");
            use_dma_ = false;
            step_    = kIdle;
            page_    = -1;
        }
    }

    static void DmaDone(void* ctx, I2CHandle::Result r)
    {
        Oled* o = (Oled*)ctx;
        o->ok_   = r == I2CHandle::Result::OK;
        o->done_ = true;
    }

    void Lost()
    {
        present_ = false; // unplugged: go back to probing
        page_    = -1;
        step_    = kIdle;
        Log("OLED lost");
    }

    bool Send(const uint8_t* d, uint16_t n)
    {
        return i2c_.TransmitBlocking(addr_, (uint8_t*)d, n, 10) == I2CHandle::Result::OK;
    }

    void Probe()
    {
        last_probe_ = System::GetNow();
        /* SSD1306 128x64 init, page addressing */
        static const uint8_t init[] = {0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
                                       0x40, 0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8,
                                       0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB,
                                       0x40, 0xA4, 0xA6, 0xAF};
        bool ok = false;
        for(uint8_t a : {0x3C, 0x3D})
        {
            addr_ = a;
            if((ok = Send(init, sizeof(init))))
                break;
        }
        if(ok && !present_)
            Log("OLED found at 0x%02X", addr_);
        if(!ok && !told_)
            Log("OLED: no reply at 0x3C or 0x3D (check SDA on pin 13, SCL on pin 12, VCC, GND)");
        told_ = true;
        present_      = ok;
        page_         = -1;
        sent_version_ = 0xFFFFFFFF;
    }

    I2CHandle     i2c_;
    volatile bool done_    = false; // set from the DMA interrupt
    volatile bool ok_      = true;
    Step          step_    = kIdle;
    bool          use_dma_ = true;
    uint32_t      started_ = 0;
    bool      present_      = false;
    int       page_         = -1;
    uint32_t  sent_version_ = 0xFFFFFFFF;
    uint32_t  last_probe_   = 0;
    uint8_t   shadow_[1024];
};

Oled oled;

/* ---- microSD hot-swap: no card-detect pin, so ask every 2 s ---- */
bool     sd_driver_ok = false; // SDMMC + FatFS set up (once, at boot)
bool     sd_mounted   = false;
int      sd_last_code = -1;
uint32_t sd_check_time = 0;
void     SdPoll(uint32_t now);

/* 3.5" screen: frame in SDRAM, one band at a time through D2 SRAM */
DSY_SDRAM_BSS uint16_t tft_fb[Tft::W * Tft::H];
DMA_BUFFER_MEM_SECTION uint8_t Tft::band_buf_[Tft::kChunkRows * Tft::W * 2];
Tft tft;

/* ================================================================== */
/* Platform for the shared box code: FatFS on the microSD               */

class DaisyPlatform : public Platform
{
  public:
    void Out(const char* line) override { ui_link.Line(line); }

    bool FileOpen(const char* path) override
    {
        return f_open(&load_file, Full(path), FA_READ) == FR_OK;
    }
    int32_t FileRead(void* dst, uint32_t n) override
    {
        UINT br = 0;
        if(f_read(&load_file, dst, n, &br) != FR_OK)
            return -1;
        return (int32_t)br;
    }
    bool FileSkip(uint32_t n) override
    {
        return f_lseek(&load_file, f_tell(&load_file) + n) == FR_OK;
    }
    uint32_t FileTell() override { return f_tell(&load_file); }
    void     FileClose() override { f_close(&load_file); }
    uint8_t* Scratch(uint32_t* size) override
    {
        *size = sizeof(sd_scratch);
        return sd_scratch;
    }

    bool DirOpen(const char* path) override
    {
        return f_opendir(&dir, Full(path)) == FR_OK;
    }
    bool DirNext(char* name, size_t cap, bool* is_dir) override
    {
        if(f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            return false;
        if(strlen(fno.fname) < cap)
            strcpy(name, fno.fname);
        else
            name[0] = 0;
        *is_dir = (fno.fattrib & AM_DIR) != 0;
        return true;
    }
    void DirClose() override { f_closedir(&dir); }

    bool WriteFile(const char* d, const char* path, const void* data, uint32_t n) override
    {
        f_mkdir(Full(d));
        UINT bw = 0;
        if(f_open(&proj_file, Full(path), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
            return false;
        /* f_write may DMA straight from `data`: it lives in AXI SRAM */
        f_write(&proj_file, data, n, &bw);
        f_close(&proj_file);
        return bw == n;
    }
    int32_t ReadFile(const char* path, void* data, uint32_t cap) override
    {
        UINT br = 0;
        if(f_open(&proj_file, Full(path), FA_READ) != FR_OK)
            return -1;
        f_read(&proj_file, data, cap, &br);
        f_close(&proj_file);
        return (int32_t)br;
    }

    bool FileCreate(const char* d, const char* path) override
    {
        /* every folder on the way: "sets", then "sets/03" */
        char sub[96];
        for(int i = 0; d[i] && i < (int)sizeof(sub) - 1; i++)
        {
            if(d[i + 1] == '/' || d[i + 1] == 0)
            {
                memcpy(sub, d, i + 1);
                sub[i + 1] = 0;
                f_mkdir(Full(sub));
            }
        }
        if(save_open_)
            f_close(&save_file);
        save_open_ = f_open(&save_file, Full(path), FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
        return save_open_;
    }
    int32_t FileWrite(const void* src, uint32_t n) override
    {
        UINT bw = 0;
        if(!save_open_ || f_write(&save_file, src, n, &bw) != FR_OK)
            return -1;
        return (int32_t)bw;
    }
    bool FileWriteClose() override
    {
        if(!save_open_)
            return false;
        save_open_ = false;
        return f_close(&save_file) == FR_OK;
    }
    bool Remove(const char* path) override { return f_unlink(Full(path)) == FR_OK; }
    bool Rename(const char* from, const char* to) override
    {
        char a[256];
        snprintf(a, sizeof(a), "%s", Full(from));
        return f_rename(a, Full(to)) == FR_OK;
    }
    int16_t* Staging(uint32_t* samples) override
    {
        *samples = kStageCap;
        return stage_mem;
    }
    int16_t* UndoMem(uint32_t* samples) override
    {
        *samples = kSlotCap;
        return undo_mem;
    }
    int16_t* SpareMem(uint32_t* samples) override
    {
        *samples = (uint32_t)kSpareBlocks * 2u * kSlotCap;
        return spare_mem[0];
    }

    float    CpuAvg() override { return cpu.GetAvgCpuLoad(); }
    float    CpuMax() override { return cpu.GetMaxCpuLoad(); }
    bool     ControllerUp() override { return UsbMidiUp() || t95.ApcUp(); }
    bool     ControllerPresent() override { return usbHost.GetPresent() || t95.AnyAttached(); }
    uint32_t Dropped() override { return ui_link.Dropped(); }
    bool     AudioRunning() override { return audio_running; }

    bool audio_running = false;

  private:
    bool save_open_ = false;
    const char* Full(const char* path)
    {
        snprintf(full_, sizeof(full_), "0:/%s", path);
        return full_;
    }
    char full_[256];
};

DaisyPlatform platform;

/* ================================================================== */
/* USB host callbacks                                                   */

void USBH_Connect(void* data)
{
    Log("device connected");
}

void USBH_Disconnect(void* data)
{
    midi_ready    = false;
    mode_sent     = false;
    sysex_pending = false;
    sx_count      = 0;
    ResetTx();
    BoxControllerGone();
    Log("device disconnected");
}

void USBH_ClassActive(void* data)
{
    if(usbHost.IsActiveClass(USBH_MIDI_CLASS))
    {
        Log("APC40 MIDI active");
        MidiUsbHandler::Config cfg;
        cfg.transport_config.periph = MidiUsbTransport::Config::Periph::HOST;
        midi.Init(cfg);
        midi.StartReceive();
        ready_time = System::GetNow();
        midi_ready = true;
    }
}

void USBH_Error(void* data)
{
    Log("USB device error");
}

/* ================================================================== */

void SdPoll(uint32_t now)
{
    if(!sd_driver_ok || now - sd_check_time < 2000)
        return;
    sd_check_time = now;
    FATFS* fs     = &fsi.GetSDFileSystem();
    if(!sd_mounted)
    {
        f_mount(nullptr, fsi.GetSDPath(), 0);
        FRESULT r = f_mount(fs, fsi.GetSDPath(), 1); // re-runs the card setup
        if(r == FR_OK)
        {
            sd_mounted   = true;
            sd_last_code = 0;
            Log("SD card in");
            BoxStorageReady(false);
        }
        else if((int)r != sd_last_code)
        {
            sd_last_code = (int)r;
            if(r == FR_NO_FILESYSTEM)
                Log("SD card answers but isn't FAT32 (code 13): format it FAT32/MBR");
        }
        return;
    }
    DIR     d;
    FRESULT r = f_opendir(&d, fsi.GetSDPath());
    if(r == FR_OK)
    {
        f_closedir(&d);
        return;
    }
    sd_mounted   = false;
    sd_last_code = (int)r;
    platform.FileClose();
    platform.DirClose();
    f_mount(nullptr, fsi.GetSDPath(), 0);
    Log("SD card removed (code %d)", (int)r);
    BoxStorageGone();
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    cpu.OnBlockStart();
    engine.SetInput(in[0], in[1]); // mic / line into AUDIO IN L (pin 16)
    engine.Process(out[0], out[1], size);
    usbdev::AudioPush(out[0], out[1], size); // the same audio, over USB
    cpu.OnBlockEnd();
}


/* ================================================================== */
/* Crash / hang diagnostics (see diag.h)                               */


const char* diag::StageName(uint32_t s)
{
    switch(s)
    {
        case 1: return "main() entered, hw.Init not finished";
        case 2: return "hw.Init done";
        case 3: return "USB serial started";
        case 4: return "engine memory set up";
        case 5: return "engine.Init done";
        case 6: return "built-in kit rendered";
        case 7: return "control code init done";
        case 8: return "USB host init done";
        case 9: return "audio started";
        case 10: return "SD init";
        case 11: return "SD mount attempted";
        case 12: return "SD scanned";
        case 13: return "sd.Init returned";
        case 14: return "FatFS init returned";
        case 15: return "f_mount returned";
        case 16: return "about to init USB host";
        case 17: return "usbHost.Init returned";
        case 18: return "about to start audio";
        case 19: return "about to start USB serial";
        case 100: return "main loop: top";
        case 101: return "main loop: usbHost.Process";
        case 102: return "main loop: reading APC40 MIDI";
        case 103: return "main loop: BoxPoll";
        case 104: return "main loop: LED send";
        case 105: return "main loop: serial flush";
        case 106: return "main loop: OLED";
        default: return "?";
    }
}

/** Previous boot crashed or hung: say so and stay put */
void ReportMode(const diag::Record& p)
{
    hw.Init(true);
    ui_link.Init();
    uint32_t next = 0, n = 0;
    while(1)
    {
        uint32_t now = System::GetNow();
        /* 3 short blinks, pause */
        uint32_t ph = now % 1600;
        hw.SetLed(ph < 900 && (ph % 300) < 120);
        if(now >= next)
        {
            next = now + 1000;
            n++;
            if(p.magic == diag::kMagicFault)
            {
                ui_link.Printf("L CRASH REPORT #%lu (press RESET to retry)", (unsigned long)n);
                ui_link.Printf("L crashed after stage %lu: %s", (unsigned long)p.stage, diag::StageName(p.stage));
                ui_link.Printf("L pc=%08lx lr=%08lx psr=%08lx", (unsigned long)p.pc, (unsigned long)p.lr, (unsigned long)p.psr);
                ui_link.Printf("L cfsr=%08lx hfsr=%08lx bfar=%08lx mmfar=%08lx", (unsigned long)p.cfsr,
                               (unsigned long)p.hfsr, (unsigned long)p.bfar, (unsigned long)p.mmfar);
            }
            else
            {
                ui_link.Printf("L STOP REPORT #%lu (press RESET to retry)", (unsigned long)n);
                ui_link.Printf("L last run stopped after stage %lu: %s", (unsigned long)p.stage, diag::StageName(p.stage));
            }
            ui_link.Printf("L boots since power-up: %lu, bootloader version %d",
                           (unsigned long)p.boots, (int)System::GetBootloaderVersion());
        }
        ui_link.Flush();
    }
}

int main(void)
{
    /* Look at what the previous run left behind */
    diag::EnableBackupSram();
    diag::Record prev;
    prev.magic = DIAG_REC.magic;
    prev.stage = DIAG_REC.stage;
    prev.pc    = DIAG_REC.pc;
    prev.lr    = DIAG_REC.lr;
    prev.psr   = DIAG_REC.psr;
    prev.cfsr  = DIAG_REC.cfsr;
    prev.hfsr  = DIAG_REC.hfsr;
    prev.bfar  = DIAG_REC.bfar;
    prev.mmfar = DIAG_REC.mmfar;
    prev.boots = DIAG_REC.boots;
    bool known = prev.magic == diag::kMagicBoot || prev.magic == diag::kMagicFault;
    DIAG_REC.boots = known ? prev.boots + 1 : 1;
    DIAG_REC.magic = 0;
    /* Watchdog resets show in the reset flags, unless the bootloader
       cleared them; startup hangs are caught by the stage anyway */
    bool wdg = (RCC->RSR & RCC_RSR_IWDG1RSTF) != 0;
    RCC->RSR |= RCC_RSR_RMVF;
    if(prev.magic == diag::kMagicFault
       || (prev.magic == diag::kMagicBoot && (prev.stage < 100 || wdg)))
        ReportMode(prev);

    DIAG_REC.magic = diag::kMagicBoot;
    diag::Stage(1);
    hw.Init(true); // 480 MHz
    hw.SetLed(true);
    diag::Stage(2);
    /* 48 kHz / 48-sample blocks are libDaisy's defaults; setting them again
       re-inits the audio hardware, so don't */
    diag::Stage(19);
    ui_link.Init();
    ui_link.OnCmd(OnHostCmd);
    t95.Init(SendLine);
    ui_link.Printf("L ApcBox 13.16 start"); // not Log: the box code isn't set up yet
    diag::Stage(3);

    /* Engine memory */
    Memory mem;
    for(int t = 0; t < kTracks; t++)
        mem.slot_mem[t] = slot_mem[t];
    mem.slot_capacity = kSlotCap;
    mem.delay_l       = dly_mem_l;
    mem.delay_r       = dly_mem_r;
    mem.delay_len     = kDelayLen;
    mem.capture_l     = cap_mem_l;
    mem.capture_r     = cap_mem_r;
    mem.capture_len   = kCaptureLen;
    mem.reverb        = new(reverb_mem) daisysp::ReverbSc();
    diag::Stage(4);
    engine.Init(kSampleRate, mem);
    diag::Stage(5);
    
    engine.RenderBuiltInKit();
    diag::Stage(6);
    
    BoxInit(&platform, &engine);
    diag::Stage(7);

    /* USB host for the APC40 */
    USBHostHandle::Config usbh;
    usbh.connect_callback      = USBH_Connect;
    usbh.disconnect_callback   = USBH_Disconnect;
    usbh.class_active_callback = USBH_ClassActive;
    usbh.error_callback        = USBH_Error;
    oled.Init();
    tft.Init(tft_fb);
    {
        /* SPI1 runs from PLL1Q = sysclk * 2 / 5 */
        unsigned long tenths = HAL_RCC_GetSysClockFreq() / 5 * 2 / tftcfg::kPrescaleDiv / 100000;
        Log("screen: SPI %s, %lu.%lu MHz (the panel can't answer: black = check CS, DC, RST)",
            tft.Ok() ? "up" : "failed", tenths / 10, tenths % 10);
    }
    diag::Stage(16);
    usbHost.Init(usbh);
    diag::Stage(17);
    usbHost.RegisterClass(USBH_MIDI_CLASS);
    diag::Stage(8);

    cpu.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    diag::Stage(18);
    hw.StartAudio(AudioCallback);
    platform.audio_running = true;
    diag::Stage(9);
    

    /* microSD: everything works without it, you just get the built-in kit */
    /* 1-bit SD mode: works with a 4-wire module (CLK, CMD = "MOSI",
       DAT0 = "MISO", DAT3 = "CS" tied high) and with a full 4-bit
       breakout. 12.5 MHz is plenty for samples and tolerates jumper wires. */
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_1;
    sd_cfg.speed = SdmmcHandler::Speed::MEDIUM_SLOW;
    diag::Stage(10);
    bool sd_up = sd.Init(sd_cfg) == SdmmcHandler::Result::OK;
    diag::Stage(13);
    sd_up = sd_up && fsi.Init(FatFSInterface::Config::MEDIA_SD) == FatFSInterface::Result::OK;
    diag::Stage(14);
    FRESULT mount_res = FR_NOT_READY;
    if(sd_up)
        mount_res = f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1);
    sd_driver_ok = sd_up;
    sd_up        = sd_up && mount_res == FR_OK;
    sd_mounted   = sd_up;
    sd_last_code = (int)mount_res;
    diag::Stage(15);
    if(!sd_up)
        Log("SD not mounted (FatFS code %d: 3 = no card answering, 13 = not FAT32)",
            (int)mount_res);
    if(sd_up)
    {
        diag::Stage(11);
        
        /* screen bus mode: saved on the card; encoder held at power-up flips it */
        {
            uint8_t m = 1;
            platform.ReadFile("apcbox/screen.bin", &m, 1);
            bool bus16 = (m != 0) != tft.BootHeld();
            if(tft.BootHeld())
            {
                uint8_t v = bus16 ? 1 : 0;
                platform.WriteFile("apcbox", "apcbox/screen.bin", &v, 1);
            }
            if(bus16 != tft.Bus16())
                tft.SetBus16(bus16);
            Log("screen: %s bus%s (hold the encoder push at power-up to switch)",
                bus16 ? "16-bit" : "8-bit", tft.BootHeld() ? ", switched and saved" : "");
        }
        BoxStorageReady();
        diag::Stage(12);
    }
    else
    {
        Log("no SD card, using the built-in kit");
        if(tft.BootHeld()) // no card to save it on: this session only
            tft.SetBus16(!tft.Bus16());
        Log("screen: %s bus", tft.Bus16() ? "16-bit" : "8-bit");
    }
    

    uint32_t blink_time = 0;
    bool     led_state  = false;

    while(1)
    {
        uint32_t now = System::GetNow();
        diag::Stage(100);
        

        /* Onboard LED: fast = no USB device, slow = device present */
        if(now > blink_time)
        {
            led_state = !led_state;
            hw.SetLed(led_state);
            blink_time = now + (usbHost.GetPresent() ? 400 : 80);
        }

        uint32_t loop_start = System::GetUs();
        loop_mark           = loop_start;
        diag::Stage(101);
        usbHost.Process();

        diag::Stage(102);
        bool up = UsbMidiUp();
        if(up)
        {
            if(!mode_sent && now - ready_time > 500)
            {
                /* Ask who it is (the reply is logged and answered), and
                   send the mode switch to the broadcast ID and the two
                   IDs these units usually have */
                QueueInquiry();
                QueueIntro(0x7F);
                QueueIntro(0x00);
                QueueIntro(0x01);
                mode_sent = true;
            }
            midi.Listen();
            while(midi.HasEvents())
            {
                MidiEvent m = midi.PopEvent();
                if(m.type == SystemCommon && m.sc_type == SystemExclusive)
                    HandleSysexIn(m.sysex_data, m.sysex_message_len);
                else
                    BoxMidi(m.type, m.channel, m.data[0], m.data[1], now);
            }
        }

        LoopPart(0);
        diag::Stage(103);
        ui_link.Poll(); // host commands, T95 controller frames
        if(ui_link.BootRequested())
        {
            /* flash.sh asked: reboot into the Daisy bootloader and wait
               there for dfu-util, no buttons needed */
            hw.SetLed(false);
            System::Delay(50);
            System::ResetToBootloader(System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
        }
        if(ui_link.TakeResync())
        {
            BoxResync();
            t95.ForceReport();
        }
        LoopPart(1);
        BoxPoll(now);
        LoopPart(2);
        t95.Poll(now, up); // after BoxPoll: LED states are fresh
        diag::Stage(104);
        if(up && mode_sent)
            LedFlush(now);
        LoopPart(3);
        diag::Stage(105);
        ui_link.Flush();
        LoopPart(4);
        diag::Stage(106);
        oled.Poll(now);
        LoopPart(5);
        tft.Poll(now);
        LoopPart(6);
        SdPoll(now);
        LoopPart(7);
        LoopReport(now, loop_start);
        {
            /* USB audio: say when a host starts / stops listening */
            static bool     was = false;
            static uint32_t stat_time = 0;
            bool            on  = usbdev::AudioStreaming();
            if(on != was || (on && now - stat_time > 10000))
            {
                usbdev::AudioStats a = usbdev::Stats();
                if(on != was)
                    Log("USB audio: %s", on ? "host is listening" : "host stopped");
                Log("USB audio: %lu packets, %lu short, %lu skipped, %lu missed, %d queued",
                    (unsigned long)a.packets, (unsigned long)a.underruns,
                    (unsigned long)a.overruns, (unsigned long)a.incomplete, a.fill);
                was       = on;
                stat_time = now;
            }
        }
    }
}
