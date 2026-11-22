/** Sets: a whole project (patterns, 303 notes, mixer, every track's sound)
 *  saved to its own folder on the card, with its sounds copied in so it
 *  doesn't depend on the sample library:
 *
 *    sets/03/set.bin             header (name, bpm, counts) + everything else
 *    sets/03/05 kick.wav         track 5's sound (48 kHz mono 16-bit)
 *    sets/last.bin               which set to open at power-up
 *
 *  Loading plays on: the new set's sounds go into a staging pool while the
 *  old set keeps going, then the engine swaps at a bar line, after a bar of
 *  transition (cut, filter, tape stop, echo out). Sounds both sets share
 *  aren't loaded again. Included by box.cpp (uses its internals).
 */
#pragma once

/* Slots on the screen. More: raise this, the list scrolls. */
constexpr int kSetSlots   = 8;
constexpr int kSetNameLen = 16;

struct SetInfo
{
    char     magic[4]; // "APS1"
    uint32_t size;     // sizeof(SetFile)
    char     name[kSetNameLen + 4];
    float    bpm;
    uint8_t  samples, loops, synths, transition;
    uint32_t bytes; // sound data on the card
};

struct SetFile
{
    SetInfo      info;
    uint8_t      patterns[kPatterns][kTracks][kSteps];
    uint8_t      notes[kPatterns][kTracks][kSteps];
    TrackParams  tracks[kTracks];
    GlobalParams global;
    char         file[kTracks][kPathLen]; // what to load: "@k0", "@303", "sets/03/05 kick.wav"
    char         orig[kTracks][kPathLen]; // where it first came from
    uint8_t      loop_bars[kTracks];
    float        loop_bpm[kTracks];
    int32_t      pattern;
};
alignas(32) SetFile setf;
SetSwap             set_swap;

/* What's on the card */
SetInfo set_list[kSetSlots];
bool    set_exists[kSetSlots] = {};
bool    set_list_ok           = false;

/* The open set */
int      open_set = -1;
char     open_name[kSetNameLen + 4] = "";
char     set_file[kTracks][kPathLen]; // each track's file in the open set
uint32_t set_gen[kTracks];            // slot_gen when it matched that file
uint32_t saved_hash = 0, cur_hash = 0;
bool     set_dirty  = false;
uint32_t hash_time  = 0;
uint8_t  set_trans  = kTransFilter; // remembered across power cycles

/* ---- screen state ---- */
enum SetsMode
{
    kSetsList,
    kSetsActions,
    kSetsConfirm,
    kSetsRename
};
enum SetsAct
{
    kActLoad,
    kActSaveHere,
    kActSaveNew,
    kActRename,
    kActDelete,
    kActTrans,
    kNumActs
};
const char* const kActNames[kNumActs] = {"LOAD", "SAVE HERE", "SAVE NEW", "RENAME", "DELETE", "FX"};
const char* const kTransNames[kNumTransitions] = {"CUT", "FILTER", "TAPE STOP", "ECHO OUT"};

bool     sets_open    = false;
int      sets_mode    = kSetsList;
int      sets_cur     = 0;
int      sets_scroll  = 0;
int      sets_act     = kActLoad;
int      confirm_act  = -1;
int      confirm_yes  = 0;
char     rename_buf[kSetNameLen + 1];
int      rename_pos   = 0;
int      apc_confirm  = -1; // APC40 fallback: pad waiting for a second press
uint32_t apc_confirm_at = 0;

/* ---- progress ---- */
uint32_t op_done = 0, op_total = 1;
char     op_msg[40] = "";

/* ================================================================== */

void SetDir(int slot, char* buf, size_t n) { snprintf(buf, n, "sets/%02d", slot + 1); }

void ReadSetList()
{
    set_list_ok = sd_ok;
    for(int i = 0; i < kSetSlots; i++)
    {
        char p[40];
        snprintf(p, sizeof(p), "sets/%02d/set.bin", i + 1);
        SetInfo& in   = set_list[i];
        int32_t  n    = sd_ok ? plat->ReadFile(p, &in, sizeof(in)) : -1;
        set_exists[i] = n == (int32_t)sizeof(in) && !memcmp(in.magic, "APS1", 4)
                        && in.size == sizeof(SetFile);
        if(set_exists[i])
            in.name[kSetNameLen] = 0;
    }
}

