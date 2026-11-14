#!/usr/bin/env python3
"""mkrle.py in.bmp out.bmp - rewrites an uncompressed 4 or 8 bit BMP with
RLE compression (runs, literal sequences, and a "delta" escape over the
blank start of the first row to exercise every RLE feature)."""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', data, 10)[0]
hsize, w, h, planes, bpp, comp = struct.unpack_from('<IiiHHI', data, 14)
assert comp == 0 and bpp in (4, 8)
rowbytes = (w * bpp + 31) // 32 * 4
out = bytearray()
for y in range(h):
    raw = data[off + y * rowbytes: off + (y + 1) * rowbytes]
    px = list(raw[:w]) if bpp == 8 else [(raw[i // 2] >> (0 if i & 1 else 4)) & 15 for i in range(w)]
    x = 0
    if y == 0:                      # skip leading zero pixels with a delta
        while x < w and px[x] == 0 and x < 200:
            x += 1
        if x:
            out += bytes([0, 2, x, 0])
    while x < w:
        run = 1
        while x + run < w and px[x + run] == px[x] and run < 255:
            run += 1
        if run >= 3:
            out += bytes([run, px[x] if bpp == 8 else px[x] * 17])
            x += run
            continue
        n = 0                       # literal sequence, until the next long run
        while x + n < w and n < 255:
            if x + n + 2 < w and px[x + n] == px[x + n + 1] == px[x + n + 2]:
                break
            n += 1
        if n < 3:                   # too short for absolute mode
            for i in range(n):
                out += bytes([1, px[x + i] if bpp == 8 else px[x + i] * 17])
        else:
            out += bytes([0, n])
            if bpp == 8:
                lit = bytes(px[x:x + n])
            else:
                seq = px[x:x + n] + [0]
                lit = bytes((seq[i] << 4) | seq[i + 1] for i in range(0, n, 2))
            out += lit + (b'\0' if len(lit) & 1 else b'')
        x += n
    out += bytes([0, 0])
out += bytes([0, 1])
head = bytearray(data[:off])
struct.pack_into('<I', head, 30, 1 if bpp == 8 else 2)
struct.pack_into('<I', head, 34, len(out))
struct.pack_into('<I', head, 2, off + len(out))
open(sys.argv[2], 'wb').write(head + out)
