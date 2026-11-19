#!/usr/bin/env python3
"""Crops a phone screenshot (adb exec-out screencap -p) to the centred 16:9 game view and
scales it to 1280x720, without dependencies:  crop-shot.py in.png out.png"""
import struct, sys, zlib

def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', 0
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype in (2, 6), 'only 8-bit RGB/RGBA'
            bpp = 3 if ctype == 2 else 4
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows

def write_png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    c = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + c(b'IDAT', zlib.compress(raw, 9)) + c(b'IEND', b''))

w, h, bpp, rows = read_png(sys.argv[1])
vw = h * 16 // 9
x0 = (w - vw) // 2
OW, OH = 1280, 720
out = []
for oy in range(OH):   # box filter
    y0, y1 = oy * h // OH, max(oy * h // OH + 1, (oy + 1) * h // OH)
    line = bytearray()
    for ox in range(OW):
        xa, xb = x0 + ox * vw // OW, x0 + max(ox * vw // OW + 1, (ox + 1) * vw // OW)
        s = [0, 0, 0]
        n = 0
        for y in range(y0, y1):
            r = rows[y]
            for x in range(xa, xb):
                s[0] += r[x * bpp]; s[1] += r[x * bpp + 1]; s[2] += r[x * bpp + 2]
                n += 1
        line += bytes(v // n for v in s)
    out.append(line)
write_png(sys.argv[2], OW, OH, out)
