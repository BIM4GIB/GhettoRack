"""The IR remote: lirc / input device, tap/hold/repeat, profiles, learning."""
from .common import *
from .util import *


def find_ir(root=""):
    """(input event device, lirc device or None, name) of the first IR
    receiver (rc core), or (None, None, None)"""
    for ev in sorted(glob.glob(root + "/sys/class/rc/rc*/input*/event*")):
        name = ""
        try:
            name = open(os.path.join(os.path.dirname(ev), "name")).read().strip()
        except OSError:
            pass
        rc = os.path.dirname(os.path.dirname(ev))
        lirc = sorted(glob.glob(os.path.join(rc, "lirc*")))
        return ("/dev/input/" + os.path.basename(ev),
                "/dev/" + os.path.basename(lirc[0]) if lirc else None, name)
    return None, None, None


class IrReader:
    """Scancodes in, tap / hold / repeat out.

    The rc core sends MSC_SCAN for the first frame and for every repeat frame
    (~110 ms apart for NEC) while a button is held, then nothing. So: a new
    scancode = pressed; no frame for IR_RELEASE_S = released."""

    def __init__(self, bridge):
        self.b = bridge
        self.fd = None              # what we read: lirc, or the input device
        self.grab_fd = None         # the input device, held only to keep its keys from the kiosk
        self.path = None
        self.lirc = False
        self.name = None
        self.buf = b""
        self.strays = 0
        self.scan = None            # button currently down
        self.down_t = 0.0
        self.held = False
        self.timers = []
        self.release_timer = None
        self.swallow = False
        self.presses = 0
        self.last_scan = None
        self.learning = None        # (profile, action, press, deadline)

    def info(self):
        return {"device": self.path, "source": "lirc" if self.lirc else "input",
                "name": self.name, "presses": self.presses, "strays": self.strays,
                "last_scan": None if self.last_scan is None else "%x" % self.last_scan,
                "learning": None if not self.learning else
                {"profile": self.learning[0], "action": self.learning[1], "press": self.learning[2]}}

    def cancel_timers(self):
        for t in self.timers:
            t.cancel()
        self.timers = []

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
        rec = LIRC_SCANCODE if self.lirc else IR_EVENT
        while len(self.buf) >= rec.size:
            fields = rec.unpack_from(self.buf)
            self.buf = self.buf[rec.size:]
            if self.lirc:
                _, flags, proto, _, scan = fields
                if proto not in NEC_PROTOS:
                    self.strays += 1
                    continue
                self.frame(scan & 0xFFFFFFFF, bool(flags & LIRC_FLAG_REPEAT))
            else:
                _, _, typ, code, value = fields
                if typ == EV_MSC and code == MSC_SCAN:
                    if value & 0xFFFFFFFF == IMON_NOISE:
                        self.strays += 1
                        continue
                    self.frame(value & 0xFFFFFFFF, None)

    def drain_grab(self):
        try:
            os.read(self.grab_fd, 4096)     # held only for the grab
        except OSError:
            pass

    def frame(self, scan, repeat):
        """repeat: True/False from lirc, None when unknown (input device)"""
        loop = asyncio.get_running_loop()
        if repeat:
            if scan != self.scan:
                # A repeat with no button down carries the previous button's
                # code (rc core): noise. It mustn't start a press, or learn.
                self.strays += 1
                return
        elif repeat is False and self.scan is not None:
            self.release()          # a fresh frame: a new press, even of the same button
        self.last_scan = scan
        if scan != self.scan:
            if self.scan is not None:
                self.release()
            self.scan, self.down_t, self.held = scan, now(), False
            self.presses += 1
            self.b.broadcast(sse_event("irraw", {"scan": "%x" % scan}))
            if self.learning:
                prof, action, press, _ = self.learning
                self.learning = None
                self.b.state.bind(prof, action, press, scan)
                log.info("IR: %s/%s in profile %s = %x", action, press, prof, scan)
                self.b.broadcast(sse_event("irlearned", {"profile": prof, "action": action,
                                                          "press": press, "scan": "%x" % scan}))
                self.swallow = True  # this press only teaches, repeats and release included
                self.cancel_timers()
            else:
                self.cancel_timers()
                self.timers = [loop.call_later(IR_HOLD_S, self.hold)]
                self.emit("down")
        # every frame, first or repeat, pushes the release back
        if self.release_timer:
            self.release_timer.cancel()
        self.release_timer = loop.call_later(IR_RELEASE_S, self.release)

    def hold(self):
        if self.scan is None:
            return
        self.held = True
        self.emit("hold")
        self.timers.append(asyncio.get_running_loop().call_later(IR_REPEAT_S, self.repeat))

    def repeat(self):
        if self.scan is None:
            return
        self.emit("repeat")
        self.timers.append(asyncio.get_running_loop().call_later(IR_REPEAT_S, self.repeat))

    def release(self):
        if self.release_timer:
            self.release_timer.cancel()
            self.release_timer = None
        if self.scan is None:
            return
        self.emit("up")
        self.cancel_timers()
        self.scan = None
        self.swallow = False

    def emit(self, phase):
        """Each browser gets the action its own view binds to this button"""
        if self.swallow:
            return
        for c in list(self.b.clients):
            a = self.decide(c.view, phase)
            if a:
                c.push(sse_event("ir", {"action": a, "phase": phase}))
        if any(c.view == "apcbox" for c in self.b.clients):
            a = self.decide("apcbox", phase)
            if a:
                self.b.daisy_action(a)

    def decide(self, view, phase):
        st, scan = self.b.state, self.scan
        tap = st.action_for(view, scan, "tap")
        hold = st.action_for(view, scan, "hold")
        quick = tap in IR_REPEATABLE and not hold   # arrows: act on press, repeat while held
        if phase == "down":
            return tap if quick else None
        if phase == "hold":
            return hold
        if phase == "repeat":
            return tap if quick else None
        if phase == "up":
            if quick:
                return None
            if self.held and hold:
                return None                     # the hold already acted
            return tap
        return None

    def close(self):
        loop = asyncio.get_running_loop()
        for fd in (self.fd, self.grab_fd):
            if fd is not None:
                loop.remove_reader(fd)
                try:
                    os.close(fd)
                except OSError:
                    pass
        if self.fd is not None:
            log.info("IR receiver %s gone", self.path)
        self.release()
        self.fd = self.grab_fd = None
        self.path = None
        self.lirc = False

    def open(self, ev, lirc, name):
        loop = asyncio.get_running_loop()
        if ev:
            try:
                g = os.open(ev, os.O_RDONLY | os.O_NONBLOCK)
                try:
                    # Exclusive: if the kernel maps the remote to keys, the kiosk's
                    # compositor would see them too and every press would act twice
                    fcntl.ioctl(g, EVIOCGRAB, 1)
                except OSError:
                    pass            # not an evdev device (tests)
                if lirc:
                    self.grab_fd = g
                    loop.add_reader(g, self.drain_grab)
                else:
                    self.fd, self.path, self.lirc = g, ev, False
            except OSError as e:
                log.warning("open %s: %s", ev, e)
        if lirc:
            try:
                fd = os.open(lirc, os.O_RDONLY | os.O_NONBLOCK)
                try:
                    fcntl.ioctl(fd, LIRC_SET_REC_MODE, struct.pack("I", LIRC_MODE_SCANCODE))
                except OSError:
                    pass            # not a lirc device (tests)
                if self.fd is not None:
                    os.close(self.fd)
                self.fd, self.path, self.lirc = fd, lirc, True
            except OSError as e:
                log.warning("open %s: %s (using the input device)", lirc, e)
        if self.fd is None and self.grab_fd is not None:   # lirc failed: read the input device
            loop.remove_reader(self.grab_fd)
            self.fd, self.grab_fd, self.path, self.lirc = self.grab_fd, None, ev, False
        if self.fd is not None:
            self.name, self.buf = name, b""
            loop.add_reader(self.fd, self.on_readable)
            log.info("IR receiver %s (%s, %s)", self.path, name, "lirc" if self.lirc else "input device")

    async def task(self):
        loop = asyncio.get_running_loop()
        while True:
            if self.learning and now() > self.learning[3]:
                self.learning = None
                self.b.broadcast(sse_event("irlearned", {"timeout": True}))
            if self.fd is None:
                a = self.b.args
                if a.ir or a.ir_lirc:               # tests: fixed paths
                    ev = a.ir if a.ir and os.path.exists(a.ir) else None
                    lirc = a.ir_lirc if a.ir_lirc and os.path.exists(a.ir_lirc) else None
                    name = "test"
                else:
                    ev, lirc, name = find_ir()
                if ev or lirc:
                    self.open(ev, lirc, name)
            await asyncio.sleep(RESCAN_S)
