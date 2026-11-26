/** Touch on the 3.5" screen (XPT2046, read by tft.h on the Daisy).
 *
 *  Taps only, so the encoder and the APC40 stay the main controls:
 *    header      play/stop (left), next view (name), a view (its dot)
 *    STEPS       a step: off > on > accent > off.  Track name: select it
 *    MIXER       a strip: select.  M / S: mute / solo.  A/B row: cycle
 *    PATTERNS    a pad: queue it.  FX: latch.  Kit: load into this page
 *    TRACK       a step: off > on > accent > off.  Waveform: start point
 *    SYSTEM      CALIBRATE TOUCH
 *    SETS        slots, action buttons, NO / YES, rename letters
 *
 *  Calibration: three crosses, saved to apcbox/touch.bin on the card.
 *  Starts by itself on the first touch if there's no saved one, from the
 *  SYSTEM view button, or by holding the encoder down for 3 seconds.
 *  Included by box.cpp after tftui.h.
 */
#pragma once

struct TouchCal
{
    char  magic[4]; // "TCA1"
    float a, b, c;  // x = a*rx + b*ry + c
    float d, e, f;  // y = d*rx + e*ry + f
};
/* A guess until it's calibrated: raw 150..3950 across the screen */
TouchCal tcal        = {{'T', 'C', 'A', '1'}, 480.f / 3800.f, 0.f, -150.f * 480.f / 3800.f,
                        0.f, 320.f / 3800.f, -150.f * 320.f / 3800.f};
bool     tcal_loaded = false;

int          touch_cal     = 0; // 0 off, 1..3 waiting for that cross
bool         cal_wait_up   = false;
int          cal_n         = 0;
double       cal_acc[2]    = {};
float        cal_raw[3][2] = {};
const int    kCalX[3]      = {40, 440, 240};
const int    kCalY[3]      = {40, 40, 280};
int          touch_samples = 0;
int          touch_x = -1, touch_y = -1; // last tap, for the little marker
uint32_t     touch_at = 0;

bool TouchCalActive() { return touch_cal != 0; }

void TouchLoadCal()
{
    TouchCal c;
    if(sd_ok && plat->ReadFile("apcbox/touch.bin", &c, sizeof(c)) == (int32_t)sizeof(c)
       && !memcmp(c.magic, "TCA1", 4))
    {
        tcal        = c;
        tcal_loaded = true;
        Log("touch calibration loaded");
    }
}

void TouchCalibrate(uint32_t now)
{
    g_now       = now;
    touch_cal   = 1;
    cal_wait_up = true; // the touch or hold that started it isn't a cross
    cal_n       = 0;
    cal_acc[0] = cal_acc[1] = 0;
    Log("touch calibration: tap the three crosses");
}

/** Solves x = a*rx + b*ry + c (and the same for y) through the 3 points */
const char* cal_why = "";

