#!/usr/bin/env python3
"""ApcBox bridge: Daisy USB serial <-> T95 controllers, and SSE for browsers.

    controllers --raw MIDI--> bridge --ESC M frames--> Daisy
    controllers <--raw MIDI-- bridge <--X lines------- Daisy (APC40 LEDs)
    Daisy --report lines--> bridge --SSE /events--> browsers

To the Daisy it sends ESC R (state dump) on connect and when the Daisy goes
quiet. Only once the Daisy reports a "P" line (firmware v13.5 on, control
protocol 1) does it also send controller MIDI (ESC M), port names (ESC N),
keepalives (ESC K) and pings (ESC P). Python 3 standard library only.

  /             the monitor page (daisy/ui/apcbox-ui.html)
  /events       SSE: a snapshot of the latest state, then every line live.
                Default events carry one report line each; "bridge" events
                carry the bridge's own status as JSON.
  /health       JSON: Daisy link, data age, counters, clients, controllers,
                round-trip times to the Daisy
  /caps         JSON: what this server supports (the page reads it to hide
                the Mac build's /ui and /tft.bmp controls)
  /             module selector and IR remote setup (t95/ui/selector.html)
  /apcbox       the ApcBox page
  /state        JSON: modules, last module, IR profiles; POST /select,
                /settings, /ir/learn, /ir/forget, /ir/cancel change it

IR remote: the kernel decodes the remote (rc core, NEC). The bridge reads
scancodes from the lirc device, which marks repeat frames and the protocol:
a press starts only on a fresh NEC frame, so stray repeats (which carry the
previous button's code) and other decoders' noise are ignored. No keymap is
involved. Without lirc it falls back to the input device (MSC_SCAN).
Buttons are bound to actions per profile (one per view, plus "all"), learnt
from the setup page, and sent to browsers as "ir" events for their view.

The Daisy is found by USB VID/PID (and optionally serial) under /sys, not by
tty index; controllers by their USB device under /sys/class/sound, not by
ALSA card number. --port and --midi force paths instead (tests, odd setups).
See t95/bridge/README.md.
"""
import argparse
import asyncio
import base64
import binascii
import shutil
import socket
import ssl
import subprocess
import collections
import fcntl
import glob
import json
import logging
import os
import struct
import sys
import termios
import time
import urllib.parse

VID, PID = "1209", "0001"
ESC_R = b"\x1bR"          # full state dump; v13.2 and older also act on it
MAX_LINE = 4096           # longest real line is D: 2 + 2048 hex digits
CLIENT_QUEUE_BYTES = 512 * 1024
MAX_CLIENTS = 16
STALE_S = 3.0             # no line for this long: ask for a dump again
RESCAN_S = 1.0
LOG_KEEP = 100            # L lines replayed to a new browser
CACHE_MAX = 512
ESC = b"\x1b"
MAX_PORTS = 8             # T95 controller ports 1..8
SERIAL_OUT_MAX = 64 * 1024  # bytes waiting for the Daisy before MIDI is dropped
SYSEX_MAX = 128           # one ESC M frame (HostCmd::kMaxMidi on the Daisy)
OUT_SYSEX_MAX = 16        # sysex messages waiting for one controller
KEEPALIVE_S = 0.5         # the Daisy drops T95 controllers after 2 s without one
PING_S = 1.0
PING_KEEP = 300

# IR remote
EV_MSC, MSC_SCAN = 0x04, 0x04
EVIOCGRAB = 0x40044590    # _IOW('E', 0x90, int)
LIRC_SET_REC_MODE = 0x40046912   # _IOW('i', 0x12, __u32)
LIRC_MODE_SCANCODE = 8
LIRC_SCANCODE = struct.Struct("QHHIQ")   # struct lirc_scancode, 24 bytes
LIRC_FLAG_REPEAT = 2
NEC_PROTOS = {9, 10, 11}  # RC_PROTO_NEC, NECX, NEC32 (the T95 remote is NECX)
IMON_NOISE = 0x7FFFFFFF   # what the imon decoder makes of noise on the T95
IR_EVENT = struct.Struct("llHHi")    # struct input_event (64-bit time)
IR_RELEASE_S = 0.2        # no repeat frame for this long: button released (NEC repeats ~110 ms)
IR_HOLD_S = 0.5
IR_REPEAT_S = 0.11
IR_LEARN_S = 15.0
IR_ACTIONS = ["up", "down", "left", "right", "ok", "back", "home", "menu",
              "play", "stop", "next", "prev", "vol_up", "vol_down",
              "1", "2", "3", "4", "5", "6", "7", "8", "9", "0"]
IR_REPEATABLE = {"up", "down", "left", "right", "vol_up", "vol_down"}
IR_PROFILES = ["all", "selector", "apcbox"]
# Remote actions the bridge plays on the Daisy itself, while a browser shows
# the ApcBox page. Firmware with control protocol 2 (v13.7) takes them as UI
# actions (ESC U): encoder, transport, track select. Older firmware: only
# play/stop, as presses of the APC40's own buttons on the port of the APC40
# that drives the box (so only when that APC40 is on the T95).
DAISY_UI = {"play": "play", "stop": "stop", "left": "turn -1", "right": "turn 1",
            "ok": "push", "back": "back", "0": "track 10",
            **{str(n): "track %d" % n for n in range(1, 10)}}
APC_BUTTONS = {"play": 0x5B, "stop": 0x5C}
MODULES = [
    {"id": "apcbox", "name": "ApcBox groovebox", "url": "/apcbox", "ready": True,
     "about": "Daisy: 16 tracks, samples and 303s, APC40 + keyboards"},
    {"id": "dj", "name": "Numark DJ", "url": None, "ready": False, "about": "later"},
    {"id": "clock", "name": "Network clock", "url": None, "ready": False, "about": "later"},
]
POST_MAX = 4096

