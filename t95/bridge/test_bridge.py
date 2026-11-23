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
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import tty
import shutil
import urllib.parse
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "apcbox_bridge.py")
UI = os.path.join(HERE, "..", "..", "daisy", "ui", "apcbox-ui.html")
REPO = os.path.join(HERE, "..", "..")


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
            [sys.executable, BRIDGE, "--port", cls.link, "--host", "127.0.0.1", "--https-port", "0",
             "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", os.path.join(cls.dir, "state.json")],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
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
        code, ctype, body = self.get("/apcbox")      # the T95's own page
        self.assertEqual(code, 200)
        self.assertIn(b"<title>ApcBox</title>", body)
        code, ctype, body = self.get("/classic")     # the monitor page (also Web Serial, the Mac build)
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
                self.on_frame(fd, f)
                if f.startswith("P "):
                    try:
                        os.write(fd, ("Y %s\n" % f[2:]).encode())
                    except OSError:
                        return

    def got(self, prefix):
        return [f for f in list(self.frames) if f.startswith(prefix)]

    def on_frame(self, fd, f):
        pass


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
            [sys.executable, BRIDGE, "--port", cls.link, "--host", "127.0.0.1", "--https-port", "0",
             "--http-port", str(cls.http), "--ui", UI, "--state", os.path.join(cls.dir, "state.json"),
             "--midi", cls.apc.link + "=APC40 mkII-ish APC40", "--midi", cls.keys.link + "=LPK25"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
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




class RouterTest(unittest.TestCase):
    """Modules and the routing matrix: the hub routes ports to ports, ApcBox routes all to the Daisy"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.link = os.path.join(cls.dir, "daisy")
        cls.daisy = RespondingDaisy(cls.link)
        cls.apc = FakeController(os.path.join(cls.dir, "apc"))
        cls.keys = FakeController(os.path.join(cls.dir, "keys"))
        cls.synth = FakeController(os.path.join(cls.dir, "synth"))
        cls.http = free_port()
        cls.state = os.path.join(cls.dir, "state.json")
        cls.start()
        cls.speak()

    @classmethod
    def speak(cls):
        """The Daisy sends a P line every second; keep sending until the bridge has heard one"""
        end = time.time() + 10
        while time.time() < end:
            cls.daisy.send("V 8\nP 3 00 00 00 -1\n")
            if cls.health()["control"] == 3:
                return
            time.sleep(0.3)
        raise AssertionError("bridge never took the P line")

    @classmethod
    def start(cls):
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", cls.link, "--host", "127.0.0.1", "--https-port", "0",
             "--http-port", str(cls.http), "--ui", UI, "--state", cls.state,
             "--midi", cls.apc.link + "=APC40", "--midi", cls.keys.link + "=LPK25",
             "--midi", cls.synth.link + "=Synth"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "a"))
        BridgeTest.http = cls.http
        cls.wait_health(lambda h: h["daisy"] and len(h["controllers"]) == 3, 10)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(5)

    wait_health = classmethod(lambda cls, *a: BridgeTest.wait_health.__func__(BridgeTest, *a))
    health = classmethod(lambda cls: BridgeTest.health.__func__(BridgeTest))
    def post(self, path, body):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.http, path), data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    wait = MidiTest.wait
    port_of = MidiTest.port_of

    def setUp(self):
        BridgeTest.http = self.http

    def routes(self):
        return json.loads(BridgeTest.get("/routes")[2])

    def drain(self):
        for c in (self.apc, self.keys, self.synth):
            c.received(0.05)

    def test_r1_apcbox_routes_everything_to_the_daisy(self):
        self.assertEqual(self.post("/select", {"module": "apcbox"})[0], 200)
        r = self.routes()
        self.assertEqual(r["module"], "apcbox")
        self.assertEqual(r["defaults"], [{"from": "*", "to": "daisy"}])
        self.assertIn("daisy", r["destinations"])
        k = self.port_of("LPK25")
        self.assertTrue(self.wait(lambda: "N%d LPK25" % k in self.daisy.got("N")))
        self.drain()
        self.keys.play([0x90, 0x3C, 0x64])
        self.assertTrue(self.wait(lambda: "M%d 903c64" % k in self.daisy.got("M")))
        self.assertEqual(self.synth.received(0.2), b"", "apcbox routed to another port")

    def test_r2_hub_unannounces_and_routes_port_to_port(self):
        self.daisy.frames.clear()
        self.assertEqual(self.post("/select", {"module": "midihub"})[0], 200)
        nums = [self.port_of(n) for n in ("APC40", "LPK25", "Synth")]
        self.assertTrue(self.wait(lambda: all("N%d -" % n in self.daisy.got("N") for n in nums)))
        self.assertEqual(self.routes()["routes"], [])
        self.drain()
        self.keys.play([0x90, 0x3C, 0x64])          # nothing routed: goes nowhere
        time.sleep(0.3)
        self.assertEqual(self.synth.received(0.1), b"")
        self.assertEqual(self.daisy.got("M"), [])
        code, _ = self.post("/routes", {"routes": [
            {"from": "LPK25", "to": "Synth", "ch": 1, "remap": 10},
            {"from": "APC40", "to": "Synth", "types": ["cc"]},
            {"from": "LPK25", "to": "APC40", "types": ["clock"]}]})
        self.assertEqual(code, 200)
        # channel filter and remap, running status kept whole per message
        self.keys.play([0x90, 0x3C, 0x64, 0x3E, 0x50, 0x91, 0x40, 0x40])
        self.assertEqual(self.synth.received(0.4).hex(), "993c64993e50")
        # type filter: only CCs from the APC
        self.apc.play([0x90, 0x35, 0x7F, 0xB0, 0x07, 0x40])
        self.assertEqual(self.synth.received(0.4).hex(), "b00740")
        # clock and transport go port to port, never to the Daisy
        self.keys.play([0xFA, 0xF8, 0xF8, 0xFC])
        self.assertEqual(self.apc.received(0.4).hex(), "faf8f8fc")
        self.assertEqual(self.daisy.got("M"), [])
        # sysex arrives whole even when it comes in pieces
        self.post("/routes", {"routes": [{"from": "*", "to": "Synth"}]})
        self.apc.play([0xF0, 0x7E, 0x00]); time.sleep(0.05)
        self.apc.play([0x06, 0x02, 0xF8, 0xF7])
        self.assertEqual(self.synth.received(0.4).hex(), "f8f07e000602f7")

    def test_r3_bad_routes_refused(self):
        for bad in ([{"from": "a"}], [{"from": "a", "to": "*"}], [{"from": "a", "to": "b", "ch": 17}],
                    [{"from": "a", "to": "b", "types": ["noise"]}], "x", [{"from": "a", "to": "b"}] * 65):
            code, body = self.post("/routes", {"routes": bad})
            self.assertEqual(code, 400, bad)

    def test_r4_presets_and_restart(self):
        self.post("/select", {"module": "midihub"})
        mine = [{"from": "LPK25", "to": "Synth"}]
        self.post("/routes", {"routes": mine})
        self.assertEqual(self.post("/presets/save", {"name": "keys to synth"})[0], 200)
        self.post("/routes", {"routes": []})
        self.assertEqual(self.post("/presets/load", {"name": "keys to synth"})[0], 200)
        self.assertEqual(self.routes()["routes"], mine)
        self.assertEqual(self.routes()["presets"], ["keys to synth"])
        self.assertEqual(self.post("/presets/load", {"name": "nope"})[0], 400)
        type(self).tearDownClass()
        type(self).start()
        type(self).speak()
        r = self.routes()
        self.assertEqual((r["module"], r["routes"], r["presets"]), ("midihub", mine, ["keys to synth"]))
        self.drain()
        self.keys.play([0x90, 0x30, 0x30])
        self.assertEqual(self.synth.received(0.5).hex(), "903030")

    def test_r6_hub_page_in_a_browser(self):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        self.post("/select", {"module": "midihub"})
        self.post("/routes", {"routes": []})
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            pg = b.new_page(viewport={"width": 390, "height": 800})   # a phone
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
            pg.goto("http://127.0.0.1:%d/midihub" % self.http)
            pg.wait_for_selector(".cell")
            rows = pg.eval_on_selector_all(".matrix tbody th", "es => es.map(e => e.textContent)")
            self.assertEqual(rows, ["any device", "APC40", "LPK25", "Synth"])
            cols = pg.eval_on_selector_all(".matrix thead th span", "es => es.map(e => e.textContent)")
            self.assertEqual(cols, ["APC40", "LPK25", "Synth", "Daisy"])
            self.assertTrue(pg.is_disabled('.cell[data-from="LPK25"][data-to="LPK25"]'))
            # tap: connected; the editor opens; channel 1 to 10, notes only
            pg.click('.cell[data-from="LPK25"][data-to="Synth"]')
            pg.wait_for_selector("#edit", state="visible")
            pg.select_option("#e-ch", "1")
            pg.select_option("#e-remap", "10")
            for t in ("cc", "pc", "at", "pb", "sysex", "clock"):
                pg.uncheck('#e-types input[value="%s"]' % t)
            pg.click("#e-save")
            pg.wait_for_timeout(400)
            self.assertEqual(self.routes()["routes"],
                             [{"from": "LPK25", "to": "Synth", "ch": 1, "remap": 10, "types": ["note"]}])
            self.assertEqual(pg.inner_text('.cell[data-from="LPK25"][data-to="Synth"]'), "1>10 note")
            self.drain()
            self.keys.play([0x90, 0x3C, 0x64, 0xB0, 0x01, 0x02])
            self.assertEqual(self.synth.received(0.4).hex(), "993c64")
            # the remote: arrows walk the grid, ok toggles
            pg.click("#e-close")
            pg.focus('.cell[data-from="APC40"][data-to="LPK25"]')
            pg.keyboard.press("ArrowRight")
            self.assertEqual(pg.evaluate("document.activeElement.dataset.to"), "Synth")
            pg.keyboard.press("ArrowDown")
            self.assertEqual(pg.evaluate("[document.activeElement.dataset.from, document.activeElement.dataset.to]"),
                             ["LPK25", "Synth"])
            # a preset, then tap twice to remove the route
            pg.fill("#p-name", "jam")
            pg.click("#p-save")
            pg.wait_for_selector("#presets td:text-is('jam')")
            pg.click('.cell[data-from="LPK25"][data-to="Synth"]')
            pg.click('.cell[data-from="LPK25"][data-to="Synth"]')
            pg.wait_for_timeout(400)
            self.assertEqual(self.routes()["routes"], [])
            pg.click("#presets button:text-is('load')")
            pg.wait_for_timeout(400)
            self.assertEqual(len(self.routes()["routes"]), 1)
            pg.screenshot(path=os.path.join(self.dir, "midihub.png"), full_page=True)
            b.close()
        self.assertEqual(errors, [])
        self.post("/presets/delete", {"name": "jam"})

    def test_r5_back_to_apcbox_reannounces(self):
        self.post("/select", {"module": "midihub"})
        self.daisy.frames.clear()
        self.post("/select", {"module": "apcbox"})
        k = self.port_of("LPK25")
        self.assertTrue(self.wait(lambda: "N%d LPK25" % k in self.daisy.got("N")))
        self.keys.play([0x90, 0x44, 0x7F])
        self.assertTrue(self.wait(lambda: "M%d 90447f" % k in self.daisy.got("M")))


class FakePeer:
    """A network MIDI device (a Mac, an iPad app): control and data UDP sockets.
    Written from the AppleMIDI / RFC 6295 packet layouts, separately from netmidi.py."""

    def __init__(self, name, invite_back=True):
        self.name = name
        self.ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ctl.bind(("127.0.0.1", 0))
        self.port = self.ctl.getsockname()[1]
        self.data = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.data.bind(("127.0.0.1", self.port + 1))
        except OSError:
            self.ctl.close()
            raise
        self.ssrc = 0x1234ABCD
        self.accept = True
        self.events = []           # ("IN"/"OK"/"BY"/"CK"/"NO", ...) and ("midi", bytes)
        self.midi = bytearray()
        self.alive = True
        self.seq = 100
        for sock in (self.ctl, self.data):
            threading.Thread(target=self._run, args=(sock,), daemon=True).start()

    @staticmethod
    def make(name):
        for _ in range(20):
            try:
                return FakePeer(name)
            except OSError:
                continue
        raise OSError("no free port pair")

    def am(self, cmd, token):
        return b"\xff\xff" + cmd + struct.pack(">III", 2, token, self.ssrc) + self.name.encode() + b"\0"

    def _run(self, sock):
        while self.alive:
            try:
                pkt, addr = sock.recvfrom(65536)
            except OSError:
                return
            if pkt[:2] == b"\xff\xff":
                cmd = pkt[2:4]
                if cmd == b"IN":
                    token = struct.unpack(">I", pkt[8:12])[0]
                    self.events.append(("IN", sock is self.data))
                    sock.sendto(self.am(b"OK" if self.accept else b"NO", token), addr)
                elif cmd == b"CK":
                    count = pkt[8]
                    ts = list(struct.unpack(">QQQ", pkt[12:36]))
                    self.events.append(("CK", count))
                    if count == 0:
                        ts[1] = 5
                        sock.sendto(b"\xff\xffCK" + struct.pack(">IB3xQQQ", self.ssrc, 1, *ts), addr)
                else:
                    self.events.append((cmd.decode(), sock is self.data))
            elif sock is self.data and pkt[1] & 0x7F == 0x61:
                h = pkt[12]
                n, i = (((h & 15) << 8) | pkt[13], 14) if h & 0x80 else (h & 15, 13)
                body = pkt[i:i + n]
                # strip the delta times: this peer only ever gets delta 0 between commands
                j, out, first = 0, bytearray(), True
                while j < len(body):
                    if not first:
                        j += 1         # delta 0
                    first = False
                    b = body[j]
                    if b == 0xF0:
                        e = body.index(0xF7, j) + 1
                    elif b >= 0xF8:
                        e = j + 1
                    else:
                        e = j + (2 if b & 0xF0 in (0xC0, 0xD0) else 3)
                    out += body[j:e]
                    j = e
                self.midi += out

    def invite(self, bridge_port):
        """We start the session (like clicking Connect on a Mac)"""
        token = 77
        self.ctl.sendto(self.am(b"IN", token), ("127.0.0.1", bridge_port))
        time.sleep(0.1)
        self.data.sendto(self.am(b"IN", token), ("127.0.0.1", bridge_port + 1))
        time.sleep(0.1)
        self.data.sendto(b"\xff\xffCK" + struct.pack(">IB3xQQQ", self.ssrc, 0, 1, 0, 0), ("127.0.0.1", bridge_port + 1))

    def send(self, bridge_port, cmds, z=False):
        """One RTP-MIDI packet; cmds is the raw command list (delta times included by the caller)"""
        self.seq += 1
        n = len(cmds)
        hdr = bytes([0x80 | 0x20 * z | (n >> 8), n & 0xFF]) if n > 15 else bytes([0x20 * z | n])
        pkt = struct.pack(">BBHII", 0x80, 0x61, self.seq, 0, self.ssrc) + hdr + bytes(cmds)
        self.data.sendto(pkt, ("127.0.0.1", bridge_port + 1))

    def bye(self, bridge_port):
        self.ctl.sendto(self.am(b"BY", 0), ("127.0.0.1", bridge_port))

    def close(self):
        self.alive = False
        self.ctl.close()
        self.data.close()


class NetMidiTest(unittest.TestCase):
    """RTP-MIDI sessions: invited by the bridge and inviting it, both directions, through the router"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.keys = FakeController(os.path.join(cls.dir, "keys"))
        cls.mac = FakePeer.make("Rene's Mac")        # the bridge finds this one (as if by Bonjour)
        cls.ipad = FakePeer.make("iPad")             # this one invites the bridge
        cls.http = free_port()
        while True:
            cls.rtp = free_port()
            try:
                t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                t.bind(("127.0.0.1", cls.rtp + 1))
                t.close()
                break
            except OSError:
                pass
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1",
             "--https-port", "0", "--http-port", str(cls.http), "--ui", UI,
             "--state", os.path.join(cls.dir, "state.json"), "--midi", cls.keys.link + "=LPK25",
             "--rtp-port", str(cls.rtp), "--no-browse", "--net-peer", "127.0.0.1:%d=Rene's Mac" % cls.mac.port],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: "Rene's Mac" in h["net"].get("sessions", []), 10)

    @classmethod
    def tearDownClass(cls):
        if cls.proc.poll() is None:
            cls.proc.terminate()
            cls.proc.wait(5)
        cls.mac.close()
        cls.ipad.close()

    post = RouterTest.post
    wait = MidiTest.wait

    def setUp(self):
        BridgeTest.http = self.http

    def health(self):
        return BridgeTest.health()

    def test_n1_bridge_invites_and_keeps_the_clock(self):
        self.assertIn(("IN", False), self.mac.events)   # control port first
        self.assertIn(("IN", True), self.mac.events)    # then data
        self.assertTrue(self.wait(lambda: ("CK", 2) in self.mac.events, 5), self.mac.events)
        h = self.health()
        s = [c for c in h["controllers"] if c["name"] == "Rene's Mac"][0]
        self.assertTrue(s["net"]["initiator"])
        self.assertIsNotNone(s["net"]["latency_ms"])

    def test_n2_peer_invites_the_bridge(self):
        self.ipad.invite(self.rtp)
        BridgeTest.wait_health(lambda h: "iPad" in h["net"]["sessions"], 5)
        self.assertEqual([e for e in self.ipad.events if e[0] == "IN"], [])

    def test_n3_midi_both_ways(self):
        self.post("/select", {"module": "midihub"})
        self.assertEqual(self.post("/routes", {"routes": [
            {"from": "LPK25", "to": "Rene's Mac"}, {"from": "iPad", "to": "LPK25"},
            {"from": "iPad", "to": "Rene's Mac", "types": ["sysex", "clock"]}]})[0], 200)
        self.keys.received(0.05)
        self.mac.midi.clear()
        # keys to the Mac: running status in, full status bytes out
        self.keys.play([0x90, 0x3C, 0x64, 0x3E, 0x50])
        self.assertTrue(self.wait(lambda: len(self.mac.midi) >= 6))
        self.assertEqual(bytes(self.mac.midi).hex(), "903c64903e50")
        # iPad to keys: delta times, running status across commands, a realtime byte between
        self.ipad.send(self.rtp, [0x91, 0x40, 0x7F, 0x00, 0x42, 0x7F, 0x81, 0x10, 0xF8, 0x05, 0xB1, 0x07, 0x20])
        self.assertEqual(self.keys.received(0.4).hex(), "91407f91427ff8b10720")
        # Z flag: a delta time before the first command too
        self.ipad.send(self.rtp, [0x00, 0xC1, 0x05], z=True)
        self.assertEqual(self.keys.received(0.4).hex(), "c105")
        # sysex split over two packets (first, last segments) reaches the Mac whole
        self.mac.midi.clear()
        self.ipad.send(self.rtp, [0xF0, 0x7E, 0x00, 0x06, 0xF0])
        self.ipad.send(self.rtp, [0xF7, 0x02, 0x01, 0xF7])
        self.assertTrue(self.wait(lambda: len(self.mac.midi) >= 7))
        self.assertEqual(bytes(self.mac.midi).hex(), "f07e00060201f7")

    def test_n4_drop_releases_held_notes(self):
        self.keys.received(0.2)                 # the sysex from n3 went to the keys too (no type filter)
        self.ipad.send(self.rtp, [0x92, 0x30, 0x60, 0x00, 0x31, 0x60])
        self.assertEqual(self.keys.received(0.4).hex(), "923060923160")
        self.ipad.bye(self.rtp)
        BridgeTest.wait_health(lambda h: "iPad" not in h["net"]["sessions"], 5)
        # every note still down from this session: n3's two (channel 2) and these two
        self.assertEqual(self.keys.received(0.4).hex(), "814000814200823000823100")

    def test_n5_never(self):
        self.assertEqual(self.post("/net/never", {"name": "iPad", "never": True})[0], 200)
        self.ipad.events.clear()
        self.ipad.invite(self.rtp)
        self.assertTrue(self.wait(lambda: self.health()["net"]["refused"] >= 1))
        self.assertNotIn("iPad", self.health()["net"]["sessions"])
        self.assertEqual(self.health()["net"]["never"], ["iPad"])
        self.post("/net/never", {"name": "iPad", "never": False})
        self.ipad.invite(self.rtp)
        BridgeTest.wait_health(lambda h: "iPad" in h["net"]["sessions"], 5)

    def test_n6_bridge_stopping_says_goodbye(self):
        self.mac.events.clear()
        self.proc.terminate()
        self.proc.wait(5)
        self.assertTrue(self.wait(lambda: ("BY", False) in self.mac.events, 2), self.mac.events)


