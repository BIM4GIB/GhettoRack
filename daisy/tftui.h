/** The 3.5" colour screen: five views drawn into a 480x320 RGB565
 *  framebuffer. Included inside box.cpp (it reads the box's state), so
 *  it builds the same on the Daisy and the Mac.
 *
 *  Views, cycled with the encoder push:
 *    STEPS     16 tracks x 16 steps, selected track's knobs
 *    MIXER     16 strips, crossfader, master
 *    PATTERNS  banks A-H x 8, performance FX, kits
 *    TRACK     selected track: steps with notes, waveform or 303, knobs
 *    SYSTEM    status, CPU graph, sample browser, log
 *  Turning the encoder: STEPS / MIXER / TRACK pick the track, PATTERNS
 *  queues the next / previous pattern, SYSTEM steps through samples.
 *  A knob turned on the APC40 pops its value up over any view.
 */

constexpr int kTftW = 480, kTftH = 320, kTftBands = kTftH / 16;

uint16_t* tfb      = nullptr; // the platform's framebuffer
int       tft_view = 0;
const char* const kViewNames[5] = {"STEPS", "MIXER", "PATTERNS", "TRACK", "SYSTEM"};

/* ---- colours (RGB565) ---- */
constexpr uint16_t Rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t cBg = Rgb(12, 13, 16), cLine = Rgb(44, 47, 54), cCell = Rgb(38, 41, 47);
constexpr uint16_t cTx = Rgb(228, 230, 234), cDim = Rgb(112, 117, 124), cFaint = Rgb(70, 74, 80);
constexpr uint16_t cGreen = Rgb(60, 190, 90), cRed = Rgb(205, 62, 56), cYellow = Rgb(242, 192, 60);
constexpr uint16_t cAmber = Rgb(125, 102, 40), cBlue = Rgb(120, 170, 232), cPurple = Rgb(180, 130, 230);
inline uint16_t Dim(uint16_t c) // about 45%
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (uint16_t)(((r * 7 / 16) << 11) | ((g * 7 / 16) << 5) | (b * 7 / 16));
}

/* ---- drawing ---- */
void TFill(int x, int y, int w, int h, uint16_t c)
{
    if(x < 0)
        w += x, x = 0;
    if(y < 0)
        h += y, y = 0;
    if(x + w > kTftW)
        w = kTftW - x;
    if(y + h > kTftH)
        h = kTftH - y;
    for(int j = 0; j < h; j++)
    {
        uint16_t* p = tfb + (y + j) * kTftW + x;
        for(int i = 0; i < w; i++)
            p[i] = c;
    }
}
void TRect(int x, int y, int w, int h, uint16_t c)
{
    TFill(x, y, w, 1, c);
    TFill(x, y + h - 1, w, 1, c);
    TFill(x, y, 1, h, c);
    TFill(x + w - 1, y, 1, h, c);
}
inline void TPix(int x, int y, uint16_t c)
{
    if(x >= 0 && y >= 0 && x < kTftW && y < kTftH)
        tfb[y * kTftW + x] = c;
}
/** Text; scale 2 doubles every pixel. Returns the x after the text. */
int TText(int x, int y, const OledFont& f, const char* s, uint16_t c, int scale = 1)
{
    for(; *s; s++)
    {
        int ch = (uint8_t)*s;
        if(ch < 32 || ch > 126)
            ch = '?';
        const uint16_t* rows = f.rows + (ch - 32) * f.h;
        for(int r = 0; r < f.h; r++)
            for(int b = 0; b < f.w; b++)
                if(rows[r] & (1u << (f.w - 1 - b)))
                {
                    if(scale == 1)
                        TPix(x + b, y + r, c);
                    else
                        TFill(x + b * scale, y + r * scale, scale, scale, c);
                }
        x += f.w * scale;
    }
    return x;
}
int TTextW(const OledFont& f, const char* s, int scale = 1)
{
    return (int)strlen(s) * f.w * scale;
}
void TTextR(int xr, int y, const OledFont& f, const char* s, uint16_t c) // right-aligned
{
    TText(xr - TTextW(f, s), y, f, s, c);
}
void TTextC(int xc, int y, const OledFont& f, const char* s, uint16_t c) // centred
{
    TText(xc - TTextW(f, s) / 2, y, f, s, c);
}
void TBar(int x, int y, int w, int h, float p, uint16_t c)
{
    TFill(x, y, w, h, cCell);
    int v = (int)(w * (p < 0 ? 0 : (p > 1 ? 1 : p)) + 0.5f);
    TFill(x, y, v, h, c);
}
/** Knob ring, 270 degrees from bottom-left, like the APC40's */
void TRing(int cx, int cy, int r, float p, uint16_t c)
{
    for(int a = 0; a <= 270; a += 2)
    {
        float    rad = (135.f + a) * 0.0174533f;
        float    cs = cosf(rad), sn = sinf(rad);
        uint16_t col = (a <= p * 270.f && p > 0.f) ? c : cCell;
        for(int k = -2; k <= 1; k++)
            TFill((int)(cx + (r + k) * cs), (int)(cy + (r + k) * sn), 2, 2, col);
    }
}

