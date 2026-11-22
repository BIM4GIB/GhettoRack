#!/usr/bin/env python3
"""Bridge tests against a fake Daisy on a pseudo-terminal.

    python3 t95/bridge/test_bridge.py        (Linux; about 30 s)

BridgeTest: echo off at open, ESC R on connect and when the Daisy goes
quiet, lines split across reads, garbage and over-long lines, the snapshot a
new browser gets, several browsers, a browser that stops reading while lines
pour in, too many browsers, and the Daisy unplugging and coming back.

MidiTest: fake controllers on ptys. Nothing forwarded before the Daisy says
"P 1", port names and keepalives after, running status, clock bytes mid
message, sysex in pieces, LED lines out in order, LED coalescing when a
controller stops reading, controller and Daisy hotplug, a Daisy that stops
reading while a controller floods, ping round trips.
"""
import errno
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "apcbox_bridge.py")
UI = os.path.join(HERE, "..", "..", "daisy", "ui", "apcbox-ui.html")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakeDaisy:
    """The master side of a pty, with the slave reachable through a symlink"""

    def __init__(self, link):
        self.link = link
        self.plug()

    def plug(self):
        self.master, slave = os.openpty()
        self.slave_name = os.ttyname(slave)
        # Leave the slave at the tty defaults (echo on): the bridge must fix that
        os.close(slave)
        tmp = self.link + ".new"
        os.symlink(self.slave_name, tmp)
        os.replace(tmp, self.link)
        self.rx = bytearray()

    def unplug(self):
        os.unlink(self.link)
        os.close(self.master)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        view = memoryview(data)
        while view:
            n = os.write(self.master, view[:4096])
            view = view[n:]

    def read_rx(self, wait=0.3):
        """What the bridge (and the tty's echo, if it were on) sent back"""
        os.set_blocking(self.master, False)
        end = time.time() + wait
        while time.time() < end:
            try:
                d = os.read(self.master, 65536)
                if d:
                    self.rx += d
            except BlockingIOError:
                time.sleep(0.02)
            except OSError:
                break
        os.set_blocking(self.master, True)
        return bytes(self.rx)


