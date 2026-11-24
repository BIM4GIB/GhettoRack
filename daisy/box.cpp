/** ApcBox control surface: everything between the APC40 and the engine.
 *
 *  Shared by the Daisy firmware (main.cpp) and the Mac build (mac/).
 *  The platform supplies files, time, status and the output line sink.
 */
#include "box.h"
#include "wav.h"
#include "kitmap.h"
#include "oledfont.h"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <strings.h>

namespace ab
{
namespace
{
Platform* plat = nullptr;
Engine*   eng  = nullptr;

uint32_t g_now = 0; // time of the event being handled

/* Short-lived message for the OLED ("filter LP 2.4k") */
char     popup[40];
uint32_t popup_until = 0;
void     Popup(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void     Popup(const char* fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    vsnprintf(popup, sizeof(popup), fmt, va);
    va_end(va);
    popup_until = g_now + 1500;
}

/* Human-readable values, matching engine.cpp's mappings */
void FmtPct(char* o, size_t n, float p)
{
    snprintf(o, n, "%d%%", (int)(p * 100.f + 0.5f));
}
void FmtHz(char* o, size_t n, float f)
{
    if(f >= 1000.f)
        snprintf(o, n, "%d.%dk", (int)(f / 1000.f), (int)(fmodf(f, 1000.f) / 100.f));
    else
        snprintf(o, n, "%d", (int)f);
}
void FmtFilter(char* o, size_t n, float x)
{
    char hz[12];
    if(x < 0.46f)
    {
        FmtHz(hz, sizeof(hz), 40.f * powf(2.f, x / 0.46f * 9.f));
        snprintf(o, n, "LP %s", hz);
    }
    else if(x > 0.54f)
    {
        FmtHz(hz, sizeof(hz), 20.f * powf(2.f, (x - 0.54f) / 0.46f * 9.f));
        snprintf(o, n, "HP %s", hz);
    }
    else
        snprintf(o, n, "off");
}
void FmtPan(char* o, size_t n, float p)
{
    int v = (int)roundf((p - 0.5f) * 200.f);
    if(v >= -1 && v <= 1) // knob centre is 64/127
        v = 0;
    if(v == 0)
        snprintf(o, n, "C");
    else
        snprintf(o, n, "%c%d", v < 0 ? 'L' : 'R', v < 0 ? -v : v);
}
const char* const kDivNames[6] = {"1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2"};

constexpr int kMidiNoteOff = 0;
constexpr int kMidiNoteOn  = 1;
constexpr int kMidiCC      = 3;

void Out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Out(const char* fmt, ...)
{
    char    line[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    if(plat) // nothing to send to before BoxInit
        plat->Out(line);
}

void LogKeep(const char* msg); // tftui.h: last lines for the SYSTEM view
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
    char    msg[260];
    va_list va;
    va_start(va, fmt);
    vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);
    Out("L %s", msg);
    LogKeep(msg);
}

/* ================================================================== */
/* APC40 mk1 protocol (Ableton mode)                                    */

namespace apc
{
constexpr uint8_t kClipFirst  = 0x35; // clip launch rows 1-5
constexpr uint8_t kRecArm     = 0x30;
constexpr uint8_t kSolo       = 0x31;
constexpr uint8_t kActivator  = 0x32;
constexpr uint8_t kTrackSel   = 0x33;
constexpr uint8_t kClipStop   = 0x34;
constexpr uint8_t kDevFirst   = 0x3A; // 8 device buttons 0x3A-0x41
constexpr uint8_t kClipTrack  = 0x3A;
constexpr uint8_t kDevOnOff   = 0x3B;
constexpr uint8_t kDevLeft    = 0x3C;
constexpr uint8_t kDevRight   = 0x3D;
constexpr uint8_t kDetail     = 0x3E;
constexpr uint8_t kRecQuant   = 0x3F;
constexpr uint8_t kOverdub    = 0x40;
constexpr uint8_t kMetronome  = 0x41;
constexpr uint8_t kMaster     = 0x50;
constexpr uint8_t kStopAll    = 0x51;
constexpr uint8_t kSceneFirst = 0x52; // scene launch 1-5
constexpr uint8_t kPan        = 0x57;
constexpr uint8_t kSendC      = 0x5A;
constexpr uint8_t kPlay       = 0x5B;
constexpr uint8_t kStop       = 0x5C;
constexpr uint8_t kRec        = 0x5D;
constexpr uint8_t kUp         = 0x5E;
constexpr uint8_t kDown       = 0x5F;
constexpr uint8_t kRight      = 0x60;
constexpr uint8_t kLeft       = 0x61;
constexpr uint8_t kShift      = 0x62;
constexpr uint8_t kTap        = 0x63;
constexpr uint8_t kNudgePlus  = 0x64;
constexpr uint8_t kNudgeMinus = 0x65;

constexpr uint8_t kFaderCC    = 7;
constexpr uint8_t kMasterCC   = 14;
constexpr uint8_t kXfadeCC    = 15;
constexpr uint8_t kDevKnobCC  = 0x10;
constexpr uint8_t kDevTypeCC  = 0x18;
constexpr uint8_t kCueCC      = 0x2F;
constexpr uint8_t kTrkKnobCC  = 0x30;
constexpr uint8_t kTrkTypeCC  = 0x38;

/* Clip LED colours */
constexpr uint8_t kOff         = 0;
constexpr uint8_t kGreen       = 1;
constexpr uint8_t kGreenBlink  = 2;
constexpr uint8_t kRed         = 3;
constexpr uint8_t kRedBlink    = 4;
constexpr uint8_t kYellow      = 5;
constexpr uint8_t kYellowBlink = 6;

/* Ring types */
constexpr uint8_t kRingSingle = 1;
constexpr uint8_t kRingVolume = 2;
constexpr uint8_t kRingPan    = 3;
} // namespace apc

/* ================================================================== */
/* LED engine
 *
 * Every LED and knob ring has a wanted value and a last-sent value.
 * Changes go out in batches of up to 16 USB-MIDI packets through the
 * host MIDI pipe directly. A batch only counts as sent when the USB
 * stack accepts it, so nothing gets lost or garbled when the pipe is
 * busy (that was what left pads stuck green in the drums build).
 */


constexpr int kMaxLeds = 400;
LedSlot           leds[kMaxLeds];
uint8_t           lwant[kMaxLeds]; // by function; apcmap.h moves them to the buttons that do those functions
int           num_leds = 0;

int led_grid[5][8];
int led_trk[5][8]; // rec arm, solo, activator, select, clip stop
int led_master;
int led_scene[5];
int led_page[4];
int led_dev[8][9];      // device button, channel 0-8
int led_trk_ring[8];
int led_trk_type[8];
int led_dev_ring[8][9]; // device knob ring, channel 0-8
int led_dev_type[8][9];

int AddLed(uint8_t status, uint8_t d1)
{
    leds[num_leds] = {status, d1, 0, 0xFF};
    return num_leds++;
}

void BuildLedTable()
{
    using namespace apc;
    for(int r = 0; r < 5; r++)
        for(int c = 0; c < 8; c++)
            led_grid[r][c] = AddLed(0x90 | c, kClipFirst + r);
    for(int k = 0; k < 5; k++)
        for(int c = 0; c < 8; c++)
            led_trk[k][c] = AddLed(0x90 | c, kRecArm + k);
    led_master = AddLed(0x90, kMaster);
    for(int i = 0; i < 5; i++)
        led_scene[i] = AddLed(0x90, kSceneFirst + i);
    for(int i = 0; i < 4; i++)
        led_page[i] = AddLed(0x90, kPan + i);
    for(int k = 0; k < 8; k++)
    {
        led_trk_ring[k] = AddLed(0xB0, kTrkKnobCC + k);
        led_trk_type[k] = AddLed(0xB0, kTrkTypeCC + k);
    }
    /* Device LEDs and rings are banked per track on the APC40, so the
       same value goes to all 9 channels; whichever bank it shows is right */
    for(int k = 0; k < 8; k++)
        for(int c = 0; c < 9; c++)
        {
            led_dev[k][c]      = AddLed(0x90 | c, kDevFirst + k);
            led_dev_ring[k][c] = AddLed(0xB0 | c, kDevKnobCC + k);
            led_dev_type[k][c] = AddLed(0xB0 | c, kDevTypeCC + k);
        }
}

inline void Want(int idx, uint8_t v)
{
    lwant[idx] = v;
}

void InvalidateLeds()
{
    for(int i = 0; i < num_leds; i++)
        leds[i].sent = 0xFF;
}

/* ================================================================== */
/* UI state                                                            */

enum Page
{
    kPagePan,
    kPageRev,
    kPageDly,
    kPageGlobal
};

int      sel_track = 0;
bool     track_notes = false; // APC40 sends TRACK SELECT notes (modes 1/2)
int      view_bank = 0;     // bank shown on row 3
int      builtin_kit_pg[2] = {0, 1}; // built-in kit per track page (gaps, DEVICE ON/OFF)
int      track_page = 0;    // 0 = tracks 1-8 on the APC40 strips, 1 = 9-16
bool     shift_used = false; // another control was touched while SHIFT was down
uint32_t shift_down_at = 0;
/* Faders aren't motorised: after a page switch each one only takes over
   once it reaches the track's level ("pick-up") */
bool     fader_live[8] = {true, true, true, true, true, true, true, true};
int      fader_pos[8]  = {-1, -1, -1, -1, -1, -1, -1, -1};
/** Track on APC40 strip c (0-7) for the current page */
inline int StripTrack(int c)
{
    return c + 8 * track_page;
}
inline int BuiltInKitFor(int t)
{
    return builtin_kit_pg[t / 8];
}
uint32_t reset_hold_start = 0; // SHIFT + STOP ALL CLIPS held since
uint32_t flash_red_until  = 0; // grid flashes red after a reset
int      page      = kPagePan;
bool     shift     = false;
bool     copy_held = false;
/* Pattern copy: hold CLIP/TRACK, tap the source, tap destinations */
int      copy_src      = -1; // source pattern, -1 = none picked yet
int      copy_tap_pad  = -1; // pad of the last tap that picked the source
uint32_t copy_tap_at   = 0;
constexpr uint32_t kDoubleTapMs = 350;

/* Undo for pattern edits (copy, clear): whole-pattern snapshots */
constexpr int kUndoLevels = 16;
struct UndoEntry
{
    int8_t  pat;
    char    label[28];
    uint8_t data[Engine::kPatternBytes];
};
UndoEntry undo_buf[kUndoLevels];
int       undo_top = 0, undo_count = 0;
void      ClearUndo();
bool     recording = false;
bool     roll_held[kTracks]     = {};
bool     ui_roll[kTracks]       = {}; // held from a T95 page
int      kb_bend_track          = -1; // the track a keyboard's wheel is bending
int      in_gain_ui             = 64; // input level as the pages set it (0-127, 64 = as it comes)
void     ForgetReportedName(int t); // the next report sends the track's name again
void     ReleaseKeys();
bool     fx_latched[kNumPerfFx] = {};
bool     fx_pressed[kNumPerfFx] = {};
uint32_t last_trig[kTracks]     = {};
uint32_t flash_until[kTracks]   = {};
uint32_t last_tap               = 0;

/* 303 tracks */
int      acid_oct        = 0;  // keyboard octave: 0 = C2, -1..2
int      held_step       = -1; // step pad held on a 303 track
bool     held_new        = false; // that press turned the step on
bool     held_edited     = false; // a knob changed its note while held
int      held_preview    = -1; // note sounding for the held step
int      key_note[8]     = {-1, -1, -1, -1, -1, -1, -1, -1}; // row 5 keys down
int      key_track[8]    = {};
const char* const kNoteNames[12]
    = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
void NoteName(char* o, size_t n, int note)
{
    snprintf(o, n, "%s%d", kNoteNames[note % 12], note / 12 - 1);
}
uint32_t tap_iv[4]              = {};
int      tap_count              = 0;

/* ================================================================== */
/* SD card: kits, sample browser, sample loader, project file          */

bool sd_ok = false;

constexpr int kMaxKits    = 60;
constexpr int kNameLen    = 64;
constexpr int kMaxFolders = 48;
constexpr int kMaxFiles   = 400;
constexpr int kPathLen    = 192;

char kit_names[kMaxKits][kNameLen]; // index 0 is the built-in kit
int  num_kits    = 1;
int  kit_page    = 0;

/** Slot path "@k<n>" = built-in kit n's sound; "" counts as kit 0 */
int BuiltInKitOf(const char* path)
{
    if(path[0] == 0)
        return 0;
    if(path[0] == '@' && path[1] == 'k')
        return atoi(path + 2);
    return -1;
}

bool IsSdSample(const char* path)
{
    return path[0] != 0 && path[0] != '@';
}

/** Slot path that turns the track into the Pocket 303 voice */
const char* const kSynthPath = "@303";
bool              IsSynthPath(const char* path)
{
    return strcmp(path, kSynthPath) == 0;
}

/** Slot path of a loop recorded from the audio input (lives in RAM only) */
const char* const kRecPath = "@rec";
bool              IsRecPath(const char* path)
{
    return strcmp(path, kRecPath) == 0;
}
int  loaded_kit_pg[2] = {0, 1}; // kit on each track page, -1 = edited
int  loading_kit  = -1;
int  loading_page = 0;

char folders[kMaxFolders][kPathLen];
int  num_folders = 0;
int  cur_folder  = 0;
char files[kMaxFiles][kNameLen + 32];
int  num_files      = 0;
int  cur_file       = -1;
int  browse_accum   = 0;
uint32_t browse_time = 0;

char slot_path[kTracks][kPathLen]; // "" = built-in sound

/** dst = a + "/" + b; the caller has checked it fits in kPathLen */
void JoinPath(char* dst, const char* a, const char* b)
{
    size_t la = strlen(a), lb = strlen(b);
    memcpy(dst, a, la);
    dst[la] = '/';
    memcpy(dst + la + 1, b, lb + 1);
}

int CmpName(const void* a, const void* b)
{
    return strcasecmp((const char*)a, (const char*)b);
}

void BuiltInKitNames()
{
    num_kits = Engine::NumBuiltInKits();
    for(int k = 0; k < num_kits; k++)
        snprintf(kit_names[k], kNameLen, "%s", Engine::BuiltInKitName(k));
}

void ScanKits()
{
    BuiltInKitNames();
    if(!plat->DirOpen("kits"))
        return;
    char name[kPathLen];
    bool is_dir;
    while(num_kits < kMaxKits && plat->DirNext(name, sizeof(name), &is_dir))
    {
        if(!is_dir || name[0] == '.' || name[0] == 0 || strlen(name) >= kNameLen)
            continue;
        strcpy(kit_names[num_kits++], name);
    }
    plat->DirClose();
    int nb = Engine::NumBuiltInKits();
    qsort(kit_names[nb], num_kits - nb, kNameLen, CmpName);
}

void ScanFolders()
{
    num_folders = 0;
    if(!plat->DirOpen("samples"))
        return;
    strcpy(folders[num_folders++], "samples");
    char name[kPathLen];
    bool is_dir;
    while(num_folders < kMaxFolders && plat->DirNext(name, sizeof(name), &is_dir))
    {
        if(!is_dir || name[0] == '.' || name[0] == 0)
            continue;
        if(strlen(name) + 9 >= kPathLen)
            continue;
        snprintf(folders[num_folders++], kPathLen, "samples/%s", name);
    }
    plat->DirClose();
    qsort(folders[1], num_folders - 1, kPathLen, CmpName);
}

void ScanFiles(int folder)
{
    num_files = 0;
    cur_file  = -1;
    if(folder < 0 || folder >= num_folders)
        return;
    if(!plat->DirOpen(folders[folder]))
        return;
    char name[kPathLen];
    bool is_dir;
    while(num_files < kMaxFiles && plat->DirNext(name, sizeof(name), &is_dir))
    {
        if(is_dir || !IsWavName(name) || strlen(name) >= sizeof(files[0]))
            continue;
        strcpy(files[num_files++], name);
    }
    plat->DirClose();
    qsort(files, num_files, sizeof(files[0]), CmpName);
    Log("folder %s: %d samples", folders[folder], num_files);
}

/* ---- loader: one job per track, read in chunks between USB polls ---- */

struct Job
{
    int8_t   track;
    bool     audition;
    char     path[kPathLen]; // "" = built-in sound
    /* set loading */
    bool     stage    = false; // into dst (staging) instead of the track's slot
    bool     from_set = false; // the track's sound comes from the open set
    int16_t* dst      = nullptr;
    uint32_t cap      = 0;
    uint8_t  loop_bars = 0; // restore a recorded loop after loading
    float    loop_bpm  = 0.f;
};

/* Track sound bookkeeping for sets */
uint32_t slot_gen[kTracks] = {};  // bumps whenever a slot's sound changes
char     slot_orig[kTracks][kPathLen]; // where the sound originally came from
int      set_op = 0;              // 0 idle, 1 saving, 2 loading a set
void     SetsJobDone(const Job& j, bool ok, uint32_t len, float rate, bool deferred);
void     SetsTrackLoaded(int t);
void     StagePlace(Job& j);

/* ---- long loops: spare memory for what doesn't fit a track's slot ----
   One region, handed out as claims: a start and a size in granules, each claim
   in one piece (the audio callback only ever sees a pointer and a capacity).
   Free space is the gaps between claims; a new claim takes the lowest gap it
   fits. Nothing here allocates, and all of it runs in the main loop.
   A take of a chosen length and a loaded sound (a loop or a plain sample
   longer than a slot) claim what they need, a take
   that runs until stopped claims two slots (spare_block), and a kept take
   gives back what it didn't use. A track's slot points at its claim while the
   long loop is on it. A claim is free again as soon as
   the slot points somewhere else, whoever moved it (a load, a kit, a set
   switch, a new take): the sweep in BoxPoll finds that out, nobody has to hand
   it back. */
constexpr uint32_t kSpareGran   = 16384; // samples in a granule (0.34 s)
constexpr int      kSpareClaims = 24;
struct SpareEntry
{
    uint16_t at;    // first granule
    uint16_t n;     // granules, 0 = this entry is unused
    int8_t   owner; // the track
    bool     held;  // claimed for a take or a load still under way: not swept
};
int16_t*   spare_mem   = nullptr;
uint32_t   spare_gran  = 0;         // granules in the region
uint32_t   spare_block = 0;         // samples in a claim of the usual size: two slots
SpareEntry spare[kSpareClaims];
int        take_block   = -1;       // the claim the take under way records into
int        take_prev    = -1;       // the claim the track's long loop is in meanwhile (a dropped take goes back to it)
bool       take_swapped = false;    // that take has its own buffer: what the track held is untouched
int        load_block   = -1;       // the claim the load under way fills

void SpareInit()
{
    uint32_t n  = 0;
    spare_mem   = plat->SpareMem(&n);
    spare_block = eng->SlotCapacity() * 2;
    spare_gran  = spare_mem ? n / kSpareGran : 0;
    if(spare_gran > 0xFFFFu)
        spare_gran = 0xFFFFu;
    for(int c = 0; c < kSpareClaims; c++)
        spare[c] = SpareEntry{0, 0, -1, false};
    take_block = take_prev = load_block = -1;
    take_swapped                        = false;
}
int16_t* SpareBase(int c) { return spare_mem + (size_t)spare[c].at * kSpareGran; }
/** Samples claim c holds */
uint32_t SpareCap(int c) { return (uint32_t)spare[c].n * kSpareGran; }
uint32_t SpareGranules(uint32_t samples) { return (samples + kSpareGran - 1) / kSpareGran; }
/** The claim `p` is the start of, -1 = not one */
int SpareIndex(const int16_t* p)
{
    if(!spare_mem || p < spare_mem)
        return -1;
    for(int c = 0; c < kSpareClaims; c++)
        if(spare[c].n && p == SpareBase(c))
            return c;
    return -1;
}
/** The claim track t's sound is in, -1 = it's in its own slot */
int SpareOf(int t) { return SpareIndex(eng->GetSlot(t).data); }
/** The next gap at or after granule `from`: where it starts and how long it
    is (0 = it starts at the end of the region); returns where the claim
    behind it ends, to go on from */
uint32_t SpareGap(uint32_t from, uint32_t* at, uint32_t* len)
{
    for(;;)
    {
        uint32_t next = spare_gran, end = spare_gran;
        bool     inside = false;
        for(int c = 0; c < kSpareClaims; c++)
        {
            const SpareEntry& e = spare[c];
            if(!e.n || (uint32_t)e.at + e.n <= from)
                continue;
            if(e.at <= from) // `from` is inside this claim: go on behind it
            {
                from   = (uint32_t)e.at + e.n;
                inside = true;
                break;
            }
            if(e.at < next)
            {
                next = e.at;
                end  = (uint32_t)e.at + e.n;
            }
        }
        if(inside)
            continue;
        if(from > spare_gran)
            from = spare_gran;
        *at  = from;
        *len = next - from;
        return end;
    }
}
/** How many more claims of the usual size fit */
int SpareFree()
{
    const uint32_t g = SpareGranules(spare_block);
    int            n = 0, entries = 0;
    if(!g)
        return 0;
    for(int c = 0; c < kSpareClaims; c++)
        entries += !spare[c].n;
    for(uint32_t from = 0, at, len; from < spare_gran;)
    {
        from = SpareGap(from, &at, &len);
        n += (int)(len / g);
    }
    return n < entries ? n : entries;
}
/** `samples` of spare memory for track t (the usual size if 0), in the lowest
    gap it fits, held until whoever asked lets go; -1 = no room */
int SpareClaim(int t, uint32_t samples = 0)
{
    const uint32_t g = SpareGranules(samples ? samples : spare_block);
    int            c = 0;
    while(c < kSpareClaims && spare[c].n)
        c++;
    if(!g || c == kSpareClaims)
        return -1;
    for(uint32_t from = 0, at, len; from < spare_gran;)
    {
        from = SpareGap(from, &at, &len);
        if(len >= g)
        {
            spare[c] = SpareEntry{(uint16_t)at, (uint16_t)g, (int8_t)t, true};
            return c;
        }
    }
    return -1;
}
/** What the page shows: samples in the region, free, in the longest gap, and
    how many claims there are */
void SpareStat(uint32_t* total, uint32_t* free_, uint32_t* largest, int* claims)
{
    uint32_t fr = 0, big = 0;
    int      n  = 0;
    for(int c = 0; c < kSpareClaims; c++)
        n += spare[c].n != 0;
    for(uint32_t from = 0, at, len; from < spare_gran;)
    {
        from = SpareGap(from, &at, &len);
        fr += len;
        if(len > big)
            big = len;
    }
    *total   = spare_gran * kSpareGran;
    *free_   = fr * kSpareGran;
    *largest = n < kSpareClaims ? big * kSpareGran : 0; // no entry left: nothing can be claimed
    *claims  = n;
}
/** Held: the sweep leaves the claim alone whatever its track's slot points at */
void SpareHold(int c, bool held) { spare[c].held = held; }
void SpareRelease(int c) { spare[c] = SpareEntry{0, 0, -1, false}; }
/** Claim c keeps only what `samples` need; the rest behind it is free again.
    It never grows, and never goes below one granule. */
void SpareTrim(int c, uint32_t samples)
{
    uint32_t g = SpareGranules(samples);
    if(g < 1)
        g = 1;
    if(g < spare[c].n)
        spare[c].n = (uint16_t)g;
}
/** Track t's slot is empty or locked and gets another sound: back to its own memory */
void SpareHome(int t)
{
    if(eng->GetSlot(t).data != eng->HomeData(t))
        eng->SlotPoint(t, eng->HomeData(t), eng->SlotCapacity());
}
void SpareSweep()
{
    if((take_block >= 0 || take_prev >= 0) && !eng->LiveRecBusy())
    {
        /* kept or dropped: the slot says which of the two is in use now */
        if(take_block >= 0)
            SpareHold(take_block, false);
        if(take_prev >= 0)
            SpareHold(take_prev, false);
        take_block = take_prev = -1;
    }
    for(int c = 0; c < kSpareClaims; c++)
        if(spare[c].n && !spare[c].held && eng->GetSlot(spare[c].owner).data != SpareBase(c))
            SpareRelease(c);
}

constexpr int kMaxJobs = 16;
Job           jobs[kMaxJobs];
int           num_jobs = 0;

enum LoadState
{
    kLoadIdle,
    kLoadWaitLock,
    kLoadRead
};
LoadState load_state = kLoadIdle;
Job       cur_job;
WavStream wav;
uint32_t  lock_start    = 0;

Job* QueueJob(int track, const char* path, bool audition)
{
    for(int i = 0; i < num_jobs; i++)
        if(jobs[i].track == track && !jobs[i].stage)
        {
            memmove(&jobs[i], &jobs[i + 1], sizeof(Job) * (num_jobs - i - 1));
            num_jobs--;
            break;
        }
    if(num_jobs >= kMaxJobs)
        return nullptr;
    Job& j = jobs[num_jobs++];
    j      = Job();
    j.track    = track;
    j.audition = audition;
    snprintf(j.path, kPathLen, "%s", path ? path : "");
    return &j;
}

void QueueLoad(int track, const char* path, bool audition)
{
    if(set_op)
    {
        Popup(set_op == 1 ? "busy saving the set" : set_op == 3 ? "busy saving the kit" : "busy loading a set");
        return;
    }
    QueueJob(track, path, audition); // replaces a queued one for the same track
}

bool TrackLoading(int t)
{
    if(load_state != kLoadIdle && cur_job.track == t && !cur_job.stage)
        return true;
    for(int i = 0; i < num_jobs; i++)
        if(jobs[i].track == t && !jobs[i].stage)
            return true;
    return false;
}

/* A sample sent from the T95 (xfer.h) is read by the loader like a file:
   "t95/<name>" reads from the received bytes in the staging pool */
bool IsT95Path(const char* p) { return strncmp(p, "t95/", 4) == 0; }
const uint8_t* mem_file = nullptr;
uint32_t       mem_size = 0, mem_pos = 0;
bool           mem_open = false;
char           mem_name[kPathLen];
void           XferLoaded(bool ok);

bool SrcOpen(const char* path)
{
    mem_open = false;
    if(IsT95Path(path))
    {
        if(!mem_file || strcmp(path, mem_name))
            return false;
        mem_pos  = 0;
        mem_open = true;
        return true;
    }
    return sd_ok && plat->FileOpen(path);
}
uint32_t SrcTell() { return mem_open ? mem_pos : plat->FileTell(); }
void     SrcClose()
{
    if(mem_open)
        mem_open = false;
    else
        plat->FileClose();
}

int32_t PlatRead(void* ctx, void* dst, uint32_t n)
{
    if(mem_open)
    {
        uint32_t k = n < mem_size - mem_pos ? n : mem_size - mem_pos;
        memcpy(dst, mem_file + mem_pos, k);
        mem_pos += k;
        return (int32_t)k;
    }
    return ((Platform*)ctx)->FileRead(dst, n);
}

bool PlatSkip(void* ctx, uint32_t n)
{
    if(mem_open)
    {
        if(n > mem_size - mem_pos)
            return false;
        mem_pos += n;
        return true;
    }
    return ((Platform*)ctx)->FileSkip(n);
}

void FinishJob(bool ok)
{
    int t = cur_job.track;
    if(load_block >= 0)
    {
        SpareHold(load_block, false); // the slot points at it, or it's free again
        load_block = -1;
    }
    if(cur_job.stage)
    {
        load_state = kLoadIdle;
        SetsJobDone(cur_job, ok, 0, 1.f, false);
        return;
    }
    slot_gen[t]++;
    if(ok && cur_job.loop_bars)
        eng->SetLoop(t, cur_job.loop_bars, cur_job.loop_bpm);
    if(!cur_job.from_set)
        snprintf(slot_orig[t], kPathLen, "%s", ok ? cur_job.path : "");
    else
        SetsTrackLoaded(t);
    if(ok)
        snprintf(slot_path[t], kPathLen, "%s", cur_job.path);
    else
        snprintf(slot_path[t], kPathLen, "@k%d", BuiltInKitFor(t));
    eng->ReleaseSlotLock(t);
    if(IsT95Path(cur_job.path))
        XferLoaded(ok);
    if(cur_job.audition && !eng->Playing())
        eng->Audition(t);
    load_state = kLoadIdle;
    if(num_jobs == 0 && loading_kit >= 0)
    {
        loaded_kit_pg[loading_page] = loading_kit;
        loading_kit                 = -1;
    }
}

/* Set loading: a sound goes into the staging pool while the old set plays */
void StageBegin()
{
    Job& j = cur_job;
    StagePlace(j);
    if(IsSynthPath(j.path) || IsRecPath(j.path))
    {
        SetsJobDone(j, IsSynthPath(j.path), 0, 1.f, false);
        return;
    }
    int bk = BuiltInKitOf(j.path);
    if(bk >= 0)
    {
        uint32_t n = eng->RenderBuiltInTo(j.track, bk, j.dst, j.cap);
        SetsJobDone(j, n > 0, n, 1.f, n == 0);
        return;
    }
    if(!sd_ok || !plat->FileOpen(j.path))
    {
        Log("can't open %s", j.path);
        SetsJobDone(j, false, 0, 1.f, false);
        return;
    }
    WavSource src{plat, PlatRead, PlatSkip};
    if(!wav.Begin(src))
    {
        plat->FileClose();
        Log("not a playable wav: %s", j.path);
        SetsJobDone(j, false, 0, 1.f, false);
        return;
    }
    /* longer than a slot (a loop or a plain sample): straight into spare
       memory, as much as the file needs (no staging, no copy afterwards). No
       room for it while the old set's long sounds are still there: it loads
       after the switch, when they are gone, late but whole (and cut to the
       slot then, if there is still no room). */
    uint32_t want = wav.TotalFrames();
    if(want > eng->SlotCapacity())
    {
        const int b = SpareClaim(j.track, want);
        if(b < 0)
        {
            plat->FileClose();
            Log("set: track %d long sound loads after the switch (no room before)", j.track + 1);
            SetsJobDone(j, true, 0, 1.f, true);
            return;
        }
        j.dst = SpareBase(b); // held until the switch has happened (AfterSwap)
        j.cap = SpareCap(b);
        load_state = kLoadRead;
        return;
    }
    if(want > j.cap) // too big for what's left: load it after the switch
    {
        plat->FileClose();
        SetsJobDone(j, true, 0, 1.f, true);
        return;
    }
    j.cap      = want;
    load_state = kLoadRead;
}

void StageRead()
{
    uint32_t scratch_size = 0;
    uint8_t* scratch      = plat->Scratch(&scratch_size);
    uint32_t o            = plat->FileTell() & 3;
    if(wav.Step(scratch + o, scratch_size - 4, cur_job.dst, cur_job.cap))
        return;
    plat->FileClose();
    load_state = kLoadIdle;
    SetsJobDone(cur_job, wav.Written() > 0, wav.Written(),
                (float)wav.SampleRate() / eng->SampleRate(), false);
}

void LoaderStep(uint32_t now)
{
    switch(load_state)
    {
        case kLoadIdle:
            if(num_jobs == 0)
                return;
            cur_job = jobs[0];
            memmove(&jobs[0], &jobs[1], sizeof(Job) * (num_jobs - 1));
            num_jobs--;
            if(cur_job.stage)
            {
                StageBegin();
                break;
            }
            eng->RequestSlotLock(cur_job.track);
            lock_start = now;
            load_state = kLoadWaitLock;
            break;

        case kLoadWaitLock:
        {
            int t = cur_job.track;
            /* The audio callback confirms within a millisecond; the
               timeout only matters if audio isn't running yet */
            if(!eng->SlotLocked(t) && (plat->AudioRunning() && now - lock_start < 200))
                return;
            Slot& s  = eng->GetSlot(t);
            s.length = 0;
            SpareHome(t); // a long loop it held is gone with this; its claim frees itself
            if(IsSynthPath(cur_job.path))
            {
                eng->SetSynth(t, true);
                FinishJob(true);
                return;
            }
            eng->SetSynth(t, false);
            eng->ClearLoop(t); // a new sound replaces a recorded loop
            int bk = BuiltInKitOf(cur_job.path);
            if(bk >= 0)
            {
                eng->RenderBuiltInSound(t, bk);
                FinishJob(true);
                return;
            }
            if(!SrcOpen(cur_job.path))
            {
                Log("can't open %s", cur_job.path);
                eng->RenderBuiltInSound(t, BuiltInKitFor(t));
                FinishJob(false);
                return;
            }
            WavSource src{plat, PlatRead, PlatSkip};
            if(!wav.Begin(src))
            {
                Log("not a playable wav: %s", cur_job.path);
                SrcClose();
                eng->RenderBuiltInSound(t, BuiltInKitFor(t));
                FinishJob(false);
                return;
            }
            /* longer than a slot (a loop or a plain sample): into spare memory if there is room, else cut */
            if(wav.TotalFrames() > eng->SlotCapacity())
            {
                SpareSweep(); // what this track held until now is free for it
                load_block = SpareClaim(t, wav.TotalFrames());
                if(load_block >= 0)
                    eng->SlotPoint(t, SpareBase(load_block), SpareCap(load_block));
                else
                    Log("track %d: no room for a long sound, cut to %d s", t + 1, (int)eng->SlotSeconds());
            }
            load_state = kLoadRead;
            break;
        }

        case kLoadRead:
        {
            if(cur_job.stage)
            {
                StageRead();
                return;
            }
            int   t    = cur_job.track;
            Slot& s    = eng->GetSlot(t);
            /* Keep the sector-aligned part of each read word-aligned in
               memory: SDMMC DMA can't write to unaligned addresses */
            uint32_t scratch_size = 0;
            uint8_t* scratch      = plat->Scratch(&scratch_size);
            uint32_t o            = SrcTell() & 3;
            bool     more = wav.Step(scratch + o, scratch_size - 4, s.data, s.capacity);
            if(more)
                return;
            SrcClose();
            s.rate = (float)wav.SampleRate() / eng->SampleRate();
            __sync_synchronize(); // sample data visible before the length
            s.length = wav.Written();
            Log("track %d <- %s (%u frames, %u Hz, %u ch, %u bit)",
                         t + 1,
                         cur_job.path,
                         (unsigned)wav.Written(),
                         (unsigned)wav.SampleRate(),
                         (unsigned)wav.Channels(),
                         (unsigned)wav.Bits());
            if(wav.Written() == 0)
            {
                SpareHome(t);
                eng->RenderBuiltInSound(t, BuiltInKitFor(t));
                FinishJob(false);
                return;
            }
            FinishJob(true);
            break;
        }
    }
}

/* kits.h (further down): settings of a saved kit, undo of a kit load */
bool KitSettingsLoad(const char* dir, char kind[8], TrackParams par[8]);
void KitBackup(int pg);
void KitUndoForget();
void KitListChanged();
bool KitTrackKept(int t);
const char* const kKitRoles[8] = {"kick", "snare", "hat", "open hat", "clap", "tom", "rim", "perc"};

/** Kits 0..n-1 are the built-in ones; the rest are folders in /kits */
/** keep_synth: 303 tracks stay 303s (a drum kit shouldn't eat the bass) */
/** Loads into the 8 tracks of one page (the current one by default) */
void LoadKit(int k, bool keep_synth = true, int pg = -1)
{
    if(k < 0 || k >= num_kits)
        return;
    if(pg < 0)
        pg = track_page;
    const int base = 8 * pg;
    char      assigned[8][kPathLen]; // index = strip, track = base + strip
    for(int t = 0; t < 8; t++)
        assigned[t][0] = 0;
    char               kind[8]  = {};
    static TrackParams par[8];
    bool               settings = false;
    if(keep_synth && !set_op)
        KitBackup(pg); // picked by hand: undo gives the 8 tracks back

    const int nb = Engine::NumBuiltInKits();
    if(k < nb)
    {
        builtin_kit_pg[pg] = k;
        for(int t = 0; t < 8; t++)
            snprintf(assigned[t], kPathLen, "@k%d", k);
    }
    else if(sd_ok)
    {
        char dir[kPathLen];
        snprintf(dir, kPathLen, "kits/%s", kit_names[k]);
        settings = KitSettingsLoad(dir, kind, par); // a kit saved on the box (v13.14)

        /* The browser's file list stays as it is, so use a small local list */
        static char names[48][kNameLen + 32];
        int         n = 0;
        if(plat->DirOpen(dir))
        {
            char name[kPathLen];
            bool is_dir;
            while(n < 48 && plat->DirNext(name, sizeof(name), &is_dir))
            {
                if(is_dir || !IsWavName(name) || strlen(name) >= sizeof(names[0]))
                    continue;
                strcpy(names[n++], name);
            }
            plat->DirClose();
        }
        qsort(names, n, sizeof(names[0]), CmpName);

        bool used[48] = {};
        for(int i = 0; i < n; i++)
        {
            if(strlen(dir) + 1 + strlen(names[i]) >= (size_t)kPathLen)
            {
                used[i] = true; // path too long, skip it
                continue;
            }
            int t = KitTrackForName(names[i]);
            if(t >= 0 && assigned[t][0] == 0)
            {
                JoinPath(assigned[t], dir, names[i]);
                used[i] = true;
            }
        }
        /* Anything unnamed fills the empty tracks in order */
        for(int i = 0; i < n; i++)
        {
            if(used[i])
                continue;
            for(int t = 0; t < 8; t++)
                if(assigned[t][0] == 0)
                {
                    JoinPath(assigned[t], dir, names[i]);
                    break;
                }
        }
    }
    for(int c = 0; c < 8; c++)
    {
        int t = base + c;
        if(settings)
        {
            /* the kit says what each track is; a track it doesn't mention stays,
               and so does a recording (it exists nowhere else) */
            if(!kind[c] || (keep_synth && KitTrackKept(t) && !TrackLoading(t)))
                continue;
            TrackParams p = par[c];
            p.mute        = eng->track[t].mute;
            p.solo        = eng->track[t].solo;
            eng->track[t] = p;
            if(pg == track_page)
                fader_live[c] = false; // the fader no longer matches
            if(kind[c] == 'a')
            {
                if(!eng->IsSynth(t) || TrackLoading(t))
                    QueueLoad(t, kSynthPath, false);
                continue;
            }
            if(assigned[c][0] == 0)
                snprintf(assigned[c], kPathLen, "@k%d", builtin_kit_pg[pg]);
            QueueLoad(t, assigned[c], false);
            continue;
        }
        if(assigned[c][0] == 0) // nothing for this track: built-in sound
            snprintf(assigned[c], kPathLen, "@k%d", builtin_kit_pg[pg]);
        if(keep_synth && (IsSynthPath(slot_path[t]) || IsRecPath(slot_path[t])) && !TrackLoading(t))
            continue;
        QueueLoad(t, assigned[c], false);
    }
    loading_kit  = k;
    loading_page = pg;
    Log("loading kit %s", kit_names[k]);
    Popup("kit %s", kit_names[k]);
}

void BrowseBackup(int t); // further down: what the track held, so undo gives it back

void LoadBrowsedFile(bool audition)
{
    if(cur_folder < 0 || cur_folder >= num_folders || cur_file < 0
       || cur_file >= num_files)
        return;
    char path[kPathLen * 2];
    snprintf(path, sizeof(path), "%s/%s", folders[cur_folder], files[cur_file]);
    if(strlen(path) >= (size_t)kPathLen)
    {
        Log("path too long: %s", path);
        return;
    }
    BrowseBackup(sel_track);
    QueueLoad(sel_track, path, audition);
    loaded_kit_pg[sel_track / 8] = -1; // the kit has been edited
    Popup("%s", files[cur_file]);
}

void BrowseStep(uint32_t now)
{
    if(browse_accum == 0 || now - browse_time < 120 || num_files == 0)
        return;
    int i = cur_file < 0 ? 0 : cur_file + browse_accum;
    i %= num_files;
    if(i < 0)
        i += num_files;
    cur_file     = i;
    browse_accum = 0;
    LoadBrowsedFile(true);
}

/* ---- project file ---- */

struct Project
{
    char         magic[4];
    uint32_t     size;
    uint8_t      patterns[kPatterns][kTracks][kSteps];
    uint8_t      notes[kPatterns][kTracks][kSteps]; // 303 notes and slides
    TrackParams  tracks[kTracks];
    GlobalParams global;
    char         paths[kTracks][kPathLen];
};
alignas(32) Project proj;

void SaveProject()
{
    if(!sd_ok)
        return;
    memcpy(proj.magic, "APCB", 4);
    proj.size = sizeof(Project);
    memcpy(proj.patterns, eng->PatternData(), sizeof(proj.patterns));
    memcpy(proj.notes, eng->NoteData(), sizeof(proj.notes));
    for(int t = 0; t < kTracks; t++)
    {
        proj.tracks[t] = eng->track[t];
        memcpy(proj.paths[t], slot_path[t], kPathLen);
    }
    proj.global = eng->global;
    bool ok = plat->WriteFile("apcbox", "apcbox/project.bin", &proj, sizeof(proj));
    Log(ok ? "project saved" : "project save FAILED");
    Popup(ok ? "saved" : "SAVE FAILED");
}

bool LoadProject()
{
    if(!sd_ok)
        return false;
    int32_t br = plat->ReadFile("apcbox/project.bin", &proj, sizeof(proj));
    if(br < 0)
        return false;
    if(br != (int32_t)sizeof(proj) || memcmp(proj.magic, "APCB", 4)
       || proj.size != sizeof(Project))
    {
        Log("project file doesn't match this firmware, ignored");
        return false;
    }
    memcpy(eng->PatternData(), proj.patterns, sizeof(proj.patterns));
    memcpy(eng->NoteData(), proj.notes, sizeof(proj.notes));
    eng->global = proj.global;
    for(int t = 0; t < kTracks; t++)
    {
        eng->track[t]                 = proj.tracks[t];
        proj.paths[t][kPathLen - 1]     = 0;
        QueueLoad(t, proj.paths[t], false);
    }
    loaded_kit_pg[0] = loaded_kit_pg[1] = -1;
    ClearUndo();
    Log("project loaded");
    Popup("project loaded");
    return true;
}

/* ================================================================== */
/* Controls                                                            */

/* ---- looper: what was on the track before a take, for undo, redo and cancel ---- */
struct LooperUndo
{
    int      track = -1;
    bool     valid = false;   // there is something to go back (or forward) to
    bool     redo  = false;   // it's the take that was undone
    bool     await = false;   // a reload is still under way
    bool     data  = false;   // the sound itself is in the undo memory
    bool     loop  = false;
    int      mode  = 0;       // what the take was (Engine::kRec...)
    int      bars = 0, off = 0;
    float    bpm  = 0.f;
    uint32_t length = 0, gen = 0;
    char     path[kPathLen] = "", orig[kPathLen] = "";
};
LooperUndo lundo;

/** What track t holds now, described; `keep`: and whether its sound can go
 *  into the undo memory (recordings exist nowhere else; anything else
 *  reloads from where it came) */
void LooperDescribe(int t, LooperUndo& u, bool keep)
{
    uint32_t    cap = 0;
    int16_t*    mem = plat->UndoMem(&cap);
    const Slot& s   = eng->GetSlot(t);
    u.track         = t;
    u.loop          = eng->IsLoop(t);
    u.bars          = eng->LoopBars(t);
    u.off           = eng->LoopOffset(t);
    u.bpm           = eng->LoopBpm(t);
    u.length        = s.length;
    snprintf(u.path, kPathLen, "%s", slot_path[t]);
    snprintf(u.orig, kPathLen, "%s", slot_orig[t]);
    u.data = keep && mem && (u.loop || IsRecPath(slot_path[t])) && s.length && s.length <= cap
             && !eng->IsSynth(t);
}

void LooperBackup(int t, int mode)
{
    uint32_t cap = 0;
    int16_t* mem = plat->UndoMem(&cap);
    LooperDescribe(t, lundo, true);
    KitUndoForget(); // one step of undo: from here it's this track
    lundo.valid = lundo.redo = lundo.await = false;
    lundo.mode  = mode;
    if(lundo.data)
        memcpy(mem, eng->GetSlot(t).data, (size_t)lundo.length * sizeof(int16_t));
}

constexpr int kUndoBrowse = 3; // lundo.mode: not a take, a sample picked with the arrows or the encoder

/** Before a browsed sample goes into a track: keep what it held. Stepping on
 *  through files keeps the first one, so undo goes back to the original */
void BrowseBackup(int t)
{
    if(eng->LiveRecState() != 0)
        return;
    if(lundo.mode == kUndoBrowse && lundo.track == t && lundo.valid && !lundo.redo)
    {
        lundo.await = true; // same streak: the load under way changes the track again
        return;
    }
    LooperBackup(t, kUndoBrowse);
    /* a recording that didn't fit the undo memory (a long loop) can't come back */
    lundo.valid = lundo.data || !(lundo.loop || IsRecPath(lundo.path));
    lundo.await = true;
}

/** Puts back what the undo holds. `keep`: what the track has now goes into
 *  the undo instead, so the same call takes it back again (undo, redo);
 *  otherwise it's dropped (cancel) */
void LooperExchange(bool keep)
{
    const int t = lundo.track;
    if(t < 0)
        return;
    uint32_t   cap = 0;
    int16_t*   mem = plat->UndoMem(&cap);
    Slot&      s   = eng->GetSlot(t);
    LooperUndo cur = lundo;
    LooperDescribe(t, cur, keep);
    if(!lundo.data && lundo.mode == Engine::kRecOverdub)
    {
        lundo.valid = false; // nothing was kept (no memory): it stays as it is
        return;
    }
    /* the same loop with other sound in it (an overdub): it keeps running */
    const bool same = lundo.data && lundo.loop && cur.loop && lundo.length == cur.length
                      && lundo.bars == cur.bars;
    /* what comes back fits the slot and the track is on a long sound in spare
       memory (which reloads from its file): back into the slot, the claim frees itself */
    if(lundo.data && !cur.data && SpareOf(t) >= 0)
    {
        s.length = 0;
        SpareHome(t);
    }
    if(lundo.data && cur.data)
    {
        uint32_t n = lundo.length > cur.length ? lundo.length : cur.length;
        int16_t  tmp[512];
        for(uint32_t i = 0; i < n; i += 512)
        {
            size_t k = (n - i < 512 ? n - i : 512) * sizeof(int16_t);
            memcpy(tmp, s.data + i, k);
            memcpy(s.data + i, mem + i, k);
            memcpy(mem + i, tmp, k);
        }
    }
    else if(lundo.data)
        memcpy(s.data, mem, (size_t)lundo.length * sizeof(int16_t));
    else if(cur.data)
        memcpy(mem, s.data, (size_t)cur.length * sizeof(int16_t));
    bool await = false;
    if(lundo.data)
    {
        if(!same)
        {
            s.rate   = 1.f;
            s.length = lundo.length;
            if(lundo.loop)
                eng->SetLoop(t, lundo.bars, lundo.bpm, lundo.off);
            else
                eng->ClearLoop(t);
            snprintf(slot_path[t], kPathLen, "%s", lundo.path);
            snprintf(slot_orig[t], kPathLen, "%s", lundo.orig);
            ForgetReportedName(t);
        }
        slot_gen[t]++;
    }
    else
    {
        eng->ClearLoop(t);
        const char* p = lundo.orig[0] ? lundo.orig : lundo.path;
        char        k[8];
        if(!p[0] || IsRecPath(p))
        {
            snprintf(k, sizeof(k), "@k%d", BuiltInKitFor(t));
            p = k;
        }
        QueueLoad(t, p, false);
        await = true;
    }
    const bool redo = !lundo.redo;
    const int  mode = lundo.mode;
    lundo           = cur;
    lundo.mode      = mode;
    lundo.redo      = redo;
    lundo.await     = await;
    lundo.gen       = slot_gen[t];
    /* it can come back if its sound was kept, or it reloads from a path */
    lundo.valid = keep && (cur.data || !(cur.loop || IsRecPath(cur.path)));
}

/** mode: Engine::kRecLoop / kRecOverdub / kRecShot. false = not now */
bool LooperStart(int t, int mode)
{
    if(eng->LiveRecState() != 0 || TrackLoading(t) || set_op)
        return false;
    if(mode == Engine::kRecOverdub)
    {
        if(!eng->IsLoop(t))
        {
            Popup("track %d has no loop", t + 1);
            return false;
        }
        if(fabsf(eng->global.bpm - eng->LoopBpm(t)) > 0.002f * eng->LoopBpm(t))
        {
            Popup("overdub: at %d BPM only", (int)(eng->LoopBpm(t) + 0.5f));
            return false;
        }
    }
    /* where the take goes: the track's own slot, or spare memory when the
       chosen length doesn't fit a slot at this tempo: as much as the bars need
       and a sixteenth more, in case the tempo is eased down meanwhile (what it
       doesn't use goes back when it's kept). No room: say so now, before
       anything is armed. A take with no chosen length gets two slots' worth if
       there is room (it moves into the slot afterwards if it fits) and the
       slot if not. */
    int16_t* buf = eng->HomeData(t);
    uint32_t cap = eng->SlotCapacity();
    int      blk = -1;
    if(mode == Engine::kRecLoop)
    {
        const int      bars = eng->lr_len_bars;
        const uint32_t need = (uint32_t)bars * eng->BarSamples();
        if(bars == 0 || need + need / 64 > cap)
        {
            blk = SpareClaim(t, bars ? need + need / 16 : 0);
            if(blk >= 0)
            {
                buf = SpareBase(blk);
                cap = SpareCap(blk);
            }
            else if(SpareOf(t) >= 0 && (!bars || need <= eng->GetSlot(t).capacity)) // over its own long loop, in place
            {
                buf = eng->GetSlot(t).data;
                cap = eng->GetSlot(t).capacity;
            }
            else if(bars)
            {
                Popup("no room: %d bars at %d BPM", bars, (int)(eng->global.bpm + 0.5f));
                Log("looper: no room in spare memory for %d bars at %d BPM on track %d", bars,
                    (int)(eng->global.bpm + 0.5f), t + 1);
                return false;
            }
        }
    }
    if(t == sel_track)
        ReleaseKeys();
    LooperBackup(t, mode);
    if(mode != Engine::kRecOverdub)
    {
        take_block   = blk;
        take_swapped = buf != eng->GetSlot(t).data;
        take_prev    = take_swapped ? SpareOf(t) : -1;
        if(take_prev >= 0)
            SpareHold(take_prev, true);
    }
    const char* q = eng->quant_rec ? "beat" : "bar";
    if(mode == Engine::kRecOverdub)
    {
        eng->LiveRecOverdub(t);
        Popup("overdub %d: from the %s", t + 1, q);
    }
    else if(mode == Engine::kRecShot)
    {
        eng->LiveRecShot(t, buf, cap);
        Popup("sample %d: waiting for sound", t + 1);
    }
    else
    {
        eng->LiveRecArm(t, buf, cap);
        Popup("rec %d: from the %s", t + 1, q);
    }
    return true;
}

void LooperCancel()
{
    if(eng->LiveRecState() == 0)
        return;
    const bool own = take_swapped && eng->LiveRecMode() != Engine::kRecOverdub;
    eng->LiveRecCancel();
    if(own) // the take had its own buffer: the engine puts the track back on what it held
    {
        take_swapped = false;
        lundo.valid  = false;
        return;
    }
    LooperExchange(false); // arming already took the track's sound away
    Popup("rec cancelled");
}

bool KitUndoCan(bool redo); // kits.h
void KitUndoDo(bool redo);

bool LooperCan(bool redo)
{
    return lundo.valid && !lundo.await && lundo.track >= 0 && lundo.redo == redo
           && slot_gen[lundo.track] == lundo.gen && eng->LiveRecState() == 0 && !set_op
           && !TrackLoading(lundo.track);
}

/** One take back, or (redo) the take that was undone back again */
void LooperUndoTake(bool redo = false)
{
    if(eng->LiveRecState() != 0)
    {
        if(!redo)
            LooperCancel();
        return;
    }
    if(KitUndoCan(redo))
    {
        KitUndoDo(redo);
        return;
    }
    if(!LooperCan(redo))
    {
        /* the last take was an overdub whose loop didn't fit the undo memory (a long loop) */
        if(!redo && lundo.track >= 0 && lundo.mode == Engine::kRecOverdub && !lundo.data
           && slot_gen[lundo.track] == lundo.gen)
            Popup("overdub on long loop: no undo");
        else
            Popup(redo ? "nothing to redo" : "nothing to undo");
        return;
    }
    int t = lundo.track;
    LooperExchange(true);
    const bool br = lundo.mode == kUndoBrowse;
    Popup(redo ? (br ? "track %d: the new sound" : "take on %d back") : (br ? "track %d: sound back" : "take on %d undone"), t + 1);
    Log("undo: %s the last %s on track %d", redo ? "redid" : "undid", br ? "sample change" : "take", t + 1);
}

/** The APC40's OVERDUB button: on top of the selected track's loop */
void LooperOverdubButton()
{
    if(eng->LiveRecState() != 0)
    {
        if(eng->LiveRecState() == 2)
            eng->LiveRecStop();
        else
            LooperCancel();
        return;
    }
    LooperStart(sel_track, Engine::kRecOverdub);
}

void ToggleRecord()
{
    recording = !recording;
    eng->SetRecord(recording);
}

void TapTempo(uint32_t now)
{
    if(now - last_tap > 2000)
        tap_count = 0;
    else
    {
        tap_iv[tap_count % 4] = now - last_tap;
        tap_count++;
        int      n   = tap_count < 4 ? tap_count : 4;
        uint32_t sum = 0;
        for(int i = 0; i < n; i++)
            sum += tap_iv[i];
        float bpm = 60000.f * n / sum;
        if(bpm < 60.f)
            bpm = 60.f;
        if(bpm > 200.f)
            bpm = 200.f;
        eng->global.bpm = bpm;
        Popup("tap %d.%d bpm", (int)bpm, (int)(bpm * 10.f) % 10);
    }
    last_tap = now;
}

void SetBpm(float b)
{
    if(b < 60.f)
        b = 60.f;
    if(b > 200.f)
        b = 200.f;
    eng->global.bpm = b;
}

/** Lets go of every row 5 key and the held step's preview note */
void ReleaseKeys()
{
    for(int c = 0; c < 8; c++)
        if(key_note[c] >= 0)
        {
            eng->LiveNote(key_track[c], key_note[c], false);
            key_note[c] = -1;
        }
    if(held_preview >= 0)
    {
        eng->LiveNote(sel_track, held_preview, false);
        held_preview = -1;
    }
    held_step = -1;
}

void SelectTrack(int t)
{
    if(t == sel_track)
        return;
    ReleaseKeys();
    sel_track = t;
}

void ReleaseAllHeld();

/** SHIFT tapped on its own: strips switch between tracks 1-8 and 9-16 */
void TogglePage()
{
    ReleaseAllHeld();
    track_page ^= 1;
    sel_track = sel_track % 8 + 8 * track_page;
    for(int c = 0; c < 8; c++)
        fader_live[c] = false;
    Popup("tracks %s", track_page ? "9-16" : "1-8");
    Log("strips now tracks %s", track_page ? "9-16" : "1-8");
}

void ReleaseAllHeld()
{
    ReleaseKeys();
    for(int f = 0; f < kNumPerfFx; f++)
    {
        fx_pressed[f] = false;
        if(!fx_latched[f])
            eng->SetPerfFx(f, false);
    }
    for(int t = 0; t < kTracks; t++)
    {
        roll_held[t] = false;
        eng->SetRoll(t, false);
    }
    shift     = false;
    copy_held = false;
}

#include "xfer.h"
#include "sets.h"
#include "kits.h"
#include "apcmap.h"

/* ------------------------------------------------------------------ */
/* Looper and quantise settings on the card (apcbox/looper.txt), so    */
/* they are there again after a power cycle. Whatever changes one      */
/* (a page, the APC40) is seen by comparing; written 1.5 s after the   */
/* last change. The input monitor is left out: it starts off, so the   */
/* box never comes up feeding its input to the speakers.               */

const int kLset = 7;
const char* const lset_name[kLset] = {"len", "count", "rec", "pat", "mute", "fx", "gain"};
int      lset_saved[kLset];
int      lset_seen[kLset];
bool     lset_known = false; // the card was asked: from here on changes are written
uint32_t lset_changed_at;

void LsetRead(int* v)
{
    v[0] = eng->lr_len_bars;
    v[1] = eng->lr_countin ? 1 : 0;
    v[2] = eng->quant_rec;
    v[3] = eng->quant_pat;
    v[4] = eng->quant_mute;
    v[5] = eng->quant_fx;
    v[6] = in_gain_ui;
}

/** One setting from the card; anything out of range is left as it is */
void LsetApply(int i, int a)
{
    if(i == 0 && (a == 0 || a == 1 || a == 2 || a == 4 || a == 8))
        eng->lr_len_bars = (uint8_t)a;
    else if(i == 1 && a >= 0 && a <= 1)
        eng->lr_countin = a == 1;
    else if(i == 2 && a >= 0 && a <= 1)
        eng->quant_rec = (uint8_t)a;
    else if(i == 3 && a >= 0 && a <= 2)
        eng->quant_pat = (uint8_t)a;
    else if(i == 4 && a >= 0 && a <= 2)
        eng->quant_mute = (uint8_t)a;
    else if(i == 5 && a >= 0 && a <= 2)
        eng->quant_fx = (uint8_t)a;
    else if(i == 6 && a >= 0 && a <= 127)
        in_gain_ui = a, eng->in_gain = a == 64 ? 1.f : a * (2.f / 127.f) * a * (2.f / 127.f);
}

/** At power-up and when a card goes in */
void LsetLoad()
{
    char    txt[160];
    int32_t n = plat->ReadFile("apcbox/looper.txt", txt, sizeof(txt) - 1);
    if(n >= 5 && !strncmp(txt, "LPS1", 4))
    {
        txt[n]     = 0;
        char* line = strchr(txt, '\n');
        while(line && *++line)
        {
            char w[8];
            int  a;
            if(sscanf(line, "%7s %d", w, &a) == 2)
                for(int i = 0; i < kLset; i++)
                    if(!strcmp(w, lset_name[i]))
                        LsetApply(i, a);
            line = strchr(line, '\n');
        }
        Log("looper and quantise settings from the card");
    }
    LsetRead(lset_saved);
    LsetRead(lset_seen);
    lset_known = true;
}

void LsetPoll(uint32_t now)
{
    if(!lset_known)
        return;
    int v[kLset];
    LsetRead(v);
    if(memcmp(v, lset_seen, sizeof(v)))
    {
        memcpy(lset_seen, v, sizeof(v));
        lset_changed_at = now;
    }
    if(!memcmp(v, lset_saved, sizeof(v)) || now - lset_changed_at <= 1500 || !sd_ok || set_op || num_jobs
       || load_state != kLoadIdle || eng->LiveRecBusy())
        return;
    char txt[160];
    int  n = snprintf(txt, sizeof(txt), "LPS1\n");
    for(int i = 0; i < kLset; i++)
        n += snprintf(txt + n, sizeof(txt) - n, "%s %d\n", lset_name[i], v[i]);
    memcpy(lset_saved, v, sizeof(v)); // one try per change: a card that can't be written isn't asked every loop
    if(!plat->WriteFile("apcbox", "apcbox/looper.txt", txt, (uint32_t)n))
        Log("looper settings: can't write them to the card");
}

/** Knobs of one track back to default; fader, mute and solo stay */
void ResetTrackKnobs(int t)
{
    TrackParams  d;
    TrackParams& tp = eng->track[t];
    d.vol           = tp.vol;
    d.mute          = tp.mute;
    d.solo          = tp.solo;
    tp              = d;
    Log("track %d knobs reset", t + 1);
    Popup("track %d reset", t + 1);
}

/* ---- 303 track controls ----
 * Step pads: press an empty step = on. Press a lit one and let go =
 * on -> accent -> off. SHIFT + step = slide on/off. Hold a step and turn
 * device knob 1 to set its note; while stopped you hear it.
 * Row 5 = one octave, C D E F G A B C; SHIFT + key = the sharp.
 * UP / DOWN = octave. */
void AcidStepButton(int pat, int s, bool on)
{
    int t = sel_track;
    if(on)
    {
        if(held_step >= 0)
            return; // one step at a time
        uint8_t v = eng->GetStep(pat, t, s);
        if(shift)
        {
            bool sl = !eng->GetSlide(pat, t, s);
            eng->SetSlide(pat, t, s, sl);
            if(!v)
                eng->SetStep(pat, t, s, kStepOn);
            Popup("step %d slide %s", s + 1, sl ? "on" : "off");
            return;
        }
        held_step   = s;
        held_edited = false;
        held_new    = (v == kStepOff);
        if(held_new)
            eng->SetStep(pat, t, s, kStepOn);
        char nn[12];
        NoteName(nn, sizeof(nn), eng->GetNote(pat, t, s));
        Popup("step %d  %s", s + 1, nn);
        if(!eng->Playing())
        {
            held_preview = eng->GetNote(pat, t, s);
            eng->LiveNote(t, held_preview, true);
        }
        return;
    }
    if(s != held_step)
        return;
    if(held_preview >= 0)
    {
        eng->LiveNote(t, held_preview, false);
        held_preview = -1;
    }
    if(!held_new && !held_edited)
    {
        uint8_t v = eng->GetStep(pat, t, s);
        eng->SetStep(pat, t, s, v == kStepOn ? kStepAccent : kStepOff);
        if(v != kStepOn)
            eng->SetSlide(pat, t, s, false);
    }
    held_step = -1;
}

/** Device knob 1 while a 303 step is held: its note, C1..C5 */
void AcidHeldNote(uint8_t v)
{
    int pat  = eng->CurrentPattern();
    int note = kAcidLowNote + (v * (kAcidHighNote - kAcidLowNote) + 63) / 127;
    if(note == eng->GetNote(pat, sel_track, held_step))
        return;
    eng->SetNote(pat, sel_track, held_step, note);
    held_edited = true;
    char nn[12];
    NoteName(nn, sizeof(nn), note);
    Popup("step %d  %s", held_step + 1, nn);
    if(held_preview >= 0)
    {
        eng->LiveNote(sel_track, note, true); // glides to it
        eng->LiveNote(sel_track, held_preview, false);
        held_preview = note;
    }
}

int AcidKeyNote(int col, bool sharp)
{
    static const int kWhite[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    int n = 36 + 12 * acid_oct + kWhite[col];
    if(sharp && col != 2 && col != 6 && col != 7) // no E#, B#, C'#
        n++;
    return n;
}

void AcidKey(int col, bool on)
{
    if(on)
    {
        if(key_note[col] >= 0)
            return;
        int n           = AcidKeyNote(col, shift);
        key_note[col]   = n;
        key_track[col]  = sel_track;
        eng->LiveNote(sel_track, n, true);
        char nn[12];
        NoteName(nn, sizeof(nn), n);
        Popup("%s", nn);
    }
    else if(key_note[col] >= 0)
    {
        eng->LiveNote(key_track[col], key_note[col], false);
        key_note[col] = -1;
    }
}

char PatName(int p, char* buf)
{
    buf[0] = (char)('A' + p / 8);
    buf[1] = (char)('1' + p % 8);
    buf[2] = 0;
    return buf[0];
}

void PushUndo(int pat, const char* label)
{
    UndoEntry& u = undo_buf[undo_top];
    u.pat        = (int8_t)pat;
    snprintf(u.label, sizeof(u.label), "%s", label);
    eng->SavePattern(pat, u.data);
    undo_top = (undo_top + 1) % kUndoLevels;
    if(undo_count < kUndoLevels)
        undo_count++;
}

void ClearUndo() { undo_top = undo_count = 0; }

void Undo()
{
    if(undo_count == 0)
    {
        Popup("nothing to undo");
        return;
    }
    undo_top = (undo_top + kUndoLevels - 1) % kUndoLevels;
    undo_count--;
    UndoEntry& u = undo_buf[undo_top];
    eng->RestorePattern(u.pat, u.data);
    Log("undo: %s", u.label);
    Popup("undo: %s", u.label);
}

/* Row 3 while CLIP/TRACK is held */
void CopyPad(int col, uint32_t now)
{
    int  p = view_bank * 8 + col;
    char a[3], b[3], lbl[28];
    PatName(p, b);
    bool dbl = copy_tap_pad == p && copy_src == p && now - copy_tap_at < kDoubleTapMs;
    if(dbl) // double tap: clear
    {
        copy_src     = -1;
        copy_tap_pad = -1;
        if(shift)
        {
            snprintf(lbl, sizeof(lbl), "clear %s trk %d", b, sel_track + 1);
            PushUndo(p, lbl);
            eng->ClearTrack(p, sel_track);
            Popup("cleared %s, track %d", b, sel_track + 1);
        }
        else
        {
            snprintf(lbl, sizeof(lbl), "clear %s", b);
            PushUndo(p, lbl);
            eng->ClearPattern(p);
            Popup("cleared %s", b);
        }
        Log("%s", lbl);
        return;
    }
    if(copy_src < 0 || copy_src == p)
    {
        /* pick the source (tapping it again later drops it) */
        if(copy_src == p)
        {
            copy_src     = -1;
            copy_tap_pad = -1;
            Popup("copy: pick source");
            return;
        }
        copy_src     = p;
        copy_tap_pad = p;
        copy_tap_at  = now;
        Popup("copy %s > ?", b);
        return;
    }
    PatName(copy_src, a);
    copy_tap_pad = -1;
    if(shift) // only the selected track
    {
        snprintf(lbl, sizeof(lbl), "copy %s>%s trk %d", a, b, sel_track + 1);
        PushUndo(p, lbl);
        eng->CopyTrack(copy_src, p, sel_track);
        Popup("%s > %s, track %d only", a, b, sel_track + 1);
    }
    else
    {
        snprintf(lbl, sizeof(lbl), "copy %s>%s", a, b);
        PushUndo(p, lbl);
        eng->CopyPattern(copy_src, p);
        Popup("%s > %s", a, b);
    }
    Log("%s", lbl);
}

void GridButton(int row, int col, bool on)
{
    int pat = eng->CurrentPattern();
    switch(row)
    {
        case 0:
        case 1:
        {
            int s = row * 8 + col;
            if(eng->IsSynth(sel_track))
            {
                AcidStepButton(pat, s, on);
                return;
            }
            if(!on)
                return;
            uint8_t v = eng->GetStep(pat, sel_track, s);
            uint8_t n;
            if(shift)
                n = (v == kStepAccent) ? kStepOn : kStepAccent;
            else
                n = v ? kStepOff : kStepOn;
            eng->SetStep(pat, sel_track, s, n);
            break;
        }
        case 2:
            if(!on)
                return;
            if(sets_open)
                SetsApcPad(2, col);
            else if(copy_held)
                CopyPad(col, g_now);
            else if(shift)
            {
                view_bank = col; // SHIFT + row 3 picks the bank
                Popup("bank %c", 'A' + col);
            }
            else
                eng->QueuePattern(view_bank * 8 + col);
            break;
        case 3:
            if(sets_open)
            {
                if(on)
                    SetsApcPad(3, col);
                return;
            }
            if(on)
            {
                if(shift)
                {
                    fx_latched[col] = !fx_latched[col];
                    eng->SetPerfFx(col, fx_latched[col]);
                }
                else
                {
                    fx_latched[col] = false;
                    fx_pressed[col] = true;
                    eng->SetPerfFx(col, true);
                }
            }
            else
            {
                fx_pressed[col] = false;
                if(!fx_latched[col])
                    eng->SetPerfFx(col, false);
            }
            break;
        case 4:
            if(eng->IsSynth(sel_track) || key_note[col] >= 0)
            {
                AcidKey(col, on);
                return;
            }
            if(!on)
                return;
            eng->TriggerLive(StripTrack(col));
            break;
    }
}

void TrackButton(uint8_t note, int strip, bool on)
{
    const int    col = StripTrack(strip); // the track, despite the name
    TrackParams& tp  = eng->track[col];
    switch(note)
    {
        case apc::kRecArm:
            roll_held[col] = on;
            eng->SetRoll(col, on || ui_roll[col]);
            break;
        case apc::kSolo:
            if(!on)
                break;
            if(shift) // SHIFT + SOLO: crossfader side AB -> A -> B -> AB
            {
                tp.xfade = (uint8_t)((tp.xfade + 1) % 3);
                Popup("xfade %d: %s", col + 1, tp.xfade == 1 ? "A (left)" : (tp.xfade == 2 ? "B (right)" : "AB (both)"));
            }
            else
                tp.solo = !tp.solo;
            break;
        case apc::kActivator:
            if(on)
                tp.mute = !tp.mute;
            break;
        case apc::kTrackSel:
            if(on && copy_held) // CLIP/TRACK + TRACK SELECT: bank A-H
            {
                view_bank = strip;
                Popup("bank %c", 'A' + strip);
            }
            else if(on)
            {
                track_notes = true;
                SelectTrack(col);
            }
            break;
        case apc::kClipStop:
            if(!on)
                return;
            if(shift)
            {
                char lbl[28], b[3];
                int  p = eng->CurrentPattern();
                PatName(p, b);
                snprintf(lbl, sizeof(lbl), "clear %s trk %d", b, col + 1);
                PushUndo(p, lbl);
                eng->ClearTrack(p, col);
                Popup("cleared %s, track %d", b, col + 1);
            }
            else
                eng->Audition(col);
            break;
    }
}

void OnButton(uint8_t ch, uint8_t note, bool on, uint32_t now)
{
    using namespace apc;
    if(note >= kClipFirst && note < kClipFirst + 5)
    {
        if(ch < 8)
            GridButton(note - kClipFirst, ch, on);
        return;
    }
    if(note >= kRecArm && note <= kClipStop)
    {
        if(ch < 8)
            TrackButton(note, ch, on);
        return;
    }
    if(note >= kSceneFirst && note < kSceneFirst + 5)
    {
        if(on)
            LoadKit(kit_page * 5 + (note - kSceneFirst)); // into this page
        return;
    }
    if(note >= kPan && note <= kSendC)
    {
        if(on)
            page = note - kPan;
        return;
    }
    switch(note)
    {
        case kShift:
            if(on)
            {
                shift         = true;
                shift_used    = false;
                shift_down_at = now;
            }
            else
            {
                bool tap = shift && !shift_used && now - shift_down_at < 500;
                shift    = false;
                if(tap)
                    TogglePage();
            }
            break;
        case kClipTrack:
            if(on && shift) // SHIFT + CLIP/TRACK: undo
            {
                Undo();
                break;
            }
            copy_held = on;
            copy_src  = -1;
            copy_tap_pad = -1;
            if(on)
                Popup("copy: pick source");
            break;
        case kPlay:
            if(on)
                eng->Play();
            break;
        case kStop:
            if(on)
                eng->Stop();
            break;
        case kRec: // live sampling from the audio input into the selected track
            if(!on)
                break;
            if(shift) // SHIFT + REC: drop the recorded loop
            {
                if(eng->LiveRecState() != 0)
                    eng->LiveRecCancel();
                if(IsRecPath(slot_path[sel_track]) || eng->IsLoop(sel_track))
                {
                    char p[8];
                    snprintf(p, sizeof(p), "@k%d", BuiltInKitFor(sel_track));
                    QueueLoad(sel_track, p, false);
                    Popup("loop %d cleared", sel_track + 1);
                }
                break;
            }
            switch(eng->LiveRecState())
            {
                case 0:
                    if(TrackLoading(sel_track))
                        break;
                    LooperStart(sel_track, Engine::kRecLoop);
                    break;
                case 1:
                    LooperCancel();
                    break;
                default:
                    eng->LiveRecStop();
                    Popup("rec %d: ends with the bar", eng->LiveRecTrack() + 1);
            }
            break;
        case kOverdub: // overdub on the selected track's loop; again = stop
            if(on)
                LooperOverdubButton();
            break;
        case kMaster:
            if(on)
                ToggleRecord();
            break;
        case kStopAll:
            if(on)
            {
                for(int f = 0; f < kNumPerfFx; f++)
                {
                    fx_latched[f] = false;
                    eng->SetPerfFx(f, false);
                }
                eng->Panic();
                if(shift)
                    reset_hold_start = now | 1; // full reset after 1 s held
            }
            else
                reset_hold_start = 0;
            break;
        case kMetronome:
            if(!on)
                break;
            if(shift) // SHIFT + METRONOME: hear the audio input
            {
                eng->input_monitor = !eng->input_monitor;
                Popup("input monitor %s", eng->input_monitor ? "on" : "off");
            }
            else
                eng->global.metronome = !eng->global.metronome;
            break;
        case kTap:
            if(on)
                TapTempo(now);
            break;
        case kNudgePlus:
            if(on)
            {
                SetBpm(roundf(eng->global.bpm) + (shift ? 5.f : 1.f));
                Popup("tempo %d", (int)eng->global.bpm);
            }
            break;
        case kNudgeMinus:
            if(on)
            {
                SetBpm(roundf(eng->global.bpm) - (shift ? 5.f : 1.f));
                Popup("tempo %d", (int)eng->global.bpm);
            }
            break;
        case kUp:
        case kDown:
            if(!on)
                break;
            if(eng->IsSynth(sel_track)) // 303 selected: keyboard octave
            {
                int o = acid_oct + (note == kUp ? 1 : -1);
                if(o >= -1 && o <= 2)
                    acid_oct = o;
                Popup("octave C%d", acid_oct + 2);
            }
            else if(note == kUp && kit_page > 0)
                kit_page--;
            else if(note == kDown && (kit_page + 1) * 5 < num_kits)
                kit_page++;
            break;
        case kLeft:
        case kRight:
            if(on && num_folders > 0)
            {
                cur_folder += (note == kRight) ? 1 : -1;
                if(cur_folder < 0)
                    cur_folder = num_folders - 1;
                if(cur_folder >= num_folders)
                    cur_folder = 0;
                ScanFiles(cur_folder);
                if(num_files)
                {
                    cur_file = 0;
                    LoadBrowsedFile(true);
                }
            }
            break;
        case kDevLeft:
        case kDevRight:
            if(on && !shift) // undo / redo: the last take, or a sample changed with SHIFT + these or the BANK arrows
                LooperUndoTake(note == kDevRight);
            else if(on && num_files > 0) // SHIFT: previous / next sample in the folder
            {
                int d = (note == kDevRight) ? 1 : -1;
                if(cur_file < 0)
                    cur_file = 0;
                else
                    cur_file = (cur_file + d + num_files) % num_files;
                LoadBrowsedFile(true);
            }
            break;
        case kDevOnOff:
            if(!on)
                break;
            if(shift)
                ResetTrackKnobs(sel_track);
            else
            {
                /* SD sample -> built-in sound -> 303 -> built-in sound */
                char p[8];
                if(IsSdSample(slot_path[sel_track]) || IsSynthPath(slot_path[sel_track])
                   || IsRecPath(slot_path[sel_track]))
                {
                    snprintf(p, sizeof(p), "@k%d", BuiltInKitFor(sel_track));
                    QueueLoad(sel_track, p, true);
                    Popup("%d %s", sel_track + 1,
                          Engine::BuiltInSoundName(BuiltInKitFor(sel_track), sel_track));
                }
                else
                {
                    ReleaseKeys();
                    QueueLoad(sel_track, kSynthPath, false);
                    Popup("%d 303", sel_track + 1);
                }
                loaded_kit_pg[sel_track / 8] = -1;
            }
            break;
        case kDetail: // SHIFT + DETAIL VIEW: the sets screen
            if(on && shift)
                SetsOpen(!sets_open);
            break;
        case kRecQuant:
            if(on && shift)
                LoadProject();
            break;
        default: break;
    }
}

inline float Knob(uint8_t v)
{
    return v * (1.f / 127.f);
}

inline uint8_t Ring(float p)
{
    int v = (int)(p * 127.f + 0.5f);
    return (uint8_t)(v < 0 ? 0 : (v > 127 ? 127 : v));
}

void OnCC(uint8_t ch, uint8_t cc, uint8_t v, uint32_t now)
{
    using namespace apc;
    if(cc == kFaderCC)
    {
        if(ch < 8)
        {
            int  t      = StripTrack(ch);
            int  target = Ring(eng->track[t].vol);
            int  prev   = fader_pos[ch];
            fader_pos[ch] = v;
            if(!fader_live[ch])
            {
                /* close enough, or moved across the level since last time */
                bool crossed = prev >= 0 && ((prev - target) * ((int)v - target) <= 0);
                if(abs((int)v - target) <= 2 || crossed)
                    fader_live[ch] = true;
                else
                {
                    Popup("vol %d %d%%: %s", t + 1, target * 100 / 127,
                          (int)v < target ? "go up" : "go down");
                    return;
                }
            }
            eng->track[t].vol = Knob(v);
            Popup("vol %d  %d%%", t + 1, (int)(Knob(v) * 100.f + 0.5f));
        }
        return;
    }
    if(cc == kMasterCC)
    {
        eng->global.master = Knob(v);
        Popup("master %d%%", (int)(Knob(v) * 100.f + 0.5f));
        return;
    }
    if(cc == kXfadeCC)
    {
        float x = (v == 63 || v == 64) ? 0.5f : Knob(v); // centre = both full
        eng->global.xfader = x;
        int a = (int)(fminf(1.f, 2.f * (1.f - x)) * 100.f + 0.5f);
        int b = (int)(fminf(1.f, 2.f * x) * 100.f + 0.5f);
        Popup("A %d%%  B %d%%", a, b);
        return;
    }
    if(cc == kCueCC)
    {
        /* CUE LEVEL is an endless knob sending steps (1..63 right,
           65..127 left): master LP/HP filter, off in the middle */
        int   d = v < 64 ? v : (int)v - 128;
        float x = eng->global.djfilter + d * (1.f / 64.f);
        float o = eng->global.djfilter;
        if((o < 0.5f && x > 0.5f) || (o > 0.5f && x < 0.5f))
            x = 0.5f; // stop at "off" on the way through
        eng->global.djfilter = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
        char f[16];
        FmtFilter(f, sizeof(f), eng->global.djfilter);
        Popup("filter %s", f);
        return;
    }
    if(cc >= kTrkKnobCC && cc < kTrkKnobCC + 8)
    {
        int           k = cc - kTrkKnobCC;
        float         p = Knob(v);
        GlobalParams& g = eng->global;
        char          f[16];
        FmtPct(f, sizeof(f), p);
        switch(page)
        {
            case kPagePan:
                eng->track[StripTrack(k)].pan = p;
                FmtPan(f, sizeof(f), p);
                Popup("pan %d  %s", StripTrack(k) + 1, f);
                break;
            case kPageRev:
                eng->track[StripTrack(k)].rev = p;
                Popup("reverb %d  %s", StripTrack(k) + 1, f);
                break;
            case kPageDly:
                eng->track[StripTrack(k)].dly = p;
                Popup("delay %d  %s", StripTrack(k) + 1, f);
                break;
            case kPageGlobal:
                switch(k)
                {
                    case 0:
                        SetBpm(60.f + v);
                        Popup("tempo %d", 60 + v);
                        break;
                    case 1:
                        g.swing = p;
                        Popup("swing %d%%", (int)(50.f + p * 25.f + 0.5f));
                        break;
                    case 2: g.rev_size = p; Popup("rev size %s", f); break;
                    case 3: g.rev_tone = p; Popup("rev tone %s", f); break;
                    case 4:
                    {
                        g.dly_time = p;
                        int di     = (int)(p * 6.f);
                        Popup("delay %s", kDivNames[di > 5 ? 5 : di]);
                        break;
                    }
                    case 5: g.dly_fb = p; Popup("delay fb %s", f); break;
                    case 6: g.drive = p; Popup("drive %s", f); break;
                    case 7: g.fx_return = p; Popup("fx return %s", f); break;
                }
                break;
        }
        return;
    }
    if(cc >= kDevKnobCC && cc < kDevKnobCC + 8)
    {
        /* The APC40 banks the device knobs per track and sends them on
           that track's channel: TRACK SELECT switches the bank and sends
           all 8 values. So the channel is the selected track. Channel 8
           is the master track: no device parameters there. */
        if(ch >= 8)
            return;
        /* In the host-controlled modes the knobs aren't banked: they edit
           whatever track TRACK SELECT picked. In generic mode the channel
           is the bank. */
        if(!track_notes)
            SelectTrack(StripTrack(ch));
        TrackParams& tp = eng->track[sel_track];
        float        p  = Knob(v);
        int          k  = cc - kDevKnobCC;
        if(eng->IsSynth(sel_track))
        {
            if(k == 0 && held_step >= 0)
            {
                AcidHeldNote(v);
                return;
            }
            if(k == 5 && (v == 63 || v == 64))
                p = 0.5f; // tune: snap to exactly 0 st at the centre
            AcidParams& a        = tp.acid;
            float*      af[8]    = {&a.cutoff, &a.reso,  &a.envmod, &a.decay,
                                    &a.accent, &a.tune, &a.slide,  &a.wave};
            bool        changed  = (int)(*af[k] * 127.f + 0.5f) != v;
            *af[k]               = p;
            if(!changed)
                return;
            char f[16];
            switch(k)
            {
                case 0:
                    FmtHz(f, sizeof(f), 150.f * powf(2.f, p * 5.f));
                    Popup("cutoff %s", f);
                    break;
                case 1: FmtPct(f, sizeof(f), p); Popup("reso %s", f); break;
                case 2: FmtPct(f, sizeof(f), p); Popup("env mod %s", f); break;
                case 3: Popup("decay %d ms", (int)(30.f * powf(80.f, p))); break;
                case 4: FmtPct(f, sizeof(f), p); Popup("accent %s", f); break;
                case 5:
                {
                    int c10 = (int)roundf((p - 0.5f) * 240.f);
                    Popup("tune %c%d.%d st", c10 < 0 ? '-' : '+', abs(c10) / 10, abs(c10) % 10);
                    break;
                }
                case 6: Popup("slide %d ms", (int)(12.f * powf(25.f, p))); break;
                case 7: Popup("wave %s", p >= 0.5f ? "square" : "saw"); break;
            }
            return;
        }
        float* fields[8] = {&tp.pitch, &tp.decay, &tp.filter, &tp.reso,
                            &tp.start, &tp.reverse, &tp.drive, &tp.prob};
        /* TRACK SELECT resends all 8 unchanged values: no popup for those */
        bool changed = (int)(*fields[k] * 127.f + 0.5f) != v;
        *fields[k]   = p;
        if(!changed)
            return;
        char f[16];
        switch(k)
        {
            case 0:
            {
                int st = (int)roundf((p - 0.5f) * 24.f);
                Popup("pitch %+d st", st);
                break;
            }
            case 1:
                if(p > 0.98f)
                    Popup("decay full");
                else
                    Popup("decay %d ms", (int)(10.f * powf(400.f, p)));
                break;
            case 2:
                FmtFilter(f, sizeof(f), p);
                Popup("filter %s", f);
                break;
            case 3: FmtPct(f, sizeof(f), p); Popup("reso %s", f); break;
            case 4: FmtPct(f, sizeof(f), p); Popup("start %s", f); break;
            case 5: Popup("%s", p >= 0.5f ? "reverse" : "forward"); break;
            case 6: FmtPct(f, sizeof(f), p); Popup("drive %s", f); break;
            case 7: FmtPct(f, sizeof(f), p); Popup("chance %s", f); break;
        }
    }
}

void HandleEvent(int type, int ch, int d0, int d1, uint32_t now)
{
    g_now = now;
    Out("E %d %d %d %d", type, ch, d0, d1);
    if(shift && !((type == kMidiNoteOn || type == kMidiNoteOff) && d0 == apc::kShift))
        shift_used = true; // SHIFT + something: not a page tap
    if(type == kMidiNoteOn || type == kMidiNoteOff)
        MapButton(ch, d0, type == kMidiNoteOn && d1 > 0, now); // through the button map (apcmap.h)
    else if(type == kMidiCC)
        MapCC(ch, d0, d1, now);
}

/* ================================================================== */
/* What the LEDs should show                                            */


void UpdateLeds(uint32_t now)
{
    using namespace apc;
    const int  pat     = eng->CurrentPattern();
    const int  queued  = eng->QueuedPattern();
    const bool playing = eng->Playing();
    const int  step    = eng->ClockStep();

    for(int t = 0; t < kTracks; t++)
    {
        uint32_t tc = eng->TriggerCount(t);
        if(tc != last_trig[t])
        {
            last_trig[t]   = tc;
            flash_until[t] = now + 70;
        }
    }

    /* Rows 1-2: steps of the selected track */
    const bool acid = eng->IsSynth(sel_track);
    for(int s = 0; s < kSteps; s++)
    {
        uint8_t v = eng->GetStep(pat, sel_track, s);
        uint8_t c = v == kStepAccent ? kRed : (v ? kGreen : kOff);
        if(acid && v && eng->GetSlide(pat, sel_track, s)) // slide: blinks
            c = v == kStepAccent ? kRedBlink : kGreenBlink;
        if(playing && s == step)
            c = kYellow;
        Want(led_grid[s / 8][s % 8], c);
    }
    /* Row 3: patterns of the viewed bank, or the banks while SHIFT is held */
    for(int i = 0; i < 8; i++)
    {
        uint8_t c = kOff;
        if(shift)
        {
            bool has = false;
            for(int j = 0; j < 8 && !has; j++)
                has = !eng->PatternEmpty(i * 8 + j);
            if(i == view_bank)
                c = kGreen;
            else if(i == pat / 8)
                c = kGreenBlink; // the playing pattern is in this bank
            else if(has)
                c = kYellow;
        }
        else
        {
            int p = view_bank * 8 + i;
            if(copy_held && p == copy_src)
                c = kYellowBlink;
            else if(p == pat)
                c = kGreen;
            else if(p == queued)
                c = kGreenBlink;
            else if(!eng->PatternEmpty(p))
                c = kYellow;
        }
        Want(led_grid[2][i], c);
    }
    /* Row 4: performance effects */
    for(int f = 0; f < kNumPerfFx; f++)
    {
        uint8_t c = kYellow;
        if(fx_latched[f])
            c = kRedBlink;
        else if(eng->PerfFxActive(f))
            c = kRed;
        Want(led_grid[3][f], c);
    }
    /* Sets screen: row 3 = slots, row 4 = LOAD, SAVE HERE, SAVE NEW, DELETE, FX */
    if(sets_open)
    {
        for(int i = 0; i < 8; i++)
        {
            uint8_t c = kOff;
            if(i < kSetSlots)
            {
                if(i == sets_cur)
                    c = i == open_set ? kGreenBlink : (set_exists[i] ? kYellowBlink : kRedBlink);
                else if(i == open_set)
                    c = kGreen;
                else if(set_exists[i])
                    c = kYellow;
            }
            Want(led_grid[2][i], c);
        }
        for(int f = 0; f < 8; f++)
        {
            uint8_t c = kOff;
            if(f < 5)
                c = apc_confirm == f ? kRedBlink : (f == 3 ? kRed : (f == 4 ? kYellow : kGreen));
            if(set_op && f < 4)
                c = kGreenBlink;
            Want(led_grid[3][f], c);
        }
    }
    /* Row 5: live pads, or the keyboard on a 303 track */
    for(int col = 0; col < 8; col++)
    {
        uint8_t c = kOff;
        if(acid)
        {
            c = key_note[col] >= 0 ? kRed : ((col == 0 || col == 7) ? kYellow : kGreen);
            Want(led_grid[4][col], c);
            continue;
        }
        int t = StripTrack(col);
        if(now < flash_until[t])
            c = kGreen;
        else if(eng->track[t].mute)
            c = kRed;
        else if(t == sel_track)
            c = kYellow;
        Want(led_grid[4][col], c);
    }

    /* Full reset: grid blinks red while held, solid red when done */
    if(reset_hold_start || now < flash_red_until)
    {
        uint8_t c = reset_hold_start ? kRedBlink : kRed;
        for(int r = 0; r < 5; r++)
            for(int i = 0; i < 8; i++)
                Want(led_grid[r][i], c);
    }

    /* Track buttons */
    for(int col = 0; col < 8; col++)
    {
        const int          t  = StripTrack(col);
        const TrackParams& tp = eng->track[t];
        bool has_steps = false;
        for(int s = 0; s < kSteps && !has_steps; s++)
            has_steps = eng->GetStep(pat, t, s) != kStepOff;
        Want(led_trk[0][col], roll_held[t] ? 1 : 0);
        /* SOLO row: while SHIFT is held it shows the crossfader side
           instead (off = AB, lit = A, blinking = B) */
        Want(led_trk[1][col], shift ? tp.xfade : (tp.solo ? 1 : 0));
        Want(led_trk[2][col], tp.mute ? 0 : 1);
        Want(led_trk[3][col], copy_held ? (col == view_bank ? 1 : 0) : (t == sel_track ? 1 : 0));
        Want(led_trk[4][col], TrackLoading(t) ? 2 : (has_steps ? 1 : 0));
    }

    Want(led_master, recording ? (playing ? 2 : 1) : 0);
    for(int i = 0; i < 5; i++)
    {
        int k = kit_page * 5 + i;
        Want(led_scene[i], (k == loading_kit && loading_page == track_page)
                               ? 2
                               : (k == loaded_kit_pg[track_page] ? 1 : 0));
    }
    for(int i = 0; i < 4; i++)
        Want(led_page[i], page == i ? 1 : 0);

    /* Device buttons */
    uint8_t dev[8] = {
        (uint8_t)(copy_held ? 1 : 0),
        (uint8_t)(IsSdSample(slot_path[sel_track]) ? 1 : 0), // lit = SD sample
        0,
        0,
        0,
        0,
        (uint8_t)(recording ? 1 : 0),
        (uint8_t)(eng->global.metronome ? 1 : 0),
    };
    for(int k = 0; k < 8; k++)
        for(int c = 0; c < 9; c++)
        {
            uint8_t v = dev[k];
            if(k == 1) // DEVICE ON/OFF: that bank's track has an SD sample or 303
            {
                int t = track_notes ? sel_track : StripTrack(c);
                v     = (c < 8
                     && (IsSdSample(slot_path[t]) || IsSynthPath(slot_path[t])
                         || IsRecPath(slot_path[t])))
                            ? 1
                            : 0;
            }
            Want(led_dev[k][c], v);
        }

    /* Track knob rings follow the page */
    const GlobalParams& g = eng->global;
    for(int k = 0; k < 8; k++)
    {
        const TrackParams& tp   = eng->track[StripTrack(k)];
        uint8_t            val  = 0;
        uint8_t            type = kRingVolume;
        switch(page)
        {
            case kPagePan:
                val  = Ring(tp.pan);
                type = kRingPan;
                break;
            case kPageRev: val = Ring(tp.rev); break;
            case kPageDly: val = Ring(tp.dly); break;
            case kPageGlobal:
            {
                const float gv[8] = {0.f,
                                     g.swing,
                                     g.rev_size,
                                     g.rev_tone,
                                     g.dly_time,
                                     g.dly_fb,
                                     g.drive,
                                     g.fx_return};
                if(k == 0)
                {
                    int b = (int)(g.bpm + 0.5f) - 60;
                    val   = (uint8_t)(b < 0 ? 0 : (b > 127 ? 127 : b));
                    type  = kRingSingle;
                }
                else
                {
                    val = Ring(gv[k]);
                    if(k == 4)
                        type = kRingSingle;
                }
                break;
            }
        }
        Want(led_trk_ring[k], val);
        Want(led_trk_type[k], type);
    }

    /* Device knob rings: bank c (channel c) shows track c, so switching
       tracks on the APC40 shows that track's settings */
    const uint8_t dt[8] = {kRingPan,    kRingVolume, kRingPan,
                           kRingVolume, kRingVolume, kRingSingle,
                           kRingVolume, kRingVolume};
    const uint8_t at[8] = {kRingVolume, kRingVolume, kRingVolume, kRingVolume,
                           kRingVolume, kRingPan,    kRingVolume, kRingSingle};
    for(int c = 0; c < 9; c++)
    {
        float dv[8] = {};
        int   src   = track_notes ? sel_track : StripTrack(c % 8); // unbanked: the selection
        const uint8_t* types = dt;
        if(c < 8 || track_notes)
        {
            const TrackParams& tp = eng->track[src];
            if(eng->IsSynth(src))
            {
                const AcidParams& a = tp.acid;
                float v[8] = {a.cutoff, a.reso, a.envmod, a.decay,
                              a.accent, a.tune, a.slide,  a.wave};
                if(src == sel_track && held_step >= 0) // knob 1 = the note
                    v[0] = (eng->GetNote(pat, src, held_step) - kAcidLowNote)
                           / (float)(kAcidHighNote - kAcidLowNote);
                memcpy(dv, v, sizeof(dv));
                types = at;
            }
            else
            {
                float v[8] = {tp.pitch, tp.decay, tp.filter, tp.reso,
                              tp.start, tp.reverse, tp.drive, tp.prob};
                memcpy(dv, v, sizeof(dv));
            }
        }
        for(int k = 0; k < 8; k++)
        {
            Want(led_dev_ring[k][c], Ring(dv[k]));
            Want(led_dev_type[k][c], (c < 8 || track_notes) ? types[k] : 0);
        }
    }
}


/* ================================================================== */
/* OLED screen, 128x64, 1 bit, SSD1306 page layout (byte = 8 rows)      */

uint8_t  fb[1024];
uint32_t fb_version = 0;
uint8_t  fb_prev[1024];

inline void Px(int x, int y, bool on = true)
{
    if(x < 0 || x >= 128 || y < 0 || y >= 64)
        return;
    uint8_t& b = fb[(y >> 3) * 128 + x];
    if(on)
        b |= 1 << (y & 7);
    else
        b &= ~(1 << (y & 7));
}

void FillRect(int x, int y, int w, int h, bool on = true)
{
    for(int j = y; j < y + h; j++)
        for(int i = x; i < x + w; i++)
            Px(i, j, on);
}

/** Draws text, returns the x after it. Clips at the right edge. */
int Text(int x, int y, const OledFont& f, const char* s, bool on = true)
{
    for(; *s; s++)
    {
        int c = (unsigned char)*s;
        if(c < 32 || c > 126)
            c = '?';
        const uint16_t* rows = f.rows + (c - 32) * f.h;
        for(int r = 0; r < f.h; r++)
            for(int i = 0; i < f.w; i++)
                if(rows[r] & (1u << (f.w - 1 - i)))
                    Px(x + i, y + r, on);
        x += f.w;
        if(x >= 128)
            break;
    }
    return x;
}

int TextWidth(const OledFont& f, const char* s)
{
    return (int)strlen(s) * f.w;
}

/** "3 closed hat" or "3 BD_tape" for the selected track */
void TrackLabel(char* o, size_t n, int t)
{
    const char* p = slot_path[t];
    if(IsSdSample(p))
    {
        const char* b = strrchr(p, '/');
        b             = b ? b + 1 : p;
        snprintf(o, n, "%d %s", t + 1, b);
        char* dot = strrchr(o, '.');
        if(dot)
            *dot = 0;
    }
    else if(IsSynthPath(p))
        snprintf(o, n, "%d 303", t + 1);
    else if(IsRecPath(p) && !eng->IsLoop(t))
        snprintf(o, n, "%d recorded", t + 1);
    else if(IsRecPath(p))
        snprintf(o, n, "%d loop %d bar%s", t + 1, eng->LoopBars(t), eng->LoopBars(t) == 1 ? "" : "s");
    else
    {
        int k = BuiltInKitOf(p);
        snprintf(o, n, "%d %s", t + 1, Engine::BuiltInSoundName(k, t));
    }
}

#include "tftui.h"
#include "touch.h"

const char* const kFxShort[kNumPerfFx]
    = {"RPT1/4", "RPT1/8", "RPT1/16", "RPT1/32", "TAPE", "REV", "CRUSH", "GATE"};

/** OLED while the sets screen is open (no colour screen needed) */
void ComposeSets(uint32_t now)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "SETS%s", set_dirty ? " *unsaved" : "");
    Text(0, -1, kFontSmall, buf);
    int first = sets_cur - 1;
    if(first < 0)
        first = 0;
    if(first > kSetSlots - 4)
        first = kSetSlots - 4 > 0 ? kSetSlots - 4 : 0;
    for(int r = 0; r < 4 && first + r < kSetSlots; r++)
    {
        int  i = first + r, y = 9 + r * 9;
        bool cur = i == sets_cur;
        snprintf(buf, sizeof(buf), "%c%02d %s", i == open_set ? '>' : ' ', i + 1,
                 set_exists[i] ? set_list[i].name : "-");
        if(cur)
            FillRect(0, y, 128, 9);
        Text(1, y, kFontSmall, buf, !cur);
    }
    if(now < popup_until)
    {
        FillRect(0, 46, 128, 9);
        Text(1, 46, kFontSmall, popup, false);
    }
    else if(set_op)
        Text(0, 46, kFontSmall, op_msg);
    else if(sets_mode == kSetsConfirm)
        Text(0, 46, kFontSmall, confirm_yes ? "confirm: YES" : "confirm: no");
    else if(sets_mode == kSetsActions)
    {
        snprintf(buf, sizeof(buf), "> %s", sets_act == kActTrans ? kTransNames[set_trans] : kActNames[sets_act]);
        Text(0, 46, kFontSmall, buf);
    }
    else
    {
        snprintf(buf, sizeof(buf), "fx %s", kTransNames[set_trans]);
        Text(0, 46, kFontSmall, buf);
    }
    Text(0, 55, kFontSmall, "row4: ld sv new del fx");
    if(memcmp(fb, fb_prev, sizeof(fb)))
    {
        memcpy(fb_prev, fb, sizeof(fb));
        fb_version++;
    }
}

void ComposeScreen(uint32_t now)
{
    memset(fb, 0, sizeof(fb));
    if(sets_open)
    {
        ComposeSets(now);
        return;
    }
    char       buf[48];
    const int  pat     = eng->CurrentPattern();
    const int  queued  = eng->QueuedPattern();
    const bool playing = eng->Playing();

    /* Top: pattern, transport, tempo */
    snprintf(buf, sizeof(buf), "%c%d", 'A' + pat / 8, pat % 8 + 1);
    Text(0, 0, kFontBig, buf);
    if(playing)
        for(int i = 0; i < 6; i++) // play triangle
            FillRect(24 + i, 3 + i, 1, 11 - 2 * i);
    else
        FillRect(24, 4, 8, 8); // stop square
    if(queued >= 0)
    {
        snprintf(buf, sizeof(buf), ">%c%d", 'A' + queued / 8, queued % 8 + 1);
        Text(36, 7, kFontSmall, buf);
    }
    if(recording && (!playing || (now / 250) % 2))
    {
        FillRect(36, 0, 19, 7);
        Text(37, -1, kFontSmall, "REC", false);
    }
    float bpm = eng->global.bpm;
    int   b10 = (int)(bpm * 10.f + 0.5f);
    if(b10 % 10)
        snprintf(buf, sizeof(buf), "%d.%d", b10 / 10, b10 % 10);
    else
        snprintf(buf, sizeof(buf), "%d", b10 / 10);
    Text(128 - TextWidth(kFontBig, buf), 0, kFontBig, buf);

    /* Selected track */
    TrackLabel(buf, sizeof(buf), sel_track);
    int lx = Text(0, 18, kFontSmall, buf);
    if(eng->IsSynth(sel_track))
    {
        snprintf(buf, sizeof(buf), " oct C%d", acid_oct + 2);
        Text(lx, 18, kFontSmall, buf);
    }
    const char* pg = track_page ? "9-16" : "1-8";
    int         px = 128 - TextWidth(kFontSmall, pg);
    if(eng->track[sel_track].mute)
    {
        Text(128 - 6, 18, kFontSmall, "M");
        px -= 9;
    }
    if(lx < px - 2)
        Text(px, 18, kFontSmall, pg);

    /* Message line: last control touched, else kit and bank */
    if(now < popup_until)
    {
        FillRect(0, 28, 128, 11);
        Text(2, 29, kFontSmall, popup, false);
    }
    else
    {
        if(loading_kit >= 0)
            snprintf(buf, sizeof(buf), "loading %s", kit_names[loading_kit]);
        else if(loaded_kit_pg[track_page] >= 0)
            snprintf(buf, sizeof(buf), "kit %s", kit_names[loaded_kit_pg[track_page]]);
        else
            snprintf(buf, sizeof(buf), "kit (edited)");
        Text(0, 29, kFontSmall, buf);
        snprintf(buf, sizeof(buf), "bank %c", 'A' + view_bank);
        Text(128 - TextWidth(kFontSmall, buf), 29, kFontSmall, buf);
    }

    /* Steps of the selected track, with the playhead under them */
    for(int st = 0; st < kSteps; st++)
    {
        int     x = st * 8 + 1;
        uint8_t v = eng->GetStep(pat, sel_track, st);
        if(v == kStepAccent)
            FillRect(x, 41, 6, 9);
        else if(v == kStepOn)
            FillRect(x, 43, 6, 6);
        else
            FillRect(x + 2, 45, 2, 2);
        if(v && eng->IsSynth(sel_track) && eng->GetSlide(pat, sel_track, st))
            FillRect(x + 6, 45, 2, 2); // slide: joins it to the next step
        if(st % 4 == 0)
            Px(x - 1, 51);
        if(playing && st == eng->CurrentStep())
            FillRect(x, 52, 6, 2);
    }

    /* Footer: active effect or CPU */
    int fx = -1;
    for(int f = 0; f < kNumPerfFx; f++)
        if(eng->PerfFxActive(f))
            fx = f;
    if(fx >= 0)
    {
        FillRect(0, 55, TextWidth(kFontSmall, kFxShort[fx]) + 2, 9);
        Text(1, 55, kFontSmall, kFxShort[fx], false);
    }
    else
    {
        snprintf(buf, sizeof(buf), "cpu %d%%", (int)(plat->CpuAvg() * 100.f + 0.5f));
        Text(0, 55, kFontSmall, buf);
    }
    const char* st = !plat->ControllerUp() ? "no APC" : (sd_ok ? "SD" : "no SD");
    Text(128 - TextWidth(kFontSmall, st), 55, kFontSmall, st);

    if(memcmp(fb, fb_prev, sizeof(fb)))
    {
        memcpy(fb_prev, fb, sizeof(fb));
        fb_version++;
    }
}

/* ================================================================== */
/* State report for the Mac UI (ui/apcbox-ui.html)
 *
 * Plain text lines, sent ~30 times a second when something changed:
 *   S  transport, selection, masks, trigger counts, CPU (every tick)
 *   G  steps of the current pattern, all tracks (128 digits)
 *   T  per-track knob values 0-127        N  per-track sample name
 *   M  global values 0-127                B  sample browser position
 *   K  kit page                           W  LED wanted / confirmed
 *   E  raw MIDI from the APC40            L  log text
 *   Q  303 track notes, current pattern   A  303 track knobs 0-127
 *   V  report version (once a second)    SM loop memory: total, free, longest free piece
 *  S fields 52-55: copy source (-1 none), open set (-1 none), unsaved, set busy
 */


/* Bump when the lines change, so an old monitor page says it's stale */
constexpr int kReportVersion = 18; // 15: KP has the number of built-in kits, LP undo/redo 101/102 = a kit load; 16: MB, MC (the button map); 17: VF (the firmware's version); 18: SM (loop memory)
constexpr int kReportVersionOld = 11; // 10: H (meters), O (patterns with steps), Z (waveform outline); 11: J, JT (sets), KL, KP (kits)
uint8_t  rep_grid[kTracks * kSteps + 1];
int      rep_grid_pat = -1;
constexpr int kWavePoints = 96;
float    rep_peak[kTracks + 2] = {};
char     rep_meter[2 * (kTracks + 2) + 1] = "";
uint64_t rep_patmask = 0;
bool     rep_patmask_force = true;
uint32_t rep_wave[kTracks] = {};
char     rep_sets[8][64];
bool     ext_follow  = false; // following the T95's clock (ESC T)
bool     ext_playing = false;
uint32_t ext_last    = 0;
uint32_t ext_frames  = 0;
char     rep_ck[40] = "";
uint32_t rep_ck_at = 0;
bool     rep_cl_play = false;
int      rep_cl_step = -1;
uint32_t rep_cl_n = 0;
int      rep_trans = -1;
int      rep_kitpg[3] = {-9, -9, -9};
bool     rep_kits_force = true;
void     KitListChanged()
{
    rep_kits_force = true;
    rep_kitpg[0]   = -9;
}
uint8_t  rep_track[kTracks][13];
uint8_t  rep_global[10];
char     rep_notes[kTracks][2 * kSteps + 1];
int      rep_notes_pat[kTracks];
uint8_t  rep_acid[kTracks][8];
char     rep_name[kTracks][kPathLen];
char     rep_lp[48] = "";
char     rep_sm[72] = "";
void     ForgetReportedName(int t) { rep_name[t][0] = 0; }
int      rep_browse[4] = {-2, -2, -2, -2};
int      rep_kit[4]    = {-2, -2, -2, -2};
char     rep_leds[2][81];
uint32_t rep_full_time = 0;
uint32_t rep_fb_version = 0, rep_fb_time = 0;

void ForceFullReport()
{
    rep_fb_version = fb_version - 1;
    rep_grid_pat = -1;
    memset(rep_track, 0xFF, sizeof(rep_track));
    memset(rep_global, 0xFF, sizeof(rep_global));
    memset(rep_acid, 0xFF, sizeof(rep_acid));
    for(int t = 0; t < kTracks; t++)
    {
        rep_notes[t][0]  = 0;
        rep_notes_pat[t] = -1;
    }
    for(int t = 0; t < kTracks; t++)
        rep_name[t][0] = 1; // never a real path
    for(int i = 0; i < 4; i++)
        rep_browse[i] = rep_kit[i] = -2;
    rep_leds[0][0] = rep_leds[1][0] = 0;
    rep_meter[0]      = 0;
    rep_patmask_force = true;
    for(int i = 0; i < 8; i++)
        rep_sets[i][0] = 0;
    rep_trans    = -1;
    rep_ck[0]    = 0;
    rep_lp[0]    = 0;
    rep_sm[0]    = 0;
    map_report   = true;
    rep_kitpg[0] = -9; // KP again; the kit names only after ESC R or when the list changes
}

const char* BaseName(const char* path)
{
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

char LedChar(uint8_t v)
{
    return v == 0xFF ? '?' : (char)('0' + (v > 9 ? 9 : v));
}

char dline[2 + 2048 + 1];

void Report(uint32_t now)
{
    /* OLED image for the monitor page, at most 5 times a second */
    if(fb_version != rep_fb_version && now - rep_fb_time >= 200)
    {
        rep_fb_version = fb_version;
        rep_fb_time    = now;
        static const char hex[] = "0123456789abcdef";
        dline[0]                = 'D';
        dline[1]                = ' ';
        for(int i = 0; i < 1024; i++)
        {
            dline[2 + 2 * i]     = hex[fb[i] >> 4];
            dline[2 + 2 * i + 1] = hex[fb[i] & 15];
        }
        dline[2 + 2048] = 0;
        plat->Out(dline);
    }

    /* Everything again once a second, in case a line got dropped */
    if(now - rep_full_time > 1000)
    {
        rep_full_time = now;
        ForceFullReport();
        Out("V %d", kReportVersion); // the page checks it matches
        Out("VF %s", kFirmware);     // which firmware this is, for whoever asks later than the start line
    }

    const int pat = eng->CurrentPattern();
    uint32_t  fx_on = 0, fx_lat = 0, roll = 0, mute = 0, solo = 0, loading = 0,
             nonempty = 0;
    for(int f = 0; f < kNumPerfFx; f++)
    {
        fx_on |= eng->PerfFxActive(f) << f;
        fx_lat |= fx_latched[f] << f;
    }
    for(int t = 0; t < kTracks; t++)
    {
        roll |= (roll_held[t] || ui_roll[t]) << t;
        mute |= eng->track[t].mute << t;
        solo |= eng->track[t].solo << t;
        loading |= TrackLoading(t) << t;
    }
    uint32_t synthmask = 0;
    for(int t = 0; t < kTracks; t++)
        synthmask |= (eng->IsSynth(t) ? 1u : 0u) << t;
    uint32_t bankmask = 0;
    for(int p = 0; p < kPatterns; p++)
    {
        if(eng->PatternEmpty(p))
            continue;
        bankmask |= 1u << (p / 8);
        if(p / 8 == view_bank)
            nonempty |= 1u << (p % 8);
    }

    Out("S %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %lu %lu %lu %lu "
                "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %lu %d %lu "
                "%d %lu %lu %lu %lu %lu %lu %lu %lu %d %d %d %d %d %d %d %d %d",
                eng->Playing() ? 1 : 0,
                eng->CurrentStep(),
                pat,
                eng->QueuedPattern(),
                recording ? 1 : 0,
                (int)(eng->global.bpm * 10.f + 0.5f),
                eng->global.metronome ? 1 : 0,
                sel_track,
                page,
                shift ? 1 : 0,
                copy_held ? 1 : 0,
                (int)(plat->CpuAvg() * 1000.f),
                (int)(plat->CpuMax() * 1000.f),
                sd_ok ? 1 : 0,
                plat->ControllerUp() ? 1 : 0,
                plat->ControllerPresent() ? 1 : 0,
                (unsigned long)fx_on,
                (unsigned long)fx_lat,
                (unsigned long)roll,
                (unsigned long)mute,
                (unsigned long)solo,
                (unsigned long)loading,
                (unsigned long)nonempty,
                (unsigned long)eng->limit_count,
                (unsigned long)eng->nan_count,
                (unsigned long)plat->Dropped(),
                (unsigned long)eng->TriggerCount(0),
                (unsigned long)eng->TriggerCount(1),
                (unsigned long)eng->TriggerCount(2),
                (unsigned long)eng->TriggerCount(3),
                (unsigned long)eng->TriggerCount(4),
                (unsigned long)eng->TriggerCount(5),
                (unsigned long)eng->TriggerCount(6),
                (unsigned long)eng->TriggerCount(7),
                view_bank,
                (unsigned long)bankmask,
                reset_hold_start ? 1 : 0,
                (unsigned long)synthmask,
                track_page,
                (unsigned long)eng->TriggerCount(8),
                (unsigned long)eng->TriggerCount(9),
                (unsigned long)eng->TriggerCount(10),
                (unsigned long)eng->TriggerCount(11),
                (unsigned long)eng->TriggerCount(12),
                (unsigned long)eng->TriggerCount(13),
                (unsigned long)eng->TriggerCount(14),
                (unsigned long)eng->TriggerCount(15),
                eng->LiveRecState(), eng->LiveRecTrack(), eng->LiveRecBars(),
                (int)(eng->InputPeak() * 100.f + 0.5f), eng->input_monitor ? 1 : 0,
                copy_held ? copy_src : -1, open_set, set_dirty ? 1 : 0, set_op);

    /* Steps of the current pattern */
    char grid[kTracks * kSteps + 1];
    for(int t = 0; t < kTracks; t++)
        for(int s = 0; s < kSteps; s++)
            grid[t * kSteps + s] = (char)('0' + eng->GetStep(pat, t, s));
    grid[kTracks * kSteps] = 0;
    if(pat != rep_grid_pat || memcmp(grid, rep_grid, sizeof(grid)))
    {
        rep_grid_pat = pat;
        memcpy(rep_grid, grid, sizeof(grid));
        Out("G %d %s", pat, grid);
    }

    /* 303 tracks: notes (hex, slide = bit 7) and knobs */
    for(int t = 0; t < kTracks; t++)
    {
        if(!eng->IsSynth(t))
        {
            rep_notes[t][0] = 0;
            continue;
        }
        char q[2 * kSteps + 1];
        static const char hx[] = "0123456789abcdef";
        for(int st = 0; st < kSteps; st++)
        {
            uint8_t b = (uint8_t)(eng->GetNote(pat, t, st)
                                  | (eng->GetSlide(pat, t, st) ? kNoteSlide : 0));
            q[2 * st]     = hx[b >> 4];
            q[2 * st + 1] = hx[b & 15];
        }
        q[2 * kSteps] = 0;
        if(strcmp(q, rep_notes[t]) || pat != rep_notes_pat[t])
        {
            strcpy(rep_notes[t], q);
            rep_notes_pat[t] = pat;
            Out("Q %d %d %s", pat, t, q);
        }
        const AcidParams& a    = eng->track[t].acid;
        uint8_t           av[8] = {Ring(a.cutoff), Ring(a.reso),  Ring(a.envmod),
                                   Ring(a.decay),  Ring(a.accent), Ring(a.tune),
                                   Ring(a.slide),  Ring(a.wave)};
        if(memcmp(av, rep_acid[t], 8))
        {
            memcpy(rep_acid[t], av, 8);
            Out("A %d %d %d %d %d %d %d %d %d", t, av[0], av[1], av[2], av[3], av[4],
                av[5], av[6], av[7]);
        }
    }

    /* Per-track values and names */
    for(int t = 0; t < kTracks; t++)
    {
        const TrackParams& p    = eng->track[t];
        uint8_t            v[13] = {Ring(p.vol),   Ring(p.pan),   Ring(p.rev),
                                    Ring(p.dly),   Ring(p.pitch), Ring(p.decay),
                                    Ring(p.filter), Ring(p.reso), Ring(p.start),
                                    Ring(p.reverse), Ring(p.drive), Ring(p.prob), p.xfade};
        if(memcmp(v, rep_track[t], 13))
        {
            memcpy(rep_track[t], v, 13);
            Out("T %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                        t, v[0], v[1], v[2], v[3], v[4], v[5],
                        v[6], v[7], v[8], v[9], v[10], v[11], v[12]);
        }
        if(strcmp(slot_path[t], rep_name[t]))
        {
            strcpy(rep_name[t], slot_path[t]);
            if(IsSdSample(slot_path[t]))
                Out("N %d %s", t, BaseName(slot_path[t]));
            else if(IsSynthPath(slot_path[t]))
                Out("N %d [303]", t);
            else if(IsRecPath(slot_path[t]))
            {
                if(eng->IsLoop(t))
                    Out("N %d [loop %d bars]", t, eng->LoopBars(t));
                else
                    Out("N %d [recorded]", t);
            }
            else
            {
                int k = BuiltInKitOf(slot_path[t]);
                Out("N %d [%s %s]", t, Engine::BuiltInKitName(k),
                    Engine::BuiltInSoundName(k, t));
            }
        }
    }

    /* Globals */
    const GlobalParams& g    = eng->global;
    uint8_t             gv[10] = {Ring(g.master),   Ring(g.xfader),
                                  Ring(g.rev_size), Ring(g.rev_tone),
                                  Ring(g.dly_time), Ring(g.dly_fb),
                                  Ring(g.drive),    Ring(g.fx_return),
                                  Ring(g.swing),    Ring(g.djfilter)};
    if(memcmp(gv, rep_global, 10))
    {
        memcpy(rep_global, gv, 10);
        Out("M %d %d %d %d %d %d %d %d %d %d", gv[0], gv[1], gv[2], gv[3], gv[4],
            gv[5], gv[6], gv[7], gv[8], gv[9]);
    }

    /* Sample browser */
    int bnow[4] = {cur_folder, num_folders, cur_file, num_files};
    if(memcmp(bnow, rep_browse, sizeof(bnow)))
    {
        memcpy(rep_browse, bnow, sizeof(bnow));
        Out("B %d %d %d %d", cur_folder, num_folders, cur_file, num_files);
        if(cur_folder >= 0 && cur_folder < num_folders)
            Out("BF %s", folders[cur_folder]);
        if(cur_file >= 0 && cur_file < num_files)
            Out("BN %s", files[cur_file]);
        else
            Out("BN -");
    }

    /* Kits on the current page */
    int know[4] = {kit_page, num_kits, loaded_kit_pg[track_page],
                   loading_page == track_page ? loading_kit : -1};
    if(memcmp(know, rep_kit, sizeof(know)))
    {
        memcpy(rep_kit, know, sizeof(know));
        Out("K %d %d %d %d", know[0], know[1], know[2], know[3]);
        for(int i = 0; i < 5; i++)
        {
            int k = kit_page * 5 + i;
            if(k < num_kits)
                Out("KN %d %d %s", i, k, kit_names[k]);
            else
                Out("KN %d %d -", i, k);
        }
    }

    /* Level meters: 16 tracks then master left and right, peak since the
       last line as two hex digits (0-ff, full scale = ff). Held a little so
       a short hit is seen. Quiet = one line of zeros, then nothing. */
    {
        static const char hx[] = "0123456789abcdef";
        char              h[2 * (kTracks + 2) + 1];
        for(int i = 0; i < kTracks + 2; i++)
        {
            float p = eng->TakePeak(i);
            if(p > 1.f)
                p = 1.f;
            rep_peak[i] = p > rep_peak[i] ? p : rep_peak[i] * 0.8f;
            int v       = (int)(rep_peak[i] * 255.f + 0.5f);
            h[2 * i]     = hx[v >> 4];
            h[2 * i + 1] = hx[v & 15];
        }
        h[2 * (kTracks + 2)] = 0;
        if(strcmp(h, rep_meter))
        {
            strcpy(rep_meter, h);
            Out("H %s", h);
        }
    }

    /* Which of the 64 patterns have steps: 16 hex digits, pattern 1 = bit 0
       of the last digit */
    {
        uint64_t m = 0;
        for(int p = 0; p < kPatterns; p++)
            if(!eng->PatternEmpty(p))
                m |= (uint64_t)1 << p;
        if(m != rep_patmask || rep_patmask_force)
        {
            rep_patmask       = m;
            rep_patmask_force = false;
            Out("O %08lx%08lx", (unsigned long)(m >> 32), (unsigned long)(m & 0xFFFFFFFFu));
        }
    }

    /* Waveform outline of one track per pass, when its sound changed:
       96 points, the peak of each slice as two hex digits. 303 tracks and
       empty slots send length 0. Not part of the once-a-second repeat. */
    {
        static int wt = 0;
        wt            = (wt + 1) % kTracks;
        const int t   = wt;
        if(!TrackLoading(t) && !(eng->LiveRecState() == 2 && eng->LiveRecTrack() == t))
        {
            Slot&    s   = eng->GetSlot(t);
            uint32_t len = eng->IsSynth(t) ? 0 : (uint32_t)s.length;
            uint32_t key = slot_gen[t] * 2654435761u + len * 40503u + (eng->IsSynth(t) ? 1u : 0u) + 1u;
            if(key != rep_wave[t])
            {
                rep_wave[t] = key;
                static const char hx[] = "0123456789abcdef";
                char              z[2 * kWavePoints + 1];
                int               pk[kWavePoints];
                int               top = 1;
                for(int i = 0; i < kWavePoints; i++)
                {
                    int peak = 0;
                    if(len && s.data)
                    {
                        uint32_t a0 = (uint32_t)((uint64_t)len * i / kWavePoints);
                        uint32_t a1 = (uint32_t)((uint64_t)len * (i + 1) / kWavePoints);
                        uint32_t st = (a1 - a0) / 256 + 1; // at most 256 reads per point
                        for(uint32_t k = a0; k < a1; k += st)
                        {
                            int v = s.data[k];
                            if(v < 0)
                                v = -v;
                            if(v > peak)
                                peak = v;
                        }
                    }
                    pk[i] = peak;
                    if(peak > top)
                        top = peak;
                }
                for(int i = 0; i < kWavePoints; i++)
                {
                    int v        = pk[i] * 255 / top; // its own loudest point = ff: the shape, not the level
                    z[2 * i]     = hx[(v >> 4) & 15];
                    z[2 * i + 1] = hx[v & 15];
                }
                z[len ? 2 * kWavePoints : 0] = 0;
                /* length in samples at the engine's rate, so the page can show seconds */
                Out("Z %d %lu %d %s", t, (unsigned long)(len ? (uint32_t)(len / (s.rate > 0.f ? s.rate : 1.f)) : 0),
                    (int)eng->SampleRate(), len ? z : "-");
            }
        }
    }

    /* Clock, both ways. CK: are we following the T95's clock, and how far
       off we were (0.1 ms, smoothed; + = behind). CL: for the T95 to follow
       us: each step as it plays, with how long ago it fired (us, counted in
       audio samples), so the report's own delay drops out. */
    if(ext_follow && (int32_t)(g_now - ext_last) > 2000)
    {
        ext_follow = false;
        eng->ExtRelease();
        Log("clock: the T95's clock stopped coming, on our own tempo");
    }
    {
        int  err = (int)(eng->ExtErrMs() * 10.f + (eng->ExtErrMs() >= 0 ? 0.5f : -0.5f));
        char ck[40];
        snprintf(ck, sizeof(ck), "CK %d %d", ext_follow ? 1 : 0, ext_follow ? err : 0);
        if(strcmp(ck, rep_ck) && (ext_follow != (rep_ck[3] == '1') || (int32_t)(g_now - rep_ck_at) > 250))
        {
            strcpy(rep_ck, ck);
            rep_ck_at = g_now;
            Out("%s", ck);
        }
    }
    MapReport();
    {
        /* looper and quantise settings: take length, count-in, quantise of
           takes / pattern switches / mutes, input level, what the running
           take is (0 loop, 1 overdub, 2 one-shot), the track whose last take
           can be undone (0 = none) */
        char lp[48];
        if(lundo.await && lundo.track >= 0 && !TrackLoading(lundo.track))
        {
            lundo.await = false; // the reload an undo asked for is in
            lundo.gen   = slot_gen[lundo.track];
        }
        snprintf(lp, sizeof(lp), "LP %d %d %d %d %d %d %d %d %d %d", eng->lr_len_bars, eng->lr_countin ? 1 : 0,
                 eng->quant_rec, eng->quant_pat, eng->quant_mute, in_gain_ui, eng->LiveRecMode(),
                 KitCan(false) ? 101 + kundo.pg : LooperCan(false) ? lundo.track + 1 : 0,
                 KitCan(true) ? 101 + kundo.pg : LooperCan(true) ? lundo.track + 1 : 0, eng->quant_fx);
        if(strcmp(lp, rep_lp))
        {
            strcpy(rep_lp, lp);
            Out("%s", lp);
        }
    }
    {
        /* loop memory (samples): all of it, free, the longest free piece, the
           number of claims, what a track's own slot holds, and what the loop
           take under way has room for (0 = none) */
        char     sm[72];
        uint32_t total, fr, big;
        int      n;
        SpareStat(&total, &fr, &big, &n);
        const bool take = eng->LiveRecState() != 0 && eng->LiveRecMode() == Engine::kRecLoop
                          && eng->LiveRecTrack() >= 0;
        snprintf(sm, sizeof(sm), "SM %lu %lu %lu %d %lu %lu", (unsigned long)total, (unsigned long)fr,
                 (unsigned long)big, n, (unsigned long)eng->SlotCapacity(),
                 (unsigned long)(take ? eng->GetSlot(eng->LiveRecTrack()).capacity : 0));
        if(strcmp(sm, rep_sm))
        {
            strcpy(rep_sm, sm);
            Out("%s", sm);
        }
    }
    {
        int  step    = eng->ClockStep();
        bool playing = eng->Playing();
        if(playing != rep_cl_play || (playing && step != rep_cl_step))
        {
            rep_cl_play = playing;
            rep_cl_step = step;
            rep_cl_n++;
            uint32_t age = (uint32_t)((uint64_t)eng->SinceStep() * 1000000u / (uint32_t)eng->SampleRate());
            Out("CL %d %d %d %lu %lu", playing ? 1 : 0, step, (int)(eng->global.bpm * 100.f + 0.5f), (unsigned long)age,
                (unsigned long)rep_cl_n);
        }
    }

    /* Sets on the card (one line per slot) and the transition; every kit by
       name, and which kit each track page has. For the T95 pages. */
    for(int i = 0; i < kSetSlots; i++)
    {
        char j[64];
        const SetInfo& in = set_list[i];
        if(set_exists[i])
            snprintf(j, sizeof(j), "J %d 1 %d %d %d %d %s", i, (int)(in.bpm * 10.f + 0.5f), in.samples, in.loops,
                     in.synths, in.name);
        else
            snprintf(j, sizeof(j), "J %d 0", i);
        if(strcmp(j, rep_sets[i]))
        {
            strcpy(rep_sets[i], j);
            Out("%s", j);
        }
    }
    if(set_trans != rep_trans)
    {
        rep_trans = set_trans;
        Out("JT %d %s", set_trans, kTransNames[set_trans]);
    }
    {
        int kp[3] = {loaded_kit_pg[0], loaded_kit_pg[1], num_kits};
        if(memcmp(kp, rep_kitpg, sizeof(kp)))
        {
            bool list = kp[2] != rep_kitpg[2];
            memcpy(rep_kitpg, kp, sizeof(kp));
            Out("KP %d %d %d %d", kp[0], kp[1], kp[2], Engine::NumBuiltInKits());
            if(list || rep_kits_force)
            {
                rep_kits_force = false;
                for(int k = 0; k < num_kits; k++)
                    Out("KL %d %s", k, kit_names[k]);
            }
        }
    }

    /* Grid + track button LEDs: what the firmware wants vs what the
       APC40 has confirmed. A mismatch that sticks = a lost message. */
    char want[81], sent[81];
    for(int i = 0; i < 40; i++)
    {
        const LedSlot& a = leds[led_grid[i / 8][i % 8]];
        const LedSlot& b = leds[led_trk[i / 8][i % 8]];
        want[i]      = LedChar(a.want);
        sent[i]      = LedChar(a.sent);
        want[40 + i] = LedChar(b.want);
        sent[40 + i] = LedChar(b.sent);
    }
    want[80] = sent[80] = 0;
    if(strcmp(want, rep_leds[0]) || strcmp(sent, rep_leds[1]))
    {
        strcpy(rep_leds[0], want);
        strcpy(rep_leds[1], sent);
        Out("W %s %s", want, sent);
    }
}

/* ================================================================== */
/* Boot defaults                                                        */

void DemoPattern()
{
    for(int s : {0, 4, 8, 12})
        eng->SetStep(0, 0, s, kStepAccent);
    eng->SetStep(0, 1, 4, kStepAccent);
    eng->SetStep(0, 1, 12, kStepAccent);
    for(int s = 0; s < 16; s += 2)
        if(s != 14)
            eng->SetStep(0, 2, s, (s % 4) ? kStepAccent : kStepOn);
    eng->SetStep(0, 3, 14, kStepOn);
    eng->SetStep(0, 4, 12, kStepOn);
    eng->track[2].vol = 0.6f;
    eng->track[3].vol = 0.6f;
    eng->track[4].rev = 0.5f;
}

/** Everything back to how it boots: demo beat, built-in kit 1,
 *  default knobs, pattern A1, transport stopped */
void FullReset(uint32_t now)
{
    eng->Stop();
    eng->Panic();
    memset(eng->PatternData(), 0, sizeof(uint8_t) * kPatterns * kTracks * kSteps);
    memset(eng->NoteData(), 0, sizeof(uint8_t) * kPatterns * kTracks * kSteps);
    ReleaseKeys();
    acid_oct = 0;
    for(int t = 0; t < kTracks; t++)
    {
        eng->track[t] = TrackParams();
        eng->SetRoll(t, false);
        roll_held[t] = false;
    }
    eng->global = GlobalParams();
    for(int f = 0; f < kNumPerfFx; f++)
    {
        fx_latched[f] = false;
        fx_pressed[f] = false;
        eng->SetPerfFx(f, false);
    }
    recording = false;
    eng->SetRecord(false);
    view_bank  = 0;
    ClearUndo();
    /* a fresh start isn't the open set any more: SAVE HERE mustn't overwrite it */
    open_set     = -1;
    open_name[0] = 0;
    for(int t = 0; t < kTracks; t++)
        set_file[t][0] = 0;
    page       = kPagePan;
    track_page = 0;
    sel_track  = 0;
    for(int c = 0; c < 8; c++)
        fader_live[c] = false; // the faders don't match the defaults
    eng->QueuePattern(0);
    DemoPattern();
    LoadKit(0, false, 0);
    LoadKit(1, false, 1); // tracks 9-16: 909
    flash_red_until = now + 600;
    MapDefault(); // and the buttons do what's printed on them again
    MapChanged();
    Log("full reset");
    Popup("RESET");
}

} // namespace

/* ================================================================== */
/* Public API                                                          */

void BoxInit(Platform* p, Engine* e)
{
    plat = p;
    eng  = e;
    for(int t = 0; t < kTracks; t++)
    {
        snprintf(slot_path[t], kPathLen, "@k%d", builtin_kit_pg[t / 8]);
        eng->RenderBuiltInSound(t, builtin_kit_pg[t / 8]); // 1-8 808, 9-16 909
    }
    SpareInit();
    BuiltInKitNames();
    BuildLedTable();
    MapLedMeta();
    MapDefault();
    DemoPattern();
}

void BoxStorageReady(bool load_project)
{
    sd_ok = true;
    ScanKits();
    ScanFolders();
    cur_folder = 0;
    ScanFiles(0);
    Log("storage ok: %d kits, %d sample folders", num_kits - Engine::NumBuiltInKits(),
        num_folders);
    ReadSetList();
    TouchLoadCal();
    MapLoad();
    LsetLoad();
    if(load_project && !LoadLastSet())
        LoadProject();
}

void BoxStorageGone()
{
    sd_ok = false;
    BuiltInKitNames();
    num_folders = 0;
    num_files   = 0;
    cur_folder  = 0;
    cur_file    = -1;
    const int nb = Engine::NumBuiltInKits();
    for(int pg = 0; pg < 2; pg++)
        if(loaded_kit_pg[pg] >= nb)
            loaded_kit_pg[pg] = -1;
    if(loading_kit >= nb)
        loading_kit = -1;
    if(kit_page * 5 >= num_kits)
        kit_page = 0;
    Log("storage gone: built-in kits only, loaded samples stay");
}

void BoxMidi(int type, int ch, int d0, int d1, uint32_t now)
{
    HandleEvent(type, ch, d0, d1, now);
}

void BoxControllerGone()
{
    ReleaseAllHeld();
}

/* MIDI keyboards (via the T95): notes play the selected track */
int8_t kb_track[128];
int8_t kb_note[128];
bool   kb_init = false;

void BoxKeys(int type, int ch, int d0, int d1, uint32_t now)
{
    g_now = now;
    (void)ch;
    if(!kb_init)
    {
        memset(kb_track, -1, sizeof(kb_track));
        kb_init = true;
    }
    if(d0 < 0 || d0 > 127)
        return;
    bool on = type == kMidiNoteOn && d1 > 0;
    if(type == 6) // pitch bend: +-2 semitones on the selected track
    {
        int v = ((d1 & 127) << 7 | (d0 & 127)) - 8192;
        if(kb_bend_track >= 0 && kb_bend_track != sel_track)
            eng->bend_st[kb_bend_track] = 0.f;
        kb_bend_track          = v ? sel_track : -1;
        eng->bend_st[sel_track] = v * (2.f / 8192.f);
        return;
    }
    if(type == 3) // knobs and wheels: the selected track
    {
        TrackParams& p = eng->track[sel_track];
        const float  v = Knob((uint8_t)(d1 & 127));
        const bool   s = eng->IsSynth(sel_track);
        switch(d0)
        {
            case 1: // mod wheel
            case 74: *(s ? &p.acid.cutoff : &p.filter) = v; break;
            case 71: *(s ? &p.acid.reso : &p.reso) = v; break;
            case 72: *(s ? &p.acid.decay : &p.decay) = v; break;
            case 73: p.start = v; break;
            case 7:
                p.vol = v;
                if(sel_track >= track_page * 8 && sel_track < track_page * 8 + 8)
                    fader_live[sel_track - track_page * 8] = false;
                break;
            case 10: p.pan = v; break;
            case 91: p.rev = v; break;
            case 93: p.dly = v; break;
            default: break;
        }
        return;
    }
    if(!on && type != kMidiNoteOn && type != kMidiNoteOff)
        return;
    if(ch == 9) // drum pads (channel 10): 36.. = tracks 1..16
    {
        if(on && d0 >= 36 && d0 < 36 + kTracks)
            eng->TriggerLive(d0 - 36);
        return;
    }
    if(kb_track[d0] >= 0) // release what this key started (also on a repeat)
    {
        eng->KeyNote(kb_track[d0], kb_note[d0], 64, false);
        kb_track[d0] = -1;
    }
    if(!on)
        return;
    int t = sel_track, n = d0;
    if(eng->IsSynth(t)) // fold into the 303's range
    {
        while(n < kAcidLowNote)
            n += 12;
        while(n > kAcidHighNote)
            n -= 12;
    }
    kb_track[d0] = (int8_t)t;
    kb_note[d0]  = (int8_t)n;
    eng->KeyNote(t, n, d1, true);
    char nn[12];
    NoteName(nn, sizeof(nn), n);
    Popup("%s  track %d", nn, t + 1);
}

void BoxKeysGone()
{
    if(kb_bend_track >= 0)
        eng->bend_st[kb_bend_track] = 0.f;
    kb_bend_track = -1;
    if(!kb_init)
        return;
    for(int k = 0; k < 128; k++)
        if(kb_track[k] >= 0)
        {
            eng->KeyNote(kb_track[k], kb_note[k], 64, false);
            kb_track[k] = -1;
        }
}

void BoxPoll(uint32_t now)
{
    g_now = now;
    {
        int t, bars, mode = 0;
        if(eng->TakeLiveRecDone(&t, &bars, &mode))
        {
            slot_gen[t]++;
            /* a take in spare memory that turned out to fit the slot moves there */
            bool lng = SpareOf(t) >= 0;
            if(lng && mode != Engine::kRecOverdub && eng->GetSlot(t).length <= eng->SlotCapacity())
            {
                const uint32_t len = eng->GetSlot(t).length;
                memcpy(eng->HomeData(t), eng->GetSlot(t).data, (size_t)len * sizeof(int16_t));
                eng->SlotHome(t, len);
                lng = false;
            }
            else if(lng && mode != Engine::kRecOverdub) // it stays: what it didn't use is free again
            {
                const int c = SpareOf(t);
                SpareTrim(c, eng->GetSlot(t).length);
                eng->SlotPoint(t, SpareBase(c), SpareCap(c));
            }
            if(lundo.track == t)
            {
                /* no undo for a take in spare memory, nor when what it replaced
                   was a recording too long to keep */
                lundo.valid = mode == Engine::kRecOverdub
                                  ? lundo.data // a long loop wasn't kept: its overdub stays
                                  : (!lng && (lundo.data || !(lundo.loop || IsRecPath(lundo.path))));
                lundo.redo  = false;
                lundo.gen   = slot_gen[t];
            }
            if(mode == Engine::kRecOverdub)
            {
                Popup(lundo.track == t && !lundo.valid ? "overdub %d done (no undo)" : "overdub %d done", t + 1);
                Log("overdubbed the loop on track %d", t + 1);
            }
            else
            {
                snprintf(slot_path[t], kPathLen, "%s", kRecPath);
                snprintf(slot_orig[t], kPathLen, "%s", kRecPath);
                ForgetReportedName(t);
                loaded_kit_pg[t / 8] = -1;
                if(mode == Engine::kRecShot)
                {
                    uint32_t ms = (uint32_t)((uint64_t)eng->GetSlot(t).length * 1000u / 48000u);
                    Popup("sample %d: %lu.%lu s", t + 1, (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
                    Log("recorded a %lu ms sample into track %d", (unsigned long)ms, t + 1);
                    if(!eng->Playing())
                        eng->Audition(t);
                }
                else
                {
                    Popup("loop %d: %d bar%s", t + 1, bars, bars == 1 ? "" : "s");
                    Log("recorded a %d bar loop into track %d%s", bars, t + 1,
                        lng ? " (long: in spare memory, no undo)" : "");
                }
            }
        }
    }
    SpareSweep();
    static uint32_t last_screen = 0;
    if(now - last_screen >= 50)
    {
        last_screen = now;
        ComposeScreen(now);
    }
    if(reset_hold_start && now - reset_hold_start >= 1000)
    {
        reset_hold_start = 0;
        FullReset(now);
    }
    LoaderStep(now);
    XferPoll(now);
    SetsPoll(now);
    if(set_op == 3)
        KitSaveStep();
    KitUndoPoll();
    BrowseStep(now);
    UpdateLeds(now);
    MapLeds();
    MapPoll(now);
    LsetPoll(now);
    static uint32_t last = 0;
    if(now - last >= 33)
    {
        last = now;
        Report(now);
    }
}

void BoxResync()
{
    ForceFullReport();
    memset(rep_wave, 0, sizeof(rep_wave)); // waveform outlines again too (16 lines over 16 passes)
    rep_kits_force = true;                 // and the kit names
}

const uint8_t* BoxScreen(uint32_t* version)
{
    *version = fb_version;
    return fb;
}

LedSlot* BoxLeds(int* n)
{
    *n = num_leds;
    return leds;
}

void BoxInvalidateLeds()
{
    InvalidateLeds();
}

void BoxLog(const char* fmt, ...)
{
    char    msg[260];
    va_list va;
    va_start(va, fmt);
    vsnprintf(msg, sizeof(msg), fmt, va);
    va_end(va);
    Out("L %s", msg); // Out drops it if BoxInit hasn't run yet
    LogKeep(msg);
}

void BoxTftInit(uint16_t* framebuffer)
{
    tfb = framebuffer;
}

uint32_t BoxTftCompose(uint32_t now)
{
    g_now = now;
    return TftCompose(now);
}

void BoxUiTurn(int d, uint32_t now)
{
    g_now = now;
    if(touch_cal)
        return;
    if(sets_open)
        SetsTurn(d);
    else
        TftTurn(d);
}

void BoxUiPush(uint32_t now)
{
    g_now = now;
    if(touch_cal)
        return;
    if(sets_open)
        SetsPush();
    else
        tft_view = (tft_view + 1) % 5;
}

void BoxTouchRaw(int rx, int ry, bool down, uint32_t now) { TouchRaw(rx, ry, down, now); }
void BoxUiTouch(int x, int y, uint32_t now) { UiTouch(x, y, now); }
void BoxTouchCalibrate(uint32_t now) { TouchCalibrate(now); }

void BoxUiBack(uint32_t now)
{
    g_now = now;
    if(touch_cal) // cancel calibration
    {
        touch_cal = 0;
        Popup(tcal_loaded ? "calibration cancelled" : "touch not calibrated");
        return;
    }
    if(sets_open)
        SetsBack();
    else
        tft_view = 0;
}

/* Things held from the T95's pages (performance FX): let go when the T95
   goes quiet, like a controller's held buttons */
bool ui_fx_held[kNumPerfFx] = {};

void BoxUiGone()
{
    for(int t = 0; t < kTracks; t++)
        if(ui_roll[t])
        {
            ui_roll[t] = false;
            if(!roll_held[t])
                eng->SetRoll(t, false);
        }
    for(int f = 0; f < kNumPerfFx; f++)
        if(ui_fx_held[f])
        {
            ui_fx_held[f] = false;
            if(!fx_latched[f] && !fx_pressed[f])
                eng->SetPerfFx(f, false);
        }
}

/** 0 = off, 1 = on, 2 = toggle */
static bool Flag(bool cur, int v) { return v == 2 ? !cur : v == 1; }

bool BoxUiCommand(const char* c, uint32_t now)
{
    g_now = now;
    int n = 0, a = 0, b = 0, d = 0, e = 0, len9 = 0;
    char w[12];
    if(sscanf(c, "turn %d", &n) == 1 && n >= -16 && n <= 16)
        BoxUiTurn(n, now);
    else if(strcmp(c, "push") == 0)
        BoxUiPush(now);
    else if(strcmp(c, "back") == 0)
        BoxUiBack(now);
    else if(strcmp(c, "play") == 0)
        eng->Play();
    else if(strcmp(c, "stop") == 0)
        eng->Stop();
    else if(sscanf(c, "track %d", &n) == 1 && n >= 1 && n <= kTracks)
    {
        SelectTrack(n - 1);
        Popup("track %d", n);
    }
    /* ---- control protocol 4: everything the T95 pages show. Tracks, steps,
       patterns and FX count from 1, values are 0-127 like the T/A/M lines ---- */
    else if(sscanf(c, "mute %d %d", &n, &a) == 2 && n >= 1 && n <= kTracks && a >= 0 && a <= 2)
        eng->track[n - 1].mute = Flag(eng->track[n - 1].mute, a);
    else if(sscanf(c, "solo %d %d", &n, &a) == 2 && n >= 1 && n <= kTracks && a >= 0 && a <= 2)
        eng->track[n - 1].solo = Flag(eng->track[n - 1].solo, a);
    else if(sscanf(c, "knob %d %d %d", &n, &a, &b) == 3 && n >= 1 && n <= kTracks && a >= 0 && a <= 12
            && b >= 0 && b <= 127)
    {
        TrackParams& p     = eng->track[n - 1];
        float*       f[12] = {&p.vol,    &p.pan,  &p.rev,   &p.dly,     &p.pitch, &p.decay,
                              &p.filter, &p.reso, &p.start, &p.reverse, &p.drive, &p.prob};
        if(a == 12)
        {
            if(b > 2)
                return false;
            p.xfade = (uint8_t)b;
        }
        else
        {
            *f[a] = Knob((uint8_t)b);
            if(a == 0 && n - 1 >= track_page * 8 && n - 1 < track_page * 8 + 8)
                fader_live[n - 1 - track_page * 8] = false; // the APC40 fader picks it up again
        }
    }
    else if(sscanf(c, "acid %d %d %d", &n, &a, &b) == 3 && n >= 1 && n <= kTracks && a >= 0 && a <= 7
            && b >= 0 && b <= 127)
    {
        AcidParams& p    = eng->track[n - 1].acid;
        float*      f[8] = {&p.cutoff, &p.reso, &p.envmod, &p.decay, &p.accent, &p.tune, &p.slide, &p.wave};
        *f[a]            = (a == 5 && (b == 63 || b == 64)) ? 0.5f : Knob((uint8_t)b);
    }
    else if(sscanf(c, "glob %d %d", &a, &b) == 2 && a >= 0 && a <= 9 && b >= 0 && b <= 127)
    {
        GlobalParams& g     = eng->global;
        float*        f[10] = {&g.master, &g.xfader, &g.rev_size,  &g.rev_tone, &g.dly_time,
                               &g.dly_fb, &g.drive,  &g.fx_return, &g.swing,    &g.djfilter};
        *f[a] = ((a == 1 || a == 9) && (b == 63 || b == 64)) ? 0.5f : Knob((uint8_t)b);
    }
    else if(sscanf(c, "tempo %d", &n) == 1 && n >= 600 && n <= 2000)
        SetBpm(n / 10.f);
    else if(sscanf(c, "step %d %d %d %d", &n, &a, &b, &d) == 4 && n >= 1 && n <= kPatterns && a >= 1
            && a <= kTracks && b >= 1 && b <= kSteps && d >= 0 && d <= 2)
        eng->SetStep(n - 1, a - 1, b - 1, (uint8_t)d);
    else if(sscanf(c, "note %d %d %d %d %d", &n, &a, &b, &d, &e) == 5 && n >= 1 && n <= kPatterns
            && a >= 1 && a <= kTracks && b >= 1 && b <= kSteps && d >= 1 && d <= 127 && e >= 0 && e <= 1)
    {
        eng->SetNote(n - 1, a - 1, b - 1, d);
        eng->SetSlide(n - 1, a - 1, b - 1, e == 1);
    }
    else if(sscanf(c, "pat %d", &n) == 1 && n >= 1 && n <= kPatterns)
    {
        eng->QueuePattern(n - 1);
        view_bank = (n - 1) / 8;
    }
    else if(sscanf(c, "pcopy %d %d", &n, &a) == 2 && n >= 1 && n <= kPatterns && a >= 1 && a <= kPatterns
            && n != a)
    {
        char x[3], y[3], lbl[28];
        PatName(n - 1, x);
        PatName(a - 1, y);
        snprintf(lbl, sizeof(lbl), "copy %s>%s", x, y);
        PushUndo(a - 1, lbl);
        eng->CopyPattern(n - 1, a - 1);
        Log("%s", lbl);
    }
    else if(sscanf(c, "pclear %d %d", &n, &a) == 2 && n >= 1 && n <= kPatterns && a >= 0 && a <= kTracks)
    {
        char x[3], lbl[28]; // a: 0 = the whole pattern, else one track
        PatName(n - 1, x);
        if(a)
            snprintf(lbl, sizeof(lbl), "clear %s trk %d", x, a);
        else
            snprintf(lbl, sizeof(lbl), "clear %s", x);
        PushUndo(n - 1, lbl);
        if(a)
            eng->ClearTrack(n - 1, a - 1);
        else
            eng->ClearPattern(n - 1);
        Log("%s", lbl);
    }
    else if(strcmp(c, "undo") == 0)
        Undo();
    else if(sscanf(c, "fx %d %d", &n, &a) == 2 && n >= 1 && n <= kNumPerfFx && a >= 0 && a <= 1)
    {
        ui_fx_held[n - 1] = a == 1;
        if(a)
            eng->SetPerfFx(n - 1, true);
        else if(!fx_latched[n - 1] && !fx_pressed[n - 1])
            eng->SetPerfFx(n - 1, false);
    }
    else if(sscanf(c, "rec %d", &a) == 1 && a >= 0 && a <= 2)
    {
        if(Flag(recording, a) != recording)
            ToggleRecord();
    }
    else if(sscanf(c, "metro %d", &a) == 1 && a >= 0 && a <= 2)
        eng->global.metronome = Flag(eng->global.metronome, a);
    else if(sscanf(c, "mon %d", &a) == 1 && a >= 0 && a <= 2)
        eng->input_monitor = Flag(eng->input_monitor, a);
    else if(sscanf(c, "type %d %11s", &n, w) == 2 && n >= 1 && n <= kTracks
            && (strcmp(w, "303") == 0 || strcmp(w, "sample") == 0))
    {
        int t = n - 1; // "sample" = the built-in sound for that track
        if(TrackLoading(t) || (eng->LiveRecState() != 0 && eng->LiveRecTrack() == t))
            return true;
        if(strcmp(w, "303") == 0)
        {
            if(!IsSynthPath(slot_path[t]))
            {
                if(t == sel_track)
                    ReleaseKeys();
                QueueLoad(t, kSynthPath, false);
            }
        }
        else if(IsSynthPath(slot_path[t]) || IsRecPath(slot_path[t]) || IsSdSample(slot_path[t]))
        {
            char p[8];
            snprintf(p, sizeof(p), "@k%d", BuiltInKitFor(t));
            QueueLoad(t, p, false);
        }
    }
    else if(sscanf(c, "lrec arm %d", &n) == 1 && n >= 1 && n <= kTracks)
        LooperStart(n - 1, Engine::kRecLoop);
    /* ---- control protocol 7: looper, one-shot sampling, quantise, roll ---- */
    else if(sscanf(c, "lrec over %d", &n) == 1 && n >= 1 && n <= kTracks)
        LooperStart(n - 1, Engine::kRecOverdub);
    else if(sscanf(c, "lrec shot %d", &n) == 1 && n >= 1 && n <= kTracks)
        LooperStart(n - 1, Engine::kRecShot);
    else if(strcmp(c, "lrec stop") == 0)
    {
        if(eng->LiveRecState() == 2)
            eng->LiveRecStop();
        else if(eng->LiveRecState() == 1)
            LooperCancel();
    }
    else if(strcmp(c, "lrec cancel") == 0)
        LooperCancel();
    else if(strcmp(c, "lrec undo") == 0)
        LooperUndoTake(false);
    else if(strcmp(c, "lrec redo") == 0) // control protocol 8
        LooperUndoTake(true);
    else if(sscanf(c, "lrec len %d", &n) == 1 && (n == 0 || n == 1 || n == 2 || n == 4 || n == 8))
        eng->lr_len_bars = (uint8_t)n;
    else if(sscanf(c, "lrec count %d", &a) == 1 && a >= 0 && a <= 2)
        eng->lr_countin = Flag(eng->lr_countin, a);
    else if(sscanf(c, "quant %11s %d", w, &a) == 2 && a >= 0 && a <= 2)
    {
        if(strcmp(w, "rec") == 0 && a <= 1)
            eng->quant_rec = (uint8_t)a;
        else if(strcmp(w, "pat") == 0)
            eng->quant_pat = (uint8_t)a;
        else if(strcmp(w, "mute") == 0)
            eng->quant_mute = (uint8_t)a;
        else if(strcmp(w, "fx") == 0) // control protocol 8
            eng->quant_fx = (uint8_t)a;
        else
            return false;
    }
    else if(sscanf(c, "ingain %d", &a) == 1 && a >= 0 && a <= 127)
        in_gain_ui = a, eng->in_gain = a == 64 ? 1.f : a * (2.f / 127.f) * a * (2.f / 127.f); // 64 = as it comes, 127 = x4
    else if(sscanf(c, "roll %d %d", &n, &a) == 2 && n >= 1 && n <= kTracks && a >= 0 && a <= 1)
    {
        ui_roll[n - 1] = a == 1;
        if(a || !roll_held[n - 1])
            eng->SetRoll(n - 1, a == 1);
    }
    else if(sscanf(c, "kit %d %d", &n, &a) == 2 && n >= 1 && n <= num_kits && a >= 0 && a <= 1)
        LoadKit(n - 1, true, a); // a: track page (0 = tracks 1-8, 1 = 9-16)
    /* ---- control protocol 9: kits made on the box. The page asks before
       "over" and "delete" ---- */
    else if((len9 = 0, sscanf(c, "kit save %d %n", &a, &len9) == 1) && len9 && (a == 0 || a == 1))
        KitSaveBegin(a, c + len9, false);
    else if((len9 = 0, sscanf(c, "kit over %d %n", &a, &len9) == 1) && len9 && (a == 0 || a == 1))
        KitSaveBegin(a, c + len9, true);
    else if((len9 = 0, sscanf(c, "kit rename %d %n", &n, &len9) == 1) && len9 && n >= 1 && n <= num_kits)
        KitRename(n - 1, c + len9);
    else if(sscanf(c, "kit delete %d", &n) == 1 && n >= 1 && n <= num_kits)
        KitDelete(n - 1);
    /* ---- control protocol 10: the APC40's button map (apcmap.h) ---- */
    else if(sscanf(c, "map b %d %d %d %d", &n, &a, &b, &d) == 4)
    {
        if(!MapSetButton(n, a, b, d))
            return false;
        MapChanged();
    }
    else if(sscanf(c, "map c %d %d", &n, &a) == 2)
    {
        if(!MapSetCc(n, a))
            return false;
        MapChanged();
    }
    else if(strcmp(c, "map reset") == 0)
    {
        MapDefault();
        MapChanged();
        Popup("buttons: standard layout");
    }
    else if(sscanf(c, "audition %d", &n) == 1 && n >= 1 && n <= kTracks)
        eng->Audition(n - 1);
    /* ---- control protocol 5: sets from the T95 pages. Slots count from 1.
       Names: what the Daisy's own screen allows; anything else becomes "_" ---- */
    else if(strncmp(c, "set ", 4) == 0)
    {
        const char* a2 = c + 4;
        char        name[kSetNameLen + 4] = "";
        int         slot = 0, len = 0;
        auto        take_name = [&](const char* s) {
            while(*s == ' ')
                s++;
            if(*s)
                SafeName(name, kSetNameLen + 1, s);
        };
        if(set_op && strncmp(a2, "trans ", 6) != 0)
        {
            Popup("busy");
            return true;
        }
        if(!sd_ok)
        {
            Popup("no SD card");
            return true;
        }
        if(sscanf(a2, "load %d", &slot) == 1 && slot >= 1 && slot <= kSetSlots)
        {
            if(set_exists[slot - 1])
                LoadSetBegin(slot - 1);
            else
                Popup("%02d is empty", slot);
        }
        else if(sscanf(a2, "save %d%n", &slot, &len) == 1 && slot >= 1 && slot <= kSetSlots)
        {
            take_name(a2 + len);
            if(!name[0])
            {
                if(set_exists[slot - 1])
                    snprintf(name, sizeof(name), "%s", set_list[slot - 1].name);
                else if(open_set >= 0 && open_name[0])
                    snprintf(name, sizeof(name), "%s", open_name);
                else
                    DefaultName(slot - 1, name, sizeof(name));
            }
            SaveSetBegin(slot - 1, name);
        }
        else if(strncmp(a2, "new", 3) == 0 && (a2[3] == 0 || a2[3] == ' '))
        {
            int s = FirstFreeSlot();
            if(s < 0)
            {
                Popup("no free slot");
                return true;
            }
            take_name(a2 + 3);
            if(!name[0])
                DefaultName(s, name, sizeof(name));
            SaveSetBegin(s, name);
        }
        else if(sscanf(a2, "delete %d", &slot) == 1 && slot >= 1 && slot <= kSetSlots)
        {
            if(set_exists[slot - 1])
                DeleteSet(slot - 1);
        }
        else if(sscanf(a2, "name %d%n", &slot, &len) == 1 && slot >= 1 && slot <= kSetSlots)
        {
            take_name(a2 + len);
            if(name[0] && set_exists[slot - 1])
                RenameSet(slot - 1, name);
        }
        else if(sscanf(a2, "trans %d", &slot) == 1 && slot >= 0 && slot < kNumTransitions)
        {
            set_trans = (uint8_t)slot;
            WriteLast();
        }
        else
            return false;
    }
    else
        return false;
    return true;
}

/* Following the T95's clock (ESC T). Frames come every sixteenth while the
   clock module sends to the Daisy; 2 s without one and we're on our own
   tempo again (and keep playing). */
void BoxClockFrame(const char* c, uint32_t now)
{
    g_now = now;
    int  bpm100 = 0, playing = 0, lat_us = 0;
    long ticks  = 0;
    if(sscanf(c, "%d %ld %d %d", &bpm100, &ticks, &playing, &lat_us) < 3 || ticks < 0)
        return;
    if(bpm100 < 6000 || bpm100 > 20000)
    {
        if(ext_follow)
            Log("clock: %d.%02d bpm is outside 60-200, not following", bpm100 / 100, bpm100 % 100);
        ext_follow = false;
        eng->ExtRelease();
        return;
    }
    if(lat_us < 0 || lat_us > 20000)
        lat_us = 0;
    if(!ext_follow)
    {
        Log("clock: following the T95");
        Popup("ext clock");
        ext_playing = eng->Playing();
    }
    ext_follow = true;
    ext_last   = now;
    ext_frames++;
    eng->global.bpm = bpm100 / 100.f;
    if(playing && !ext_playing)
        eng->Play();
    else if(!playing && ext_playing)
        eng->Stop();
    ext_playing = playing != 0;
    if(playing)
    {
        // where the outside clock is now: its position plus the time the frame took to get here
        float pos16 = (float)(ticks % 96) / 6.f + (lat_us * 1e-6f) * (bpm100 / 100.f) / 15.f;
        eng->ExtSync(pos16);
    }
}

void BoxFileFrame(const char* frame, uint32_t now) { XferFrame(frame, now); }

} // namespace ab