/* ---- knob names and values, shared by STEPS and TRACK ---- */
void KnobText(int t, int k, const char** name, char* val, size_t n, float* p)
{
    const TrackParams& tp = eng->track[t];
    if(eng->IsSynth(t))
    {
        static const char* const names[8]
            = {"cutoff", "reso", "env mod", "decay", "accent", "tune", "slide", "wave"};
        const AcidParams& a  = tp.acid;
        float             v[8] = {a.cutoff, a.reso, a.envmod, a.decay, a.accent, a.tune, a.slide, a.wave};
        *name = names[k];
        *p    = v[k];
        switch(k)
        {
            case 0:
            {
                char hz[12];
                FmtHz(hz, sizeof(hz), 150.f * powf(2.f, v[0] * 5.f));
                snprintf(val, n, "%sHz", hz);
                break;
            }
            case 3: snprintf(val, n, "%dms", (int)(30.f * powf(80.f, v[3]))); break;
            case 5: snprintf(val, n, "%+.1f", (v[5] - 0.5f) * 24.f); break;
            case 6: snprintf(val, n, "%dms", (int)(12.f * powf(25.f, v[6]))); break;
            case 7: snprintf(val, n, "%s", v[7] >= 0.5f ? "square" : "saw"); break;
            default: FmtPct(val, n, v[k]);
        }
        return;
    }
    static const char* const names[8]
        = {"pitch", "decay", "filter", "reso", "start", "reverse", "drive", "prob"};
    float v[8] = {tp.pitch, tp.decay, tp.filter, tp.reso, tp.start, tp.reverse, tp.drive, tp.prob};
    *name = names[k];
    *p    = v[k];
    switch(k)
    {
        case 0: snprintf(val, n, "%+d st", (int)roundf((v[0] - 0.5f) * 24.f)); break;
        case 1:
            if(v[1] > 0.99f)
                snprintf(val, n, "full");
            else
                FmtPct(val, n, v[1]);
            break;
        case 2: FmtFilter(val, n, v[2]); break;
        case 5: snprintf(val, n, "%s", v[5] >= 0.5f ? "reverse" : "forward"); break;
        default: FmtPct(val, n, v[k]);
    }
}

/* ---- log and CPU history for SYSTEM ---- */
constexpr int kLogLines = 16;
char          log_ring[kLogLines][64];
uint32_t      log_time[kLogLines];
int           log_n = 0;
void          LogKeep(const char* msg)
{
    int i = log_n++ % kLogLines;
    snprintf(log_ring[i], sizeof(log_ring[i]), "%s", msg);
    log_time[i] = g_now;
}
constexpr int kCpuHist = 180;
uint8_t       cpu_hist[kCpuHist];
int           cpu_hist_n    = 0;
uint32_t      cpu_hist_time = 0;

/* ---- waveform of the selected track's sample, cached ---- */
int8_t   wave_min[kTftW], wave_max[kTftW];
int      wave_track = -1;
uint32_t wave_len   = 0;
char     wave_path[kPathLen];
void     WaveScan(int t, int w)
{
    const Slot& s = eng->GetSlot(t);
    if(t == wave_track && s.length == wave_len && !strcmp(wave_path, slot_path[t]))
        return;
    wave_track = t;
    wave_len   = s.length;
    snprintf(wave_path, kPathLen, "%s", slot_path[t]);
    for(int x = 0; x < w; x++)
    {
        uint32_t a = (uint32_t)((uint64_t)s.length * x / w);
        uint32_t b = (uint32_t)((uint64_t)s.length * (x + 1) / w);
        int      lo = 0, hi = 0;
        uint32_t stride = (b - a) / 64 + 1; // enough points to see the shape
        for(uint32_t i = a; i < b && s.data; i += stride)
        {
            int v = s.data[i] >> 8;
            lo    = v < lo ? v : lo;
            hi    = v > hi ? v : hi;
        }
        wave_min[x] = (int8_t)lo;
        wave_max[x] = (int8_t)hi;
    }
}

/* ================================================================== */

