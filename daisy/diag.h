/** Startup diagnostics for the Daisy build.
 *
 *  Keeps a record in backup SRAM, which survives a reset but not a power
 *  cycle:
 *   - the last startup/loop stage reached
 *   - fault details if the CPU crashes (registers + faulting address)
 *  Two watchdogs reset the board when the main loop stops (main.cpp, "the
 *  watchdogs"): the audio callback after 8 s without a pass, the chip's own
 *  after 33 s for when a card call is what hangs or the interrupts are dead
 *  too. After such a reset ApcBox starts again by itself and says so in the
 *  log; three in a row and it stays in report mode.
 *
 *  After a crash, a start-up that stopped, or three watchdog resets in a row
 *  the next boot goes into report mode instead of starting normally: the onboard LED blinks 3 short + pause, and the
 *  report is printed to the monitor page's log once a second.
 *  Press RESET to try a normal start again; ESC BOOT (the flash script) is
 *  taken there too.
 */
#pragma once
#include "daisy_seed.h"

namespace diag
{
constexpr uint32_t kMagicBoot  = 0xB007B007;
constexpr uint32_t kMagicFault = 0xFA017FA0;
constexpr uint32_t kMagicHang  = 0x4A4E6701; // the audio callback's watchdog did the reset
constexpr uint32_t kDogCount   = 0x57444F00; // + watchdog resets in a row (low byte)

struct Record
{
    uint32_t magic;
    uint32_t stage;
    uint32_t pc, lr, psr, cfsr, hfsr, bfar, mmfar;
    uint32_t boots;
    uint32_t hang; // kMagicHang: set by the callback's watchdog just before it resets
    uint32_t dog;  // kDogCount + how many watchdog resets in a row
};

/** Fixed spot 2 KB into backup SRAM, clear of libDaisy's bootloader info
 *  at the start (the bootloader writes that at a fixed address) */
#define DIAG_REC (*(volatile diag::Record*)0x38800800)

inline void EnableBackupSram()
{
    RCC->AHB4ENR |= RCC_AHB4ENR_BKPRAMEN;
    (void)RCC->AHB4ENR;
    PWR->CR1 |= PWR_CR1_DBP;
    __DSB();
}

inline void Stage(uint32_t s)
{
    DIAG_REC.stage = s;
}

/** The chip's own watchdog, as long as it goes: LSI 32 kHz / 256 = 125 Hz, 4096 counts, 32.8 s.
 *  Longer than the 30 s the card driver may wait in one call (SD_TIMEOUT in libDaisy's
 *  sd_diskio.c), so a card that stalls gets to fail by itself. It cannot be stopped again. */
inline void StartWatchdog()
{
    IWDG1->KR  = 0xCCCC; // start
    IWDG1->KR  = 0x5555; // unlock
    IWDG1->PR  = 6;      // LSI / 256
    IWDG1->RLR = 4095;
    while(IWDG1->SR) {}
    IWDG1->KR = 0xAAAA;
}

inline void Kick()
{
    IWDG1->KR = 0xAAAA;
}

const char* StageName(uint32_t s);

} // namespace diag
