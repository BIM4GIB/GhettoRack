/** SunFounder 3.5" IPS (ST7796U, 480x320) on SPI1, plus the encoder and
 *  the back button. Daisy only; the views themselves are in tftui.h.
 *
 *  Wiring (screen's Pi header pin -> Daisy):
 *    23 SCK  -> D8  (pin 9)      18 DC   -> D13 (pin 14)
 *    19 MOSI -> D10 (pin 11)     22 RST  -> D14 (pin 15)
 *    24 CS   -> D7  (pin 8)       1 3.3V -> 3V3 (pin 38)
 *     2 5V   -> 5V rail           6 GND  -> GND (pin 40)
 *  Touch (XPT2046, same SPI lines, its own CS, read slowly between frames):
 *    21 T_DO -> D9  (pin 10)     26 T_CS -> D16 (pin 23)
 *    11 IRQ  -> D17 (pin 24)
 *  Encoder: A -> D18 (pin 25), B -> D19 (pin 26), push -> D15 (pin 22),
 *  + -> 3V3 (never 5V: breakout pull-ups would put 5V on the pins),
 *  GND -> GND. Back button: D20 (pin 27) to GND.
 *
 *  The frame lives in SDRAM; changed 16-row bands are copied into a
 *  DMA-safe buffer and sent while the main loop carries on.
 *  SPI1's clock is moved from PLL2P (25 MHz, too slow) to PLL1Q.
 */
#pragma once
#include "daisy_seed.h"
#include "box.h"

namespace tftcfg
{
/* Orientation and colour order: if the picture is mirrored, upside down
   or red and blue are swapped, these are the values to change. */
constexpr uint8_t kMadctl  = 0x28; // MV (landscape) | BGR
constexpr bool    kInvert  = true; // IPS panels want inversion on
/* SPI clock = 160 MHz (192 boosted) / prescaler. The ST7796 is specified
   for ~15 MHz writes and jumper wires add more trouble: PS_16 = 10-12 MHz.
   On a soldered board PS_8 (20-24 MHz) is worth trying. */
constexpr auto    kPrescale = daisy::SpiHandle::Config::BaudPrescaler::PS_16;
constexpr int     kPrescaleDiv = 16;
} // namespace tftcfg

class Tft
{
  public:
    static constexpr int W = 480, H = 320, kBandRows = 16, kBands = H / kBandRows;
    /* The DMA-safe RAM is small (32 KB, shared): send a band in halves */
    static constexpr int kChunkRows = 8;

