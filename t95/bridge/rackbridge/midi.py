"""USB MIDI ports: discovery under /sys, the MIDI byte parser, per-port queues."""
from .common import *
from .util import *


def data_len(status):
    """Data bytes after a channel status byte"""
    return 1 if status & 0xF0 in (0xC0, 0xD0) else 2


class MidiParser:
    """Raw MIDI bytes in, complete messages out.

    Handles running status, realtime bytes (F8-FF) arriving anywhere, even
    inside other messages, and sysex split across reads. Clock, start, stop
    and continue come out as one-byte messages, song position as three (the router never sends them
    to the Daisy); other realtime and system common bytes are dropped and
    counted. Sysex longer than max is dropped too."""

    def __init__(self, max_sysex=SYSEX_MAX):
        self.max_sysex = max_sysex
        self.status = 0         # running status
        self.data = []
        self.sysex = None       # bytearray while inside F0..F7
        self.realtime = 0
        self.dropped = 0

    def feed(self, data):
        out = []
        for b in data:
            if b >= 0xF8:
                self.realtime += 1
                if b in (0xF8, 0xFA, 0xFB, 0xFC):  # clock, start, continue, stop: routable
                    out.append(bytes([b]))
                continue
            if self.sysex is not None:
                if b == 0xF7:
                    self.sysex.append(b)
                    if len(self.sysex) <= self.max_sysex:
                        out.append(bytes(self.sysex))
                    else:
                        self.dropped += 1
                    self.sysex = None
                    continue
                if b < 0x80:
                    if len(self.sysex) <= self.max_sysex:
                        self.sysex.append(b)
                    continue
                self.sysex = None   # unterminated: a new status ends it
                self.dropped += 1
            if b == 0xF0:
                self.sysex = bytearray([b])
                self.status = 0
            elif b == 0xF2:         # song position pointer: two data bytes, no running status
                self.status = 0xF2
                self.data = []
            elif b >= 0xF0:         # other system common: not forwarded
                self.status = 0
                self.data = []
                self.dropped += 1
            elif b >= 0x80:
                self.status = b
                self.data = []
            elif self.status:
                self.data.append(b)
                if len(self.data) == data_len(self.status):
                    out.append(bytes([self.status] + self.data))
                    self.data = []
                    if self.status == 0xF2:
                        self.status = 0
            # data byte without a status: ignored
        return out


def find_midi(root=""):
    """{key: (device path, name)} for the raw MIDI ports under /sys.
    The key is the USB device path, so it survives ALSA renumbering."""
    found = {}
    for d in sorted(glob.glob(root + "/sys/class/sound/midiC*D*")):
        base = os.path.basename(d)              # midiC3D0
        try:
            card, dev = base[5:].split("D")
            card_dir = root + "/sys/class/sound/card" + card
            name = open(os.path.join(card_dir, "id")).read().strip()
            key = os.path.realpath(os.path.join(card_dir, "device"))
            usb = os.path.dirname(key)          # the USB device above the interface
            fp = os.path.join(usb, "product")
            if os.path.exists(fp):
                name = open(fp).read().strip() or name
        except (OSError, ValueError):
            continue
        if dev != "0":
            name += " " + dev
        found[key + ":" + dev] = ("/dev/snd/" + base, name)
    return found


def clean_name(name):
    name = "".join(c if 0x20 <= ord(c) < 0x7F else "?" for c in name).strip()
    return (name or "MIDI")[:32]


class MidiPort:
    """One controller on the T95. Out queue: sysex in order, then the newest
    value per note/CC (LED states): if it backs up, old LED values are
    replaced rather than piling up."""

    def __init__(self, num, key, path, name, fd):
        self.num, self.key, self.path, self.name, self.fd = num, key, path, name, fd
        self.net = False
        self.held = set()       # (status & 0xF, note) held by notes from this port
        self.parser = MidiParser(SYSEX_PORT_MAX)
        self.out_sysex = collections.deque()
        self.out_fifo = collections.deque()  # routed messages: strict order, never coalesced
        self.out_led = collections.OrderedDict()
        self.out_buf = b""      # bytes of a message the device took only part of
        self.writing = False
        self.announced = 0.0
        self.in_msgs = self.out_msgs = 0
        self.dropped_in = self.dropped_out = self.coalesced = self.unrouted = 0

    def info(self, kinds):
        return {"port": self.num, "name": self.name, "path": self.path, "kind": kinds.get(self.num, "not on the Daisy"),
                "in": self.in_msgs, "out": self.out_msgs, "dropped_in": self.dropped_in,
                "dropped_out": self.dropped_out, "coalesced": self.coalesced, "unrouted": self.unrouted,
                "realtime_dropped": self.parser.realtime, "bad_in": self.parser.dropped}


