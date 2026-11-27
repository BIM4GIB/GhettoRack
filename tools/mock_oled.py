"""Mockups of the bare Daisy's screen at 256x64 with 16 greys, drawn with the
box's own fonts (daisy/oledfont.h). Usage: tools/mock_oled.py daisy/oledfont.h docs/images/mock-oled-256x64.png"""
import re, sys
from PIL import Image, ImageDraw, ImageFont

src = open(sys.argv[1]).read()


def font(name, w, h):
    body = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    return w, h, [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body)]


SMALL = font("kFontSmallRows", 6, 9)
BIG = font("kFontBigRows", 10, 16)


class Scr:
    def __init__(s, w, h):
        s.w, s.h = w, h
        s.p = [[0] * w for _ in range(h)]

    def px(s, x, y, v=15):
        if 0 <= x < s.w and 0 <= y < s.h:
            s.p[y][x] = v

    def rect(s, x, y, w, h, v=15):
        for j in range(y, y + h):
            for i in range(x, x + w):
                s.px(i, j, v)

    def text(s, x, y, f, t, v=15):
        w, h, rows = f
        for ch in t:
            c = ord(ch) if 32 <= ord(ch) <= 126 else ord("?")
            g = rows[(c - 32) * h:(c - 32) * h + h]
            for r in range(h):
                for i in range(w):
                    if g[r] & (1 << (w - 1 - i)):
                        s.px(x + i, y + r, v)
            x += w
        return x

    def rtext(s, xr, y, f, t, v=15):
        s.text(xr - len(t) * f[0], y, f, t, v)

    def image(s, scale, gap):
        im = Image.new("RGB", (s.w * scale, s.h * scale), (0, 0, 0))
        d = ImageDraw.Draw(im)
        for y in range(s.h):
            for x in range(s.w):
                v = s.p[y][x]
                if v:
                    b = int(255 * (v / 15) ** 0.8)
                    d.rectangle([x * scale, y * scale, x * scale + scale - 1 - gap,
                                 y * scale + scale - 1 - gap], fill=(b, b, b))
        return im


STEPS = [2, 0, 1, 0, 1, 0, 1, 1, 2, 0, 1, 0, 1, 0, 2, 1]
PLAY = 6


def play_icon(s, x, y, v=15):
    for i in range(6):
        s.rect(x + i, y + i, 1, 11 - 2 * i, v)


def today():
    s = Scr(128, 64)
    s.text(0, 0, BIG, "A1")
    play_icon(s, 24, 3)
    s.text(36, 7, SMALL, ">A2")
    s.rtext(128, 0, BIG, "128")
    s.text(0, 18, SMALL, "3 closed hat")
    s.rtext(128, 18, SMALL, "1-8")
    s.text(0, 29, SMALL, "kit 808")
    s.rtext(128, 29, SMALL, "bank A")
    for st, v in enumerate(STEPS):
        x = st * 8 + 1
        if v == 2:
            s.rect(x, 41, 6, 9)
        elif v == 1:
            s.rect(x, 43, 6, 6)
        else:
            s.rect(x + 2, 45, 2, 2)
        if st % 4 == 0:
            s.px(x - 1, 51)
        if st == PLAY:
            s.rect(x, 52, 6, 2)
    s.text(0, 55, SMALL, "cpu 34%")
    s.rect(53, 55, 20, 9)
    s.text(54, 55, SMALL, "TRK", 0)
    s.rtext(128, 55, SMALL, "SD")
    return s


LEVEL = [9, 6, 8, 0, 5, 7, 0, 3, 6, 0, 4, 0, 7, 2, 0, 5]
MUTED = {4, 11}
SEL = 2


