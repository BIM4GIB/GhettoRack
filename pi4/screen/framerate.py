#!/usr/bin/env python3
"""How fast the kiosk's page really gets drawn on the 3.5" screen, view by view:
the gap between the page's frames and what goes over SPI meanwhile.
   python3 pi4/screen/framerate.py [seconds per view]
Leaves the page on the view it found."""
import json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shot import Page

S = "/sys/class/spi_master/spi0/statistics/"
rd = lambda n: int(open(S + n).read())


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
    p = Page()
    was = p.eval("curName")
    hz = int.from_bytes(open(next(d for d in ("/proc/device-tree/soc/spi@7e204000/grscreen@0/spi-max-frequency",) if os.path.exists(d)), "rb").read(), "big")
    print("SPI clock asked for: %.1f MHz   playing: %s   zoom: %s" % (hz / 1e6, p.eval("st.playing"), p.eval("smZoom")))
    for v in ("sm_steps", "sm_track", "sm_mix", "sm_pat", "sm_lib", "sm_sys"):
        p.eval("show(%s); 1" % json.dumps(v))
        time.sleep(1)
        p.eval("(() => { window.__f = []; let last = performance.now(); window.__go = true;"
               " const f = t => { window.__f.push(Math.round(t - last)); last = t; if (window.__go) requestAnimationFrame(f); };"
               " requestAnimationFrame(f); return 1; })()")
        b, m = rd("bytes_tx"), rd("messages")
        time.sleep(secs)
        f = sorted(json.loads(p.eval("window.__go = false; JSON.stringify(window.__f)"))[1:])
        b2, m2 = rd("bytes_tx"), rd("messages")
        flushes = max(1, (m2 - m) / 6)          # a flush is three commands, each a command and a data message
        print("%-9s %4.1f frames/s, gap median %3d ms, worst %4d ms | SPI %4d kB/s, %4.1f flushes/s of %3d kB" % (
            v, len(f) / secs, f[len(f) // 2] if f else -1, f[-1] if f else -1, (b2 - b) / secs / 1000, flushes / secs, (b2 - b) / flushes / 1000))
    p.eval("show(%s); 1" % json.dumps(was))


if __name__ == "__main__":
    main()
