/** The APC40's layout as a table that can be changed. Included by box.cpp,
 *  inside its namespace, after kits.h.
 *
 *  Every button has a number (cid), every knob and fader one too (ccid).
 *  The firmware's own functions stay where they always were: "what button L
 *  does, alone or with SHIFT, in the standard layout". The map says, for
 *  each physical button, alone and with SHIFT, which of those it does now:
 *
 *      bmap[cid][0 alone | 1 SHIFT] = L | (0x80 if it's L's SHIFT function)
 *
 *  0xFF = nothing. L >= kMapBtns are functions no button has in the standard
 *  layout (kMapVirt of them). LEDs follow: a button shows the light of the
 *  function it does alone. Knobs and faders: cmap[ccid] = the knob or fader
 *  whose job it does; its ring follows.
 *
 *  cid:  0-39   pads, row * 8 + column
 *        40-44  SCENE LAUNCH 1-5
 *        45-84  strip rows, 8 each: RECORD ARM, SOLO, ACTIVATOR, TRACK
 *               SELECTION, CLIP STOP
 *        85 MASTER, 86 STOP ALL CLIPS
 *        87-90  PAN, SEND A, SEND B, SEND C
 *        91 PLAY, 92 STOP, 93 REC, 94 up, 95 down, 96 right, 97 left,
 *        98 SHIFT (never mapped), 99 TAP TEMPO, 100 NUDGE +, 101 NUDGE -
 *        102-109 CLIP/TRACK, DEVICE ON/OFF, left arrow, right arrow, DETAIL
 *               VIEW, REC QUANTIZATION, MIDI OVERDUB, METRONOME
 *        110 one-shot sample, 111 save the page as a kit, 112 take length,
 *        113 count-in, 114 pattern quantise, 115 FX quantise
 *  ccid: 0-7 TRACK CONTROL knobs, 8-15 DEVICE CONTROL knobs, 16-23 faders,
 *        24 MASTER fader, 25 crossfader, 26 CUE LEVEL (endless: never mapped)
 *
 *  Saved as apcbox/apcmap.txt on the card (only what differs from standard):
 *      APM1
 *      b <cid> <0|1> <L or 255> <0|1>
 *      c <ccid> <ccid>
 */

constexpr int     kMapBtns  = 110;
constexpr int     kMapVirt  = 6;
constexpr int     kMapCcs   = 27;
constexpr uint8_t kMapNone  = 0xFF;
constexpr int     kCidShift = 98;
constexpr int     kCcCue    = 26;

uint8_t  bmap[kMapBtns][2];
uint8_t  cmap[kMapCcs];
uint8_t  map_held[kMapBtns]; // what a held button started (0xFE = not held)
uint8_t  vwant[kMapVirt];    // lights of the functions without a button
bool     map_dirty      = false; // not on the card yet
bool     map_report     = true;  // tell the pages
uint32_t map_changed_at = 0;

/* lwant[] (box.cpp): what the box wants each light to be, before the map moves them */
int16_t led_cid[kMaxLeds];  // the button a light belongs to, -1 = not a button
int8_t  led_ring[kMaxLeds]; // the knob (ccid) a ring belongs to, -1 = not a ring
uint8_t led_ch[kMaxLeds];   // device lights exist once per track bank
bool    led_type[kMaxLeds]; // a ring's style, not its value

void    OnButton(uint8_t ch, uint8_t note, bool on, uint32_t now);
void    OnCC(uint8_t ch, uint8_t cc, uint8_t v, uint32_t now);
inline uint8_t Ring(float p);

int CidOf(int note, int ch)
{
    using namespace apc;
    if(note >= kClipFirst && note < kClipFirst + 5)
        return ch < 8 ? (note - kClipFirst) * 8 + ch : -1;
    if(note >= kSceneFirst && note < kSceneFirst + 5)
        return 40 + note - kSceneFirst;
    if(note >= kRecArm && note <= kClipStop)
        return ch < 8 ? 45 + (note - kRecArm) * 8 + ch : -1;
    if(note >= kMaster && note <= kStopAll)
        return 85 + note - kMaster;
    if(note >= kPan && note <= kNudgeMinus)
        return 87 + note - kPan;
    if(note >= kDevFirst && note < kDevFirst + 8)
        return 102 + note - kDevFirst;
    return -1;
}

void CidNote(int cid, int* note, int* ch)
{
    using namespace apc;
    *ch = 0;
    if(cid < 40)
        *note = kClipFirst + cid / 8, *ch = cid % 8;
    else if(cid < 45)
        *note = kSceneFirst + cid - 40;
    else if(cid < 85)
        *note = kRecArm + (cid - 45) / 8, *ch = (cid - 45) % 8;
    else if(cid < 87)
        *note = kMaster + cid - 85;
    else if(cid < 102)
        *note = kPan + cid - 87;
    else
        *note = kDevFirst + cid - 102;
}

