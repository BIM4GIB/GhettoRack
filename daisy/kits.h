/** Kits made on the box. Included by box.cpp, inside its namespace, after
 *  sets.h.
 *
 *  A kit is a folder on the card: kits/<name>/ with the sounds of the 8
 *  tracks of a page ("1 kick.wav" .. "8 perc.wav": the leading digit is the
 *  track) and, from v13.14, kit.txt with each track's settings:
 *
 *      APK1
 *      1 s vol pan rev dly pitch decay filter reso start reverse drive prob xfade
 *      3 a (the same 13) cutoff reso envmod decay accent tune slide wave
 *
 *  One line per track that is in the kit, numbers x10000 (xfade as it is).
 *  s = a sample track, a = a 303 with those knobs (no sound file, no steps).
 *  A track that held a live loop when the kit was saved has no line: loading
 *  the kit leaves that track alone. A kit without kit.txt loads as before.
 */

constexpr int kKitNameLen = 40;
constexpr int kKitFields  = 13;

void KitLoadedNames(char names[3][kNameLen])
{
    const int idx[3] = {loaded_kit_pg[0], loaded_kit_pg[1], loading_kit};
    for(int i = 0; i < 3; i++)
        snprintf(names[i], kNameLen, "%s", idx[i] >= 0 && idx[i] < num_kits ? kit_names[idx[i]] : "");
}

int FindKit(const char* name)
{
    for(int k = 0; k < num_kits; k++)
        if(!strcasecmp(kit_names[k], name)) // the card doesn't tell cases apart
            return k;
    return -1;
}

/** The list again (it is sorted, so the numbers move): what's loaded keeps pointing at its kit */
void KitsRescan()
{
    char was[3][kNameLen];
    KitLoadedNames(was);
    ScanKits();
    loaded_kit_pg[0] = was[0][0] ? FindKit(was[0]) : -1;
    loaded_kit_pg[1] = was[1][0] ? FindKit(was[1]) : -1;
    loading_kit      = was[2][0] ? FindKit(was[2]) : -1;
    if(kit_page * 5 >= num_kits)
        kit_page = 0;
    KitListChanged();
}

/** A name the card takes: letters, digits, space - _ # +, no spaces at the ends */
void KitName(char* dst, const char* src)
{
    while(*src == ' ')
        src++;
    size_t k = 0;
    for(; *src && k < (size_t)kKitNameLen; src++)
    {
        char c  = *src;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '
                  || c == '-' || c == '_' || c == '#' || c == '+';
        dst[k++] = ok ? c : '_';
    }
    while(k && dst[k - 1] == ' ')
        k--;
    dst[k] = 0;
}

/* ------------------------------------------------------------------ */
/* Settings file                                                       */

float* KitField(TrackParams& p, int i)
{
    float* f[kKitFields - 1] = {&p.vol, &p.pan, &p.rev, &p.dly, &p.pitch, &p.decay,
                                &p.filter, &p.reso, &p.start, &p.reverse, &p.drive, &p.prob};
    return f[i];
}
float* KitAcidField(AcidParams& a, int i)
{
    float* f[8] = {&a.cutoff, &a.reso, &a.envmod, &a.decay, &a.accent, &a.tune, &a.slide, &a.wave};
    return f[i];
}

char kit_txt[1600];

/** kit.txt for the 8 tracks from `base`; kind[c]: 's', 'a' or 0 (not in the kit) */
int KitSettingsBuild(int base, const char kind[8])
{
    int n = snprintf(kit_txt, sizeof(kit_txt), "APK1\n");
    for(int c = 0; c < 8; c++)
    {
        if(!kind[c])
            continue;
        TrackParams p = eng->track[base + c];
        n += snprintf(kit_txt + n, sizeof(kit_txt) - n, "%d %c", c + 1, kind[c]);
        for(int i = 0; i < kKitFields - 1; i++)
            n += snprintf(kit_txt + n, sizeof(kit_txt) - n, " %d", (int)(*KitField(p, i) * 10000.f + 0.5f));
        n += snprintf(kit_txt + n, sizeof(kit_txt) - n, " %d", p.xfade);
        if(kind[c] == 'a')
            for(int i = 0; i < 8; i++)
                n += snprintf(kit_txt + n, sizeof(kit_txt) - n, " %d",
                              (int)(*KitAcidField(p.acid, i) * 10000.f + 0.5f));
        n += snprintf(kit_txt + n, sizeof(kit_txt) - n, "\n");
    }
    return n;
}