void TftHeader(uint32_t now)
{
    char buf[32];
    int  pat = eng->CurrentPattern();
    snprintf(buf, sizeof(buf), "%c%d", "ABCDEFGH"[pat >> 3], (pat & 7) + 1);
    int x = TText(4, 2, kFontBig, buf, cTx);
    if(eng->Playing())
        for(int i = 0; i < 12; i++) // play triangle
            TFill(x + 6, 3 + i, i < 6 ? i + 1 : 12 - i, 1, cGreen);
    else
        TFill(x + 6, 5, 9, 9, cDim); // stop square
    snprintf(buf, sizeof(buf), "%.1f", eng->global.bpm);
    x = TText(x + 22, 2, kFontBig, buf, cTx);
    snprintf(buf, sizeof(buf), "%d/16", eng->CurrentStep() + 1);
    x = TText(x + 10, 7, kFontSmall, buf, cDim);
    if(eng->Recording())
        x = TText(x + 10, 7, kFontSmall, "REC", cRed);
    if(eng->LiveRecState())
    {
        snprintf(buf, sizeof(buf), eng->LiveRecState() == 1 ? "ARMED %d" : "SAMPLING %d:%d",
                 eng->LiveRecTrack() + 1, eng->LiveRecBars() + 1);
        if(eng->LiveRecState() == 1)
            snprintf(buf, sizeof(buf), "ARMED %d", eng->LiveRecTrack() + 1);
        x = TText(x + 10, 7, kFontSmall, buf, ((now / 250) & 1) ? cRed : cTx);
    }
    if(eng->LiveRecState() || eng->input_monitor) // input meter
    {
        float p = eng->InputPeak();
        TBar(x + 8, 9, 40, 5, p, p > 0.9f ? cRed : cGreen);
        x += 48;
    }
    x = TText(x + 10, 7, kFontSmall, track_page ? "9-16" : "1-8", cYellow);
    TText(x + 14, 7, kFontSmall, sets_open ? "SETS" : kViewNames[tft_view], sets_open ? cYellow : cTx);
    if(!sets_open)
        for(int v = 0; v < 5; v++)
            TFill(kTftW - 58 + v * 11, 8, 6, 6, v == tft_view ? cTx : cLine);
    else if(set_dirty)
        TTextR(kTftW - 4, 7, kFontSmall, "* unsaved", cYellow);
    TFill(0, 21, kTftW, 1, cLine);
    (void)now;
}

/* ---- 1 STEPS ---- */
void ViewSteps()
{
    const int pat = eng->CurrentPattern(), step = eng->CurrentStep();
    const bool playing = eng->Playing();
    const int y0 = 25, rh = 13, nx = 106, cw = 23;
    char buf[40];
    for(int t = 0; t < kTracks; t++)
    {
        int  y   = y0 + t * rh + (t >= 8 ? 4 : 0);
        bool off = (t >> 3) != track_page;
        TrackLabel(buf, sizeof(buf), t);
        buf[17] = 0;
        if(t == sel_track)
            TRect(0, y - 2, nx - 4, rh, cYellow);
        uint16_t tc = eng->track[t].mute ? cRed : cTx;
        TText(3, y, kFontSmall, buf, off ? Dim(tc) : tc);
        bool acid = eng->IsSynth(t);
        for(int s = 0; s < kSteps; s++)
        {
            uint8_t  v = eng->GetStep(pat, t, s);
            uint16_t c = v == kStepAccent ? cRed : (v ? cGreen : cCell);
            if(off && v)
                c = Dim(c);
            int x = nx + s * cw;
            TFill(x, y, cw - 3, rh - 3, c);
            if(acid && v)
            {
                if(eng->GetSlide(pat, t, s))
                    TFill(x + cw - 3, y + 3, 3, 4, cYellow);
                if(t == sel_track)
                {
                    char nn[8];
                    NoteName(nn, sizeof(nn), eng->GetNote(pat, t, s));
                    TText(x + 1, y + 1, kFontSmall, nn, cBg);
                }
            }
        }
    }
    if(playing)
    {
        int x = nx + step * cw;
        TRect(x - 2, y0 - 3, cw + 1, 16 * rh + 8, cYellow);
    }
    /* selected track and its knobs */
    int y = y0 + 16 * rh + 8;
    TFill(0, y - 3, kTftW, 1, cLine);
    TrackLabel(buf, sizeof(buf), sel_track);
    int x = TText(4, y, kFontSmall, buf, cYellow);
    if(eng->IsSynth(sel_track))
    {
        snprintf(buf, sizeof(buf), "oct C%d", acid_oct + 2);
        TText(x + 16, y, kFontSmall, buf, cYellow);
        if(held_step >= 0)
        {
            char nn[8];
            NoteName(nn, sizeof(nn), eng->GetNote(pat, sel_track, held_step));
            snprintf(buf, sizeof(buf), "held step %d: %s", held_step + 1, nn);
            TTextR(kTftW - 4, y, kFontSmall, buf, cTx);
        }
    }
    for(int k = 0; k < 8; k++)
    {
        const char* n;
        char        v[16];
        float       p;
        KnobText(sel_track, k, &n, v, sizeof(v), &p);
        int kx = 4 + (k % 4) * 120, ky = y + 14 + (k / 4) * 24;
        TText(kx, ky, kFontSmall, n, cDim);
        TTextR(kx + 110, ky, kFontSmall, v, cTx);
        TBar(kx, ky + 11, 110, 3, p, cBlue);
    }
}