class SseClient:
    """Reads /events on a thread; keeps default-event lines and bridge events"""

    def __init__(self, port, rcvbuf=None, read=True, path="/events"):
        self.sock = socket.socket()
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.connect(("127.0.0.1", port))
        self.sock.sendall(b"GET %s HTTP/1.1\r\nHost: x\r\n\r\n" % path.encode())
        self.lines, self.events, self.status = [], [], None
        self.named = []             # (event, data) for other named events
        self.lock = threading.Lock()
        self.closed = False
        if read:
            threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        buf = b""
        head_done = False
        self.sock.settimeout(60)
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    break
                buf += d
                if not head_done:
                    if b"\r\n\r\n" not in buf:
                        continue
                    head, buf = buf.split(b"\r\n\r\n", 1)
                    self.status = head.split(b"\r\n")[0].decode()
                    head_done = True
                while b"\n\n" in buf:
                    msg, buf = buf.split(b"\n\n", 1)
                    ev, data = "message", []
                    for l in msg.decode().split("\n"):
                        if l.startswith("event: "):
                            ev = l[7:]
                        elif l.startswith("data: "):
                            data.append(l[6:])
                    if not data:
                        continue
                    with self.lock:
                        if ev == "bridge":
                            self.events.append(json.loads(data[0]))
                        elif ev == "message":
                            self.lines.append(data[0])
                        else:
                            self.named.append((ev, json.loads(data[0])))
        except OSError:
            pass
        self.closed = True

    def wait_for(self, pred, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if pred(self):
                    return True
            time.sleep(0.02)
        return False

    def close(self):
        try:
            # shutdown first: a close() while the reader thread sits in recv()
            # doesn't send the FIN on Linux, so the browser would never "leave"
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class BridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.link = os.path.join(cls.dir, "daisy")
        cls.daisy = FakeDaisy(cls.link)
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", cls.link, "--host", "127.0.0.1",
             "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", os.path.join(cls.dir, "state.json")],
            stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.wait_health(lambda h: h["daisy"], 10)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(5)

    @classmethod
    def get(cls, path):
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (cls.http, path), timeout=5) as r:
            return r.status, r.headers.get("Content-Type"), r.read()

    @classmethod
    def health(cls):
        return json.loads(cls.get("/health")[2])

    @classmethod
    def wait_health(cls, pred, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            try:
                h = cls.health()
                if pred(h):
                    return h
            except OSError:
                pass
            time.sleep(0.05)
        raise AssertionError("health never matched: %s" % cls.health())

    # Tests run in name order; t1.. keeps them in a sensible sequence

    def test_t1_esc_r_on_connect_and_echo_off(self):
        rx = self.daisy.read_rx(0.5)
        self.assertTrue(rx.startswith(b"\x1bR"), rx[:20])
        self.daisy.rx.clear()
        # A burst full of R and B: with echo on these would come straight back
        self.daisy.send("V 7\nB 1 2 3 4\nBF Breaks\nD 0BBB1B\nL RESET BOOT R B\n")
        rx = self.daisy.read_rx(0.5)
        self.assertEqual(rx.replace(b"\x1bR", b""), b"", "anything but ESC R came back: %r" % rx)

    def test_t2_pages(self):
        code, ctype, body = self.get("/")
        self.assertEqual(code, 200)
        self.assertIn(b"GHETTORACK", body)          # the module selector
        code, ctype, body = self.get("/apcbox")
        self.assertEqual(code, 200)
        self.assertIn(b"APCBOX", body)
        caps = json.loads(self.get("/caps")[2])
        self.assertEqual(caps["source"], "daisy-bridge")
        self.assertFalse(caps["ui"] or caps["tft"] or caps["control"])
        with self.assertRaises(urllib.error.HTTPError) as e:
            self.get("/tft.bmp")
        self.assertEqual(e.exception.code, 404)

    def test_t3_fragmented_lines(self):
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: c.events))
        for ch in "T 3 1 2 3 4 5 6 7 8 9 10 11 12 0\n":
            self.daisy.send(ch)
            time.sleep(0.002)
        d = "D " + "AB" * 1024
        self.daisy.send(d[:7]); time.sleep(0.05)
        self.daisy.send(d[7:1500]); time.sleep(0.05)
        self.daisy.send(d[1500:] + "\r\nN 3 kick.wav\n")
        ok = c.wait_for(lambda c: "N 3 kick.wav" in c.lines)
        self.assertTrue(ok, c.lines[-5:])
        self.assertIn("T 3 1 2 3 4 5 6 7 8 9 10 11 12 0", c.lines)
        self.assertIn(d, c.lines)
        c.close()

    def test_t4_garbage_dropped(self):
        bad0 = self.health()["bad_lines"]
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: c.events))
        self.daisy.send(b"x" * 10000)                 # over-long, no newline
        self.daisy.send(b"still the long one\n")
        self.daisy.send(b"\x01\x02\xffjunk\n")         # binary
        self.daisy.send(b"M 1 2 3 4 5 6 7 8 9 64\n")
        self.assertTrue(c.wait_for(lambda c: "M 1 2 3 4 5 6 7 8 9 64" in c.lines))
        self.assertFalse(any("junk" in l or "long one" in l or "xxxx" in l for l in c.lines))
        self.assertGreaterEqual(self.health()["bad_lines"], bad0 + 2)
        c.close()

    def test_t5_snapshot_for_late_browser(self):
        self.daisy.send("V 7\nS 1 4 0\nT 0 old\nT 1 one\nT 0 new\nE 144 0 53 127\nL hello snapshot\nS 1 5 0\n")
        time.sleep(0.3)
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: "L hello snapshot" in c.lines))
        self.assertEqual(c.lines[0], "V 7")
        self.assertIn("T 0 new", c.lines)
        self.assertIn("T 1 one", c.lines)
        self.assertIn("S 1 5 0", c.lines)
        self.assertNotIn("T 0 old", c.lines)
        self.assertNotIn("S 1 4 0", c.lines)
        self.assertNotIn("E 144 0 53 127", c.lines)
        self.assertTrue(c.events and c.events[0]["daisy"])
        c.close()

    def test_t6_many_browsers(self):
        cs = [SseClient(self.http) for _ in range(5)]
        for c in cs:
            self.assertTrue(c.wait_for(lambda c: c.events))
        self.daisy.send("L to everyone\n")
        for c in cs:
            self.assertTrue(c.wait_for(lambda c: c.lines.count("L to everyone") >= 1))
        self.wait_health(lambda h: h["clients"] >= 5)
        for c in cs:
            c.close()
        self.wait_health(lambda h: h["clients"] == 0)

    def test_t7_stalled_browser_doesnt_hold_anyone_up(self):
        slow = SseClient(self.http, rcvbuf=4096, read=False)
        fast = SseClient(self.http)
        self.assertTrue(fast.wait_for(lambda c: c.events))
        self.wait_health(lambda h: h["clients"] >= 2)
        n = 5000                                   # ~10 MB of D lines
        pad = "0" * 2040
        t0 = time.time()
        for i in range(n):
            self.daisy.send("D %08X%s\n" % (i, pad))
        self.daisy.send("L pump done\n")
        self.assertTrue(fast.wait_for(lambda c: "L pump done" in c.lines, 30), "fast browser stuck")
        took = time.time() - t0
        got = [l for l in fast.lines if l.startswith("D ") and l.endswith(pad)]
        idx = [int(l[2:10], 16) for l in got]
        self.assertEqual(idx, list(range(n)), "fast browser lost or reordered lines")
        h = self.health()
        self.assertGreaterEqual(h["client_resyncs"], 1, h)
        print("\n  10 MB through, stalled browser present: %.1f s, %d resyncs" % (took, h["client_resyncs"]))
        slow.close()
        fast.close()

    def test_t8_too_many_browsers(self):
        cs = [SseClient(self.http) for _ in range(16)]
        self.wait_health(lambda h: h["clients"] >= 16)
        extra = SseClient(self.http)
        self.assertTrue(extra.wait_for(lambda c: c.status is not None or c.closed))
        self.assertIn("503", extra.status or "")
        for c in cs + [extra]:
            c.close()
        self.wait_health(lambda h: h["clients"] == 0)

    def test_t9_goes_quiet_then_unplug_and_replug(self):
        self.daisy.rx.clear()
        time.sleep(4.0)                            # nothing from the Daisy
        self.assertIn(b"\x1bR", self.daisy.read_rx(0.2), "no new ESC R when quiet")
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: c.events))
        connects = self.health()["connects"]
        self.daisy.unplug()
        self.wait_health(lambda h: not h["daisy"])
        self.assertTrue(c.wait_for(lambda c: not c.events[-1]["daisy"]))
        time.sleep(1.5)
        self.daisy.plug()
        self.wait_health(lambda h: h["daisy"] and h["connects"] == connects + 1, 10)
        self.assertTrue(self.daisy.read_rx(0.5).startswith(b"\x1bR"))
        # Opened mid-stream: the first bytes are the tail of an OLED line
        self.daisy.send("0000ff00c1\nV 7\nL back again\n")
        self.assertTrue(c.wait_for(lambda c: "L back again" in c.lines))
        self.assertFalse(any(l.startswith("0000ff") for l in c.lines), "line fragment forwarded")
        self.assertTrue(c.events[-1]["daisy"])
        self.assertFalse(c.closed, "browser dropped across a Daisy replug")
        c.close()



