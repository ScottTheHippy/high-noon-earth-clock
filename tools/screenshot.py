#!/usr/bin/env python3
"""Grab a screenshot from a running High Noon Earth Clock over USB serial.

usage: screenshot.py PORT OUT.png        (needs: pip install pyserial numpy pillow)

The clock answers the serial command "S" with the frame it is showing: the
4-byte tag "SHOT", a 32-bit byte count, then 466x466 little-endian RGB565
pixels in the panel's native orientation. This rotates it upright and masks
the square to the round display.
"""
import sys, time
import numpy as np
import serial
from PIL import Image

SCR = 466
port, out = sys.argv[1:3]
total = SCR * SCR * 2

with serial.Serial(port, 115200, timeout=2) as s:
    for attempt in range(5):
        s.reset_input_buffer()
        s.write(b"S\n")
        buf = b""
        deadline = time.time() + 8
        while time.time() < deadline:
            buf += s.read(65536)
            i = buf.find(b"SHOT")
            if i >= 0 and len(buf) >= i + 8 + total:
                data = buf[i + 8:i + 8 + total]
                break
        else:
            continue
        break
    else:
        sys.exit("no complete frame received")

px = np.frombuffer(data, dtype="<u2").reshape(SCR, SCR)
r = ((px >> 11) & 31) * 255 // 31
g = ((px >> 5) & 63) * 255 // 63
b = (px & 31) * 255 // 31
native = np.dstack([r, g, b]).astype(np.uint8)
upright = np.rot90(native, 1)  # undo the panel-mount rotation done in the firmware

yy, xx = np.mgrid[0:SCR, 0:SCR]
inside = (xx + 0.5 - SCR / 2) ** 2 + (yy + 0.5 - SCR / 2) ** 2 <= (SCR / 2) ** 2
alpha = np.where(inside, 255, 0).astype(np.uint8)
Image.fromarray(np.dstack([upright, alpha]), "RGBA").save(out)
print("saved", out)
