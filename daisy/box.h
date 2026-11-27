/** ApcBox control surface, shared by the Daisy firmware and the Mac build.
 *
 *  Takes APC40 MIDI in, drives the engine, decides what every LED and
 *  knob ring should show, loads samples, saves projects, and writes the
 *  text state stream the monitor page reads.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <stdint.h>
#include <stddef.h>
#include "engine.h"

namespace ab
{
/** The firmware's version: the start line of the log and the `VF` report line */
constexpr char kFirmware[] = "13.36";

/** One APC40 LED or knob ring: wanted value and last value confirmed sent */
struct LedSlot
{
    uint8_t status, d1, want, sent; // sent 0xFF = unknown, resend
};

/** What the box needs from the machine it runs on.
 *  Paths are relative to the sample root ("kits/808/BD.wav"). */
class Platform
{
  public:
    /** One line of the monitor stream, without the newline */
    virtual void Out(const char* line) = 0;

    /* Sample loading: one file open at a time */
    virtual bool     FileOpen(const char* path)          = 0;
    virtual int32_t  FileRead(void* dst, uint32_t n)     = 0; // bytes, -1 error
    virtual bool     FileSkip(uint32_t n)                = 0;
    virtual uint32_t FileTell()                          = 0;
    virtual void     FileClose()                         = 0;
    /** Read buffer for the loader (DMA-safe on the Daisy) */
    virtual uint8_t* Scratch(uint32_t* size)             = 0;

    /* Directory listing: one directory open at a time */
    virtual bool DirOpen(const char* path)                          = 0;
    /** Next entry; false at the end. Names that don't fit come back empty. */
    virtual bool DirNext(char* name, size_t cap, bool* is_dir)      = 0;
    virtual void DirClose()                                         = 0;

    /* Whole-file project I/O. WriteFile creates `dir` first. */
    virtual bool    WriteFile(const char* dir, const char* path, const void* data, uint32_t n) = 0;
    virtual int32_t ReadFile(const char* path, void* data, uint32_t cap) = 0; // bytes, -1 missing

    /* Streaming writes for sets (one write file at a time, separate from
       the read file). FileCreate makes every folder in `dir` first. Data
       passed to FileWrite should come from Scratch() (DMA-safe). */
    virtual bool    FileCreate(const char* dir, const char* path) { return false; }
    virtual int32_t FileWrite(const void* src, uint32_t n) { return -1; }
    virtual bool    FileWriteClose() { return false; }
    /** Deletes a file or an empty folder */
    virtual bool    Remove(const char* path) { return false; }
    /** Renames a file or a folder */
    virtual bool    Rename(const char* from, const char* to) { return false; }
    /** Spare memory where a set's sounds load while the old set plays
     *  (samples; nullptr = none, sounds then load after the switch) */
    virtual int16_t* Staging(uint32_t* samples)
    {
        *samples = 0;
        return nullptr;
    }

    /** Spare memory the size of one slot for the looper's undo (samples;
     *  nullptr = takes can't be undone) */
    virtual int16_t* UndoMem(uint32_t* samples)
    {
        *samples = 0;
        return nullptr;
    }

    /** Spare memory for loops that don't fit a track's slot (samples; nullptr
     *  = none). The box cuts it into blocks of two slots each. */
    virtual int16_t* SpareMem(uint32_t* samples)
    {
        *samples = 0;
        return nullptr;
    }

    /* Status for the monitor */
    virtual float    CpuAvg()            = 0; // 0..1
    virtual float    CpuMax()            = 0;
    virtual bool     ControllerUp()      = 0; // APC40 talking MIDI
    virtual bool     ControllerPresent() = 0; // something plugged in
    virtual uint32_t Dropped()           = 0; // monitor lines dropped
    virtual bool     AudioRunning()      = 0;

    virtual ~Platform() {}
};

/** Builds the LED table, puts the demo beat in pattern 1 */
void BoxInit(Platform* p, Engine* e);
/** Storage mounted: scans kits and samples, loads the project (at boot;
 *  a card plugged in later doesn't replace what you're working on) */
void BoxStorageReady(bool load_project = true);
/** The card was pulled: back to built-in kits, loaded samples keep playing */
void BoxStorageGone();
/** MIDI from the APC40. type: 0 note off, 1 note on, 3 control change */
void BoxMidi(int type, int ch, int d0, int d1, uint32_t now);
/** The controller went away: release everything held */
void BoxControllerGone();
/** MIDI from a keyboard (LPK25, MPK249 via the T95): notes play the
 *  selected track. Same type numbers as BoxMidi */
void BoxKeys(int type, int ch, int d0, int d1, uint32_t now);
/** All keyboards gone: release their notes */
void BoxKeysGone();
/** Main loop: loader, browser, LED states, monitor stream */
void BoxPoll(uint32_t now);
/** Resend the whole state on the monitor stream */
void BoxResync();
/** True once after a page asked for a restart (`restart` in ESC U): the platform resets the
 *  board. Where there is no board to reset (the Mac build) it is taken and dropped */
bool BoxTakeRestart();

LedSlot* BoxLeds(int* n);
/** 128x64 OLED image in SSD1306 page order (1024 bytes); the version
 *  changes whenever the picture does */
const uint8_t* BoxScreen(uint32_t* version);
void     BoxInvalidateLeds();

/* 480x320 colour screen (RGB565, row-major). The box draws, the platform
   sends the changed 16-row bands (bit b = rows 16b..16b+15). */
void     BoxTftInit(uint16_t* framebuffer);
uint32_t BoxTftCompose(uint32_t now);
/** Encoder turn (steps), push = next view, back = STEPS */
/** false: this box has no colour screen (it sits on the companion box, or there
 *  is none). The encoder then has three jobs instead of five views, and the
 *  OLED says which: tracks, patterns, samples. */
void BoxHasTft(bool present);
void BoxUiTurn(int d, uint32_t now);
void BoxUiPush(uint32_t now);
void BoxUiBack(uint32_t now);
/** A UI action from the T95 (remote, page): "turn N", "push", "back",
 *  "play", "stop", "track N" (1-16) and, from control protocol 4, the
 *  commands in protocol/serial.md (mute, solo, knob, step...).
 *  false = not understood */
bool BoxUiCommand(const char* action, uint32_t now);
/** The T95 went quiet: let go of what its pages were holding (FX) */
void BoxUiGone();
/** ESC T from the T95's clock module: "bpm100 ticks playing latency_us".
 *  The Daisy follows it: see Engine::ExtSync */
void BoxClockFrame(const char* frame, uint32_t now);
/** A sample transfer frame from the T95 (text after ESC F, see xfer.h) */
void BoxFileFrame(const char* frame, uint32_t now);
/** Touch: raw XPT2046 readings (Daisy), a tap in screen pixels (Mac build),
 *  and starting the 3-cross calibration */
void BoxTouchRaw(int rx, int ry, bool down, uint32_t now);
void BoxUiTouch(int x, int y, uint32_t now);
void BoxTouchCalibrate(uint32_t now);
void     BoxLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace ab
