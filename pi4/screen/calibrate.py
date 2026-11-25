#!/usr/bin/env python3
"""Touch calibration for the 3.5" screen on the Pi's header.
   sudo python3 pi4/screen/calibrate.py

Shows five crosses on the kiosk's page, one after the other. Tap each with
the stylus or fingernail you will play with. It reads the touch chip's own
numbers (not what the page gets, so an old calibration doesn't matter), fits
them to where the crosses were, writes /etc/udev/rules.d/98-ghettorack-touch.rules
and restarts the kiosk. --dry: work it out and print it, change nothing.
--wait N: seconds to wait for each tap (40)."""
import glob, json, os, select, statistics, struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shot import Page

W, H = 480, 320
TARGETS = [(40, 40), (440, 40), (440, 280), (40, 280), (240, 160)]
RULE = "/etc/udev/rules.d/98-ghettorack-touch.rules"
EV = struct.Struct("qqHHi")     # struct input_event on a 64-bit kernel

OVERLAY = """((x, y, msg) => { let o = document.getElementById('gr-cal');
  if (!o) { o = document.createElement('div'); o.id = 'gr-cal'; document.body.appendChild(o);
    o.style.cssText = 'position:fixed;left:0;top:0;width:480px;height:320px;z-index:99999;background:#000;color:#fff;font:14px monospace;touch-action:none';
    for (const t of ['pointerdown', 'pointerup', 'click']) o.addEventListener(t, e => { e.stopPropagation(); e.preventDefault(); }, true); }
  const line = (l, t, w, h) => '<div style="position:absolute;left:' + l + 'px;top:' + t + 'px;width:' + w + 'px;height:' + h + 'px;background:#fc0"></div>';
  o.innerHTML = '<div style="position:absolute;left:0;right:0;top:' + (x < 0 ? 140 : y > 160 ? 90 : 210) + 'px;text-align:center">' + msg + '</div>'
    + (x < 0 ? '' : line(x - 20, y, 41, 1) + line(x, y - 20, 1, 41));
  return 1; })(%d, %d, %s)"""


def overlay(page, x, y, msg):
    page.eval(OVERLAY % (x, y, json.dumps(msg)))


def touch_device():
    for n in glob.glob("/sys/class/input/event*/device/name"):
        if "ADS7846" in open(n).read():
            return "/dev/input/" + n.split("/")[4]
    sys.exit("no ADS7846 touch device: is the screen's overlay loaded?")


def one_touch(fd, timeout=40):
    """Raw (x, y) of one touch: the middle of what the chip said while it was down"""
    xs, ys, end = [], [], time.time() + timeout
    while time.time() < end:
        if not select.select([fd], [], [], 0.5)[0]:
            continue
        data = os.read(fd, EV.size * 64)
        for i in range(0, len(data) - EV.size + 1, EV.size):
            _, _, typ, code, val = EV.unpack_from(data, i)
            if typ == 3 and code == 0:
                xs.append(val)
            elif typ == 3 and code == 1:
                ys.append(val)
            elif typ == 1 and code == 330 and val == 0:
                if len(xs) >= 6 and len(ys) >= 6:       # a real press, not a graze
                    cut = len(xs) // 4
                    return statistics.median(xs[cut:len(xs) - cut or None]), statistics.median(ys[cut:len(ys) - cut or None])
                xs, ys = [], []
    return None


def fit(points, values):
    """a, b, c with a*x + b*y + c = value, least squares over (x, y) points"""
    n = len(points)
    sx = sum(p[0] for p in points); sy = sum(p[1] for p in points)
    sxx = sum(p[0] * p[0] for p in points); syy = sum(p[1] * p[1] for p in points); sxy = sum(p[0] * p[1] for p in points)
    sv = sum(values); sxv = sum(p[0] * v for p, v in zip(points, values)); syv = sum(p[1] * v for p, v in zip(points, values))
    m = [[sxx, sxy, sx, sxv], [sxy, syy, sy, syv], [sx, sy, n, sv]]
    for i in range(3):                                   # Gauss-Jordan on a 3x3
        piv = max(range(i, 3), key=lambda r: abs(m[r][i]))
        m[i], m[piv] = m[piv], m[i]
        if abs(m[i][i]) < 1e-12:
            sys.exit("the taps don't span the screen: try again")
        m[i] = [v / m[i][i] for v in m[i]]
        for r in range(3):
            if r != i:
                m[r] = [a - m[r][i] * b for a, b in zip(m[r], m[i])]
    return m[0][3], m[1][3], m[2][3]