uint32_t ProjectHash()
{
    uint32_t h   = 2166136261u;
    auto     mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for(size_t i = 0; i < n; i++)
            h = (h ^ b[i]) * 16777619u;
    };
    mix(eng->PatternData(), sizeof(uint8_t) * kPatterns * kTracks * kSteps);
    mix(eng->NoteData(), sizeof(uint8_t) * kPatterns * kTracks * kSteps);
    for(int t = 0; t < kTracks; t++)
    {
        const TrackParams& p = eng->track[t];
        /* field by field: padding bytes would make it noisy */
        float f[12] = {p.vol, p.pan, p.rev, p.dly, p.pitch, p.decay,
                       p.filter, p.reso, p.start, p.reverse, p.drive, p.prob};
        mix(f, sizeof(f));
        uint8_t b[3] = {p.mute, p.solo, p.xfade};
        mix(b, 3);
        float a[8] = {p.acid.cutoff, p.acid.reso, p.acid.envmod, p.acid.decay,
                      p.acid.accent, p.acid.tune, p.acid.slide, p.acid.wave};
        mix(a, sizeof(a));
        mix(&slot_gen[t], 4);
        mix(slot_path[t], strlen(slot_path[t]));
    }
    const GlobalParams& g = eng->global;
    float gf[11] = {g.bpm, g.swing, g.rev_size, g.rev_tone, g.dly_time, g.dly_fb,
                    g.drive, g.fx_return, g.master, g.xfader, g.djfilter};
    mix(gf, sizeof(gf));
    return h;
}

void MarkSaved()
{
    saved_hash = cur_hash = ProjectHash();
    set_dirty  = false;
}

void WriteLast()
{
    uint8_t b[4] = {(uint8_t)(open_set + 1), set_trans, 0, 0};
    plat->WriteFile("sets", "sets/last.bin", b, 4);
}

/** File-name-safe copy of a sound's name */
void SafeName(char* dst, size_t n, const char* src)
{
    const char* b = strrchr(src, '/');
    b             = b ? b + 1 : src;
    size_t k      = 0;
    for(; *b && k + 1 < n; b++)
    {
        char c = *b;
        if(c == '.')
            break;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == ' ' || c == '-' || c == '_' || c == '#' || c == '+';
        dst[k++] = ok ? c : '_';
    }
    dst[k] = 0;
    if(k == 0)
        snprintf(dst, n, "sound");
}

/* ================================================================== */
/* Saving                                                              */

int      save_slot = -1;
int      save_t    = 0;     // track being written
bool     save_wav[kTracks]; // needs its sound written
bool     save_open = false; // a wav is open
uint32_t save_pos  = 0;     // samples written of it
int      save_phase = 0;    // 0 wait for loader, 1 sounds, 2 set.bin, 3 tidy
bool     save_fail  = false;

char save_name[kSetNameLen + 4];

void SaveSetBegin(int slot, const char* name)
{
    if(!sd_ok || set_op)
        return;
    snprintf(save_name, sizeof(save_name), "%s", name);
    save_slot  = slot;
    save_phase = 0; // builds once the loader is idle
    set_op     = 1;
    op_done    = 0;
    op_total   = 1;
    snprintf(op_msg, sizeof(op_msg), "saving %02d %s", slot + 1, name);
    Log("%s", op_msg);
}

