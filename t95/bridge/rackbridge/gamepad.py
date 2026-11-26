"""A game controller on the box's USB (seen: a Sony PlayStation 3 pad, 2026-11-26):
its buttons are the remote's actions for the pages, its sticks sweep the Daisy.

Buttons go to every open page as the same "ir" events a remote makes, so a page
needs nothing new. Play and stop go to the Daisy once, not through the pages:
two open pages would each toggle. The sticks spring back, so they only get
controls that have a middle to spring back to, or put back what was there:

    left  stick  X   the master filter (low-pass left, high-pass right, off in the middle)
                 Y   the selected track's filter; let go and it is what it was
    right stick  X   the crossfader (both sides in the middle)
                 Y   the selected track's pitch; let go and it is what it was
    L2 / R2          the selected track's sound, once per pull (no velocity yet)

Nothing here is timing: a sweep is at most GP_RATE_S apart and the Daisy plays."""
from .common import *
from .util import *

EV_KEY, EV_ABS = 0x01, 0x03
GP_BUTTONS = {544: "up", 545: "down", 546: "left", 547: "right",      # the D-pad
              304: "ok", 305: "back", 307: "menu", 316: "home",         # X, O, triangle, PS
              310: "prev", 311: "next",                                 # L1, R1: the view before / after
              315: "play", 314: "stop"}                                 # start, select
GP_REPEAT = {"up", "down", "left", "right"}
GP_DAISY = {"play": "play", "stop": "stop"}                             # to the Daisy itself, once
GP_DEAD = 12             # of 127 either way from the middle: a worn stick rests anywhere in there
GP_RATE_S = 0.04         # a control is sent at most this often while a stick moves
GP_PULL = 60             # of 255: how far a trigger is pulled before it plays, and half of it to let go
# stick axis -> (what, action number): "glob" has its own middle; "knob" is the selected track's, put back after
GP_AXES = {0: ("glob", 9), 3: ("glob", 1), 1: ("knob", 6), 4: ("knob", 4)}
GP_TRIGGERS = (2, 5)


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
        self.axis = {}              # axis -> {"want": value to send, "sent": last sent, "at": when, "base": (track, value) for a knob}
        self.pulled = set()
        self.timer = None
        self.presses = self.sent = self.dropped = 0

    def info(self):
        return {"device": self.path, "name": self.name, "presses": self.presses, "sent": self.sent, "dropped": self.dropped}

    # ---- buttons ----
    def act(self, action):
        if action in GP_DAISY:
            self.send(GP_DAISY[action])
            return
        for c in list(self.b.clients):
            c.push(sse_event("ir", {"action": action, "phase": "down", "pad": True}))

    def repeat(self, code):
        if code not in self.down:
            return
        self.act(GP_BUTTONS[code])
        self.down[code] = asyncio.get_running_loop().call_later(IR_REPEAT_S, self.repeat, code)

    def key(self, code, value):
        action = GP_BUTTONS.get(code)
        if action is None or value == 2:        # 2: the kernel's own repeat
            return
        t = self.down.pop(code, None)
        if t:
            t.cancel()
        if value == 1:
            self.presses += 1
            self.act(action)
            self.down[code] = (asyncio.get_running_loop().call_later(IR_HOLD_S, self.repeat, code)
                               if action in GP_REPEAT else None)

    # ---- sticks and triggers ----
    def send(self, action):
        if self.b.ctl >= 4 and self.b.ui_send(action):
            self.sent += 1
            return True
        self.dropped += 1
        return False

    def selected(self):
        """the Daisy's selected track, 0-15, or None while it hasn't said"""
        try:
            return max(0, min(15, int(self.b.cache.lines["S"].split()[8])))
        except (KeyError, IndexError, ValueError):
            return None

    def knob_now(self, track, k):
        try:
            return max(0, min(127, int(self.b.cache.lines["T %d" % track].split()[2 + k])))
        except (KeyError, IndexError, ValueError):
            return None

    def moved(self, code, value):
        if code in GP_TRIGGERS:
            if value >= GP_PULL and code not in self.pulled:
                self.pulled.add(code)
                t = self.selected()
                if t is not None:
                    self.send("audition %d" % (t + 1))
            elif value < GP_PULL // 2:
                self.pulled.discard(code)
            return
        if code not in GP_AXES:
            return
        what, k = GP_AXES[code]
        v = stick(255 - value if code in (1, 4) else value)        # up is more
        a = self.axis.get(code)
        if a is None:
            if v == 64:
                return
            a = self.axis[code] = {"sent": None, "at": 0.0, "base": None}
            if what == "knob":                                      # whose knob, and what it was: put back on letting go
                t = self.selected()
                base = None if t is None else self.knob_now(t, k)
                if base is None:
                    del self.axis[code]
                    return
                a["base"] = (t, base)
        a["want"] = v
        self.flush()

    def flush(self):
        """send what the sticks want, each control at most every GP_RATE_S; the last value always goes"""
        self.timer = None
        t, wait = now(), None
        for code, a in list(self.axis.items()):
            what, k = GP_AXES[code]
            home = a["want"] == 64
            if a["want"] != a["sent"] or home:
                due = a["at"] + GP_RATE_S - t
                if due > 0 and not home:
                    wait = due if wait is None else min(wait, due)
                    continue
                if what == "glob":
                    self.send("glob %d %d" % (k, a["want"]))
                else:
                    tr, base = a["base"]
                    self.send("knob %d %d %d" % (tr + 1, k, base if home else a["want"]))
                a["sent"], a["at"] = a["want"], t
            if home:
                del self.axis[code]
        if wait is not None:
            self.timer = asyncio.get_running_loop().call_later(wait, self.flush)

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
        self.pulled = set()
        for a in self.axis.values():            # a stick held when the cable went: back to the middle
            a["want"] = 64
        if self.timer:
            self.timer.cancel()
        self.flush()
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