class PortsMixin:
    """Bridge methods for USB MIDI controllers"""

    def midi_candidates(self):
        ble = self.ble.candidates()    # Bluetooth MIDI devices, each behind a pty (ble.py)
        if self.midi_paths:            # --midi: fixed paths (tests)
            return dict({path: (path, name) for path, name in self.midi_paths.items() if os.path.exists(path)}, **ble)
        return dict(find_midi(), **ble)

    def attach_midi(self, key, path, name):
        free = [n for n in range(1, MAX_PORTS + 1) if n not in self.ports]
        if not free:
            return
        try:
            fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
            if os.isatty(fd):          # tests use ptys: make them raw
                open_raw_fd(fd)
        except OSError as e:
            log.warning("open %s: %s", path, e)
            return
        base = clean_name(name)
        taken = {q.name for q in self.ports.values()}
        nm, k = base, 2
        while nm in taken:              # two of the same controller: routes go by name
            nm = "%s %d" % (base[:29], k)
            k += 1
        p = MidiPort(free[0], key, path, nm, fd)
        self.ports[p.num] = p
        asyncio.get_running_loop().add_reader(fd, self.on_midi_readable, p)
        log.info("controller %s on %s: port %d", p.name, path, p.num)
        self.refresh_daisy_ports()
        self.announce()

    def detach_midi(self, p, why=""):
        self.release_held(p)
        loop = asyncio.get_running_loop()
        loop.remove_reader(p.fd)
        loop.remove_writer(p.fd)
        with self.clock.lock:            # the clock thread must not write to a closed (reused) fd
            try:
                os.close(p.fd)
            except OSError:
                pass
            p.fd = None
        self.ports.pop(p.num, None)
        log.info("controller %s (port %d) gone%s", p.name, p.num, why)
        if p.num in self.daisy_ports:
            self.daisy_ports.discard(p.num)
            self.ctl_send(b"%sN%d -\n" % (ESC, p.num))
        self.announce()

    def on_midi_readable(self, p):
        try:
            data = os.read(p.fd, 4096)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            self.detach_midi(p, ": read failed")
            return
        for msg in p.parser.feed(data):
            p.in_msgs += 1
            self.route(p, msg)

    def midi_out(self, line):
        """X p hex: from the Daisy to controller p"""
        try:
            _, num, hexs = line.split(" ", 2)
            p = self.ports.get(int(num))
            data = bytes.fromhex(hexs)
        except ValueError:
            self.asm.bad += 1
            return
        if p is None:
            return
        for msg in MidiParser().feed(data):
            if msg[0] == 0xF0:
                if len(p.out_sysex) >= OUT_SYSEX_MAX:
                    p.dropped_out += 1
                    continue
                p.out_sysex.append(msg)
            else:
                key = msg[:2] if msg[0] < 0xC0 else msg[:1]
                if key in p.out_led:
                    p.coalesced += 1
                    del p.out_led[key]
                p.out_led[key] = msg
        self.midi_flush(p)

    def midi_flush(self, p):
        if p.net:
            return self.net_flush(p)
        loop = asyncio.get_running_loop()
        try:
            while True:
                if not p.out_buf:
                    if p.out_sysex:
                        p.out_buf = p.out_sysex.popleft()
                    elif p.out_fifo:
                        p.out_buf = p.out_fifo.popleft()
                    elif p.out_led:
                        p.out_buf = p.out_led.popitem(last=False)[1]
                    else:
                        break
                    p.out_msgs += 1
                n = os.write(p.fd, p.out_buf)
                p.out_buf = p.out_buf[n:]
        except BlockingIOError:
            pass
        except OSError:
            self.detach_midi(p, ": write failed")
            return
        busy = bool(p.out_buf or p.out_sysex or p.out_fifo or p.out_led)
        if busy and not p.writing:
            loop.add_writer(p.fd, self.midi_flush, p)
        elif not busy and p.writing:
            loop.remove_writer(p.fd)
        p.writing = busy

    async def midi_task(self):
        if self.args.no_midi:
            return
        while True:
            have = {p.key: p for p in self.ports.values() if not p.net}
            cand = self.midi_candidates()
            for key, p in have.items():
                if key not in cand:
                    self.detach_midi(p, ": unplugged")
            for key, (path, name) in cand.items():
                if key not in have:
                    self.attach_midi(key, path, name)
            await asyncio.sleep(RESCAN_S)
