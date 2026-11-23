"""The Daisy link: finding the Seed, the serial line, telemetry cache, control frames,
ping, keepalive. A self-contained block: a second Seed later = a second link."""
from .common import *
from .util import *
from .midi import MidiParser


def find_daisy(serial=None):
    """(tty path, USB serial) of the ApcBox CDC port, or (None, None)"""
    hits = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        try:
            intf = os.path.realpath(os.path.join(tty, "device"))
            usb = os.path.dirname(intf)
            rd = lambda f: open(os.path.join(usb, f)).read().strip()
            if rd("idVendor") != VID or rd("idProduct") != PID:
                continue
            ser = rd("serial") if os.path.exists(os.path.join(usb, "serial")) else ""
        except OSError:
            continue
        if serial and ser != serial:
            continue
        hits.append(("/dev/" + os.path.basename(tty), ser))
    if len(hits) > 1:
        log.warning("%d ApcBox devices, using %s; pick one with --serial", len(hits), hits[0][0])
    return hits[0] if hits else (None, None)


class LineAssembler:
    """Bytes in, complete report lines out. Bad input is dropped and counted."""

    def __init__(self, max_line=MAX_LINE):
        self.buf = bytearray()
        self.max = max_line
        self.skipping = False   # inside an over-long line: drop to the next \n
        self.bad = 0

    def feed(self, data):
        out = []
        self.buf += data
        while True:
            i = self.buf.find(b"\n")
            if i < 0:
                if len(self.buf) > self.max:
                    self.buf.clear()
                    if not self.skipping:
                        self.bad += 1
                    self.skipping = True
                return out
            raw = bytes(self.buf[:i])
            del self.buf[:i + 1]
            if self.skipping:
                self.skipping = False
                continue
            if raw.endswith(b"\r"):
                raw = raw[:-1]
            if not raw:
                continue
            if len(raw) > self.max or any(b < 0x20 or b > 0x7e for b in raw):
                self.bad += 1
                continue
            out.append(raw.decode("ascii"))

    def reset(self):
        self.buf.clear()
        self.skipping = False


class StateCache:
    """Latest line per (type, key) so a new browser starts with a full picture"""

    def __init__(self):
        self.lines = collections.OrderedDict()
        self.logs = collections.deque(maxlen=LOG_KEEP)

    @staticmethod
    def key(line):
        parts = line.split(" ", 2)
        t = parts[0]
        if t == "L":
            return None
        if t == "E":            # raw MIDI events: history, not state
            return False
        if t in KEYED and len(parts) > 1:
            return t + " " + parts[1]
        return t

    def put(self, line):
        k = self.key(line)
        if k is None:
            self.logs.append(line)
        elif k is not False:
            self.lines[k] = line
            self.lines.move_to_end(k)
            while len(self.lines) > CACHE_MAX:
                self.lines.popitem(last=False)

    def snapshot(self):
        v = self.lines.get("V")
        rest = [l for k, l in self.lines.items() if k != "V"]
        return ([v] if v else []) + rest + list(self.logs)

    def clear(self):
        self.lines.clear()
        self.logs.clear()