bool SolveCal()
{
    cal_why = "points on a line";
    const float(*r)[2] = cal_raw;
    float det = r[0][0] * (r[1][1] - r[2][1]) - r[0][1] * (r[1][0] - r[2][0])
                + (r[1][0] * r[2][1] - r[2][0] * r[1][1]);
    if(fabsf(det) < 1000.f) // points on a line: a bad tap
        return false;
    auto solve = [&](const int* v, float& a, float& b, float& c) {
        a = (v[0] * (r[1][1] - r[2][1]) - r[0][1] * (v[1] - v[2]) + (v[1] * r[2][1] - v[2] * r[1][1])) / det;
        b = (r[0][0] * (v[1] - v[2]) - v[0] * (r[1][0] - r[2][0]) + (r[1][0] * v[2] - r[2][0] * v[1])) / det;
        c = (r[0][0] * (r[1][1] * v[2] - r[2][1] * v[1]) - r[0][1] * (r[1][0] * v[2] - r[2][0] * v[1])
             + v[0] * (r[1][0] * r[2][1] - r[2][0] * r[1][1]))
            / det;
    };
    /* the crosses are far apart on screen, so the raw readings must be too */
    for(int i = 0; i < 3; i++)
        for(int j = i + 1; j < 3; j++)
            if(fabsf(r[i][0] - r[j][0]) + fabsf(r[i][1] - r[j][1]) < 600.f)
            {
                cal_why = "two crosses read almost the same spot";
                return false;
            }
    TouchCal c;
    memcpy(c.magic, "TCA1", 4);
    solve(kCalX, c.a, c.b, c.c);
    solve(kCalY, c.d, c.e, c.f);
    /* plausible: about 480 px over the panel's ~3800 raw counts, mostly on one axis.
       (Three points always fit exactly, so this is the real check.) */
    float gx = sqrtf(c.a * c.a + c.b * c.b), gy = sqrtf(c.d * c.d + c.e * c.e);
    if(gx < 0.05f || gx > 0.4f || gy < 0.03f || gy > 0.3f)
    {
        cal_why = "the scale doesn't fit the panel";
        return false;
    }
    tcal        = c;
    tcal_loaded = true;
    return true;
}

void TouchTap(int x, int y);

/** Daisy: one reading of the touch chip every ~16 ms while touched */
void TouchRaw(int rx, int ry, bool down, uint32_t now)
{
    g_now = now;
    if(touch_cal)
    {
        if(cal_wait_up)
        {
            if(!down)
                cal_wait_up = false;
            return;
        }
        if(down)
        {
            if(++cal_n > 2) // skip the first readings of each press
            {
                cal_acc[0] += rx;
                cal_acc[1] += ry;
            }
            return;
        }
        if(cal_n > 4)
        {
            cal_raw[touch_cal - 1][0] = (float)(cal_acc[0] / (cal_n - 2));
            cal_raw[touch_cal - 1][1] = (float)(cal_acc[1] / (cal_n - 2));
            Log("touch cross %d: raw x %d, y %d (%d readings)", touch_cal,
                (int)cal_raw[touch_cal - 1][0], (int)cal_raw[touch_cal - 1][1], cal_n - 2);
            if(++touch_cal > 3)
            {
                touch_cal = 0;
                if(SolveCal())
                {
                    bool saved = sd_ok && plat->WriteFile("apcbox", "apcbox/touch.bin", &tcal, sizeof(tcal));
                    Popup(saved ? "touch calibrated" : "touch calibrated (no card: not saved)");
                    Log("touch: x = %d/10000 rx %+d/10000 ry %+d, y = %d/10000 rx %+d/10000 ry %+d",
                        (int)(tcal.a * 1e4f), (int)(tcal.b * 1e4f), (int)tcal.c, (int)(tcal.d * 1e4f),
                        (int)(tcal.e * 1e4f), (int)tcal.f);
                }
                else
                {
                    Log("touch calibration refused: %s", cal_why);
                    bool same = fabsf(cal_raw[0][0] - cal_raw[1][0]) < 50 && fabsf(cal_raw[0][1] - cal_raw[1][1]) < 50
                                && fabsf(cal_raw[0][0] - cal_raw[2][0]) < 50 && fabsf(cal_raw[0][1] - cal_raw[2][1]) < 50;
                    if(same)
                        Log("touch reads the same everywhere: check T_DO, Pi pin 21 -> Seed pin 10");
                    Popup(same ? "touch reads one spot: check pin 10 wire" : "missed a cross, again");
                    TouchCalibrate(now);
                    cal_wait_up = false;
                }
            }
        }
        cal_n      = 0;
        cal_acc[0] = cal_acc[1] = 0;
        return;
    }
    if(!down)
    {
        touch_samples = 0;
        return;
    }
    if(!tcal_loaded) // first touch ever: calibrate first
    {
        TouchCalibrate(now);
        return;
    }
    /* act on the second reading: the first one of a press is often off */
    if(++touch_samples != 2)
        return;
    {
        static uint32_t last_log = 0;
        if(now - last_log > 1000)
        {
            last_log = now;
            Log("touch raw x %d, y %d", rx, ry);
        }
    }
    int x = (int)(tcal.a * rx + tcal.b * ry + tcal.c + 0.5f);
    int y = (int)(tcal.d * rx + tcal.e * ry + tcal.f + 0.5f);
    if(x < 0 || x >= kTftW || y < 0 || y >= kTftH)
        return;
    TouchTap(x, y);
}