class FakeController:
    """A MIDI controller: the bridge opens the pty slave through a symlink"""

    def __init__(self, link):
        self.link = link
        self.plug()

    def plug(self):
        self.master, slave = os.openpty()
        tty.setraw(self.master)
        name = os.ttyname(slave)
        os.close(slave)
        tmp = self.link + ".new"
        os.symlink(name, tmp)
        os.replace(tmp, self.link)

    def unplug(self):
        os.unlink(self.link)
        os.close(self.master)

    def play(self, data):
        os.write(self.master, bytes(data))

    def received(self, wait=0.3):
        out = b""
        os.set_blocking(self.master, False)
        end = time.time() + wait
        while time.time() < end:
            try:
                d = os.read(self.master, 65536)
                out += d
                end = max(end, time.time() + 0.1)
            except BlockingIOError:
                time.sleep(0.02)
        os.set_blocking(self.master, True)
        return out


class RespondingDaisy(FakeDaisy):
    """Records the frames the bridge sends and answers pings"""

    def plug(self):
        super().plug()
        self.frames = []
        self.reading = True
        self.alive = True
        threading.Thread(target=self._run, args=(self.master,), daemon=True).start()

    def unplug(self):
        self.alive = False
        super().unplug()

    def _run(self, fd):
        buf = b""
        while self.alive:
            if not self.reading:
                time.sleep(0.05)
                continue
            try:
                d = os.read(fd, 65536)
            except OSError as e:
                if e.errno == errno.EIO and self.alive:
                    time.sleep(0.02)  # no slave open yet (the bridge hasn't opened it)
                    continue
                return
            if not d:
                return
            buf += d
            while True:
                i = buf.find(b"\x1b")
                if i < 0:
                    buf = b""
                    break
                buf = buf[i:]
                if len(buf) < 2:
                    break
                c = buf[1:2]
                if c in (b"R", b"K"):
                    self.frames.append(c.decode())
                    buf = buf[2:]
                    continue
                j = buf.find(b"\n")
                if j < 0:
                    break
                f = buf[1:j].decode()
                buf = buf[j + 1:]
                self.frames.append(f)
                if f.startswith("P "):
                    try:
                        os.write(fd, ("Y %s\n" % f[2:]).encode())
                    except OSError:
                        return

    def got(self, prefix):
        return [f for f in list(self.frames) if f.startswith(prefix)]


class MidiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.link = os.path.join(cls.dir, "daisy")
        cls.daisy = RespondingDaisy(cls.link)
        cls.apc = FakeController(os.path.join(cls.dir, "apc"))
        cls.keys = FakeController(os.path.join(cls.dir, "keys"))
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", cls.link, "--host", "127.0.0.1",
             "--http-port", str(cls.http), "--ui", UI, "--state", os.path.join(cls.dir, "state.json"),
             "--midi", cls.apc.link + "=APC40 mkII-ish APC40", "--midi", cls.keys.link + "=LPK25"],
            stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        BridgeTest.http = cls.http
        cls.health = BridgeTest.health
        cls.wait_health = BridgeTest.wait_health
        cls.get = BridgeTest.get
        cls.wait_health(lambda h: h["daisy"] and len(h["controllers"]) == 2, 10)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(5)

    def setUp(self):
        BridgeTest.http = self.http

    def wait(self, pred, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.02)
        return False

    def port_of(self, name):
        for c in self.health()["controllers"]:
            if c["name"].startswith(name):
                return c["port"]

    def test_m1_nothing_before_the_daisy_says_p(self):
        self.keys.play([0x90, 0x3C, 0x64])
        time.sleep(0.5)
        self.assertEqual(self.daisy.got("M"), [])
        self.assertEqual(self.daisy.got("N"), [])
        self.assertEqual(self.daisy.got("K"), [])
        h = self.health()
        self.assertEqual(h["control"], 0)
        k = [c for c in h["controllers"] if c["name"] == "LPK25"][0]
        self.assertEqual(k["dropped_in"], 1)

    def test_m2_names_keepalive_ping_after_p(self):
        self.daisy.send("V 8\nP 1 00 00 00 -1\n")
        self.assertTrue(self.wait(lambda: len(self.daisy.got("N")) >= 2))
        names = sorted(self.daisy.got("N"))
        a, k = self.port_of("APC40"), self.port_of("LPK25")
        self.assertIn("N%d APC40 mkII-ish APC40" % a, names)
        self.assertIn("N%d LPK25" % k, names)
        self.assertTrue(self.wait(lambda: len(self.daisy.got("K")) >= 2, 2))
        h = self.wait_health(lambda h: h["ping_ms"]["count"] >= 2, 5)
        print("\n  ping over a pty: %s" % h["ping_ms"])
        self.assertEqual(h["control"], 1)

    def test_m3_parser_running_status_clock_sysex(self):
        k = self.port_of("LPK25")
        self.daisy.frames.clear()
        # running status, a clock byte in the middle of a message, note off
        self.keys.play([0x90, 0x3C, 0x64, 0xF8, 0x3E, 0xF8, 0x64, 0x80, 0x3C, 0x00, 0xFE, 0xC0, 0x05])
        self.assertTrue(self.wait(lambda: len(self.daisy.got("M")) >= 4))
        self.assertEqual(self.daisy.got("M"), ["M%d 903c64" % k, "M%d 903e64" % k, "M%d 803c00" % k, "M%d c005" % k])
        # identity reply in three pieces, clock inside
        a = self.port_of("APC40")
        self.daisy.frames.clear()
        reply = bytes.fromhex("f07e000602477300190001000000 7f7f7f7f004b020009000700020308000301 0601f7".replace(" ", ""))
        self.apc.play(reply[:3]); time.sleep(0.05)
        self.apc.play(reply[3:20] + b"\xf8"); time.sleep(0.05)
        self.apc.play(reply[20:])
        self.assertTrue(self.wait(lambda: self.daisy.got("M")))
        self.assertEqual(self.daisy.got("M"), ["M%d %s" % (a, reply.hex())])

    def test_m4_leds_out_in_order_not_to_browsers(self):
        a = self.port_of("APC40")
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: c.events))
        self.apc.received(0.1)
        self.daisy.send("X %d f04700736000044201010" "1f7\nX %d 903501903601b03040\nL leds sent\n" % (a, a))
        got = self.apc.received(0.5)
        self.assertEqual(got.hex(), "f047007360000442010101f7" "903501903601b03040")
        self.assertTrue(c.wait_for(lambda c: "L leds sent" in c.lines))
        self.assertFalse(any(l.startswith("X ") for l in c.lines), "X lines reached a browser")
        c.close()

    def test_m5_leds_coalesce_when_controller_stalls(self):
        a = self.port_of("APC40")
        self.apc.received(0.1)
        # The controller stops reading; the Daisy keeps changing 16 LEDs
        last = {}
        lines = []
        for i in range(3000):
            msgs = ""
            for n in range(16):
                v = (i * 7 + n) % 128
                msgs += "90%02x%02x" % (0x35 + n, v)
                last[0x35 + n] = v
            lines.append("X %d %s" % (a, msgs))
        self.daisy.send("\n".join(lines) + "\n")
        time.sleep(1.0)
        got = self.apc.received(1.0)
        final = {}
        for i in range(0, len(got) - 2, 3):
            if got[i] == 0x90:
                final[got[i + 1]] = got[i + 2]
        self.assertEqual(final, last, "controller didn't end on the newest LED values")
        h = self.health()
        pc = [c for c in h["controllers"] if c["port"] == a][0]
        print("\n  LED flood: %d bytes reached the controller instead of %d, %d coalesced"
              % (len(got), 3000 * 16 * 3, pc["coalesced"]))
        self.assertGreater(pc["coalesced"], 0)
        self.assertLess(len(got), 3000 * 16 * 3)

    def test_m6_controller_unplug_replug(self):
        k = self.port_of("LPK25")
        self.daisy.frames.clear()
        self.keys.unplug()
        self.assertTrue(self.wait(lambda: "N%d -" % k in self.daisy.got("N")))
        self.wait_health(lambda h: len(h["controllers"]) == 1)
        self.keys.plug()
        self.assertTrue(self.wait(lambda: "N%d LPK25" % k in self.daisy.got("N")))
        self.keys.play([0x90, 0x40, 0x7F])
        self.assertTrue(self.wait(lambda: "M%d 90407f" % k in self.daisy.got("M")))

    def test_m7_daisy_reconnect_reannounces(self):
        self.daisy.unplug()
        self.wait_health(lambda h: not h["daisy"])
        self.daisy.plug()
        self.wait_health(lambda h: h["daisy"], 10)
        self.keys.play([0x90, 0x41, 0x7F])  # before P: dropped
        time.sleep(0.3)
        self.assertEqual(self.daisy.got("M"), [])
        self.daisy.send("V 8\nP 1 03 01 02 1\n")
        self.assertTrue(self.wait(lambda: len(self.daisy.got("N")) >= 2))
        h = self.health()
        kinds = {c["name"]: c["kind"] for c in h["controllers"]}
        self.assertEqual(kinds, {"APC40 mkII-ish APC40": "apc40 (driving)", "LPK25": "keys"})

    def test_m7b_daisy_forgets_a_port(self):
        # The Daisy's P line stops listing a port we still have: re-announce it
        self.daisy.frames.clear()
        end = time.time() + 4
        while len(self.daisy.got("N")) < 2 and time.time() < end:
            self.daisy.send("P 1 00 00 00 -1\n")  # the Daisy sends one a second
            time.sleep(0.5)
        self.assertGreaterEqual(len(self.daisy.got("N")), 2)
        n = len(self.daisy.got("N"))
        self.daisy.send("P 1 00 00 00 -1\nP 1 00 00 00 -1\n")
        time.sleep(0.5)
        self.assertEqual(len(self.daisy.got("N")), n, "re-announced more than every 2 s")
        self.daisy.send("P 1 03 01 02 1\n")

    def test_m8_daisy_stops_reading_controller_floods(self):
        self.daisy.reading = False
        k = self.port_of("LPK25")
        flood = bytes([0x90, 0x30, 0x40]) * 30000
        t0 = time.time()
        for i in range(0, len(flood), 3000):
            self.keys.play(flood[i:i + 3000])
        h = self.wait_health(lambda h: h["serial_dropped"] > 0, 10)
        t = time.time()
        self.health()
        self.assertLess(time.time() - t, 1.0, "bridge unresponsive under pressure")
        self.assertLessEqual(h["serial_out_bytes"], 64 * 1024 + 64)
        kc = [c for c in h["controllers"] if c["port"] == k][0]
        print("\n  Daisy not reading: %d MIDI messages dropped, queue %d bytes"
              % (kc["dropped_in"], h["serial_out_bytes"]))
        self.daisy.reading = True
        # It drains and works again
        self.daisy.frames.clear()
        self.assertTrue(self.wait(lambda: self.health()["serial_out_bytes"] == 0, 10))
        self.keys.play([0x90, 0x42, 0x7F])
        self.assertTrue(self.wait(lambda: "M%d 90427f" % k in self.daisy.got("M"), 5))



