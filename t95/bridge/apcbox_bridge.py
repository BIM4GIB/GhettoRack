#!/usr/bin/env python3
"""ApcBox telemetry bridge: Daisy USB serial -> SSE for browsers.

Read-only. The only thing it ever sends the Daisy is ESC R (full state dump):
on connect, and again if the Daisy goes quiet. Python 3 standard library only.

    Daisy --USB serial--> bridge --SSE /events--> browsers

  /             the monitor page (daisy/ui/apcbox-ui.html)
  /events       SSE: a snapshot of the latest state, then every line live.
                Default events carry one report line each; "bridge" events
                carry the bridge's own status as JSON.
  /health       JSON: Daisy link, data age, counters, clients
  /caps         JSON: what this server supports (the page reads it to hide
                the Mac build's /ui and /tft.bmp controls)

The Daisy is found by USB VID/PID (and optionally serial) under /sys, not by
tty index. --port forces a path instead (tests, odd setups).
See t95/bridge/README.md.
"""
import argparse
import asyncio
import collections
import glob
import json
import logging
import os
import sys
import termios
import time

VID, PID = "1209", "0001"
ESC_R = b"\x1bR"          # full state dump; v13.2 and older also act on it
MAX_LINE = 4096           # longest real line is D: 2 + 2048 hex digits
CLIENT_QUEUE_BYTES = 512 * 1024
MAX_CLIENTS = 16
STALE_S = 3.0             # no line for this long: ask for a dump again
RESCAN_S = 1.0
LOG_KEEP = 100            # L lines replayed to a new browser
CACHE_MAX = 512

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
    except termios.error:
        os.close(fd)
        raise
    return fd


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

    def __init__(self, writer, peer):
        self.writer = writer
        self.peer = peer
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
        with open(args.ui, "rb") as f:
            self.page = f.read()

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
        }

    def announce(self):
        ev = sse_event("bridge", self.status())
        for c in list(self.clients):
            c.push(ev)

    # -- serial side
    def ask_dump(self):
        self.last_ask = now()
        try:
            os.write(self.fd, ESC_R)
        except OSError as e:
            log.warning("write to %s: %s", self.port, e)

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
            self.cache.put(line)
            chunk = sse_data(line)
            for c in list(self.clients):
                c.push(chunk)

    def close_port(self):
        loop = asyncio.get_running_loop()
        if self.fd is not None:
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
        path = target.split("?", 1)[0]
        try:
            if method != "GET":
                await self.reply(writer, 405, "text/plain", b"GET only\n")
            elif path in ("/", "/index.html", "/apcbox-ui.html"):
                await self.reply(writer, 200, "text/html; charset=utf-8", self.page)
            elif path == "/health":
                await self.reply(writer, 200, "application/json", json.dumps(self.status(), indent=1).encode() + b"\n")
            elif path == "/caps":
                caps = {"source": "daisy-bridge", "bridge": 1, "control": False, "ui": False, "tft": False}
                await self.reply(writer, 200, "application/json", json.dumps(caps).encode() + b"\n")
            elif path == "/events":
                await self.stream(reader, writer, peer)
                return
            else:
                await self.reply(writer, 404, "text/plain", b"not found\n")
        except (ConnectionError, asyncio.TimeoutError):
            pass
        writer.close()

    @staticmethod
    async def reply(writer, code, ctype, body):
        reason = {200: "OK", 404: "Not Found", 405: "Method Not Allowed", 503: "Service Unavailable"}[code]
        writer.write(("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n" % (code, reason, ctype, len(body))).encode() + body)
        await asyncio.wait_for(writer.drain(), 10)

    async def stream(self, reader, writer, peer):
        if len(self.clients) >= MAX_CLIENTS:
            self.rejected += 1
            await self.reply(writer, 503, "text/plain", b"too many clients\n")
            writer.close()
            return
        c = Client(writer, peer)
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


async def main_async(args):
    b = Bridge(args)
    server = await asyncio.start_server(b.handle_http, args.host, args.http_port, limit=16384)
    log.info("serving http://%s:%d/", args.host, args.http_port)
    async with server:
        await asyncio.gather(server.serve_forever(), b.serial_task())


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--http-port", type=int, default=8040)
    ap.add_argument("--serial", help="USB serial number of the Daisy to use (several connected)")
    ap.add_argument("--port", help="tty path instead of finding the Daisy by VID/PID")
    ap.add_argument("--ui", default=os.path.join(here, "apcbox-ui.html")
                    if os.path.exists(os.path.join(here, "apcbox-ui.html"))
                    else os.path.join(here, "..", "..", "daisy", "ui", "apcbox-ui.html"))
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