def main(popup=None):
    s = Scr(256, 64)
    # top: pattern, transport, queued; selected track and kit; tempo
    s.text(0, 0, BIG, "A1")
    play_icon(s, 24, 3)
    s.text(36, 7, SMALL, ">A2", 9)
    s.text(64, -1, SMALL, "3 closed hat")
    if popup:
        s.rect(62, 8, 132, 10)
        s.text(64, 8, SMALL, popup, 0)
    else:
        s.text(64, 8, SMALL, "kit 808", 8)
        s.text(118, 8, SMALL, "bank A", 8)
    s.rtext(256, 0, BIG, "128")
    # all 16 tracks: number, level, mute, which one is selected
    for t in range(16):
        x = t * 16
        n = str(t + 1)
        tx = x + (16 - len(n) * 6) // 2
        if t == SEL:
            s.rect(x + 1, 20, 14, 9)
            s.text(tx, 20, SMALL, n, 0)
        else:
            s.text(tx, 20, SMALL, n, 4 if t in MUTED else 11)
        if t in MUTED:
            s.rect(x + 3, 38, 10, 1, 3)
        else:
            lv = LEVEL[t]
            s.rect(x + 3, 39 - max(lv, 1), 10, max(lv, 1), 7 if lv else 3)
            if lv:
                s.rect(x + 3, 39 - lv, 10, 1, 15)
    s.rect(0, 40, 127, 1, 5)  # the 8 tracks the APC40 shows now
    # steps of the selected track
    for st, v in enumerate(STEPS):
        x = st * 16 + 1
        if v == 2:
            s.rect(x, 43, 14, 9)
        elif v == 1:
            s.rect(x, 45, 14, 6, 8)
        else:
            s.rect(x + 6, 47, 2, 2, 5)
        if st % 4 == 0:
            s.px(x - 1, 53, 8)
        if st == PLAY:
            s.rect(x, 53, 14, 2)
    # footer
    s.text(0, 55, SMALL, "cpu 34%", 7)
    s.rect(117, 55, 20, 9)
    s.text(118, 55, SMALL, "TRK", 0)
    s.rtext(256, 55, SMALL, "SD", 7)
    return s


SETS = [("01", "warehouse live"), ("02", "sunday acid jam"), ("03", "dub techno sketches 2"),
        ("04", "-"), ("05", "breaks with the looper")]


def sets():
    s = Scr(256, 64)
    s.text(0, -1, SMALL, "SETS *unsaved")
    for r, (n, name) in enumerate(SETS):
        y = 9 + r * 9
        line = "%s%s %s" % (">" if r == 0 else " ", n, name)
        if r == 1:
            s.rect(0, y, 162, 9)
            s.text(1, y, SMALL, line, 0)
        else:
            s.text(1, y, SMALL, line, 5 if name == "-" else 12)
    s.text(0, 55, SMALL, "row4: ld sv new del fx", 6)
    s.rect(165, 0, 1, 64, 4)
    # the status stays in view while the list is open
    x0 = 170
    s.text(x0, 0, BIG, "A1")
    play_icon(s, x0 + 24, 3)
    s.rtext(256, 0, BIG, "128")
    s.text(x0, 19, SMALL, "3 closed hat", 9)
    for st, v in enumerate(STEPS):
        x = x0 + 3 + st * 5
        if v:
            s.rect(x, 32 if v == 2 else 34, 4, 8 if v == 2 else 6, 15 if v == 2 else 8)
        else:
            s.rect(x + 1, 36, 2, 2, 5)
        if st == PLAY:
            s.rect(x, 42, 4, 2)
    s.text(x0, 46, SMALL, "switch: cut", 9)
    s.rtext(256, 55, SMALL, "SD", 7)
    return s


SC = 4  # screen pixels per OLED pixel for the 3.12" (76.8 mm wide -> 1024 px)
new = [("Main screen, 256x64", main()), ("Main screen, a knob just turned", main("filter LP 2.4k")),
       ("Set list, status stays in view", sets())]
old = today().image(SC, 0)
# 0.96" glass is 21.7 mm wide against 76.8 mm: same physical scale as the big ones
old_true = today().image(SC, 0).resize((round(1024 * 21.7 / 76.8), round(256 * 10.9 / 19.2)), Image.LANCZOS)

try:
    cap = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 22)
except OSError:
    cap = ImageFont.load_default()
M, CAPH, PAD = 40, 34, 14
H = M + sum(CAPH + 256 + 2 * PAD + 26 for _ in new) + CAPH + 256 + 2 * PAD + M
out = Image.new("RGB", (1024 + 2 * M + 2 * PAD, H), (38, 38, 40))
d = ImageDraw.Draw(out)
y = M
for title, s in new:
    d.text((M, y), title, fill=(210, 210, 210), font=cap)
    y += CAPH
    d.rectangle([M, y, M + 1024 + 2 * PAD, y + 256 + 2 * PAD], fill=(0, 0, 0))
    out.paste(s.image(SC, 0), (M + PAD, y + PAD))
    y += 256 + 2 * PAD + 26
d.text((M, y), "Today: 0.96\" 128x64, at the same physical scale (left) and enlarged (right)", fill=(210, 210, 210), font=cap)
y += CAPH
d.rectangle([M, y, M + old_true.width + 2 * PAD, y + old_true.height + 2 * PAD], fill=(0, 0, 0))
out.paste(old_true, (M + PAD, y + PAD))
x2 = M + old_true.width + 2 * PAD + 40
d.rectangle([x2, y, x2 + 512 + 2 * PAD, y + 256 + 2 * PAD], fill=(0, 0, 0))
out.paste(old, (x2 + PAD, y + PAD))
out.save(sys.argv[2])
print(out.size)