/* ---- 2 MIXER ---- */
void ViewMixer()
{
    char buf[48];
    const float xf = eng->global.xfader;
    const float side[3] = {1.f, fminf(1.f, 2.f * (1.f - xf)), fminf(1.f, 2.f * xf)};
    for(int t = 0; t < kTracks; t++)
    {
        const TrackParams& tp  = eng->track[t];
        int                x   = 4 + t * 29 + (t >= 8 ? 8 : 0);
        bool               off = (t >> 3) != track_page;
        auto               C   = [&](uint16_t c) { return off ? Dim(c) : c; };
        snprintf(buf, sizeof(buf), "%d", t + 1);
        TTextC(x + 13, 26, kFontSmall, buf, C(t == sel_track ? cYellow : cTx));
        if(t == sel_track)
            TRect(x, 23, 27, 248, cYellow);
        const int top = 40, bot = 214;
        TFill(x + 9, top, 9, bot - top, cCell);
        int h = (int)((bot - top) * tp.vol);
        uint16_t vc = tp.mute ? cRed : cGreen;
        TFill(x + 9, bot - h, 9, h, C(side[tp.xfade % 3] < 0.01f ? Dim(vc) : vc));
        if(tp.xfade)
            TTextC(x + 13, 262, kFontSmall, tp.xfade == 1 ? "A" : "B", C(cYellow));
        int px = x + 2 + (int)(20 * tp.pan);
        TFill(x + 2, 222, 23, 2, cCell);
        TFill(px, 220, 3, 6, C(cTx));
        TBar(x + 2, 232, 23, 3, tp.rev, C(cBlue));
        TBar(x + 2, 240, 23, 3, tp.dly, C(cPurple));
        TText(x + 4, 250, kFontSmall, "M", tp.mute ? C(cRed) : cFaint);
        TText(x + 16, 250, kFontSmall, "S", tp.solo ? C(cBlue) : cFaint);
    }
    TFill(0, 274, kTftW, 1, cLine);
    const GlobalParams& g = eng->global;
    TText(4, 280, kFontSmall, "crossfader", cDim);
    TFill(80, 284, 200, 2, cCell);
    TFill(80 + (int)(196 * xf), 279, 5, 12, cTx);
    int a = (int)(side[1] * 100.f + 0.5f), b = (int)(side[2] * 100.f + 0.5f);
    if(a == 100 && b == 100)
        snprintf(buf, sizeof(buf), "A + B full");
    else
        snprintf(buf, sizeof(buf), "A %d%%  B %d%%", a, b);
    TText(292, 280, kFontSmall, buf, cTx);
    char f1[16], f2[16];
    FmtFilter(f1, sizeof(f1), g.djfilter);
    FmtPct(f2, sizeof(f2), g.master);
    snprintf(buf, sizeof(buf), "master %s   cue filter %s", f2, f1);
    TText(4, 296, kFontSmall, buf, cTx);
    snprintf(buf, sizeof(buf), "rev %d%%  dly fb %d%%  drive %d%%  swing %d%%", (int)(g.rev_size * 100),
             (int)(g.dly_fb * 100), (int)(g.drive * 100), (int)(g.swing * 100));
    TText(4, 308, kFontSmall, buf, cDim);
}

/* ---- 3 PATTERNS ---- */
void ViewPatterns(uint32_t now)
{
    char       buf[64];
    const int  cur = eng->CurrentPattern(), q = eng->QueuedPattern();
    const bool blink = (now / 250) & 1;
    for(int b = 0; b < 8; b++)
    {
        int y = 28 + b * 26;
        buf[0] = "ABCDEFGH"[b], buf[1] = 0;
        TText(6, y + 6, kFontSmall, buf, b == view_bank ? cTx : cDim);
        for(int p = 0; p < 8; p++)
        {
            int      i = b * 8 + p, x = 20 + p * 36;
            bool     has = !eng->PatternEmpty(i);
            uint16_t c = i == cur ? cGreen : (has ? cAmber : cCell);
            TFill(x, y, 32, 22, c);
            if(i == q && blink)
                TRect(x - 2, y - 2, 36, 26, cGreen);
            snprintf(buf, sizeof(buf), "%d", p + 1);
            TTextC(x + 16, y + 7, kFontSmall, buf, i == cur ? cBg : (has ? cTx : cFaint));
        }
    }
    TText(20, 240, kFontSmall, "green playing, outline queued, amber has steps", cDim);
    TText(316, 26, kFontSmall, "FX  row 4", cDim);
    static const char* const fx[kNumPerfFx]
        = {"repeat 1/4", "repeat 1/8", "repeat 1/16", "repeat 1/32", "tape stop", "reverse", "crush", "gate"};
    for(int f = 0; f < kNumPerfFx; f++)
    {
        int  y  = 40 + f * 24;
        bool on = fx_latched[f] || eng->PerfFxActive(f);
        TFill(316, y, 160, 20, on ? cRed : cCell);
        TText(322, y + 6, kFontSmall, fx[f], cTx);
        if(fx_latched[f])
            TTextR(470, y + 6, kFontSmall, "latched", cTx);
    }
    TFill(0, 256, kTftW, 1, cLine);
    snprintf(buf, sizeof(buf), "KITS  page %d/%d  into tracks %s", kit_page + 1, (num_kits + 4) / 5,
             track_page ? "9-16" : "1-8");
    TText(4, 262, kFontSmall, buf, cDim);
    for(int i = 0; i < 5; i++)
    {
        int k = kit_page * 5 + i, x = 4 + i * 95;
        if(k >= num_kits)
            break;
        bool loaded = k == loaded_kit_pg[track_page];
        bool loading = k == loading_kit && loading_page == track_page;
        TFill(x, 276, 90, 22, loaded ? cGreen : (loading && blink ? cAmber : cCell));
        snprintf(buf, sizeof(buf), "%d %s", i + 1, kit_names[k]);
        buf[14] = 0;
        TText(x + 5, 283, kFontSmall, buf, loaded ? cBg : cTx);
    }
    TText(4, 306, kFontSmall, "turn: queue next / previous pattern", cDim);
}

