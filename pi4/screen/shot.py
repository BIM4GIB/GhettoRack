#!/usr/bin/env python3
"""Screenshot of the kiosk's page through Chromium's DevTools on 127.0.0.1:9222.
   python3 shot.py out.png        also prints the window size and device pixel ratio"""
import base64, json, os, socket, struct, sys, urllib.request

tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
tab = next(t for t in tabs if t["type"] == "page")
path = tab["webSocketDebuggerUrl"].split("9222", 1)[1]
s = socket.create_connection(("127.0.0.1", 9222))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1:9222\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, key)).encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]


def send(obj):
    d = json.dumps(obj).encode()
    mask = os.urandom(4)
    n = len(d)
    hdr = b"\x81" + (bytes([0x80 | n]) if n < 126 else b"\xfe" + struct.pack(">H", n))
    s.sendall(hdr + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(d)))


def need(n):
    global buf
    while len(buf) < n:
        c = s.recv(65536)
        if not c:
            raise EOFError
        buf += c
    out, buf = buf[:n], buf[n:]
    return out


def recv():
    msg = b""
    while True:
        b0, b1 = need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", need(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", need(8))[0]
        msg += need(n)
        if b0 & 0x80:
            return msg


def call(i, method, params=None):
    send({"id": i, "method": method, "params": params or {}})
    while True:
        m = json.loads(recv())
        if m.get("id") == i:
            return m.get("result", m)


r = call(1, "Runtime.evaluate", {"expression": "JSON.stringify([innerWidth, innerHeight, devicePixelRatio, location.href])"})
print(r["result"]["value"])
r = call(2, "Page.captureScreenshot", {"format": "png"})
open(sys.argv[1], "wb").write(base64.b64decode(r["data"]))
