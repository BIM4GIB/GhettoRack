#!/usr/bin/env python3
"""The kiosk's page through Chromium's DevTools on 127.0.0.1:9222.
   python3 shot.py out.png        a screenshot; also prints the window size and the page
   Page() is used by calibrate.py too."""
import base64, json, os, socket, struct, sys, urllib.request


class Page:
    def __init__(self):
        tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
        tab = next(t for t in tabs if t["type"] == "page")
        path = tab["webSocketDebuggerUrl"].split("9222", 1)[1]
        self.s = socket.create_connection(("127.0.0.1", 9222))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1:9222\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, key)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self.n = 0

    def _send(self, obj):
        d = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(d)
        hdr = b"\x81" + (bytes([0x80 | n]) if n < 126 else b"\xfe" + struct.pack(">H", n) if n < 65536 else b"\xff" + struct.pack(">Q", n))
        self.s.sendall(hdr + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(d)))

    def _need(self, n):
        while len(self.buf) < n:
            c = self.s.recv(65536)
            if not c:
                raise EOFError
            self.buf += c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv(self):
        msg = b""
        while True:
            b0, b1 = self._need(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._need(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._need(8))[0]
            msg += self._need(n)
            if b0 & 0x80:
                return msg

    def call(self, method, params=None):
        self.n += 1
        self._send({"id": self.n, "method": method, "params": params or {}})
        while True:
            m = json.loads(self._recv())
            if m.get("id") == self.n:
                return m.get("result", m)

    def eval(self, js):
        """The value of a JavaScript expression in the page"""
        return self.call("Runtime.evaluate", {"expression": js, "returnByValue": True}).get("result", {}).get("value")

    def screenshot(self, path):
        open(path, "wb").write(base64.b64decode(self.call("Page.captureScreenshot", {"format": "png"})["data"]))


if __name__ == "__main__":
    p = Page()
    print(p.eval("JSON.stringify([innerWidth, innerHeight, devicePixelRatio, location.href])"))
    p.screenshot(sys.argv[1])