class FindMidiTest(unittest.TestCase):
    """Controller discovery against a sysfs tree shaped like the T95's"""

    def test_names_and_keys_from_usb(self):
        sys.path.insert(0, HERE)
        import apcbox_bridge as b
        root = tempfile.mkdtemp()
        def dev(card, usbpath, product, mididevs):
            usb = os.path.join(root, "sys/devices/platform/soc/usb" + usbpath)
            intf = usb + "/" + usbpath.split("/")[-1] + ":1.0"
            os.makedirs(intf)
            if product:
                open(usb + "/product", "w").write(product + "\n")
            cd = os.path.join(root, "sys/devices/sound/card%d" % card)
            os.makedirs(cd)
            open(cd + "/id", "w").write("Card%d\n" % card)
            os.symlink(intf, cd + "/device")
            os.makedirs(os.path.join(root, "sys/class/sound"), exist_ok=True)
            os.symlink(cd, os.path.join(root, "sys/class/sound/card%d" % card))
            for m in mididevs:
                os.symlink(cd, os.path.join(root, "sys/class/sound/midiC%dD%d" % (card, m)))
        dev(3, "8/8-1", "LPK25", [0])
        dev(4, "4/4-1/4-1.2", "Akai APC40", [0])
        dev(5, "4/4-1/4-1.3", "", [0, 1])          # no product string: ALSA id
        dev(2, "5/5-1", "ApcBox", [])               # the Daisy: audio only, no MIDI
        f = b.find_midi(root)
        names = sorted(n for _, n in f.values())
        self.assertEqual(names, ["Akai APC40", "Card5", "Card5 1", "LPK25"])
        self.assertTrue(all(k.split(":")[-1] in ("0", "1") for k in f))
        self.assertIn("/dev/snd/midiC3D0", [p for p, _ in f.values()])
        # Same controller, different ALSA card number: same key
        k1 = [k for k, v in f.items() if v[1] == "LPK25"][0]
        root2 = tempfile.mkdtemp()
        root, root_old = root2, root
        dev(7, "8/8-1", "LPK25", [0])
        k2 = [k for k, v in b.find_midi(root2).items() if v[1] == "LPK25"][0]
        self.assertEqual(k1.replace(root_old, ""), k2.replace(root2, ""))



SIM = os.path.join(HERE, "..", "..", "daisy", "hostbuild", "t95sim")


