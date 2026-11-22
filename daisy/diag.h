/** Startup diagnostics for the Daisy build.
 *
 *  Keeps a record in backup SRAM, which survives a reset but not a power
 *  cycle:
 *   - the last startup/loop stage reached
 *   - fault details if the CPU crashes (registers + faulting address)
 *  A watchdog resets the board if anything hangs for 8 s.
 *
 *  After a crash or hang the next boot goes into report mode instead of
 *  starting normally: the onboard LED blinks 3 short + pause, and the
 *  report is printed to the monitor page's log once a second.
 *  Press RESET to try a normal start again.
 */
#pragma once
#include "daisy_seed.h"

namespace diag
{
constexpr uint32_t kMagicBoot  = 0xB007B007;
constexpr uint32_t kMagicFault = 0xFA017FA0;

struct Record
{
    uint32_t magic;
    uint32_t stage;
    uint32_t pc, lr, psr, cfsr, hfsr, bfar, mmfar;
    uint32_t boots;
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

inline void StartWatchdog()
{
    IWDG1->KR  = 0xCCCC; // start
    IWDG1->KR  = 0x5555; // unlock
    IWDG1->PR  = 4;      // LSI / 64 = 500 Hz
    IWDG1->RLR = 4000;   // 8 s
    while(IWDG1->SR) {}
    IWDG1->KR = 0xAAAA;
}

inline void Kick()
{
    IWDG1->KR = 0xAAAA;
}

const char* StageName(uint32_t s);

} // namespace diag
