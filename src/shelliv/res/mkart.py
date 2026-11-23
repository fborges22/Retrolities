#!/usr/bin/env python3
"""Generates ShellIV's artwork (python3 src/shelliv/res/mkart.py).

Writes, next to this script:
  *.ico            32x32 16-colour icons (application and builtin item icons)
  glyphs.bmp       strip of 16x16 glyphs (sidebar, details view, toolbar)
  glyphsm.bmp      its transparency mask (white = transparent)
  drag*.cur        drag and drop cursors
The images are drawn with a few raster primitives in the 16 VGA colours,
shaded and outlined in the style of Windows 3.x icons.  The generated files
are committed; rerun this script after changing the art.
"""
import math
import os
import struct

OUT = os.path.dirname(os.path.abspath(__file__))

VGA = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0),
       (0, 0, 128), (128, 0, 128), (0, 128, 128), (128, 128, 128),
       (192, 192, 192), (255, 0, 0), (0, 255, 0), (255, 255, 0),
       (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
K, DRED, DGRN, OLIVE, NAVY, PURP, TEAL, GRAY, SILV, RED, LIME, YEL, BLUE, MAG, CYAN, WHT = range(16)
T = None    # transparent


class Canvas:
    def __init__(self, n):
        self.n = n
        self.p = [[T] * n for _ in range(n)]

    def set(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.p[y][x] = c

    def get(self, x, y):
        if 0 <= x < self.n and 0 <= y < self.n:
            return self.p[y][x]
        return T

    def rect(self, x0, y0, x1, y1, c):           # inclusive
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        for x in range(x0, x1 + 1):
            self.set(x, y0, c)
            self.set(x, y1, c)
        for y in range(y0, y1 + 1):
            self.set(x0, y, c)
            self.set(x1, y, c)

    def dither(self, x0, y0, x1, y1, a, b, frac=None):
        """Vertical blend from a to b with an ordered dither."""
        bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
        h = max(1, y1 - y0)
        for y in range(y0, y1 + 1):
            t = (y - y0) / h if frac is None else frac
            for x in range(x0, x1 + 1):
                self.set(x, y, b if t * 16 > bayer[y % 4][x % 4] + 0.5 else a)

    def line(self, x0, y0, x1, y1, c, w=1):
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(n + 1):
            x = round(x0 + (x1 - x0) * i / n)
            y = round(y0 + (y1 - y0) * i / n)
            for dx in range(w):
                for dy in range(w):
                    self.set(x + dx - w // 2, y + dy - w // 2, c)

    def ellipse(self, cx, cy, rx, ry, c, fn=None):
        for y in range(self.n):
            for x in range(self.n):
                if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    self.set(x, y, fn(x, y) if fn else c)

    def poly(self, pts, c, fn=None):
        for y in range(self.n):
            yc = y + 0.5
            xs = []
            for i in range(len(pts)):
                (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
                if (ay <= yc < by) or (by <= yc < ay):
                    xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.ceil(xs[i] - 0.5)), int(math.floor(xs[i + 1] - 0.5)) + 1):
                    self.set(x, y, fn(x, y) if fn else c)

    def outline(self, c=K):
        snap = [row[:] for row in self.p]
        for y in range(self.n):
            for x in range(self.n):
                if snap[y][x] is not T:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < self.n and 0 <= yy < self.n and snap[yy][xx] is not T \
                            and snap[yy][xx] != c:
                        self.p[y][x] = c
                        break

    def paste(self, other, ox, oy):
        for y in range(other.n):
            for x in range(other.n):
                if other.p[y][x] is not T:
                    self.set(x + ox, y + oy, other.p[y][x])


# ---- file writers -----------------------------------------------------------

def bmp_rows(pixels, bpp):
    out = b''
    for row in reversed(pixels):
        if bpp == 4:
            row = row + [0] * (len(row) % 2)
            data = bytes((row[i] << 4) | row[i + 1] for i in range(0, len(row), 2))
        else:
            row = row + [0] * (-len(row) % 8)
            data = bytes(sum(row[i + j] << (7 - j) for j in range(8)) for i in range(0, len(row), 8))
        out += data + b'\0' * (-len(data) % 4)
    return out


def table(colors):
    return b''.join(struct.pack('<BBBB', b, g, r, 0) for r, g, b in colors)


def write_bmp(name, pixels, bpp, colors):
    body = struct.pack('<IiiHHIIiiII', 40, len(pixels[0]), len(pixels), 1, bpp, 0, 0, 0, 0, 0, 0) \
        + table(colors) + bmp_rows(pixels, bpp)
    off = 14 + 40 + 4 * len(colors)
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(struct.pack('<2sIHHI', b'BM', 14 + len(body), 0, 0, off) + body)


def write_icon_file(name, kind, bpp, xor, and_mask, colors, hot=(0, 0)):
    size = len(xor)
    image = (struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, bpp, 0, 0, 0, 0, 0, 0)
             + table(colors) + bmp_rows(xor, bpp) + bmp_rows(and_mask, 1))
    if kind == 1:
        entry = struct.pack('<BBBBHHII', size, size, len(colors) & 255, 0, 1, bpp, len(image), 22)
    else:
        entry = struct.pack('<BBBBHHII', size, size, 0, 0, hot[0], hot[1], len(image), 22)
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(struct.pack('<HHH', 0, kind, 1) + entry + image)


def write_ico(name, c):
    xor = [[0 if v is T else v for v in row] for row in c.p]
    andm = [[1 if v is T else 0 for v in row] for row in c.p]
    write_icon_file(name, 1, 4, xor, andm, VGA)


# ---- shared shapes ------------------------------------------------------------

def page(c, x0, y0, x1, y1, fold=5, fill=WHT):
    c.poly([(x0, y0), (x1 - fold + 1, y0), (x1 + 1, y0 + fold), (x1 + 1, y1 + 1), (x0, y1 + 1)], fill)
    for y in range(y0 + fold, y1 + 1):
        c.set(x1, y, SILV)
    for x in range(x0 + 1, x1 + 1):
        c.set(x, y1, SILV)
    for i in range(fold):                       # dog-ear
        for j in range(i + 1):
            c.set(x1 - fold + 1 + j, y0 + i, SILV if j < i else GRAY)
    c.line(x1 - fold + 1, y0, x1 - fold + 1, y0 + fold - 1, GRAY)
    c.line(x1 - fold + 1, y0 + fold - 1, x1, y0 + fold - 1, GRAY)


def folder(c, x0, y0, x1, y1, open_=False):
    w, h = x1 - x0, y1 - y0
    tab = max(3, w * 2 // 5)
    c.rect(x0, y0, x0 + tab, y0 + 2, YEL)
    c.rect(x0, y0 + 2, x1, y1, OLIVE)           # back panel
    if open_:
        c.poly([(x0 + 3, y0 + h // 3 + 1), (x1 + 1, y0 + h // 3 + 1), (x1 - 2, y1 + 1), (x0, y1 + 1)],
               YEL, lambda x, y: WHT if y == y0 + h // 3 + 1 else YEL)
    else:
        c.rect(x0, y0 + 4, x1, y1, YEL)
        for x in range(x0 + 1, x1):
            c.set(x, y0 + 4, WHT)
        for y in range(y0 + 5, y1 + 1):
            c.set(x1, y, OLIVE)
        for x in range(x0, x1 + 1):
            c.set(x, y1, OLIVE)
    c.set(x0 + 1, y0, WHT)


def window(c, x0, y0, x1, y1, title=NAVY, body=WHT):
    c.rect(x0, y0, x1, y1, body)
    c.rect(x0, y0, x1, y0 + 3, title)
    for x in range(x0, x1 + 1):                 # title gradient to teal
        if (x - x0) > (x1 - x0) * 2 // 3 and (x + y0) % 2 == 0:
            c.set(x, y0 + 1, TEAL)
            c.set(x, y0 + 2, TEAL)
    c.rect(x0 + 1, y0 + 1, x0 + 2, y0 + 2, WHT)
    c.rect(x1 - 2, y0 + 1, x1 - 1, y0 + 2, SILV)
    c.line(x0, y0 + 4, x1, y0 + 4, K)


def drive_box(c, ox, oy):
    c.poly([(ox + 6, oy + 9), (ox + 29, oy + 9), (ox + 25, oy + 14), (ox + 2, oy + 14)], SILV,
           lambda x, y: WHT if y < oy + 11 else SILV)
    c.rect(ox + 2, oy + 14, ox + 24, oy + 23, SILV)
    c.poly([(ox + 25, oy + 14), (ox + 29, oy + 9), (ox + 29, oy + 18), (ox + 25, oy + 23)], GRAY)
    c.line(ox + 2, oy + 14, ox + 24, oy + 14, WHT)
    c.line(ox + 5, oy + 18, ox + 17, oy + 18, GRAY)
    c.line(ox + 5, oy + 19, ox + 17, oy + 19, WHT)
    c.rect(ox + 20, oy + 19, ox + 22, oy + 20, LIME)
    c.line(ox + 2, oy + 23, ox + 24, oy + 23, GRAY)


# ---- 32x32 icons -------------------------------------------------------------

def ico_folder():
    c = Canvas(32)
    folder(c, 2, 5, 29, 27)
    c.outline()
    return c


def ico_floppy():
    c = Canvas(32)
    c.rect(4, 2, 27, 28, NAVY)
    c.line(5, 3, 5, 27, BLUE)
    c.line(5, 3, 26, 3, BLUE)
    c.rect(10, 2, 22, 11, SILV)
    c.line(10, 2, 22, 2, WHT)
    c.rect(17, 4, 19, 9, NAVY)
    c.line(22, 3, 22, 11, GRAY)
    c.rect(7, 15, 24, 28, WHT)
    for y in (18, 21, 24):
        c.line(9, y, 22, y, SILV)
    c.line(9, 18, 16, 18, BLUE)
    c.rect(25, 25, 26, 27, K)
    c.set(27, 2, T)
    c.outline()
    return c


def ico_harddisk():
    c = Canvas(32)
    drive_box(c, 0, 3)
    c.outline()
    return c


def ico_netdrive():
    c = Canvas(32)
    drive_box(c, 0, -3)
    c.rect(13, 21, 16, 25, GRAY)
    c.rect(1, 26, 30, 28, LIME)
    c.line(1, 28, 30, 28, DGRN)
    c.rect(13, 21, 14, 25, SILV)
    c.outline()
    return c


def ico_cdrom():
    c = Canvas(32)

    def sheen(x, y):
        a = math.degrees(math.atan2(y - 16, x - 16)) % 360
        if 20 < a < 60:
            return CYAN if (x + y) % 2 else SILV
        if 60 <= a < 85:
            return MAG if (x + y) % 2 else SILV
        if 200 < a < 240:
            return YEL if (x + y) % 2 else SILV
        if 240 <= a < 265:
            return LIME if (x + y) % 2 else SILV
        return WHT if (a < 20 or 180 < a < 200) else SILV
    c.ellipse(15.5, 15.5, 14, 14, SILV, sheen)
    c.ellipse(15.5, 15.5, 5, 5, WHT)
    c.ellipse(15.5, 15.5, 4, 4, SILV)
    c.ellipse(15.5, 15.5, 2.2, 2.2, T)
    for y in range(32):
        for x in range(32):
            if c.get(x, y) is not T and (x - 15.5) ** 2 + (y - 15.5) ** 2 <= 2.2 ** 2:
                c.set(x, y, T)
    c.outline()
    return c


def ico_computer():
    c = Canvas(32)
    c.rect(3, 2, 28, 21, SILV)
    c.line(3, 2, 28, 2, WHT)
    c.line(3, 2, 3, 21, WHT)
    c.line(28, 3, 28, 21, GRAY)
    c.line(4, 21, 28, 21, GRAY)
    c.rect(6, 5, 25, 17, K)
    c.dither(7, 6, 24, 16, NAVY, TEAL)
    for i in range(4):
        c.set(9 + i, 8 + i, WHT) if i < 3 else None
    c.line(8, 7, 11, 7, CYAN)
    c.set(25, 19, LIME)
    c.set(24, 19, LIME)
    c.rect(12, 22, 19, 24, GRAY)
    c.rect(6, 25, 25, 28, SILV)
    c.line(6, 25, 25, 25, WHT)
    c.line(6, 28, 25, 28, GRAY)
    c.outline()
    return c


def ico_group():
    c = Canvas(32)
    window(c, 2, 4, 29, 27)
    cols = [(RED, DRED), (BLUE, NAVY), (LIME, DGRN), (YEL, OLIVE), (MAG, PURP), (CYAN, TEAL)]
    for i, (a, b) in enumerate(cols):
        x = 6 + (i % 3) * 8
        y = 11 + (i // 3) * 8
        c.rect(x, y, x + 4, y + 4, a)
        c.line(x + 4, y + 1, x + 4, y + 4, b)
        c.line(x + 1, y + 4, x + 4, y + 4, b)
        c.set(x, y, WHT)
    c.outline()
    return c


def ico_recent():
    c = Canvas(32)
    c.ellipse(15.5, 15.5, 14, 14, BLUE)
    c.ellipse(15.5, 15.5, 11.5, 11.5, WHT)
    for a in range(0, 360, 30):
        r = math.radians(a)
        x, y = 15.5 + 9.5 * math.cos(r), 15.5 + 9.5 * math.sin(r)
        c.set(int(round(x - 0.5)), int(round(y - 0.5)), K if a % 90 == 0 else GRAY)
    c.line(15, 15, 15, 8, K, 2)
    c.line(16, 16, 21, 19, K, 2)
    c.set(15, 15, RED)
    c.set(16, 16, RED)
    c.ellipse(15.5, 15.5, 14, 14, BLUE, lambda x, y: NAVY if (x - 15.5) ** 2 + (y - 15.5) ** 2 > 12.5 ** 2
              and x + y > 34 else c.get(x, y))
    c.outline()
    return c


def star_pts(cx, cy, ro, ri):
    pts = []
    for i in range(10):
        a = math.radians(-90 + i * 36)
        r = ro if i % 2 == 0 else ri
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def ico_star():
    c = Canvas(32)
    c.poly(star_pts(16, 17, 15, 6.5), YEL, lambda x, y: OLIVE if x > 17 and y > 18 else
           (WHT if x + y < 22 and (x + y) % 3 == 0 else YEL))
    c.outline()
    return c


def ico_doc():
    c = Canvas(32)
    page(c, 6, 1, 25, 29, 7)
    c.outline()
    return c


def ico_text():
    c = Canvas(32)
    page(c, 6, 1, 25, 29, 7)
    for i, y in enumerate(range(10, 27, 3)):
        c.line(9, y, 22 - (5 if i % 3 == 2 else 0), y, GRAY)
    c.line(9, 6, 16, 6, NAVY)
    c.outline()
    return c


def ico_program():
    c = Canvas(32)
    window(c, 2, 4, 29, 27)
    c.rect(3, 9, 28, 10, SILV)
    c.ellipse(10, 18, 4.5, 4.5, RED, lambda x, y: WHT if x < 9 and y < 17 else RED)
    c.rect(16, 13, 25, 21, BLUE)
    c.line(16, 13, 25, 13, CYAN)
    c.poly([(9, 25), (14, 19), (19, 25)], LIME)
    c.outline()
    return c


def ico_dosprog():
    c = Canvas(32)
    c.rect(2, 4, 29, 27, K)
    c.rect(2, 4, 29, 7, SILV)
    c.line(2, 4, 29, 4, WHT)
    c.rect(3, 5, 4, 6, GRAY)
    font = {'C': ["###", "#..", "#..", "#..", "###"], ':': ["...", ".#.", "...", ".#.", "..."],
            '\\': ["#..", "#..", ".#.", "..#", "..#"], '>': ["#..", ".#.", "..#", ".#.", "#.."]}
    x = 5
    for ch in "C:\\>":
        for dy, row in enumerate(font[ch]):
            for dx, v in enumerate(row):
                if v == '#':
                    c.set(x + dx, 11 + dy, SILV)
        x += 4
    c.rect(22, 15, 24, 15, WHT)
    for y in (19, 22):
        c.line(5, y, 5 + (14 if y == 19 else 9), y, GRAY)
    c.outline(GRAY)
    return c


def ico_image():
    c = Canvas(32)
    c.rect(2, 5, 29, 26, OLIVE)
    c.rect(3, 6, 28, 25, YEL)
    c.rect(5, 8, 26, 23, CYAN)
    c.dither(5, 8, 26, 15, BLUE, CYAN)
    c.ellipse(21, 12, 3, 3, YEL)
    c.ellipse(11, 11, 3, 1.5, WHT)
    c.poly([(5, 24), (11, 15), (16, 21), (20, 17), (27, 24)], LIME,
           lambda x, y: DGRN if (x + y) % 4 == 0 or y > 21 else LIME)
    c.line(3, 25, 28, 25, OLIVE)
    c.line(28, 6, 28, 25, OLIVE)
    c.outline()
    return c


def ico_waste(full=False):
    c = Canvas(32)
    if full:
        c.ellipse(11, 7, 6, 4, WHT)
        c.ellipse(20, 6, 6, 4, WHT)
        c.ellipse(16, 4, 4, 3, WHT)
        for p in ((9, 6), (12, 8), (19, 5), (22, 7), (16, 3), (14, 5)):
            c.set(p[0], p[1], SILV)
        c.outline()
    c.poly([(4, 9), (28, 9), (25, 30), (7, 30)], SILV,
           lambda x, y: (GRAY if x % 4 == 1 else WHT if x == 7 or x == 8 else SILV))
    c.ellipse(15.8, 9, 12.5, 3, GRAY)
    c.ellipse(15.8, 9.4, 10.5, 2, K if not full else GRAY)
    cc = Canvas(32)
    cc.p = [row[:] for row in c.p]
    c.outline()
    return c


def ico_search():
    c = Canvas(32)
    c.line(19, 19, 28, 28, OLIVE, 4)
    c.line(20, 19, 28, 27, YEL, 1)
    c.ellipse(12.5, 12.5, 10, 10, K)
    c.ellipse(12.5, 12.5, 8, 8, CYAN, lambda x, y: WHT if (x - 9) ** 2 + (y - 9) ** 2 < 6 else
              (TEAL if x + y > 30 else CYAN))
    c.outline()
    return c


def gear(c, cx, cy, r, col=GRAY):
    for a in range(0, 360, 45):
        rr = math.radians(a)
        x, y = cx + (r + 1.5) * math.cos(rr), cy + (r + 1.5) * math.sin(rr)
        c.ellipse(x, y, 1.6, 1.6, col)
    c.ellipse(cx, cy, r, r, col, lambda x, y: SILV if x + y < cx + cy - 2 else col)
    c.ellipse(cx, cy, r * 0.4, r * 0.4, WHT)


def ico_sysfile():
    c = Canvas(32)
    page(c, 6, 1, 25, 29, 7)
    gear(c, 15.5, 17.5, 5)
    c.outline()
    return c


def ico_ramdisk():
    c = Canvas(32)
    c.rect(1, 9, 30, 23, DGRN)
    c.line(1, 9, 30, 9, LIME)
    for i in range(4):
        x = 3 + i * 7
        c.rect(x, 11, x + 4, 17, K)
        c.line(x, 11, x + 4, 11, GRAY)
    for x in range(2, 30, 2):
        c.rect(x, 20, x, 23, YEL)
    c.set(15, 23, T)
    c.set(16, 23, T)
    c.outline()
    return c


def ico_archive():
    c = Canvas(32)
    books = [(RED, DRED, 6), (BLUE, NAVY, 13), (LIME, DGRN, 20)]
    for a, b, y in books:
        c.rect(3, y, 28, y + 6, a)
        c.line(3, y + 6, 28, y + 6, b)
        c.line(3, y, 28, y, WHT)
        c.rect(26, y + 1, 27, y + 5, WHT)
    c.rect(14, 4, 17, 28, YEL)
    c.line(17, 4, 17, 28, OLIVE)
    c.rect(13, 14, 18, 18, SILV)
    c.frame(13, 14, 18, 18, GRAY)
    c.outline()
    return c


def ico_music():
    c = Canvas(32)
    page(c, 6, 1, 25, 29, 7)
    c.rect(13, 9, 21, 10, K)
    c.line(13, 9, 13, 22, K)
    c.line(21, 9, 21, 20, K)
    c.ellipse(11.5, 22.5, 2.6, 2, K)
    c.ellipse(19.5, 20.5, 2.6, 2, K)
    c.outline()
    return c


def ico_app():
    c = Canvas(32)

    def tile(x, y):
        return c.get(x, y)
    # rounded tile with a deep blue to teal gradient
    for y in range(1, 31):
        for x in range(1, 31):
            corner = min(x - 1, 30 - x), min(y - 1, 30 - y)
            if corner[0] + corner[1] < 3 and min(corner) < 2:
                continue
            c.set(x, y, 0)
    c.dither(1, 1, 30, 30, NAVY, TEAL)
    for y in range(1, 31):
        for x in range(1, 31):
            corner = min(x - 1, 30 - x), min(y - 1, 30 - y)
            if corner[0] + corner[1] < 3 and min(corner) < 2:
                c.set(x, y, T)
    w = Canvas(32)
    window(w, 12, 4, 27, 17)
    w.rect(13, 9, 26, 16, WHT)
    for y in (11, 13, 15):
        w.line(15, y, 24, y, SILV)
    w.outline()
    f = Canvas(32)
    folder(f, 4, 13, 19, 26, open_=True)
    f.outline()
    c.paste(w, 0, 0)
    c.paste(f, 0, 0)
    for p in ((25, 23), (26, 22), (27, 23), (26, 24), (26, 21), (26, 25), (24, 23), (28, 23)):
        c.set(p[0], p[1], WHT if p in ((25, 23), (26, 22), (27, 23), (26, 24)) else CYAN)
    c.set(26, 23, YEL)
    c.outline(NAVY)
    return c


# ---- 16x16 glyphs ------------------------------------------------------------

def g_folder(open_=False):
    c = Canvas(16)
    folder(c, 1, 3, 14, 13, open_)
    c.outline()
    return c


def g_floppy():
    c = Canvas(16)
    c.rect(2, 1, 14, 14, NAVY)
    c.rect(5, 1, 11, 5, SILV)
    c.rect(8, 2, 9, 4, NAVY)
    c.rect(4, 8, 12, 14, WHT)
    c.line(5, 10, 11, 10, SILV)
    c.line(5, 12, 11, 12, SILV)
    c.outline()
    return c


def g_drive(net=False):
    c = Canvas(16)
    oy = 2 if net else 4
    c.poly([(3, oy), (15, oy), (13, oy + 3), (1, oy + 3)], WHT)
    c.rect(1, oy + 3, 12, oy + 8, SILV)
    c.poly([(13, oy + 3), (15, oy), (15, oy + 5), (13, oy + 8)], GRAY)
    c.line(3, oy + 5, 8, oy + 5, GRAY)
    c.set(10, oy + 6, LIME)
    c.set(11, oy + 6, LIME)
    if net:
        c.rect(7, oy + 9, 8, 13, GRAY)
        c.line(0, 14, 15, 14, LIME)
        c.line(0, 15, 15, 15, DGRN)
    c.outline()
    return c


def g_cdrom():
    c = Canvas(16)
    c.ellipse(7.5, 7.5, 7, 7, SILV, lambda x, y: CYAN if 9 < x and y < 6 else
              (MAG if x > 10 and 6 <= y < 8 else (YEL if x < 6 and y > 9 else
                                                    (WHT if x < 6 and y < 6 else SILV))))
    c.ellipse(7.5, 7.5, 2.2, 2.2, WHT)
    c.ellipse(7.5, 7.5, 1, 1, T)
    c.set(7, 7, T)
    c.set(8, 8, T)
    c.set(7, 8, T)
    c.set(8, 7, T)
    c.outline()
    return c


def g_computer():
    c = Canvas(16)
    c.rect(1, 1, 14, 10, SILV)
    c.rect(3, 3, 12, 8, K)
    c.dither(4, 4, 11, 7, NAVY, TEAL)
    c.rect(6, 11, 9, 12, GRAY)
    c.rect(3, 13, 12, 14, SILV)
    c.outline()
    return c


def g_group():
    c = Canvas(16)
    c.rect(1, 2, 14, 13, WHT)
    c.rect(1, 2, 14, 3, NAVY)
    c.set(2, 2, WHT)
    for i, col in enumerate((RED, BLUE, LIME, YEL)):
        x = 3 + (i % 2) * 6
        y = 5 + (i // 2) * 4
        c.rect(x, y, x + 3, y + 2, col)
    c.outline()
    return c


def g_recent():
    c = Canvas(16)
    c.ellipse(7.5, 7.5, 7, 7, BLUE)
    c.ellipse(7.5, 7.5, 5.5, 5.5, WHT)
    c.line(7, 3, 7, 7, K)
    c.line(8, 8, 11, 10, K)
    c.outline()
    return c


def g_star():
    c = Canvas(16)
    c.poly(star_pts(7.5, 8.2, 7.6, 3.2), YEL, lambda x, y: OLIVE if x > 8 and y > 9 else YEL)
    c.outline()
    return c


def g_task():
    c = Canvas(16)
    c.rect(1, 2, 14, 13, WHT)
    c.rect(1, 2, 14, 4, NAVY)
    c.rect(12, 3, 13, 3, SILV)
    for y in (7, 9, 11):
        c.line(3, y, 11 - (y % 3), y, SILV)
    c.outline()
    return c


def g_page(fill_fn=None):
    c = Canvas(16)
    page(c, 3, 0, 12, 14, 4)
    if fill_fn:
        fill_fn(c)
    c.outline()
    return c


def g_program():
    c = Canvas(16)
    c.rect(1, 2, 14, 13, WHT)
    c.rect(1, 2, 14, 4, NAVY)
    c.set(2, 3, WHT)
    c.rect(3, 7, 6, 10, RED)
    c.rect(8, 6, 12, 11, BLUE)
    c.outline()
    return c


def g_dosprog():
    c = Canvas(16)
    c.rect(1, 2, 14, 13, K)
    c.rect(1, 2, 14, 4, SILV)
    c.line(3, 7, 4, 7, SILV)
    c.set(5, 8, SILV)
    c.line(3, 9, 4, 9, SILV)
    c.line(7, 9, 9, 9, WHT)
    c.outline(GRAY)
    return c


def g_search():
    c = Canvas(16)
    c.line(10, 10, 14, 14, OLIVE, 2)
    c.ellipse(6, 6, 5.5, 5.5, K)
    c.ellipse(6, 6, 4, 4, CYAN, lambda x, y: WHT if x < 5 and y < 5 else CYAN)
    c.outline()
    return c


def g_image():
    c = Canvas(16)
    c.rect(0, 2, 15, 13, OLIVE)
    c.rect(1, 3, 14, 12, CYAN)
    c.ellipse(11, 5.5, 1.5, 1.5, YEL)
    c.poly([(1, 13), (5, 7), (8, 10), (10, 8), (15, 13)], LIME)
    c.outline()
    return c


def g_waste():
    c = Canvas(16)
    c.poly([(2, 3), (14, 3), (12, 15), (4, 15)], SILV, lambda x, y: GRAY if x % 3 == 0 else SILV)
    c.ellipse(7.8, 3.2, 6.5, 1.8, GRAY)
    c.ellipse(7.8, 3.4, 5, 1, K)
    c.outline()
    return c


def g_ramdisk():
    c = Canvas(16)
    c.rect(0, 4, 15, 11, DGRN)
    for x in (2, 6, 10):
        c.rect(x, 5, x + 2, 8, K)
    for x in range(1, 15, 2):
        c.set(x, 10, YEL)
        c.set(x, 11, YEL)
    c.outline()
    return c


def g_archive():
    c = Canvas(16)
    for col, dk, y in ((RED, DRED, 2), (BLUE, NAVY, 6), (LIME, DGRN, 10)):
        c.rect(1, y, 14, y + 3, col)
        c.line(1, y + 3, 14, y + 3, dk)
    c.rect(7, 1, 8, 14, YEL)
    c.outline()
    return c


def arrow_left():
    c = Canvas(16)
    pts = [(1, 8), (7, 2), (7, 5), (14, 5), (14, 11), (7, 11), (7, 14)]
    c.poly(pts, BLUE, lambda x, y: CYAN if y < 7 else (NAVY if y > 9 else BLUE))
    c.outline()
    return c


def flip_h(c):
    n = Canvas(16)
    for y in range(16):
        n.p[y] = list(reversed(c.p[y]))
    return n


def rot_cw(c):
    n = Canvas(16)
    for y in range(16):
        for x in range(16):
            n.p[x][15 - y] = c.p[y][x]
    return n


def g_up():
    c = Canvas(16)
    pts = [(8, 1), (14, 7), (11, 7), (11, 14), (5, 14), (5, 7), (2, 7)]
    c.poly(pts, LIME, lambda x, y: WHT if x < 7 and y < 6 else (DGRN if x > 9 else LIME))
    c.outline()
    return c


def g_vicons():
    c = Canvas(16)
    for i, col in enumerate((BLUE, LIME, RED, YEL)):
        x = 1 + (i % 2) * 8
        y = 1 + (i // 2) * 8
        c.rect(x, y, x + 5, y + 4, col)
        c.frame(x, y, x + 5, y + 4, K)
        c.line(x, y + 6, x + 5, y + 6, GRAY)
    return c


def g_vtiles():
    c = Canvas(16)
    for i, col in enumerate((BLUE, LIME, RED)):
        y = 1 + i * 5
        c.rect(0, y, 3, y + 3, col)
        c.frame(0, y, 3, y + 3, K)
        c.line(5, y, 14, y, K)
        c.line(5, y + 2, 11, y + 2, GRAY)
    return c


def g_vdetails():
    c = Canvas(16)
    c.line(0, 1, 15, 1, GRAY)
    for i, col in enumerate((BLUE, LIME, YEL, RED, CYAN)):
        y = 3 + i * 3
        c.rect(0, y, 1, y + 1, col)
        c.line(3, y, 8, y, K)
        c.line(10, y, 12, y, GRAY)
        c.line(14, y, 15, y, GRAY)
    return c


def g_cut():
    c = Canvas(16)
    c.line(4, 0, 9, 9, GRAY)
    c.line(11, 0, 6, 9, GRAY)
    c.line(5, 0, 9, 8, SILV)
    c.line(10, 0, 7, 8, SILV)
    c.ellipse(4.5, 12, 2.8, 2.8, RED)
    c.ellipse(11, 12, 2.8, 2.8, RED)
    c.ellipse(4.5, 12, 1.2, 1.2, T)
    c.ellipse(11, 12, 1.2, 1.2, T)
    for p in ((4, 12), (5, 12), (4, 11), (5, 11), (11, 12), (10, 12), (11, 11), (10, 11)):
        c.set(p[0], p[1], T)
    c.outline()
    return c


def g_copy():
    c = Canvas(16)
    a = Canvas(16)
    page(a, 0, 0, 8, 10, 3)
    for y in (4, 6, 8):
        a.line(2, y, 6, y, GRAY)
    a.outline()
    b = Canvas(16)
    page(b, 6, 4, 14, 14, 3)
    for y in (8, 10, 12):
        b.line(8, y, 12, y, BLUE)
    b.outline()
    c.paste(a, 0, 0)
    c.paste(b, 0, 0)
    return c


def g_paste():
    c = Canvas(16)
    c.rect(1, 2, 11, 14, OLIVE)
    c.rect(2, 3, 10, 13, YEL)
    c.rect(4, 1, 8, 3, SILV)
    c.outline()
    p = Canvas(16)
    page(p, 7, 6, 14, 15, 3)
    for y in (10, 12):
        p.line(9, y, 12, y, BLUE)
    p.outline()
    c.paste(p, 0, 0)
    return c


def g_delete():
    c = Canvas(16)
    c.line(3, 3, 12, 12, RED, 3)
    c.line(12, 3, 3, 12, RED, 3)
    c.line(3, 2, 12, 11, WHT) if False else None
    c.outline(DRED)
    return c


def g_props():
    c = Canvas(16)
    page(c, 1, 0, 10, 14, 3)
    for y in (4, 6, 8):
        c.line(3, y, 7, y, GRAY)
    c.outline()
    c.ellipse(11, 11, 4.4, 4.4, BLUE)
    c.rect(10, 10, 11, 14, WHT)
    c.rect(10, 7, 11, 8, WHT)
    return c


def g_newfolder():
    c = g_folder()
    for p in ((12, 0), (12, 1), (12, 2), (12, 3), (12, 4), (10, 2), (11, 2), (13, 2), (14, 2),
              (11, 1), (13, 3), (11, 3), (13, 1)):
        c.set(p[0], p[1], YEL if p[0] == 12 or p[1] == 2 else OLIVE)
    c.set(12, 2, WHT)
    return c


def g_run():
    c = Canvas(16)
    c.ellipse(7.5, 7.5, 7, 7, DGRN)
    c.ellipse(7.5, 7.5, 6, 6, LIME, lambda x, y: WHT if x < 6 and y < 5 else LIME)
    c.poly([(5, 4), (12, 8), (5, 12)], WHT)
    c.poly([(6, 5), (11, 8), (6, 11)], DGRN)
    return c


def g_preview():
    c = Canvas(16)
    c.rect(0, 1, 15, 14, WHT)
    c.rect(0, 1, 15, 3, NAVY)
    c.rect(9, 4, 15, 14, CYAN)
    c.line(9, 4, 9, 14, K)
    for y in (6, 8, 10, 12):
        c.line(2, y, 7, y, SILV)
    c.ellipse(12, 8, 1.5, 1.5, YEL)
    c.frame(0, 1, 15, 14, K)
    return c


def g_refresh():
    c = Canvas(16)
    for a in range(30, 330, 4):
        r = math.radians(a)
        c.set(int(round(7.5 + 5.5 * math.cos(r))), int(round(7.5 + 5.5 * math.sin(r))), DGRN)
        c.set(int(round(7.5 + 4.5 * math.cos(r))), int(round(7.5 + 4.5 * math.sin(r))), LIME)
    c.poly([(10, 1), (15, 4), (10, 7)], DGRN)
    return c


def g_theme():
    c = Canvas(16)
    c.ellipse(7.5, 8, 7.4, 6.5, YEL, lambda x, y: OLIVE if x + y > 18 else YEL)
    c.ellipse(4.5, 10.5, 1.5, 1.5, T)
    for (x, y, col) in ((4, 5, RED), (8, 4, BLUE), (12, 6, LIME), (11, 11, MAG)):
        c.ellipse(x, y, 1.6, 1.6, col)
    c.outline()
    return c


def glyphs():
    def text_lines(c):
        for y in (4, 6, 8, 10, 12):
            c.line(5, y, 10 - (2 if y == 12 else 0), y, GRAY)

    def gear_small(c):
        gear(c, 7.5, 8.5, 2.6)

    def note(c):
        c.line(9, 4, 9, 11, K)
        c.line(10, 4, 11, 5, K)
        c.ellipse(7.5, 11.5, 1.8, 1.3, K)

    order = [
        g_folder(), g_folder(True), g_floppy(), g_drive(), g_cdrom(), g_drive(True),
        g_computer(), g_group(), g_recent(), g_star(), g_task(), g_page(), g_program(),
        g_search(), g_page(text_lines), g_image(), g_waste(), g_ramdisk(), g_dosprog(),
        g_page(gear_small), g_archive(), g_page(note),
        arrow_left(), flip_h(arrow_left()), g_up(), g_vicons(), g_vtiles(), g_vdetails(),
        g_cut(), g_copy(), g_paste(), g_delete(), g_props(), g_newfolder(), g_run(),
        g_preview(), g_refresh(), g_theme(),
    ]
    rows = [[] for _ in range(16)]
    mask = [[] for _ in range(16)]
    for g in order:
        for y in range(16):
            rows[y] += [SILV if v is T else v for v in g.p[y]]
            mask[y] += [1 if v is T else 0 for v in g.p[y]]
    write_bmp('glyphs.bmp', rows, 4, VGA)
    write_bmp('glyphsm.bmp', mask, 1, [(0, 0, 0), (255, 255, 255)])
    return len(order)


# ---- cursors ------------------------------------------------------------------

ARROW = [
    "X...........",
    "XX..........",
    "XWX.........",
    "XWWX........",
    "XWWWX.......",
    "XWWWWX......",
    "XWWWWWX.....",
    "XWWWWWWX....",
    "XWWWWWWWX...",
    "XWWWWWWWWX..",
    "XWWWWWXXXXX.",
    "XWWXWWX.....",
    "XWX.XWWX....",
    "XX..XWWX....",
    "X....XWWX...",
    ".....XWWX...",
    "......XX....",
]


def cursor(name, badge):
    xor = [[0] * 32 for _ in range(32)]
    andm = [[1] * 32 for _ in range(32)]

    def put(x, y, white):
        andm[y][x] = 0
        xor[y][x] = 1 if white else 0
    if badge != 'no':
        for y, row in enumerate(ARROW):
            for x, ch in enumerate(row):
                if ch != '.':
                    put(x, y, ch == 'W')
    bx, by = 9, 17
    if badge in ('move', 'copy', 'link'):
        for y in range(by, by + 11):
            for x in range(bx, bx + 13):
                edge = y in (by, by + 10) or x in (bx, bx + 12)
                put(x, y, not edge)
        for x in range(bx + 2, bx + 11, 2):       # a "file" inside: dotted lines
            put(x, by + 3, False)
            put(x, by + 6, False)
        if badge == 'copy':
            for y in range(by + 2, by + 9):
                for x in range(bx + 4, bx + 9):
                    put(x, y, True)
            for i in range(-3, 4):
                put(bx + 6 + i, by + 5, False)
                put(bx + 6, by + 5 + i, False)
        if badge == 'link':
            for y in range(by + 2, by + 9):
                for x in range(bx + 3, bx + 10):
                    put(x, y, True)
            for i in range(5):
                put(bx + 4 + i, by + 7 - i, False)
            for i in range(3):
                put(bx + 6 + i, by + 3, False)
                put(bx + 8, by + 3 + i, False)
    if badge == 'no':
        for y in range(32):
            for x in range(32):
                d = math.hypot(x - 10.5, y - 10.5)
                on_ring = 7 <= d <= 10
                on_bar = d < 9 and abs((x - 10.5) - (y - 10.5)) < 2.0
                if on_ring or on_bar:
                    put(x, y, False)
                elif d < 11.2 and (d > 6 or abs((x - 10.5) - (y - 10.5)) < 3):
                    put(x, y, True)
        hot = (10, 10)
    else:
        hot = (0, 0)
    write_icon_file(name, 2, 1, xor, andm, [(0, 0, 0), (255, 255, 255)], hot)


def main():
    icons = {
        'shelliv.ico': ico_app(), 'folder.ico': ico_folder(), 'floppy.ico': ico_floppy(),
        'harddisk.ico': ico_harddisk(), 'cdrom.ico': ico_cdrom(), 'netdrive.ico': ico_netdrive(),
        'computer.ico': ico_computer(), 'group.ico': ico_group(), 'recent.ico': ico_recent(),
        'star.ico': ico_star(), 'doc.ico': ico_doc(), 'program.ico': ico_program(),
        'dosprog.ico': ico_dosprog(), 'text.ico': ico_text(), 'image.ico': ico_image(),
        'waste.ico': ico_waste(), 'wastefull.ico': ico_waste(True), 'search.ico': ico_search(),
        'sysfile.ico': ico_sysfile(), 'ramdisk.ico': g2i(), 'archive.ico': ico_archive(),
        'music.ico': ico_music(),
    }
    for name, c in icons.items():
        write_ico(name, c)
    n = glyphs()
    for name in ('move', 'copy', 'no', 'link'):
        cursor('drag%s.cur' % name, name)
    print('wrote %d icons, %d glyphs and 4 cursors' % (len(icons), n))


def g2i():
    return ico_ramdisk()


if __name__ == '__main__':
    main()
