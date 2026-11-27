"""A game controller on the box's USB (seen: a Sony PlayStation 3 pad, 2026-11-26):
its buttons are the remote's actions for the pages or jobs on the Daisy, its sticks
and triggers sweep the Daisy and spring back. What each control does is a table
(PAD_DEFAULT) that a page can change: GET /pad, POST /pad, kept in state.json.

Buttons with a page's job go to the pages on the box's own screen as the same "ir"
events a remote makes, so a page needs nothing new (to every open page only when
the box shows none). Jobs on the Daisy go to the Daisy once, not through the pages:
two open pages would each act.

A stick or trigger adds to what is there and lets go of it again: pushed, the control
moves from its value towards its end; let go, it is what it was (the master filter and
the crossfader: their middle). So any knob can sit on a stick, with or without a middle.

Nothing here is timing: a sweep is at most GP_RATE_S apart and the Daisy plays."""
from .common import *
from .util import *

EV_KEY, EV_ABS = 0x01, 0x03
# what a button can do. For the pages: the remote's actions. For the Daisy: the rest
PAD_PAGE_JOBS = ["up", "down", "left", "right", "ok", "back", "home", "menu", "prev", "next"]
PAD_BTN_JOBS = (PAD_PAGE_JOBS + ["play", "stop", "toggle", "trig", "pat-", "pat+"]
                + ["t%d" % n for n in range(1, 17)] + ["none"])
# what a stick axis or a trigger can do: (glob / knob, its number); "trig": the selected track's sound, once per pull
PAD_AXIS = {"filter": ("glob", 9), "xfade": ("glob", 1), "t.vol": ("knob", 0), "t.pan": ("knob", 1),
            "t.rev": ("knob", 2), "t.dly": ("knob", 3), "t.pitch": ("knob", 4), "t.filter": ("knob", 6)}
PAD_AXIS_JOBS = list(PAD_AXIS) + ["trig", "none"]
PAD_BTNS = [544, 545, 546, 547, 304, 305, 307, 308, 310, 311, 314, 315, 316, 317, 318]   # D-pad, X O triangle square, L1 R1, select start PS, L3 R3
PAD_AXES = [0, 1, 3, 4, 2, 5]                                                             # LX LY RX RY, L2 R2
PAD_DEFAULT = {"btn": {"544": "up", "545": "down", "546": "left", "547": "right",
                       "304": "ok", "305": "back", "307": "menu", "308": "none", "316": "home",
                       "310": "prev", "311": "next", "315": "play", "314": "stop", "317": "none", "318": "none"},
               "axis": {"0": "filter", "1": "t.filter", "3": "xfade", "4": "t.pitch", "2": "trig", "5": "trig"}}
GP_REPEAT = {"up", "down", "left", "right"}
GP_DEAD = 12             # of 63 either way from the middle: a worn stick rests anywhere in there
GP_RATE_S = 0.04         # a control is sent at most this often while a stick moves
GP_SHOW_S = 0.1          # ... and shown on a page's drawing at most this often
GP_PULL = 60             # of 255: how far a trigger is pulled before it plays, and half of it to let go
GP_TRIGGERS = (2, 5)


def pad_valid(d):
    """the changes a state file or a page may hold: {"btn": {code: job}, "axis": {code: job}}, bad entries dropped"""
    out = {"btn": {}, "axis": {}}
    if isinstance(d, dict):
        for kind, codes, jobs in (("btn", PAD_BTNS, PAD_BTN_JOBS), ("axis", PAD_AXES, PAD_AXIS_JOBS)):
            for k, v in (d.get(kind) or {}).items() if isinstance(d.get(kind), dict) else ():
                if isinstance(k, str) and k.isdigit() and int(k) in codes and v in jobs and PAD_DEFAULT[kind][k] != v:
                    out[kind][k] = v
    return out


def find_pad(root=""):
    """(event device, name) of the first game controller: it has a south button and two stick axes"""
    for d in sorted(glob.glob(root + "/sys/class/input/event*/device")):
        try:
            key = open(os.path.join(d, "capabilities", "key")).read().split()
            ab = int(open(os.path.join(d, "capabilities", "abs")).read().split()[-1], 16)
            bits = 0
            for w in key:
                bits = (bits << 64) | int(w, 16)
            if bits >> 304 & 1 and ab & 3 == 3:
                return ("/dev/input/" + os.path.basename(os.path.dirname(d)),
                        open(os.path.join(d, "name")).read().strip())
        except (OSError, ValueError, IndexError):
            pass
    return None, None


def stick(v):
    """0..255 from the pad -> 0..127 with the middle held at exactly 64"""
    x = round((v - 127.5) / 127.5 * 63.5)
    if abs(x) <= GP_DEAD:
        return 64
    x = (abs(x) - GP_DEAD) / (63.5 - GP_DEAD) * 63       # the travel left after the dead zone is the whole range
    return max(0, min(127, round(64 + (x if v > 127 else -x))))