# Samples (control protocol 3): the T95 keeps the library, the Daisy gets a
# track's sound over USB. See daisy/xfer.h for the Daisy side.
AUDIO_EXT = (".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".m4a",
             ".webm", ".mp4")   # .webm / .mp4: recordings from the page (Chrome / Safari)
XFER_CHUNK = 512          # bytes per ESC F D frame (kXferChunk on the Daisy)
XFER_WINDOW = 4           # chunks in flight; the Daisy's receive ring takes 8 KB
XFER_RESEND_S = 0.5
XFER_TRIES = 8            # sends per chunk before giving up
UPLOAD_MAX = 64 * 1024 * 1024

# Line types whose second field says which track/slot they describe
KEYED = {"T", "N", "Q", "A", "KN"}

log = logging.getLogger("bridge")


def now():
    return time.monotonic()


# ---------------------------------------------------------------- device

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


def open_raw(path):
    """Open a tty raw, echo off, before reading anything"""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        open_raw_fd(fd)
    except termios.error:
        os.close(fd)
        raise
    return fd


def open_raw_fd(fd):
    """Raw, echo off (cfmakeraw)"""
    a = termios.tcgetattr(fd)
    # cfmakeraw
    a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
              | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
    a[1] &= ~termios.OPOST
    a[2] &= ~(termios.CSIZE | termios.PARENB)
    a[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    a[6][termios.VMIN] = 1
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)


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


# ---------------------------------------------------------------- MIDI

def data_len(status):
    """Data bytes after a channel status byte"""
    return 1 if status & 0xF0 in (0xC0, 0xD0) else 2


class MidiParser:
    """Raw MIDI bytes in, complete messages out.

    Handles running status, realtime bytes (F8-FF) arriving anywhere, even
    inside other messages, and sysex split across reads. Realtime and system
    common messages are dropped and counted: clock and tempo sync belong on
    the Daisy, later. Sysex longer than one frame is dropped too."""

    def __init__(self):
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
                continue
            if self.sysex is not None:
                if b == 0xF7:
                    self.sysex.append(b)
                    if len(self.sysex) <= SYSEX_MAX:
                        out.append(bytes(self.sysex))
                    else:
                        self.dropped += 1
                    self.sysex = None
                    continue
                if b < 0x80:
                    self.sysex.append(b)
                    continue
                self.sysex = None   # unterminated: a new status ends it
                self.dropped += 1
            if b == 0xF0:
                self.sysex = bytearray([b])
                self.status = 0
            elif b >= 0xF0:         # system common: not forwarded
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
        self.parser = MidiParser()
        self.out_sysex = collections.deque()
        self.out_led = collections.OrderedDict()
        self.out_buf = b""      # bytes of a message the device took only part of
        self.writing = False
        self.announced = 0.0
        self.in_msgs = self.out_msgs = 0
        self.dropped_in = self.dropped_out = self.coalesced = 0

    def info(self, kinds):
        return {"port": self.num, "name": self.name, "path": self.path, "kind": kinds.get(self.num, "not on the Daisy"),
                "in": self.in_msgs, "out": self.out_msgs, "dropped_in": self.dropped_in,
                "dropped_out": self.dropped_out, "coalesced": self.coalesced,
                "realtime_dropped": self.parser.realtime, "bad_in": self.parser.dropped}


# ---------------------------------------------------------------- state

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


def sse_data(line):
    return ("data: " + line + "\n\n").encode()


def sse_event(name, obj):
    return ("event: " + name + "\ndata: " + json.dumps(obj, separators=(",", ":")) + "\n\n").encode()


class Client:
    """One browser. Its own bounded queue: a slow one can't hold up anyone."""

    def __init__(self, writer, peer, view=""):
        self.writer = writer
        self.peer = peer
        self.view = view            # selector, apcbox...: picks the IR profile
        self.q = collections.deque()
        self.qbytes = 0
        self.wake = asyncio.Event()
        self.need_snapshot = True
        self.resyncs = 0
        self.closed = False

    def push(self, chunk):
        if self.need_snapshot:
            return              # the snapshot it gets next includes this
        if self.qbytes + len(chunk) > CLIENT_QUEUE_BYTES:
            # Too far behind: throw the backlog away, start it over from a snapshot
            self.q.clear()
            self.qbytes = 0
            self.need_snapshot = True
            self.resyncs += 1
        else:
            self.q.append(chunk)
            self.qbytes += len(chunk)
        self.wake.set()


# ---------------------------------------------------------------- samples

def safe_join(root, rel):
    """root/rel, or None if rel tries to leave root"""
    if root is None:
        return None
    rootr = os.path.realpath(root)
    p = os.path.realpath(os.path.join(rootr, (rel or "").lstrip("/")))
    return p if p == rootr or p.startswith(rootr + os.sep) else None


def fix_wav_sizes(data):
    """ffmpeg writing WAV to a pipe can't go back to fill in the sizes"""
    b = bytearray(data)
    if len(b) < 12 or b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        return data
    b[4:8] = struct.pack("<I", len(b) - 8)
    i = 12
    while i + 8 <= len(b):
        cid, size = b[i:i + 4], struct.unpack_from("<I", b, i + 4)[0]
        if cid == b"data":
            b[i + 4:i + 8] = struct.pack("<I", len(b) - i - 8)
            break
        i += 8 + size + (size & 1)
    return bytes(b)