/** A tap in screen pixels (the Daisy after calibration, the Mac build's mouse) */
void UiTouch(int x, int y, uint32_t now)
{
    g_now = now;
    TouchTap(x, y);
}

/* ---- what a tap does ---- */
void StepTap(int pat, int t, int s)
{
    uint8_t v  = eng->GetStep(pat, t, s);
    uint8_t nv = v == kStepOff ? kStepOn : (v == kStepOn ? kStepAccent : kStepOff);
    eng->SetStep(pat, t, s, nv);
    if(nv == kStepOff && eng->IsSynth(t))
        eng->SetSlide(pat, t, s, false);
}

void SetsTap(int x, int y)
{
    if(sets_mode == kSetsConfirm)
    {
        if(y >= 156 && y < 192)
        {
            if(x >= kTftW / 2 - 110 && x < kTftW / 2 - 10)
                sets_mode = kSetsActions; // NO
            else if(x >= kTftW / 2 + 10 && x < kTftW / 2 + 110)
            {
                sets_mode = kSetsActions;
                DoAct(confirm_act);
            }
        }
        return;
    }
    if(sets_mode == kSetsRename)
    {
        if(y >= 138 && y < 176 && x >= 56 && x < 56 + kSetNameLen * 23)
            rename_pos = (x - 56) / 23; // the encoder changes the letter
        else if(y < 110 || y > 200 || x < 40 || x > 440)
            RenameDone(); // outside the box: done
        return;
    }
    if(x < 244 && y >= 54 && y < 54 + 8 * 26) // a slot
    {
        int i = sets_scroll + (y - 54) / 26;
        if(i < kSetSlots)
        {
            sets_cur  = i;
            sets_mode = kSetsActions; // ready for an action button
        }
        return;
    }
    if(y >= 268 && y < 300) // action buttons
    {
        int a = (x - 4) / 79;
        if(a < 0 || a >= kTftActs)
            return;
        sets_act = a;
        if(NeedsConfirm(a))
        {
            confirm_act = a;
            confirm_yes = 0;
            sets_mode   = kSetsConfirm;
        }
        else
        {
            sets_mode = kSetsActions;
            DoAct(a);
        }
    }
}