int CcidOf(int cc, int ch)
{
    using namespace apc;
    if(cc == kFaderCC)
        return ch < 8 ? 16 + ch : -1;
    if(cc == kMasterCC)
        return 24;
    if(cc == kXfadeCC)
        return 25;
    if(cc == kCueCC)
        return kCcCue;
    if(cc >= kTrkKnobCC && cc < kTrkKnobCC + 8)
        return cc - kTrkKnobCC;
    if(cc >= kDevKnobCC && cc < kDevKnobCC + 8)
        return ch < 8 ? 8 + cc - kDevKnobCC : -1;
    return -1;
}

bool MapIsDefault()
{
    for(int i = 0; i < kMapBtns; i++)
        if(bmap[i][0] != i || bmap[i][1] != (i | 0x80))
            return false;
    for(int i = 0; i < kMapCcs; i++)
        if(cmap[i] != i)
            return false;
    return true;
}

void MapDefault()
{
    for(int i = 0; i < kMapBtns; i++)
    {
        bmap[i][0]  = (uint8_t)i;
        bmap[i][1]  = (uint8_t)(i | 0x80);
        map_held[i] = 0xFE;
    }
    for(int i = 0; i < kMapCcs; i++)
        cmap[i] = (uint8_t)i;
    map_report = true;
}

void MapChanged()
{
    map_dirty      = true;
    map_changed_at = g_now;
    map_report     = true;
    ReleaseAllHeld(); // nothing stays stuck under a button that now does something else
    for(int i = 0; i < kMapBtns; i++)
        map_held[i] = 0xFE;
}

/** false = not a mapping this box can do */
bool MapSetButton(int p, int s, int l, int t)
{
    if(p < 0 || p >= kMapBtns || p == kCidShift || s < 0 || s > 1 || t < 0 || t > 1)
        return false;
    if(l != kMapNone && (l < 0 || l >= kMapBtns + kMapVirt || l == kCidShift))
        return false;
    bmap[p][s] = l == kMapNone ? kMapNone : (uint8_t)(l | (t && l < kMapBtns ? 0x80 : 0));
    return true;
}

bool MapSetCc(int p, int l)
{
    if(p < 0 || p >= kMapCcs || p == kCcCue)
        return false;
    if(l != kMapNone && (l < 0 || l >= kMapCcs || l == kCcCue))
        return false;
    cmap[p] = (uint8_t)l;
    return true;
}

/* ------------------------------------------------------------------ */
/* On the card                                                         */

char map_txt[4200];

void MapSave()
{
    int n = snprintf(map_txt, sizeof(map_txt), "APM1\n");
    for(int i = 0; i < kMapBtns; i++)
        for(int s = 0; s < 2; s++)
            if(bmap[i][s] != (i | (s ? 0x80 : 0)) && n < (int)sizeof(map_txt) - 24)
            {
                uint8_t e = bmap[i][s];
                n += snprintf(map_txt + n, sizeof(map_txt) - n, "b %d %d %d %d\n", i, s,
                              e == kMapNone ? 255 : e & 0x7F, e == kMapNone ? 0 : e >> 7);
            }
    for(int i = 0; i < kMapCcs; i++)
        if(cmap[i] != i && n < (int)sizeof(map_txt) - 24)
            n += snprintf(map_txt + n, sizeof(map_txt) - n, "c %d %d\n", i, cmap[i]);
    bool ok   = plat->WriteFile("apcbox", "apcbox/apcmap.txt", map_txt, (uint32_t)n);
    map_dirty = false;
    Log(ok ? "button map saved" : "button map: can't write it to the card");
    if(!ok)
        Popup("MAP NOT SAVED");
}

/** At power-up and when a card goes in: the saved map, or the standard one */
void MapLoad()
{
    MapDefault();
    int32_t n = plat->ReadFile("apcbox/apcmap.txt", map_txt, sizeof(map_txt) - 1);
    if(n < 5 || strncmp(map_txt, "APM1", 4))
        return;
    map_txt[n] = 0;
    int   count = 0;
    char* line  = strchr(map_txt, '\n');
    while(line && *++line)
    {
        int p, s, l, t;
        if(sscanf(line, "b %d %d %d %d", &p, &s, &l, &t) == 4)
            count += MapSetButton(p, s, l, t);
        else if(sscanf(line, "c %d %d", &p, &l) == 2)
            count += MapSetCc(p, l);
        line = strchr(line, '\n');
    }
    if(count)
        Log("button map: %d changes from the card", count);
}