class DaisyMixin:
    """Bridge methods for the Daisy link"""

    def kinds(self):
        """Port kinds from the Daisy's P line: P ver attached apc keys active"""
        k = {}
        try:
            v, att, apc, keys, active = self.p_line.split()[1:6]
            att, apc, keys, active = int(att, 16), int(apc, 16), int(keys, 16), int(active)
        except (AttributeError, ValueError):
            return k
        for n in range(1, MAX_PORTS + 1):
            bit = 1 << (n - 1)
            if apc & bit:
                k[n] = "apc40 (driving)" if active == n else "apc40"
            elif keys & bit:
                k[n] = "keys"
            elif att & bit:
                k[n] = "identifying"
        return k

    def ping_stats(self):
        if not self.rtts:
            return {"count": 0, "lost": self.pings_lost}
        r = sorted(self.rtts)
        avg = sum(r) / len(r)
        jitter = (sum((x - avg) ** 2 for x in r) / len(r)) ** 0.5
        return {"count": len(r), "lost": self.pings_lost, "min": round(r[0], 2), "avg": round(avg, 2),
                "p99": round(r[min(len(r) - 1, int(len(r) * 0.99))], 2), "max": round(r[-1], 2),
                "jitter": round(jitter, 2)}

    def ask_dump(self):
        self.last_ask = now()
        self.serial_send(ESC_R, essential=True)

    def serial_send(self, data, essential=False):
        """Queue bytes for the Daisy. Never blocks: if the Daisy isn't taking
        them, MIDI beyond SERIAL_OUT_MAX is dropped and counted."""
        if self.fd is None:
            return False
        if not essential and len(self.sout) + len(data) > SERIAL_OUT_MAX:
            self.serial_dropped += 1
            return False
        self.sout += data
        self.serial_flush()
        return True

    def serial_flush(self):
        """MIDI and control first; sample data only when nothing else waits"""
        loop = asyncio.get_running_loop()
        if self.fd is None:
            return
        try:
            for q in (self.sout, self.sout_lo):
                while q:
                    n = os.write(self.fd, q)
                    del q[:n]
                if self.sout:
                    break
        except BlockingIOError:
            pass
        except OSError as e:
            log.warning("write to %s: %s", self.port, e)
            self.sout.clear()
            self.sout_lo.clear()
        if self.sout or self.sout_lo:
            loop.add_writer(self.fd, self.serial_flush)
        else:
            loop.remove_writer(self.fd)

    def data_send(self, data):
        """Sample frames: low priority, never more than a window's worth queued"""
        if self.fd is None or self.ctl < 3:
            return False
        self.sout_lo += data
        self.serial_flush()
        return True

    def ctl_send(self, data):
        """Control frames: only to a Daisy that said it understands them"""
        if self.ctl >= 1:
            return self.serial_send(data)
        return False

    def on_readable(self):
        try:
            data = os.read(self.fd, 65536)
        except BlockingIOError:
            return
        except OSError as e:
            log.warning("read %s: %s", self.port, e)
            data = b""
        if not data:
            asyncio.get_running_loop().remove_reader(self.fd) # don't spin until the task notices
            self.lost.set()
            return
        for line in self.asm.feed(data):
            self.lines += 1
            self.last_line = now()
            t = line.split(" ", 1)[0]
            if t == "X":
                self.midi_out(line)
                continue
            if t == "Y":
                self.pong(line)
                continue
            if t in ("FO", "FA", "FC", "FL"):
                self.xfer_replies.put_nowait(line)
                continue
            if t == "P":
                self.on_p_line(line)
            self.cache.put(line)
            chunk = sse_data(line)
            for c in list(self.clients):
                c.push(chunk)

    def on_p_line(self, line):
        self.p_line = line
        try:
            v = int(line.split()[1])
        except (IndexError, ValueError):
            return
        if v >= 1 and self.ctl == 0:
            self.ctl = v
            log.info("Daisy speaks control protocol %d: forwarding controllers", v)
            self.daisy_ports.clear()
            self.refresh_daisy_ports()    # (re)announce what the module routes to it
            return
        # The Daisy dropped a port we still have (its keepalive timed out, it
        # rebooted without a USB reset...): tell it again, at most every 2 s
        try:
            att = int(line.split()[2], 16)
        except (IndexError, ValueError):
            return
        t = now()
        for p in self.ports.values():
            if p.num in self.daisy_ports and not att & (1 << (p.num - 1)) and t - p.announced > 2.0:
                log.info("Daisy doesn't list port %d (%s): announcing it again", p.num, p.name)
                self.announce_port(p)

    def announce_port(self, p):
        p.announced = now()
        self.ctl_send(b"%sN%d %s\n" % (ESC, p.num, p.name.encode()))

    def apc_port(self):
        """T95 port of the APC40 driving the box (P line), or None"""
        try:
            active = int(self.p_line.split()[5])
        except (AttributeError, IndexError, ValueError):
            return None
        return active if active in self.ports else None

    def daisy_action(self, a):
        """A remote action for the Daisy: ESC U when it speaks protocol 2,
        else the APC40 button for play/stop"""
        if self.ctl >= 2 and a in DAISY_UI:
            return self.ui_send(DAISY_UI[a])
        if a in APC_BUTTONS:
            return self.apc_button(APC_BUTTONS[a])
        return False

    def ui_send(self, action):
        return self.ctl_send(b"%sU %s\n" % (ESC, action.encode()))

    def ui_do(self, actions):
        """POST /do: UI actions from a page, checked before they go on the wire"""
        if isinstance(actions, str):
            actions = [actions]
        if not isinstance(actions, list) or not 0 < len(actions) <= 32:
            return 400, {"error": "a: an action or a list of up to 32"}
        for a in actions:
            if not isinstance(a, str) or not 0 < len(a) <= UI_ACTION_MAX or \
                    not all(c in "abcdefghijklmnopqrstuvwxyz0123456789 -" for c in a):
                return 400, {"error": "bad action"}
            need = UI_ACTIONS.get(a.split(" ", 1)[0])
            if need is None:
                return 400, {"error": "unknown action %s" % a.split(" ", 1)[0]}
            if self.fd is None:
                return 503, {"error": "the Daisy isn't connected"}
            if self.ctl < need:
                return 503, {"error": "the Daisy's firmware is too old for this (needs v13.%d+)" % {4: 9, 5: 10}.get(need, 7)}
        sent = sum(1 for a in actions if self.ui_send(a))
        if sent < len(actions):
            return 503, {"error": "the Daisy isn't taking commands fast enough", "sent": sent}
        return 200, {"ok": True}

    def apc_button(self, note):
        """Press and release one APC40 button, as if on the APC40 itself"""
        p = self.apc_port()
        if p is None:
            log.info("remote: no APC40 on the T95 is driving the box, button %02x ignored", note)
            return False
        self.ctl_send(b"%sM%d %02x%02x7f\n" % (ESC, p, 0x90, note))
        self.ctl_send(b"%sM%d %02x%02x00\n" % (ESC, p, 0x80, note))
        return True

    def pong(self, line):
        sent = self.pings.pop(line[2:].strip(), None)
        if sent is not None:
            self.rtts.append((now() - sent) * 1000.0)

    def close_port(self):
        loop = asyncio.get_running_loop()
        self.ctl = 0
        self.p_line = None
        self.sout.clear()
        self.sout_lo.clear()
        self.pings.clear()
        if self.fd is not None:
            loop.remove_writer(self.fd)
            loop.remove_reader(self.fd)
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = None
        self.connected_at = None

    async def serial_task(self):
        loop = asyncio.get_running_loop()
        waiting_logged = False
        while True:
            if self.args.port:
                path, ser = (self.args.port if os.path.exists(self.args.port) else None), None
            else:
                path, ser = find_daisy(self.args.serial)
            if not path:
                if not waiting_logged:
                    log.info("waiting for the Daisy (USB %s:%s)", VID, PID)
                    waiting_logged = True
                await asyncio.sleep(RESCAN_S)
                continue
            try:
                self.fd = open_raw(path)
            except OSError as e:
                log.warning("open %s: %s", path, e)
                await asyncio.sleep(RESCAN_S)
                continue
            waiting_logged = False
            termios.tcflush(self.fd, termios.TCIFLUSH)
            self.port, self.serial = path, ser
            self.connects += 1
            self.connected_at = now()
            self.last_line = None
            self.asm.reset()
            # The Daisy streams all the time: the first bytes after open are
            # usually the middle of a line. Drop up to the first newline.
            self.asm.skipping = True
            self.cache.clear()         # a different Daisy, or the same one rebooted
            self.ctl = 0               # until it sends a P line
            for c in self.clients:
                c.q.clear(); c.qbytes = 0; c.need_snapshot = True; c.wake.set()
            self.lost.clear()
            loop.add_reader(self.fd, self.on_readable)
            log.info("Daisy on %s%s", path, " serial " + ser if ser else "")
            self.ask_dump()
            self.announce()
            while not self.lost.is_set():
                try:
                    await asyncio.wait_for(self.lost.wait(), 0.5)
                except asyncio.TimeoutError:
                    pass
                if self.args.port and not os.path.exists(self.args.port):
                    break
                t = now()
                quiet = t - (self.last_line or self.connected_at)
                if quiet > STALE_S and t - self.last_ask > STALE_S:
                    self.ask_dump()
            log.warning("Daisy on %s gone", self.port)
            self.close_port()
            self.announce()

    async def control_task(self):
        """Keepalive and ping, while the Daisy speaks the control protocol"""
        last_ping = 0.0
        while True:
            await asyncio.sleep(KEEPALIVE_S)
            if self.ctl < 1:
                continue
            self.ctl_send(ESC + b"K")
            t = now()
            for k, sent in list(self.pings.items()):
                if t - sent > 2.0:
                    del self.pings[k]
                    self.pings_lost += 1
            if t - last_ping >= PING_S:
                last_ping = t
                self.ping_seq += 1
                pid = "%d" % self.ping_seq
                self.pings[pid] = now()
                self.ctl_send(b"%sP %s\n" % (ESC, pid.encode()))