@unittest.skipUnless(os.path.exists(SIM), "build daisy/hostbuild/t95sim first (see daisy/t95sim.cpp)")
class EndToEndTest(unittest.TestCase):
    """The real firmware control code (box.cpp, ports.h, hostcmd.h in
    daisy/t95sim) behind a pty, the real bridge, fake APC40 and LPK25."""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        link = os.path.join(cls.dir, "daisy")
        cls.sim = subprocess.Popen([SIM, link], stderr=subprocess.DEVNULL)
        end = time.time() + 5
        while not os.path.exists(link) and time.time() < end:
            time.sleep(0.05)
        cls.apc = FakeController(os.path.join(cls.dir, "apc"))
        cls.keys = FakeController(os.path.join(cls.dir, "keys"))
        cls.http = free_port()
        cls.ir = os.path.join(cls.dir, "lirc")
        os.mkfifo(cls.ir)
        cls.irw = os.open(cls.ir, os.O_RDWR | os.O_NONBLOCK)
        with open(os.path.join(cls.dir, "state.json"), "w") as f:   # remote: play/stop on two buttons
            json.dump({"ir": {"all": {"play/tap": 0x2FE1C, "stop/tap": 0x2FE1D}}}, f)
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", link, "--host", "127.0.0.1", "--http-port", str(cls.http),
             "--ui", UI, "--state", os.path.join(cls.dir, "state.json"), "--ir-lirc", cls.ir,
             "--midi", cls.apc.link + "=Akai APC40", "--midi", cls.keys.link + "=LPK25"],
            stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["control"] == 1 and len(h["controllers"]) == 2, 10)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.sim.terminate()
        cls.proc.wait(5)
        cls.sim.wait(5)

    def setUp(self):
        BridgeTest.http = self.http

    def test_e1_apc40_identified_driven_leds(self):
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: c.events))
        got = b""
        end = time.time() + 3
        while b"\xf0\x7e\x7f\x06\x01\xf7" not in got and time.time() < end:
            got += self.apc.received(0.1)
        self.assertIn(b"\xf0\x7e\x7f\x06\x01\xf7", got, "no identity request reached the APC40")
        # Answer like the real one did on 2026-11-22
        self.apc.play(bytes.fromhex("f07e000602477300190001000000" "7f7f7f7f004b020009000700020308000301" "0601f7"))
        got = b""
        end = time.time() + 3
        while time.time() < end and not (b"\xf0\x47\x00\x73\x60\x00\x04\x42\x01\x01\x01\xf7" in got and len(got) > 60):
            got += self.apc.received(0.2)
        intro = got.find(bytes.fromhex("f047007360000442010101f7"))
        self.assertGreaterEqual(intro, 0, "no mode switch to device id 0")
        leds = got[intro + 12:]
        self.assertGreater(len(leds), 30, "no LEDs after the mode switch")
        self.assertTrue(all(leds[i] & 0xF0 in (0x80, 0x90, 0xB0) for i in range(0, len(leds) - 2, 3)),
                        "LED bytes aren't whole note/CC messages: %s" % leds[:30].hex())
        h = BridgeTest.wait_health(lambda h: any(x["kind"] == "apc40 (driving)" for x in h["controllers"]), 5)
        # SHIFT on the APC40 shows in the S line (field 9)
        self.apc.play([0x90, 0x62, 0x7F])
        self.assertTrue(c.wait_for(lambda c: any(l.startswith("S ") and l.split()[10] == "1" for l in c.lines[-20:])))
        self.apc.play([0x80, 0x62, 0x00])
        self.assertTrue(c.wait_for(lambda c: c.lines and [l for l in c.lines if l.startswith("S ")][-1].split()[10] == "0"))
        self.assertTrue(c.wait_for(lambda c: any(l.startswith("S ") and l.split()[15] == "1" and l.split()[16] == "1" for l in c.lines[-20:])),
                        "S line doesn't say APC40 up/present")
        c.close()

    def test_e2_keyboard_note_plays_selected_track(self):
        c = SseClient(self.http)
        self.assertTrue(c.wait_for(lambda c: any(l.startswith("S ") for l in c.lines)))
        def trig(c):
            # trigger count of the selected track (field 7); e1's SHIFT tap flipped the page
            f = [l for l in c.lines if l.startswith("S ")][-1].split()[1:]
            t = int(f[7])
            return int(f[26 + t] if t < 8 else f[39 + t - 8])
        before = trig(c)
        self.keys.play([0x90, 0x3C, 0x64])
        k = [x for x in BridgeTest.health()["controllers"] if x["name"] == "LPK25"][0]["port"]
        self.assertTrue(c.wait_for(lambda c: "I %d 903c64" % k in c.lines), "no I line for the key")
        self.assertTrue(c.wait_for(lambda c: trig(c) > before), "the selected track didn't play")
        self.keys.play([0x80, 0x3C, 0x00])
        h = BridgeTest.wait_health(lambda h: h["ping_ms"]["count"] >= 3, 6)
        kinds = sorted(x["kind"] for x in h["controllers"])
        self.assertEqual(kinds, ["apc40 (driving)", "keys"])
        print("\n  end to end ping (pty, desktop): %s" % h["ping_ms"])
        c.close()

    def test_e2b_remote_play_stop_reach_the_daisy(self):
        """Remote play/stop become APC40 PLAY/STOP presses on the driving
        APC40's port; the real firmware code starts and stops the transport"""
        playing = lambda c: [l for l in c.lines if l.startswith("S ")][-1].split()[1]
        press = lambda scan: (os.write(self.irw, struct.pack("QHHIQ", 0, 0, 10, 0, scan)), time.sleep(0.4))
        BridgeTest.wait_health(lambda h: any(x["kind"] == "apc40 (driving)" for x in h["controllers"]), 8)
        sel = SseClient(self.http, path="/events?view=selector")
        self.assertTrue(sel.wait_for(lambda c: any(l.startswith("S ") for l in c.lines)))
        press(0x2FE1C)              # no ApcBox page open: the Daisy isn't touched
        time.sleep(0.5)
        self.assertEqual(playing(sel), "0")
        box = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(box.wait_for(lambda c: any(l.startswith("S ") for l in c.lines)))
        press(0x2FE1C)
        self.assertTrue(box.wait_for(lambda c: playing(c) == "1"), "remote play didn't start the Daisy")
        press(0x2FE1D)
        self.assertTrue(box.wait_for(lambda c: playing(c) == "0"), "remote stop didn't stop it")
        sel.close(); box.close()

    def test_e3_page_runs_in_a_browser(self):
        """The page itself: no script errors, bridge mode, controller chip.
        Needs Playwright with Chromium (desktop only; skipped otherwise)."""
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            pg = b.new_page()
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
            pg.goto("http://127.0.0.1:%d/apcbox" % self.http)
            pg.wait_for_timeout(2500)
            chips = pg.eval_on_selector_all("header .chip:not(.hidden)", "es => es.map(e => e.textContent)")
            hidden_tft = pg.eval_on_selector("#tftpanel", "e => e.classList.contains('hidden')")
            b.close()
        self.assertEqual(errors, [], "script errors on the page")
        self.assertIn("T95 bridge: connected", chips)
        self.assertIn("Daisy: connected", chips)
        self.assertTrue(any(c.startswith("T95 controllers: APC40 1 (driving)") and "keys 2" in c for c in chips), chips)
        self.assertTrue(hidden_tft)



