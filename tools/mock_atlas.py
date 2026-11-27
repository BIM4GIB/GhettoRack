"""An atlas of frames for a 320x170 colour panel (the 1.9" and 1.47" ST7789
boards) on the bare Daisy: the 3.5" screen's five views, the set list and the
overlays, redrawn for this size with the box's fonts (daisy/oledfont.h) and
palette (daisy/tftui.h). No touch: encoder and back button only. The sample
data is invented. Usage: mock_atlas.py <oledfont.h> <out.png>"""
import math, re, sys
from PIL import Image, ImageDraw, ImageFont

src = open(sys.argv[1]).read()


def font(name, w, h):
    body = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    return w, h, [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body)]


SMALL = font("kFontSmallRows", 6, 9)
BIG = font("kFontBigRows", 10, 16)

BG, LINE, CELL = (12, 13, 16), (44, 47, 54), (38, 41, 47)
TX, DIM, FAINT = (228, 230, 234), (112, 117, 124), (70, 74, 80)
GREEN, RED, YELLOW = (60, 190, 90), (205, 62, 56), (242, 192, 60)
AMBER, BLUE, PURPLE = (125, 102, 40), (120, 170, 232), (180, 130, 230)
BOX, ROW = (30, 33, 40), (48, 52, 60)
W, H = 320, 170
HM = 14  # the first and last rows keep this far from the sides: the 1.47" glass has round corners