/** Everything into setf, and which sounds need writing */
void SaveBuild()
{
    const int   slot = save_slot;
    const char* name = save_name;
    SetFile&    f    = setf;
    memset(&f.info, 0, sizeof(f.info));
    memcpy(f.info.magic, "APS1", 4);
    f.info.size = sizeof(SetFile);
    snprintf(f.info.name, sizeof(f.info.name), "%s", name);
    f.info.bpm        = eng->global.bpm;
    f.info.transition = set_trans;
    memcpy(f.patterns, eng->PatternData(), sizeof(f.patterns));
    memcpy(f.notes, eng->NoteData(), sizeof(f.notes));
    f.global  = eng->global;
    f.pattern = eng->QueuedPattern() >= 0 ? eng->QueuedPattern() : eng->CurrentPattern();
    char dir[24];
    SetDir(slot, dir, sizeof(dir));
    op_total = 1;
    op_done  = 0;
    for(int t = 0; t < kTracks; t++)
    {
        f.tracks[t]    = eng->track[t];
        f.loop_bars[t] = 0;
        f.loop_bpm[t]  = 0.f;
        save_wav[t]    = false;
        const char* sp = slot_path[t];
        snprintf(f.orig[t], kPathLen, "%s", slot_orig[t][0] ? slot_orig[t] : sp);
        if(eng->IsSynth(t) || IsSynthPath(sp))
        {
            snprintf(f.file[t], kPathLen, "%s", kSynthPath);
            f.info.synths++;
            continue;
        }
        if(BuiltInKitOf(sp) >= 0 && !eng->IsLoop(t))
        {
            snprintf(f.file[t], kPathLen, "%s", sp[0] ? sp : "@k0");
            continue;
        }
        const Slot& s = eng->GetSlot(t);
        if(s.length == 0)
        {
            snprintf(f.file[t], kPathLen, "@k%d", BuiltInKitFor(t));
            continue;
        }
        if(eng->IsLoop(t))
        {
            f.loop_bars[t] = (uint8_t)eng->LoopBars(t);
            f.loop_bpm[t]  = eng->LoopBpm(t);
            f.info.loops++;
        }
        else
            f.info.samples++;
        f.info.bytes += s.length * 2 + 44;
        /* the same sound already in this set: keep its file */
        if(slot == open_set && set_file[t][0] && set_gen[t] == slot_gen[t])
        {
            snprintf(f.file[t], kPathLen, "%s", set_file[t]);
            continue;
        }
        char nm[40];
        if(eng->IsLoop(t) || IsRecPath(f.orig[t]))
            snprintf(nm, sizeof(nm), "loop");
        else
            SafeName(nm, sizeof(nm), f.orig[t]);
        snprintf(f.file[t], kPathLen, "%s/%02d %s.wav", dir, t + 1, nm);
        save_wav[t] = true;
        op_total += s.length;
    }
    save_t    = 0;
    save_open = false;
    save_pos  = 0;
    save_fail = false;
}