class TimedReader:
    """Reads a fake controller's output in a thread, timestamping every byte"""

    def __init__(self, ctl):
        self.ctl = ctl
        self.got = []           # (monotonic, byte)
        self.alive = True
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        import select
        while self.alive:
            r, _, _ = select.select([self.ctl.master], [], [], 0.1)
            if r:
                try:
                    d = os.read(self.ctl.master, 4096)
                except OSError:
                    return
                t = time.monotonic()
                self.got.extend((t, b) for b in d)

    def clear(self):
        self.got = []

    def ticks(self):
        return [t for t, b in list(self.got) if b == 0xF8]

    def bytes_(self):
        return bytes(b for _, b in list(self.got))

    def stop(self):
        self.alive = False


def intervals_ms(ts):
    iv = [(b - a) * 1000 for a, b in zip(ts, ts[1:])]
    if not iv:
        return 0, 0, 0
    m = sum(iv) / len(iv)
    sd = (sum((x - m) ** 2 for x in iv) / len(iv)) ** 0.5
    return m, sd, max(abs(x - m) for x in iv)


class ClockTest(unittest.TestCase):
    """The clock module: internal tempo, following MIDI clock (Cubase), song position, Link"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.cubase = FakeController(os.path.join(cls.dir, "cubase"))
        cls.synth = FakeController(os.path.join(cls.dir, "synth"))
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1",
             "--https-port", "0", "--http-port", str(cls.http), "--ui", UI, "--rtp-port", "0",
             "--state", os.path.join(cls.dir, "state.json"),
             "--midi", cls.cubase.link + "=Cubase", "--midi", cls.synth.link + "=Synth"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: len(h["controllers"]) == 2, 10)
        cls.rx = TimedReader(cls.synth)

    @classmethod
    def tearDownClass(cls):
        cls.rx.stop()
        cls.proc.terminate()
        cls.proc.wait(5)

    post = RouterTest.post
    wait = MidiTest.wait

    def setUp(self):
        BridgeTest.http = self.http

    def clock(self):
        return json.loads(BridgeTest.get("/clock?json=1")[2])

    def play_clock(self, bpm, secs, jitter_ms=0.0, start=True, stop_event=None):
        """Cubase: ticks on a perfect grid, each sent a random amount late (Wi-Fi)"""
        import random
        rnd = random.Random(7)
        period = 60.0 / (bpm * 24)
        if start:
            self.cubase.play([0xFA])
        t0 = time.monotonic() + 0.01
        n = 0
        while time.monotonic() < t0 + secs and not (stop_event and stop_event.is_set()):
            due = t0 + n * period + abs(rnd.gauss(0, jitter_ms / 1000.0))
            d = due - time.monotonic()
            if d > 0:
                time.sleep(d)
            self.cubase.play([0xF8])
            n += 1
        return n

    def test_c1_internal_tempo(self):
        self.assertEqual(self.post("/clock", {"on": True, "source": "internal", "bpm": 120, "outputs": ["Synth"],
                                              "send_stopped": False})[0], 200)
        time.sleep(0.3)
        self.rx.clear()
        self.assertEqual(self.post("/clock/transport", {"cmd": "start"})[0], 200)
        time.sleep(2.0)
        self.post("/clock/transport", {"cmd": "stop"})
        time.sleep(0.2)
        got = self.rx.bytes_()
        self.assertEqual(got[0], 0xFA, got[:4].hex())
        self.assertEqual(got[-1], 0xFC)
        m, sd, worst = intervals_ms(self.rx.ticks())
        c = self.clock()
        print("\n  internal 120 BPM: %d ticks, interval %.3f ms (ideal 20.833), sd %.3f, worst %.3f; late %s"
              % (len(self.rx.ticks()), m, sd, worst, c["late_ms"]))
        self.assertAlmostEqual(m, 20.833, delta=0.25)
        self.assertTrue(90 <= len(self.rx.ticks()) <= 100)
        self.assertFalse(c["playing"])
        # stopped, send_stopped off: no ticks
        self.rx.clear()
        time.sleep(0.3)
        self.assertEqual(self.rx.ticks(), [])
        self.assertEqual(self.post("/clock", {"bpm": 900})[0], 400)

    def test_c2_follows_cubase_and_smooths(self):
        self.post("/clock", {"on": True, "source": "Cubase", "outputs": ["Synth"], "send_stopped": True})
        self.rx.clear()
        n = self.play_clock(125, 4.0, jitter_ms=2.0)
        c = self.clock()
        out = self.rx.ticks()
        # drop the first beat out: the fit is settling
        m, sd, worst = intervals_ms(out[24:])
        print("\n  following 125 BPM sent with 2 ms jitter: locked at %.2f, input jitter %s ms; out interval %.3f ms "
              "(ideal 20.000), sd %.3f, worst %.3f; late %s" % (c["tempo"], c["in_jitter_ms"], m, sd, worst, c["late_ms"]))
        self.assertTrue(c["locked"])
        self.assertAlmostEqual(c["tempo"], 125.0, delta=0.4)
        self.assertEqual(self.rx.bytes_()[0], 0xFA)
        self.assertAlmostEqual(m, 20.0, delta=0.2)
        self.assertLess(sd, 1.0)
        self.assertLess(abs(len(out) - n), 10)
        self.assertEqual(self.post("/clock/transport", {"cmd": "start"})[0], 400)   # transport is Cubase's

    def test_c3_song_position_and_continue(self):
        self.post("/clock", {"on": True, "source": "Cubase", "outputs": ["Synth"]})
        self.play_clock(125, 0.6, start=False)
        self.cubase.play([0xFC])
        time.sleep(0.1)
        self.rx.clear()
        self.cubase.play([0xF2, 0x10, 0x00, 0xFB])   # cursor to bar 2, then continue
        stop = threading.Event()
        th = threading.Thread(target=self.play_clock, args=(125, 1.0), kwargs={"start": False, "stop_event": stop})
        th.start()
        time.sleep(0.3)
        c = self.clock()
        th.join()
        got = self.rx.bytes_().replace(b"\xf8", b"")
        self.assertTrue(got.startswith(bytes.fromhex("f21000fb")), got.hex())
        self.assertTrue(c["position"].startswith("2.1."), c["position"])
        self.assertTrue(c["playing"])

    def test_c4_source_gone_holds_tempo(self):
        self.post("/clock", {"on": True, "source": "Cubase", "outputs": ["Synth"], "send_stopped": True})
        self.play_clock(110, 1.5)
        self.assertTrue(self.clock()["locked"])
        self.rx.clear()
        time.sleep(1.5)
        c = self.clock()
        self.assertFalse(c["locked"])
        m, sd, _ = intervals_ms(self.rx.ticks()[30:])
        self.assertAlmostEqual(m, 60000 / (110 * 24), delta=0.3)   # still ticking at the last tempo

    def test_c5_link(self):
        try:
            import aalink
        except ImportError:
            self.skipTest("aalink not installed")
        import asyncio as aio

        async def peer():
            lk = aalink.Link(97.0)
            lk.enabled = True
            await aio.sleep(1.5)       # a session that has been running a while, like Live left open
            # the bridge joins the session: it takes the peer's tempo
            self.post("/clock", {"on": True, "source": "link", "outputs": ["Synth"]})
            for _ in range(60):
                await aio.sleep(0.1)
                c = self.clock()
                if c["link_peers"] and abs(c["tempo"] - 97.0) < 0.05:
                    break
            self.assertGreaterEqual(c["link_peers"], 1)
            self.assertAlmostEqual(c["tempo"], 97.0, delta=0.05)
            self.rx.clear()
            await aio.sleep(1.5)
            m, sd, _ = intervals_ms(self.rx.ticks()[6:])
            print("\n  Link 97 BPM: out interval %.3f ms (ideal %.3f), sd %.3f" % (m, 60000 / (97 * 24), sd))
            self.assertAlmostEqual(m, 60000 / (97 * 24), delta=0.3)
            # the bridge leads: internal tempo shared on Link
            self.post("/clock", {"source": "internal", "bpm": 133, "link": True})
            for _ in range(60):
                await aio.sleep(0.1)
                if abs(lk.tempo - 133) < 0.05:
                    break
            self.assertAlmostEqual(lk.tempo, 133, delta=0.05)
            lk.enabled = False
        aio.run(peer())
        self.post("/clock", {"link": False})

    def test_c6_page_in_a_browser(self):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        self.post("/clock", {"on": True, "source": "internal", "bpm": 120, "outputs": []})
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            pg = b.new_page()
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto("http://127.0.0.1:%d/clock" % self.http)
            pg.wait_for_timeout(800)
            self.assertIn("120.0", pg.inner_text("#bpm"))
            pg.click("#p1")
            pg.wait_for_timeout(500)
            self.assertEqual(self.clock()["bpm"], 121)
            pg.click("#outputs label:has-text('Synth') input")
            pg.wait_for_timeout(500)
            self.assertEqual(self.clock()["outputs"], ["Synth"])
            pg.click("#play")
            self.assertTrue(self.wait(lambda: self.clock()["playing"], 3), self.clock())
            pg.wait_for_timeout(700)
            self.assertEqual(pg.inner_text("#play"), "Stop")
            pg.click("#sources button:text-is('Cubase')")
            pg.wait_for_timeout(600)
            self.assertEqual(self.clock()["source"], "Cubase")
            self.assertTrue(pg.is_disabled("#play"))
            pg.screenshot(path=os.path.join(self.dir, "clock.png"), full_page=True)
            b.close()
        self.assertEqual(errors, [])

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
        cls.sim = subprocess.Popen([SIM, link], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.sim.kill)       # also if setUpClass fails below
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
            json.dump({"ir": {"all": {"play/tap": 0x2FE1C, "stop/tap": 0x2FE1D, "3/tap": 0x2FE03}}}, f)
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", link, "--host", "127.0.0.1", "--https-port", "0", "--http-port", str(cls.http),
             "--ui", UI, "--state", os.path.join(cls.dir, "state.json"), "--ir-lirc", cls.ir,
             "--midi", cls.apc.link + "=Akai APC40", "--midi", cls.keys.link + "=LPK25"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.addClassCleanup(cls.proc.kill)
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["control"] >= 1 and len(h["controllers"]) == 2, 10)

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

    def test_e2c_ui_command_track_and_encoder(self):
        """v13.7 control protocol 2: remote digits pick tracks, /ui drives the
        encoder (the page's buttons)"""
        h = BridgeTest.health()
        self.assertGreaterEqual(h["control"], 2)
        caps = json.loads(BridgeTest.get("/caps")[2])
        self.assertTrue(caps["ui"])
        sel = lambda c: int([l for l in c.lines if l.startswith("S ")][-1].split()[8])
        box = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(box.wait_for(lambda c: any(l.startswith("S ") for l in c.lines)))
        os.write(self.irw, struct.pack("QHHIQ", 0, 0, 10, 0, 0x2FE03))   # "3"
        time.sleep(0.4)
        self.assertTrue(box.wait_for(lambda c: sel(c) == 2), "remote 3 didn't select track 3")
        for a in ("turn&d=1", "push", "back", "turn&d=-1"):
            self.assertEqual(BridgeTest.get("/ui?a=" + a)[0], 200, a)
        with self.assertRaises(urllib.error.HTTPError) as e:
            BridgeTest.get("/ui?a=explode")
        self.assertEqual(e.exception.code, 404)
        box.close()

    def test_e2d_sample_to_the_real_firmware(self):
        """A WAV from the library into track 3 through the firmware's xfer.h
        and loader (t95sim)"""
        lib = os.path.join(os.path.dirname(os.path.join(self.dir, "state.json")), "samples", "t")
        os.makedirs(lib, exist_ok=True)
        make_wav(os.path.join(lib, "e2e tone.wav"), 2.0)
        c = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(c.wait_for(lambda c: c.events))
        code, out = SampleTest.post(self, "/samples/send", {"src": "lib", "path": "t/e2e tone.wav", "track": 3})
        self.assertEqual(code, 200, out)
        t = SampleTest.wait_xfer(self, out["id"])
        self.assertEqual(t["state"], "done", t)
        self.assertTrue(c.wait_for(lambda c: "N 2 e2e_tone.wav" in c.lines), [l for l in c.lines if l.startswith("N 2")])
        print("\n  2 s sample to the firmware over a pty: %d KB in %.2f s (%s KB/s)" % (
            t["bytes"] // 1024, t["seconds"], t["kb_per_s"]))
        c.close()

    def test_e2e_record_in_the_page(self):
        """The page's record button with Chromium's fake microphone: record,
        upload, convert (ffmpeg), send to the selected track, firmware loads it"""
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        if not shutil.which("ffmpeg"):
            self.skipTest("ffmpeg needed to convert the recording")
        c = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(c.wait_for(lambda c: any(l.startswith("S ") for l in c.lines)))
        with sync_playwright() as pw:
            b = pw.chromium.launch(args=["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream"])
            pg = b.new_page()
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto("http://127.0.0.1:%d/classic" % self.http)   # localhost counts as secure
            pg.wait_for_timeout(1500)
            self.assertFalse(pg.is_visible("#smp-rec-hint"), "localhost can record: no hint")
            pg.click("#smp-rec")
            pg.wait_for_timeout(2200)
            self.assertRegex(pg.inner_text("#smp-rec-time").lower(), r"^[12]\.\d s$")
            pg.click("#smp-rec")                                  # stop: upload and send
            ok = False
            for _ in range(100):
                pg.wait_for_timeout(200)
                if "done in" in pg.inner_text("#smp-xfer"):
                    ok = True
                    break
            xfer = pg.inner_text("#smp-xfer")
            b.close()
        self.assertEqual(errors, [], "script errors")
        self.assertTrue(ok, "recording didn't reach the Daisy: " + xfer)
        sel = int([l for l in c.lines if l.startswith("S ")][-1].split()[8])
        self.assertTrue(c.wait_for(lambda c: any(l.startswith("N %d rec_" % sel) for l in c.lines)),
                        [l for l in c.lines if l.startswith("N %d" % sel)][-2:])
        print("\n  page recording -> track %d: %s" % (sel + 1, xfer.splitlines()[0]))
        c.close()

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
            pg.goto("http://127.0.0.1:%d/classic" % self.http)
            pg.wait_for_timeout(2500)
            chips = pg.eval_on_selector_all("header .chip:not(.hidden)", "es => es.map(e => e.textContent)")
            panel_hidden = pg.eval_on_selector("#tftpanel", "e => e.classList.contains('hidden')")
            hidden_tft = pg.eval_on_selector("#tft", "e => e.classList.contains('hidden')")
            samples_shown = not pg.eval_on_selector("#samples", "e => e.classList.contains('hidden')")
            smp_rows = pg.eval_on_selector_all("#smp-list .smp-row", "es => es.length")
            # the Daisy's encoder from the page (v13.7: UI command)
            with pg.expect_response(lambda r: "/ui?a=push" in r.url) as resp:
                pg.click("button[data-ui='push']")
            ui_status = resp.value.status
            b.close()
        self.assertEqual(errors, [], "script errors on the page")
        self.assertIn("T95 bridge: connected", chips)
        self.assertIn("Daisy: connected", chips)
        self.assertTrue(any(c.startswith("T95 controllers: APC40 1 (driving)") and "keys 2" in c for c in chips), chips)
        self.assertTrue(hidden_tft, "the screen preview is Mac-only")
        self.assertFalse(panel_hidden, "encoder buttons should show with v13.7")
        self.assertEqual(ui_status, 200)
        self.assertTrue(samples_shown and smp_rows >= 1, "Samples panel should list the library")




@unittest.skipUnless(os.path.exists(SIM), "build daisy/hostbuild/t95sim first (see daisy/t95sim.cpp)")
class PageControlTest(unittest.TestCase):
    """v13.9, control protocol 4: a page drives the real firmware control code
    through POST /do with no APC40 or keyboard anywhere."""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        link = os.path.join(cls.dir, "daisy")
        cls.card = os.path.join(cls.dir, "card")         # the Daisy's SD card: sets go here
        os.makedirs(cls.card)
        cls.lib = os.path.join(cls.dir, "lib")           # the T95's sample library
        os.makedirs(os.path.join(cls.lib, "breaks"))
        make_wav(os.path.join(cls.lib, "a kick.wav"), secs=0.4, hz=60)
        make_wav(os.path.join(cls.lib, "b tone.wav"), secs=1.0, hz=440)
        make_wav(os.path.join(cls.lib, "breaks", "loop.wav"), secs=2.0, hz=220)
        cls.sim = subprocess.Popen([SIM, link, cls.card], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.sim.kill)
        end = time.time() + 5
        while not os.path.exists(link) and time.time() < end:
            time.sleep(0.05)
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", link, "--host", "127.0.0.1", "--https-port", "0", "--rtp-port", "0", "--library", cls.lib,
             "--http-port", str(cls.http), "--ui", UI, "--state", os.path.join(cls.dir, "state.json"), "--no-midi"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.addClassCleanup(cls.proc.kill)
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["control"] >= 4, 10)
        cls.box = SseClient(cls.http, path="/events?view=apcbox")
        assert cls.box.wait_for(lambda c: any(l.startswith("S ") for l in c.lines))

    @classmethod
    def tearDownClass(cls):
        cls.box.close()
        cls.proc.terminate()
        cls.sim.terminate()
        cls.proc.wait(5)
        cls.sim.wait(5)

    post = RouterTest.post

    def setUp(self):
        BridgeTest.http = self.http

    def do(self, *a):
        return self.post("/do", {"a": list(a)})

    def last(self, prefix):
        return [l for l in list(self.box.lines) if l.startswith(prefix)][-1]

    def s(self, i):
        return int(self.last("S ").split()[1 + i])

    def test_p1_no_controller_and_the_page_still_drives(self):
        h = BridgeTest.health()
        self.assertEqual(h["controllers"], [])
        self.assertEqual(json.loads(BridgeTest.get("/caps")[2])["control_version"], 10)
        self.assertEqual(self.do("mute 3 1", "solo 9 1", "track 5")[0], 200)
        self.assertTrue(self.box.wait_for(lambda c: self.s(19) & 4 and self.s(20) & 256 and self.s(7) == 4))
        self.do("mute 3 0", "solo 9 0", "track 1")
        self.assertTrue(self.box.wait_for(lambda c: self.s(19) == 0 and self.s(20) == 0))

    def test_p2_bad_actions_never_reach_the_daisy(self):
        for bad in ("explode 1", "MUTE 3 1", "mute 3 1\nboot", "mute\x1bBOOT", "", "x" * 40, 5, ["mute 3 1"] * 33):
            code, _ = self.post("/do", {"a": bad})
            self.assertEqual(code, 400, repr(bad))
        self.assertTrue(BridgeTest.health()["daisy"])

    def test_p3_pots_steps_patterns(self):
        self.do("knob 5 0 100", "knob 5 1 20", "glob 0 90", "tempo 1337")
        self.assertTrue(self.box.wait_for(lambda c: self.last("T 4 ").split()[2:4] == ["100", "20"]))
        self.assertTrue(self.box.wait_for(lambda c: self.last("M ").split()[1] == "90" and self.s(5) == 1337))
        self.do("tempo 1200", "glob 0 102")
        pat = self.s(2)
        g0 = self.last("G ").split()[2]
        self.do("step %d 16 16 2" % (pat + 1))
        self.assertTrue(self.box.wait_for(lambda c: self.last("G ").split()[2][255] == "2"))
        self.do("step %d 16 16 %s" % (pat + 1, g0[255]))
        # pattern H8 gets a step: the 64-pattern mask shows it, clearing removes it
        self.do("step 64 1 1 1")
        self.assertTrue(self.box.wait_for(lambda c: int(self.last("O ").split()[1], 16) >> 63 & 1))
        self.do("pclear 64 0")
        self.assertTrue(self.box.wait_for(lambda c: not int(self.last("O ").split()[1], 16) >> 63 & 1))

    def test_p4_meters_and_waveforms(self):
        # a browser that connects late gets an outline for all 16 tracks
        late = SseClient(self.http, path="/events?view=apcbox")
        self.assertTrue(late.wait_for(lambda c: len({l.split()[1] for l in c.lines if l.startswith("Z ")}) == 16, 8))
        z = [l for l in late.lines if l.startswith("Z 0 ")][-1].split()
        self.assertEqual(len(z[4]), 192)
        self.assertGreater(int(z[2]), 100)
        late.close()
        n = len(self.box.lines)
        self.do("knob 1 0 110", "audition 1")
        hit = lambda c: any(l.startswith("H ") and int(l[2:4], 16) > 20 and int(l[34:36], 16) > 10 for l in c.lines[n:])
        self.assertTrue(self.box.wait_for(hit, 3), "no meter movement after a hit on track 1")
        quiet = lambda c: self.last("H ") == "H " + "0" * 36
        self.assertTrue(self.box.wait_for(quiet, 6))

    def test_p5_the_page_in_a_browser(self):
        """The T95's ApcBox page against the firmware control code: TV and phone, no scrolling, controls work"""
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        self.assertEqual(subprocess.run([sys.executable, os.path.join(HERE, "..", "ui", "build.py"), "--check"]).returncode, 0,
                         "t95/ui/apcbox.html is out of date: run t95/ui/build.py")
        shots = os.environ.get("APCBOX_SHOTS")
        url = "http://127.0.0.1:%d/apcbox" % self.http
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            errors = []
            # the TV, a Mac window, an iPad both ways, an iPhone with Safari's bars showing, a big iPhone
            for name, vp in (("tv", {"width": 1920, "height": 1080}), ("mac", {"width": 1440, "height": 800}),
                             ("ipad", {"width": 1180, "height": 750}), ("ipad-upright", {"width": 820, "height": 1100}),
                             ("phone", {"width": 390, "height": 664}), ("phone-big", {"width": 430, "height": 740}),
                             ("phone-sideways", {"width": 750, "height": 340}), ("phone-big-sideways", {"width": 932, "height": 400})):
                pg = b.new_page(viewport=vp)
                pg.on("pageerror", lambda e: errors.append(str(e)))
                pg.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
                self.do("pat 1", "step 1 2 3 2")                                          # an accent on pattern 1
                for v in ("perform", "steps", "mixer", "sound", "samples", "samples&sm=rec", "samples&sm=kits", "samples&sm=loop", "map", "patterns",
                          "patterns&pt=sets", "fx", "system"):
                    pg.goto(url + "?v=" + v + ("&tv=1" if name == "tv" else ""))
                    pg.wait_for_selector(".tab.on")
                    pg.wait_for_timeout(500)
                    size = pg.evaluate("[document.documentElement.scrollWidth, document.documentElement.scrollHeight]")
                    self.assertEqual(size, [vp["width"], vp["height"]], "%s %s scrolls" % (name, v))
                    # every view button and every control is inside the window
                    out = pg.evaluate("""() => { const W = innerWidth, H = innerHeight, bad = [];
                        for (const e of document.querySelectorAll('.tab, .st, .cell, .bigpad, .pad, .sc, .bs, .tb, .wave, .li, .setrow, .b, input')) {
                            const r = e.getBoundingClientRect();
                            if (r.width && (r.left < -1 || r.top < -1 || r.right > W + 1 || r.bottom > H + 1)) bad.push(e.className); }
                        return [document.querySelectorAll('.tab').length, bad]; }""")
                    self.assertEqual(out, [8, []], "%s %s: something is off the screen" % (name, v))
                    if v == "steps":
                        # an accented step stays in its row (its class once collided with the APC40 map's)
                        g = pg.evaluate("""() => { const c = [...document.querySelectorAll('.grid .sc')]; let bad = 0;
                            for (let r = 0; r < c.length / 16; r++) if (new Set(c.slice(r * 16, r * 16 + 16).map(e => Math.round(e.getBoundingClientRect().top))).size > 1) bad++;
                            return [c.length, bad, document.querySelectorAll('.grid .sc.ac').length,
                                    c.filter(e => getComputedStyle(e).position !== 'static').length]; }""")
                        self.assertEqual((g[0], g[1], g[3]), (256, 0, 0), "%s: step cells out of their rows" % name)
                        self.assertGreater(g[2], 0, "no accented step to check with")
                    mode = pg.evaluate("document.body.className")
                    self.assertEqual("short" in mode, "sideways" in name, "%s got layout '%s'" % (name, mode))
                    self.assertEqual("phone" in mode, name in ("phone", "phone-big", "ipad-upright"), "%s got layout '%s'" % (name, mode))
                    if shots:
                        pg.screenshot(path=os.path.join(shots, "%s-%s.png" % (name, v)))
                pg.close()
            pg = b.new_page(viewport={"width": 1920, "height": 1080})
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto(url + "?v=perform")
            pg.wait_for_selector(".strips .st")
            self.assertEqual(pg.locator(".strips .st").count(), 16)
            pg.wait_for_function("document.querySelector('.strips .st .nm').textContent.length > 2")
            # mute on strip 3, from the page to the firmware and back to the LED
            pg.locator(".strips .st").nth(2).locator(".b.m").click()
            self.assertTrue(self.box.wait_for(lambda c: self.s(19) & 4))
            pg.wait_for_function("document.querySelectorAll('.strips .st')[2].querySelector('.b.m').classList.contains('on')")
            pg.locator(".strips .st").nth(2).locator(".b.m").click()
            self.assertTrue(self.box.wait_for(lambda c: self.s(19) == 0))
            # drag the pan pot of strip 2 up: the Daisy's T line follows
            pot = pg.locator(".strips .st").nth(1).locator(".pot").first
            bx = pot.bounding_box()
            pan0 = int(self.last("T 1 ").split()[3])
            pg.mouse.move(bx["x"] + bx["width"] / 2, bx["y"] + bx["height"] / 2)
            pg.mouse.down()
            pg.mouse.move(bx["x"] + bx["width"] / 2, bx["y"] + bx["height"] / 2 - 40, steps=5)
            pg.mouse.up()
            self.assertTrue(self.box.wait_for(lambda c: int(self.last("T 1 ").split()[3]) > pan0 + 15))
            self.do("knob 2 1 %d" % pan0)
            # play from the top bar
            pg.click(".top .tb >> nth=1")
            self.assertTrue(self.box.wait_for(lambda c: self.s(0) == 1))
            pg.wait_for_function("document.querySelector('.top .tb.play') !== null")
            pg.click(".top .tb >> nth=0")
            self.assertTrue(self.box.wait_for(lambda c: self.s(0) == 0))
            # steps: tap a cell twice = accent, in the firmware's pattern
            pg.keyboard.press("2")
            pg.wait_for_selector(".grid .sc")
            cell = pg.locator(".grid .sc").nth(15 * 16 + 14)      # track 16, step 15
            g0 = self.last("G ").split()[2][254]
            cell.click()
            cell.click()
            want = str((int(g0) + 2) % 3)
            self.assertTrue(self.box.wait_for(lambda c: self.last("G ").split()[2][254] == want))
            self.do("step %d 16 15 %s" % (self.s(2) + 1, g0))
            # patterns: tap B3, it's queued (or playing at once when stopped)
            pg.keyboard.press("6")
            pg.locator(".bigpad").nth(10).click()
            self.assertTrue(self.box.wait_for(lambda c: 10 in (self.s(2), self.s(3))))
            self.do("pat 1")
            # sound: the selected track's waveform is drawn
            pg.keyboard.press("4")
            pg.wait_for_selector(".wave canvas")
            pg.wait_for_timeout(600)
            drawn = pg.evaluate("""() => { const c = document.querySelector('.wave canvas'), d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
                let n = 0; for (let i = 1; i < d.length; i += 4) if (d[i] > 150) n++; return n; }""")
            self.assertGreater(drawn, 500, "no waveform on the Sound view")
            b.close()
        self.assertEqual(errors, [])

    def test_p6_samples_view_waveforms_preview_send_kits(self):
        code, _, body = BridgeTest.get("/samples/peaks?src=lib&path=" + urllib.parse.quote("b tone.wav"))
        pk = json.loads(body)
        self.assertAlmostEqual(pk["secs"], 1.0, delta=0.05)
        self.assertEqual(len(pk["peaks"]), 192)
        code, ctype, wav = BridgeTest.get("/samples/preview?src=lib&path=" + urllib.parse.quote("b tone.wav"))
        self.assertEqual((ctype, wav[:4], wav[8:12]), ("audio/wav", b"RIFF", b"WAVE"))
        for bad in ("nope.wav", "../state.json", "breaks"):
            with self.assertRaises(urllib.error.HTTPError) as e:
                BridgeTest.get("/samples/peaks?src=lib&path=" + urllib.parse.quote(bad))
            self.assertEqual(e.exception.code, 400, bad)
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        with sync_playwright() as pw:
            b = pw.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
            pg = b.new_page(viewport={"width": 390, "height": 664})
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto("http://127.0.0.1:%d/apcbox?v=samples" % self.http)
            pg.wait_for_selector(".li canvas")
            names = pg.eval_on_selector_all(".li .nm2", "es => es.map(e => e.textContent)")
            self.assertEqual(names, ["📁 breaks", "a kick.wav", "b tone.wav"])
            pg.wait_for_function("""() => { const c = document.querySelectorAll('.li canvas')[1], d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
                let n = 0; for (let i = 1; i < d.length; i += 4) if (d[i] > 100) n++; return n > 200; }""")
            # hear it on this device: the button lights while it plays
            pg.locator(".li").nth(2).locator(".b.act").click()
            pg.wait_for_function("document.querySelectorAll('.li')[2].querySelector('.b.act').classList.contains('on') && pvSrc !== null")
            # into a folder and back
            pg.locator(".li.dir").first.click()
            pg.wait_for_function("document.querySelectorAll('.li .nm2')[1] && document.querySelectorAll('.li .nm2')[1].textContent === 'loop.wav'")
            pg.locator(".li.dir").first.click()
            pg.wait_for_function("document.querySelectorAll('.li .nm2').length === 3")
            # send the tone to track 4: it arrives in the firmware's loader
            self.do("track 4")
            pg.wait_for_function("document.querySelector('.hdr .big').textContent.startsWith('4 ')")
            pg.locator(".li").nth(2).locator("button:text-is('send')").click()
            pg.wait_for_function("document.querySelector('.msg').textContent.includes('is in track 4')", timeout=20000)
            self.assertTrue(self.box.wait_for(lambda c: self.last("N 3 ") == "N 3 b_tone.wav", 5), self.last("N 3 "))
            # kits: put kit 2 on tracks 9-16
            pg.goto("http://127.0.0.1:%d/apcbox?v=samples&sm=kits" % self.http)
            pg.wait_for_selector(".li.kit")
            self.assertGreaterEqual(pg.locator(".li.kit").count(), 2)
            k0 = self.last("KP ").split()
            want = "0" if k0[2] != "0" else "1"
            pg.locator(".li.kit").nth(int(want)).locator("button:text-is('9-16')").click()
            self.assertTrue(self.box.wait_for(lambda c: self.last("KP ").split()[2] == want, 8), self.last("KP "))
            pg.wait_for_function("document.querySelectorAll('.li.kit')[%s].querySelectorAll('.b.on').length >= 1" % want)
            b.close()
        self.assertEqual(errors, [])
        self.do("track 1", "type 4 sample")

    def lp(self):
        return [int(x) for x in self.last("LP ").split()[1:]]

    def test_p65_looper_quantise_roll_trim(self):
        """v13.12, control protocol 7: looper lengths, overdub, undo, quantise, roll; trim and normalise on the T95"""
        import wave
        import io
        from rackbridge.samples import edit_wav, edit_args
        # ---- trim and normalise, on the bytes ----
        src = open(os.path.join(self.lib, "b tone.wav"), "rb").read()          # 1 s, 48 kHz mono 16-bit
        out = edit_wav(src, 0.25, 0.75, True)
        w = wave.open(io.BytesIO(out))
        self.assertEqual((w.getnframes(), w.getframerate(), w.getnchannels()), (24000, 48000, 1))
        fr = struct.unpack("<24000h", w.readframes(24000))
        self.assertEqual((fr[0], fr[-1]), (0, 0))                              # faded at both cuts
        self.assertTrue(31500 <= max(max(fr), -min(fr)) <= 31700)              # -0.3 dB
        self.assertEqual(edit_wav(src, 0, 1, False), src)
        self.assertRaises(ValueError, edit_wav, src, 0.5, 0.5001, False)
        self.assertRaises(ValueError, edit_wav, b"RIFFxxxxWAVEnope" + bytes(64), 0, 1, True)
        self.assertEqual(edit_args(None, None, False), (0.0, 1.0, False))
        self.assertIsNone(edit_args("0.8", "0.2", False))
        self.assertIsNone(edit_args("x", 1, False))
        # ---- over HTTP: the preview is what would be sent ----
        q = "/samples/preview?src=lib&path=" + urllib.parse.quote("b tone.wav")
        whole = BridgeTest.get(q)[2]
        half = BridgeTest.get(q + "&start=0.5&end=1&norm=1")[2]
        self.assertTrue(abs((len(half) - 44) * 2 - (len(whole) - 44)) <= 8, (len(half), len(whole)))
        with self.assertRaises(urllib.error.HTTPError) as e:
            BridgeTest.get(q + "&start=0.9&end=0.1")
        self.assertEqual(e.exception.code, 400)
        self.assertEqual(self.post("/samples/send", {"src": "lib", "path": "b tone.wav", "track": 11, "start": 2, "end": 3})[0], 400)
        code, out = self.post("/samples/send", {"src": "lib", "path": "b tone.wav", "track": 11, "start": 0.25, "end": 0.5, "norm": True})
        self.assertEqual(code, 200, out)
        # the firmware loaded a quarter of a second: its outline line says how long
        self.assertTrue(self.box.wait_for(lambda c: any(l.startswith("Z 10 ") and abs(int(l.split()[2]) - 12000) < 200 for l in c.lines), 20),
                        [l[:30] for l in self.box.lines if l.startswith("Z 10 ")])
        # ---- looper from a page ----
        self.do("stop", "tempo 1800", "track 9")                               # a bar is 1.33 s
        self.assertEqual(self.do("lrec len 1", "lrec count 0", "quant rec 0")[0], 200)
        self.assertTrue(self.box.wait_for(lambda c: self.lp()[0] == 1, 5), self.last("LP "))
        self.assertEqual(self.do("lrec arm 9")[0], 200)
        self.assertTrue(self.box.wait_for(lambda c: self.s(47) == 2 and self.s(48) == 8, 5))          # recording on track 9
        self.assertTrue(self.box.wait_for(lambda c: self.s(47) == 0 and self.lp()[7] == 9, 5), self.last("LP "))   # stopped by itself; can be undone
        self.assertEqual(self.last("N 8 "), "N 8 [loop 1 bars]")
        self.do("lrec over 9")
        self.assertTrue(self.box.wait_for(lambda c: self.s(47) != 0 and self.lp()[6] == 1, 5), self.last("LP "))
        self.assertTrue(self.box.wait_for(lambda c: self.s(47) == 0, 6))
        self.do("lrec undo")                                                   # the overdub
        self.assertTrue(self.box.wait_for(lambda c: self.lp()[7:9] == [0, 9], 5), self.last("LP "))
        self.assertEqual(self.last("N 8 "), "N 8 [loop 1 bars]")
        self.do("lrec redo")                                                   # v13.13: and back again
        self.assertTrue(self.box.wait_for(lambda c: self.lp()[7:9] == [9, 0], 5), self.last("LP "))
        self.do("lrec undo")
        self.assertTrue(self.box.wait_for(lambda c: self.lp()[7:9] == [0, 9], 5), self.last("LP "))
        self.do("lrec over 3")                                                 # no loop there: refused
        time.sleep(0.3)
        self.assertEqual(self.s(47), 0)
        # settings come back on the LP line
        self.assertEqual(self.lp()[9], 1)                                      # held FX snap to the step unless told
        self.do("lrec len 4", "lrec count 1", "quant rec 1", "quant pat 2", "quant mute 1", "ingain 100", "quant fx 2")
        self.assertTrue(self.box.wait_for(lambda c: self.lp()[:6] == [4, 1, 1, 2, 1, 100] and self.lp()[9] == 2, 5), self.last("LP "))
        for bad in ("lrec len 3", "quant pat 3", "quant rec 2", "quant banana 1", "roll 17 1", "ingain 200", "quant fx 3"):
            self.do(bad)
        time.sleep(0.3)
        self.assertEqual(self.lp()[:6], [4, 1, 1, 2, 1, 100])
        # roll: held from a page, shows in the S line's roll mask
        self.do("roll 3 1")
        self.assertTrue(self.box.wait_for(lambda c: self.s(18) & 4, 5), self.last("S "))
        self.do("roll 3 0")
        self.assertTrue(self.box.wait_for(lambda c: not self.s(18) & 4, 5))
        self.do("lrec len 0", "lrec count 0", "quant rec 0", "quant pat 0", "quant mute 0", "quant fx 1", "ingain 64", "stop", "type 9 sample", "type 11 sample")
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            errors = []
            for w_, h_ in ((1920, 1080), (390, 664), (844, 390)):
                pg = b.new_page(viewport={"width": w_, "height": h_})
                pg.on("pageerror", lambda e: errors.append(str(e)))
                # the looper view: settings go to the firmware and come back lit
                pg.goto("http://127.0.0.1:%d/apcbox?v=samples&sm=loop" % self.http)
                pg.wait_for_selector(".lset .seg")
                self.assertEqual(pg.evaluate("[document.documentElement.scrollHeight > innerHeight + 1, document.documentElement.scrollWidth > innerWidth + 1]"), [False, False])
                pg.click(".lset .seg span:text-is('beat')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[2] == 1, 5), self.last("LP "))
                pg.click(".lset .seg span:text-is('bar')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[2] == 0, 5))
                self.assertTrue(pg.locator("button.bigpad:has-text('OVERDUB')").is_disabled())      # no loop on the track
                pg.click(".lset .seg span:text-is('1')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[0] == 1, 5))
                pg.locator("button.bigpad:has-text('REC LOOP')").click()
                self.assertTrue(self.box.wait_for(lambda c: self.s(47) != 0, 5))
                pg.wait_for_function("document.querySelector('.msg').textContent.includes('bar 1 of 1') || document.querySelector('.msg').textContent.includes('armed')")
                self.assertTrue(self.box.wait_for(lambda c: self.s(47) == 0 and self.lp()[7] == self.s(7) + 1, 6), self.last("LP "))
                pg.wait_for_function("!document.querySelectorAll('button.bigpad')[1].disabled")       # OVERDUB now
                pg.locator("button.bigpad:has-text('UNDO TAKE')").click()
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[7] == 0 and self.lp()[8] == self.s(7) + 1, 5), self.last("LP "))
                pg.locator("button.bigpad:has-text('REDO TAKE')").click()               # the same pad
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[8] == 0 and self.lp()[7] == self.s(7) + 1, 5), self.last("LP "))
                pg.locator("button.bigpad:has-text('UNDO TAKE')").click()
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[7] == 0, 5))
                self.do("stop", "lrec len 0")
                # quantise on the System view
                pg.goto("http://127.0.0.1:%d/apcbox?v=system" % self.http)
                pg.wait_for_selector(".qrow .seg")
                self.assertEqual(pg.evaluate("[document.documentElement.scrollHeight > innerHeight + 1, document.documentElement.scrollWidth > innerWidth + 1]"), [False, False])
                pg.click(".qrow .qc:nth-child(1) span:text-is('step')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[3] == 2, 5))
                pg.wait_for_function("document.querySelector('.qrow .qc:nth-child(1) span.on').textContent === 'step'")
                pg.click(".qrow .qc:nth-child(1) span:text-is('bar')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[3] == 0, 5))
                pg.wait_for_function("document.querySelector('.qrow .qc:nth-child(4) span.on').textContent === 'step'")
                pg.click(".qrow .qc:nth-child(4) span:text-is('beat')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[9] == 2, 5))
                pg.click(".qrow .qc:nth-child(4) span:text-is('step')")
                self.assertTrue(self.box.wait_for(lambda c: self.lp()[9] == 1, 5))
                # trim view: drag both markers in, send
                pg.goto("http://127.0.0.1:%d/apcbox?v=samples&sm=lib" % self.http)
                pg.wait_for_selector(".li.file")
                pg.locator(".li.file").nth(1).locator(".nm2").click()
                pg.wait_for_selector(".wave.trim")
                pg.wait_for_function("document.querySelector('.wave.trim .tag').textContent.includes('of 1.00 s')")
                bb = pg.locator(".wave.trim").bounding_box()
                y = bb["y"] + bb["height"] / 2
                for a, c in ((0.02, 0.25), (0.98, 0.75)):
                    pg.mouse.move(bb["x"] + bb["width"] * a, y)
                    pg.mouse.down()
                    pg.mouse.move(bb["x"] + bb["width"] * c, y, steps=4)
                    pg.mouse.up()
                self.assertRegex(pg.locator(".wave.trim .tag").inner_text(), r"0\.2[45] – 0\.7[456] s · keeps 0\.5[0-2] of 1\.00 s")
                self.assertEqual(pg.evaluate("[document.documentElement.scrollHeight > innerHeight + 1, document.documentElement.scrollWidth > innerWidth + 1]"), [False, False])
                self.do("track 12")
                pg.wait_for_function("[...document.querySelectorAll('.hdr .big')].pop().textContent.startsWith('12 ')")
                n0 = len([l for l in self.box.lines if l.startswith("Z 11 ")])
                pg.click("button:text-is('SEND')")
                pg.wait_for_function("document.querySelector('.msg').textContent.includes('is in track 12')", timeout=20000)
                self.assertTrue(self.box.wait_for(lambda c: len([l for l in c.lines if l.startswith("Z 11 ")]) > n0
                                                  and abs(int(self.last("Z 11 ").split()[2]) - 24500) < 1500, 8), self.last("Z 11 ")[:30])
                pg.click("button:text-is('‹ back')")
                pg.wait_for_selector(".li.file")
                pg.close()
                self.do("type 12 sample", "track 1")
            b.close()
        self.assertEqual(errors, [])

    def test_p68_kits_made_on_the_box(self):
        """v13.14, control protocol 9: a page's 8 tracks saved as a kit, with settings and 303s; rename, delete, undo"""
        kits = os.path.join(self.card, "kits")
        nb = int(self.last("KP ").split()[4])
        n0 = int(self.last("KP ").split()[3])
        self.assertGreaterEqual(nb, 1)
        self.do("stop", "kit 1 0")
        time.sleep(1.0)
        self.do("track 2", "knob 2 0 30", "type 3 303")                        # track 2 quiet, track 3 a 303
        self.assertTrue(self.box.wait_for(lambda c: self.last("N 2 ") == "N 2 [303]", 8), self.last("N 2 "))
        self.assertEqual(self.do("kit save 0 page kit")[0], 200)
        self.assertTrue(self.box.wait_for(lambda c: any(l == "KL %d page kit" % i for i in range(nb, n0 + 1) for l in c.lines), 15))
        d = os.path.join(kits, "page kit")
        files = sorted(os.listdir(d))
        self.assertIn("kit.txt", files)
        self.assertIn("1 kick.wav", files)
        self.assertFalse([f for f in files if f.startswith("3 ")], files)       # the 303 is a line in kit.txt, not a sound
        txt = open(os.path.join(d, "kit.txt")).read().split("\n")
        self.assertEqual(txt[0], "APK1")
        self.assertTrue(any(l.startswith("3 a ") and len(l.split()) == 23 for l in txt), txt)
        self.assertTrue(any(l.startswith("2 s ") and len(l.split()) == 15 for l in txt), txt)
        self.assertEqual(int(self.last("KP ").split()[3]), n0 + 1)
        k = next(int(l.split()[1]) for l in reversed(self.box.lines) if l.startswith("KL ") and l.endswith(" page kit"))
        # the same name again needs "over" (the page asks first)
        self.do("kit save 0 page kit")
        time.sleep(0.5)
        self.assertEqual(int(self.last("KP ").split()[3]), n0 + 1)
        # change the page, load the kit: settings and the 303 come back; undo and redo of the load
        self.do("type 3 sample", "knob 2 0 100")
        time.sleep(0.8)
        self.do("kit %d 0" % (k + 1))
        self.assertTrue(self.box.wait_for(lambda c: self.last("N 2 ") == "N 2 [303]" and self.last("LP ").split()[8] == "101", 8),
                        (self.last("N 2 "), self.last("LP ")))
        self.do("lrec undo")
        self.assertTrue(self.box.wait_for(lambda c: self.last("N 2 ") != "N 2 [303]" and self.last("LP ").split()[9] == "101", 8),
                        (self.last("N 2 "), self.last("LP ")))
        self.do("lrec redo")
        self.assertTrue(self.box.wait_for(lambda c: self.last("N 2 ") == "N 2 [303]", 8))
        # names: capitals and odd characters don't get past the bridge; built-in kits stay
        self.assertEqual(self.do("kit save 0 Bad/Name")[0], 400)
        self.do("kit delete 1", "kit rename 1 mine")
        time.sleep(0.4)
        self.assertEqual(int(self.last("KP ").split()[3]), n0 + 1)
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            errors = []
            for i, (w_, h_) in enumerate(((1920, 1080), (390, 664), (844, 390))):
                pg = b.new_page(viewport={"width": w_, "height": h_})
                pg.on("pageerror", lambda e: errors.append(str(e)))
                pg.goto("http://127.0.0.1:%d/apcbox?v=samples&sm=kits" % self.http)
                pg.wait_for_selector(".kslot")
                pg.wait_for_function("document.querySelectorAll('.li.kit').length > 0")
                self.assertEqual(pg.evaluate("[document.documentElement.scrollHeight > innerHeight + 1, document.documentElement.scrollWidth > innerWidth + 1]"), [False, False])
                self.assertEqual(pg.locator(".kslot").count(), 8)
                pg.locator(".kslot").nth(4).click()                              # a slot: selects the track
                self.assertTrue(self.box.wait_for(lambda c: self.s(7) == 4, 5))
                # save as a new kit
                name = "web kit %d" % i
                pg.fill("input[type=text]", name)
                pg.locator("button.bigpad:has-text('SAVE AS KIT')").click()
                self.assertTrue(self.box.wait_for(lambda c: os.path.exists(os.path.join(kits, name, "kit.txt")), 15))
                pg.wait_for_function("[...document.querySelectorAll('.li.kit .nm2')].some(e => e.textContent === %s) || true" % json.dumps(name))
                # the same name: asks before it replaces
                pg.wait_for_function("document.querySelectorAll('button.bigpad')[0].textContent.startsWith('SAVE OVER')")
                t0 = os.path.getmtime(os.path.join(kits, name, "kit.txt"))
                time.sleep(1.1)
                pg.locator("button.bigpad:has-text('SAVE OVER')").click()
                pg.wait_for_function("document.querySelectorAll('button.bigpad')[0].textContent.startsWith('SURE?')")
                time.sleep(0.5)
                self.assertEqual(os.path.getmtime(os.path.join(kits, name, "kit.txt")), t0)      # nothing yet
                pg.locator("button.bigpad:has-text('SURE?')").click()
                self.assertTrue(self.box.wait_for(lambda c: os.path.getmtime(os.path.join(kits, name, "kit.txt")) > t0, 15))
                # rename it: find its row (the list has pages), tap, type, RENAME
                pg.wait_for_function("!document.querySelectorAll('button.bigpad')[0].disabled")
                for _ in range(12):
                    if pg.locator(".li.kit .nm2", has_text=name).count():
                        break
                    pg.locator(".hdr button.b:has-text('▶')").last.click()
                pg.locator(".li.kit", has=pg.locator(".nm2", has_text=name)).locator(".nm2").click()
                self.assertEqual(pg.input_value("input[type=text]"), name)
                new = "renamed %d" % i
                pg.fill("input[type=text]", new)
                pg.locator("button.bigpad:has-text('RENAME')").click()
                self.assertTrue(self.box.wait_for(lambda c: os.path.exists(os.path.join(kits, new, "kit.txt")) and not os.path.exists(os.path.join(kits, name)), 8))
                # delete it: asks first
                pg.wait_for_function("[...document.querySelectorAll('.li.kit .nm2')].some(e => e.textContent === %s)" % json.dumps(new))
                pg.locator(".li.kit", has=pg.locator(".nm2", has_text=new)).locator(".nm2").click()
                pg.locator("button.bigpad:has-text('DELETE')").click()
                pg.wait_for_function("document.querySelectorAll('button.bigpad')[2].textContent.startsWith('SURE?')")
                time.sleep(0.4)
                self.assertTrue(os.path.exists(os.path.join(kits, new)))
                pg.locator("button.bigpad:has-text('SURE?')").click()
                self.assertTrue(self.box.wait_for(lambda c: not os.path.exists(os.path.join(kits, new)), 8))
                # a built-in kit can't be renamed or deleted from here
                pg.wait_for_function("document.querySelectorAll('.li.kit').length > 0")
                if pg.locator(".hdr button.b:has-text('◀')").last.is_enabled():
                    for _ in range(12):
                        if not pg.locator(".hdr button.b:has-text('◀')").last.is_enabled():
                            break
                        pg.locator(".hdr button.b:has-text('◀')").last.click()
                pg.locator(".li.kit .nm2").first.click()
                self.assertTrue(pg.locator("button.bigpad:has-text('DELETE')").is_disabled())
                self.assertTrue(pg.locator("button.bigpad:has-text('RENAME')").is_disabled())
                pg.close()
            b.close()
        self.assertEqual(errors, [])
        self.do("kit delete %d" % (k + 1), "kit 1 0", "track 1")
        self.assertTrue(self.box.wait_for(lambda c: not os.path.exists(d), 8))

    def test_p69_button_map(self):
        """v13.15, control protocol 10: the APC40's layout as a table the map page edits"""
        def bmap():
            m = {}
            for l in self.box.lines:
                if l.startswith("MB "):
                    _, frm, h = l.split()
                    for k in range(len(h) // 4):
                        m[int(frm) + k] = (int(h[k * 4:k * 4 + 2], 16), int(h[k * 4 + 2:k * 4 + 4], 16))
            return m
        self.assertTrue(self.box.wait_for(lambda c: len(bmap()) == 110, 5))
        self.assertTrue(all(v == (k, k | 128) for k, v in bmap().items()))
        self.assertEqual(self.last("MC "), "MC " + "".join("%02x" % i for i in range(27)))
        # METRONOME (109) alone = PLAY (91); SHIFT + DETAIL VIEW (106) = nothing; knob 1 = fader 3
        self.assertEqual(self.do("map b 109 0 91 0", "map b 106 1 255 0", "map c 0 18")[0], 200)
        self.assertTrue(self.box.wait_for(lambda c: bmap()[109] == (91, 109 | 128) and bmap()[106] == (106, 255)
                                          and self.last("MC ")[3:5] == "12", 5), (bmap().get(109), self.last("MC ")))
        self.do("map b 98 0 91 0", "map b 91 0 98 0", "map c 26 0", "map b 200 0 1 0")     # SHIFT, the endless knob, nonsense
        time.sleep(0.4)
        self.assertEqual(bmap()[98], (98, 98 | 128))
        self.assertEqual(bmap()[91], (91, 91 | 128))
        # it is on the card (so the Daisy has it without the T95)
        f = os.path.join(self.card, "apcbox", "apcmap.txt")
        self.assertTrue(self.box.wait_for(lambda c: os.path.exists(f) and "b 109 0 91 0" in open(f).read(), 6))
        self.assertIn("c 0 18", open(f).read())
        self.assertIn("b 106 1 255 0", open(f).read())
        self.do("map reset")
        self.assertTrue(self.box.wait_for(lambda c: all(v == (k, k | 128) for k, v in bmap().items()), 5))
        self.assertTrue(self.box.wait_for(lambda c: open(f).read().strip() == "APM1", 6), open(f).read())
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            errors = []
            for w_, h_ in ((1920, 1080), (390, 664), (844, 390)):
                pg = b.new_page(viewport={"width": w_, "height": h_})
                pg.on("pageerror", lambda e: errors.append(str(e)))
                pg.goto("http://127.0.0.1:%d/apcbox?v=map" % self.http)
                pg.wait_for_function("document.querySelectorAll('.ac').length === 137 && !!st.bmap && st.bmap.length === 110")
                self.assertEqual(pg.evaluate("[document.documentElement.scrollHeight > innerHeight + 1, document.documentElement.scrollWidth > innerWidth + 1]"), [False, False])
                ctl = lambda cid: pg.evaluate("cur.ctl.findIndex(c => c.cid === %d)" % cid)
                tap = lambda i: pg.evaluate("cur.els[%d].click()" % i)
                info = lambda: pg.locator(".apcinfo").inner_text()
                # METRONOME alone := PLAY (one job: assigned at once)
                tap(ctl(109))
                self.assertIn("click", info())
                pg.locator(".apcrow", has_text="ALONE").locator("button:has-text('CHANGE')").click()
                self.assertIn("whose job", info())
                tap(ctl(91))
                self.assertTrue(self.box.wait_for(lambda c: bmap()[109] == (91, 109 | 128), 5), bmap().get(109))
                pg.wait_for_function("cur.els[%d].classList.contains('chg') && cur.ctl[%d].b.textContent === 'play'" % (ctl(109), ctl(109)))
                self.assertIn("printed on PLAY", info())
                # SHIFT + METRONOME := the left arrow, which has two jobs: asks, take its SHIFT one
                pg.locator(".apcrow", has_text="SHIFT").locator("button:has-text('CHANGE')").click()
                tap(ctl(104))
                self.assertIn("two jobs", info())
                pg.locator(".apcbtns button:has-text('SHIFT: sample')").click()
                self.assertTrue(self.box.wait_for(lambda c: bmap()[109] == (91, 104 | 128), 5), bmap().get(109))
                # a job no button has
                tap(ctl(107))
                pg.locator(".apcrow", has_text="ALONE").locator("button:has-text('CHANGE')").click()
                pg.locator(".apcbtns button:has-text('One-shot sample')").click()
                self.assertTrue(self.box.wait_for(lambda c: bmap()[107][0] == 110, 5), bmap().get(107))
                # a whole row: the RECORD ARM row (roll) does the pad row 4 jobs (FX holds)
                tap(ctl(45))
                pg.locator(".apcrow", has_text="ALONE").locator("button:has-text('CHANGE')").click()
                pg.locator(".apcbtns button:has-text('whole row')").click()
                tap(ctl(24 + 3))
                pg.locator(".apcbtns button").first.click()                                   # its job alone (the FX hold)
                self.assertTrue(self.box.wait_for(lambda c: [bmap()[45 + k][0] for k in range(8)] == [24 + k for k in range(8)], 5),
                                [bmap().get(45 + k) for k in range(8)])
                # a knob: TRACK CONTROL 2 := the crossfader
                tap(pg.evaluate("cur.ctl.findIndex(c => c.ccid === 1)"))
                pg.locator(".apcrow button:has-text('CHANGE')").click()
                tap(pg.evaluate("cur.ctl.findIndex(c => c.ccid === 25)"))
                self.assertTrue(self.box.wait_for(lambda c: self.last("MC ")[5:7] == "19", 5), self.last("MC "))
                # SHIFT can't be changed, and can't be a target
                tap(ctl(98))
                self.assertEqual(pg.locator(".apcrow button:has-text('CHANGE')").count(), 0)
                # one button back, then everything (asks)
                tap(ctl(107))
                pg.locator(".apcrow button:has-text('↺')").click()
                self.assertTrue(self.box.wait_for(lambda c: bmap()[107] == (107, 107 | 128), 5))
                pg.locator("button:has-text('RESET ALL')").click()
                pg.wait_for_function("[...document.querySelectorAll('button')].some(b => b.textContent === 'SURE?')")
                time.sleep(0.3)
                self.assertEqual(bmap()[109][0], 91)
                pg.locator("button:has-text('SURE?')").click()
                self.assertTrue(self.box.wait_for(lambda c: all(v == (k, k | 128) for k, v in bmap().items())
                                                  and self.last("MC ") == "MC " + "".join("%02x" % i for i in range(27)), 5))
                pg.wait_for_function("document.querySelectorAll('.ac.chg').length === 0")
                pg.close()
            b.close()
        self.assertEqual(errors, [])

    def test_p7_sets_from_the_page(self):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        self.assertTrue(self.box.wait_for(lambda c: self.s(13) == 1))            # the card is there
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            pg = b.new_page(viewport={"width": 1440, "height": 800})
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto("http://127.0.0.1:%d/apcbox?v=patterns&pt=sets" % self.http)
            pg.wait_for_selector(".setrow")
            self.assertEqual(pg.locator(".setrow.has").count(), 0)
            self.do("tempo 1410", "mute 6 1")
            pg.fill("input[type=text]", "Night Jam!")
            pg.click("button:text-is('SAVE AS NEW')")
            pg.wait_for_function("document.querySelector('.setrow.has .nm2') && document.querySelector('.setrow.has .nm2').textContent === 'night jam-'", timeout=15000)
            self.assertTrue(os.path.exists(os.path.join(self.card, "sets", "01", "set.bin")))
            self.assertEqual(self.last("J 0 ").split(" ", 6)[3], "1410")
            pg.wait_for_function("document.querySelector('.setrow.open') !== null")
            # change things, then load the set back: a second tap confirms dropping the changes
            self.do("tempo 900", "mute 6 0")
            self.assertTrue(self.box.wait_for(lambda c: self.s(54) == 1 and self.s(5) == 900, 5))
            pg.locator(".setrow").first.click()
            pg.click("button:text-is('LOAD')")
            pg.wait_for_selector("button:text-is('DROP CHANGES? TAP AGAIN')")
            pg.click("button:text-is('DROP CHANGES? TAP AGAIN')")
            self.assertTrue(self.box.wait_for(lambda c: self.s(5) == 1410 and self.s(19) & 32 and self.s(55) == 0, 20), self.last("S "))
            # rename, change the transition, delete (two taps)
            pg.fill("input[type=text]", "second")
            pg.click("button:text-is('RENAME')")
            pg.wait_for_function("document.querySelector('.setrow.has .nm2').textContent === 'second'")
            t0 = self.last("JT ")
            pg.click(".hdr .b.act")
            self.assertTrue(self.box.wait_for(lambda c: self.last("JT ") != t0))
            pg.click("button:text-is('DELETE')")
            pg.click("button:text-is('DELETE? TAP AGAIN')")
            pg.wait_for_function("document.querySelectorAll('.setrow.has').length === 0")
            self.assertFalse(os.path.exists(os.path.join(self.card, "sets", "01", "set.bin")))
            b.close()
        self.assertEqual(errors, [])
        self.do("tempo 1200", "mute 6 0", "set trans 1")


@unittest.skipUnless(os.path.exists(SIM), "build daisy/hostbuild/t95sim first (see daisy/t95sim.cpp)")
class ClockDaisyTest(unittest.TestCase):
    """v13.11: the firmware's sequencer follows the T95's clock (ESC T), and the T95's clock follows the Daisy (CL)"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        link = os.path.join(cls.dir, "daisy")
        cls.sim = subprocess.Popen([SIM, link], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.sim.kill)
        end = time.time() + 5
        while not os.path.exists(link) and time.time() < end:
            time.sleep(0.05)
        cls.synth = FakeController(os.path.join(cls.dir, "synth"))
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", link, "--host", "127.0.0.1", "--https-port", "0", "--rtp-port", "0",
             "--http-port", str(cls.http), "--ui", UI, "--state", os.path.join(cls.dir, "state.json"),
             "--midi", cls.synth.link + "=Synth"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.addClassCleanup(cls.proc.kill)
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["control"] >= 6, 10)
        cls.box = SseClient(cls.http, path="/events?view=apcbox")
        assert cls.box.wait_for(lambda c: any(l.startswith("S ") for l in c.lines))
        cls.rx = TimedReader(cls.synth)

    @classmethod
    def tearDownClass(cls):
        cls.rx.stop()
        cls.box.close()
        cls.proc.terminate()
        cls.sim.terminate()
        cls.proc.wait(5)
        cls.sim.wait(5)

    post = RouterTest.post
    wait = MidiTest.wait

    def setUp(self):
        BridgeTest.http = self.http

    def s(self, i):
        return int([l for l in list(self.box.lines) if l.startswith("S ")][-1].split()[1 + i])

    def clock(self):
        return json.loads(BridgeTest.get("/clock?json=1")[2])

    def test_k1_daisy_follows_the_t95(self):
        self.assertTrue(self.clock()["daisy_ok"])
        self.assertEqual(self.post("/clock", {"on": True, "source": "internal", "bpm": 137, "outputs": ["daisy", "Synth"]})[0], 200)
        # tempo arrives while stopped; then start: the firmware's sequencer starts with us
        self.assertTrue(self.box.wait_for(lambda c: self.s(5) == 1370 and self.s(0) == 0, 5))
        self.post("/clock/transport", {"cmd": "start"})
        self.assertTrue(self.box.wait_for(lambda c: self.s(0) == 1, 3))
        time.sleep(5)
        c = self.clock()
        print("\n  Daisy following the T95 at 137 BPM: off by %s ms (smoothed), T95 ticks late %s" % (c["daisy_err_ms"], c["late_ms"]))
        self.assertTrue(c["daisy_following"])
        self.assertLess(abs(c["daisy_err_ms"]), 4.0)
        # same sixteenth on both: the T95's position and the firmware's step
        pos16 = (int(c["position"].split(".")[1]) - 1) * 4 + int(c["position"].split(".")[2]) - 1
        self.assertIn((self.s(1) - pos16) % 16, (0, 1, 15))
        # a tempo change follows; stop stops it
        self.post("/clock", {"bpm": 96})
        self.assertTrue(self.box.wait_for(lambda c: self.s(5) == 960, 3))
        self.post("/clock/transport", {"cmd": "stop"})
        self.assertTrue(self.box.wait_for(lambda c: self.s(0) == 0, 3))
        # no longer sent to the Daisy: it says it's on its own again
        self.post("/clock", {"outputs": ["Synth"]})
        self.assertTrue(self.wait(lambda: not self.clock()["daisy_following"], 5))

    def test_k2_t95_follows_the_daisy(self):
        do = lambda *a: self.post("/do", {"a": list(a)})
        do("stop", "tempo 1300", "glob 8 60")              # with swing: only the even steps are used
        self.post("/clock", {"on": True, "source": "daisy", "outputs": ["Synth"], "send_stopped": False})
        time.sleep(0.5)
        self.rx.clear()
        do("play")
        time.sleep(5)
        c = self.clock()
        ticks = self.rx.ticks()
        m, sd, worst = intervals_ms(ticks[48:])
        print("\n  T95 following the Daisy at 130 BPM with swing: locked %s at %.2f, out interval %.3f ms (ideal %.3f), sd %.3f; "
              "step reports jitter %s ms" % (c["locked"], c["tempo"], m, 60000 / (130 * 24), sd, c["in_jitter_ms"]))
        self.assertTrue(c["locked"] and c["playing"])
        self.assertAlmostEqual(c["tempo"], 130.0, delta=0.01)
        self.assertEqual(self.rx.bytes_()[0], 0xFA)          # it started with the Daisy
        self.assertAlmostEqual(m, 60000 / (130 * 24), delta=0.15)
        self.assertLess(sd, 1.5)
        do("stop")
        self.assertTrue(self.wait(lambda: self.rx.bytes_()[-1:] == b"\xfc", 3))
        self.assertFalse(self.clock()["playing"])
        do("glob 8 0", "tempo 1200")
        self.post("/clock", {"on": False, "source": "internal", "outputs": []})


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
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1", "--https-port", "0",
             "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", cls.state, "--ir", cls.fifo],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "a"))
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
            self.assertEqual(names, ["ApcBox groovebox", "MIDI hub", "Clock", "Cubase sync", "Numark DJ"])
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
            self.assertTrue(pg.is_visible("#tabs"))
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
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1", "--https-port", "0",
             "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", cls.state, "--ir-lirc", cls.fifo],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "a"))
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



def make_wav(path, secs=0.5, rate=48000, ch=1, width=2, hz=330.0):
    import math
    import wave
    n = int(secs * rate)
    with wave.open(path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(width)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            v = int(9000 * math.sin(2 * math.pi * hz * i / rate))
            frames += (v.to_bytes(2, "little", signed=True) if width == 2
                       else (v << 8).to_bytes(3, "little", signed=True)) * ch
        w.writeframes(bytes(frames))


class LossyDaisy(RespondingDaisy):
    """Takes ESC F frames like the firmware, but loses some acks and claims
    CRC errors on others, so the bridge has to resend"""

    def plug(self):
        super().plug()
        import random
        self.rng = random.Random(7)
        self.files = {}
        self.refuse = None
        self.drop = 0.15
        self.crcerr = 0.05

    def say(self, fd, line):
        try:
            os.write(fd, (line + "\n").encode())
        except OSError:
            pass

    def on_frame(self, fd, f):
        import base64 as b64
        import binascii as ba
        if f.startswith("FO "):
            _, i, track, size, name = f.split(" ", 4)
            if self.refuse:
                return self.say(fd, "FO %s err %s" % (i, self.refuse))
            self.files[i] = {"size": int(size), "chunks": {}, "track": int(track), "name": name}
            self.say(fd, "FO %s ok 1572864" % i)
        elif f.startswith("FD "):
            _, i, seq, crc, data = f.split(" ", 4)
            raw = b64.b64decode(data)
            if ba.crc32(raw) & 0xFFFFFFFF != int(crc, 16):
                return self.say(fd, "FA %s %s err crc" % (i, seq))
            if self.rng.random() < self.crcerr:
                return self.say(fd, "FA %s %s err crc" % (i, seq))
            self.files[i]["chunks"][int(seq)] = raw
            if self.rng.random() >= self.drop:
                self.say(fd, "FA %s %s ok" % (i, seq))
        elif f.startswith("FC "):
            i = f.split()[1]
            x = self.files[i]
            need = (x["size"] + 511) // 512
            if len(x["chunks"]) != need:
                return self.say(fd, "FC %s err missing %d" % (i, need - len(x["chunks"])))
            x["data"] = b"".join(x["chunks"][k] for k in range(need))
            self.say(fd, "FC %s ok" % i)
            self.say(fd, "FL %s ok" % i)


class SampleTest(unittest.TestCase):
    """Library, SD card, uploads, and sending samples through a lossy link"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.lib = os.path.join(cls.dir, "lib")
        cls.sd = os.path.join(cls.dir, "sd")
        os.makedirs(os.path.join(cls.lib, "drums"))
        os.makedirs(os.path.join(cls.sd, "Kits", "808"))
        make_wav(os.path.join(cls.lib, "drums", "tone.wav"), 1.0)
        make_wav(os.path.join(cls.sd, "Kits", "808", "BD.wav"), 0.3)
        make_wav(os.path.join(cls.sd, "Kits", "808", "SD 24bit stereo.wav"), 0.3, ch=2, width=3, rate=44100)
        open(os.path.join(cls.sd, "Kits", "readme.txt"), "w").write("not audio")
        cls.link = os.path.join(cls.dir, "daisy")
        cls.daisy = LossyDaisy(cls.link)
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", cls.link, "--host", "127.0.0.1", "--https-port", "0", "--http-port", str(cls.http),
             "--ui", UI, "--no-midi", "--state", os.path.join(cls.dir, "state.json"),
             "--library", cls.lib, "--sdcard", cls.sd, "--sdcard-any"],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.addClassCleanup(cls.proc.kill)
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: h["daisy"], 10)
        cls.daisy.send("V 9\nP 3 00 00 00 -1\n")
        BridgeTest.wait_health(lambda h: h["control"] == 3, 5)

    def setUp(self):
        BridgeTest.http = self.http

    post = IrTest.post

    def samples(self, src, path=""):
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/samples?src=%s&path=%s"
                                        % (self.http, src, urllib.parse.quote(path)), timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def wait_xfer(self, xid, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            s = BridgeTest.health()["samples"]
            for t in [s["current"]] + s["recent"]:
                if t and t["id"] == xid and t["state"] in ("done", "failed"):
                    return t
            time.sleep(0.1)
        raise AssertionError("transfer %d didn't finish: %s" % (xid, BridgeTest.health()["samples"]))

    def test_s1_listing_and_traversal(self):
        code, d = self.samples("lib")
        self.assertEqual((code, d["dirs"]), (200, ["drums"]))
        code, d = self.samples("sd", "Kits/808")
        self.assertEqual(sorted(f["name"] for f in d["files"]), ["BD.wav", "SD 24bit stereo.wav"])
        code, d = self.samples("sd", "Kits")
        self.assertEqual((d["dirs"], d["files"]), (["808"], []), "readme.txt isn't audio")
        for bad in ("../", "../../etc", "/etc", "drums/../../"):
            self.assertEqual(self.samples("lib", bad)[0], 400, bad)
        self.assertEqual(self.post("/samples/send", {"src": "lib", "path": "../state.json", "track": 1})[0], 400)

    def test_s2_upload(self):
        p = os.path.join(self.dir, "up.wav")
        make_wav(p, 0.2)
        body = open(p, "rb").read()
        req = urllib.request.Request("http://127.0.0.1:%d/samples/upload?name=my%%20clap.wav" % self.http,
                                     data=body, method="POST")
        with urllib.request.urlopen(req, timeout=10) as r:
            out = json.loads(r.read())
        self.assertEqual(out["path"], "uploads/my clap.wav")
        self.assertEqual(open(os.path.join(self.lib, "uploads", "my clap.wav"), "rb").read(), body)
        req = urllib.request.Request("http://127.0.0.1:%d/samples/upload?name=evil.sh" % self.http,
                                     data=b"rm -rf /", method="POST")
        with self.assertRaises(urllib.error.HTTPError):
            urllib.request.urlopen(req, timeout=5)

    def test_s3_import_from_sd(self):
        code, out = self.post("/samples/import", {"path": "Kits"})
        self.assertEqual((code, out["copied"]), (200, 2))
        self.assertTrue(os.path.isfile(os.path.join(self.lib, "sdcard", "Kits", "808", "BD.wav")))
        self.assertFalse(os.path.exists(os.path.join(self.lib, "sdcard", "Kits", "readme.txt")))
        self.assertEqual(self.post("/samples/import", {"path": "Kits"})[1]["copied"], 0, "already there")

    def test_s4_send_through_a_lossy_link(self):
        sys.path.insert(0, HERE)
        import apcbox_bridge as b
        c = SseClient(self.http, path="/events?view=apcbox")
        code, out = self.post("/samples/send", {"src": "lib", "path": "drums/tone.wav", "track": 3})
        self.assertEqual(code, 200)
        t = self.wait_xfer(out["id"])
        self.assertEqual(t["state"], "done", t)
        self.assertGreater(t["resent"], 0, "the lossy link should have forced resends")
        got = self.daisy.files[str(out["id"])]
        want, _ = b.convert_for_daisy(os.path.join(self.lib, "drums", "tone.wav"))
        self.assertEqual(got["data"], want, "the Daisy got different bytes")
        self.assertEqual(got["track"], 3)
        self.assertTrue(c.wait_for(lambda c: any(e == "xfer" and d["state"] == "done" for e, d in c.named)))
        self.assertFalse(any(l.startswith("F") for l in c.lines), "F replies reached a browser")
        print("\n  lossy link: %d chunks, %d resent, %.1f s, %s" % (t["chunks"], t["resent"], t["seconds"], t["how"]))
        c.close()

    def test_s5_sd_file_24bit_stereo_converted(self):
        code, out = self.post("/samples/send", {"src": "sd", "path": "Kits/808/SD 24bit stereo.wav", "track": 5})
        t = self.wait_xfer(out["id"])
        self.assertEqual(t["state"], "done", t)
        data = self.daisy.files[str(out["id"])]["data"]
        if shutil.which("ffmpeg"):
            # mono, 16-bit, 48 kHz
            self.assertEqual(struct.unpack_from("<HHIIHH", data, 20), (1, 1, 48000, 96000, 2, 16))

    def test_s6_daisy_refuses(self):
        self.daisy.refuse = "too-big"
        code, out = self.post("/samples/send", {"src": "lib", "path": "drums/tone.wav", "track": 2})
        t = self.wait_xfer(out["id"])
        self.daisy.refuse = None
        self.assertEqual(t["state"], "failed")
        self.assertIn("too-big", t["error"])
        self.assertEqual(self.post("/samples/send", {"src": "lib", "path": "drums/tone.wav", "track": 17})[0], 400)



class HttpsTest(unittest.TestCase):
    """The HTTPS port (for the record button on phones)"""

    def test_https_serves_the_pages(self):
        import ssl as _ssl
        d = tempfile.mkdtemp()
        http, https = free_port(), free_port()
        proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(d, "nodaisy"), "--host", "127.0.0.1",
             "--http-port", str(http), "--https-port", str(https), "--ui", UI, "--no-midi",
             "--state", os.path.join(d, "state.json")],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, "bridge.log"), "w"))
        self.addCleanup(proc.kill)
        ctx = _ssl.create_default_context(cafile=None)
        body = None
        for _ in range(60):
            if os.path.exists(os.path.join(d, "tls.crt")):
                try:
                    ctx = _ssl.create_default_context(cafile=os.path.join(d, "tls.crt"))
                    with urllib.request.urlopen("https://127.0.0.1:%d/classic" % https, context=ctx, timeout=5) as r:
                        body = r.read()
                    break
                except (OSError, _ssl.SSLError):
                    pass
            time.sleep(0.2)
        self.assertIsNotNone(body, "no HTTPS (log: %s)" % open(os.path.join(d, "bridge.log")).read()[-500:])
        self.assertIn(b"APCBOX", body)
        caps = json.loads(urllib.request.urlopen("https://127.0.0.1:%d/caps" % https, context=ctx, timeout=5).read())
        self.assertEqual(caps["https_port"], https)
        self.assertEqual(oct(os.stat(os.path.join(d, "tls.key")).st_mode & 0o777), "0o600")




