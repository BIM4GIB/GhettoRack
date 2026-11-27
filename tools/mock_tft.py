"""Mockups of the bare Daisy's screen on a 2.4" 320x240 colour panel, with the
box's fonts (daisy/oledfont.h) and palette (daisy/tftui.h), colours used as
docs/CANON.md section 6 says. Usage: tools/mock_tft.py daisy/oledfont.h docs/images/mock-oled-256x64.png docs/images/mock-tft-320x240.png"""
import re, sys
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
AMBER, BLUE = (125, 102, 40), (120, 170, 232)
W, H = 320, 240


class Scr:
    def __init__(s):
        s.im = Image.new("RGB", (W, H), BG)
        s.d = ImageDraw.Draw(s.im)

    def fill(s, x, y, w, h, c):
        s.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def frame(s, x, y, w, h, c):
        s.d.rectangle([x, y, x + w - 1, y + h - 1], outline=c)

    def text(s, x, y, f, t, c, sc=1):
        w, h, rows = f
        for ch in t:
            o = ord(ch) if 32 <= ord(ch) <= 126 else ord("?")
            g = rows[(o - 32) * h:(o - 32) * h + h]
            for r in range(h):
                for i in range(w):
                    if g[r] & (1 << (w - 1 - i)):
                        s.fill(x + i * sc, y + r * sc, sc, sc, c)
            x += w * sc
        return x

    def rtext(s, xr, y, f, t, c, sc=1):
        s.text(xr - len(t) * f[0] * sc, y, f, t, c, sc)

    def ctext(s, xc, y, f, t, c, sc=1):
        s.text(xc - len(t) * f[0] * sc // 2, y, f, t, c, sc)


STEPS = [2, 0, 1, 0, 1, 0, 1, 1, 2, 0, 1, 0, 1, 0, 2, 1]
PLAY = 6
LEVEL = [36, 24, 31, 0, 20, 27, 0, 12, 24, 0, 16, 0, 39, 8, 0, 20]
MUTED = {4, 11}
SOLO = set()
SEL = 2


def status(s):
    """Top strip: pattern, transport, queued pattern, tempo."""
    s.text(4, 2, BIG, "A1", TX, 2)
    for i in range(12):  # play triangle
        s.fill(50 + i, 6 + i, 1, 24 - 2 * i, GREEN)
    s.text(70, 14, SMALL, ">A2", YELLOW, 2)
    s.rtext(W - 4, 2, BIG, "128", TX, 2)
    s.fill(0, 37, W, 1, LINE)


def main(popup=None):
    s = Scr()
    status(s)
    s.text(4, 43, SMALL, "3 closed hat", TX, 2)
    if popup:
        s.fill(0, 64, W, 22, YELLOW)
        s.text(4, 66, SMALL, popup, BG, 2)
    else:
        s.text(4, 70, SMALL, "kit 808", DIM)
        s.rtext(W - 4, 70, SMALL, "bank A", DIM)
    # 16 tracks: number, level, mute
    for t in range(16):
        x = t * 20
        col = AMBER if t in MUTED else (YELLOW if t == SEL else TX)
        s.ctext(x + 10, 92, SMALL, str(t + 1), col)
        s.fill(x + 4, 104, 12, 40, CELL)
        if t in MUTED:
            s.ctext(x + 10, 120, SMALL, "M", AMBER)
        elif LEVEL[t]:
            lv = LEVEL[t]
            s.fill(x + 4, 144 - lv, 12, lv, GREEN)
            if lv > 36:
                s.fill(x + 4, 104, 12, 4, RED)
        if t == SEL:
            s.frame(x + 1, 89, 18, 58, YELLOW)
    s.fill(0, 150, 159, 2, DIM)  # the 8 tracks under the APC40's faders
    # steps of the selected track
    for st, v in enumerate(STEPS):
        x = st * 20 + 1
        s.fill(x, 160, 18, 34, RED if v == 2 else GREEN if v else CELL)
        if st % 4 == 0:
            s.fill(x, 197, 18, 1, FAINT)
        if st == PLAY:
            s.fill(x, 200, 18, 4, YELLOW)
    # footer
    s.fill(0, 213, W, 1, LINE)
    s.text(4, 220, SMALL, "cpu 34%", DIM, 1)
    s.frame(127, 216, 66, 22, DIM)
    s.ctext(160, 218, SMALL, "TRK", TX, 2)
    s.fill(W - 26, 222, 6, 6, GREEN)
    s.rtext(W - 4, 220, SMALL, "SD", DIM)
    return s.im


SETS = ["warehouse live", "sunday acid jam", "dub techno sketches 2", "-",
        "breaks with the looper", "-", "-", "kitchen 303"]


def sets():
    s = Scr()
    status(s)
    s.text(4, 43, SMALL, "SETS", YELLOW)
    s.text(40, 43, SMALL, "* unsaved", YELLOW)
    s.rtext(W - 4, 43, SMALL, "switch: cut", DIM)
    for r, name in enumerate(SETS):
        y = 56 + r * 19
        empty = name == "-"
        if r == 0:
            s.fill(3, y + 5, 6, 8, GREEN)  # the open set
        s.text(12, y, SMALL, "%02d" % (r + 1), DIM if empty else TX, 2)
        s.text(44, y, SMALL, name, FAINT if empty else TX, 2)
        if r == 1:
            s.frame(0, y - 1, W, 20, YELLOW)
    s.fill(0, 213, W, 1, LINE)
    s.text(4, 220, SMALL, "row4: ld sv new del fx", DIM)
    s.fill(W - 26, 222, 6, 6, GREEN)
    s.rtext(W - 4, 220, SMALL, "SD", DIM)
    return s.im


SC = 2  # 2 screen pixels per panel pixel: 48.96 mm -> 640 px, the scale of the OLED image (76.8 mm -> 1024 px)
shots = [("Main screen, 320x240", main()), ("A knob just turned", main("filter LP 2.4k")),
         ("Set list", sets())]
oled = Image.open(sys.argv[2]).crop((54, 88, 54 + 1024, 88 + 256))

try:
    cap = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
except OSError:
    cap = ImageFont.load_default()
M, CAPH, PAD, GAP = 40, 34, 14, 30
pw, ph = W * SC + 2 * PAD, H * SC + 2 * PAD
out = Image.new("RGB", (2 * M + 3 * pw + 2 * GAP, 2 * M + 2 * CAPH + ph + GAP + 256 + 2 * PAD), (38, 38, 40))
d = ImageDraw.Draw(out)
for i, (title, im) in enumerate(shots):
    x = M + i * (pw + GAP)
    d.text((x, M), title, fill=(210, 210, 210), font=cap)
    d.rectangle([x, M + CAPH, x + pw - 1, M + CAPH + ph - 1], fill=BG)
    out.paste(im.resize((W * SC, H * SC), Image.NEAREST), (x + PAD, M + CAPH + PAD))
y = M + CAPH + ph + GAP
d.text((M, y), "The 3.12\" 256x64 OLED at the same physical scale", fill=(210, 210, 210), font=cap)
y += CAPH
d.rectangle([M, y, M + 1024 + 2 * PAD - 1, y + 256 + 2 * PAD - 1], fill=(0, 0, 0))
out.paste(oled, (M + PAD, y + PAD))
out.save(sys.argv[3])
print(out.size)