/* ---- 4 TRACK ---- */
void ViewTrack()
{
    char       buf[48];
    const int  t = sel_track, pat = eng->CurrentPattern(), step = eng->CurrentStep();
    const bool acid = eng->IsSynth(t);
    const TrackParams& tp = eng->track[t];
    TrackLabel(buf, sizeof(buf), t);
    int x = TText(6, 26, kFontBig, buf, cYellow);
    char v[16], pn[16];
    FmtPct(v, sizeof(v), tp.vol);
    FmtPan(pn, sizeof(pn), tp.pan);
    if(acid)
        snprintf(buf, sizeof(buf), "oct C%d   vol %s  pan %s", acid_oct + 2, v, pn);
    else
        snprintf(buf, sizeof(buf), "vol %s  pan %s%s%s", v, pn, tp.mute ? "  MUTED" : "",
                 tp.solo ? "  SOLO" : "");
    TText(x + 16, 32, kFontSmall, buf, cDim);
    for(int s = 0; s < kSteps; s++)
    {
        int      x0 = 6 + s * 29, y0 = 50;
        uint8_t  st = eng->GetStep(pat, t, s);
        uint16_t c  = st == kStepAccent ? cRed : (st ? cGreen : cCell);
        TFill(x0, y0, 26, 40, c);
        if(acid && st)
        {
            char nn[8];
            NoteName(nn, sizeof(nn), eng->GetNote(pat, t, s));
            nn[4] = 0;
            char a[3] = {nn[0], (char)(nn[1] == '#' ? '#' : 0), 0};
            TText(x0 + 2, y0 + 4, kFontSmall, a, cBg);
            TText(x0 + 2, y0 + 16, kFontSmall, nn + (nn[1] == '#' ? 2 : 1), cBg);
            if(eng->GetSlide(pat, t, s))
                TFill(x0 + 26, y0 + 16, 3, 6, cYellow);
        }
        if(s == held_step)
            TRect(x0 - 2, y0 - 2, 30, 44, cTx);
        if(eng->Playing() && s == step)
            TRect(x0 - 2, y0 - 2, 30, 44, cYellow);
        snprintf(buf, sizeof(buf), "%d", s + 1);
        TTextC(x0 + 13, y0 + 44, kFontSmall, buf, cFaint);
    }
    /* middle: waveform, or what the 303 is doing */
    const int wy = 108, wh = 60;
    TRect(4, wy, kTftW - 8, wh, cLine);
    if(acid)
    {
        TTextC(kTftW / 2, wy + 18, kFontBig, "303", cDim);
        TTextC(kTftW / 2, wy + 40, kFontSmall, "row 5 keys, UP/DOWN octave, SHIFT+step slide", cFaint);
    }
    else if(eng->GetSlot(t).length == 0)
        TTextC(kTftW / 2, wy + 26, kFontSmall, "empty", cDim);
    else
    {
        const int w = kTftW - 10;
        WaveScan(t, w);
        int mid = wy + wh / 2;
        for(int i = 0; i < w; i++)
        {
            int a = mid - wave_max[i] * (wh / 2 - 3) / 128, b = mid - wave_min[i] * (wh / 2 - 3) / 128;
            TFill(5 + i, a, 1, b - a + 1, cBlue);
        }
        int sx = 5 + (int)(w * tp.start); // start knob
        TFill(sx, wy + 1, 1, wh - 2, cYellow);
    }
    /* knobs */
    for(int k = 0; k < 8; k++)
    {
        const char* n;
        char        val[16];
        float       p;
        KnobText(t, k, &n, val, sizeof(val), &p);
        int cx = 60 + (k % 4) * 120, cy = 204 + (k / 4) * 70;
        TRing(cx, cy, 22, p, cBlue);
        TTextC(cx, cy - 4, kFontSmall, val, cTx);
        TTextC(cx, cy + 27, kFontSmall, n, cDim);
    }
}