void TouchTap(int x, int y)
{
    touch_x  = x;
    touch_y  = y;
    touch_at = g_now;
    if(touch_cal)
        return;
    /* header */
    if(y < 22)
    {
        if(sets_open)
        {
            if(x > 300)
                SetsOpen(false);
            return;
        }
        if(x < 44)
        {
            if(eng->Playing())
                eng->Stop();
            else
                eng->Play();
        }
        else if(x >= kTftW - 60)
        {
            int v = (x - (kTftW - 60)) / 11;
            tft_view = v < 0 ? 0 : (v > 4 ? 4 : v);
        }
        else if(x > 260)
            tft_view = (tft_view + 1) % 5;
        return;
    }
    if(sets_open)
    {
        SetsTap(x, y);
        return;
    }
    const int pat = eng->CurrentPattern();
    switch(tft_view)
    {
        case 0: // STEPS
        {
            for(int t = 0; t < kTracks; t++)
            {
                int ty = 25 + t * 13 + (t >= 8 ? 4 : 0);
                if(y < ty - 2 || y >= ty + 11)
                    continue;
                if(x < 102)
                    SelectTrack(t);
                else if(x >= 106 && x < 106 + 16 * 23)
                    StepTap(pat, t, (x - 106) / 23);
                return;
            }
            break;
        }
        case 1: // MIXER
        {
            if(y > 272)
                break;
            for(int t = 0; t < kTracks; t++)
            {
                int cx = 4 + t * 29 + (t >= 8 ? 8 : 0);
                if(x < cx || x >= cx + 28)
                    continue;
                TrackParams& tp = eng->track[t];
                if(y >= 246 && y < 260)
                {
                    if(x - cx < 14)
                        tp.mute = !tp.mute;
                    else
                        tp.solo = !tp.solo;
                }
                else if(y >= 260)
                {
                    tp.xfade = (uint8_t)((tp.xfade + 1) % 3);
                    Popup("track %d on %s", t + 1, tp.xfade == 0 ? "A+B" : (tp.xfade == 1 ? "A" : "B"));
                }
                else
                    SelectTrack(t);
                return;
            }
            break;
        }
        case 2: // PATTERNS
        {
            if(x >= 20 && x < 20 + 8 * 36 && y >= 28 && y < 28 + 8 * 26)
            {
                int b = (y - 28) / 26, p = (x - 20) / 36, i = b * 8 + p;
                eng->QueuePattern(i);
                view_bank = b;
                Popup("pattern %c%d", "ABCDEFGH"[b], p + 1);
            }
            else if(x >= 316 && y >= 40 && y < 40 + kNumPerfFx * 24)
            {
                int f         = (y - 40) / 24;
                fx_latched[f] = !fx_latched[f];
                eng->SetPerfFx(f, fx_latched[f]);
            }
            else if(y >= 272 && y < 300)
            {
                int i = (x - 4) / 95, k = kit_page * 5 + i;
                if(i >= 0 && i < 5 && k < num_kits)
                    LoadKit(k, true, track_page);
            }
            break;
        }
        case 3: // TRACK
        {
            if(y >= 48 && y < 92 && x >= 6 && x < 6 + 16 * 29)
                StepTap(pat, sel_track, (x - 6) / 29);
            else if(y >= 108 && y < 168 && !eng->IsSynth(sel_track) && eng->GetSlot(sel_track).length)
            {
                float s = (float)(x - 5) / (kTftW - 10);
                s       = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
                eng->track[sel_track].start = s;
                Popup("start %d%%", (int)(s * 100.f + 0.5f));
            }
            break;
        }
        default: // SYSTEM
            if(x >= 372 && y >= 106 && y < 136)
                TouchCalibrate(g_now);
            break;
    }
}

/* ---- calibration screen ---- */
void TouchCalDraw(uint32_t now)
{
    TFill(0, 0, kTftW, kTftH, cBg);
    TTextC(kTftW / 2, 120, kFontBig, "TOUCH CALIBRATION", cTx);
    char buf[48];
    snprintf(buf, sizeof(buf), "tap the cross and hold a moment (%d of 3)", touch_cal);
    TTextC(kTftW / 2, 148, kFontSmall, buf, cDim);
    TTextC(kTftW / 2, 164, kFontSmall, "a fingernail or stylus works best.  back: cancel", cFaint);
    int      cx = kCalX[touch_cal - 1], cy = kCalY[touch_cal - 1];
    uint16_t c  = ((now / 300) & 1) ? cYellow : cTx;
    TFill(cx - 14, cy, 29, 1, c);
    TFill(cx, cy - 14, 1, 29, c);
    TRect(cx - 4, cy - 4, 9, 9, c);
}

/** A small dot where the last tap landed (for half a second) */
void TouchMarkDraw(uint32_t now)
{
    if(touch_x < 0 || touch_cal || now - touch_at > 500)
        return;
    TFill(touch_x - 2, touch_y - 2, 5, 5, cYellow);
}