void PutU32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/** One chunk of the save per call */
void SaveStep()
{
    uint32_t scratch_size = 0;
    uint8_t* scratch      = plat->Scratch(&scratch_size);
    char     dir[24];
    SetDir(save_slot, dir, sizeof(dir));
    switch(save_phase)
    {
        case 0: // the loader shares the scratch buffer and the card
            if(num_jobs == 0 && load_state == kLoadIdle)
            {
                SaveBuild();
                save_phase = 1;
            }
            return;
        case 1:
        {
            while(save_t < kTracks && !save_wav[save_t])
                save_t++;
            if(save_t >= kTracks)
            {
                save_phase = 2;
                return;
            }
            const Slot& s = eng->GetSlot(save_t);
            if(!save_open)
            {
                if(!plat->FileCreate(dir, setf.file[save_t]))
                {
                    Log("can't write %s", setf.file[save_t]);
                    save_fail = true;
                    save_phase = 3;
                    return;
                }
                uint32_t rate = (uint32_t)(s.rate * eng->SampleRate() + 0.5f);
                uint32_t data = s.length * 2;
                uint8_t* h    = scratch;
                memcpy(h, "RIFF", 4);
                PutU32(h + 4, 36 + data);
                memcpy(h + 8, "WAVEfmt ", 8);
                PutU32(h + 16, 16);
                h[20] = 1, h[21] = 0; // PCM
                h[22] = 1, h[23] = 0; // mono
                PutU32(h + 24, rate);
                PutU32(h + 28, rate * 2);
                h[32] = 2, h[33] = 0;
                h[34] = 16, h[35] = 0;
                memcpy(h + 36, "data", 4);
                PutU32(h + 40, data);
                save_open = plat->FileWrite(h, 44) == 44;
                save_pos  = 0;
                if(!save_open)
                {
                    plat->FileWriteClose();
                    save_fail  = true;
                    save_phase = 3;
                }
                return;
            }
            uint32_t n = s.length - save_pos;
            if(n > scratch_size / 2)
                n = scratch_size / 2;
            if(n)
            {
                memcpy(scratch, s.data + save_pos, n * 2);
                if(plat->FileWrite(scratch, n * 2) != (int32_t)(n * 2))
                {
                    plat->FileWriteClose();
                    Log("write failed: %s", setf.file[save_t]);
                    save_fail  = true;
                    save_phase = 3;
                    return;
                }
                save_pos += n;
                op_done += n;
            }
            if(save_pos >= s.length)
            {
                plat->FileWriteClose();
                save_open = false;
                save_t++;
            }
            return;
        }
        case 2:
        {
            char p[40];
            snprintf(p, sizeof(p), "%s/set.bin", dir);
            /* FileCreate makes "sets" too, if no sound file did */
            bool ok = plat->FileCreate(dir, p);
            ok      = ok && plat->FileWrite(&setf, sizeof(setf)) == (int32_t)sizeof(setf);
            ok      = plat->FileWriteClose() && ok;
            if(!ok)
            {
                Log("can't write %s", p);
                save_fail = true;
            }
            save_phase = 3;
            return;
        }
        case 3:
        {
            if(!save_fail)
            {
                /* tidy: sounds the set no longer uses */
                char gone[8][48];
                int  ng = 0;
                if(plat->DirOpen(dir))
                {
                    char name[48];
                    bool is_dir;
                    while(ng < 8 && plat->DirNext(name, sizeof(name), &is_dir))
                    {
                        size_t L = strlen(name);
                        if(is_dir || L < 5 || strcasecmp(name + L - 4, ".wav"))
                            continue;
                        char full[kPathLen];
                        snprintf(full, sizeof(full), "%s/%s", dir, name);
                        bool used = false;
                        for(int t = 0; t < kTracks && !used; t++)
                            used = !strcmp(setf.file[t], full);
                        if(!used)
                            snprintf(gone[ng++], sizeof(gone[0]), "%s", name);
                    }
                    plat->DirClose();
                }
                for(int i = 0; i < ng; i++)
                {
                    char full[kPathLen];
                    snprintf(full, sizeof(full), "%s/%s", dir, gone[i]);
                    plat->Remove(full);
                }
                open_set = save_slot;
                snprintf(open_name, sizeof(open_name), "%s", setf.info.name);
                for(int t = 0; t < kTracks; t++)
                {
                    bool wav = setf.file[t][0] != '@';
                    snprintf(set_file[t], kPathLen, "%s", wav ? setf.file[t] : "");
                    set_gen[t] = slot_gen[t];
                }
                MarkSaved();
                WriteLast();
                Popup("saved %02d %s", save_slot + 1, setf.info.name);
                Log("set %02d saved (%u KB of sound)", save_slot + 1,
                    (unsigned)(setf.info.bytes / 1024));
            }
            else
                Popup("SAVE FAILED");
            set_op = 0;
            ReadSetList();
            return;
        }
    }
}

/* ================================================================== */
/* Loading                                                             */

int      load_slot   = -1;
int      load_phase  = 0;   // 1 staging, 2 waiting for the bar line, 3 copying home
int      stage_jobs  = 0;   // staging jobs still out
uint32_t stage_used  = 0;
int16_t* stage_mem_p = nullptr;
uint32_t stage_cap   = 0;
bool     deferred[kTracks];
int      copy_t      = 0;
uint32_t copy_pos    = 0;

void SetsJobDone(const Job& j, bool ok, uint32_t len, float rate, bool defer)
{
    int t = j.track;
    stage_jobs--;
    if(defer)
    {
        deferred[t] = true; // loads into the track after the switch
        set_swap.data[t] = nullptr;
        return;
    }
    if(!ok)
    {
        if(!IsSynthPath(j.path))
            Log("set: track %d sound missing (%s), built-in instead", t + 1, j.path);
        set_swap.data[t] = nullptr;
        if(!IsSynthPath(j.path)) // fall back to the built-in sound, after the switch
        {
            snprintf(setf.file[t], kPathLen, "@k%d", BuiltInKitFor(t));
            deferred[t] = true;
        }
        return;
    }
    set_swap.data[t]   = j.dst;
    set_swap.length[t] = len;
    set_swap.rate[t]   = rate;
    if(len) // give back the unused part of its reservation
        stage_used = (uint32_t)(j.dst - stage_mem_p) + ((len + 15) & ~15u);
    op_done++;
}