/* ---- 5 SYSTEM ---- */
void ViewSystem(uint32_t now)
{
    char buf[80];
    struct Row
    {
        const char* k;
        char        v[48];
        bool        ok;
    } rows[5];
    rows[0] = {"APC40", "", plat->ControllerUp()};
    snprintf(rows[0].v, 48, "%s", plat->ControllerUp() ? "talking MIDI"
                                  : (plat->ControllerPresent() ? "plugged, not talking" : "not plugged"));
    rows[1] = {"SD", "", sd_ok};
    snprintf(rows[1].v, 48, "%s", sd_ok ? "mounted" : "no card");
    rows[2] = {"audio", "", plat->AudioRunning()};
    snprintf(rows[2].v, 48, "%s", plat->AudioRunning() ? "48 kHz running" : "stopped");
    rows[3] = {"limiter", "", eng->nan_count == 0};
    snprintf(rows[3].v, 48, "%lu hits, %lu DSP resets", (unsigned long)eng->limit_count,
             (unsigned long)eng->nan_count);
    rows[4] = {"link", "", plat->Dropped() == 0};
    snprintf(rows[4].v, 48, "%lu lines dropped", (unsigned long)plat->Dropped());
    for(int i = 0; i < 5; i++)
    {
        int y = 28 + i * 14;
        TFill(6, y + 2, 6, 6, rows[i].ok ? cGreen : cRed);
        TText(18, y, kFontSmall, rows[i].k, cDim);
        TText(72, y, kFontSmall, rows[i].v, cTx);
    }
    /* CPU graph */
    snprintf(buf, sizeof(buf), "CPU %d%% / peak %d%%", (int)(plat->CpuAvg() * 100.f),
             (int)(plat->CpuMax() * 100.f));
    TText(286, 28, kFontSmall, buf, cTx);
    const int gx = 286, gy = 42, gw = kCpuHist, gh = 56;
    TRect(gx - 1, gy - 1, gw + 2, gh + 2, cLine);
    int n = cpu_hist_n < kCpuHist ? cpu_hist_n : kCpuHist;
    for(int i = 0; i < n; i++)
    {
        int v = cpu_hist[(cpu_hist_n - n + i) % kCpuHist];
        int h = v * gh / 100;
        TFill(gx + gw - n + i, gy + gh - h, 1, h, v > 70 ? cRed : cBlue);
    }
    /* browser */
    TFill(0, 104, kTftW, 1, cLine);
    if(num_folders)
        snprintf(buf, sizeof(buf), "BROWSER  folder %d/%d  %s", cur_folder + 1, num_folders,
                 folders[cur_folder]);
    else
        snprintf(buf, sizeof(buf), "BROWSER  no /samples folder on the card");
    buf[58] = 0; // leave room for the touch button
    TText(4, 110, kFontSmall, buf, cDim);
    if(cur_file >= 0 && num_files)
        snprintf(buf, sizeof(buf), "> %s  (%d/%d)", files[cur_file], cur_file + 1, num_files);
    else
        snprintf(buf, sizeof(buf), "turn: step through samples into the track");
    buf[58] = 0;
    TText(4, 124, kFontSmall, buf, cTx);
    TFill(372, 107, 104, 28, cCell);
    TTextC(424, 111, kFontSmall, "CALIBRATE", cTx);
    TTextC(424, 122, kFontSmall, "TOUCH", cTx);
    /* log */
    TFill(0, 140, kTftW, 1, cLine);
    int nl = log_n < kLogLines ? log_n : kLogLines;
    int show = nl < 13 ? nl : 13;
    for(int i = 0; i < show; i++)
    {
        int idx = (log_n - show + i) % kLogLines;
        uint32_t s = log_time[idx] / 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu  %s", (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60), log_ring[idx]);
        buf[78] = 0;
        TText(4, 146 + i * 13, kFontSmall, buf, i >= show - 3 ? cTx : cDim);
    }
    (void)now;
}