def convert_for_daisy(path):
    """(wav bytes, how) ready for the Daisy: mono 16-bit 48 kHz via ffmpeg;
    without ffmpeg, a WAV goes as it is (the Daisy's parser takes PCM 8-32
    bit, float, any rate, up to 8 channels and mixes down itself)"""
    ff = shutil.which("ffmpeg")
    if ff:
        r = subprocess.run([ff, "-v", "error", "-nostdin", "-i", path, "-map_metadata", "-1",
                            "-fflags", "+bitexact", "-ac", "1", "-ar", "48000", "-sample_fmt", "s16",
                            "-f", "wav", "-"], capture_output=True, timeout=120)
        if r.returncode != 0 or len(r.stdout) < 44:
            raise ValueError("can't convert: " + r.stderr.decode(errors="replace").strip()[:200])
        return fix_wav_sizes(r.stdout), "mono 16-bit 48 kHz"
    if not path.lower().endswith((".wav", ".wave")):
        raise ValueError("only WAV without ffmpeg (sudo apt install ffmpeg)")
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    return data, "as it is (no ffmpeg)"


class Transfer:
    """One sample on its way to the Daisy"""

    def __init__(self, tid, src, rel, path, track):
        self.id, self.src, self.rel, self.path, self.track = tid, src, rel, path, track
        self.name = os.path.basename(path)
        self.state = "queued"          # converting, sending, loading, done, failed
        self.error = None
        self.size = 0
        self.acked = 0
        self.chunks = 0
        self.resent = 0
        self.started = None
        self.finished = None
        self.how = ""
        self.capacity = None
        self.frames = None

    def info(self):
        t = (self.finished or now()) - self.started if self.started else 0
        return {"id": self.id, "name": self.name, "track": self.track, "state": self.state,
                "error": self.error, "bytes": self.size, "chunks": self.chunks, "acked": self.acked,
                "resent": self.resent, "seconds": round(t, 2), "how": self.how,
                "kb_per_s": round(self.acked * XFER_CHUNK / 1024 / t, 1) if t > 0 and self.acked else None,
                "cut": (self.frames is not None and self.capacity is not None and self.frames > self.capacity)}


# ---------------------------------------------------------------- state, IR

class State:
    """What survives a restart: last module, autostart, IR bindings.
    Written atomically; a missing or broken file means defaults."""

    def __init__(self, path):
        self.path = path
        self.data = {"last": None, "autostart_s": 5, "hdmi_audio": False,
                     "ir": {p: {} for p in IR_PROFILES}}
        try:
            with open(path) as f:
                d = json.load(f)
            if isinstance(d, dict):
                self.data["last"] = d.get("last") if d.get("last") in [m["id"] for m in MODULES] else None
                a = d.get("autostart_s", 5)
                self.data["autostart_s"] = a if isinstance(a, int) and 0 <= a <= 60 else 5
                self.data["hdmi_audio"] = d.get("hdmi_audio") is True
                for prof, binds in (d.get("ir") or {}).items():
                    if prof in IR_PROFILES and isinstance(binds, dict):
                        self.data["ir"][prof] = {k: v for k, v in binds.items() if self.valid(k, v)}
        except (OSError, ValueError):
            pass

    @staticmethod
    def valid(key, v):
        action, _, press = key.partition("/")
        return (action in IR_ACTIONS and press in ("tap", "hold")
                and isinstance(v, int) and 0 <= v < 1 << 32)

    def save(self):
        if not self.path:
            return
        tmp = self.path + ".tmp"
        try:
            with open(tmp, "w") as f:
                json.dump(self.data, f, indent=1)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("saving %s: %s", self.path, e)

    def bind(self, profile, action, press, scan):
        binds = self.data["ir"][profile]
        for k in [k for k, v in binds.items() if v == scan and k.endswith("/" + press)]:
            del binds[k]                # one action per button and press, per profile
        binds[action + "/" + press] = scan
        self.save()

    def forget(self, profile, action, press):
        self.data["ir"][profile].pop(action + "/" + press, None)
        self.save()

    def action_for(self, view, scan, press):
        """The action a button does in a view; the "all" profile fills gaps"""
        for prof in (view, "all"):
            for k, v in self.data["ir"].get(prof, {}).items():
                if v == scan and k.endswith("/" + press):
                    return k.split("/")[0]
        return None


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


# ---------------------------------------------------------------- HDMI audio

class HdmiAudio:
    """The Daisy's USB audio (capture card ApcBox) passed on to the TV over
    HDMI with alsaloop, while the setting is on and both cards exist.
    alsaloop exits when the Daisy goes away; it's started again when it's
    back. Adds ~40 ms plus the TV's own delay: for listening, not playing."""

    def __init__(self, bridge):
        self.b = bridge
        self.proc = None
        self.starts = 0
        self.last_error = None

    def cards(self):
        root = self.b.args.asound
        return (os.path.exists(os.path.join(root, self.b.args.daisy_card)),
                os.path.exists(os.path.join(root, self.b.args.hdmi_card)))

    def info(self):
        daisy, hdmi = self.cards()
        return {"on": self.b.state.data["hdmi_audio"], "running": self.running(),
                "daisy_card": daisy, "hdmi_card": hdmi, "starts": self.starts, "error": self.last_error}

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.running():
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    async def task(self):
        while True:
            want = self.b.state.data["hdmi_audio"]
            daisy, hdmi = self.cards()
            if want and daisy and hdmi and not self.running():
                if self.proc is not None and self.proc.returncode not in (None, 0, -15):
                    self.last_error = "alsaloop exited %s" % self.proc.returncode
                cmd = [self.b.args.alsaloop, "-C", "plughw:CARD=" + self.b.args.daisy_card,
                       "-P", "plughw:CARD=" + self.b.args.hdmi_card, "-t", "40000", "-S", "1"]
                try:
                    self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                                 stderr=subprocess.DEVNULL)
                    self.starts += 1
                    log.info("HDMI audio on (%s)", " ".join(cmd))
                except OSError as e:
                    self.last_error = str(e)
                    self.proc = None
            elif (not want or not daisy) and self.running():
                self.stop()
                log.info("HDMI audio off")
            await asyncio.sleep(2)