def dim(c):
    return tuple(v * 7 // 16 for v in c)


class Scr:
    def __init__(s):
        s.im = Image.new("RGB", (W, H), BG)
        s.d = ImageDraw.Draw(s.im)

    def fill(s, x, y, w, h, c):
        if w > 0 and h > 0:
            s.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def frame(s, x, y, w, h, c):
        s.d.rectangle([x, y, x + w - 1, y + h - 1], outline=c)

    def text(s, x, y, f, t, c):
        w, h, rows = f
        for ch in t:
            o = ord(ch) if 32 <= ord(ch) <= 126 else ord("?")
            g = rows[(o - 32) * h:(o - 32) * h + h]
            for r in range(h):
                for i in range(w):
                    if g[r] & (1 << (w - 1 - i)):
                        s.im.putpixel((x + i, y + r), c) if 0 <= x + i < W and 0 <= y + r < H else None
            x += w
        return x

    def rtext(s, xr, y, f, t, c):
        s.text(xr - len(t) * f[0], y, f, t, c)

    def ctext(s, xc, y, f, t, c):
        s.text(xc - len(t) * f[0] // 2, y, f, t, c)

    def bar(s, x, y, w, h, p, c):
        s.fill(x, y, w, h, CELL)
        s.fill(x, y, int(w * p), h, c)


# ---- invented state ----
NAMES = ["kick", "snare", "closed hat", "open hat", "clap", "low tom", "rim", "cowbell",
         "303", "303", "loop 4 bars", "recorded", "BD_tape", "vox chop", "stab", "noise up"]
GRID = ["2000100020001000", "0000200000002000", "2010101120101021", "0000001000000010",
        "0000200000002001", "0000000100100000", "0010000000100100", "1000000010000000",
        "2011012010110210", "0000000000000000", "1000000000000000", "0000000000000000",
        "2000000020000000", "0001000000010000", "0000000000000000", "0000000000000010"]
GRID = [[int(c) for c in r] for r in GRID]
VOL = [.9, .7, .6, .5, .7, .5, .4, .3, .8, .6, .75, 0, .9, .5, 0, .4]
PAN = [.5, .5, .65, .3, .5, .4, .7, .2, .5, .5, .5, .5, .5, .8, .5, .5]
REV = [0, .3, .1, .2, .5, .2, .1, 0, .2, 0, .3, 0, 0, .6, 0, .4]
DLY = [0, 0, .2, .3, 0, 0, .4, 0, .5, 0, 0, 0, 0, .5, 0, .2]
MUTED, SOLO, XF = {4, 11}, set(), {0: 1, 1: 1, 8: 2, 10: 2}
SYNTH = {8, 9}
NOTES = ["C2", "", "C2", "D#2", "", "C3", "G2", "", "C2", "", "A#1", "C2", "", "F2", "C3", ""]
SLIDES = {2, 10, 13}
STEP = 6


def header(s, view, name, playing=True, queued=">A2", bpm="128", mid=None, page="1-8"):
    s.text(HM, 2, BIG, "A1", TX)
    if playing:
        for i in range(12):
            s.fill(HM + 26, 3 + i, i + 1 if i < 6 else 12 - i, 1, GREEN)
    else:
        s.fill(HM + 26, 5, 9, 9, DIM)
    x = HM + 42
    if queued:
        x = s.text(x, 6, SMALL, queued, YELLOW) + 8
    if mid:
        mid(x)
    else:
        x = s.text(x, 6, SMALL, page, YELLOW) + 10
        s.text(x, 6, SMALL, name, YELLOW if view < 0 else TX)
        if view >= 0:
            for v in range(5):
                s.fill(206 + v * 9, 8, 5, 5, TX if v == view else LINE)
    s.rtext(W - HM, 2, BIG, bpm, TX)
    s.fill(0, 20, W, 1, LINE)


def popup(s, msg, y=66):
    w = len(msg) * 10 + 24
    x = (W - w) // 2
    s.fill(x, y, w, 34, BOX)
    s.frame(x, y, w, 34, YELLOW)
    s.text(x + 12, y + 9, BIG, msg, TX)


# ---- 1 STEPS: 16 tracks x 16 steps ----
def steps(sel=2, pop=None, **hd):
    s = Scr()
    header(s, 0, "STEPS", **hd)
    y0, rh, nx, cw = 23, 8, 16, 19
    for t in range(16):
        y = y0 + t * rh + (4 if t >= 8 else 0)
        off = t >= 8
        n = str(t + 1)
        if t == sel:
            s.fill(0, y - 1, 15, rh, YELLOW)
            s.rtext(14, y - 1, SMALL, n, BG)
        else:
            c = AMBER if t in MUTED else TX
            s.rtext(14, y - 1, SMALL, n, dim(c) if off and t not in MUTED else c)
        for st in range(16):
            v = GRID[t][st]
            c = RED if v == 2 else GREEN if v else CELL
            if v and (off or t in MUTED):
                c = dim(c)
            s.fill(nx + st * cw, y, cw - 2, rh - 2, c)
            if st % 4 == 0 and t == 0:
                s.fill(nx + st * cw, y0 - 2, cw - 2, 1, FAINT)
    s.frame(nx + STEP * cw - 2, y0 - 2, cw + 2, 16 * rh + 6, YELLOW)
    s.fill(0, 157, W, 1, LINE)
    s.text(HM, 160, SMALL, "%d %s" % (sel + 1, NAMES[sel]), YELLOW)
    s.rtext(W - HM, 160, SMALL, "kit 808  bank A", DIM)
    if pop:
        popup(s, pop)
    return s


# ---- 2 MIXER: 16 strips, crossfader, master ----
def mixer(sel=2, pop=None):
    s = Scr()
    header(s, 1, "MIXER")
    for t in range(16):
        x = t * 20
        off = t >= 8
        C = (lambda c: dim(c)) if off else (lambda c: c)
        s.ctext(x + 10, 24, SMALL, str(t + 1), C(YELLOW if t == sel else TX))
        if t == sel:
            s.frame(x, 22, 20, 124, YELLOW)
        top, bot = 35, 103
        s.fill(x + 6, top, 8, bot - top, CELL)
        h = int((bot - top) * VOL[t])
        s.fill(x + 6, bot - h, 8, h, C(AMBER if t in MUTED else GREEN))
        s.fill(x + 3, 109, 14, 2, CELL)
        s.fill(x + 3 + int(12 * PAN[t]), 107, 2, 6, C(TX))
        s.bar(x + 3, 116, 14, 3, REV[t], C(BLUE))
        s.bar(x + 3, 121, 14, 3, DLY[t], C(PURPLE))
        s.text(x + 3, 126, SMALL, "M", C(AMBER) if t in MUTED else FAINT)
        s.text(x + 11, 126, SMALL, "S", C(BLUE) if t in SOLO else FAINT)
        if t in XF:
            s.ctext(x + 10, 136, SMALL, "A" if XF[t] == 1 else "B", C(YELLOW))
    s.fill(159, 24, 1, 120, LINE)
    s.fill(0, 148, W, 1, LINE)
    s.text(HM, 155, SMALL, "A", YELLOW)
    s.fill(HM + 10, 159, 110, 2, CELL)
    s.fill(HM + 10 + 70, 154, 4, 12, TX)
    s.text(HM + 124, 155, SMALL, "B", YELLOW)
    s.text(HM + 138, 155, SMALL, "A 60%", TX)
    s.rtext(W - HM, 155, SMALL, "master 80%", TX)
    if pop:
        popup(s, pop)
    return s


# ---- 3 PATTERNS: banks A-H x 8, performance FX, kits ----
HAS = {0, 1, 2, 3, 8, 9, 10, 16, 17, 24, 40, 41, 42, 43}
FXN = ["RPT1/4", "RPT1/8", "RPT1/16", "RPT1/32", "TAPE", "REV", "CRUSH", "GATE"]
KITS = ["808", "909", "acoustic", "lofi", "techno"]


def patterns(cur=0, q=1, fx_on=(), latched=()):
    s = Scr()
    header(s, 2, "PATTERNS")
    for b in range(8):
        y = 24 + b * 16
        s.text(4 if b else HM - 8, y + 3, SMALL, "ABCDEFGH"[b], TX if b == 0 else DIM)
        for p in range(8):
            i, x = b * 8 + p, 16 + p * 23
            has = i in HAS
            s.fill(x, y, 21, 14, GREEN if i == cur else AMBER if has else CELL)
            if i == q:
                s.frame(x - 1, y - 1, 23, 16, YELLOW)
            s.ctext(x + 11, y + 3, SMALL, str(p + 1), BG if i == cur else TX if has else FAINT)
    s.text(HM, 158, SMALL, "turn: queue next / previous", DIM)
    x0 = 206
    s.text(x0, 24, SMALL, "FX row 4", DIM)
    for f in range(8):
        x, y = x0 + (f % 2) * 55, 35 + (f // 2) * 15
        on = f in fx_on or f in latched
        s.fill(x, y, 53, 13, RED if on else CELL)
        s.text(x + 3, y + 2, SMALL, FXN[f], TX)
        if f in latched:
            s.fill(x + 48, y + 2, 3, 3, TX)
    s.fill(x0, 97, W - x0, 1, LINE)
    s.text(x0, 101, SMALL, "KITS 1/3 > 1-8", DIM)
    for i, k in enumerate(KITS):
        y = 112 + i * 11
        s.fill(x0, y, 108, 10, GREEN if i == 0 else CELL)
        s.text(x0 + 3, y, SMALL, "%d %s" % (i + 1, k), BG if i == 0 else TX)
    return s


# ---- 4 TRACK: the selected track ----
def wave(n):
    out = []
    for i in range(n):
        p = i / n
        e = math.exp(-p * 7) + 0.25 * math.exp(-((p - .45) ** 2) * 300) + 0.12 * math.exp(-((p - .7) ** 2) * 500)
        out.append(min(1.0, e) * (0.75 + 0.25 * math.sin(i * 1.7) * math.sin(i * 0.31)))
    return out


def track(t=2, rec=None, **hd):
    s = Scr()
    header(s, 3, "TRACK", **hd)
    acid = t in SYNTH
    s.text(4, 24, BIG, "%d %s" % (t + 1, NAMES[t]), YELLOW)
    info = ("oct C2  " if acid else "") + "vol %d%% pan C" % int(VOL[t] * 100)
    s.rtext(W - 4, 29, SMALL, info, DIM)
    for st in range(16):
        x, y = 2 + st * 20, 44
        v = GRID[t][st]
        s.fill(x, y, 18, 24, RED if v == 2 else GREEN if v else CELL)
        if acid and v:
            nn = NOTES[st] or "C2"
            s.text(x + 2, y + 2, SMALL, nn[:-1], BG)
            s.text(x + 2, y + 13, SMALL, nn[-1], BG)
            if st in SLIDES:
                s.fill(x + 18, y + 9, 2, 6, YELLOW)
        if st == STEP:
            s.frame(x - 1, y - 1, 20, 26, YELLOW)
    wy, wh = 73, 42
    s.frame(2, wy, W - 4, wh, RED if rec else LINE)
    if acid:
        s.ctext(W // 2, wy + 6, BIG, "303", DIM)
        s.ctext(W // 2, wy + 27, SMALL, "row 5 keys, UP/DOWN octave, SHIFT+step slide", FAINT)
    else:
        w = W - 6
        n = int(w * rec) if rec else w
        for i, a in enumerate(wave(w)[:n]):
            h = int(a * (wh // 2 - 3))
            s.fill(3 + i, wy + wh // 2 - h, 1, 2 * h + 1, RED if rec else BLUE)
        if not rec:
            s.fill(3 + int(w * 0.04), wy + 1, 1, wh - 2, YELLOW)
    names = (["cutoff", "reso", "env mod", "decay", "accent", "tune", "slide", "wave"] if acid else
             ["pitch", "decay", "filter", "reso", "start", "reverse", "drive", "prob"])
    vals = (["1.2k", "62%", "48%", "30%", "70%", "0", "60ms", "saw"] if acid else
            ["0", "100%", "LP 2.4k", "20%", "4%", "off", "10%", "100%"])
    ps = [.55, .62, .48, .3, .7, .5, .4, 0] if acid else [.5, 1, .6, .2, .04, 0, .1, 1]
    for k in range(8):
        kx, ky = 6 + (k % 4) * 78, 121 + (k // 4) * 24
        s.text(kx, ky, SMALL, names[k][:max(1, 11 - len(vals[k]))], DIM)
        s.rtext(kx + 72, ky, SMALL, vals[k], TX)
        s.bar(kx, ky + 11, 72, 4, ps[k], BLUE)
    return s


# ---- 5 SYSTEM: status, CPU, browser, log ----
def system(trouble=False):
    s = Scr()
    header(s, 4, "SYSTEM", playing=not trouble, queued=None if trouble else ">A2")
    rows = [("APC40", "not plugged" if trouble else "talking MIDI", not trouble),
            ("SD", "no card" if trouble else "mounted", not trouble),
            ("audio", "48 kHz running", True),
            ("limiter", "3 hits, 0 resets", True),
            ("link", "0 lines dropped", True)]
    for i, (k, v, ok) in enumerate(rows):
        y = 24 + i * 12
        s.fill(4, y + 2, 6, 6, GREEN if ok else RED)
        s.text(14, y, SMALL, k, DIM)
        s.text(62, y, SMALL, v, TX if ok else RED)
    s.text(206, 24, SMALL, "CPU 34% peak 41%", TX)
    gx, gy, gw, gh = 206, 36, 108, 46
    s.frame(gx - 1, gy - 1, gw + 2, gh + 2, LINE)
    for i in range(gw):
        v = 30 + 8 * math.sin(i * .21) + 5 * math.sin(i * .83) + (38 if 70 < i < 76 else 0)
        h = int(v * gh / 100)
        s.fill(gx + i, gy + gh - h, 1, h, RED if v > 70 else BLUE)
    s.fill(0, 86, W, 1, LINE)
    if trouble:
        s.text(4, 90, SMALL, "BROWSER  no /samples folder on the card", DIM)
        s.text(4, 101, SMALL, "turn: step through samples into the track", FAINT)
    else:
        s.text(4, 90, SMALL, "BROWSER  folder 2/5  breaks", DIM)
        s.text(4, 101, SMALL, "> amen_170.wav  (4/23)", TX)
    s.fill(0, 113, W, 1, LINE)
    log = (["00:00  ApcBox v13.33", "00:01  no SD card", "00:01  waiting for the APC40", "00:09  still waiting"] if trouble else
           ["03:12  set 02 loaded, 412 ms", "03:40  kit 808 into tracks 1-8", "04:02  rec 11: 4 bars", "04:31  saved"])
    for i, l in enumerate(log):
        s.text(6, 117 + i * 12, SMALL, l, TX if i >= 2 else DIM)
    return s


# ---- SETS ----
SETS = ["warehouse live", "sunday acid jam", "dub sketches 2", None, "breaks + looper", None, None, "kitchen 303"]
ACTS = ["LOAD", "SAVE HERE", "SAVE NEW", "RENAME", "DELETE", "FX: CUT", "DEFAULT"]


def sets(cur=1, mode="list", act=0, confirm=None, yes=False, rename=None, busy=None):
    s = Scr()
    header(s, -1, "SETS *unsaved", queued=None)
    for r, name in enumerate(SETS):
        y = 23 + r * 18
        sel = r == cur
        s.fill(0, y, 198, 17, ROW if sel else BG)
        if sel:
            s.frame(0, y, 198, 17, YELLOW if mode == "list" else DIM)
        x0 = HM if r == 7 else 4
        if r == 0:
            s.fill(x0, y + 5, 5, 7, GREEN)
        s.text(x0 + 8, y + 4, SMALL, "%02d" % (r + 1), DIM)
        s.text(x0 + 24, y, BIG, name or "empty", TX if name else FAINT)
    rx = 204
    s.fill(200, 22, 1, H - 22, LINE)
    if mode == "list":
        name = SETS[cur]
        s.text(rx, 25, SMALL, "%02d" % (cur + 1), YELLOW)
        if name:
            for i, l in enumerate(["126.0 bpm", "9 samples", "2 loops, 2 x 303", "14.2 MB of sound"]):
                s.text(rx, 38 + i * 12, SMALL, l, TX if i == 0 else DIM)
        else:
            s.text(rx, 38, SMALL, "SAVE HERE puts", DIM)
            s.text(rx, 50, SMALL, "the project here", DIM)
        s.text(rx, 94, SMALL, "switch: CUT", DIM)
        s.text(rx, 106, SMALL, "at the bar line", DIM)
        s.text(rx, 134, SMALL, "turn: pick a slot", FAINT)
        s.text(rx, 146, SMALL, "push: actions", FAINT)
        s.text(rx, 158, SMALL, "back: close", FAINT)
    else:
        for a, n in enumerate(ACTS):
            y = 24 + a * 15
            on = a == act
            s.fill(rx, y, 112, 13, (RED if a == 4 else YELLOW) if on else CELL)
            s.text(rx + 4, y + 2, SMALL, n, (TX if a == 4 else BG) if on else TX)
        s.text(rx, 134, SMALL, "turn: pick", FAINT)
        s.text(rx, 146, SMALL, "push: do it", FAINT)
        s.text(rx, 158, SMALL, "back: slots", FAINT)
    if busy is not None:
        s.fill(rx - 2, 118, 114, 50, BOX)
        s.frame(rx - 2, 118, 114, 50, YELLOW)
        s.text(rx + 4, 124, SMALL, "loading 5 of 9", TX)
        s.bar(rx + 4, 140, 102, 10, busy, GREEN)
    if confirm:
        w = max(max(len(l) for l in confirm) * 10 + 28, 224)
        bx = (W - w) // 2
        s.fill(bx, 42, w, 96, BOX)
        s.frame(bx, 42, w, 96, YELLOW)
        for i, l in enumerate(confirm):
            s.ctext(W // 2, 52 + i * 20, BIG, l, TX)
        s.fill(W // 2 - 100, 104, 92, 24, CELL if yes else TX)
        s.ctext(W // 2 - 54, 108, BIG, "NO", TX if yes else BG)
        s.fill(W // 2 + 8, 104, 92, 24, RED if yes else CELL)
        s.ctext(W // 2 + 54, 108, BIG, "YES", TX)
    if rename is not None:
        txt, pos = rename
        s.fill(4, 44, 312, 92, BOX)
        s.frame(4, 44, 312, 92, YELLOW)
        s.text(14, 52, SMALL, "rename %02d" % (cur + 1), DIM)
        for i in range(16):
            cx = 9 + i * 19
            ch = txt[i] if i < len(txt) else " "
            s.fill(cx, 68, 17, 28, YELLOW if i == pos else CELL)
            s.ctext(cx + 9, 74, BIG, ch, BG if i == pos else TX)
        s.text(14, 104, SMALL, "turn: letter  push: next  back: done", DIM)
        s.text(14, 118, SMALL, "blank = default name", FAINT)
    return s


# ---- header states and overlays ----
def sampling():
    s = track(10, rec=0.4, queued=None, mid=lambda x: None)
    x = s.text(HM + 42, 6, SMALL, "SAMPLING 11:2", RED)
    s.bar(x + 6, 8, 34, 5, 0.7, GREEN)
    return s


def recording():
    s = steps(sel=2, queued=None, mid=lambda x: None)
    x = s.text(HM + 42, 6, SMALL, "REC", RED)
    s.text(x + 10, 6, SMALL, "1-8", YELLOW)
    s.text(x + 38, 6, SMALL, "STEPS", TX)
    for v in range(5):
        s.fill(206 + v * 9, 8, 5, 5, TX if v == 0 else LINE)
    return s


def overload():
    s = track(8)
    msg = "OVERLOAD: 1 of 2 303s out"
    w = len(msg) * 10 + 24
    x = (W - w) // 2
    s.fill(x, 26, w, 32, RED)
    s.frame(x, 26, w, 32, TX)
    s.text(x + 12, 34, BIG, msg, TX)
    return s


frames = [
    ("1  STEPS", steps()),
    ("2  MIXER", mixer()),
    ("3  PATTERNS, A2 queued, a repeat held", patterns(fx_on=(2,), latched=(4,))),
    ("4  TRACK, a sample", track(2)),
    ("4  TRACK, a 303", track(8)),
    ("5  SYSTEM", system()),
    ("SETS, the list", sets()),
    ("SETS, actions", sets(mode="act", act=3)),
    ("SETS, a name", sets(mode="act", act=3, rename=("sunday acid jam", 7))),
    ("SETS, confirm", sets(cur=2, mode="act", act=4, confirm=["delete 03", "dub sketches 2?"], yes=True)),
    ("SETS, loading", sets(mode="act", act=0, busy=0.55)),
    ("A knob turned, over any view", mixer(pop="filter LP 2.4k")),
    ("Step recording", recording()),
    ("Sampling a loop into track 11", sampling()),
    ("Overload", overload()),
    ("Stopped, no APC40, no card", system(trouble=True)),
]

SC, COLS = 2, 3
try:
    cap = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 20)
    capb = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 26)
except OSError:
    cap = capb = ImageFont.load_default()
M, CAPH, PAD, GAP = 40, 32, 10, 26
pw, ph = W * SC + 2 * PAD, H * SC + 2 * PAD
rows = (len(frames) + COLS - 1) // COLS
top = M + 44
# the last row of the sheet: the first frame on both panels at the same physical scale
R19, R147 = 640, round(640 * 32.8 / 42.7)
out = Image.new("RGB", (2 * M + COLS * pw + (COLS - 1) * GAP, top + rows * (CAPH + ph + GAP) + CAPH + ph + M), (38, 38, 40))
d = ImageDraw.Draw(out)
d.text((M, M - 6), "Bare Daisy on a 320x170 colour panel: every frame, encoder and back button only", fill=(235, 235, 235), font=capb)
for i, (title, s) in enumerate(frames):
    x = M + (i % COLS) * (pw + GAP)
    y = top + (i // COLS) * (CAPH + ph + GAP)
    d.text((x, y), title, fill=(210, 210, 210), font=cap)
    d.rectangle([x, y + CAPH, x + pw - 1, y + CAPH + ph - 1], fill=BG)
    out.paste(s.im.resize((W * SC, H * SC), Image.NEAREST), (x + PAD, y + CAPH + PAD))
# pick the remaining cells of the last row for the size comparison
used = len(frames) % COLS
y = top + (rows - 1) * (CAPH + ph + GAP) if used else top + rows * (CAPH + ph + GAP)
x = M + used * (pw + GAP)
first = frames[0][1].im
d.text((x, y), "1.9\" (left) and 1.47\" (right, round corners guessed) at the same physical scale", fill=(210, 210, 210), font=cap)
d.rectangle([x, y + CAPH, x + pw - 1, y + CAPH + ph - 1], fill=BG)
out.paste(first.resize((R19, R19 * H // W), Image.NEAREST), (x + PAD, y + CAPH + PAD))
x2 = x + pw + GAP
small = first.resize((R147, R147 * H // W), Image.LANCZOS)
mask = Image.new("L", small.size, 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, small.width - 1, small.height - 1], radius=round(22 * R147 / W), fill=255)
d.rectangle([x2, y + CAPH, x2 + R147 + 2 * PAD - 1, y + CAPH + small.height + 2 * PAD - 1], fill=(0, 0, 0))
out.paste(small, (x2 + PAD, y + CAPH + PAD), mask)
out = out.crop((0, 0, out.width, y + CAPH + ph + M))
out.save(sys.argv[2])
print(out.size)