FAKE_ARECORD = """#!%s
# arecord for the tests: a 440 Hz tone as raw s16le 48 kHz stereo, in real time
import math, os, struct, sys, time
with open(%r, "a") as f:
    f.write(" ".join(sys.argv[1:]) + " | ALSA_CONFIG_PATH=" + os.environ.get("ALSA_CONFIG_PATH", "") + "\\n")
if os.path.exists(%r):
    sys.stderr.write("arecord: main:831: audio open error: Device or resource busy\\n")
    sys.exit(1)
out = sys.stdout.buffer
n = 0
t0 = time.monotonic()
while True:
    chunk = b"".join(struct.pack("<hh", int(12000 * math.sin(2 * math.pi * 440 * (n + i) / 48000)),
                                 int(6000 * math.sin(2 * math.pi * 440 * (n + i) / 48000))) for i in range(480))
    n += 480
    out.write(chunk)
    out.flush()
    d = t0 + n / 48000 - time.monotonic()
    if d > 0:
        time.sleep(d)
"""


class SettingsTest(unittest.TestCase):
    """The Settings page and the bridge's way to the settings helper (t95/helper), with fake system tools"""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, os.path.join(REPO, "t95", "helper"))
        import test_helper
        cls.dir = tempfile.mkdtemp()
        cls.netplan = os.path.join(cls.dir, "netplan")
        os.makedirs(cls.netplan)
        tools = {}
        for t in ("wpa_cli", "netplan", "hostnamectl", "systemctl", "bluetoothctl"):
            os.makedirs(os.path.join(cls.dir, "bin"), exist_ok=True)
            tools[t] = os.path.join(cls.dir, "bin", t)
            with open(tools[t], "w") as f:
                f.write(test_helper.FAKE)
            os.chmod(tools[t], 0o755)
        cls.sock = os.path.join(cls.dir, "h.sock")
        cls.calls = os.path.join(cls.dir, "calls.log")
        cls.wpa = test_helper.FakeWpa(os.path.join(cls.dir, "wpa"))
        cls.helper = subprocess.Popen(
            [sys.executable, os.path.join(REPO, "t95", "helper", "ghettorack_helper.py"), "--socket", cls.sock,
             "--state-dir", os.path.join(cls.dir, "hstate"), "--netplan-dir", cls.netplan, "--trial-s", "6", "--select-s", "0.05", "--connect-s", "6", "--scan-s", "0.05",
             "--wpa-ctrl", os.path.join(cls.dir, "wpa"),
             "--attach", os.path.join(cls.dir, "hciattach_opi"), "--group", "no-such-group", "--wpa-cli", tools["wpa_cli"],
             "--netplan", tools["netplan"], "--hostnamectl", tools["hostnamectl"], "--systemctl", tools["systemctl"],
             "--bluetoothctl", tools["bluetoothctl"]],
            env=dict(os.environ, FAKELOG=cls.calls, FAKEDIR=cls.dir), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.helper.kill)
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1", "--https-port", "0",
             "--rtp-port", "0", "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", os.path.join(cls.dir, "state.json"),
             "--helper-socket", cls.sock],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.addClassCleanup(cls.proc.kill)
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: True, 10)

    def post(self, path, body):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (self.http, path), data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def state(self):
        return json.loads(BridgeTest.get("/sys/state")[2])

    def test_s1_state_and_commands_pass_through(self):
        st = self.state()
        self.assertEqual(st["wifi"]["ssid"], "Home Net")
        self.assertEqual((st["pin_set"], st["bridge"]["daisy"], st["bridge"]["controllers"]), (False, False, []))
        code, out = self.post("/sys/do", {"cmd": "scan"})
        self.assertEqual((code, [n["ssid"] for n in out["networks"]]), (200, ["Home Net", "Cafe Open"]))
        self.assertEqual(self.post("/sys/do", {"cmd": "status; reboot"})[0], 400)
        self.assertEqual(self.post("/sys/do", {"cmd": "handle"})[0], 400)
        self.assertEqual(self.post("/sys/do", [1])[0], 400)
        code, out = self.post("/sys/do", {"cmd": "reboot"})
        self.assertEqual((code, out["error"]), (400, "set a PIN first"))
        self.assertEqual(self.post("/sys/do", {"cmd": "pin_set", "new": "2468"}), (200, {"ok": True}))
        code, out = self.post("/sys/do", {"cmd": "hostname", "name": "rack", "pin": "0000"})
        self.assertEqual((code, out["error"]), (400, "wrong PIN"))
        self.assertEqual(self.post("/sys/do", {"cmd": "hostname", "name": "rack", "pin": "2468"}), (200, {"ok": True}))
        self.assertIn("hostnamectl set-hostname rack", open(self.calls).read())

    def test_s2_secrets_only_over_https_or_from_the_box(self):
        import asyncio
        from rackbridge.web import WebMixin

        class B(WebMixin):
            args = type("A", (), {"helper_socket": self.sock})
        b = B()
        for body in ({"cmd": "pin_check", "pin": "2468"}, {"cmd": "join", "ssid": "X", "psk": "x" * 8},
                     {"cmd": "pin_set", "new": "1111"}):
            code, out = asyncio.run(b.sys_do(json.dumps(body).encode(), False))
            self.assertEqual((code, out.get("https")), (400, True), body)
        self.assertEqual(asyncio.run(b.sys_do(b'{"cmd": "scan"}', False))[0], 200)       # looking is free
        self.assertEqual(asyncio.run(b.sys_do(b'{"cmd": "pin_check", "pin": "2468"}', True))[0], 200)
        # no helper on the box: said plainly, nothing breaks
        b.args = type("A", (), {"helper_socket": os.path.join(self.dir, "nope.sock")})
        code, out = asyncio.run(b.sys_do(b'{"cmd": "scan"}', True))
        self.assertEqual((code, out["helper"]), (503, False))

    def test_s3_the_page(self):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        url = "http://127.0.0.1:%d/settings" % self.http
        self.assertIn(b"SETTINGS", BridgeTest.get("/settings")[2])
        self.assertIn(b'href="/settings"', BridgeTest.get("/")[2])
        with sync_playwright() as pw:
            b = pw.chromium.launch()
            errors = []
            for w_, h_ in ((1920, 1080), (390, 664), (844, 390)):
                pg = b.new_page(viewport={"width": w_, "height": h_})
                pg.on("pageerror", lambda e: errors.append(str(e)))
                for s_ in ("net", "bt", "sys", "dev"):
                    pg.goto(url + "?s=" + s_)
                    pg.wait_for_function("document.getElementById('c-helper').textContent === 'helper: running'")
                    self.assertEqual(pg.evaluate("[document.documentElement.scrollHeight > innerHeight + 1, document.documentElement.scrollWidth > innerWidth + 1]"),
                                     [False, False], (w_, h_, s_))
                    out = pg.evaluate("""() => [...document.querySelectorAll('section.on button, section.on input, nav button')].filter(e => {
                        const r = e.getBoundingClientRect(); return r.width && (r.right > innerWidth + 1 || r.bottom > innerHeight + 1 || r.left < -1); }).map(e => e.id || e.textContent)""")
                    self.assertEqual(out, [], (w_, h_, s_))
                pg.goto(url)
                pg.wait_for_function("document.getElementById('n-now').textContent === 'Home Net'")
                pg.click("#n-scan")
                pg.wait_for_selector(".net.cur")
                self.assertGreaterEqual(pg.locator(".net").count(), 2 if h_ > 400 else 1)
                # joining needs the PIN: without it, a hint and nothing sent
                while not pg.locator(".net", has_text="Cafe Open").count():
                    pg.click("#n-next")
                pg.locator(".net", has_text="Cafe Open").locator("button:has-text('JOIN')").click()
                pg.click("#n-go")
                pg.wait_for_function("document.getElementById('n-banner').textContent.includes('PIN')")
                self.assertFalse(os.path.exists(os.path.join(self.netplan, "40-ghettorack-wifi.yaml")))
                pg.fill("#pin", "2468")
                pg.wait_for_function("document.getElementById('c-pin').textContent === 'unlocked'")
                # the box can't get onto it (as with a wrong password): back within seconds, said plainly, nothing saved,
                # no stale "saved" row with a FORGET that can't work, and JOIN works again at once
                self.wpa.mode = "wrong"
                t0 = time.time()
                pg.click("#n-go")
                pg.wait_for_function("document.getElementById('n-banner').textContent.includes('wrong password')", timeout=8000)
                self.assertLess(time.time() - t0, 5)
                self.assertIn("went back to Home Net", pg.inner_text("#n-banner"))
                self.assertFalse(os.path.exists(os.path.join(self.netplan, "40-ghettorack-wifi.yaml")))
                self.assertEqual(self.wpa.names(), ["Home Net"])
                pg.wait_for_selector(".net.cur")
                while not pg.locator(".net", has_text="Cafe Open").count():
                    pg.click("#n-next")
                row = pg.locator(".net", has_text="Cafe Open")
                self.assertEqual((row.locator("button:has-text('FORGET')").count(), row.locator("button:has-text('JOIN')").is_enabled()), (0, True))
                # second try gets on: the page confirms by itself and it is saved; then FORGET takes it out again
                self.wpa.mode = "ok"
                row.locator("button:has-text('JOIN')").click()
                pg.click("#n-go")
                pg.wait_for_function("document.getElementById('n-banner').textContent === 'Cafe Open works and is saved.'", timeout=8000)
                self.assertTrue(os.path.exists(os.path.join(self.netplan, "40-ghettorack-wifi.yaml")))
                self.assertEqual(self.state()["trial"], None)
                while not pg.locator(".net", has_text="Cafe Open").locator("button:has-text('FORGET')").count():
                    if pg.locator("#n-next").is_enabled():
                        pg.click("#n-next")
                    time.sleep(0.1)
                f = pg.locator(".net", has_text="Cafe Open").locator("button", has_text=re.compile("FORGET|SURE"))
                f.click()
                f.click()
                pg.wait_for_function("document.getElementById('n-banner').textContent === 'Forgot Cafe Open.'")
                self.assertFalse(os.path.exists(os.path.join(self.netplan, "40-ghettorack-wifi.yaml")))
                self.assertEqual(self.wpa.names(), ["Home Net"])
                # Bluetooth: not set up, said how; System: reboot asks twice
                pg.click("nav button:has-text('Bluetooth')")
                pg.wait_for_function("document.getElementById('b-text').textContent.includes('get-bt-tool.sh')")
                self.assertTrue(pg.locator("#b-toggle").is_disabled())
                pg.click("nav button:has-text('System')")
                n0 = open(self.calls).read().count("systemctl reboot")
                pg.click("#y-reboot")
                pg.wait_for_function("document.getElementById('y-reboot').textContent === 'SURE?'")
                time.sleep(1.5)
                self.assertEqual(open(self.calls).read().count("systemctl reboot"), n0)
                pg.click("#y-reboot")
                pg.wait_for_function("document.getElementById('y-banner').textContent.startsWith('Rebooting')")
                end = time.time() + 5
                while open(self.calls).read().count("systemctl reboot") == n0 and time.time() < end:
                    time.sleep(0.1)
                self.assertEqual(open(self.calls).read().count("systemctl reboot"), n0 + 1)
                pg.close()
            b.close()
        self.assertEqual(errors, [])