# ---------------------------------------------------------------- bridge

class Bridge:
    def __init__(self, args):
        self.args = args
        self.cache = StateCache()
        self.asm = LineAssembler()
        self.clients = set()
        self.fd = None
        self.port = None
        self.serial = None
        self.connected_at = None
        self.last_line = None
        self.last_ask = 0.0
        self.lines = 0
        self.connects = 0
        self.client_resyncs = 0
        self.rejected = 0
        self.lost = asyncio.Event()
        self.sout = bytearray()          # waiting to go to the Daisy
        self.serial_dropped = 0
        self.ctl = 0                     # Daisy control protocol version (P line)
        self.p_line = None
        self.ports = {}                  # num -> MidiPort
        self.pings = {}                  # id -> send time
        self.ping_seq = 0
        self.rtts = collections.deque(maxlen=PING_KEEP)
        self.pings_lost = 0
        self.midi_paths = dict(x.split("=", 1) if "=" in x else (x, os.path.basename(x))
                               for x in (args.midi or []))
        with open(args.ui, "rb") as f:
            self.page = f.read()
        self.selector = b"<!doctype html><title>no selector</title><a href=/apcbox>ApcBox</a>"
        try:
            with open(args.selector, "rb") as f:
                self.selector = f.read()
        except (OSError, TypeError):
            log.warning("no selector page at %s", args.selector)
        self.state = State(args.state)
        self.ir = IrReader(self)
        self.hdmi = HdmiAudio(self)
        self.library = args.library or os.path.join(os.path.dirname(args.state), "samples")
        self.sdcard = args.sdcard
        self.sout_lo = bytearray()       # sample data: goes only when no MIDI is waiting
        self.xfer_q = collections.deque()
        self.xfer_cur = None
        self.xfer_done = collections.deque(maxlen=10)
        self.xfer_seq = 0
        self.xfer_wake = asyncio.Event()
        self.xfer_replies = asyncio.Queue()

    # -- status
    def status(self):
        t = now()
        return {
            "daisy": self.fd is not None,
            "port": self.port,
            "serial": self.serial,
            "data_age_s": None if self.last_line is None else round(t - self.last_line, 3),
            "connected_s": None if self.connected_at is None else round(t - self.connected_at, 1),
            "lines": self.lines,
            "bad_lines": self.asm.bad,
            "connects": self.connects,
            "clients": len(self.clients),
            "client_resyncs": self.client_resyncs + sum(c.resyncs for c in self.clients),
            "rejected_clients": self.rejected,
            "control": self.ctl,
            "serial_out_bytes": len(self.sout),
            "serial_dropped": self.serial_dropped,
            "controllers": [p.info(self.kinds()) for p in sorted(self.ports.values(), key=lambda p: p.num)],
            "ping_ms": self.ping_stats(),
            "ir": self.ir.info(),
            "samples": self.samples_info(),
            "hdmi_audio": self.hdmi.info(),
        }

    def samples_info(self):
        return {"library": self.library, "sdcard": self.sdcard if self.sd_ready() else None,
                "ffmpeg": bool(shutil.which("ffmpeg")), "daisy_ready": self.ctl >= 3,
                "current": self.xfer_cur.info() if self.xfer_cur else None,
                "queued": [t.info() for t in self.xfer_q],
                "recent": [t.info() for t in self.xfer_done]}

    def sd_ready(self):
        return bool(self.sdcard) and os.path.isdir(self.sdcard) and (
            os.path.ismount(self.sdcard) or self.args.sdcard_any)

    def roots(self, src):
        if src == "lib":
            os.makedirs(self.library, exist_ok=True)
            return self.library
        if src == "sd" and self.sd_ready():
            return self.sdcard
        return None

    def list_samples(self, src, rel):
        root = self.roots(src)
        p = safe_join(root, rel)
        if p is None or not os.path.isdir(p):
            return 400, {"error": "no such folder" if root else "no SD card in the T95"}
        dirs, files = [], []
        try:
            for e in sorted(os.scandir(p), key=lambda e: e.name.lower()):
                if e.name.startswith("."):
                    continue
                if e.is_dir(follow_symlinks=False):
                    dirs.append(e.name)
                elif e.is_file(follow_symlinks=False) and e.name.lower().endswith(AUDIO_EXT):
                    files.append({"name": e.name, "size": e.stat().st_size})
        except OSError as e:
            return 400, {"error": str(e)}
        rel = os.path.relpath(p, os.path.realpath(root))
        return 200, {"src": src, "path": "" if rel == "." else rel, "dirs": dirs[:500], "files": files[:2000]}

    def xfer_event(self, t):
        self.broadcast(sse_event("xfer", t.info()))

    def queue_send(self, src, rel, track):
        root = self.roots(src)
        p = safe_join(root, rel)
        if p is None or not os.path.isfile(p):
            return 400, {"error": "no such file"}
        if not isinstance(track, int) or not 1 <= track <= 16:
            return 400, {"error": "track 1-16"}
        if self.ctl < 3:
            return 503, {"error": "the Daisy's firmware can't take samples yet (v13.8+)"}
        self.xfer_seq += 1
        t = Transfer(self.xfer_seq % 250 + 1, src, rel, p, track)
        self.xfer_q.append(t)
        self.xfer_wake.set()
        self.xfer_event(t)
        return 200, {"ok": True, "id": t.id}

    async def await_reply(self, prefix, timeout):
        end = now() + timeout
        while True:
            left = end - now()
            if left <= 0:
                return None
            try:
                line = await asyncio.wait_for(self.xfer_replies.get(), left)
            except asyncio.TimeoutError:
                return None
            if line.startswith(prefix):
                return line

    async def xfer_task(self):
        while True:
            await self.xfer_wake.wait()
            self.xfer_wake.clear()
            while self.xfer_q:
                t = self.xfer_q.popleft()
                self.xfer_cur = t
                try:
                    await self.run_transfer(t)
                except Exception as e:     # one bad sample mustn't stop the bridge
                    log.exception("transfer %s", t.name)
                    t.state, t.error = "failed", str(e)[:200]
                t.finished = now()
                self.xfer_event(t)
                self.xfer_done.appendleft(t)
                self.xfer_cur = None

    async def run_transfer(self, t):
        t.state = "converting"
        self.xfer_event(t)
        data, t.how = await asyncio.get_running_loop().run_in_executor(None, convert_for_daisy, t.path)
        t.size = len(data)
        t.chunks = (len(data) + XFER_CHUNK - 1) // XFER_CHUNK
        frames = None
        try:
            if data[22:24] == b"\x01\x00" and data[34:36] == b"\x10\x00":   # mono 16-bit
                frames = (len(data) - 44) // 2
        except IndexError:
            pass
        t.frames = frames
        while not self.xfer_replies.empty():
            self.xfer_replies.get_nowait()
        name = "".join(c if c.isalnum() or c in "._-" else "_" for c in t.name)[:32]
        t.started = now()
        t.state = "sending"
        self.ctl_send(b"%sFO %d %d %d %s\n" % (ESC, t.id, t.track, t.size, name.encode()))
        r = await self.await_reply("FO %d " % t.id, 3)
        if not r or " ok" not in r:
            raise RuntimeError("Daisy refused: " + (r.split(" err ", 1)[1] if r and " err " in r else "no answer"))
        try:
            t.capacity = int(r.split()[3])
        except (IndexError, ValueError):
            pass
        self.xfer_event(t)
        acked = set()
        sent = {}          # seq -> (time, tries)
        nxt = 0
        last_event = now()

        def send(seq):
            chunk = data[seq * XFER_CHUNK:(seq + 1) * XFER_CHUNK]
            crc = binascii.crc32(chunk) & 0xFFFFFFFF
            tries = sent.get(seq, (0, 0))[1] + 1
            if tries > XFER_TRIES:
                raise RuntimeError("chunk %d failed %d times" % (seq, XFER_TRIES))
            if tries > 1:
                t.resent += 1
            sent[seq] = (now(), tries)
            self.data_send(b"%sFD %d %d %x %s\n" % (ESC, t.id, seq, crc, base64.b64encode(chunk)))

        while len(acked) < t.chunks:
            if self.fd is None or self.ctl < 3:
                raise RuntimeError("the Daisy went away")
            # fill the window
            while nxt < t.chunks and len(sent) - len(acked & sent.keys()) < XFER_WINDOW:
                if nxt not in acked:
                    send(nxt)
                nxt += 1
            # resend what timed out
            tnow = now()
            for seq, (ts, _) in list(sent.items()):
                if seq not in acked and tnow - ts > XFER_RESEND_S:
                    send(seq)
            try:
                line = await asyncio.wait_for(self.xfer_replies.get(), 0.1)
            except asyncio.TimeoutError:
                continue
            parts = line.split()
            if parts[0] != "FA" or len(parts) < 4 or parts[1] != str(t.id):
                continue
            seq = int(parts[2])
            if parts[3] == "ok":
                acked.add(seq)
                t.acked = len(acked)
            elif len(parts) > 4 and parts[4] == "stale":
                raise RuntimeError("the Daisy dropped the transfer")
            elif seq not in acked:
                send(seq)          # crc / length: send it again now
            if now() - last_event > 0.25:
                last_event = now()
                self.xfer_event(t)
        self.ctl_send(b"%sFC %d\n" % (ESC, t.id))
        r = await self.await_reply("FC %d " % t.id, 3)
        if not r or " ok" not in r:
            raise RuntimeError("commit: " + (r or "no answer"))
        t.state = "loading"
        self.xfer_event(t)
        r = await self.await_reply("FL %d " % t.id, 15)
        if not r or not r.endswith("ok"):
            raise RuntimeError("the Daisy couldn't load it" if r else "no answer while loading")
        t.state = "done"
        log.info("sample %s -> track %d: %d bytes in %.1f s (%d resent)", t.name, t.track, t.size,
                 now() - t.started, t.resent)

    def import_sd(self, rel):
        """Copy a file or a folder (audio only) from the SD card into the library"""
        root = self.roots("sd")
        src = safe_join(root, rel)
        if src is None or not os.path.exists(src):
            return 0
        dest_root = os.path.join(self.library, "sdcard")
        n = 0
        if os.path.isfile(src):
            items = [(src, os.path.join(dest_root, os.path.basename(src)))]
        else:
            items = []
            for d, _, fs in os.walk(src):
                for f in fs:
                    if f.lower().endswith(AUDIO_EXT) and not f.startswith("."):
                        full = os.path.join(d, f)
                        items.append((full, os.path.join(dest_root, os.path.relpath(full, os.path.dirname(src)))))
        for a, b in items:
            os.makedirs(os.path.dirname(b), exist_ok=True)
            if not os.path.exists(b) or os.path.getsize(b) != os.path.getsize(a):
                shutil.copyfile(a, b + ".part")
                os.replace(b + ".part", b)
                n += 1
        return n

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

    def announce(self):
        self.broadcast(sse_event("bridge", self.status()))

    def broadcast(self, chunk):
        for c in list(self.clients):
            c.push(chunk)

    def state_json(self):
        return {"modules": MODULES, "last": self.state.data["last"],
                "autostart_s": self.state.data["autostart_s"],
                "hdmi_audio": self.hdmi.info(),
                "ir": {"profiles": self.state.data["ir"], "actions": IR_ACTIONS,
                       "profile_names": IR_PROFILES, **self.ir.info()}}

    def post(self, path, body):
        """(code, json) for a POST. Small JSON bodies only."""
        try:
            d = json.loads(body.decode() or "{}")
            if not isinstance(d, dict):
                raise ValueError
        except ValueError:
            return 400, {"error": "JSON object expected"}
        if path == "/select":
            m = d.get("module")
            if m not in [x["id"] for x in MODULES if x["ready"]]:
                return 400, {"error": "unknown or unready module"}
            self.state.data["last"] = m
            self.state.save()
            return 200, {"ok": True}
        if path == "/settings":
            if "autostart_s" in d:
                a = d["autostart_s"]
                if not isinstance(a, int) or not 0 <= a <= 60:
                    return 400, {"error": "autostart_s: 0-60"}
                self.state.data["autostart_s"] = a
            if "hdmi_audio" in d:
                if not isinstance(d["hdmi_audio"], bool):
                    return 400, {"error": "hdmi_audio: true/false"}
                self.state.data["hdmi_audio"] = d["hdmi_audio"]
            if not ({"autostart_s", "hdmi_audio"} & d.keys()):
                return 400, {"error": "autostart_s or hdmi_audio"}
            self.state.save()
            return 200, {"ok": True}
        if path in ("/ir/learn", "/ir/forget"):
            prof, action, press = d.get("profile"), d.get("action"), d.get("press", "tap")
            if prof not in IR_PROFILES or action not in IR_ACTIONS or press not in ("tap", "hold"):
                return 400, {"error": "profile, action, press (tap/hold) expected"}
            if path == "/ir/forget":
                self.state.forget(prof, action, press)
            else:
                self.ir.learning = (prof, action, press, now() + IR_LEARN_S)
            return 200, {"ok": True}
        if path == "/ir/cancel":
            self.ir.learning = None
            return 200, {"ok": True}
        if path == "/samples/send":
            return self.queue_send(d.get("src", "lib"), d.get("path", ""), d.get("track"))
        if path == "/samples/delete":
            p = safe_join(self.roots("lib"), d.get("path", ""))
            if p is None or not os.path.isfile(p):
                return 400, {"error": "no such file in the library"}
            os.remove(p)
            return 200, {"ok": True}
        if path == "/samples/cancel":
            n = len(self.xfer_q)
            self.xfer_q.clear()
            return 200, {"ok": True, "dropped": n}
        return 404, {"error": "not found"}

    # -- serial side
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
            for p in self.ports.values():  # (re)announce everything
                self.announce_port(p)
            return
        # The Daisy dropped a port we still have (its keepalive timed out, it
        # rebooted without a USB reset...): tell it again, at most every 2 s
        try:
            att = int(line.split()[2], 16)
        except (IndexError, ValueError):
            return
        t = now()
        for p in self.ports.values():
            if not att & (1 << (p.num - 1)) and t - p.announced > 2.0:
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

    # -- controllers
    def midi_candidates(self):
        if self.midi_paths:            # --midi: fixed paths (tests)
            return {path: (path, name) for path, name in self.midi_paths.items() if os.path.exists(path)}
        return find_midi()

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
        p = MidiPort(free[0], key, path, clean_name(name), fd)
        self.ports[p.num] = p
        asyncio.get_running_loop().add_reader(fd, self.on_midi_readable, p)
        log.info("controller %s on %s: port %d", p.name, path, p.num)
        self.announce_port(p)
        self.announce()

    def detach_midi(self, p, why=""):
        loop = asyncio.get_running_loop()
        loop.remove_reader(p.fd)
        loop.remove_writer(p.fd)
        try:
            os.close(p.fd)
        except OSError:
            pass
        self.ports.pop(p.num, None)
        log.info("controller %s (port %d) gone%s", p.name, p.num, why)
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
            if not self.ctl_send(b"%sM%d %s\n" % (ESC, p.num, msg.hex().encode())):
                p.dropped_in += 1

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
        loop = asyncio.get_running_loop()
        try:
            while True:
                if not p.out_buf:
                    if p.out_sysex:
                        p.out_buf = p.out_sysex.popleft()
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
        busy = bool(p.out_buf or p.out_sysex or p.out_led)
        if busy and not p.writing:
            loop.add_writer(p.fd, self.midi_flush, p)
        elif not busy and p.writing:
            loop.remove_writer(p.fd)
        p.writing = busy

    async def midi_task(self):
        if self.args.no_midi:
            return
        while True:
            have = {p.key: p for p in self.ports.values()}
            cand = self.midi_candidates()
            for key, p in have.items():
                if key not in cand:
                    self.detach_midi(p, ": unplugged")
            for key, (path, name) in cand.items():
                if key not in have:
                    self.attach_midi(key, path, name)
            await asyncio.sleep(RESCAN_S)

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

    # -- browser side
    async def handle_http(self, reader, writer):
        peer = writer.get_extra_info("peername")
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            writer.close()
            return
        try:
            method, target = head.split(b"\r\n", 1)[0].decode("latin-1").split(" ")[:2]
        except ValueError:
            method, target = "", ""
        path, _, query = target.partition("?")
        params = dict(kv.partition("=")[::2] for kv in query.split("&") if kv)
        try:
            if method == "POST":
                n = 0
                for h in head.decode("latin-1").split("\r\n")[1:]:
                    k, _, v = h.partition(":")
                    if k.strip().lower() == "content-length":
                        n = int(v.strip() or 0)
                if path == "/samples/upload":
                    code, out = await self.upload(reader, n, params.get("name", ""))
                    await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
                    writer.close()
                    return
                if path == "/samples/import":
                    body = await asyncio.wait_for(reader.readexactly(min(n, POST_MAX)), 10) if n else b""
                    try:
                        rel = json.loads(body.decode() or "{}").get("path", "")
                    except (ValueError, AttributeError):
                        rel = None
                    if rel is None or not self.sd_ready():
                        code, out = 400, {"error": "no SD card in the T95" if rel is not None else "JSON expected"}
                    else:
                        cnt = await asyncio.get_running_loop().run_in_executor(None, self.import_sd, rel)
                        code, out = 200, {"ok": True, "copied": cnt}
                    await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
                    writer.close()
                    return
                if n > POST_MAX:
                    raise ValueError
                body = await asyncio.wait_for(reader.readexactly(n), 10) if n else b""
                code, out = self.post(path, body)
                await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
            elif method != "GET":
                await self.reply(writer, 405, "text/plain", b"GET or POST\n")
            elif path in ("/", "/index.html"):
                await self.reply(writer, 200, "text/html; charset=utf-8", self.selector)
            elif path in ("/apcbox", "/apcbox-ui.html"):
                await self.reply(writer, 200, "text/html; charset=utf-8", self.page)
            elif path == "/ui":
                # The page's encoder buttons (same URLs as the Mac build)
                a = params.get("a", "")
                act = {"push": "push", "back": "back"}.get(a)
                if a == "turn":
                    try:
                        d = max(-16, min(16, int(params.get("d", "0"))))
                        act = "turn %d" % d if d else None
                    except ValueError:
                        act = None
                if self.ctl < 2:
                    await self.reply(writer, 503, "text/plain", b"the Daisy's firmware has no UI command (v13.7+)\n")
                elif act and self.ui_send(act):
                    await self.reply(writer, 200, "text/plain", b"ok\n")
                else:
                    await self.reply(writer, 404, "text/plain", b"unknown UI action\n")
            elif path == "/samples":
                code, out = self.list_samples(params.get("src", "lib"),
                                              urllib.parse.unquote(params.get("path", "")))
                await self.reply(writer, code, "application/json", json.dumps(out).encode() + b"\n")
            elif path == "/state":
                await self.reply(writer, 200, "application/json", json.dumps(self.state_json(), indent=1).encode() + b"\n")
            elif path == "/health":
                await self.reply(writer, 200, "application/json", json.dumps(self.status(), indent=1).encode() + b"\n")
            elif path == "/caps":
                caps = {"source": "daisy-bridge", "bridge": 6, "control": self.ctl >= 2, "midi": True, "ir": True,
                        "samples": True, "https_port": self.args.https_port,
                        "selector": "/", "ui": self.ctl >= 2, "tft": False}
                await self.reply(writer, 200, "application/json", json.dumps(caps).encode() + b"\n")
            elif path == "/events":
                await self.stream(reader, writer, peer, params.get("view", "")[:32])
                return
            else:
                await self.reply(writer, 404, "text/plain", b"not found\n")
        except (ConnectionError, asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError):
            pass
        writer.close()

    async def upload(self, reader, n, name):
        """Raw body into the library's uploads folder"""
        name = os.path.basename(urllib.parse.unquote(name)).strip()
        if not name or name.startswith(".") or not name.lower().endswith(AUDIO_EXT):
            return 400, {"error": "name an audio file (?name=kick.wav)"}
        if not 0 < n <= UPLOAD_MAX:
            return 400, {"error": "empty or over %d MB" % (UPLOAD_MAX >> 20)}
        d = os.path.join(self.library, "uploads")
        os.makedirs(d, exist_ok=True)
        dest = os.path.join(d, name)
        part = dest + ".part"
        try:
            with open(part, "wb") as f:
                left = n
                while left:
                    chunk = await asyncio.wait_for(reader.read(min(left, 65536)), 30)
                    if not chunk:
                        raise ConnectionError("upload cut off")
                    f.write(chunk)
                    left -= len(chunk)
            os.replace(part, dest)
        except (OSError, ConnectionError, asyncio.TimeoutError) as e:
            try:
                os.remove(part)
            except OSError:
                pass
            return 400, {"error": str(e)}
        return 200, {"ok": True, "path": "uploads/" + name}

    @staticmethod
    async def reply(writer, code, ctype, body):
        reason = {200: "OK", 400: "Bad Request", 404: "Not Found", 405: "Method Not Allowed",
                  503: "Service Unavailable"}.get(code, "Error")
        writer.write(("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n" % (code, reason, ctype, len(body))).encode() + body)
        await asyncio.wait_for(writer.drain(), 10)

    async def stream(self, reader, writer, peer, view=""):
        if len(self.clients) >= MAX_CLIENTS:
            self.rejected += 1
            await self.reply(writer, 503, "text/plain", b"too many clients\n")
            writer.close()
            return
        c = Client(writer, peer, view)
        self.clients.add(c)

        async def watch_eof():
            # A browser that leaves while the Daisy is quiet must free its slot now,
            # not at the next write
            try:
                while await reader.read(4096):
                    pass
            except (ConnectionError, OSError):
                pass
            c.closed = True
            c.wake.set()
        eof = asyncio.create_task(watch_eof())
        log.info("browser %s connected (%d)", peer, len(self.clients))
        try:
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         b"Cache-Control: no-store\r\nConnection: keep-alive\r\n"
                         b"X-Accel-Buffering: no\r\n\r\nretry: 1000\n\n")
            last_ping = now()
            while not c.closed:
                if c.need_snapshot:
                    c.need_snapshot = False
                    snap = b"".join(sse_data(l) for l in self.cache.snapshot())
                    writer.write(sse_event("bridge", self.status()) + snap)
                while c.q:
                    chunk = c.q.popleft()
                    c.qbytes -= len(chunk)
                    writer.write(chunk)
                # Only this browser's task waits here; the serial reader never does
                await asyncio.wait_for(writer.drain(), 30)
                if now() - last_ping > 15:
                    writer.write(b": ping\n\n")
                    last_ping = now()
                c.wake.clear()
                if not c.q and not c.need_snapshot and not c.closed:
                    try:
                        await asyncio.wait_for(c.wake.wait(), 15)
                    except asyncio.TimeoutError:
                        pass
        except (ConnectionError, asyncio.TimeoutError, OSError):
            pass
        finally:
            eof.cancel()
            self.clients.discard(c)
            self.client_resyncs += c.resyncs
            c.closed = True
            writer.close()
            log.info("browser %s left (%d)", peer, len(self.clients))