void MapPoll(uint32_t now)
{
    if(map_dirty && now - map_changed_at > 1500 && sd_ok && !set_op && num_jobs == 0 && load_state == kLoadIdle)
        MapSave();
}

void MapReport()
{
    if(!map_report)
        return;
    map_report = false;
    char hex[40 * 4 + 1];
    for(int from = 0; from < kMapBtns; from += 40)
    {
        int n = 0;
        for(int i = from; i < from + 40 && i < kMapBtns; i++)
            n += snprintf(hex + n, sizeof(hex) - n, "%02x%02x", bmap[i][0], bmap[i][1]);
        Out("MB %d %s", from, hex);
    }
    int n = 0;
    for(int i = 0; i < kMapCcs; i++)
        n += snprintf(hex + n, sizeof(hex) - n, "%02x", cmap[i]);
    Out("MC %s", hex);
}

/* ------------------------------------------------------------------ */
/* Functions no button has in the standard layout                      */

void VirtualButton(int v, bool on, uint32_t now)
{
    (void)now;
    if(!on)
        return;
    switch(v)
    {
        case 0: // one-shot sample into the selected track; again = stop
            if(eng->LiveRecState() == 2)
                eng->LiveRecStop();
            else if(eng->LiveRecState() == 1)
                LooperCancel();
            else
                LooperStart(sel_track, Engine::kRecShot);
            break;
        case 1: // the 8 tracks on the strips as a new kit: "kit 01", "kit 02"..
        {
            char name[12];
            for(int i = 1; i < 100; i++)
            {
                snprintf(name, sizeof(name), "kit %02d", i);
                if(FindKit(name) < 0)
                {
                    KitSaveBegin(track_page, name, false);
                    return;
                }
            }
            Popup("no free kit name");
            break;
        }
        case 2:
        {
            const uint8_t next[9] = {1, 2, 4, 4, 8, 8, 8, 8, 0};
            eng->lr_len_bars      = next[eng->lr_len_bars > 8 ? 8 : eng->lr_len_bars];
            if(eng->lr_len_bars)
                Popup("take: %d bar%s", eng->lr_len_bars, eng->lr_len_bars == 1 ? "" : "s");
            else
                Popup("take: until stopped");
            break;
        }
        case 3:
            eng->lr_countin = !eng->lr_countin;
            Popup("count-in %s", eng->lr_countin ? "on" : "off");
            break;
        case 4:
            eng->quant_pat = (uint8_t)((eng->quant_pat + 1) % 3);
            Popup("patterns: on the %s", eng->quant_pat == 0 ? "bar" : eng->quant_pat == 1 ? "beat" : "step");
            break;
        case 5:
            eng->quant_fx = (uint8_t)((eng->quant_fx + 1) % 3);
            Popup("FX: %s", eng->quant_fx == 0 ? "at once" : eng->quant_fx == 1 ? "on the step" : "on the beat");
            break;
        default: break;
    }
}

