#!/usr/bin/env python3
"""Bridge tests against a fake Daisy on a pseudo-terminal.

    python3 t95/bridge/test_bridge.py        (Linux; about 30 s)

Covers: echo off at open, ESC R on connect and when the Daisy goes quiet,
lines split across reads, garbage and over-long lines, the snapshot a new
browser gets, several browsers, a browser that stops reading while lines
pour in, too many browsers, and the Daisy unplugging and coming back.
"""
import json
import os
import socket
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

    def __init__(self, port, rcvbuf=None, read=True):
        self.sock = socket.socket()
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.connect(("127.0.0.1", port))
        self.sock.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\n\r\n")
        self.lines, self.events, self.status = [], [], None
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
                        else:
                            self.lines.append(data[0])
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
             "--http-port", str(cls.http), "--ui", UI],
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
        self.daisy.send("V 7\nL back again\n")
        self.assertTrue(c.wait_for(lambda c: "L back again" in c.lines))
        self.assertTrue(c.events[-1]["daisy"])
        self.assertFalse(c.closed, "browser dropped across a Daisy replug")
        c.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