def local_ips():
    ips = set()
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=5).stdout
        ips.update(out.split())
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))     # no packet is sent; picks the outgoing address
            ips.add(s.getsockname()[0])
    except OSError:
        pass
    ips.add("127.0.0.1")
    return sorted(i for i in ips if ":" not in i)


def tls_context(directory):
    """Self-signed certificate for the HTTPS port (phones only give the page
    the microphone over HTTPS). Made once with openssl, kept in the state
    folder; a new one when the T95's address changes."""
    os.makedirs(directory, exist_ok=True)
    crt, key = os.path.join(directory, "tls.crt"), os.path.join(directory, "tls.key")
    ips = local_ips()
    host = socket.gethostname()
    want = ",".join(["DNS:" + host, "DNS:" + host + ".local"] + ["IP:" + i for i in ips])
    stamp = os.path.join(directory, "tls.san")
    have = open(stamp).read() if os.path.exists(stamp) else ""
    if not (os.path.exists(crt) and os.path.exists(key)) or have != want:
        r = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
                            "-subj", "/CN=GhettoRack " + host, "-addext", "subjectAltName=" + want,
                            "-keyout", key, "-out", crt], capture_output=True, timeout=60)
        if r.returncode != 0:
            raise OSError("openssl: " + r.stderr.decode(errors="replace")[:200])
        os.chmod(key, 0o600)
        with open(stamp, "w") as f:
            f.write(want)
        log.info("made a TLS certificate for %s", want)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    return ctx