class ListenTest(unittest.TestCase):
    """The Daisy's audio to browsers: /audio.pcm from a fake arecord, the page's Listen button"""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.asound = os.path.join(cls.dir, "asound")
        os.makedirs(os.path.join(cls.asound, "HDMI"))
        cls.alog = os.path.join(cls.dir, "arecord.log")
        cls.busy = os.path.join(cls.dir, "busy")
        cls.arecord = os.path.join(cls.dir, "arecord")
        with open(cls.arecord, "w") as f:
            f.write(FAKE_ARECORD % (sys.executable, cls.alog, cls.busy))
        os.chmod(cls.arecord, 0o755)
        # alsaloop that can't open the shared device, like a box where dsnoop doesn't work
        cls.loop_log = os.path.join(cls.dir, "alsaloop.log")
        cls.alsaloop = os.path.join(cls.dir, "alsaloop")
        with open(cls.alsaloop, "w") as f:
            f.write("#!/bin/sh\necho \"$*\" >> %s\ncase \"$*\" in *daisyshare*) exit 1;; esac\nexec sleep 300\n" % cls.loop_log)
        os.chmod(cls.alsaloop, 0o755)
        cls.http = free_port()
        cls.proc = subprocess.Popen(
            [sys.executable, BRIDGE, "--port", os.path.join(cls.dir, "nodaisy"), "--host", "127.0.0.1", "--https-port", "0",
             "--rtp-port", "0", "--http-port", str(cls.http), "--ui", UI, "--no-midi", "--state", os.path.join(cls.dir, "state.json"),
             "--arecord", cls.arecord, "--alsaloop", cls.alsaloop, "--asound", cls.asound],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(cls.dir, "bridge.log"), "w"))
        cls.addClassCleanup(cls.proc.kill)
        BridgeTest.http = cls.http
        BridgeTest.wait_health(lambda h: "listen" in h, 10)

    def setUp(self):
        BridgeTest.http = self.http

    def stream(self):
        s = socket.create_connection(("127.0.0.1", self.http), timeout=5)
        s.sendall(b"GET /audio.pcm HTTP/1.1\r\nHost: x\r\n\r\n")
        return s

    @staticmethod
    def read_for(s, secs):
        buf = b""
        end = time.time() + secs
        s.settimeout(0.2)
        while time.time() < end:
            try:
                d = s.recv(65536)
                if not d:
                    break
                buf += d
            except socket.timeout:
                pass
        return buf

    def test_l1_no_daisy_audio_says_so(self):
        with self.assertRaises(urllib.error.HTTPError) as e:
            BridgeTest.get("/audio.pcm")
        self.assertEqual(e.exception.code, 503)
        self.assertIn(b"Daisy's audio isn't on the T95", e.exception.read())

    def test_l2_stream_is_the_daisys_audio_and_stops_with_the_last_listener(self):
        os.makedirs(os.path.join(self.asound, "ApcBox"), exist_ok=True)
        s = self.stream()
        raw = self.read_for(s, 1.5)
        head, _, pcm = raw.partition(b"\r\n\r\n")
        self.assertIn(b"200 OK", head)
        self.assertIn(b"X-Audio: s16le 48000 2", head)
        secs = len(pcm) / 192000
        print("\n  listen: %.2f s of audio in 1.5 s" % secs)
        self.assertTrue(1.0 < secs < 1.8, secs)
        n = len(pcm) // 4
        left = struct.unpack("<%dh" % (2 * n), pcm[:4 * n])[0::2]
        self.assertGreater(max(left), 11000)          # the tone, not silence or garbage
        self.assertLess(max(left), 12100)
        # rising zero crossings: 440 a second
        zc = sum(1 for a, b in zip(left, left[1:]) if a < 0 <= b)
        self.assertAlmostEqual(zc / (n / 48000), 440, delta=8)
        log = open(self.alog).read()
        self.assertIn("-D plug:daisyshare -f S16_LE -r 48000 -c 2 -t raw", log)
        conf = log.strip().rsplit("ALSA_CONFIG_PATH=", 1)[1]
        self.assertIn('pcm "hw:CARD=ApcBox,DEV=0"', open(conf).read())
        h = BridgeTest.health()["listen"]
        self.assertEqual((h["clients"], h["running"]), (1, True))
        # a second listener shares the one capture
        s2 = self.stream()
        self.assertGreater(len(self.read_for(s2, 0.6)), 60000)
        self.assertEqual(BridgeTest.health()["listen"]["starts"], 1)
        s.close()
        s2.close()
        BridgeTest.wait_health(lambda h: not h["listen"]["running"] and h["listen"]["clients"] == 0, 6)

    def test_l3_a_stalled_listener_loses_old_audio_not_the_bridge(self):
        os.makedirs(os.path.join(self.asound, "ApcBox"), exist_ok=True)
        slow = socket.socket()
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)     # before connecting, so it takes effect
        slow.connect(("127.0.0.1", self.http))
        slow.sendall(b"GET /audio.pcm HTTP/1.1\r\nHost: x\r\n\r\n")
        fast = self.stream()
        self.addCleanup(slow.close)
        self.addCleanup(fast.close)
        got = self.read_for(fast, 4.0)                 # the slow one reads nothing for 4 s
        self.assertGreater(len(got), 192000 * 3)       # the fast one is unaffected
        t = time.time()
        h = BridgeTest.health()
        self.assertLess(time.time() - t, 1.0)
        self.assertGreater(h["listen"]["dropped_bytes"], 0)
        slow.close()
        fast.close()
        BridgeTest.wait_health(lambda h: h["listen"]["clients"] == 0, 6)

    def test_l4_too_many_and_capture_failing(self):
        os.makedirs(os.path.join(self.asound, "ApcBox"), exist_ok=True)
        ss = [self.stream() for _ in range(4)]
        for x in ss:
            self.assertIn(b"200 OK", self.read_for(x, 0.3)[:20])
        with self.assertRaises(urllib.error.HTTPError) as e:
            BridgeTest.get("/audio.pcm")
        self.assertEqual(e.exception.code, 503)
        for x in ss:
            x.close()
        BridgeTest.wait_health(lambda h: h["listen"]["clients"] == 0, 6)
        open(self.busy, "w").close()                   # the device is busy: arecord exits at once
        try:
            s = self.stream()
            raw = self.read_for(s, 1.5)
            s.close()
            self.assertLess(len(raw), 500)
            BridgeTest.wait_health(lambda h: "resource busy" in (h["listen"]["error"] or ""), 5)
        finally:
            os.unlink(self.busy)

    def test_l5_listen_button_in_the_page(self):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            self.skipTest("playwright not installed")
        os.makedirs(os.path.join(self.asound, "ApcBox"), exist_ok=True)
        with sync_playwright() as pw:
            b = pw.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
            pg = b.new_page(viewport={"width": 390, "height": 664})
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto("http://127.0.0.1:%d/apcbox" % self.http)
            pg.wait_for_selector(".tb.listen")
            pg.click(".tb.listen")
            pg.wait_for_function("Listen.state === 'on' && Listen.got > 48000 && Listen.played > 20000", timeout=8000)
            st = pg.evaluate("({got: Listen.got, played: Listen.played, under: Listen.under, buffered: Listen.buffered(), rate: Listen.ctx.sampleRate})")
            print("\n  page listening: %s" % st)
            self.assertLess(st["buffered"], 48000 * 0.5)       # stays near live
            self.assertEqual(pg.inner_text(".tb.listen"), "LISTENING")
            pg.click(".tb.listen")
            pg.wait_for_function("Listen.state === 'off'")
            b.close()
        self.assertEqual(errors, [])
        BridgeTest.wait_health(lambda h: h["listen"]["clients"] == 0 and not h["listen"]["running"], 6)

    def test_l6_tv_sound_falls_back_when_sharing_fails(self):
        os.makedirs(os.path.join(self.asound, "ApcBox"), exist_ok=True)
        post = lambda body: RouterTest.post(self, "/settings", body)
        self.assertEqual(post({"hdmi_audio": True})[0], 200)
        BridgeTest.wait_health(lambda h: h["hdmi_audio"]["running"] and not h["listen"]["shared"], 12)
        time.sleep(0.5)
        lines = open(self.loop_log).read().strip().splitlines()
        self.assertIn("plug:daisyshare", lines[0])
        self.assertIn("-C plughw:CARD=ApcBox", lines[-1])
        # the TV sound has the card to itself now: a browser is told why it can't listen
        with self.assertRaises(urllib.error.HTTPError) as e:
            BridgeTest.get("/audio.pcm")
        self.assertIn(b"turn it off to listen here", e.exception.read())
        post({"hdmi_audio": False})
        BridgeTest.wait_health(lambda h: not h["hdmi_audio"]["running"], 6)
        s = self.stream()
        self.assertIn(b"200 OK", self.read_for(s, 0.5)[:20])
        self.assertIn("-D plughw:CARD=ApcBox", open(self.alog).read().strip().splitlines()[-1])
        s.close()

