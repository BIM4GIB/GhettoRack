/** Desktop test for pattern copy/undo and sets (save, load in sync, transitions).
 *  Builds the real box.cpp into this file with a folder as the "SD card". */
#include "box.cpp"
#include "ports.h"
#include <cassert>
#include <cmath>
#include <dirent.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace ab;

static std::string root;
static int         fails = 0;
#define CHECK(c)                                                    \
    do                                                              \
    {                                                               \
        if(!(c))                                                    \
        {                                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
            fails++;                                                \
        }                                                           \
    } while(0)

static std::vector<std::string> xfer_lines;
static std::vector<std::string> ui_lines; // H (meters), O (pattern mask), Z (waveform outline)
class TestPlat : public Platform
{
  public:
    void Out(const char* l) override
    {
        if(l[0] == 'F' && strchr("OACL", l[1])) // sample transfer replies
            xfer_lines.push_back(l);
        if((strchr("HOZJ", l[0]) && l[1] == ' ') || !strncmp(l, "JT ", 3) || !strncmp(l, "KL ", 3) || !strncmp(l, "KP ", 3) || !strncmp(l, "CK ", 3) || !strncmp(l, "CL ", 3) || !strncmp(l, "MB ", 3) || !strncmp(l, "MC ", 3))
            ui_lines.push_back(l);
    }
    bool FileOpen(const char* p) override { return (rf = fopen(Full(p).c_str(), "rb")) != nullptr; }
    int32_t FileRead(void* d, uint32_t n) override { return (int32_t)fread(d, 1, n, rf); }
    bool FileSkip(uint32_t n) override { return fseek(rf, n, SEEK_CUR) == 0; }
    uint32_t FileTell() override { return (uint32_t)ftell(rf); }
    void FileClose() override
    {
        if(rf)
            fclose(rf);
        rf = nullptr;
    }
    uint8_t* Scratch(uint32_t* s) override
    {
        *s = sizeof(scratch);
        return scratch;
    }
    bool DirOpen(const char* p) override
    {
        dpath = Full(p);
        return (d = opendir(dpath.c_str())) != nullptr;
    }
    bool DirNext(char* name, size_t cap, bool* is_dir) override
    {
        dirent* e;
        do
        {
            e = d ? readdir(d) : nullptr;
        } while(e && e->d_name[0] == '.');
        if(!e)
            return false;
        snprintf(name, cap, "%s", e->d_name);
        struct stat sb;
        *is_dir = stat((dpath + "/" + e->d_name).c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
        return true;
    }
    void DirClose() override
    {
        if(d)
            closedir(d);
        d = nullptr;
    }
    bool WriteFile(const char* dd, const char* p, const void* data, uint32_t n) override
    {
        mkdir(Full(dd).c_str(), 0755);
        FILE* f = fopen(Full(p).c_str(), "wb");
        if(!f)
            return false;
        bool ok = fwrite(data, 1, n, f) == n;
        fclose(f);
        return ok;
    }
    int32_t ReadFile(const char* p, void* data, uint32_t cap) override
    {
        FILE* f = fopen(Full(p).c_str(), "rb");
        if(!f)
            return -1;
        int32_t r = (int32_t)fread(data, 1, cap, f);
        fclose(f);
        return r;
    }
    bool FileCreate(const char* dd, const char* p) override
    {
        std::string acc;
        for(const char* q = dd; *q; q++)
        {
            acc += *q;
            if(q[1] == '/' || q[1] == 0)
                mkdir(Full(acc.c_str()).c_str(), 0755);
        }
        wf = fopen(Full(p).c_str(), "wb");
        return wf != nullptr;
    }
    int32_t FileWrite(const void* s, uint32_t n) override { return wf ? (int32_t)fwrite(s, 1, n, wf) : -1; }
    bool    FileWriteClose() override
    {
        bool ok = wf && fclose(wf) == 0;
        wf      = nullptr;
        return ok;
    }
    bool Remove(const char* p) override { return ::remove(Full(p).c_str()) == 0; }
    bool Rename(const char* a, const char* b) override { return ::rename(Full(a).c_str(), Full(b).c_str()) == 0; }
    int16_t* UndoMem(uint32_t* n) override
    {
        static std::vector<int16_t> u(768 * 1024);
        *n = (uint32_t)u.size();
        return u.data();
    }
    int16_t* SpareMem(uint32_t* n) override // like the Daisy's: 8 blocks of two slots
    {
        static std::vector<int16_t> sp(8 * 1536 * 1024);
        *n = (uint32_t)sp.size();
        return sp.data();
    }
    int16_t* Staging(uint32_t* n) override
    {
        static std::vector<int16_t> m(stage_samples);
        *n = (uint32_t)m.size();
        return m.data();
    }
    float    CpuAvg() override { return 0; }
    float    CpuMax() override { return 0; }
    bool     ControllerUp() override { return true; }
    bool     ControllerPresent() override { return true; }
    uint32_t Dropped() override { return 0; }
    bool     AudioRunning() override { return true; }

    uint32_t stage_samples = 4600 * 1024;

  private:
    std::string Full(const char* p) { return root + "/" + p; }
    FILE*       rf = nullptr;
    FILE*       wf = nullptr;
    DIR*        d  = nullptr;
    std::string dpath;
    uint8_t     scratch[16384];
};

static TestPlat plat_;
static Engine   e;
static uint32_t now_ms = 0;
static float    outl[48], outr[48];
static float    peak_last = 0.f;
static float    test_in_amp = 0.f; // a 220 Hz tone on the audio input

/* 1 ms of everything: audio block, main loop poll */
static void Tick(int n = 1)
{
    for(int i = 0; i < n; i++)
    {
        static float in[48] = {};
        static uint32_t ph = 0;
        for(int k = 0; k < 48; k++, ph++)
            in[k] = test_in_amp * sinf(6.2831853f * 220.f * (float)(ph % 48000) / 48000.f);
        e.SetInput(in, in);
        e.Process(outl, outr, 48);
        for(int k = 0; k < 48; k++)
            peak_last = fmaxf(peak_last, fabsf(outl[k]));
        now_ms++;
        BoxPoll(now_ms);
    }
}

static void Btn(int note, int ch, bool on)
{
    BoxMidi(on ? 1 : 0, ch, note, on ? 127 : 0, now_ms);
    Tick(5);
}
static void Tap(int note, int ch = 0)
{
    Btn(note, ch, true);
    Btn(note, ch, false);
}

static void WriteSine(const char* rel, int frames, float hz)
{
    std::string p = root + "/" + rel;
    FILE*       f = fopen(p.c_str(), "wb");
    uint32_t    data = frames * 2;
    auto        w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto        w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    w32(36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(1);
    w32(48000);
    w32(96000);
    w16(2);
    w16(16);
    fwrite("data", 1, 4, f);
    w32(data);
    for(int i = 0; i < frames; i++)
    {
        int16_t s = (int16_t)(12000 * sinf(6.2831853f * hz * i / 48000.f));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static bool Exists(const std::string& rel)
{
    struct stat sb;
    return stat((root + "/" + rel).c_str(), &sb) == 0;
}

static int WaitIdle(int max_ms = 20000)
{
    int n = 0;
    while((set_op || load_phase || num_jobs || load_state != kLoadIdle) && n < max_ms)
    {
        Tick();
        n++;
    }
    return n;
}

/* ---- T95 controllers (ports.h) ---- */
static std::vector<std::string> t95_lines;
static bool                     t95_room = true;
static bool T95Send(const char* l)
{
    if(!t95_room)
        return false;
    t95_lines.push_back(l);
    return true;
}
static T95Ports t95p;
static HostCmd  t95cmd;
static void     T95Frame(const std::string& f)
{
    for(unsigned char b : f)
    {
        HostCmd::Cmd c = t95cmd.Feed(b);
        if(c > HostCmd::kBoot)
            t95p.OnCmd(t95cmd, c, now_ms);
    }
}
static void T95Tick(int n, bool direct = false, bool keepalive = true)
{
    for(int i = 0; i < n; i++)
    {
        Tick();
        if(keepalive && now_ms % 500 == 0)
            T95Frame("\x1bK");
        t95p.Poll(now_ms, direct);
    }
}
static int T95Count(const char* prefix)
{
    int n = 0;
    for(auto& l : t95_lines)
        n += l.rfind(prefix, 0) == 0;
    return n;
}

int main()
{
    char tmpl[] = "/tmp/apcsetsXXXXXX";
    root        = mkdtemp(tmpl);
    mkdir((root + "/samples").c_str(), 0755);
    mkdir((root + "/samples/test").c_str(), 0755);
    WriteSine("samples/test/tone a.wav", 24000, 440.f);
    WriteSine("samples/test/tone b.wav", 12000, 660.f);
    WriteSine("samples/test/long.wav", 600000, 220.f); // 12.5 s

    static std::vector<int16_t> slots(768 * 1024 * (size_t)kTracks);
    static std::vector<float>   dl(96000), dr(96000), cl(192000), cr(192000);
    static daisysp::ReverbSc    rv;
    Memory                      mem;
    for(int t = 0; t < kTracks; t++)
        mem.slot_mem[t] = slots.data() + (size_t)t * 768 * 1024;
    mem.slot_capacity = 768 * 1024;
    mem.delay_l = dl.data(), mem.delay_r = dr.data(), mem.delay_len = 96000;
    mem.capture_l = cl.data(), mem.capture_r = cr.data(), mem.capture_len = 192000;
#ifdef USE_DAISYSP_LGPL
    mem.reverb = &rv;
#endif
    e.Init(48000.f, mem);
    BoxInit(&plat_, &e);
    BoxStorageReady(true);
    Tick(50);

    const int row3 = apc::kClipFirst + 2, row4 = apc::kClipFirst + 3;

    /* ---------- copy / undo ---------- */
    e.ClearPattern(3);
    e.ClearPattern(4);
    CHECK(!e.PatternEmpty(0));
    Btn(apc::kClipTrack, 0, true);
    Tap(row3, 0); // source A1
    CHECK(copy_src == 0);
    Tap(row3, 3); // -> A4
    CHECK(!memcmp(e.PatternData()[3], e.PatternData()[0], kTracks * kSteps));
    Tap(row3, 4); // -> A5 (source stays)
    CHECK(!e.PatternEmpty(4));
    Btn(apc::kClipTrack, 0, false);
    CHECK(copy_src == -1);
    /* undo twice: A5 then A4 empty again */
    Btn(apc::kShift, 0, true);
    Tap(apc::kClipTrack);
    CHECK(e.PatternEmpty(4));
    Tap(apc::kClipTrack);
    CHECK(e.PatternEmpty(3));
    Btn(apc::kShift, 0, false);

    /* track only: select track 2, SHIFT on the destination */
    SelectTrack(1);
    e.SetStep(5, 0, 7, kStepOn); // destination already has something on track 1
    Btn(apc::kClipTrack, 0, true);
    Tap(row3, 0);
    Btn(apc::kShift, 0, true);
    Tap(row3, 5);
    Btn(apc::kShift, 0, false);
    Btn(apc::kClipTrack, 0, false);
    CHECK(!memcmp(e.PatternData()[5][1], e.PatternData()[0][1], kSteps));
    CHECK(e.GetStep(5, 0, 7) == kStepOn); // other tracks kept
    CHECK(e.GetStep(5, 0, 0) == kStepOff);

    /* double tap clears; undo brings it back */
    Btn(apc::kClipTrack, 0, true);
    Btn(row3, 5, true);
    Btn(row3, 5, false);
    Btn(row3, 5, true);
    Btn(row3, 5, false);
    Btn(apc::kClipTrack, 0, false);
    CHECK(e.PatternEmpty(5));
    Btn(apc::kShift, 0, true);
    Tap(apc::kClipTrack);
    Btn(apc::kShift, 0, false);
    CHECK(e.GetStep(5, 0, 7) == kStepOn);

    /* bank pick with TRACK SELECT while CLIP/TRACK is held */
    Btn(apc::kClipTrack, 0, true);
    Tap(apc::kTrackSel, 2);
    CHECK(view_bank == 2);
    Tap(row3, 0); // source C1 (empty)
    Tap(apc::kTrackSel, 0);
    Tap(row3, 6); // -> A7
    Btn(apc::kClipTrack, 0, false);
    CHECK(e.PatternEmpty(6));
    CHECK(sel_track == 1); // TRACK SELECT didn't change the track
    printf("copy/undo ok\n");

    /* ---------- set 01: two SD samples, a 303, a "loop" ---------- */
    QueueLoad(2, "samples/test/tone a.wav", false);
    QueueLoad(3, "samples/test/tone b.wav", false);
    QueueLoad(5, kSynthPath, false);
    WaitIdle();
    CHECK(e.GetSlot(2).length == 24000);
    CHECK(e.IsSynth(5));
    /* fake a recorded 2-bar loop on track 7 */
    {
        Slot& s = e.GetSlot(6);
        for(uint32_t i = 0; i < 90000; i++)
            s.data[i] = (int16_t)(8000 * sinf(i * 0.01f));
        s.length = 90000;
        snprintf(slot_path[6], kPathLen, "%s", kRecPath);
        snprintf(slot_orig[6], kPathLen, "%s", kRecPath);
        e.SetLoop(6, 2, 120.f);
        Tick(2);
        CHECK(e.IsLoop(6));
    }
    e.SetStep(9, 2, 3, kStepAccent);
    e.global.bpm = 128.f;
    Tick(600);
    CHECK(set_dirty);

    SetsOpen(true);
    CHECK(sets_open);
    sets_cur = 0;
    DoAct(kActSaveHere);
    int ms = WaitIdle();
    printf("set 01 saved in %d ms of polls\n", ms);
    CHECK(set_exists[0]);
    CHECK(open_set == 0);
    CHECK(!set_dirty);
    CHECK(Exists("sets/01/set.bin"));
    CHECK(Exists("sets/01/03 tone a.wav"));
    CHECK(Exists("sets/01/04 tone b.wav"));
    CHECK(Exists("sets/01/07 loop.wav"));
    CHECK(!Exists("sets/01/06 loop.wav"));
    CHECK(set_list[0].samples == 2 && set_list[0].loops == 1 && set_list[0].synths == 1);
    CHECK(!strcmp(set_list[0].name, "128bpm"));

    /* save again, unchanged: no sound rewritten (mtime check via delete) */
    ::remove((root + "/sets/01/03 tone a.wav").c_str());
    DoAct(kActSaveHere);
    WaitIdle();
    CHECK(!Exists("sets/01/03 tone a.wav")); // kept its file name, didn't rewrite
    /* put it back for the load test */
    {
        sets_cur = 0;
        set_gen[2] = slot_gen[2] - 1; // pretend it changed
        DoAct(kActSaveHere);
        WaitIdle();
        CHECK(Exists("sets/01/03 tone a.wav"));
    }

    /* ---------- set 02: different ---------- */
    QueueLoad(2, "samples/test/tone b.wav", false); // track 3 changes
    QueueLoad(5, "@k0", false);                     // 303 -> 808
    QueueLoad(8, "samples/test/long.wav", false);   // a big one
    WaitIdle();
    e.ClearPattern(9);
    e.global.bpm = 100.f;
    e.track[0].vol = 0.3f;
    Tick(600);
    CHECK(set_dirty);
    DoAct(kActSaveNew);
    WaitIdle();
    CHECK(open_set == 1 && set_exists[1]);
    CHECK(Exists("sets/02/09 long.wav"));

    /* ---------- load 01 while playing, filter transition ---------- */
    e.Play();
    Tick(300);
    set_trans = kTransFilter;
    sets_cur  = 0;
    uint32_t gen3 = slot_gen[3]; // track 4 (tone b) is in both sets: shared
    DoAct(kActLoad);
    CHECK(set_op == 2);
    /* the old set keeps playing until the switch */
    int  waited = 0;
    bool saw_trans = false;
    while(!eng->TakeSwapDone() && waited < 20000 && load_phase != 3)
    {
        if(e.SwapState() == 2)
            saw_trans = true;
        if(load_phase == 3)
            break;
        Tick();
        waited++;
        if(load_phase == 2 && e.SwapState() == 0 && !set_op)
            break;
    }
    WaitIdle();
    printf("load 01: switched after %d ms, transition bar seen: %d\n", waited, saw_trans);
    CHECK(saw_trans);
    CHECK(open_set == 0);
    CHECK(fabsf(e.global.bpm - 128.f) < 0.01f);
    CHECK(e.GetStep(9, 2, 3) == kStepAccent);
    CHECK(e.IsSynth(5));
    CHECK(e.IsLoop(6) && e.LoopBars(6) == 2);
    CHECK(e.GetSlot(6).length == 90000);
    CHECK(e.GetSlot(2).length == 24000); // tone a back on track 3
    CHECK(e.GetSlot(2).data == e.HomeData(2)); // copied back home
    CHECK(slot_gen[3] == gen3);                // shared: not reloaded
    CHECK(!set_dirty);
    CHECK(undo_count == 0);
    CHECK(fabsf(e.global.djfilter - 0.5f) < 0.01f);

    /* ---------- load 02 with echo out, then a tape stop, then stopped ---------- */
    for(int tr : {kTransEcho, kTransTape, kTransCut})
    {
        set_trans = (uint8_t)tr;
        sets_cur  = (sets_cur == 0) ? 1 : 0;
        DoAct(kActLoad);
        int n = WaitIdle();
        printf("load %02d with %s: %d ms\n", sets_cur + 1, kTransNames[tr], n);
        CHECK(open_set == sets_cur);
        CHECK(!set_dirty);
        CHECK(e.SwapState() == 0);
    }
    CHECK(open_set == 1);
    CHECK(fabsf(e.global.bpm - 100.f) < 0.01f);
    CHECK(e.GetSlot(8).length == 600000);
    CHECK(!e.IsSynth(5));
    e.Stop();
    Tick(10);
    sets_cur = 0;
    DoAct(kActLoad);
    WaitIdle();
    CHECK(open_set == 0 && fabsf(e.global.bpm - 128.f) < 0.01f);

    /* ---------- tiny staging pool: everything loads after the switch ---------- */
    {
        /* reuse the pool but pretend it's nearly full */
        sets_cur = 1;
        DoAct(kActLoad);
        stage_used = stage_cap - 100; // what's left can't hold anything
        WaitIdle();
        CHECK(open_set == 1);
        CHECK(e.GetSlot(8).length == 600000);
        CHECK(e.GetSlot(2).length == 12000);
        CHECK(!set_dirty);
    }

    /* ---------- confirm logic, rename, delete ---------- */
    e.global.bpm = 90.f;
    Tick(600);
    CHECK(set_dirty);
    sets_cur  = 0;
    sets_mode = kSetsActions;
    sets_act  = kActLoad;
    SetsPush();
    CHECK(sets_mode == kSetsConfirm); // unsaved changes
    SetsTurn(1);
    SetsPush();
    WaitIdle();
    CHECK(open_set == 0);
    sets_mode = kSetsActions;
    sets_act  = kActRename;
    SetsPush();
    CHECK(sets_mode == kSetsRename);
    SetsTurn(1); // first letter '1' -> '2'?
    for(int i = 0; i < kSetNameLen; i++)
        SetsPush();
    CHECK(sets_mode == kSetsList);
    printf("renamed to '%s'\n", set_list[0].name);
    CHECK(set_list[0].name[0] == '2');
    /* APC fallback delete of slot 2: needs two presses */
    Tap(row3, 1);
    CHECK(sets_cur == 1);
    Tap(row4, 3);
    CHECK(set_exists[1]);
    Tap(row4, 3);
    CHECK(!set_exists[1]);
    CHECK(!Exists("sets/02"));

    /* ---------- power cycle: opens the last set ---------- */
    CHECK(Exists("sets/last.bin"));
    e.global.bpm = 77.f;
    open_set     = -1;
    BoxStorageReady(true);
    WaitIdle();
    CHECK(open_set == 0 && fabsf(e.global.bpm - 128.f) < 0.01f);

    /* ---------- touch ---------- */
    {
        SetsOpen(false);
        /* a panel mounted swapped and flipped: raw x follows screen y, backwards */
        auto rawx = [](int x, int y) { (void)x; return 3900 - y * 11; };
        auto rawy = [](int x, int y) { (void)y; return 250 + (int)(x * 7.4f); };
        auto press = [&](int x, int y, int n) {
            for(int i = 0; i < n; i++)
            {
                BoxTouchRaw(rawx(x, y) + (i % 3) - 1, rawy(x, y), true, now_ms);
                Tick(16);
            }
            BoxTouchRaw(0, 0, false, now_ms);
            Tick(16);
        };
        CHECK(!tcal_loaded);
        press(200, 200, 3); // first touch ever: starts calibration
        CHECK(touch_cal == 1);
        static uint16_t fbc[480 * 320];
        BoxTftInit(fbc);
        BoxTftCompose(now_ms); // draws the cross
        for(int i = 0; i < 3; i++)
            press(kCalX[i], kCalY[i], 10);
        CHECK(touch_cal == 0 && tcal_loaded);
        CHECK(Exists("apcbox/touch.bin"));
        float ex = tcal.a * rawx(300, 100) + tcal.b * rawy(300, 100) + tcal.c;
        float ey = tcal.d * rawx(300, 100) + tcal.e * rawy(300, 100) + tcal.f;
        printf("touch: (300,100) maps to (%.1f, %.1f)\n", ex, ey);
        CHECK(fabsf(ex - 300) < 3 && fabsf(ey - 100) < 3);

        /* header: tap a view dot */
        tft_view = 0;
        press(480 - 60 + 2 * 11 + 3, 10, 3);
        CHECK(tft_view == 2);
        /* PATTERNS: queue C4 */
        press(20 + 3 * 36 + 10, 28 + 2 * 26 + 10, 3);
        CHECK(e.QueuedPattern() == 19 || e.CurrentPattern() == 19); // stopped: switches at once
        /* STEPS: track 3, step 6 cycles off > on > accent > off */
        tft_view = 0;
        int pat  = e.CurrentPattern();
        e.SetStep(pat, 2, 5, kStepOff);
        int sx = 106 + 5 * 23 + 8, sy = 25 + 2 * 13 + 4;
        UiTouch(sx, sy, now_ms);
        CHECK(e.GetStep(pat, 2, 5) == kStepOn);
        UiTouch(sx, sy, now_ms);
        CHECK(e.GetStep(pat, 2, 5) == kStepAccent);
        UiTouch(sx, sy, now_ms);
        CHECK(e.GetStep(pat, 2, 5) == kStepOff);
        UiTouch(20, 25 + 9 * 13 + 4 + 4, now_ms); // track 10's name
        CHECK(sel_track == 9);
        /* MIXER: mute track 4, cycle its A/B */
        tft_view = 1;
        bool m   = e.track[3].mute;
        UiTouch(4 + 3 * 29 + 4, 252, now_ms);
        CHECK(e.track[3].mute != m);
        uint8_t xf = e.track[3].xfade;
        UiTouch(4 + 3 * 29 + 10, 265, now_ms);
        CHECK(e.track[3].xfade == (xf + 1) % 3);
        /* TRACK: waveform sets the start */
        tft_view = 3;
        SelectTrack(2);
        UiTouch(5 + (480 - 10) / 4, 130, now_ms);
        CHECK(fabsf(e.track[2].start - 0.25f) < 0.01f);
        /* SETS: tap slot 3, then SAVE HERE */
        SetsOpen(true);
        UiTouch(100, 54 + 2 * 26 + 10, now_ms);
        CHECK(sets_cur == 2 && sets_mode == kSetsActions);
        UiTouch(4 + kActSaveHere * 79 + 30, 284, now_ms);
        WaitIdle();
        CHECK(set_exists[2]);
        /* delete it: needs YES */
        UiTouch(4 + kActDelete * 79 + 30, 284, now_ms);
        CHECK(sets_mode == kSetsConfirm);
        UiTouch(480 / 2 + 60, 170, now_ms);
        CHECK(!set_exists[2]);
        SetsOpen(false);
        /* SYSTEM button starts calibration, back cancels and keeps the old one */
        tft_view = 4;
        UiTouch(420, 120, now_ms);
        CHECK(touch_cal == 1);
        BoxUiBack(now_ms);
        CHECK(touch_cal == 0 && tcal_loaded);
        /* a fresh boot reads the saved calibration */
        tcal_loaded = false;
        TouchLoadCal();
        CHECK(tcal_loaded);
        printf("touch ok\n");
    }

    /* the TFT and OLED draw it without trouble */
    static uint16_t fb16[480 * 320];
    BoxTftInit(fb16);
    SetsOpen(true);
    BoxTftCompose(now_ms);
    ComposeScreen(now_ms);
    sets_mode = kSetsConfirm;
    BoxTftCompose(now_ms);
    sets_mode = kSetsRename;
    BoxTftCompose(now_ms);

    /* ---------- T95 controllers: APC40 and keyboards through the bridge ---------- */
    {
        t95p.Init(T95Send);
        T95Tick(10);
        CHECK(T95Count("P 10 00 00 00 -1") >= 1); // version line with nothing attached
        t95_lines.clear();
        T95Frame("\x1bN1 APC40 mkI\n");
        T95Tick(5);
        CHECK(T95Count("X 1 f07e7f0601f7") == 1); // identity request
        T95Frame("\x1bM1 F07E000602477300190001000000 7F7F7F7F004B020009000700020308000301 0601F7\n");
        T95Tick(5);
        CHECK(t95p.Active() == 1);
        T95Tick(200);
        CHECK(T95Count("X 1 f04700736000044201010" "1f7") == 1); // one intro, to device id 0
        CHECK(t95p.ApcUp());
        int led_lines = 0;
        for(auto& l : t95_lines)
            led_lines += l.rfind("X 1 9", 0) == 0 || l.rfind("X 1 b", 0) == 0 || l.rfind("X 1 8", 0) == 0;
        CHECK(led_lines >= 1); // LEDs went out to it
        int nl, unsent = 0;
        LedSlot* lt = BoxLeds(&nl);
        for(int i = 0; i < nl; i++)
            unsent += lt[i].want != lt[i].sent;
        CHECK(unsent == 0);

        /* its buttons drive the box */
        char f[64];
        snprintf(f, sizeof(f), "\x1bM1 90%02x7f\n", apc::kShift);
        T95Frame(f);
        CHECK(shift);
        snprintf(f, sizeof(f), "\x1bM1 80%02x00\n", apc::kShift);
        T95Frame(f);
        CHECK(!shift);

        /* no room in the serial queue: LEDs wait, nothing is marked sent */
        t95_room = false;
        InvalidateLeds();
        T95Tick(20);
        unsent = 0;
        for(int i = 0; i < nl; i++)
            unsent += lt[i].want != lt[i].sent;
        CHECK(unsent > 0);
        t95_room = true;
        T95Tick(50);
        unsent = 0;
        for(int i = 0; i < nl; i++)
            unsent += lt[i].want != lt[i].sent;
        CHECK(unsent == 0);

        /* an APC40 on the Daisy's own USB-A wins */
        T95Tick(5, true);
        CHECK(t95p.Active() == 0 && !t95p.ApcUp());
        snprintf(f, sizeof(f), "\x1bM1 90%02x7f\n", apc::kShift);
        T95Frame(f);
        CHECK(!shift); // ignored while USB-A drives
        T95Tick(5, false);
        CHECK(t95p.Active() == 1);
        T95Tick(300);
        CHECK(t95p.ApcUp());

        /* keyboard: no identity reply, notes play the selected track */
        T95Frame("\x1bN2 LPK25\n");
        T95Tick(1100);
        SelectTrack(0);
        uint32_t trig0 = e.TriggerCount(0);
        t95_lines.clear();
        T95Frame("\x1bM2 903c64\n"); // C4 vel 100
        T95Tick(5);
        CHECK(e.TriggerCount(0) == trig0 + 1);
        CHECK(T95Count("I 2 903c64") == 1);
        CHECK(kb_track[60] == 0);
        T95Frame("\x1bM2 9043 50\n"); // another key, held
        T95Tick(5);
        T95Frame("\x1bM2 803c00\n");
        T95Tick(5);
        CHECK(kb_track[60] == -1 && kb_track[67] == 0);
        T95Tick(1000);
        CHECK(T95Count("P 10 03 01 02 1") >= 1);

        /* a 303 track: the note is folded into the 303's range and played */
        e.SetSynth(1, true);
        Tick(5);
        SelectTrack(1);
        uint32_t trig1 = e.TriggerCount(1);
        T95Frame("\x1bM2 906478\n"); // E7, hard: accent
        T95Tick(5);
        CHECK(e.TriggerCount(1) == trig1 + 1);
        CHECK(kb_track[100] == 1 && kb_note[100] >= kAcidLowNote && kb_note[100] <= kAcidHighNote
              && kb_note[100] % 12 == 100 % 12);
        T95Frame("\x1bM2 806400\n");
        T95Tick(5);
        CHECK(kb_track[100] == -1);
        e.SetSynth(1, false);
        SelectTrack(0);

        /* UI actions (control protocol 2) */
        CHECK(T95Count("P 10 ") >= 1);
        e.Stop();
        T95Tick(2);
        T95Frame("\x1bU play\n");
        T95Tick(2);                 // the engine starts on its next block
        CHECK(e.Playing());
        T95Frame("\x1bU stop\n");
        T95Tick(2);
        CHECK(!e.Playing());
        T95Frame("\x1bU track 5\n");
        CHECK(sel_track == 4);
        T95Frame("\x1bU track 17\n\x1bU track 0\n\x1bU dance\n");
        CHECK(sel_track == 4);
        SetsOpen(false);
        touch_cal = 0;
        int view0 = tft_view;
        T95Frame("\x1bU push\n");
        CHECK(tft_view != view0);
        T95Frame("\x1bU back\n");
        T95Frame("\x1bU turn 1\n");
        SelectTrack(0);

        /* Control protocol 4: the T95 pages drive everything. None of it
           goes through the APC40 code (test_bridge.py runs it with no
           controller at all) */
        {
            auto U = [](const char* a) { T95Frame(std::string("\x1bU ") + a + "\n"); };
            auto last = [](char type) {
                for(auto it = ui_lines.rbegin(); it != ui_lines.rend(); ++it)
                    if((*it)[0] == type)
                        return *it;
                return std::string();
            };
            e.Stop();
            U("mute 3 1");
            U("solo 9 1");
            CHECK(e.track[2].mute && e.track[8].solo);
            U("mute 3 2");
            U("solo 9 0");
            CHECK(!e.track[2].mute && !e.track[8].solo);
            U("knob 4 0 127");   // volume
            U("knob 4 1 0");     // pan hard left
            U("knob 4 12 2");    // crossfader side B
            CHECK(e.track[3].vol > 0.99f && e.track[3].pan < 0.01f && e.track[3].xfade == 2);
            U("knob 4 12 3");    // no such side
            U("knob 17 0 5");
            U("knob 4 13 5");
            U("knob 4 0 128");
            CHECK(e.track[3].xfade == 2 && e.track[3].vol > 0.99f);
            U("knob 4 0 100");
            U("knob 4 1 64");
            U("knob 4 12 0");
            U("acid 2 0 127");
            U("acid 2 5 64");    // tune snaps to the centre
            CHECK(e.track[1].acid.cutoff > 0.99f && e.track[1].acid.tune == 0.5f);
            U("glob 0 64");
            U("glob 8 0");
            CHECK(fabsf(e.global.master - 64 / 127.f) < 0.01f && e.global.swing < 0.01f);
            U("glob 0 102");
            U("tempo 1337");
            CHECK(fabsf(e.global.bpm - 133.7f) < 0.01f);
            U("tempo 5000");
            CHECK(fabsf(e.global.bpm - 133.7f) < 0.01f);
            U("tempo 1200");

            /* steps and notes in any pattern, undo for copy and clear */
            int p = 40;          // pattern F1, empty so far
            CHECK(e.PatternEmpty(p));
            U("step 41 6 1 1");
            U("step 41 6 5 2");
            CHECK(e.GetStep(p, 5, 0) == kStepOn && e.GetStep(p, 5, 4) == kStepAccent);
            U("step 41 6 5 3");  // no such value
            U("step 65 1 1 1");
            U("step 41 6 17 1");
            CHECK(e.GetStep(p, 5, 4) == kStepAccent);
            U("note 41 10 3 48 1");
            CHECK(e.GetNote(p, 9, 2) == 48 && e.GetSlide(p, 9, 2));
            U("note 41 10 3 50 0");
            CHECK(e.GetNote(p, 9, 2) == 50 && !e.GetSlide(p, 9, 2));
            T95Tick(40);
            {
                std::string o = last('O');
                CHECK(o.size() == 18);
                unsigned long long m = strtoull(o.c_str() + 2, nullptr, 16);
                CHECK(m & (1ull << p));
                CHECK(!(m & (1ull << 41)));
            }
            U("pcopy 41 42");
            CHECK(e.GetStep(41, 5, 4) == kStepAccent);
            U("undo");
            CHECK(e.PatternEmpty(41));
            U("pclear 41 6");
            CHECK(e.GetStep(p, 5, 0) == kStepOff && e.GetNote(p, 9, 2) == 50);
            U("undo");
            CHECK(e.GetStep(p, 5, 0) == kStepOn);
            U("pclear 41 0");
            CHECK(e.PatternEmpty(p));
            U("pat 41");
            CHECK(e.QueuedPattern() == p || e.CurrentPattern() == p);
            U("pat 1");
            T95Tick(5);

            /* transport bits */
            U("rec 1");
            CHECK(recording);
            U("rec 2");
            CHECK(!recording);
            U("metro 1");
            U("mon 1");
            CHECK(e.global.metronome && e.input_monitor);
            U("metro 0");
            U("mon 0");
            CHECK(!e.global.metronome && !e.input_monitor);

            /* a held FX lets go when the T95 goes quiet */
            U("fx 2 1");
            CHECK(e.PerfFxActive(1));
            U("fx 2 0");
            CHECK(!e.PerfFxActive(1));
            U("fx 9 1");

            /* track type, live loop arm from the page */
            U("type 7 303");
            T95Tick(300);
            CHECK(e.IsSynth(6));
            U("type 7 sample");
            T95Tick(300);
            CHECK(!e.IsSynth(6));
            U("type 7 banana");
            U("lrec arm 7");
            T95Tick(5);
            CHECK(e.LiveRecState() != 0 && e.LiveRecTrack() == 6); // armed, or already recording from the bar
            U("lrec cancel");
            T95Tick(5);
            CHECK(e.LiveRecState() == 0);
            e.Stop();            // arming started the transport
            T95Tick(1500);

            /* meters: a hit on track 1 shows there and on the master, not on track 2 */
            ui_lines.clear();
            U("knob 1 0 110");
            U("audition 1");
            T95Tick(60);
            {
                int peak1 = 0, peak2 = 0, pm = 0;
                for(auto& l : ui_lines)
                    if(l[0] == 'H' && l.size() == 2 + 36)
                    {
                        auto hx = [&](int i) { return (int)strtol(l.substr(2 + 2 * i, 2).c_str(), nullptr, 16); };
                        peak1 = std::max(peak1, hx(0));
                        peak2 = std::max(peak2, hx(1));
                        pm    = std::max(pm, hx(16));
                    }
                printf("meters: track 1 peak %d/255, master %d/255\n", peak1, pm);
                CHECK(peak1 > 20 && pm > 10 && peak2 == 0);
            }
            T95Tick(3000);
            if(last('H') != "H " + std::string(36, '0'))
                printf("last meters: %s\n", last('H').c_str());
            CHECK(last('H') == "H " + std::string(36, '0'));   // silence: all zero

            /* waveform outlines: every track once after a resync; 96 points for a sample */
            ui_lines.clear();
            BoxResync();
            T95Tick(33 * 20);
            {
                int n = 0, with = 0;
                std::string z1;
                for(auto& l : ui_lines)
                    if(l[0] == 'Z')
                    {
                        n++;
                        int t = atoi(l.c_str() + 2);
                        size_t sp = l.rfind(' ');
                        if(l.substr(sp + 1) != "-")
                        {
                            with++;
                            CHECK(l.size() - sp - 1 == 192);
                        }
                        if(t == 0)
                            z1 = l;
                    }
                if(!(n == 16 && with >= 8))
                    printf("outlines: %d lines, %d with a waveform\n", n, with);
                CHECK(n == 16 && with >= 8);
                CHECK(!z1.empty());
                unsigned long len = 0;
                int           sr  = 0;
                sscanf(z1.c_str(), "Z 0 %lu %d", &len, &sr);
                CHECK(len > 100 && sr == 48000);
                // a kick: loud at the start, quiet at the end
                size_t sp = z1.rfind(' ');
                int head = (int)strtol(z1.substr(sp + 1 + 2, 2).c_str(), nullptr, 16);
                int tail = (int)strtol(z1.substr(sp + 1 + 190, 2).c_str(), nullptr, 16);
                printf("outline of track 1: %lu samples, first %d last %d\n", len, head, tail);
                CHECK(head > tail);
            }
            ui_lines.clear();
            T95Tick(1500);       // the once-a-second repeat doesn't resend outlines
            {
                int n = 0;
                for(auto& l : ui_lines)
                    n += l[0] == 'Z';
                CHECK(n == 0);
            }
            /* Control protocol 5: sets and kits from a page */
            {
                auto has = [](const std::string& want) {
                    for(auto& l : ui_lines)
                        if(l == want)
                            return true;
                    return false;
                };
                auto wait_idle = []() {
                    for(int i = 0; i < 20000 && (set_op || load_phase); i++)
                        T95Tick(1);
                    T95Tick(50);
                };
                wait_idle();
                int free0 = FirstFreeSlot();
                CHECK(free0 >= 0);
                ui_lines.clear();
                e.track[4].mute = true;
                U("tempo 1410");
                U("set new jam one");
                wait_idle();
                CHECK(set_exists[free0] && !strcmp(set_list[free0].name, "jam one"));
                CHECK(open_set == free0);
                char want[64];
                snprintf(want, sizeof(want), "J %d 1 1410 %d %d %d jam one", free0, set_list[free0].samples,
                         set_list[free0].loops, set_list[free0].synths);
                if(!has(want))
                    printf("no line '%s'\n", want);
                CHECK(has(want));
                // change things, load it back: as saved
                e.track[4].mute = false;
                U("tempo 900");
                T95Tick(600);
                CHECK(set_dirty);
                char cmd[40];
                snprintf(cmd, sizeof(cmd), "set load %d", free0 + 1);
                U(cmd);
                wait_idle();
                CHECK(e.track[4].mute && fabsf(e.global.bpm - 141.f) < 0.01f);
                // save over it with a new name, rename, transition
                snprintf(cmd, sizeof(cmd), "set save %d second-take", free0 + 1);
                U(cmd);
                wait_idle();
                CHECK(!strcmp(set_list[free0].name, "second-take"));
                snprintf(cmd, sizeof(cmd), "set name %d final!", free0 + 1);   // "!" isn't allowed in a name
                U(cmd);
                T95Tick(50);
                CHECK(!strcmp(set_list[free0].name, "final_"));
                U("set trans 3");
                T95Tick(50);
                CHECK(set_trans == 3 && has("JT 3 ECHO OUT"));
                U("set trans 9");
                U("set load 9");
                U("set explode 1");
                CHECK(set_trans == 3);
                snprintf(cmd, sizeof(cmd), "set delete %d", free0 + 1);
                U(cmd);
                T95Tick(50);
                CHECK(!set_exists[free0]);
                snprintf(want, sizeof(want), "J %d 0", free0);
                CHECK(has(want));
                e.track[4].mute = false;
                U("tempo 1200");
                U("set trans 1");
                // every kit by name after a resync, and which kit each track page has
                ui_lines.clear();
                BoxResync();
                T95Tick(100);
                int kl = 0, kp = 0;
                for(auto& l : ui_lines)
                {
                    kl += !l.compare(0, 3, "KL ");
                    kp += !l.compare(0, 3, "KP ");
                }
                CHECK(kl == num_kits && kp >= 1);
                U("kit 2 1");
                wait_idle();
                T95Tick(3000);
                CHECK(loaded_kit_pg[1] == 1);
                printf("t95 sets and kits ok (%d kits)\n", num_kits);
            }
            /* Control protocol 6: following the T95's clock (ESC T), and reporting our own steps (CL) */
            {
                auto frame = [](int bpm100, long ticks, int playing, int lat_us = 0) {
                    char f[48];
                    snprintf(f, sizeof(f), "\x1bT %d %ld %d %d\n", bpm100, ticks, playing, lat_us);
                    T95Frame(f);
                };
                e.Stop();
                T95Tick(50);
                e.global.swing = 0.f;
                // 125 bpm: a sixteenth is exactly 120 ms. The outside clock starts; we start with it.
                long ticks = 0;
                frame(12500, ticks, 1);
                T95Tick(1);
                CHECK(ext_follow && e.Playing() && fabsf(e.global.bpm - 125.f) < 0.01f);
                // 6 bars of frames on the outside clock's grid (one per sixteenth), each arriving 0-4 ms
                // late, with the Daisy starting 9 ms behind
                T95Tick(9);
                uint32_t t0 = now_ms - 9 - 1;   // when the outside clock was at 0
                unsigned seed = 5;
                float worst_late = 0;
                for(int k = 1; k <= 96; k++)
                {
                    seed       = seed * 1103515245u + 12345u;
                    int jitter = (seed >> 16) % 5;
                    uint32_t arrive = t0 + 120u * k + jitter;
                    T95Tick((int)(arrive - now_ms));
                    ticks += 6;
                    frame(12500, ticks, 1, 2000); // the bridge says frames take 2 ms (the average of 0-4)
                    T95Tick(0);
                    if(k > 64)
                        worst_late = std::max(worst_late, fabsf(e.ExtErrMs()));
                }
                printf("clock follow: smoothed error after 4 bars %.2f ms (frames 0-4 ms late)\n", worst_late);
                CHECK(worst_late < 2.0f);
                CHECK(e.Playing() && ext_follow);
                // the steps still come from the audio clock: 125 bpm, not the frames' arrival times
                CHECK(fabsf(e.global.bpm - 125.f) < 0.01f);
                // a jump of half a bar on the outside clock (locate): we jump too
                int before = e.ClockStep();
                ticks += 48;
                frame(12500, ticks, 1);
                T95Tick(2);
                CHECK(((e.ClockStep() - before + 16) % 16) >= 7 && ((e.ClockStep() - before + 16) % 16) <= 9);
                // our own steps are reported with their age, for the T95 to follow us
                {
                    int n = 0;
                    unsigned long worst_age = 0;
                    for(auto& l : ui_lines)
                        if(!l.compare(0, 5, "CL 1 "))
                        {
                            int           st, bpm;
                            unsigned long age, cnt;
                            if(sscanf(l.c_str(), "CL 1 %d %d %lu %lu", &st, &bpm, &age, &cnt) == 4)
                            {
                                n++;
                                worst_age = std::max(worst_age, age);
                                CHECK(bpm == 12500 && st >= 0 && st < 16);
                            }
                        }
                    printf("step reports: %d, oldest %lu us\n", n, worst_age);
                    CHECK(n > 80 && worst_age < 40000);
                }
                // too fast for the Daisy: it stops following, keeps its tempo
                frame(25000, ticks, 1);
                T95Tick(2);
                CHECK(!ext_follow && fabsf(e.global.bpm - 125.f) < 0.01f);
                frame(12500, ticks, 1);
                T95Tick(2);
                CHECK(ext_follow);
                // the outside clock stops: so do we. Then it goes away: we're on our own
                frame(12500, ticks, 0);
                T95Tick(5);
                CHECK(!e.Playing());
                T95Tick(2300);
                CHECK(!ext_follow);
                bool said = false;
                for(auto& l : ui_lines)
                    said |= l == "CK 0 0";
                CHECK(said);
                U("tempo 1200");
                printf("t95 clock follow ok\n");
            }
            SelectTrack(0);
            printf("t95 page controls ok\n");
        }

        /* ping */
        T95Frame("\x1bP abc123\n");
        CHECK(T95Count("Y abc123") == 1);

        /* keyboard unplugged: its held note is released */
        T95Frame("\x1bN2 -\n");
        T95Tick(5);
        CHECK(kb_track[67] == -1);

        /* the bridge goes silent: everything from the T95 is dropped */
        snprintf(f, sizeof(f), "\x1bM1 90%02x7f\n", apc::kShift);
        T95Frame(f);
        CHECK(shift);
        T95Tick(2100, false, false);
        CHECK(t95p.Active() == -1 && !t95p.AnyAttached());
        CHECK(!shift); // held buttons released
        CHECK(T95Count("P 10 00 00 00 -1") >= 1);

        /* A frame stamped a moment after the main loop read the clock (how the
           Daisy does it) must not look like 49 days of silence */
        T95Frame("\x1bN4 LPK25\n");
        {
            HostCmd h;
            for(unsigned char b : std::string("\x1bK"))
                if(h.Feed(b) == HostCmd::kKeepalive)
                    t95p.OnCmd(h, HostCmd::kKeepalive, now_ms + 3);
        }
        t95p.Poll(now_ms, false);
        CHECK(t95p.AnyAttached());
        T95Tick(5);
        CHECK(t95p.AnyAttached());
        T95Frame("\x1bN4 -\n");

        /* ---------- samples from the T95 (ESC F) ---------- */
        {
            auto b64 = [](const uint8_t* d, size_t n) {
                static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string o;
                for(size_t i = 0; i < n; i += 3)
                {
                    uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
                    o += t[v >> 18 & 63];
                    o += t[v >> 12 & 63];
                    o += i + 1 < n ? t[v >> 6 & 63] : '=';
                    o += i + 2 < n ? t[v & 63] : '=';
                }
                return o;
            };
            auto has = [](const char* prefix) {
                for(auto& l : xfer_lines)
                    if(l.rfind(prefix, 0) == 0)
                        return true;
                return false;
            };
            // a 0.25 s 16-bit mono WAV, as the T95 sends it (converted to 48 kHz)
            const int        frames = 12000;
            std::vector<uint8_t> w(44 + frames * 2);
            auto p32 = [&](int o, uint32_t v) { memcpy(&w[o], &v, 4); };
            auto p16 = [&](int o, uint16_t v) { memcpy(&w[o], &v, 2); };
            memcpy(&w[0], "RIFF", 4); p32(4, 36 + frames * 2); memcpy(&w[8], "WAVEfmt ", 8);
            p32(16, 16); p16(20, 1); p16(22, 1); p32(24, 48000); p32(28, 96000); p16(32, 2); p16(34, 16);
            memcpy(&w[36], "data", 4); p32(40, frames * 2);
            for(int i = 0; i < frames; i++)
            {
                int16_t v = (int16_t)(9000 * sinf(6.2831853f * 330.f * i / 48000.f));
                memcpy(&w[44 + i * 2], &v, 2);
            }
            const int nchunks = (int)((w.size() + 511) / 512);
            auto chunk = [&](int id, int seq, bool corrupt = false) {
                size_t   off = (size_t)seq * 512, n = std::min<size_t>(512, w.size() - off);
                uint32_t crc = Crc32(&w[off], (uint32_t)n) ^ (corrupt ? 1u : 0u);
                char     h[64];
                snprintf(h, sizeof(h), "\x1b" "FD %d %d %lx ", id, seq, (unsigned long)crc);
                T95Frame(std::string(h) + b64(&w[off], n) + "\n");
            };
            SelectTrack(2);
            T95Tick(5);
            uint32_t old_len = e.GetSlot(2).length;
            CHECK(old_len > 0); // a built-in sound is playing there now
            xfer_lines.clear();

            char o[96];
            snprintf(o, sizeof(o), "\x1b" "FO 7 3 %d tone 330.wav\n", (int)w.size());
            T95Frame(o);
            CHECK(has("FO 7 ok"));
            snprintf(o, sizeof(o), "\x1b" "FO 8 3 %d other.wav\n", (int)w.size());
            T95Frame(o);
            CHECK(has("FO 8 err busy")); // one at a time
            // backwards, chunk 3 twice, chunk 5 corrupted first
            for(int sq = nchunks - 1; sq >= 0; sq--)
            {
                if(sq == 5)
                    chunk(7, sq, true);
                chunk(7, sq);
                if(sq == 3)
                    chunk(7, sq);
            }
            CHECK(has("FA 7 5 err crc"));
            CHECK(has("FA 7 0 ok"));
            CHECK(e.GetSlot(2).length == old_len); // still the old sound while receiving
            // a set can't load while the staging pool holds the sample
            int op0 = set_op;
            LoadSetBegin(0);
            CHECK(set_op == op0);
            T95Frame("\x1b" "FC 7\n");
            CHECK(has("FC 7 ok"));
            for(int i = 0; i < 200 && !has("FL 7"); i++)
                T95Tick(1);
            CHECK(has("FL 7 ok"));
            CHECK(e.GetSlot(2).length == (uint32_t)frames);
            CHECK(strcmp(slot_path[2], "t95/tone_330.wav") == 0);
            CHECK(!XferBusy());

            // commit with chunks missing, then abort
            xfer_lines.clear();
            snprintf(o, sizeof(o), "\x1b" "FO 9 3 %d x.wav\n", (int)w.size());
            T95Frame(o);
            chunk(9, 0);
            T95Frame("\x1b" "FC 9\n");
            char miss[32];
            snprintf(miss, sizeof(miss), "FC 9 err missing %d", nchunks - 1);
            CHECK(has(miss));
            T95Frame("\x1b" "FA 9\n");
            CHECK(has("FC 9 err aborted") && !XferBusy());

            // too big, bad track, a set busy
            xfer_lines.clear();
            T95Frame("\x1b" "FO 10 3 99999999 huge.wav\n");
            T95Frame("\x1b" "FO 11 17 1000 t.wav\n");
            set_op = 2;
            T95Frame("\x1b" "FO 12 3 1000 t.wav\n");
            set_op = 0;
            CHECK(has("FO 10 err too-big") && has("FO 11 err track") && has("FO 12 err set-busy"));

            // silence for 10 s: dropped, and the next one works
            snprintf(o, sizeof(o), "\x1b" "FO 13 3 %d y.wav\n", (int)w.size());
            T95Frame(o);
            CHECK(XferBusy());
            T95Tick(10100);
            CHECK(!XferBusy());
            snprintf(o, sizeof(o), "\x1b" "FO 14 3 %d y.wav\n", (int)w.size());
            T95Frame(o);
            CHECK(has("FO 14 ok"));
            T95Frame("\x1b" "FA 14\n");
            SelectTrack(0);
            printf("t95 samples ok\n");
        }

        /* garbage and frames for ports nobody announced do nothing */
        T95Frame("\x1bM3 903c64\n\x1bM9 903c64\n\x1bMx\n\x1bN\n");
        CHECK(t95p.Unattached() == 1);
        printf("t95 ports ok\n");
    }

    /* ---------- looper, one-shot sampling, quantise, roll, keyboard knobs ---------- */
    {
        auto U     = [](const char* a) {
            CHECK(BoxUiCommand(a, now_ms));
            Tick(2);
        };
        auto until = [](const std::function<bool()>& f, int max_ms) {
            int n = 0;
            while(!f() && n++ < max_ms)
                Tick();
            return f();
        };
        auto idle = [] { return e.LiveRecState() == 0; };
        e.Stop();
        WaitIdle();
        Tick(50);
        U("tempo 1200"); // a bar is 2000 ms, a step 125 ms
        U("pat 1");
        const Slot& s5 = e.GetSlot(4);

        /* a take of a fixed length stops by itself */
        U("lrec len 1");
        test_in_amp = 0.3f;
        ui_lines.clear();
        U("lrec arm 5");
        Tick(30);
        CHECK(e.LiveRecState() == 2 && e.LiveRecMode() == Engine::kRecLoop);
        Tick(2100);
        CHECK(idle() && e.IsLoop(4) && e.LoopBars(4) == 1 && labs((long)s5.length - 96000) < 4);
        Tick(40);
        auto lpf = [](int i) { // field i of the LP line
            int f[10] = {};
            sscanf(rep_lp, "LP %d %d %d %d %d %d %d %d %d %d", f, f + 1, f + 2, f + 3, f + 4, f + 5, f + 6, f + 7, f + 8, f + 9);
            return f[i];
        };
        CHECK(lpf(7) == 5 && lpf(8) == 0 && lpf(9) == 1); // the take on track 5 can be undone; FX snap to the step

        /* overdub: on top, in place, one pass; undo puts the first take back */
        std::vector<int16_t> first(s5.data, s5.data + s5.length);
        test_in_amp = 0.2f;
        U("lrec over 5");
        CHECK(e.LiveRecState() == 1 && e.LiveRecMode() == Engine::kRecOverdub);
        CHECK(until([] { return e.LiveRecState() == 2; }, 2100));
        CHECK(e.IsLoop(4)); // still playing underneath
        CHECK(until(idle, 2100));
        Tick(5);
        long diff = 0;
        for(uint32_t k = 0; k < s5.length; k++)
            diff += labs((long)s5.data[k] - first[k]);
        CHECK(diff > 96000L * 1000 && labs((long)s5.length - 96000) < 4 && e.LoopBars(4) == 1);
        std::vector<int16_t> dubbed(s5.data, s5.data + s5.length);
        U("lrec undo");
        CHECK(!memcmp(s5.data, first.data(), first.size() * 2) && e.IsLoop(4));
        Tick(40);
        CHECK(lpf(7) == 0 && lpf(8) == 5);
        U("lrec undo"); // only one take back
        CHECK(e.IsLoop(4) && !memcmp(s5.data, first.data(), first.size() * 2));
        U("lrec redo"); // and forward again
        CHECK(!memcmp(s5.data, dubbed.data(), dubbed.size() * 2) && e.IsLoop(4));
        U("lrec redo");
        CHECK(!memcmp(s5.data, dubbed.data(), dubbed.size() * 2));
        /* the APC40: the device arrows are undo and redo, OVERDUB overdubs the selected track */
        Btn(apc::kDevLeft, 0, true);
        Btn(apc::kDevLeft, 0, false);
        CHECK(!memcmp(s5.data, first.data(), first.size() * 2));
        Btn(apc::kDevRight, 0, true);
        Btn(apc::kDevRight, 0, false);
        CHECK(!memcmp(s5.data, dubbed.data(), dubbed.size() * 2));
        Btn(apc::kDevLeft, 0, true);
        Btn(apc::kDevLeft, 0, false);
        CHECK(!memcmp(s5.data, first.data(), first.size() * 2) && e.IsLoop(4));
        SelectTrack(4);
        Btn(apc::kOverdub, 0, true);
        Btn(apc::kOverdub, 0, false);
        Tick(3);
        CHECK(e.LiveRecState() == 1 && e.LiveRecMode() == Engine::kRecOverdub && e.LiveRecTrack() == 4);
        Btn(apc::kOverdub, 0, true); // again before it started: off
        Btn(apc::kOverdub, 0, false);
        Tick(2100);
        CHECK(idle() && e.IsLoop(4) && !memcmp(s5.data, first.data(), first.size() * 2));
        SelectTrack(0);

        /* not at another tempo (it would land in the wrong place) */
        U("tempo 1300");
        U("lrec over 5");
        CHECK(idle());
        U("tempo 1200");
        Tick(5);

        /* a new take over a loop, undone: the old loop is back */
        U("lrec len 2");
        U("lrec arm 5");
        CHECK(until([] { return e.LiveRecState() == 2; }, 2100));
        CHECK(until(idle, 4100));
        Tick(5);
        CHECK(e.LoopBars(4) == 2 && labs((long)s5.length - 192000) < 4);
        U("lrec undo");
        Tick(2100);
        CHECK(e.IsLoop(4) && e.LoopBars(4) == 1 && labs((long)s5.length - 96000) < 4);
        CHECK(!memcmp(s5.data, first.data(), first.size() * 2));

        /* cancelling an armed take gives the loop back too */
        U("lrec arm 5");
        Tick(3);
        U("lrec cancel");
        Tick(2100);
        CHECK(idle() && e.IsLoop(4) && labs((long)s5.length - 96000) < 4);

        /* a take that starts on the beat keeps its place in the bar */
        U("quant rec 1");
        U("lrec len 1");
        CHECK(until([] { return e.CurrentStep() == 5; }, 2100));
        U("lrec arm 6");
        CHECK(until([] { return e.LiveRecState() == 2; }, 500));
        CHECK(e.CurrentStep() == 8);
        CHECK(until(idle, 2100));
        CHECK(e.CurrentStep() == 8 && e.IsLoop(5) && e.LoopBars(5) == 1 && e.LoopOffset(5) == 8);
        U("quant rec 0");

        /* count-in: from stopped, a bar of clicks first */
        e.Stop();
        Tick(50);
        U("lrec count 1");
        U("lrec arm 8");
        Tick(1500);
        CHECK(e.LiveRecState() == 1 && e.Playing());
        Tick(600);
        CHECK(e.LiveRecState() == 2);
        CHECK(until(idle, 2100));
        CHECK(e.IsLoop(7) && e.LoopBars(7) == 1);
        U("lrec count 0");
        U("lrec len 0");

        /* one-shot: waits for sound, ends when told, nothing to do with the transport */
        e.Stop();
        Tick(50);
        test_in_amp = 0.f;
        U("lrec shot 7");
        Tick(200);
        CHECK(e.LiveRecState() == 1 && e.LiveRecMode() == Engine::kRecShot && !e.Playing());
        test_in_amp = 0.5f;
        Tick(300);
        CHECK(e.LiveRecState() == 2);
        U("lrec stop");
        Tick(5);
        const Slot& s7 = e.GetSlot(6);
        CHECK(idle() && !e.IsLoop(6) && !e.Playing());
        CHECK(s7.length > 13500 && s7.length < 15000 && s7.data[s7.length - 1] == 0);
        CHECK(IsRecPath(slot_path[6]));
        test_in_amp = 0.f;
        const uint32_t shot_len = s7.length;
        U("lrec undo"); // the kit sound comes back
        WaitIdle();
        CHECK(!IsRecPath(slot_path[6]) && BuiltInKitOf(slot_path[6]) >= 0);
        Tick(40);
        U("lrec redo"); // and the recording again
        CHECK(IsRecPath(slot_path[6]) && s7.length == shot_len && !e.IsLoop(6));
        U("lrec undo");
        WaitIdle();
        CHECK(!IsRecPath(slot_path[6]) && BuiltInKitOf(slot_path[6]) >= 0);

        /* a sample changed by accident with the arrows: undo gives the original back */
        {
            SelectTrack(9);
            char was[kPathLen];
            snprintf(was, kPathLen, "%s", slot_path[9]);
            ScanFolders();
            cur_folder = 0;
            for(int i = 0; i < num_folders; i++)
                if(!strcmp(folders[i], "samples/test"))
                    cur_folder = i;
            ScanFiles(cur_folder);
            CHECK(num_files >= 2);
            Btn(apc::kShift, 0, true);
            Btn(apc::kDevRight, 0, true); // SHIFT + arrow: next sample
            Btn(apc::kDevRight, 0, false);
            WaitIdle();
            Btn(apc::kDevRight, 0, true); // and another: still one undo back to the original
            Btn(apc::kDevRight, 0, false);
            Btn(apc::kShift, 0, false);
            WaitIdle();
            Tick(40);
            char browsed[kPathLen];
            snprintf(browsed, kPathLen, "%s", slot_path[9]);
            CHECK(strncmp(browsed, "samples/test/", 13) == 0 && strcmp(browsed, was) && lpf(7) == 10);
            Btn(apc::kDevLeft, 0, true); // the arrow alone: undo
            Btn(apc::kDevLeft, 0, false);
            WaitIdle();
            Tick(40);
            CHECK(!strcmp(slot_path[9], was) && lpf(7) == 0 && lpf(8) == 10);
            Btn(apc::kDevRight, 0, true); // redo: the browsed one again
            Btn(apc::kDevRight, 0, false);
            WaitIdle();
            Tick(40);
            CHECK(!strcmp(slot_path[9], browsed));
            Btn(apc::kDevLeft, 0, true);
            Btn(apc::kDevLeft, 0, false);
            WaitIdle();
            CHECK(!strcmp(slot_path[9], was));
            /* over a recorded loop: the loop itself comes back */
            SelectTrack(4);
            CHECK(e.IsLoop(4));
            Btn(apc::kShift, 0, true);
            Btn(apc::kDevRight, 0, true);
            Btn(apc::kDevRight, 0, false);
            Btn(apc::kShift, 0, false);
            WaitIdle();
            Tick(40);
            CHECK(!IsRecPath(slot_path[4]));
            Btn(apc::kDevLeft, 0, true);
            Btn(apc::kDevLeft, 0, false);
            Tick(2100);
            CHECK(e.IsLoop(4) && IsRecPath(slot_path[4]) && labs((long)s5.length - 96000) < 4
                  && !memcmp(s5.data, first.data(), first.size() * 2));
            SelectTrack(0);
        }

        /* pattern switches: on the next step, on the beat, on the bar */
        e.Play();
        Tick(20);
        U("quant pat 2");
        CHECK(until([] { return e.CurrentStep() == 5; }, 2100));
        U("pat 3");
        Tick(130);
        CHECK(e.CurrentPattern() == 2);
        U("quant pat 1");
        CHECK(until([] { return e.CurrentStep() == 9; }, 2100));
        U("pat 1");
        Tick(130);
        CHECK(e.CurrentPattern() == 2);
        CHECK(until([] { return e.CurrentStep() == 12; }, 500));
        CHECK(e.CurrentPattern() == 0);
        U("quant pat 0");
        CHECK(until([] { return e.CurrentStep() == 2; }, 2100));
        U("pat 3");
        Tick(1000);
        CHECK(e.CurrentPattern() == 0);
        CHECK(until([] { return e.CurrentStep() == 0; }, 2100));
        CHECK(e.CurrentPattern() == 2);
        U("pat 1");

        /* mutes can wait for the bar */
        U("quant mute 2");
        CHECK(until([] { return e.CurrentStep() == 2; }, 4100));
        U("mute 1 1");
        Tick(300);
        CHECK(e.track[0].mute && !e.MuteLive(0));
        CHECK(until([] { return e.CurrentStep() == 0; }, 2100));
        Tick(3);
        CHECK(e.MuteLive(0));
        U("quant mute 0");
        U("mute 1 0");
        Tick(3);
        CHECK(!e.MuteLive(0));

        /* held FX wait for the step or the beat, on and off; at once when stopped */
        CHECK(e.quant_fx == 1);
        U("quant fx 2");
        CHECK(until([] { return e.CurrentStep() == 5; }, 2100));
        U("fx 3 1");
        Tick(60);
        CHECK(e.PerfFxActive(2) && !e.PerfFxLive(2));
        CHECK(until([] { return e.CurrentStep() == 8; }, 500));
        Tick(3);
        CHECK(e.PerfFxLive(2));
        U("fx 3 0");
        Tick(60);
        CHECK(!e.PerfFxActive(2) && e.PerfFxLive(2));
        CHECK(until([] { return e.CurrentStep() == 12; }, 600));
        Tick(3);
        CHECK(!e.PerfFxLive(2));
        U("quant fx 1");
        CHECK(until([] { return e.CurrentStep() == 1; }, 2100));
        Tick(20);
        U("fx 3 1");
        Tick(20);
        CHECK(!e.PerfFxLive(2));
        CHECK(until([] { return e.CurrentStep() == 2; }, 200));
        Tick(3);
        CHECK(e.PerfFxLive(2));
        U("quant fx 0");
        U("fx 3 0");
        Tick(3);
        CHECK(!e.PerfFxLive(2));
        U("quant fx 1");

        /* roll from a page; it lets go when the T95 goes quiet */
        uint32_t c = e.TriggerCount(2);
        U("roll 3 1");
        Tick(1000);
        CHECK(e.TriggerCount(2) - c >= 4);
        BoxUiGone();
        Tick(300);
        c = e.TriggerCount(2);
        e.ClearTrack(e.CurrentPattern(), 2);
        Tick(1000);
        CHECK(e.TriggerCount(2) == c);
        e.Stop();
        Tick(50);

        /* a keyboard's knobs, wheel and pads */
        SelectTrack(1);
        BoxKeys(3, 0, 74, 100, now_ms);
        CHECK(fabsf(e.track[1].filter - 100.f / 127.f) < 0.001f);
        BoxKeys(3, 0, 7, 50, now_ms);
        CHECK(fabsf(e.track[1].vol - 50.f / 127.f) < 0.001f);
        BoxKeys(6, 0, 0, 96, now_ms); // half way up
        CHECK(fabsf(e.bend_st[1] - 1.f) < 0.01f);
        BoxKeys(6, 0, 0, 64, now_ms);
        CHECK(e.bend_st[1] == 0.f);
        c = e.TriggerCount(2);
        BoxKeys(1, 9, 38, 100, now_ms);
        Tick(5);
        CHECK(e.TriggerCount(2) == c + 1);
        e.track[1] = TrackParams();
        U("ingain 127");
        CHECK(fabsf(e.in_gain - 4.f) < 0.01f);
        U("ingain 64");
        CHECK(e.in_gain == 1.f);
        CHECK(!BoxUiCommand("lrec len 3", now_ms) && !BoxUiCommand("quant rec 2", now_ms));
        for(int t : {4, 5, 7}) // back to kit sounds
        {
            e.ClearLoop(t);
            char p[8];
            snprintf(p, sizeof(p), "@k%d", BuiltInKitFor(t));
            QueueLoad(t, p, false);
        }
        WaitIdle();
        SelectTrack(0);
        printf("looper, quantise, roll, keyboard controls ok\n");
    }

    /* ---------- kits made on the box: save, settings, 303s, undo, rename, delete ---------- */
    {
        auto U    = [](const char* a) {
            CHECK(BoxUiCommand(a, now_ms));
            Tick(2);
        };
        auto done = [] {
            int n = 0;
            while(set_op && n++ < 20000)
                Tick();
            Tick(60);
            return set_op == 0;
        };
        auto lp = [](int i) {
            int f[10] = {};
            sscanf(rep_lp, "LP %d %d %d %d %d %d %d %d %d %d", f, f + 1, f + 2, f + 3, f + 4, f + 5, f + 6, f + 7, f + 8, f + 9);
            return f[i];
        };
        const int nb = Engine::NumBuiltInKits();
        e.Stop();
        WaitIdle();
        U("tempo 1200");
        U("kit 1 0"); // a known start: the first built-in kit on tracks 1-8
        WaitIdle();
        e.track[0].vol = 0.3f;
        e.track[1].pan = 0.9f;
        QueueLoad(1, "samples/test/tone a.wav", false);
        QueueLoad(2, kSynthPath, false);
        WaitIdle();
        e.track[2].acid.cutoff = 0.9f;
        e.track[2].rev         = 0.4f;
        e.track[2].xfade       = 2;
        U("lrec len 1");
        U("lrec arm 4"); // a live loop on track 4: not part of a kit
        int w = 0;
        while((e.LiveRecState() != 0 || !e.IsLoop(3)) && w++ < 5000)
            Tick();
        CHECK(e.IsLoop(3));
        e.Stop();
        Tick(20);
        U("lrec len 0");

        const int n0 = num_kits;
        U("kit save 0   my first kit!  ");
        CHECK(set_op == 3);
        CHECK(done());
        int k = FindKit("my first kit_");
        CHECK(num_kits == n0 + 1 && k >= nb);
        CHECK(Exists("kits/my first kit_/1 kick.wav") && Exists("kits/my first kit_/2 tone a.wav"));
        CHECK(Exists("kits/my first kit_/kit.txt") && Exists("kits/my first kit_/8 perc.wav"));
        CHECK(!Exists("kits/my first kit_/3 hat.wav") && !Exists("kits/my first kit_/4 open hat.wav"));
        CHECK(loaded_kit_pg[0] != k); // a loop among the tracks: they aren't quite this kit
        Tick(60);
        CHECK(rep_kitpg[2] == num_kits);

        /* other sounds and settings on the page, then the saved kit back */
        e.ClearLoop(3);
        QueueLoad(3, "@k0", false);
        QueueLoad(2, "@k0", false);
        QueueLoad(1, "@k0", false);
        WaitIdle();
        e.track[0].vol = 0.8f;
        e.track[1].pan = 0.5f;
        e.track[2]     = TrackParams();
        Tick(60);
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "kit %d 0", k + 1);
        U(cmd);
        WaitIdle();
        Tick(60);
        CHECK(fabsf(e.track[0].vol - 0.3f) < 0.001f && fabsf(e.track[1].pan - 0.9f) < 0.001f);
        CHECK(!strcmp(slot_path[1], "kits/my first kit_/2 tone a.wav") && !strcmp(slot_path[0], "kits/my first kit_/1 kick.wav"));
        CHECK(e.IsSynth(2) && fabsf(e.track[2].acid.cutoff - 0.9f) < 0.001f && fabsf(e.track[2].rev - 0.4f) < 0.001f
              && e.track[2].xfade == 2);
        CHECK(!strcmp(slot_path[3], "@k0")); // not in the kit: left alone
        CHECK(lp(7) == 101 && lp(8) == 0);  // the kit load can be undone

        /* undo: the 8 tracks as before the kit; redo: the kit again */
        U("lrec undo");
        WaitIdle();
        Tick(60);
        CHECK(!e.IsSynth(2) && !strcmp(slot_path[1], "@k0") && fabsf(e.track[0].vol - 0.8f) < 0.001f
              && fabsf(e.track[1].pan - 0.5f) < 0.001f);
        CHECK(lp(7) == 0 && lp(8) == 101);
        Btn(apc::kDevRight, 0, true); // the APC40's arrow
        Btn(apc::kDevRight, 0, false);
        WaitIdle();
        Tick(60);
        CHECK(e.IsSynth(2) && !strcmp(slot_path[1], "kits/my first kit_/2 tone a.wav") && fabsf(e.track[0].vol - 0.3f) < 0.001f);
        CHECK(loaded_kit_pg[0] == k);

        /* a recording on a track is never replaced by a kit */
        U("lrec len 1");
        U("lrec arm 6");
        w = 0;
        while((e.LiveRecState() != 0 || !e.IsLoop(5)) && w++ < 5000)
            Tick();
        e.Stop();
        Tick(20);
        U("lrec len 0");
        U(cmd);
        WaitIdle();
        CHECK(e.IsLoop(5) && IsRecPath(slot_path[5]));
        e.ClearLoop(5);
        QueueLoad(5, "@k0", false);
        WaitIdle();

        /* the same name again: only with "over"; other sound on track 2: its old file goes */
        U("kit save 0 MY FIRST KIT_");
        CHECK(set_op == 0 && num_kits == n0 + 1);
        QueueLoad(1, "samples/test/tone b.wav", false);
        WaitIdle();
        U("kit over 0 my first kit_");
        CHECK(set_op == 3 && done());
        CHECK(num_kits == n0 + 1 && Exists("kits/my first kit_/2 tone b.wav") && !Exists("kits/my first kit_/2 tone a.wav"));
        CHECK(Exists("kits/my first kit_/1 kick.wav") && Exists("kits/my first kit_/6 tom.wav"));
        k = FindKit("my first kit_");
        CHECK(loaded_kit_pg[0] == k); // all 8 tracks are in it now

        /* built-in kits can't be replaced, renamed or deleted */
        snprintf(cmd, sizeof(cmd), "kit save 0 %s", kit_names[0]);
        U(cmd);
        CHECK(set_op == 0);
        U("kit over 0 808");
        U("kit delete 1");
        U("kit rename 1 mine");
        CHECK(set_op == 0 && num_kits == n0 + 1 && !strcmp(kit_names[0], Engine::BuiltInKitName(0)));

        /* rename: the folder moves, what's loaded follows */
        snprintf(cmd, sizeof(cmd), "kit %d 0", k + 1);
        U(cmd);
        WaitIdle();
        snprintf(cmd, sizeof(cmd), "kit rename %d second/kit", k + 1);
        U(cmd);
        int k2 = FindKit("second_kit");
        CHECK(k2 >= nb && FindKit("my first kit_") < 0 && num_kits == n0 + 1);
        CHECK(Exists("kits/second_kit/kit.txt") && !Exists("kits/my first kit_/kit.txt"));
        CHECK(loaded_kit_pg[0] == k2 && !strcmp(slot_path[1], "kits/second_kit/2 tone b.wav"));

        /* delete: the folder is gone, the sounds stay in the tracks */
        snprintf(cmd, sizeof(cmd), "kit delete %d", k2 + 1);
        U(cmd);
        CHECK(num_kits == n0 && FindKit("second_kit") < 0 && !Exists("kits/second_kit"));
        CHECK(loaded_kit_pg[0] == -1 && e.GetSlot(1).length > 0);

        /* a kit.txt with a broken line: that track loads with its sound only */
        mkdir((root + "/kits/hand made").c_str(), 0755);
        WriteSine("kits/hand made/1 kick.wav", 4800, 80.f);
        WriteSine("kits/hand made/2 snare.wav", 4800, 200.f);
        {
            FILE* f = fopen((root + "/kits/hand made/kit.txt").c_str(), "w");
            fprintf(f, "APK1\n1 s 2500 5000 0 0 5000 10000 5000 2000 0 0 0 10000 0\n2 s 9000 banana\n");
            fclose(f);
        }
        KitsRescan();
        int kh = FindKit("hand made");
        CHECK(kh >= nb);
        e.track[1].vol = 0.66f;
        snprintf(cmd, sizeof(cmd), "kit %d 0", kh + 1);
        U(cmd);
        WaitIdle();
        CHECK(fabsf(e.track[0].vol - 0.25f) < 0.001f && !strcmp(slot_path[0], "kits/hand made/1 kick.wav"));
        CHECK(fabsf(e.track[1].vol - 0.66f) < 0.001f && strcmp(slot_path[1], "kits/hand made/2 snare.wav"));
        snprintf(cmd, sizeof(cmd), "kit delete %d", kh + 1);
        U(cmd);
        CHECK(!Exists("kits/hand made"));

        for(int t = 0; t < 8; t++)
            e.track[t] = TrackParams();
        LoadKit(0, false, 0);
        WaitIdle();
        SelectTrack(0);
        printf("kits: save, settings, 303 state, undo, rename, delete ok\n");
    }

    /* ---------- the button map: any control can do any other's job, lights follow ---------- */
    {
        using namespace apc;
        auto U   = [](const char* a) {
            CHECK(BoxUiCommand(a, now_ms));
            Tick(8);
        };
        auto led = [](int status, int d1) {
            int      n;
            LedSlot* l = BoxLeds(&n);
            for(int i = 0; i < n; i++)
                if(l[i].status == status && l[i].d1 == d1)
                    return (int)l[i].want;
            return -1;
        };
        e.Stop();
        WaitIdle();
        if(track_page)
            TogglePage(); // strips = tracks 1-8
        Tick(30);
        CHECK(MapIsDefault());
        CHECK(CidOf(kMetronome, 3) == 109 && CidOf(kPlay, 0) == 91 && CidOf(kShift, 0) == kCidShift
              && CidOf(kClipFirst + 4, 0) == 32 && CidOf(kActivator, 2) == 63 && CidOf(kClipStop, 0) == 77);
        for(int c = 0; c < kMapBtns; c++)
        {
            int n, ch;
            CidNote(c, &n, &ch);
            CHECK(CidOf(n, ch) == c);
        }

        /* METRONOME does PLAY's job; with SHIFT it still does its own SHIFT job (monitor) */
        U("map b 109 0 91 0");
        const bool click = e.global.metronome;
        Tap(kMetronome);
        CHECK(e.Playing() && e.global.metronome == click);
        e.Stop();
        Tick(20);
        const bool mon = e.input_monitor;
        Btn(kShift, 0, true);
        Tap(kMetronome);
        Btn(kShift, 0, false);
        CHECK(e.input_monitor != mon && !e.Playing());
        e.input_monitor = mon;

        /* a pad of the bottom row mutes track 3: its light is the mute light, in a pad's colour */
        SelectTrack(0);
        e.track[2].mute = false;
        U("map b 32 0 63 0");
        Tick(20);
        CHECK(led(0x90, kClipFirst + 4) == kGreen); // channel 0, row 5: on = not muted
        uint32_t trig = e.TriggerCount(0);
        Tap(kClipFirst + 4, 0);
        CHECK(e.track[2].mute && e.TriggerCount(0) == trig); // no longer plays track 1
        Tick(20);
        CHECK(led(0x90, kClipFirst + 4) == kOff);
        Tap(kClipFirst + 4, 0);
        CHECK(!e.track[2].mute);

        /* the other way: a strip button does a pad's job and shows its colour as on / blink */
        U("map b 61 0 24 0"); // ACTIVATOR 1 = FX pad 1 (hold)
        Btn(kActivator, 0, true);
        CHECK(e.PerfFxActive(0) && !e.track[0].mute);
        Tick(20);
        CHECK(led(0x90, kActivator) != 0);
        Btn(kActivator, 0, false);
        CHECK(!e.PerfFxActive(0));

        /* SHIFT layer on its own: SHIFT + CLIP STOP 1 = the left arrow's undo (no SHIFT in it) */
        U("map b 77 1 104 0");
        U("lrec len 1");
        SelectTrack(3);
        U("lrec arm 4");
        int w = 0;
        while((e.LiveRecState() != 0 || !e.IsLoop(3)) && w++ < 5000)
            Tick();
        e.Stop();
        Tick(60);
        CHECK(e.IsLoop(3));
        Btn(kShift, 0, true);
        Tap(kClipStop, 0);
        Btn(kShift, 0, false);
        WaitIdle();
        CHECK(!e.IsLoop(3)); // undone, and track 1 wasn't cleared
        U("lrec len 0");
        SelectTrack(0);

        /* a held button lets go of what it started, whatever SHIFT does meanwhile */
        U("map b 108 0 27 0"); // MIDI OVERDUB = FX pad 4 (hold)
        Btn(kOverdub, 0, true);
        CHECK(e.PerfFxActive(3));
        Btn(kShift, 0, true);
        Btn(kOverdub, 0, false);
        Btn(kShift, 0, false);
        CHECK(!e.PerfFxActive(3) && e.LiveRecState() == 0);

        /* a function without a button: one-shot sampling on DETAIL VIEW; nothing on REC QUANTIZATION */
        U("map b 106 0 110 0");
        U("map b 107 1 255 0");
        Tap(kDetail);
        CHECK(e.LiveRecState() == 1 && e.LiveRecMode() == Engine::kRecShot);
        Tick(20);
        CHECK(led(0x90, kDetail) == 2); // waiting for sound: blinks
        Tap(kDetail);
        CHECK(e.LiveRecState() == 0);

        /* knobs: TRACK CONTROL 1 is fader 3; its ring shows that level */
        U("map c 0 18");
        e.track[2].vol = 0.5f;
        fader_live[2]  = true;
        BoxMidi(3, 0, kTrkKnobCC, 100, now_ms);
        Tick(20);
        CHECK(fabsf(e.track[2].vol - 100.f / 127.f) < 0.001f && fabsf(e.track[0].pan - 0.5f) < 0.001f);
        CHECK(led(0xB0, kTrkKnobCC) == 100 && led(0xB0, kTrkTypeCC) == kRingVolume);
        BoxMidi(3, 2, kFaderCC, 100, now_ms); // the fader itself still does its job
        CHECK(fabsf(e.track[2].vol - 100.f / 127.f) < 0.001f);

        /* what can't be mapped */
        CHECK(!BoxUiCommand("map b 98 0 91 0", now_ms) && !BoxUiCommand("map b 91 0 98 0", now_ms));
        CHECK(!BoxUiCommand("map b 110 0 91 0", now_ms) && !BoxUiCommand("map b 5 0 116 0", now_ms));
        CHECK(!BoxUiCommand("map c 26 3", now_ms) && !BoxUiCommand("map c 3 26", now_ms) && !BoxUiCommand("map c 27 0", now_ms));

        /* the pages hear about it; it goes onto the card and comes back */
        ui_lines.clear();
        map_report = true;
        Tick(60);
        int mb = 0, mc = 0;
        for(auto& l : ui_lines)
            mb += l.rfind("MB ", 0) == 0, mc += l.rfind("MC ", 0) == 0;
        CHECK(mb == 3 && mc == 1);
        Tick(1700);
        CHECK(!map_dirty && Exists("apcbox/apcmap.txt"));
        uint8_t was[kMapBtns][2], wasc[kMapCcs];
        memcpy(was, bmap, sizeof(was));
        memcpy(wasc, cmap, sizeof(wasc));
        MapDefault();
        CHECK(MapIsDefault());
        MapLoad();
        CHECK(!memcmp(was, bmap, sizeof(was)) && !memcmp(wasc, cmap, sizeof(wasc)));
        CHECK(bmap[109][0] == 91 && bmap[77][1] == 104 && bmap[107][1] == kMapNone && bmap[106][0] == 110 && cmap[0] == 18);

        /* back to what's printed on the buttons */
        U("map reset");
        CHECK(MapIsDefault());
        Tap(kMetronome);
        CHECK(e.global.metronome != click && !e.Playing());
        Tap(kMetronome);
        Tick(1700);
        MapLoad();
        CHECK(MapIsDefault());
        e.track[2].vol = 0.8f;
        printf("button map: buttons, SHIFT layer, lights, knobs, card ok\n");
    }

    /* A performance FX held from a page lets go when the T95 goes quiet,
       even with no controller attached */
    T95Frame("\x1bK\x1bU fx 3 1\n");
    T95Tick(1500);
    CHECK(e.PerfFxActive(2));       // keepalives still coming
    T95Tick(2200, false, false);    // the bridge died with the pad held
    CHECK(!e.PerfFxActive(2));

    /* ---- 16 s slots, spare blocks for the loops that don't fit one ---- */
    {
        auto U = [](const char* a) {
            CHECK(BoxUiCommand(a, now_ms));
            Tick(2);
        };
        auto until = [](const std::function<bool()>& f, int max_ms) {
            int n = 0;
            while(!f() && n++ < max_ms)
                Tick();
            return f();
        };
        auto idle = [] { return e.LiveRecState() == 0; };
        auto rec  = [] { return e.LiveRecState() == 2; };
        auto kit  = [](int t) {
            char p[8];
            snprintf(p, sizeof(p), "@k%d", BuiltInKitFor(t));
            e.ClearLoop(t);
            QueueLoad(t, p, false);
            WaitIdle();
            Tick(5);
        };
        auto fill = [] { // every free block taken by someone else
            for(int b = 0; b < spare_n; b++)
                if(spare_owner[b] < 0)
                {
                    spare_owner[b] = 99;
                    spare_held[b]  = true;
                }
        };
        auto unfill = [] {
            for(int b = 0; b < spare_n; b++)
                if(spare_owner[b] == 99)
                {
                    spare_owner[b] = -1;
                    spare_held[b]  = false;
                }
        };
        const uint32_t cap = e.SlotCapacity();
        const int      T   = 2; // track 3
        const Slot&    s   = e.GetSlot(T);
        e.Stop();
        WaitIdle();
        Tick(50);
        kit(T);
        kit(5);
        Tick(5);
        CHECK(cap == 768u * 1024u && spare_n == 8 && spare_block == 2 * cap && SpareFree() == 8);
        U("tempo 1000"); // a bar is 2400 ms: 8 bars are 19.2 s, more than a slot holds
        U("lrec count 0");
        U("quant rec 0");
        test_in_amp = 0.3f;

        /* 4 bars fit the slot: no block */
        U("lrec len 4");
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(SpareFree() == 8 && s.data == e.HomeData(T));
        U("lrec cancel");
        Tick(20);
        WaitIdle();
        Tick(5);
        const uint32_t kit_len = s.length;
        const int16_t  kit_a = s.data[100], kit_b = s.data[kit_len / 2];
        CHECK(kit_len > 0 && s.data == e.HomeData(T) && !e.IsLoop(T));

        /* 8 bars don't: the take gets a spare block, and the track's sound stays
           whole where it is. Dropped, the track is back on it and the block is free */
        U("lrec len 8");
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(SpareFree() == 7 && SpareOf(T) >= 0 && s.capacity == spare_block);
        Tick(3000);
        U("lrec cancel");
        Tick(5);
        CHECK(idle() && s.data == e.HomeData(T) && s.capacity == cap && s.length == kit_len);
        CHECK(s.data[100] == kit_a && s.data[kit_len / 2] == kit_b && !e.IsLoop(T));
        CHECK(SpareFree() == 8 && !LooperCan(false) && !IsRecPath(slot_path[T]));

        /* kept: a loop of 8 bars, longer than a slot, and no undo for it */
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(until(idle, 8 * 2400 + 200));
        Tick(40);
        CHECK(e.IsLoop(T) && e.LoopBars(T) == 8 && labs((long)s.length - 921600) < 8);
        const int      blk      = SpareOf(T);
        const uint32_t long_len = s.length;
        CHECK(blk >= 0 && SpareFree() == 7 && s.length > cap && s.data[cap + 5000] != 0);
        CHECK(!LooperCan(false) && IsRecPath(slot_path[T]));
        test_in_amp = 0.f;
        float pk = 0.f; // it plays past the 16 s mark
        Tick(17000);
        for(int i = 0; i < 2000; i++)
        {
            Tick();
            for(int k = 0; k < 48; k++)
                pk = fmaxf(pk, fabsf(outl[k]));
        }
        CHECK(pk > 0.01f);
        test_in_amp = 0.3f;

        /* a new long take over it: a second block while it runs, the loop untouched;
           dropped, the loop is back and the second block is free */
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(SpareFree() == 6 && SpareOf(T) >= 0 && SpareOf(T) != blk);
        Tick(1000);
        U("lrec cancel");
        Tick(5);
        CHECK(idle() && SpareOf(T) == blk && s.length == long_len && e.IsLoop(T) && SpareFree() == 7);

        /* overdub on a long loop: in place, and nothing to go back to */
        U("lrec len 1");
        U("lrec over 3");
        CHECK(until(rec, 2600));
        CHECK(until(idle, 2600));
        Tick(40);
        CHECK(SpareOf(T) == blk && s.length == long_len && e.IsLoop(T));
        CHECK(!LooperCan(false) && strstr(popup, "no undo")); // said when the overdub ends, and no undo offered
        BoxUiCommand("lrec undo", now_ms);
        Tick(40);
        CHECK(SpareOf(T) == blk && s.length == long_len && e.IsLoop(T) && !LooperCan(false) && !LooperCan(true));
        CHECK(!strcmp(popup, "overdub on long loop: no undo")); // not "take undone": nothing changed

        /* no block free: a chosen length that doesn't fit is refused before anything is armed */
        fill();
        CHECK(SpareFree() == 0);
        U("lrec len 8");
        const uint32_t gen6 = slot_gen[5], len6 = e.GetSlot(5).length;
        U("lrec arm 6");
        Tick(30);
        CHECK(!e.LiveRecBusy() && slot_gen[5] == gen6 && e.GetSlot(5).length == len6 && !e.IsLoop(5));
        /* with no length chosen the take goes into the slot, and can be undone */
        U("lrec len 0");
        U("lrec arm 6");
        CHECK(until(rec, 2600));
        CHECK(e.GetSlot(5).data == e.HomeData(5) && e.GetSlot(5).capacity == cap);
        Tick(2400 + 200);
        U("lrec stop");
        CHECK(until(idle, 2600));
        Tick(40);
        CHECK(e.IsLoop(5) && e.LoopBars(5) == 2 && SpareOf(5) < 0 && LooperCan(false));
        /* a long take over the track's own long loop goes in place then */
        U("lrec len 8");
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(SpareOf(T) == blk);
        U("lrec cancel");
        Tick(20);
        WaitIdle();
        Tick(5);
        CHECK(s.data == e.HomeData(T) && s.capacity == cap && !e.IsLoop(T));
        unfill();
        CHECK(SpareFree() == 8);

        /* no length chosen and a block free: the take gets the block, and moves
           into the slot afterwards because it fits. Undo works as for any take */
        U("lrec len 0");
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(SpareFree() == 7 && SpareOf(T) >= 0);
        Tick(2400 + 200);
        U("lrec stop");
        CHECK(until(idle, 2600));
        Tick(40);
        CHECK(e.IsLoop(T) && e.LoopBars(T) == 2 && labs((long)s.length - 2 * 115200) < 8);
        CHECK(s.data == e.HomeData(T) && s.capacity == cap && SpareFree() == 8 && LooperCan(false));
        U("lrec undo");
        WaitIdle();
        Tick(40);
        CHECK(!IsRecPath(slot_path[T]) && !e.IsLoop(T) && LooperCan(true));

        /* a long loop saved in a set comes back whole, in a block */
        U("lrec len 8");
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(until(idle, 8 * 2400 + 200));
        Tick(40);
        test_in_amp = 0.f;
        CHECK(SpareOf(T) >= 0 && e.LoopBars(T) == 8);
        std::vector<int16_t> take(s.data, s.data + s.length);
        const int            slot = FirstFreeSlot();
        CHECK(slot >= 0);
        U("set new long loop");
        WaitIdle();
        CHECK(set_exists[slot] && open_set == slot);
        kit(T);
        CHECK(SpareFree() == 8 && s.data == e.HomeData(T) && !e.IsLoop(T));
        std::vector<int16_t> next(e.HomeData(T + 1), e.HomeData(T + 1) + 4096); // the neighbour's memory
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "set load %d", slot + 1);
        U(cmd);
        WaitIdle();
        Tick(50);
        CHECK(e.IsLoop(T) && e.LoopBars(T) == 8 && s.length == take.size());
        CHECK(SpareOf(T) >= 0 && SpareFree() == 7 && s.capacity == spare_block);
        CHECK(s.length == take.size() && !memcmp(s.data, take.data(), take.size() * 2));
        CHECK(!memcmp(e.HomeData(T + 1), next.data(), next.size() * 2));
        /* the same, loaded straight into the track (as when the staging pool is full) */
        kit(T);
        CHECK(SpareFree() == 8);
        {
            Job* j = QueueJob(T, setf.file[T], false);
            CHECK(j != nullptr);
            if(j)
            {
                j->loop_bars = 8;
                j->loop_bpm  = 100.f;
            }
            WaitIdle();
            Tick(50);
            CHECK(SpareOf(T) >= 0 && SpareFree() == 7 && s.length == take.size() && e.IsLoop(T));
            CHECK(!memcmp(s.data, take.data(), take.size() * 2));
        }
        /* no block free when it loads: cut to the slot, nothing written past it */
        kit(T);
        fill();
        U(cmd);
        WaitIdle();
        Tick(50);
        CHECK(s.data == e.HomeData(T) && s.length == cap && s.capacity == cap && e.IsLoop(T));
        CHECK(!memcmp(s.data, take.data(), (size_t)cap * 2));
        CHECK(!memcmp(e.HomeData(T + 1), next.data(), next.size() * 2));
        kit(T);
        {
            Job* j = QueueJob(T, setf.file[T], false);
            if(j)
                j->loop_bars = 8, j->loop_bpm = 100.f;
            WaitIdle();
            Tick(50);
            CHECK(s.data == e.HomeData(T) && s.length == cap);
            CHECK(!memcmp(e.HomeData(T + 1), next.data(), next.size() * 2));
        }
        unfill();
        kit(T);
        CHECK(SpareFree() == 8);

        /* a plain sample longer than a slot is cut, loaded straight in or with a set */
        WriteSine("samples/test/verylong.wav", 900000, 330.f); // 18.75 s
        const int V = 8;
        kit(V);
        kit(V + 1);
        std::vector<int16_t> nextv(e.HomeData(V + 1), e.HomeData(V + 1) + 4096);
        QueueLoad(V, "samples/test/verylong.wav", false);
        WaitIdle();
        Tick(5);
        CHECK(e.GetSlot(V).length == cap && e.GetSlot(V).data == e.HomeData(V) && SpareFree() == 8);
        CHECK(!memcmp(e.HomeData(V + 1), nextv.data(), nextv.size() * 2));
        const int slot2 = FirstFreeSlot();
        U("set new long sample");
        WaitIdle();
        CHECK(slot2 >= 0 && set_exists[slot2]);
        kit(V);
        snprintf(cmd, sizeof(cmd), "set load %d", slot2 + 1);
        U(cmd);
        WaitIdle();
        Tick(50);
        CHECK(e.GetSlot(V).length == cap && e.GetSlot(V).data == e.HomeData(V) && SpareFree() == 8);
        CHECK(!memcmp(e.HomeData(V + 1), nextv.data(), nextv.size() * 2));

        /* a one-shot over a long loop goes into the slot; the loop's block is free once it's kept */
        U("lrec len 8");
        test_in_amp = 0.3f;
        U("lrec arm 3");
        CHECK(until(rec, 2600));
        CHECK(until(idle, 8 * 2400 + 200));
        Tick(40);
        CHECK(SpareOf(T) >= 0 && SpareFree() == 7);
        e.Stop();
        Tick(50);
        test_in_amp = 0.f;
        U("lrec shot 3");
        Tick(100);
        test_in_amp = 0.5f;
        Tick(300);
        CHECK(rec() && s.data == e.HomeData(T) && SpareFree() == 7);
        U("lrec stop");
        Tick(5);
        test_in_amp = 0.f;
        CHECK(idle() && !e.IsLoop(T) && s.data == e.HomeData(T) && s.length > 10000 && s.length < 20000);
        CHECK(SpareFree() == 8 && !LooperCan(false));

        kit(T);
        kit(5);
        kit(V);
        U("lrec len 0");
        U("tempo 1200");
        printf("long loops: spare blocks, refusal, cancel, sets, cut loads ok\n");
    }

    CHECK(e.nan_count == 0);
    printf("%s (%d failures), peak %.2f\n", fails ? "FAILED" : "all ok", fails, peak_last);
    return fails ? 1 : 0;
}