    void Init(uint16_t* framebuffer)
    {
        using namespace daisy;
        fb_ = framebuffer;
        /* SPI1 from PLL1Q (sysclk VCO / 5: 160 MHz, 192 MHz boosted) */
        __HAL_RCC_PLLCLKOUT_ENABLE(RCC_PLL1_DIVQ);
        __HAL_RCC_SPI123_CONFIG(RCC_SPI123CLKSOURCE_PLL);

        SpiHandle::Config c;
        c.periph         = SpiHandle::Config::Peripheral::SPI_1;
        c.mode           = SpiHandle::Config::Mode::MASTER;
        c.direction      = SpiHandle::Config::Direction::TWO_LINES; // MISO for touch
        c.nss            = SpiHandle::Config::NSS::SOFT;
        c.baud_prescaler = tftcfg::kPrescale;
        c.pin_config.sclk = seed::D8;
        c.pin_config.mosi = seed::D10;
        c.pin_config.miso = seed::D9;
        c.pin_config.nss  = Pin();
        ok_ = spi_.Init(c) == SpiHandle::Result::OK;

        tcs_.Init(seed::D16, GPIO::Mode::OUTPUT);
        tcs_.Write(true); // touch chip off the bus
        tirq_.Init(seed::D17, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
        cs_.Init(seed::D7, GPIO::Mode::OUTPUT);
        dc_.Init(seed::D13, GPIO::Mode::OUTPUT);
        rst_.Init(seed::D14, GPIO::Mode::OUTPUT);
        cs_.Write(true);

        enc_.Init(seed::D18, seed::D19, seed::D15);
        back_.Init(seed::D20);
        /* encoder push held at power-up: the other bus mode (see SetBus16) */
        GPIO push;
        push.Init(seed::D15, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
        System::Delay(2);
        boot_held_ = !push.Read();
        enc_.Init(seed::D18, seed::D19, seed::D15);

        InitPanel();
        ab::BoxTftInit(fb_);
        dirty_ = 0xFFFFFFFFu;
    }

    /** true: every command and setting goes out as a 16-bit word, for boards
     *  with shift registers between the Pi header and the controller (the
     *  "125 MHz SPI" kind). false: plain 8-bit 4-wire SPI. Re-inits the panel. */
    void SetBus16(bool on)
    {
        bus16_ = on;
        if(ok_)
        {
            InitPanel();
            dirty_ = 0xFFFFFFFFu;
        }
    }
    bool Bus16() const { return bus16_; }
    bool BootHeld() const { return boot_held_; }

    void InitPanel()
    {
        using namespace daisy;
        rst_.Write(true);
        System::Delay(5);
        rst_.Write(false);
        System::Delay(20);
        rst_.Write(true);
        System::Delay(150);

        Cmd(0x01); // software reset
        System::Delay(150);
        Cmd(0x11); // out of sleep
        System::Delay(150);
        CmdData(0xF0, 0xC3); // unlock extended commands
        CmdData(0xF0, 0x96);
        CmdData(0x36, tftcfg::kMadctl);
        CmdData(0x3A, 0x55); // 16-bit colour
        CmdData(0xB4, 0x01); // 1-dot inversion
        Cmd(tftcfg::kInvert ? 0x21 : 0x20);
        CmdData(0xF0, 0x3C); // lock again
        CmdData(0xF0, 0x69);
        Cmd(0x29); // display on
        System::Delay(20);
    }

    bool Ok() const { return ok_; }

    /** Main loop: input every ms, draw at 15 fps, send changed bands */
    void Poll(uint32_t now)
    {
        if(now != last_ms_)
        {
            last_ms_ = now;
            enc_.Debounce();
            back_.Debounce();
            int d = enc_.Increment();
            if(d)
                ab::BoxUiTurn(d, now);
            if(enc_.FallingEdge()) // released
            {
                if(!long_done_)
                    ab::BoxUiPush(now);
                long_done_ = false;
            }
            if(enc_.Pressed() && !long_done_ && enc_.TimeHeldMs() > 700)
            {
                ab::BoxUiBack(now); // hold the push: back to STEPS
                long_done_ = true;
            }
            if(enc_.Pressed() && !cal_done_ && enc_.TimeHeldMs() > 3000)
            {
                ab::BoxTouchCalibrate(now); // keep holding: touch calibration
                cal_done_ = true;
            }
            if(!enc_.Pressed())
                cal_done_ = false;
            if(back_.RisingEdge())
                ab::BoxUiBack(now);
        }
        if(!ok_ || busy_)
            return;
        /* touch: between frames, about 60 times a second while pressed */
        if(sending_ == 0 && now - touch_time_ >= 16)
        {
            touch_time_ = now;
            bool irq = !tirq_.Read(); // low while touched
            if(irq || touching_)
            {
                int  rx, ry;
                bool down = ReadTouch(&rx, &ry) && irq;
                if(down || touching_)
                    ab::BoxTouchRaw(rx, ry, down, now);
                touching_ = down;
            }
        }
        if(sending_ == 0 && now - frame_time_ >= 66)
        {
            frame_time_ = now;
            sending_ = ab::BoxTftCompose(now) | (dirty_ & ((1u << kBands) - 1));
            dirty_   = 0;
        }
        if(sending_)
        {
            int b = __builtin_ctz(sending_);
            SendChunk(b * kBandRows + half_ * kChunkRows);
            if(++half_ == kBandRows / kChunkRows)
            {
                half_ = 0;
                sending_ &= ~(1u << b);
            }
            // one chunk per pass; the DMA runs meanwhile
        }
    }

  private:
    /** One XPT2046 reading at ~1.25 MHz (it can't take the screen's 40 MHz).
     *  Commands overlap with the previous answer, as the chip allows. */
    bool ReadTouch(int* rx, int* ry)
    {
        SPI_TypeDef* spi = SPI1;
        if(spi->CR1 & SPI_CR1_SPE) // a transfer is still going
            return false;
        static const uint8_t tx[19] = {0xB1, 0xC1, 0, 0x91, 0, 0x91, 0, 0xD1, 0, 0x91, 0,
                                       0xD1, 0, 0x91, 0, 0xD0, 0, 0, 0};
        uint8_t        rxb[19]      = {};
        uint32_t       cfg          = spi->CFG1;
        spi->CFG1 = (cfg & ~SPI_CFG1_MBR) | SPI_BAUDRATEPRESCALER_128;
        tcs_.Write(false);
        bool ok = spi_.BlockingTransmitAndReceive((uint8_t*)tx, rxb, sizeof(tx), 5)
                  == daisy::SpiHandle::Result::OK;
        tcs_.Write(true);
        spi->CFG1 = cfg;
        if(!ok)
            return false;
        auto v = [&](int i) { return (int)(((rxb[i] << 8) | rxb[i + 1]) >> 3) & 0xFFF; };
        int  z = v(1) + 4095 - v(3);
        int  a[3] = {v(7), v(11), v(15)}, b[3] = {v(9), v(13), v(17)};
        auto med = [](int* q) {
            int x = q[0], y = q[1], w = q[2];
            return x > y ? (y > w ? y : (x > w ? w : x)) : (x > w ? x : (y > w ? w : y));
        };
        *rx = med(b); // X channel
        *ry = med(a); // Y channel
        return z > 300;
    }

    void Write(uint8_t* d, size_t n)
    {
        spi_.BlockingTransmit(d, n, 10);
    }
    /* One command or parameter byte: alone (8-bit bus) or as 0x00, b (16-bit) */
    void Byte(uint8_t b)
    {
        uint8_t w[2] = {0, b};
        if(bus16_)
            Write(w, 2);
        else
            Write(&b, 1);
    }
    void Cmd(uint8_t c)
    {
        cs_.Write(false);
        dc_.Write(false);
        Byte(c);
        cs_.Write(true);
    }
    void CmdData(uint8_t c, uint8_t d)
    {
        cs_.Write(false);
        dc_.Write(false);
        Byte(c);
        dc_.Write(true);
        Byte(d);
        cs_.Write(true);
    }
    void Window(int y0, int y1)
    {
        uint8_t ca[4] = {0, 0, (uint8_t)((W - 1) >> 8), (uint8_t)((W - 1) & 0xFF)};
        uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
        dc_.Write(false);
        Byte(0x2A);
        dc_.Write(true);
        for(int i = 0; i < 4; i++)
            Byte(ca[i]);
        dc_.Write(false);
        Byte(0x2B);
        dc_.Write(true);
        for(int i = 0; i < 4; i++)
            Byte(ra[i]);
        dc_.Write(false);
        Byte(0x2C); // memory write: pixels follow (16 bits each either way)
        dc_.Write(true);
    }

    void SendChunk(int y)
    {
        /* RGB565 big-endian into the DMA buffer */
        const uint16_t* src = fb_ + y * W;
        uint8_t*        dst = band_buf_;
        for(int i = 0; i < kChunkRows * W; i++)
        {
            dst[2 * i]     = (uint8_t)(src[i] >> 8);
            dst[2 * i + 1] = (uint8_t)src[i];
        }
        cs_.Write(false);
        Window(y, y + kChunkRows - 1);
        busy_ = true;
        if(spi_.DmaTransmit(band_buf_, kChunkRows * W * 2, nullptr, DmaDone, this)
           != daisy::SpiHandle::Result::OK)
        {
            cs_.Write(true);
            busy_ = false;
            dirty_ |= 1u << (y / kBandRows); // try again next frame
        }
    }

    static void DmaDone(void* ctx, daisy::SpiHandle::Result)
    {
        Tft* t = (Tft*)ctx;
        t->cs_.Write(true);
        t->busy_ = false;
    }

    daisy::SpiHandle  spi_;
    daisy::GPIO       cs_, dc_, rst_, tcs_, tirq_;
    daisy::Encoder    enc_;
    daisy::Switch     back_;
    uint16_t*         fb_         = nullptr;
    bool              ok_         = false;
    volatile bool     busy_       = false;
    uint32_t          sending_    = 0, dirty_ = 0;
    uint32_t          frame_time_ = 0, last_ms_ = 0;
    bool              long_done_  = false, cal_done_ = false;
    bool              bus16_      = true, boot_held_ = false;
    bool              touching_   = false;
    uint32_t          touch_time_ = 0;
    int               half_       = 0;

  public:
    static uint8_t band_buf_[kChunkRows * W * 2];
};