async def main_async(args):
    b = Bridge(args)
    server = await asyncio.start_server(b.handle_http, args.host, args.http_port, limit=16384)
    log.info("serving http://%s:%d/", args.host, args.http_port)
    servers = [server.serve_forever()]
    if args.https_port:
        try:
            ctx = tls_context(os.path.dirname(args.state))
            tls = await asyncio.start_server(b.handle_http, args.host, args.https_port, ssl=ctx, limit=16384)
            servers.append(tls.serve_forever())
            log.info("serving https://%s:%d/ (self-signed: the browser warns once)", args.host, args.https_port)
        except (OSError, ssl.SSLError) as e:
            log.warning("no HTTPS (the page's record button needs it on phones): %s", e)
    async with server:
        await asyncio.gather(*servers, b.serial_task(), b.midi_task(), b.control_task(),
                             b.ir.task(), b.xfer_task(), b.hdmi.task())


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--http-port", type=int, default=8040)
    ap.add_argument("--https-port", type=int, default=8443, help="0 = no HTTPS")
    ap.add_argument("--serial", help="USB serial number of the Daisy to use (several connected)")
    ap.add_argument("--port", help="tty path instead of finding the Daisy by VID/PID")
    ap.add_argument("--midi", action="append", metavar="PATH=NAME",
                    help="controller device path instead of scanning /sys (repeatable)")
    ap.add_argument("--no-midi", action="store_true", help="don't touch MIDI controllers")
    ap.add_argument("--ui", default=os.path.join(here, "apcbox-ui.html")
                    if os.path.exists(os.path.join(here, "apcbox-ui.html"))
                    else os.path.join(here, "..", "..", "daisy", "ui", "apcbox-ui.html"))
    ap.add_argument("--selector", default=next((p for p in (os.path.join(here, "selector.html"),
                    os.path.join(here, "..", "ui", "selector.html")) if os.path.exists(p)), None))
    ap.add_argument("--state", default=os.path.join(os.environ.get("STATE_DIRECTORY", "/var/lib/apcbox-bridge"),
                    "state.json"), help="where the last module and IR bindings are kept")
    ap.add_argument("--ir", metavar="PATH", help="IR input event device instead of finding it under /sys")
    ap.add_argument("--ir-lirc", metavar="PATH", help="IR lirc device instead of finding it under /sys")
    ap.add_argument("--library", help="sample library folder (default: next to the state file)")
    ap.add_argument("--alsaloop", default="/usr/bin/alsaloop", help="for the HDMI audio setting")
    ap.add_argument("--asound", default="/proc/asound", help="where the sound cards are listed by name")
    ap.add_argument("--daisy-card", default="ApcBox", help="the Daisy's USB audio card name")
    ap.add_argument("--hdmi-card", default="HDMI", help="the T95's HDMI audio card name")
    ap.add_argument("--sdcard", default="/media/sdcard", help="where the T95's SD card is mounted (udev rule)")
    ap.add_argument("--sdcard-any", action="store_true", help="use --sdcard even if it isn't a mount point (tests)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(levelname)s %(message)s", stream=sys.stderr)
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
