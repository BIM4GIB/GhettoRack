"""The clock module: one tempo for everything on the T95.

Sources: internal (tempo set on the page), Link (Ableton Link via the
optional aalink package), or MIDI clock from any port (Cubase over network
MIDI, a drum machine over USB). Outputs: MIDI clock (24 per beat) plus start,
stop, continue and song position to the ports ticked on the page, and the
tempo to Link.

Following MIDI clock: incoming ticks are timestamped and fitted to a straight
line (least squares over the last 96), and our ticks go out on that line, so
Wi-Fi jitter on the way in isn't passed on. If the source stops ticking, the
last tempo is held.

The Daisy, both ways (firmware v13.11): tick "daisy" under "send clock to"
and every sixteenth it gets an ESC T frame (tempo, position, playing, how
long frames take); its sequencer stays on its audio clock and steers itself
onto ours. Or pick the Daisy as the source: it reports each step with how
long ago it fired (CL lines), and our ticks go out on its grid.

The ticks come from one thread that sleeps to just before each deadline and
spins the rest, with the GIL switch interval lowered so the web server can't
hold it for long. Lateness of every tick is measured and shown. The Daisy's
own sequencer keeps its own clock; following this one is a firmware step.
"""
from .common import *
from .util import *

try:
    import aalink                      # Ableton Link (optional): pip install aalink
except ImportError:
    aalink = None


