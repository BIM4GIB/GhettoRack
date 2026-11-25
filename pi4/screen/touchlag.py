#!/usr/bin/env python3
"""Where a tap's time goes on the 3.5" screen: from the touch chip's event in
the kernel, to the page's handlers, to the next frame the page draws.
   python3 pi4/screen/touchlag.py [seconds]      then tap a few things
Reads the touch device (group input) and the kiosk's page (DevTools)."""
import glob, json, os, select, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shot import Page

EV = struct.Struct("qqHHi")
HOOK = """(() => { if (window.__lag) { window.__lag.length = 0; return 1; }
  const L = window.__lag = [], ep = t => Math.round(performance.timeOrigin + t);
  for (const t of ['pointerdown', 'pointerup', 'click', 'touchstart', 'touchend'])
    document.addEventListener(t, e => { const now = Date.now(), r = [t, now, ep(e.timeStamp), -1];
      L.push(r); requestAnimationFrame(() => requestAnimationFrame(() => { r[3] = Date.now(); })); }, true);
  return 1; })()"""


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    dev = next("/dev/input/" + n.split("/")[4] for n in glob.glob("/sys/class/input/event*/device/name") if "ADS7846" in open(n).read())
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    page = Page()
    page.eval(HOOK)
    print("tap now, for %d s" % secs, flush=True)
    kern, end = [], time.time() + secs
    while time.time() < end:
        if not select.select([fd], [], [], 0.3)[0]:
            continue
        got = time.time() * 1000
        data = os.read(fd, EV.size * 64)
        for i in range(0, len(data) - EV.size + 1, EV.size):
            s, us, typ, code, val = EV.unpack_from(data, i)
            if typ == 1 and code == 330:
                kern.append(("down" if val else "up", s * 1000 + us // 1000, round(got)))
    time.sleep(0.5)
    lag = json.loads(page.eval("JSON.stringify(window.__lag)"))
    downs = [k for k in kern if k[0] == "down"]
    ups = [k for k in kern if k[0] == "up"]
    print("%d presses seen by the kernel, %d events in the page" % (len(downs), len(lag)))
    for d in downs:
        u = next((x for x in ups if x[1] >= d[1]), None)
        line = "press at %d: held %s ms" % (d[1] % 100000, u[1] - d[1] if u else "?")
        for name in ("touchstart", "pointerdown", "pointerup", "click"):
            e = next((x for x in lag if x[0] == name and x[1] >= d[1] - 50), None)
            if e:
                line += " | %s +%d (browser stamp +%d)" % (name, e[1] - d[1], e[2] - d[1])
                if name == "click":
                    line += " | drawn +%d" % (e[3] - d[1]) if e[3] > 0 else " | not drawn"
        print(line)


if __name__ == "__main__":
    main()