/* ---- SETS (SHIFT + DETAIL VIEW) ---- */
void ViewSets(uint32_t now)
{
    char       buf[64];
    const bool blink = (now / 250) & 1;
    /* title: the open set */
    if(open_set >= 0)
        snprintf(buf, sizeof(buf), "open: %02d %s", open_set + 1, open_name);
    else
        snprintf(buf, sizeof(buf), "open: (not saved yet)");
    int x = TText(6, 28, kFontBig, buf, cTx);
    if(set_dirty)
        TText(x + 10, 33, kFontSmall, "* unsaved changes", cYellow);
    TFill(0, 48, kTftW, 1, cLine);

    /* left: the slots */
    const int rows = 8;
    if(sets_cur < sets_scroll)
        sets_scroll = sets_cur;
    if(sets_cur >= sets_scroll + rows)
        sets_scroll = sets_cur - rows + 1;
    for(int r = 0; r < rows && sets_scroll + r < kSetSlots; r++)
    {
        int  i  = sets_scroll + r, y = 54 + r * 26;
        bool cur = i == sets_cur;
        TFill(4, y, 236, 23, cur ? Rgb(48, 52, 60) : cCell);
        if(cur)
            TRect(4, y, 236, 23, sets_mode == kSetsList ? cYellow : cDim);
        if(i == open_set)
            TFill(8, y + 8, 6, 6, cGreen);
        snprintf(buf, sizeof(buf), "%02d", i + 1);
        TText(18, y + 7, kFontSmall, buf, cDim);
        if(set_exists[i])
        {
            TText(38, y + 7, kFontSmall, set_list[i].name, cTx);
            snprintf(buf, sizeof(buf), "%d", (int)(set_list[i].bpm + 0.5f));
            TTextR(234, y + 7, kFontSmall, buf, cDim);
        }
        else
            TText(38, y + 7, kFontSmall, "empty", cFaint);
    }
    if(kSetSlots > rows) // scroll bar
    {
        int h = 208 * rows / kSetSlots;
        TFill(242, 54 + 208 * sets_scroll / kSetSlots, 2, h, cDim);
    }

    /* right: details of the selected slot */
    const int rx = 252;
    if(set_exists[sets_cur])
    {
        const SetInfo& in = set_list[sets_cur];
        snprintf(buf, sizeof(buf), "%02d %s", sets_cur + 1, in.name);
        TText(rx, 56, kFontBig, buf, cTx);
        snprintf(buf, sizeof(buf), "%.1f bpm", in.bpm);
        TText(rx, 80, kFontSmall, buf, cTx);
        snprintf(buf, sizeof(buf), "%d samples  %d loops  %d x 303", in.samples, in.loops, in.synths);
        TText(rx, 96, kFontSmall, buf, cDim);
        snprintf(buf, sizeof(buf), "%.1f MB of sound", in.bytes / 1048576.f);
        TText(rx, 112, kFontSmall, buf, cDim);
        if(sets_cur == open_set)
            TText(rx, 132, kFontSmall, "this is the open set", cGreen);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%02d empty", sets_cur + 1);
        TText(rx, 56, kFontBig, buf, cDim);
        TText(rx, 80, kFontSmall, "SAVE HERE puts the current", cDim);
        TText(rx, 92, kFontSmall, "project in this slot", cDim);
    }
    snprintf(buf, sizeof(buf), "switch: %s at the bar line", kTransNames[set_trans]);
    TText(rx, 160, kFontSmall, buf, cDim);
    if(!sd_ok)
        TText(rx, 180, kFontSmall, "no SD card", cRed);

    /* busy: progress */
    if(set_op || load_phase)
    {
        TFill(rx, 200, 222, 50, Rgb(30, 33, 40));
        TRect(rx, 200, 222, 50, cYellow);
        TText(rx + 8, 208, kFontSmall, op_msg, cTx);
        float p = op_total ? (float)op_done / (float)op_total : 0.f;
        if(load_phase >= 2)
            p = 1.f;
        TBar(rx + 8, 226, 206, 10, p > 1.f ? 1.f : p, load_phase == 2 && blink ? cAmber : cGreen);
    }

    /* actions */
    TFill(0, 266, kTftW, 1, cLine);
    for(int a = 0; a < kNumActs; a++)
    {
        int  ax = 4 + a * 79, w = 75;
        bool on = sets_mode == kSetsActions && a == sets_act;
        uint16_t bg = on ? (a == kActDelete ? cRed : cYellow) : cCell;
        TFill(ax, 272, w, 24, bg);
        if(a == kActTrans)
            snprintf(buf, sizeof(buf), "%s", kTransNames[set_trans]);
        else
            snprintf(buf, sizeof(buf), "%s", kActNames[a]);
        TTextC(ax + w / 2, 280, kFontSmall, buf, on ? cBg : cTx);
    }
    const char* hint = "turn: pick a slot   push: actions   back: close";
    if(sets_mode == kSetsActions)
        hint = "turn: pick   push: do it   back: slots";
    else if(sets_mode == kSetsRename)
        hint = "turn: letter   push: next   back: done";
    else if(sets_mode == kSetsConfirm)
        hint = "turn: no / yes   push: confirm   back: cancel";
    TText(6, 304, kFontSmall, hint, cDim);

    /* confirm */
    if(sets_mode == kSetsConfirm)
    {
        ConfirmText(confirm_act, buf, sizeof(buf));
        int w = TTextW(kFontBig, buf) + 40;
        if(w < 260)
            w = 260;
        if(w > kTftW - 20)
            w = kTftW - 20;
        int bx = (kTftW - w) / 2;
        TFill(bx, 110, w, 90, Rgb(30, 33, 40));
        TRect(bx, 110, w, 90, cYellow);
        TTextC(kTftW / 2, 124, kFontBig, buf, cTx);
        TFill(kTftW / 2 - 110, 160, 100, 26, confirm_yes ? cCell : cTx);
        TTextC(kTftW / 2 - 60, 169, kFontSmall, "NO", confirm_yes ? cTx : cBg);
        TFill(kTftW / 2 + 10, 160, 100, 26, confirm_yes ? cRed : cCell);
        TTextC(kTftW / 2 + 60, 169, kFontSmall, "YES", cTx);
    }

    /* rename */
    if(sets_mode == kSetsRename)
    {
        TFill(40, 110, 400, 90, Rgb(30, 33, 40));
        TRect(40, 110, 400, 90, cYellow);
        snprintf(buf, sizeof(buf), "rename %02d", sets_cur + 1);
        TText(56, 120, kFontSmall, buf, cDim);
        for(int i = 0; i < kSetNameLen; i++)
        {
            int  cx = 56 + i * 23;
            char ch[2] = {rename_buf[i], 0};
            TFill(cx, 142, 20, 30, i == rename_pos ? cYellow : cCell);
            TTextC(cx + 10, 149, kFontBig, ch, i == rename_pos ? cBg : cTx);
        }
        TText(56, 182, kFontSmall, "blank = default name", cFaint);
    }
}