/** A sound that loaded straight into its track after the switch */
void SetsTrackLoaded(int t)
{
    set_gen[t] = slot_gen[t];
    MarkSaved();
}

void LoadSetBegin(int slot)
{
    if(!sd_ok || set_op)
        return;
    if(XferBusy()) // the staging pool holds a sample from the T95
    {
        Popup("busy receiving a sample");
        return;
    }
    char p[40];
    snprintf(p, sizeof(p), "sets/%02d/set.bin", slot + 1);
    int32_t n = plat->ReadFile(p, &setf, sizeof(setf));
    if(n != (int32_t)sizeof(setf) || memcmp(setf.info.magic, "APS1", 4)
       || setf.info.size != sizeof(SetFile))
    {
        Popup("set %02d unreadable", slot + 1);
        Log("set %02d doesn't match this firmware", slot + 1);
        return;
    }
    setf.info.name[kSetNameLen] = 0;
    SetSwap& sw   = set_swap;
    sw.patterns   = &setf.patterns[0][0][0];
    sw.notes      = &setf.notes[0][0][0];
    sw.global     = setf.global;
    sw.pattern    = setf.pattern >= 0 && setf.pattern < kPatterns ? setf.pattern : 0;
    sw.transition = set_trans;
    stage_mem_p   = plat->Staging(&stage_cap);
    stage_used    = 0;
    stage_jobs    = 0;
    op_done       = 0;
    op_total      = 1;
    /* the loader runs these one by one; each reserves what's left of the
       pool and gives back what it didn't use */
    for(int t = 0; t < kTracks; t++)
    {
        setf.file[t][kPathLen - 1] = setf.orig[t][kPathLen - 1] = 0;
        sw.tracks[t]    = setf.tracks[t];
        sw.loop_bars[t] = setf.loop_bars[t];
        sw.loop_bpm[t]  = setf.loop_bpm[t];
        sw.synth[t]     = IsSynthPath(setf.file[t]);
        sw.replace[t]   = false;
        sw.data[t]      = nullptr;
        sw.length[t]    = 0;
        sw.rate[t]      = 1.f;
        deferred[t]     = false;
        if(sw.synth[t])
            continue;
        const char* f = setf.file[t];
        const char* o = setf.orig[t];
        bool        same = !strcmp(f, slot_path[t])
                    || (o[0] && !IsRecPath(o) && !strcmp(o, slot_orig[t]) && !eng->IsLoop(t));
        if(same && eng->GetSlot(t).length > 0 && !eng->IsSynth(t) && !TrackLoading(t)
           && (setf.loop_bars[t] == 0) == !eng->IsLoop(t))
            continue; // already playing it
        sw.replace[t] = true;
        op_total++;
    }
    /* stage in track order; built-ins are small, so do them first */
    for(int pass = 0; pass < 2; pass++)
        for(int t = 0; t < kTracks; t++)
        {
            if(!sw.replace[t] || (BuiltInKitOf(setf.file[t]) >= 0) != (pass == 0))
                continue;
            Job* j = QueueJob(t, setf.file[t], false);
            if(!j)
            {
                deferred[t] = true;
                continue;
            }
            j->stage = true;
            j->dst   = stage_mem_p; // fixed up when it starts
            stage_jobs++;
        }
    load_slot  = slot;
    load_phase = 1;
    set_op     = 2;
    snprintf(op_msg, sizeof(op_msg), "loading %02d %s", slot + 1, setf.info.name);
    Log("%s", op_msg);
}

/** Before a staging job starts: point it at the free part of the pool */
void StagePlace(Job& j)
{
    if(!stage_mem_p || stage_used + 64 >= stage_cap)
    {
        j.dst = stage_mem_p;
        j.cap = 0; // nothing left: SetsJobDone defers it
        return;
    }
    j.dst = stage_mem_p + stage_used;
    j.cap = stage_cap - stage_used;
}