class Gamepad:
    def __init__(self, bridge):
        self.b = bridge
        self.fd = None
        self.path = self.name = None
        self.buf = b""
        self.down = {}              # button code -> its repeat timer (or None)
        self.axis = {}              # axis -> {"d": -1..1 wanted, "sent": last value sent, "at": when, "job", "base": (track or None, value)}
        self.pulled = set()
        self.shown = {}             # axis -> (when, d) last told to the pages' drawing
        self.timer = None
        self.presses = self.sent = self.dropped = 0

    def info(self):
        return {"device": self.path, "name": self.name, "presses": self.presses, "sent": self.sent, "dropped": self.dropped}

    # ---- the table ----
    def map(self):
        ch = self.b.state.data.get("pad") or {}
        return {k: dict(PAD_DEFAULT[k], **(ch.get(k) or {})) for k in ("btn", "axis")}

    def describe(self):
        return dict(self.info(), map=self.map(), default=PAD_DEFAULT,
                    jobs={"btn": PAD_BTN_JOBS, "axis": PAD_AXIS_JOBS}, buttons=PAD_BTNS, axes=PAD_AXES)

    def set(self, d):
        """POST /pad: {"btn": code, "job": j}, {"axis": code, "job": j} or {"reset": true}"""
        st = self.b.state
        cur = pad_valid(st.data.get("pad"))
        if d.get("reset") is True:
            cur = {"btn": {}, "axis": {}}
        else:
            kind = "btn" if "btn" in d else "axis" if "axis" in d else None
            code, job = d.get(kind) if kind else None, d.get("job")
            if (kind is None or not isinstance(code, int) or isinstance(code, bool)
                    or code not in (PAD_BTNS if kind == "btn" else PAD_AXES)
                    or job not in (PAD_BTN_JOBS if kind == "btn" else PAD_AXIS_JOBS)):
                return 400, {"error": "btn or axis (a control of the pad) and job (one of its jobs), or reset"}
            cur[kind].pop(str(code), None)
            if PAD_DEFAULT[kind][str(code)] != job:
                cur[kind][str(code)] = job
        self.let_go()                   # a stick held while its job changes: what it moved goes back first
        st.data["pad"] = cur
        st.save()
        self.b.broadcast(sse_event("padmap", self.map()))
        return 200, {"ok": True, "map": self.map()}

    # ---- buttons ----
    def s_field(self, i):
        try:
            return int(self.b.cache.lines["S"].split()[1 + i])
        except (KeyError, IndexError, ValueError):
            return None

    def selected(self):
        """the Daisy's selected track, 0-15, or None while it hasn't said"""
        t = self.s_field(7)
        return None if t is None else max(0, min(15, t))

    def act(self, job):
        if job == "none":
            return
        if job in PAD_PAGE_JOBS:
            # The pad is plugged into the box, so it drives the box's own screen. Only with no page
            # open there does it go to the others: a phone left on the jam recorder started a take
            # at every X while the screen was on ApcBox (rig, 2026-11-26)
            clients = list(self.b.clients)
            own = [c for c in clients if c.peer and c.peer[0] in ("127.0.0.1", "::1")]
            for c in (own if self.b.args.pad_local and own else clients):
                c.push(sse_event("ir", {"action": job, "phase": "down", "pad": True}))
        elif job in ("play", "stop"):
            self.send(job)
        elif job == "toggle":
            self.send("stop" if self.s_field(0) else "play")
        elif job == "trig":
            t = self.selected()
            if t is not None:
                self.send("audition %d" % (t + 1))
        elif job in ("pat-", "pat+"):
            p = self.s_field(2)
            if p is not None:
                self.send("pat %d" % (max(0, min(63, p + (1 if job == "pat+" else -1))) + 1))
        elif job[0] == "t" and job[1:].isdigit():
            self.send("track %s" % job[1:])

    def repeat(self, code, job):
        if code not in self.down:
            return
        self.act(job)
        self.down[code] = asyncio.get_running_loop().call_later(IR_REPEAT_S, self.repeat, code, job)

    def key(self, code, value):
        if code not in PAD_BTNS or value == 2:       # 2: the kernel's own repeat
            return
        t = self.down.pop(code, None)
        if t:
            t.cancel()
        self.b.broadcast(sse_event("pad", {"b": code, "on": value == 1}))      # for a page's drawing
        if value == 1:
            self.presses += 1
            job = self.map()["btn"].get(str(code), "none")
            self.act(job)
            self.down[code] = (asyncio.get_running_loop().call_later(IR_HOLD_S, self.repeat, code, job)
                               if job in GP_REPEAT else None)

    # ---- sticks and triggers ----
    def send(self, action):
        if self.b.ctl >= 4 and self.b.ui_send(action):
            self.sent += 1
            return True
        self.dropped += 1
        return False

    def knob_now(self, track, k):
        try:
            return max(0, min(127, int(self.b.cache.lines["T %d" % track].split()[2 + k])))
        except (KeyError, IndexError, ValueError):
            return None

    def show(self, code, d):
        t, was = now(), self.shown.get(code)
        if was and was[1] == d:
            return
        if d == 0 or not was or t - was[0] >= GP_SHOW_S:
            self.shown[code] = (t, d)
            self.b.broadcast(sse_event("pad", {"a": code, "d": round(d, 2)}))

    def moved(self, code, value):
        if code not in PAD_AXES:
            return
        if code in GP_TRIGGERS:
            d = 0.0 if value < 8 else min(1.0, value / 255.0)
        else:
            v = stick(255 - value if code in (1, 4) else value)        # up is more
            d = (v - 64) / (63.0 if v > 64 else 64.0)
        self.show(code, d)
        job = self.map()["axis"].get(str(code), "none")
        if job == "trig":
            pull = value if code in GP_TRIGGERS else abs(d) * 255
            if pull >= GP_PULL and code not in self.pulled:
                self.pulled.add(code)
                self.act("trig")
            elif pull < GP_PULL // 2:
                self.pulled.discard(code)
            return
        if job not in PAD_AXIS:
            return
        what, k = PAD_AXIS[job]
        a = self.axis.get(code)
        if a is None:
            if d == 0:
                return
            if what == "glob":
                base = (None, 64)
            else:                                                   # whose knob, and what it was: put back on letting go
                t = self.selected()
                v0 = None if t is None else self.knob_now(t, k)
                if v0 is None:
                    return
                base = (t, v0)
            a = self.axis[code] = {"sent": None, "at": 0.0, "job": job, "base": base}
        a["d"] = d
        self.flush()

    def flush(self):
        """send what the sticks want, each control at most every GP_RATE_S; the last value always goes"""
        self.timer = None
        t, wait = now(), None
        for code, a in list(self.axis.items()):
            what, k = PAD_AXIS[a["job"]]
            tr, base = a["base"]
            d, home = a["d"], a["d"] == 0
            want = round(base + d * (127 - base)) if d >= 0 else round(base + d * base)
            if want != a["sent"] or home:
                due = a["at"] + GP_RATE_S - t
                if due > 0 and not home:
                    wait = due if wait is None else min(wait, due)
                    continue
                if home and a["sent"] is None:
                    pass                                            # never moved it: nothing to put back
                elif what == "glob":
                    self.send("glob %d %d" % (k, want))
                else:
                    self.send("knob %d %d %d" % (tr + 1, k, want))
                a["sent"], a["at"] = want, t
            if home:
                del self.axis[code]
        if wait is not None:
            self.timer = asyncio.get_running_loop().call_later(wait, self.flush)

    def let_go(self):
        """every stick as if released: what it moved goes back"""
        for a in self.axis.values():
            a["d"] = 0
        if self.timer:
            self.timer.cancel()
        self.flush()
        self.pulled = set()

    # ---- the device ----
    def on_readable(self):
        try:
            data = os.read(self.fd, IR_EVENT.size * 64)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            self.close()
            return
        self.buf += data
        while len(self.buf) >= IR_EVENT.size:
            _, _, typ, code, value = IR_EVENT.unpack_from(self.buf)
            self.buf = self.buf[IR_EVENT.size:]
            if typ == EV_KEY:
                self.key(code, value)
            elif typ == EV_ABS:
                self.moved(code, value)

    def close(self):
        if self.fd is None:
            return
        asyncio.get_running_loop().remove_reader(self.fd)
        try:
            os.close(self.fd)
        except OSError:
            pass
        log.info("game controller %s gone", self.name)
        for t in self.down.values():
            if t:
                t.cancel()
        self.down = {}
        self.let_go()                           # a stick held when the cable went
        self.fd = self.path = self.name = None
        self.buf = b""

    def open(self, path, name):
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError as e:
            if getattr(self, "said", None) != path:
                self.said = path
                log.warning("game controller %s: %s", path, e.strerror)
            return
        try:
            fcntl.ioctl(fd, EVIOCGRAB, 1)       # ours alone: the kiosk's browser mustn't act on it too
        except OSError:
            pass                                # not an evdev device (tests)
        self.fd, self.path, self.name, self.buf = fd, path, name, b""
        asyncio.get_running_loop().add_reader(fd, self.on_readable)
        log.info("game controller %s (%s)", name, path)

    async def task(self):
        while True:
            if self.fd is None:
                a = self.b.args
                if a.pad:
                    path, name = (a.pad, "test") if os.path.exists(a.pad) else (None, None)
                else:
                    path, name = find_pad()
                if path:
                    self.open(path, name)
            await asyncio.sleep(RESCAN_S)