void VirtualLeds()
{
    const bool shot = eng->LiveRecMode() == Engine::kRecShot;
    vwant[0]        = shot && eng->LiveRecState() == 2 ? 1 : shot && eng->LiveRecState() == 1 ? 2 : 0;
    vwant[1]        = set_op == 3 ? 2 : 0;
    vwant[2]        = eng->lr_len_bars ? 1 : 0;
    vwant[3]        = eng->lr_countin ? 1 : 0;
    vwant[4]        = eng->quant_pat ? 1 : 0;
    vwant[5]        = eng->quant_fx ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* In: a physical button or knob, through the map                      */

void MapButton(uint8_t ch, uint8_t note, bool on, uint32_t now)
{
    int cid = CidOf(note, ch);
    if(cid < 0 || cid == kCidShift)
    {
        OnButton(ch, note, on, now);
        return;
    }
    uint8_t e;
    if(on)
    {
        e             = bmap[cid][shift ? 1 : 0];
        map_held[cid] = e;
    }
    else
    {
        e             = map_held[cid]; // let go of what the press started, whatever SHIFT is now
        map_held[cid] = 0xFE;
        if(e == 0xFE)
            return;
    }
    if(e == kMapNone)
        return;
    const int  l    = e & 0x7F;
    const bool real = shift;
    shift           = (e & 0x80) != 0; // the function's own SHIFT, not the finger's
    if(l >= kMapBtns)
        VirtualButton(l - kMapBtns, on, now);
    else
    {
        int n, c;
        CidNote(l, &n, &c);
        OnButton((uint8_t)c, (uint8_t)n, on, now);
    }
    shift = real;
}

void MapCC(uint8_t ch, uint8_t cc, uint8_t v, uint32_t now)
{
    using namespace apc;
    int p = CcidOf(cc, ch);
    if(p < 0 || p == kCcCue || cmap[p] == p)
    {
        OnCC(ch, cc, v, now);
        return;
    }
    const int l = cmap[p];
    if(l == kMapNone)
        return;
    if(l < 8)
        OnCC(0, (uint8_t)(kTrkKnobCC + l), v, now);
    else if(l < 16) // device knobs go by track bank: the bank of the selected track
        OnCC((uint8_t)(p >= 8 && p < 16 ? ch : sel_track % 8), (uint8_t)(kDevKnobCC + l - 8), v, now);
    else if(l < 24)
        OnCC((uint8_t)(l - 16), kFaderCC, v, now);
    else if(l == 24)
        OnCC(0, kMasterCC, v, now);
    else if(l == 25)
        OnCC(0, kXfadeCC, v, now);
}

/* ------------------------------------------------------------------ */
/* Out: the lights, moved to where the functions are                   */

void MapLedMeta()
{
    for(int i = 0; i < kMaxLeds; i++)
    {
        led_cid[i]  = -1;
        led_ring[i] = -1;
        led_ch[i]   = 0;
        led_type[i] = false;
        lwant[i]    = 0;
    }
    for(int r = 0; r < 5; r++)
        for(int c = 0; c < 8; c++)
        {
            led_cid[led_grid[r][c]] = (int16_t)(r * 8 + c);
            led_cid[led_trk[r][c]]  = (int16_t)(45 + r * 8 + c);
        }
    led_cid[led_master] = 85;
    for(int i = 0; i < 5; i++)
        led_cid[led_scene[i]] = (int16_t)(40 + i);
    for(int i = 0; i < 4; i++)
        led_cid[led_page[i]] = (int16_t)(87 + i);
    for(int k = 0; k < 8; k++)
    {
        led_ring[led_trk_ring[k]] = (int8_t)k;
        led_ring[led_trk_type[k]] = (int8_t)k;
        led_type[led_trk_type[k]] = true;
        for(int c = 0; c < 9; c++)
        {
            led_cid[led_dev[k][c]]       = (int16_t)(102 + k);
            led_ch[led_dev[k][c]]        = (uint8_t)c;
            led_ring[led_dev_ring[k][c]] = (int8_t)(8 + k);
            led_ch[led_dev_ring[k][c]]   = (uint8_t)c;
            led_ring[led_dev_type[k][c]] = (int8_t)(8 + k);
            led_ch[led_dev_type[k][c]]   = (uint8_t)c;
            led_type[led_dev_type[k][c]] = true;
        }
    }
}

/** The light of function L (bank c for the device buttons); -1 = it has none */
int MapLedOf(int l, int c)
{
    if(l < 40)
        return led_grid[l / 8][l % 8];
    if(l < 45)
        return led_scene[l - 40];
    if(l < 85)
        return led_trk[(l - 45) / 8][(l - 45) % 8];
    if(l == 85)
        return led_master;
    if(l >= 87 && l <= 90)
        return led_page[l - 87];
    if(l >= 102 && l < kMapBtns)
        return led_dev[l - 102][c];
    return -1;
}

/** Called at the end of UpdateLeds: lwant (by function) -> leds[].want (by button) */
void MapLeds()
{
    VirtualLeds();
    for(int i = 0; i < num_leds; i++)
    {
        uint8_t v = lwant[i];
        if(led_cid[i] >= 0)
        {
            const int     p = led_cid[i];
            const uint8_t e = bmap[p][0];
            if(e != p)
            {
                v = 0;
                if(e != kMapNone)
                {
                    const int l = e & 0x7F;
                    if(l >= kMapBtns)
                        v = vwant[l - kMapBtns];
                    else
                    {
                        int s = MapLedOf(l, led_ch[i]);
                        v     = s >= 0 ? lwant[s] : 0;
                        /* pads have colours, the other buttons are on / off / blink */
                        if(l < 40 && p >= 40 && v)
                            v = (v & 1) ? 1 : 2;
                    }
                }
            }
        }
        else if(led_ring[i] >= 0)
        {
            const int p = led_ring[i], l = cmap[p], c = led_ch[i];
            if(l != p)
            {
                if(l == kMapNone)
                    v = led_type[i] ? lwant[i] : 0;
                else if(l < 8)
                    v = lwant[led_type[i] ? led_trk_type[l] : led_trk_ring[l]];
                else if(l < 16)
                    v = lwant[led_type[i] ? led_dev_type[l - 8][c] : led_dev_ring[l - 8][c]];
                else
                {
                    float x = l < 24 ? eng->track[StripTrack(l - 16)].vol
                                     : l == 24 ? eng->global.master : eng->global.xfader;
                    v       = led_type[i] ? (l == 25 ? apc::kRingPan : apc::kRingVolume) : Ring(x);
                }
            }
        }
        leds[i].want = v;
    }
}