void AfterSwap()
{
    const SetSwap& sw = set_swap;
    for(int t = 0; t < kTracks; t++)
    {
        if(sw.synth[t] || sw.replace[t])
        {
            snprintf(slot_path[t], kPathLen, "%s", setf.file[t]);
            snprintf(slot_orig[t], kPathLen, "%s", setf.orig[t][0] ? setf.orig[t] : setf.file[t]);
            if(sw.replace[t])
                slot_gen[t]++;
        }
        bool wav = setf.file[t][0] != '@';
        snprintf(set_file[t], kPathLen, "%s", wav ? setf.file[t] : "");
        set_gen[t] = slot_gen[t];
        roll_held[t] = false;
        eng->SetRoll(t, false);
    }
    open_set = load_slot;
    snprintf(open_name, sizeof(open_name), "%s", setf.info.name);
    loaded_kit_pg[0] = loaded_kit_pg[1] = -1;
    view_bank        = sw.pattern / 8;
    for(int c = 0; c < 8; c++)
        fader_live[c] = false; // pick up: faders don't jump the new levels
    ClearUndo();
    ReleaseKeys();
    WriteLast();
    MarkSaved();
    Popup("set %02d %s", load_slot + 1, setf.info.name);
    Log("set %02d in", load_slot + 1);
    copy_t   = 0;
    copy_pos = 0;
}

void LoadStep()
{
    switch(load_phase)
    {
        case 1:
            if(stage_jobs > 0)
                return;
            load_phase = 2;
            if(plat->AudioRunning())
                eng->ArmSwap(&set_swap);
            else
                eng->ApplySwapNow(&set_swap);
            snprintf(op_msg, sizeof(op_msg), "%02d %s at the next bar", load_slot + 1,
                     setf.info.name);
            return;
        case 2:
            if(!eng->TakeSwapDone())
                return;
            AfterSwap();
            load_phase = 3;
            return;
        case 3:
        {
            /* staged sounds back into their own slots, a piece at a time */
            while(copy_t < kTracks && !set_swap.data[copy_t])
                copy_t++;
            if(copy_t < kTracks)
            {
                int      t   = copy_t;
                uint32_t len = set_swap.length[t];
                uint32_t n   = len - copy_pos;
                if(n > 128 * 1024)
                    n = 128 * 1024;
                memcpy(eng->HomeData(t) + copy_pos, set_swap.data[t] + copy_pos, n * 2);
                copy_pos += n;
                if(copy_pos >= len)
                {
                    eng->SlotHome(t, len);
                    copy_t++;
                    copy_pos = 0;
                }
                return;
            }
            set_op = 0;
            /* what didn't fit the staging pool loads now, straight in */
            for(int t = 0; t < kTracks; t++)
                if(deferred[t])
                {
                    Job* j = QueueJob(t, setf.file[t], false);
                    if(j)
                    {
                        j->from_set  = true;
                        j->loop_bars = setf.loop_bars[t];
                        j->loop_bpm  = setf.loop_bpm[t];
                    }
                }
            load_phase = 0;
            return;
        }
    }
}

/* ================================================================== */
/* Screen and controls                                                 */

void DefaultName(int slot, char* buf, size_t n)
{
    (void)slot; // the list shows the number already
    snprintf(buf, n, "%dbpm", (int)(eng->global.bpm + 0.5f));
}

int FirstFreeSlot()
{
    for(int i = 0; i < kSetSlots; i++)
        if(!set_exists[i])
            return i;
    return -1;
}

void SetsOpen(bool on)
{
    sets_open = on;
    sets_mode = kSetsList;
    apc_confirm = -1;
    if(on)
    {
        ReadSetList();
        if(open_set >= 0)
            sets_cur = open_set;
    }
}

bool NeedsConfirm(int act)
{
    switch(act)
    {
        case kActLoad: return set_dirty;
        case kActSaveHere: return set_exists[sets_cur] && sets_cur != open_set;
        case kActDelete: return true;
        default: return false;
    }
}

