#!/usr/bin/env python3
"""Colour bars on /dev/fb0: red green blue white black yellow, left to right;
a checkered strip on the top and left edges, a white 1-pixel frame around all."""
import struct
w, h = [int(x) for x in open("/sys/class/graphics/fb0/virtual_size").read().split(",")]
stride = int(open("/sys/class/graphics/fb0/stride").read())
cols = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0), (255, 255, 0)]
px = lambda r, g, b: struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
buf = bytearray()
for y in range(h):
    row = bytearray()
    for x in range(w):
        c = cols[x * len(cols) // w]
        if y < 12 or x < 12:
            c = (255, 255, 255) if (x // 4 + y // 4) % 2 else (0, 0, 0)
        if x in (0, w - 1) or y in (0, h - 1):
            c = (255, 255, 255)
        row += px(*c)
    buf += row + bytes(stride - len(row))
open("/dev/fb0", "wb").write(buf)
print("drawn", w, h)