/** Reads kits/<name>/kit.txt. false = the kit has none (or it's not ours) */
bool KitSettingsLoad(const char* dir, char kind[8], TrackParams par[8])
{
    char p[kPathLen];
    snprintf(p, sizeof(p), "%s/kit.txt", dir);
    int32_t n = plat->ReadFile(p, kit_txt, sizeof(kit_txt) - 1);
    if(n < 5 || strncmp(kit_txt, "APK1", 4))
        return false;
    kit_txt[n] = 0;
    for(int c = 0; c < 8; c++)
        kind[c] = 0;
    char* line = strchr(kit_txt, '\n');
    while(line && *++line)
    {
        char* end = strchr(line, '\n');
        if(end)
            *end = 0;
        int  c = 0, used = 0;
        char k = 0;
        if(sscanf(line, "%d %c%n", &c, &k, &used) == 2 && c >= 1 && c <= 8 && (k == 's' || k == 'a'))
        {
            TrackParams tp;
            const char* q    = line + used;
            int         want = kKitFields + (k == 'a' ? 8 : 0), got = 0;
            for(; got < want; got++)
            {
                char* e;
                long  v = strtol(q, &e, 10);
                if(e == q)
                    break;
                q = e;
                if(v < 0)
                    v = 0;
                if(got < kKitFields - 1)
                    *KitField(tp, got) = v > 10000 ? 1.f : v / 10000.f;
                else if(got == kKitFields - 1)
                    tp.xfade = v > 2 ? 0 : (uint8_t)v;
                else
                    *KitAcidField(tp.acid, got - kKitFields) = v > 10000 ? 1.f : v / 10000.f;
            }
            if(got == want) // a line cut short is ignored: the track loads with its sound only
            {
                par[c - 1]  = tp;
                kind[c - 1] = k;
            }
        }
        line = end;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Undo of a kit load: what the 8 tracks held                          */

struct KitUndo
{
    bool        valid = false, redo = false, await = false;
    int         pg    = 0, kit = -1;
    char        path[8][kPathLen];
    TrackParams par[8];
    uint32_t    gen[8];
};
KitUndo kundo;

bool KitTrackKept(int t) // a recording is never replaced by a kit: it exists nowhere else
{
    return IsRecPath(slot_path[t]) || eng->IsLoop(t);
}

void KitDescribe(KitUndo& u, int pg)
{
    u.pg  = pg;
    u.kit = loading_kit >= 0 && loading_page == pg ? loading_kit : loaded_kit_pg[pg];
    for(int c = 0; c < 8; c++)
    {
        int t = pg * 8 + c;
        snprintf(u.path[c], kPathLen, "%s", eng->IsSynth(t) ? kSynthPath : slot_path[t]);
        u.par[c] = eng->track[t];
    }
}

void KitBackup(int pg)
{
    KitDescribe(kundo, pg);
    kundo.valid = true;
    kundo.redo  = false;
    kundo.await = true; // until the kit is in
    lundo.valid = false; // one step of undo: this is it now
}

bool KitCan(bool redo)
{
    if(!kundo.valid || kundo.await || kundo.redo != redo || set_op || eng->LiveRecState() != 0 || num_jobs)
        return false;
    for(int c = 0; c < 8; c++)
        if(slot_gen[kundo.pg * 8 + c] != kundo.gen[c])
            return false; // something else changed a track since
    return true;
}

/** Called often: once the loads are in, remember what the tracks look like */
void KitUndoPoll()
{
    if(kundo.valid && kundo.await && num_jobs == 0 && loading_kit < 0 && load_state == kLoadIdle)
    {
        kundo.await = false;
        for(int c = 0; c < 8; c++)
            kundo.gen[c] = slot_gen[kundo.pg * 8 + c];
    }
}

/** Puts the 8 tracks back as they were; what they are now goes into the undo (so: redo) */
void KitUndoSwap()
{
    static KitUndo now; // big: not on the stack
    const int      pg = kundo.pg;
    KitDescribe(now, pg);
    for(int c = 0; c < 8; c++)
    {
        int t = pg * 8 + c;
        if(KitTrackKept(t) || IsRecPath(kundo.path[c]))
            continue;
        TrackParams p = kundo.par[c];
        p.mute        = eng->track[t].mute;
        p.solo        = eng->track[t].solo;
        eng->track[t] = p;
        if(strcmp(kundo.path[c], now.path[c]))
            QueueLoad(t, kundo.path[c][0] ? kundo.path[c] : "@k0", false);
    }
    if(pg == track_page)
        for(int c = 0; c < 8; c++)
            fader_live[c] = false;
    loaded_kit_pg[pg] = -1;
    if(kundo.kit >= 0)
    {
        loading_kit  = kundo.kit;
        loading_page = pg;
    }
    now.valid = true;
    now.redo  = !kundo.redo;
    now.await = true;
    kundo     = now;
}

/* ------------------------------------------------------------------ */
/* Saving a page as a kit (set_op 3)                                   */

char     ksave_name[kKitNameLen + 1];
char     ksave_dir[kKitNameLen + 8];
char     ksave_file[8][kPathLen];
char     ksave_kind[8];
bool     ksave_wav[8];
int      ksave_pg = 0, ksave_c = 0, ksave_phase = 0;
bool     ksave_open = false, ksave_fail = false;
uint32_t ksave_pos = 0;

/** over: an SD kit of that name may be replaced. Says why not in a popup */
bool KitSaveBegin(int pg, const char* raw, bool over)
{
    char name[kKitNameLen + 1];
    KitName(name, raw);
    if(!sd_ok)
        return Popup("no SD card"), false;
    if(set_op || eng->LiveRecState() != 0)
        return Popup("busy"), false;
    if(!name[0])
        return Popup("the kit needs a name"), false;
    int k = FindKit(name);
    if(k >= 0 && k < Engine::NumBuiltInKits())
        return Popup("%s is built in: other name", kit_names[k]), false;
    if(k >= 0 && !over)
        return Popup("kit %s exists", kit_names[k]), false;
    if(k < 0 && num_kits >= kMaxKits)
        return Popup("no room: %d kits", kMaxKits), false;
    snprintf(ksave_name, sizeof(ksave_name), "%s", k >= 0 ? kit_names[k] : name);
    snprintf(ksave_dir, sizeof(ksave_dir), "kits/%s", ksave_name);
    ksave_pg    = pg;
    ksave_phase = 0;
    set_op      = 3;
    op_done     = 0;
    op_total    = 1;
    snprintf(op_msg, sizeof(op_msg), "saving kit %.24s", ksave_name);
    Log("saving tracks %d-%d as kit %s", pg * 8 + 1, pg * 8 + 8, ksave_name);
    return true;
}

void KitSaveStep()
{
    uint32_t scratch_size = 0;
    uint8_t* scratch      = plat->Scratch(&scratch_size);
    const int base        = ksave_pg * 8;
    switch(ksave_phase)
    {
        case 0: // the loader shares the scratch buffer and the card
            if(num_jobs || load_state != kLoadIdle)
                return;
            op_total = 1;
            for(int c = 0; c < 8; c++)
            {
                int         t = base + c;
                const Slot& s = eng->GetSlot(t);
                ksave_wav[c]  = false;
                ksave_file[c][0] = 0;
                if(eng->IsSynth(t) || IsSynthPath(slot_path[t]))
                    ksave_kind[c] = 'a';
                else if(eng->IsLoop(t) || s.length == 0)
                    ksave_kind[c] = 0; // loops belong in sets
                else
                {
                    char nm[40];
                    if(IsRecPath(slot_path[t]))
                        snprintf(nm, sizeof(nm), "recorded");
                    else if(BuiltInKitOf(slot_path[t]) >= 0)
                        snprintf(nm, sizeof(nm), "%s", kKitRoles[c]);
                    else
                    {
                        SafeName(nm, sizeof(nm), slot_orig[t][0] ? slot_orig[t] : slot_path[t]);
                        /* "3 hat" saved again as track 3 shouldn't become "3 3 hat" */
                        const char* q = nm;
                        if(q[0] >= '1' && q[0] <= '8' && (q[1] == ' ' || q[1] == '_'))
                            q += 2;
                        memmove(nm, q, strlen(q) + 1);
                        if(!nm[0])
                            snprintf(nm, sizeof(nm), "sound");
                    }
                    snprintf(ksave_file[c], kPathLen, "%s/%d %.36s.wav", ksave_dir, c + 1, nm);
                    ksave_kind[c] = 's';
                    ksave_wav[c]  = true;
                    op_total += s.length;
                }
            }
            ksave_c    = 0;
            ksave_open = false;
            ksave_fail = false;
            ksave_phase = 1;
            return;
        case 1:
        {
            while(ksave_c < 8 && !ksave_wav[ksave_c])
                ksave_c++;
            if(ksave_c >= 8)
            {
                ksave_phase = 2;
                return;
            }
            const Slot& s = eng->GetSlot(base + ksave_c);
            if(!ksave_open)
            {
                if(!plat->FileCreate(ksave_dir, ksave_file[ksave_c]))
                {
                    Log("can't write %s", ksave_file[ksave_c]);
                    ksave_fail  = true;
                    ksave_phase = 3;
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
                ksave_open = plat->FileWrite(h, 44) == 44;
                ksave_pos  = 0;
                if(!ksave_open)
                {
                    plat->FileWriteClose();
                    ksave_fail  = true;
                    ksave_phase = 3;
                }
                return;
            }
            uint32_t n = s.length > ksave_pos ? s.length - ksave_pos : 0;
            if(n > scratch_size / 2)
                n = scratch_size / 2;
            if(n)
            {
                memcpy(scratch, s.data + ksave_pos, n * 2);
                if(plat->FileWrite(scratch, n * 2) != (int32_t)(n * 2))
                {
                    plat->FileWriteClose();
                    Log("write failed: %s", ksave_file[ksave_c]);
                    ksave_fail  = true;
                    ksave_phase = 3;
                    return;
                }
                ksave_pos += n;
                op_done += n;
            }
            if(ksave_pos >= s.length)
            {
                plat->FileWriteClose();
                ksave_open = false;
                ksave_c++;
            }
            return;
        }
        case 2:
        {
            char p[kPathLen];
            snprintf(p, sizeof(p), "%s/kit.txt", ksave_dir);
            int n = KitSettingsBuild(base, ksave_kind);
            if(!plat->WriteFile(ksave_dir, p, kit_txt, (uint32_t)n))
            {
                Log("can't write %s", p);
                ksave_fail = true;
            }
            ksave_phase = 3;
            return;
        }
        default:
        {
            if(!ksave_fail)
            {
                /* tidy: sounds from an older save of this kit. A track that isn't
                   in the kit this time (a loop) keeps its numbered file */
                static char gone[24][kNameLen + 32];
                int         ng = 0;
                if(plat->DirOpen(ksave_dir))
                {
                    char name[kNameLen + 32];
                    bool is_dir;
                    while(ng < 24 && plat->DirNext(name, sizeof(name), &is_dir))
                    {
                        if(is_dir || !IsWavName(name))
                            continue;
                        char full[kPathLen];
                        snprintf(full, sizeof(full), "%s/%s", ksave_dir, name);
                        bool keep = false;
                        for(int c = 0; c < 8 && !keep; c++)
                            keep = !strcasecmp(ksave_file[c], full)
                                   || (!ksave_kind[c] && name[0] == '1' + c && !isdigit((unsigned char)name[1]));
                        if(!keep)
                            snprintf(gone[ng++], sizeof(gone[0]), "%s", name);
                    }
                    plat->DirClose();
                }
                for(int i = 0; i < ng; i++)
                {
                    char full[kPathLen];
                    snprintf(full, sizeof(full), "%s/%s", ksave_dir, gone[i]);
                    plat->Remove(full);
                }
                KitsRescan();
                int k = FindKit(ksave_name);
                /* the tracks now are this kit (unless a loop sits among them) */
                bool whole = true;
                for(int c = 0; c < 8; c++)
                    whole = whole && ksave_kind[c];
                if(k >= 0 && whole && loading_kit < 0)
                    loaded_kit_pg[ksave_pg] = k;
                Popup("kit %s saved", ksave_name);
                Log("kit %s saved", ksave_name);
            }
            else
                Popup("KIT SAVE FAILED");
            set_op = 0;
            return;
        }
    }
}

/** Only kits on the card. The page asks before it sends this */
void KitDelete(int k)
{
    if(k < Engine::NumBuiltInKits() || k >= num_kits || !sd_ok || set_op)
    {
        Popup(k >= 0 && k < Engine::NumBuiltInKits() ? "built-in kits stay" : "can't delete now");
        return;
    }
    char dir[kKitNameLen + 32];
    snprintf(dir, sizeof(dir), "kits/%s", kit_names[k]);
    static char names[48][kNameLen + 32];
    int         n = 0;
    if(plat->DirOpen(dir))
    {
        char nm[kNameLen + 32];
        bool is_dir;
        while(n < 48 && plat->DirNext(nm, sizeof(nm), &is_dir))
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
    bool ok = plat->Remove(dir);
    char nm[kNameLen];
    snprintf(nm, sizeof(nm), "%s", kit_names[k]);
    for(int pg = 0; pg < 2; pg++)
        if(loaded_kit_pg[pg] == k)
            loaded_kit_pg[pg] = -1; // the sounds stay in the tracks; the kit is gone
    if(loading_kit == k)
        loading_kit = -1;
    KitsRescan();
    Popup(ok ? "kit %s deleted" : "kit %s: not all deleted", nm);
    Log("kit %s deleted%s", nm, ok ? "" : " (something stayed in its folder)");
}

void KitRename(int k, const char* raw)
{
    char name[kKitNameLen + 1];
    KitName(name, raw);
    if(k < Engine::NumBuiltInKits() || k >= num_kits || !sd_ok || set_op || !name[0])
    {
        Popup(k >= 0 && k < Engine::NumBuiltInKits() ? "built-in kits keep their names" : "can't rename now");
        return;
    }
    int other = FindKit(name);
    if(other >= 0 && other != k)
    {
        Popup("kit %s exists", kit_names[other]);
        return;
    }
    char a[kKitNameLen + 32], b[kKitNameLen + 32];
    snprintf(a, sizeof(a), "kits/%s", kit_names[k]);
    snprintf(b, sizeof(b), "kits/%s", name);
    if(!plat->Rename(a, b))
    {
        Popup("rename failed");
        return;
    }
    /* tracks playing its files: their paths move with it, so a set saved later finds them */
    size_t la = strlen(a);
    for(int t = 0; t < kTracks; t++)
        for(char* p : {slot_path[t], slot_orig[t]})
            if(!strncasecmp(p, a, la) && p[la] == '/')
            {
                char q[kPathLen];
                snprintf(q, sizeof(q), "%s%s", b, p + la);
                snprintf(p, kPathLen, "%s", q);
            }
    snprintf(kit_names[k], kNameLen, "%s", name); // so what's loaded follows the new name
    KitsRescan();
    Popup("kit renamed: %s", name);
    Log("kit renamed to %s", name);
}

/* The one undo button: a kit load is the last thing done */
void KitUndoForget() { kundo.valid = false; }
bool KitUndoCan(bool redo) { return KitCan(redo); }
void KitUndoDo(bool redo)
{
    int pg = kundo.pg;
    KitUndoSwap();
    Popup(redo ? "tracks %d-%d: the kit again" : "tracks %d-%d: as before the kit", pg * 8 + 1, pg * 8 + 8);
    Log("undo: %s the kit load on tracks %d-%d", redo ? "redid" : "undid", pg * 8 + 1, pg * 8 + 8);
}