const char* ConfirmText(int act, char* buf, size_t n)
{
    switch(act)
    {
        case kActLoad: snprintf(buf, n, "unsaved changes. load %02d anyway?", sets_cur + 1); break;
        case kActSaveHere:
            snprintf(buf, n, "overwrite %02d %s?", sets_cur + 1, set_list[sets_cur].name);
            break;
        case kActDelete:
            snprintf(buf, n, "delete %02d %s?", sets_cur + 1, set_list[sets_cur].name);
            break;
        default: buf[0] = 0;
    }
    return buf;
}

void DeleteSet(int slot)
{
    char dir[24];
    SetDir(slot, dir, sizeof(dir));
    char names[24][48];
    int  n = 0;
    if(plat->DirOpen(dir))
    {
        char nm[48];
        bool is_dir;
        while(n < 24 && plat->DirNext(nm, sizeof(nm), &is_dir))
            if(!is_dir && nm[0])
                snprintf(names[n++], sizeof(names[0]), "%s", nm);
        plat->DirClose();
    }
    for(int i = 0; i < n; i++)
    {
        char p[kPathLen];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        plat->Remove(p);
    }
    plat->Remove(dir);
    if(open_set == slot)
    {
        open_set = -1;
        open_name[0] = 0;
        for(int t = 0; t < kTracks; t++)
            set_file[t][0] = 0;
        WriteLast();
    }
    Popup("deleted %02d", slot + 1);
    Log("set %02d deleted", slot + 1);
    ReadSetList();
}

void RenameSet(int slot, const char* name)
{
    char p[40];
    snprintf(p, sizeof(p), "sets/%02d/set.bin", slot + 1);
    int32_t n = plat->ReadFile(p, &setf, sizeof(setf));
    if(n != (int32_t)sizeof(setf) || memcmp(setf.info.magic, "APS1", 4))
    {
        Popup("rename failed");
        return;
    }
    snprintf(setf.info.name, sizeof(setf.info.name), "%s", name);
    char dir[24];
    SetDir(slot, dir, sizeof(dir));
    bool ok = plat->WriteFile(dir, p, &setf, sizeof(setf));
    if(ok && slot == open_set)
        snprintf(open_name, sizeof(open_name), "%s", name);
    Popup(ok ? "renamed" : "rename failed");
    ReadSetList();
}

/** Does an action (confirmed, if it needed it) */
void DoAct(int act)
{
    if(set_op)
    {
        Popup("busy");
        return;
    }
    if(!sd_ok)
    {
        Popup("no SD card");
        return;
    }
    char name[kSetNameLen + 4];
    switch(act)
    {
        case kActLoad:
            if(!set_exists[sets_cur])
            {
                Popup("%02d is empty", sets_cur + 1);
                return;
            }
            LoadSetBegin(sets_cur);
            break;
        case kActSaveHere:
            if(set_exists[sets_cur])
                snprintf(name, sizeof(name), "%s", set_list[sets_cur].name);
            else if(open_set >= 0 && open_name[0])
                snprintf(name, sizeof(name), "%s", open_name);
            else
                DefaultName(sets_cur, name, sizeof(name));
            SaveSetBegin(sets_cur, name);
            break;
        case kActSaveNew:
        {
            int s = FirstFreeSlot();
            if(s < 0)
            {
                Popup("no free slot");
                return;
            }
            sets_cur = s;
            DefaultName(s, name, sizeof(name));
            SaveSetBegin(s, name);
            break;
        }
        case kActRename:
            if(!set_exists[sets_cur])
            {
                Popup("%02d is empty", sets_cur + 1);
                return;
            }
            snprintf(rename_buf, sizeof(rename_buf), "%-*s", kSetNameLen, set_list[sets_cur].name);
            rename_pos = 0;
            sets_mode  = kSetsRename;
            return;
        case kActDelete:
            if(!set_exists[sets_cur])
                return;
            DeleteSet(sets_cur);
            break;
        case kActTrans:
            set_trans = (uint8_t)((set_trans + 1) % kNumTransitions);
            Popup("transition %s", kTransNames[set_trans]);
            WriteLast();
            return;
    }
    sets_mode = kSetsList;
}

/* Rename: turn picks the letter, push moves on, back is done */
const char kNameChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_#+abcdefghijklmnopqrstuvwxyz";