class IrTest(unittest.TestCase):
    """The remote: a FIFO stands in for the input device; frames are real
    struct input_event records, repeats every 110 ms like NEC"""

    EV = struct.Struct("llHHi")

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.fifo = os.path.join(cls.dir, "ir")
        os.mkfifo(cls.fifo)
        cls.w = os.open(cls.fifo, os.O_RDWR | os.O_NONBLOCK)   # a writer is always there
        cls.state = os.path.join(cls.dir, "state.json")
        cls.http = free_port()
        cls.start()

    @classmethod
    def start(cls):
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1",
             "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", cls.state, "--ir", cls.fifo],
            stderr=open(os.path.join(cls.dir, "bridge.log"), "a"))
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["ir"]["device"] == cls.fifo, 10)

    @classmethod
    def stop(cls):
        cls.proc.terminate()
        cls.proc.wait(5)

    @classmethod
    def tearDownClass(cls):
        cls.stop()

    def setUp(self):
        BridgeTest.http = self.http

    def post(self, path, body):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.http, path), data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def state_now(self):
        return json.loads(BridgeTest.get("/state")[2])

    def press(self, scan, secs=0.0):
        """One button: a frame, repeat frames every 110 ms for secs, then let go"""
        end = time.time() + secs
        while True:
            os.write(self.w, self.EV.pack(0, 0, 4, 4, scan) + self.EV.pack(0, 0, 0, 0, 0))
            if time.time() >= end:
                break
            time.sleep(0.11)
        time.sleep(0.35)            # released (no frame for 200 ms)

    def irs(self, c):
        return [d["action"] for e, d in list(c.named) if e == "ir"]

    def test_i1_learn(self):
        c = SseClient(self.http, path="/events?view=selector")
        self.assertTrue(c.wait_for(lambda c: c.events))
        code, _ = self.post("/ir/learn", {"profile": "selector", "action": "up"})
        self.assertEqual(code, 200)
        self.assertEqual(self.state_now()["ir"]["learning"]["action"], "up")
        self.press(0x2FE0A, 0.4)    # held while learning: repeats must not act
        self.assertTrue(c.wait_for(lambda c: any(e == "irlearned" for e, _ in c.named)))
        self.assertEqual(self.state_now()["ir"]["profiles"]["selector"]["up/tap"], 0x2FE0A)
        self.assertEqual(self.irs(c), [], "the learning press also acted")
        self.assertIsNone(self.state_now()["ir"]["learning"])
        c.close()

    def test_i2_views_get_their_own_actions(self):
        self.post("/ir/learn", {"profile": "all", "action": "ok"}); self.press(0x2FE0B)
        self.post("/ir/learn", {"profile": "apcbox", "action": "home"}); self.press(0x2FE0C)
        a = SseClient(self.http, path="/events?view=selector")
        b = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(a.wait_for(lambda c: c.events) and b.wait_for(lambda c: c.events))
        self.press(0x2FE0B)
        self.press(0x2FE0C)
        self.assertTrue(b.wait_for(lambda c: len(self.irs(c)) >= 2))
        time.sleep(0.2)
        self.assertEqual(self.irs(a), ["ok"])              # "all" fills in; home is apcbox-only
        self.assertEqual(self.irs(b), ["ok", "home"])
        a.close(); b.close()

    def test_i3_hold(self):
        self.post("/ir/learn", {"profile": "selector", "action": "home", "press": "hold"}); self.press(0x2FE0B)
        a = SseClient(self.http, path="/events?view=selector")
        b = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(a.wait_for(lambda c: c.events) and b.wait_for(lambda c: c.events))
        self.press(0x2FE0B, 0.9)    # long press
        self.assertTrue(b.wait_for(lambda c: self.irs(c)))
        time.sleep(0.2)
        self.assertEqual(self.irs(a), ["home"], "selector: the hold action, and no tap after it")
        self.assertEqual(self.irs(b), ["ok"], "apcbox has no hold for it: a long press is a tap")
        self.press(0x2FE0B)         # short press: the tap
        self.assertTrue(a.wait_for(lambda c: self.irs(c) == ["home", "ok"]))
        a.close(); b.close()

    def test_i4_arrows_repeat(self):
        a = SseClient(self.http, path="/events?view=selector")
        self.assertTrue(a.wait_for(lambda c: c.events))
        t0 = time.time()
        self.press(0x2FE0A, 1.2)    # "up" from i1, held
        n = self.irs(a).count("up")
        self.assertGreaterEqual(n, 6, "held arrow should repeat (got %d)" % n)
        a.close()

    def test_i5_bad_requests(self):
        self.assertEqual(self.post("/ir/learn", {"profile": "nope", "action": "up"})[0], 400)
        self.assertEqual(self.post("/ir/learn", {"profile": "all", "action": "rm -rf"})[0], 400)
        self.assertEqual(self.post("/select", {"module": "dj"})[0], 400)   # not ready
        self.assertEqual(self.post("/settings", {"autostart_s": 999})[0], 400)
        self.assertEqual(self.post("/nothing", {})[0], 404)
        req = urllib.request.Request("http://127.0.0.1:%d/select" % self.http, data=b"x" * 10000, method="POST")
        with self.assertRaises(Exception):
            urllib.request.urlopen(req, timeout=5)

    def test_i6_forget_and_survive_restart(self):
        self.assertEqual(self.post("/select", {"module": "apcbox"})[0], 200)
        self.assertEqual(self.post("/settings", {"autostart_s": 3})[0], 200)
        self.post("/ir/forget", {"profile": "selector", "action": "home", "press": "hold"})
        before = self.state_now()
        self.assertNotIn("home/hold", before["ir"]["profiles"]["selector"])
        type(self).stop()
        type(self).start()
        after = self.state_now()
        self.assertEqual(after["last"], "apcbox")
        self.assertEqual(after["autostart_s"], 3)
        self.assertEqual(after["ir"]["profiles"], before["ir"]["profiles"])

    def test_i7_pages_in_a_browser(self):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        self.post("/select", {"module": "apcbox"})
        self.post("/settings", {"autostart_s": 1})
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            pg = b.new_page()
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
            # A phone opening the selector: no countdown, modules listed, learn from the table
            pg.goto("http://127.0.0.1:%d/" % self.http)
            pg.wait_for_timeout(1500)
            names = pg.eval_on_selector_all(".module .name", "es => es.map(e => e.textContent)")
            self.assertEqual(names, ["ApcBox groovebox", "Numark DJ", "Network clock"])
            self.assertFalse(pg.is_visible("#countdown"))
            pg.click("#tabs button:text-is('apcbox')")    # the apcbox profile tab
            pg.click("#binds tr:nth-child(3) button:has-text('learn') >> nth=0")  # left, tap
            pg.wait_for_timeout(300)
            self.assertTrue(pg.is_visible("#learn"))
            self.press(0x2FE0D)
            pg.wait_for_timeout(800)
            cell = pg.inner_text("#binds tr:nth-child(3) td:nth-child(2)")
            self.assertEqual(cell, "2fe0d")
            # Remote on a freshly opened selector: the first module has focus, ok opens it
            pg.goto("http://127.0.0.1:%d/" % self.http)
            pg.wait_for_timeout(1000)
            self.press(0x2FE0B)         # "ok" (profile all)
            pg.wait_for_timeout(1500)
            self.assertTrue(pg.url.endswith("/apcbox"), pg.url)
            # The kiosk at boot: counts down, then opens the last module with the TV layout
            pg.goto("http://127.0.0.1:%d/?boot=1&tv=1" % self.http)
            pg.wait_for_timeout(400)
            self.assertTrue(pg.is_visible("#countdown"))
            pg.wait_for_timeout(2500)
            self.assertTrue(pg.url.endswith("/apcbox?tv=1"), pg.url)
            self.assertTrue(pg.eval_on_selector("body", "e => e.classList.contains('tv')"))
            self.assertTrue(pg.is_visible("#modules"))
            # home on the remote (apcbox profile) goes back to the selector
            self.press(0x2FE0C)
            pg.wait_for_timeout(1200)
            self.assertIn("/?tv=1", pg.url)
            # Any button during the countdown keeps you on the selector
            pg.goto("http://127.0.0.1:%d/?boot=1" % self.http)
            pg.wait_for_timeout(300)
            self.press(0x2FE0A)
            pg.wait_for_timeout(1800)
            self.assertFalse(pg.url.endswith("/apcbox"), pg.url)
            b.close()
        self.assertEqual(errors, [], "script errors")



