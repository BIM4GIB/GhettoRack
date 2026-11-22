/** Desktop test for pattern copy/undo and sets (save, load in sync, transitions).
 *  Builds the real box.cpp into this file with a folder as the "SD card". */
#include "box.cpp"
#include <cassert>
#include <cmath>
#include <dirent.h>
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

class TestPlat : public Platform
{
  public:
    void Out(const char*) override {}
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

/* 1 ms of everything: audio block, main loop poll */
static void Tick(int n = 1)
{
    for(int i = 0; i < n; i++)
    {
        static float in[48] = {};
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

int main()
{
    char tmpl[] = "/tmp/apcsetsXXXXXX";
    root        = mkdtemp(tmpl);
    mkdir((root + "/samples").c_str(), 0755);
    mkdir((root + "/samples/test").c_str(), 0755);
    WriteSine("samples/test/tone a.wav", 24000, 440.f);
    WriteSine("samples/test/tone b.wav", 12000, 660.f);
    WriteSine("samples/test/long.wav", 600000, 220.f); // 12.5 s

    static std::vector<int16_t> slots(1536 * 1024 * (size_t)kTracks);
    static std::vector<float>   dl(96000), dr(96000), cl(192000), cr(192000);
    static daisysp::ReverbSc    rv;
    Memory                      mem;
    for(int t = 0; t < kTracks; t++)
        mem.slot_mem[t] = slots.data() + (size_t)t * 1536 * 1024;
    mem.slot_capacity = 1536 * 1024;
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

    CHECK(e.nan_count == 0);
    printf("%s (%d failures), peak %.2f\n", fails ? "FAILED" : "all ok", fails, peak_last);
    return fails ? 1 : 0;
}
