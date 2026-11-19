#!/usr/bin/env python3
"""Converts a binary PPM (P6) to PNG without dependencies: ppm2png.py in.ppm out.png"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
parts = d.split(maxsplit=4)
w, h = int(parts[1]), int(parts[2])
p = d[len(d) - w * h * 3:]
raw = b''.join(b'\0' + p[y * w * 3:(y + 1) * w * 3] for y in range(h))
c = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + c(b'IDAT', zlib.compress(raw)) + c(b'IEND', b''))