class HdmiAudioTest(unittest.TestCase):
    """The HDMI audio setting starts/stops alsaloop (a fake one here)"""

    def test_toggle_persist_and_follow_the_daisy(self):
        d = tempfile.mkdtemp()
        asound = os.path.join(d, "asound")
        os.makedirs(os.path.join(asound, "HDMI"))
        log = os.path.join(d, "alsaloop.log")
        fake = os.path.join(d, "alsaloop")
        with open(fake, "w") as f:
            f.write("#!/bin/sh\necho \"$*\" >> %s\nexec sleep 300\n" % log)
        os.chmod(fake, 0o755)
        http = free_port()
        state = os.path.join(d, "state.json")

        def start():
            p = subprocess.Popen(
                [sys.executable, BRIDGE, "--port", os.path.join(d, "nodaisy"), "--host", "127.0.0.1",
                 "--https-port", "0", "--http-port", str(http), "--ui", UI, "--no-midi", "--state", state,
                 "--alsaloop", fake, "--asound", asound],
                stdout=subprocess.DEVNULL, stderr=open(os.path.join(d, "bridge.log"), "a"))
            BridgeTest.http = http
            BridgeTest.wait_health(lambda h: "hdmi_audio" in h, 10)
            return p

        def running():
            return BridgeTest.health()["hdmi_audio"]["running"]

        proc = start()
        self.addCleanup(lambda: proc.kill())
        post = lambda body: IrTest.post(type("x", (), {"http": http})(), "/settings", body)
        self.assertEqual(post({"hdmi_audio": True})[0], 200)
        time.sleep(2.5)
        self.assertFalse(running(), "no Daisy card yet: nothing to pass on")
        self.assertEqual(BridgeTest.health()["hdmi_audio"]["daisy_card"], False)
        os.makedirs(os.path.join(asound, "ApcBox"))          # the Daisy plugged in
        BridgeTest.wait_health(lambda h: h["hdmi_audio"]["running"], 6)
        self.assertIn("-C plug:daisyshare -P plughw:CARD=HDMI -t 40000 -S 1", open(log).read())   # shared with listening
        os.rmdir(os.path.join(asound, "ApcBox"))             # unplugged: stops
        BridgeTest.wait_health(lambda h: not h["hdmi_audio"]["running"], 6)
        os.makedirs(os.path.join(asound, "ApcBox"))          # back: starts again
        BridgeTest.wait_health(lambda h: h["hdmi_audio"]["running"] and h["hdmi_audio"]["starts"] == 2, 6)
        proc.terminate(); proc.wait(5)
        proc = start()                                        # setting survives a restart
        BridgeTest.wait_health(lambda h: h["hdmi_audio"]["on"] and h["hdmi_audio"]["running"], 6)
        self.assertEqual(post({"hdmi_audio": False})[0], 200)
        BridgeTest.wait_health(lambda h: not h["hdmi_audio"]["running"], 6)
        self.assertEqual(post({"hdmi_audio": "yes"})[0], 400)
        subprocess.run(["pkill", "-f", fake])


if __name__ == "__main__":
    unittest.main(verbosity=2)