def main():
    dry = "--dry" in sys.argv
    wait = int(sys.argv[sys.argv.index("--wait") + 1]) if "--wait" in sys.argv else 40
    if not dry and os.geteuid() != 0:
        sys.exit("run with sudo (it writes a udev rule), or with --dry")
    page = Page()
    size = page.eval("[innerWidth, innerHeight]")
    if not size or size[1] != H or not W <= size[0] <= W + 40:
        sys.exit("the kiosk's page is %s, not %dx%d: is it on the 3.5\" screen?" % (size, W, H))
    fd = os.open(touch_device(), os.O_RDONLY | os.O_NONBLOCK)
    raw = []
    try:
        for i, (x, y) in enumerate(TARGETS):
            overlay(page, x, y, "tap the cross (%d of %d)" % (i + 1, len(TARGETS)))
            time.sleep(0.6)
            while select.select([fd], [], [], 0)[0]:     # what was touched before the cross was there
                os.read(fd, 4096)
            t = one_touch(fd, wait)
            if t is None:
                sys.exit("no tap on cross %d within %d s: nothing changed" % (i + 1, wait))
            raw.append((t[0] / 4095, t[1] / 4095))
            print("cross %d at %3d,%3d: chip says %4d,%4d" % (i + 1, x, y, t[0], t[1]), flush=True)
        ax = fit(raw, [t[0] / W for t in TARGETS])
        ay = fit(raw, [t[1] / H for t in TARGETS])
        err = [((ax[0] * r[0] + ax[1] * r[1] + ax[2]) * W - t[0], (ay[0] * r[0] + ay[1] * r[1] + ay[2]) * H - t[1]) for r, t in zip(raw, TARGETS)]
        worst = max(max(abs(e[0]), abs(e[1])) for e in err)
        matrix = " ".join("%.5f" % v for v in ax + ay)
        print("matrix: " + matrix)
        print("left over at the crosses, px: " + "  ".join("%+.1f,%+.1f" % e for e in err))
        if worst > 12:
            overlay(page, -1, -1, "taps were off by %d px: not saved, run it again" % worst)
            time.sleep(3)
            sys.exit("worst %.0f px: a tap missed its cross. Nothing changed, run it again" % worst)
        if dry:
            return
        with open(RULE, "w") as f:
            f.write("# GhettoRack: touch on the SunFounder 3.5 inch screen, from pi4/screen/calibrate.py\n"
                    "# (%s, five crosses, worst %.1f px left over)\n" % (time.strftime("%Y-%m-%d"), worst)
                    + 'ACTION=="add|change", SUBSYSTEM=="input", ATTRS{name}=="ADS7846 Touchscreen", '
                    + 'ENV{LIBINPUT_CALIBRATION_MATRIX}="%s"\n' % matrix)
        overlay(page, -1, -1, "saved. the screen restarts")
        subprocess.run(["udevadm", "control", "--reload"], check=True)
        subprocess.run(["udevadm", "trigger", "--action=change", "--subsystem-match=input"], check=True)
        time.sleep(1)
        subprocess.run(["systemctl", "restart", "kiosk"], check=True)      # the compositor reads it when it starts
        print("saved to " + RULE + ", kiosk restarted")
    finally:
        try:
            if dry or len(raw) < len(TARGETS):
                page.eval("(() => { const o = document.getElementById('gr-cal'); if (o) o.remove(); return 1; })()")
        except Exception:
            pass


if __name__ == "__main__":
    main()