class IrLircTest(IrTest):
    """The same remote tests through the lirc device (repeat-flagged frames),
    plus the noise seen on the T95: imon fakes and stray repeats"""

    LIRC = struct.Struct("QHHIQ")

    @classmethod
    def start(cls):
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1",
             "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", cls.state, "--ir-lirc", cls.fifo],
            stderr=open(os.path.join(cls.dir, "bridge.log"), "a"))
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["ir"]["device"] == cls.fifo and h["ir"]["source"] == "lirc", 10)

    def rec(self, scan, repeat=False, proto=10):
        os.write(self.w, self.LIRC.pack(0, 2 if repeat else 0, proto, 0, scan))

    def press(self, scan, secs=0.0):
        end = time.time() + secs
        self.rec(scan)
        while time.time() < end:
            time.sleep(0.11)
            self.rec(scan, repeat=True)
        time.sleep(0.35)

    def test_i8_learn_through_the_t95_noise(self):
        """The 2026-11-22 trace: imon 0x7fffffff fakes and a lone repeat of the
        previous button arrive between presses. Each learn must get its own
        button, and earlier rows must keep theirs."""
        prof = "selector"
        actions = ["left", "right", "back", "menu"]
        codes = [0x2FE16, 0x2FE50, 0x2FE51, 0x2FE1A]
        prev = None
        for a, code in zip(actions, codes):
            self.assertEqual(self.post("/ir/learn", {"profile": prof, "action": a})[0], 200)
            time.sleep(0.3)
            self.rec(0x7FFFFFFF, proto=23)              # imon noise
            if prev is not None:
                self.rec(prev, repeat=True)             # lone repeat: previous button's code
            time.sleep(0.3)
            self.assertIsNotNone(self.state_now()["ir"]["learning"], "noise was learnt for " + a)
            self.press(code, 0.25)
            prev = code
        binds = self.state_now()["ir"]["profiles"][prof]
        for a, code in zip(actions, codes):
            self.assertEqual(binds.get(a + "/tap"), code, a)
        self.assertGreaterEqual(BridgeTest.health()["ir"]["strays"], 7)

    def test_i9_fresh_frame_is_a_new_press_of_the_same_button(self):
        c = SseClient(self.http, path="/events?view=selector")
        self.assertTrue(c.wait_for(lambda c: c.events))
        # ok (all) pressed twice, quickly: two fresh frames = two taps
        self.rec(0x2FE0B); time.sleep(0.05); self.rec(0x2FE0B, repeat=True); time.sleep(0.08)
        self.rec(0x2FE0B); time.sleep(0.4)
        self.assertTrue(c.wait_for(lambda c: self.irs(c).count("ok") == 2), self.irs(c))
        c.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