/* ---- popup over any view ---- */
void TftPopup(uint32_t now)
{
    if(now >= popup_until)
        return;
    int w = TTextW(kFontBig, popup) + 24;
    if(w > kTftW - 20)
        w = kTftW - 20;
    int x = (kTftW - w) / 2, y = sets_open ? 216 : 138; // below the sets list's middle
    TFill(x, y, w, 40, Rgb(30, 33, 40));
    TRect(x, y, w, 40, cYellow);
    TText(x + 12, y + 12, kFontBig, popup, cTx);
}

/** Draws the current view; returns which 16-row bands changed */
bool TouchCalActive();
void TouchCalDraw(uint32_t now);
void TouchMarkDraw(uint32_t now);

uint32_t TftCompose(uint32_t now)
{
    if(!tfb)
        return 0;
    if(now - cpu_hist_time >= 250)
    {
        cpu_hist_time = now;
        cpu_hist[cpu_hist_n++ % kCpuHist] = (uint8_t)(plat->CpuAvg() * 100.f);
    }
    TFill(0, 0, kTftW, kTftH, cBg);
    TftHeader(now);
    if(TouchCalActive())
        TouchCalDraw(now);
    else if(sets_open)
        ViewSets(now);
    else
    switch(tft_view)
    {
        case 0: ViewSteps(); break;
        case 1: ViewMixer(); break;
        case 2: ViewPatterns(now); break;
        case 3: ViewTrack(); break;
        default: ViewSystem(now); break;
    }
    TftPopup(now);
    TouchMarkDraw(now);

    static uint32_t sums[kTftBands];
    static bool     first = true;
    uint32_t        dirty = 0;
    for(int b = 0; b < kTftBands; b++)
    {
        const uint32_t* p = (const uint32_t*)(tfb + b * 16 * kTftW);
        uint32_t        h = 2166136261u;
        for(int i = 0; i < 16 * kTftW / 2; i++)
            h = (h ^ p[i]) * 16777619u;
        if(first || h != sums[b])
            dirty |= 1u << b;
        sums[b] = h;
    }
    first = false;
    return dirty;
}

/* ---- the encoder and buttons ---- */
void TftTurn(int d)
{
    if(d == 0)
        return;
    switch(tft_view)
    {
        case 2: // PATTERNS: queue the next / previous pattern
        {
            int q = eng->QueuedPattern() >= 0 ? eng->QueuedPattern() : eng->CurrentPattern();
            q += d;
            q = q < 0 ? 0 : (q > kPatterns - 1 ? kPatterns - 1 : q);
            eng->QueuePattern(q);
            view_bank = q >> 3;
            Popup("pattern %c%d", "ABCDEFGH"[q >> 3], (q & 7) + 1);
            break;
        }
        case 4: // SYSTEM: browse samples into the selected track
            browse_accum += d;
            browse_time = g_now;
            break;
        default: // pick the track
        {
            int t = sel_track + d;
            t     = t < 0 ? 0 : (t > kTracks - 1 ? kTracks - 1 : t);
            SelectTrack(t);
            char lb[40];
            TrackLabel(lb, sizeof(lb), t);
            Popup("%s", lb);
        }
    }
}