def bpm_ok(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and CLOCK_BPM_MIN <= v <= CLOCK_BPM_MAX


def fit_line(pts):
    """Least squares t = a + b*i over [(i, t)]; returns (a, b, residual rms)"""
    n = len(pts)
    mi = sum(i for i, _ in pts) / n
    mt = sum(t for _, t in pts) / n
    sii = sum((i - mi) ** 2 for i, _ in pts)
    if sii == 0:
        return None
    b = sum((i - mi) * (t - mt) for i, t in pts) / sii
    a = mt - b * mi
    rms = (sum((t - (a + b * i)) ** 2 for i, t in pts) / n) ** 0.5
    return a, b, rms


class Clock:
    def __init__(self, bridge, saved):
        self.b = bridge
        self.lock = threading.Lock()           # held around tick writes; port close takes it too
        self.on = saved.get("on") is True
        self.source = saved.get("source") if isinstance(saved.get("source"), str) else "internal"
        self.bpm = float(saved["bpm"]) if bpm_ok(saved.get("bpm")) else 120.0
        self.outputs = [o for o in saved.get("outputs", []) if isinstance(o, str)][:16]
        self.send_stopped = saved.get("send_stopped", True) is not False
        self.link_on = saved.get("link") is True
        self.playing = False
        self.pos = 0                           # ticks since song start (6 per 16th)
        self.cmds = collections.deque()        # transport messages for the thread to send first
        self.wake = threading.Event()
        # following MIDI clock
        self.in_ticks = collections.deque(maxlen=CLOCK_FIT)
        self.in_n = 0
        self.in_last = None
        self.in_bpm = None
        self.in_jitter_ms = None
        self.model = None                      # (a, b) while locked
        self.off_line = 0
        # the Daisy: following it (CL lines) and what it says about following us (CK)
        self.d_tick = 0
        self.d_sent_playing = False
        self.d_bars = 0
        self.d_step = -1
        self.d_playing = False
        self.d_bpm = None
        self.daisy_follow = None               # (following us, error ms) from its CK line
        self.loop = None
        # Link
        self.link = None
        self.link_err = None
        # stats
        self.late = collections.deque(maxlen=480)
        self.sent = self.dropped = 0
        self.thread = None
        self.stop_thread = False

    def settings(self):
        return {"on": self.on, "source": self.source, "bpm": round(self.bpm, 2), "outputs": self.outputs,
                "send_stopped": self.send_stopped, "link": self.link_on}

    def info(self):
        late = sorted(self.late)
        st = {"late_ms": None}
        if late:
            st["late_ms"] = {"avg": round(sum(late) / len(late), 3), "p99": round(late[int(len(late) * 0.99)], 3),
                             "max": round(late[-1], 3), "count": len(late)}
        locked = self.source not in ("internal", "link") and self.model is not None and self.in_last is not None \
            and time.monotonic() - self.in_last < self.lost_s()
        sixteenths = self.pos // 6
        return dict(self.settings(), playing=self.playing, tempo=round(self.tempo(), 2),
                    position="%d.%d.%d" % (sixteenths // 16 + 1, sixteenths // 4 % 4 + 1, sixteenths % 4 + 1),
                    following=self.source not in ("internal", "link"), locked=locked,
                    in_ticks=self.in_n, in_jitter_ms=self.in_jitter_ms, sent=self.sent, dropped=self.dropped,
                    link_available=aalink is not None, link_error=self.link_err,
                    link_peers=self.link.num_peers if self.link is not None else None,
                    daisy_ok=self.b.fd is not None and self.b.ctl >= 6,
                    daisy_following=bool(self.daisy_follow and self.daisy_follow[0]),
                    daisy_err_ms=self.daisy_follow[1] if self.daisy_follow and self.daisy_follow[0] else None, **st)

    def tempo(self):
        if self.source == "link" and self.link is not None:
            return self.link.tempo
        if self.source not in ("internal", "link") and self.in_bpm:
            return self.in_bpm
        return self.bpm

    # ---- from the bridge (asyncio thread) ----

    def set(self, d):
        if "bpm" in d:
            if not bpm_ok(d["bpm"]):
                return 400, {"error": "bpm %g-%g" % (CLOCK_BPM_MIN, CLOCK_BPM_MAX)}
            self.bpm = float(d["bpm"])
            if self.link is not None and self.source != "link":
                self.link.tempo = self.bpm
        if "source" in d:
            s = d["source"]
            if not isinstance(s, str) or not 0 < len(s) <= 40:
                return 400, {"error": "source: internal, link or a port name"}
            if s == "link" and aalink is None:
                return 400, {"error": "Link needs the aalink package (install.sh tries to add it)"}
            if s == "daisy" and self.b.ctl < 6 and self.b.fd is not None:
                return 400, {"error": "following the Daisy needs firmware v13.11"}
            if s != self.source:
                self.source = s
                self.d_playing = False          # the Daisy's next step report starts us if it's playing
                self.in_ticks.clear()
                self.model = None
                self.in_bpm = None
                if self.playing:                # the new source starts it again
                    self.cmds.append(b"\xfc")
        if "outputs" in d:
            o = d["outputs"]
            if not isinstance(o, list) or len(o) > 16 or not all(isinstance(x, str) and len(x) <= 40 for x in o):
                return 400, {"error": "outputs: list of port names"}
            self.outputs = o
        for k, attr in (("on", "on"), ("send_stopped", "send_stopped"), ("link", "link_on")):
            if k in d:
                if not isinstance(d[k], bool):
                    return 400, {"error": "%s: true/false" % k}
                setattr(self, attr, d[k])
        if self.source == "link":
            self.link_on = True
        self.apply_link()
        self.b.state.data["clock"] = self.settings()
        self.b.state.save()
        self.wake.set()
        return 200, {"ok": True}

    def transport(self, cmd):
        """Start, stop, continue from the page or the remote (internal and Link sources)"""
        if self.source not in ("internal", "link"):
            return 400, {"error": "following %s: transport comes from there" % self.source}
        if cmd == "start":
            self.cmds.append(b"\xfa")
        elif cmd == "stop":
            self.cmds.append(b"\xfc")
        elif cmd == "continue":
            self.cmds.append(b"\xfb")
        elif cmd == "toggle":
            self.cmds.append(b"\xfc" if self.playing else b"\xfa")
        else:
            return 400, {"error": "start, stop, continue or toggle"}
        self.wake.set()
        return 200, {"ok": True}

    def feed(self, msg):
        """A clock message from the source port (timestamped now)"""
        t = time.monotonic()
        s = msg[0]
        if s == 0xF8:
            if self.in_last is not None and t - self.in_last > CLOCK_LOST_S:
                self.in_ticks.clear()           # a gap: don't fit across it
            self.in_n += 1
            self.in_last = t
            if self.model is not None and len(self.in_ticks) >= 24:
                a, b = self.model
                err = abs(t - (a + b * self.in_n))
                # off the line by more than jitter explains, several ticks running: a tempo change
                if err > max(0.004, 4 * (self.in_jitter_ms or 0) / 1000.0):
                    self.off_line += 1
                    if self.off_line >= 4:
                        keep = list(self.in_ticks)[-3:]
                        self.in_ticks.clear()
                        self.in_ticks.extend(keep)
                        self.off_line = 0
                else:
                    self.off_line = 0
            self.in_ticks.append((self.in_n, t))
            if len(self.in_ticks) >= 12:
                f = fit_line(list(self.in_ticks))
                if f and f[1] > 0:
                    bpm = 60.0 / (f[1] * CLOCK_PPQ)
                    if CLOCK_BPM_MIN <= bpm <= CLOCK_BPM_MAX:
                        self.model = (f[0], f[1])
                        self.in_bpm = bpm
                        self.in_jitter_ms = round(f[2] * 1000, 3)
        elif s in (0xFA, 0xFB, 0xFC, 0xF2):
            self.cmds.append(bytes(msg))        # passed on in order, before the next tick
            self.wake.set()

    def lost_s(self):
        return 2.0 if self.source == "daisy" else CLOCK_LOST_S     # the Daisy reports every other sixteenth

    def feed_daisy(self, line):
        """A CL line: 'CL playing step bpm100 age_us n'. Even steps only (swing moves the odd ones)."""
        t = time.monotonic()
        try:
            _, playing, step, bpm100, age, _n = line.split()
            playing, step, bpm, age = playing == "1", int(step), int(bpm100) / 100.0, int(age) / 1e6
        except ValueError:
            return
        if self.source != "daisy":
            self.d_playing = playing
            return
        lat = (self.b.ping_stats().get("avg") or 0.0) / 2000.0
        if playing != self.d_playing:
            self.d_playing = playing
            if playing:
                self.in_ticks.clear()
                self.d_bars, self.d_step = 0, -1
                sixteenth = step if age < 0.2 else 0
                self.cmds.append(b"\xfa" if sixteenth == 0 else bytes([0xF2, sixteenth & 0x7F, 0]) )
                if sixteenth:
                    self.cmds.append(b"\xfb")
            else:
                self.cmds.append(b"\xfc")
            self.wake.set()
        if not playing or not CLOCK_BPM_MIN <= bpm <= CLOCK_BPM_MAX:
            return
        if step < self.d_step:
            self.d_bars += 1
        self.d_step = step
        if self.d_bpm != bpm:                    # tempo changed: start the fit again
            self.d_bpm = bpm
            self.in_ticks.clear()
        if step & 1:
            return
        idx = (self.d_bars * 16 + step) * 6
        self.in_n += 1
        self.in_last = t
        self.in_ticks.append((idx, t - age - lat))
        pts = list(self.in_ticks)[-32:]
        b = 60.0 / (bpm * CLOCK_PPQ)            # the Daisy says its tempo: only the phase is fitted
        a = sum(tt - b * i for i, tt in pts) / len(pts)
        self.model = (a, b)
        self.in_bpm = bpm
        if len(pts) > 2:
            self.in_jitter_ms = round((sum((tt - (a + b * i)) ** 2 for i, tt in pts) / len(pts)) ** 0.5 * 1000, 3)

    def daisy_ck(self, line):
        try:
            _, follow, err = line.split()
            self.daisy_follow = (follow == "1", int(err) / 10.0)
        except ValueError:
            pass

    def to_daisy(self, pos, playing):
        """In the asyncio thread: one ESC T frame (the clock thread asked for it)"""
        b = self.b
        if b.fd is None or b.ctl < 6:
            return
        lat_us = int((b.ping_stats().get("avg") or 0.0) * 500)
        b.ctl_send(b"%sT %d %d %d %d\n" % (ESC, int(round(self.tempo() * 100)), pos, 1 if playing else 0, lat_us))

    def apply_link(self):
        if aalink is None or not (self.link_on and self.on):
            if self.link is not None:
                self.link.enabled = False
            return
        try:
            if self.link is None:
                self.link = aalink.Link(self.bpm)
                self.link.quantum = 4
            self.link.enabled = True
            if self.source != "link":
                self.link.tempo = self.bpm
            self.link_err = None
        except Exception as e:                  # a Link failure must not take the bridge down
            self.link_err = str(e)[:200]
            log.warning("Link: %s", e)

    def start_thread(self):
        self.loop = asyncio.get_running_loop()
        sys.setswitchinterval(0.001)           # the web server can't hold the GIL for 5 ms
        self.thread = threading.Thread(target=self.run, name="clock", daemon=True)
        self.thread.start()

    # ---- the clock thread ----

    def out_ports(self):
        b = self.b
        return [p for p in list(b.ports.values()) if p.name in self.outputs and p.name != self.source]

    def send(self, msg, ports):
        for p in ports:
            if getattr(p, "net", False):
                ok = self.b.net_send_threadsafe(p, msg)
            else:
                try:
                    ok = os.write(p.fd, msg) == len(msg)
                except (BlockingIOError, OSError, TypeError):
                    ok = False
            if ok:
                self.sent += 1
            else:
                self.dropped += 1

    def next_deadline(self, last, now):
        """When the next tick goes out (monotonic seconds)"""
        src = self.source
        if src == "link" and self.link is not None:
            try:
                beat, tempo = self.link.beat, self.link.tempo
            except Exception:
                return now + 60.0 / (self.bpm * CLOCK_PPQ)
            period = 60.0 / (tempo * CLOCK_PPQ)
            ticks = beat * CLOCK_PPQ
            due = now + (math.floor(ticks) + 1 - ticks) * period
            while last and due < last + 0.5 * period:   # never the same tick twice
                due += period
            return due
        if src not in ("internal", "link") and self.model is not None and self.in_last is not None \
                and now - self.in_last < self.lost_s():
            a, b = self.model
            j = math.floor((now - a) / b) + 1    # the next tick on the fitted line
            while last and a + j * b < last + 0.5 * b:   # the line moved a little: never the same tick twice
                j += 1
            return a + j * b
        period = 60.0 / (self.tempo() * CLOCK_PPQ)
        return last + period if last and now - last < 0.5 else now + period

    def run(self):
        last = 0.0
        while not self.stop_thread:
            now = time.monotonic()
            if not self.on:
                self.wake.wait(0.2)
                self.wake.clear()
                last = 0.0
                continue
            due = self.next_deadline(last, now)
            # sleep until close, waking early for transport messages
            while True:
                now = time.monotonic()
                left = due - now - CLOCK_SPIN_S
                if left <= 0 or self.cmds:
                    break
                if self.wake.wait(min(left, 0.05)):
                    self.wake.clear()
                    if self.cmds:
                        break
                    due = self.next_deadline(last, time.monotonic())
            with self.lock:
                ports = self.out_ports()
                while self.cmds:
                    m = self.cmds.popleft()
                    if m == b"\xfa":
                        self.playing, self.pos = True, 0
                    elif m == b"\xfb":
                        self.playing = True
                    elif m == b"\xfc":
                        self.playing = False
                    elif m[0] == 0xF2 and len(m) == 3:
                        self.pos = (m[1] | m[2] << 7) * 6
                    self.send(m, ports)
                    if self.link is not None and m in (b"\xfa", b"\xfc"):
                        try:
                            self.link.playing = m == b"\xfa"
                        except Exception:
                            pass
                if time.monotonic() < due - CLOCK_SPIN_S:
                    continue                    # woke for a transport message; tick later
                while time.monotonic() < due:
                    pass
                sent_at = time.monotonic()
                if self.playing or self.send_stopped:
                    self.send(b"\xf8", ports)
                # the Daisy: a frame every sixteenth (and at once when play/stop changes), from the asyncio thread
                if "daisy" in self.outputs and self.source != "daisy":
                    self.d_tick = (self.d_tick + 1) % 6 if not self.playing else self.pos % 6
                    if self.d_tick == 0 or self.playing != self.d_sent_playing:
                        self.d_sent_playing = self.playing
                        self.loop.call_soon_threadsafe(self.to_daisy, self.pos, self.playing)
                if self.playing:
                    self.pos += 1
            self.late.append((sent_at - due) * 1000.0)
            last = due