void RenameTurn(int d)
{
    const int n  = (int)sizeof(kNameChars) - 1;
    const char* p = strchr(kNameChars, rename_buf[rename_pos]);
    int i = p ? (int)(p - kNameChars) : 0;
    i     = ((i + d) % n + n) % n;
    rename_buf[rename_pos] = kNameChars[i];
}

void RenameDone()
{
    char nm[kSetNameLen + 1];
    snprintf(nm, sizeof(nm), "%s", rename_buf);
    int L = (int)strlen(nm);
    while(L > 0 && nm[L - 1] == ' ')
        nm[--L] = 0;
    if(L == 0)
        DefaultName(sets_cur, nm, sizeof(nm));
    RenameSet(sets_cur, nm);
    sets_mode = kSetsList;
}

void SetsTurn(int d)
{
    switch(sets_mode)
    {
        case kSetsList:
            sets_cur = (sets_cur + d) % kSetSlots;
            if(sets_cur < 0)
                sets_cur += kSetSlots;
            break;
        case kSetsActions:
            sets_act = ((sets_act + d) % kNumActs + kNumActs) % kNumActs;
            break;
        case kSetsConfirm: confirm_yes = d > 0 ? 1 : 0; break;
        case kSetsRename: RenameTurn(d); break;
    }
}

void SetsPush()
{
    switch(sets_mode)
    {
        case kSetsList: sets_mode = kSetsActions; break;
        case kSetsActions:
            if(NeedsConfirm(sets_act))
            {
                confirm_act = sets_act;
                confirm_yes = 0;
                sets_mode   = kSetsConfirm;
            }
            else
                DoAct(sets_act);
            break;
        case kSetsConfirm:
            sets_mode = kSetsActions;
            if(confirm_yes)
                DoAct(confirm_act);
            break;
        case kSetsRename:
            if(++rename_pos >= kSetNameLen)
                RenameDone();
            break;
    }
}

void SetsBack()
{
    switch(sets_mode)
    {
        case kSetsList: SetsOpen(false); break;
        case kSetsActions: sets_mode = kSetsList; break;
        case kSetsConfirm: sets_mode = kSetsActions; break;
        case kSetsRename: RenameDone(); break;
    }
}

/* APC40 without the screen: row 3 picks the slot, row 4 pads 1-5 are
   LOAD, SAVE HERE, SAVE NEW, DELETE, FX; a second press confirms */
const int kApcActs[5] = {kActLoad, kActSaveHere, kActSaveNew, kActDelete, kActTrans};

void SetsApcPad(int row, int col)
{
    if(row == 2)
    {
        sets_cur = col % kSetSlots;
        sets_mode = kSetsList;
        apc_confirm = -1;
        if(set_exists[sets_cur])
            Popup("%02d %s", sets_cur + 1, set_list[sets_cur].name);
        else
            Popup("%02d empty", sets_cur + 1);
        return;
    }
    if(col >= 5)
        return;
    int act = kApcActs[col];
    if(NeedsConfirm(act) && !(apc_confirm == col && g_now - apc_confirm_at < 3000))
    {
        apc_confirm    = col;
        apc_confirm_at = g_now;
        char buf[48];
        Popup("again: %s", ConfirmText(act, buf, sizeof(buf)));
        return;
    }
    apc_confirm = -1;
    if(act == kActRename)
        return;
    DoAct(act);
}

/** Main loop */
void SetsPoll(uint32_t now)
{
    if(set_op == 1)
        SaveStep();
    else if(set_op == 2 || load_phase)
        LoadStep();
    if(!set_op && now - hash_time >= 500)
    {
        hash_time = now;
        cur_hash  = ProjectHash();
        set_dirty = cur_hash != saved_hash;
    }
}

/** At power-up: the last open set, if there is one */
bool LoadLastSet()
{
    uint8_t b[4] = {};
    if(plat->ReadFile("sets/last.bin", b, 4) != 4)
        return false;
    if(b[1] < kNumTransitions)
        set_trans = b[1];
    int slot = (int)b[0] - 1;
    ReadSetList();
    if(slot < 0 || slot >= kSetSlots || !set_exists[slot])
        return false;
    LoadSetBegin(slot);
    return set_op == 2;
}
