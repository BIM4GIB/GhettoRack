#!/usr/bin/env python3
"""What chrome://gpu would say, for the kiosk's Chromium (no screen needed).

    python3 t95/kiosk/gpuinfo.py

Talks to Chromium's DevTools on 127.0.0.1:9222 (kiosk.service opens it,
local only) and prints the GPU feature status: whether compositing and
rasterization run on the Mali (panfrost) or in software. Standard library
only: a minimal WebSocket client, enough for one request.
"""
import base64
import json
import os
import socket
import sys
import urllib.request

PORT = 9222


def ws_call(url, method):
    host, path = url[len("ws://"):].split("/", 1)
    h, p = host.split(":")
    s = socket.create_connection((h, int(p)), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, key)).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += s.recv(1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        sys.exit("DevTools refused the connection: " + head.split(b"\r\n")[0].decode())
    msg = json.dumps({"id": 1, "method": method}).encode()
    mask = os.urandom(4)
    n = len(msg)
    hdr = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + n.to_bytes(2, "big"))
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(msg)))
    data = b""
    while True:                 # one (possibly fragmented) text reply
        b0, b1 = s.recv(1)[0], s.recv(1)[0]
        ln = b1 & 0x7F
        if ln == 126:
            ln = int.from_bytes(recv_n(s, 2), "big")
        elif ln == 127:
            ln = int.from_bytes(recv_n(s, 8), "big")
        data += recv_n(s, ln)
        if b0 & 0x80:
            break
    s.close()
    return json.loads(data)


def recv_n(s, n):
    out = b""
    while len(out) < n:
        c = s.recv(n - len(out))
        if not c:
            raise ConnectionError("closed")
        out += c
    return out


def main():
    try:
        ver = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/version" % PORT, timeout=5))
    except OSError as e:
        sys.exit("no DevTools on 127.0.0.1:%d (kiosk running? kiosk.service from this repo?): %s" % (PORT, e))
    print("browser:", ver.get("Browser"))
    r = ws_call(ver["webSocketDebuggerUrl"], "SystemInfo.getInfo").get("result", {})
    gpu = r.get("gpu", {})
    for d in gpu.get("devices", []):
        print("device: %s %s" % (d.get("vendorString", ""), d.get("deviceString", "")))
    for k in ("driverBugWorkarounds",):
        pass
    print("feature status:")
    for k, v in sorted(gpu.get("featureStatus", {}).items()):
        print("  %-28s %s" % (k, v))
    aux = gpu.get("auxAttributes", {})
    for k in ("glRenderer", "glVersion", "glImplementationParts", "displayType"):
        if k in aux:
            print("%s: %s" % (k, aux[k]))


if __name__ == "__main__":
    main()
