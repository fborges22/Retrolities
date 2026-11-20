#!/usr/bin/env python3
"""Generate src/polaris/glyphs.h: Polaris' custom VGA character glyphs.

The boot menu reprograms unused CP437 code points with a wordmark logo,
16x16 icons and interface chrome (rounded corners, smooth progress bars,
toggles...).  The VGA is switched to 8-dot characters so glyphs join without
gaps.  Without a VGA every custom code falls back to a plain CP437 character.

    python3 tools/mkglyphs.py                 # write ../glyphs.h
    python3 tools/mkglyphs.py --render-logo F # re-render logo.txt from font F
"""
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE.parent / 'glyphs.h'
LOGO = HERE / 'logo.txt'

# code points that Polaris never needs in their CP437 meaning
FREE = ([c for c in range(0x01, 0x10)] + [0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D] +
        list(range(0x80, 0xB0)) + [0xB5, 0xB6, 0xB7, 0xB8, 0xBD, 0xBE, 0xC6, 0xC7] +
        list(range(0xCF, 0xD9)) + list(range(0xE0, 0xF9)))


def g(art):
    """16 rows of 8 columns ('#' = on) -> 16 bytes."""
    rows = [r for r in art.strip('\n').split('\n')]
    assert len(rows) == 16, (len(rows), art)
    out = []
    for r in rows:
        r = r.strip().ljust(8, '.')
        assert len(r) == 8, r
        v = 0
        for i, ch in enumerate(r):
            if ch == '#':
                v |= 0x80 >> i
        out.append(v)
    return out


def wide(art, cols=2):
    """16 rows of 8*cols columns -> list of glyphs, left to right."""
    rows = [r.strip() for r in art.strip('\n').split('\n')]
    assert len(rows) == 16, (len(rows), art[:40])
    rows = [r.ljust(8 * cols, '.') for r in rows]
    return [g('\n'.join(r[8 * c:8 * c + 8] for r in rows)) for c in range(cols)]


def rowsv(spec):
    """build a glyph from {row: pattern} with blank rows elsewhere"""
    lines = ['........'] * 16
    for r, p in spec.items():
        lines[r] = p
    return g('\n'.join(lines))


# --------------------------------------------------------------------------
# interface glyphs: name -> (bytes, fallback CP437 char)
# --------------------------------------------------------------------------
UI = []


def ui(name, data, fallback):
    UI.append((name, data, fallback))


V = '...##...'
ui('RTL', rowsv({**{r: V for r in range(10, 16)}, 7: '.....###', 8: '....##..', 9: '...##...'}), 0xDA)
ui('RTR', rowsv({**{r: V for r in range(10, 16)}, 7: '###.....', 8: '..##....', 9: '...##...'}), 0xBF)
ui('RBL', rowsv({**{r: V for r in range(0, 6)}, 5: '...##...', 6: '....##..', 7: '.....###'}), 0xC0)
ui('RBR', rowsv({**{r: V for r in range(0, 6)}, 5: '...##...', 6: '..##....', 7: '###.....'}), 0xD9)
for i in range(1, 9):     # progress fill, i/8 of the cell, rows 5..10
    pat = ('#' * i).ljust(8, '.')
    ui(f'BAR{i}', rowsv({r: pat for r in range(5, 11)}), 0xDB if i >= 4 else 0x20)
ui('TRACK', rowsv({7: '########', 8: '########'}), 0xC4)
ui('SCRTRACK', rowsv({r: '...##...' if r % 2 == 0 else '........' for r in range(16)}), 0xB3)
ui('SCRTHUMB', rowsv({r: '..####..' for r in range(1, 15)}), 0xDB)
ui('BOXOFF', g('''
........
........
........
........
.######.
#......#
#......#
#......#
#......#
#......#
#......#
.######.
........
........
........
........'''), 0xF9)
ui('BOXON', g('''
........
........
........
........
.######.
#######.
######.#
#.###..#
##.#..##
###..###
####.###
.######.
........
........
........
........'''), 0xFE)
ui('RADOFF', g('''
........
........
........
........
..####..
.#....#.
#......#
#......#
#......#
#......#
.#....#.
..####..
........
........
........
........'''), 0x09)
ui('RADON', g('''
........
........
........
........
..####..
.#....#.
#..##..#
#.####.#
#.####.#
#..##..#
.#....#.
..####..
........
........
........
........'''), 0x07)
on_l, on_r = wide('''
................
................
................
................
..############..
.##############.
################
############..##
###########....#
###########....#
############..##
################
.##############.
..############..
................
................''')
off_l, off_r = wide('''
................
................
................
................
..############..
.#............#.
#..##.........#.
#.####........#.
#.####........#.
#..##.........#.
#.............#.
.#............#.
..############..
................
................
................''')
ui('TOGON1', on_l, ord('[')); ui('TOGON2', on_r, ord(']'))
ui('TOGOFF1', off_l, ord('[')); ui('TOGOFF2', off_r, ord(']'))
ui('STAR1', rowsv({8: '...#....'}), 0xFA)
ui('STAR2', rowsv({7: '...#....', 8: '..###...', 9: '...#....'}), 0xF9)
ui('STAR3', rowsv({5: '...#....', 6: '...#....', 7: '..###...', 8: '#######.', 9: '..###...',
                   10: '...#....', 11: '...#....'}), ord('*'))
ui('ACCENT', rowsv({r: '###.....' for r in range(16)}), 0xDD)
ui('CHEVRON', rowsv({5: '.##.....', 6: '..##....', 7: '...##...', 8: '...##...', 9: '..##....',
                     10: '.##.....'}), 0x10)
ui('DOT', rowsv({7: '..##....', 8: '..##....'}), 0xF9)
ui('KEYL', rowsv({r: '.#######' for r in range(2, 14)} | {1: '..######', 14: '..######'}), 0xDE)
ui('KEYR', rowsv({r: '#######.' for r in range(2, 14)} | {1: '######..', 14: '######..'}), 0xDD)
ui('SHADE1', rowsv({r: ('#...#...' if r % 4 == 0 else '..#...#.' if r % 4 == 2 else '........')
                    for r in range(16)}), 0xB0)
ui('SHADE2', rowsv({r: ('#.#.#.#.' if r % 2 == 0 else '........') for r in range(16)}), 0xB1)
ui('HLINE2', rowsv({7: '########'}), 0xC4)

# --------------------------------------------------------------------------
# icons (16x16, two glyphs) with two-letter fallbacks
# --------------------------------------------------------------------------
ICONS = [
    ('DISK', 'HD', '''
................
................
................
..############..
.#............#.
.#............#.
.#............#.
.##############.
.#............#.
.#.......##.#.#.
.#............#.
..############..
................
................
................
................'''),
    ('DOS', 'C>', '''
................
.##############.
.#............#.
.#.##.........#.
.#..##........#.
.#...##.......#.
.#..##........#.
.#.##..#####..#.
.#............#.
.##############.
.......##.......
.....######.....
................
................
................
................'''),
    ('WIN9X', 'Wn', '''
................
................
..######.######.
..######.######.
..######.######.
..######.######.
..######.######.
................
..######.######.
..######.######.
..######.######.
..######.######.
..######.######.
................
................
................'''),
    ('WINNT', 'NT', '''
................
..........#####.
....####.######.
.######..######.
.######..######.
.######.#######.
.######.######..
................
.######.######..
.######.#######.
.######..######.
.######..######.
....####.######.
..........#####.
................
................'''),
    ('OS2', '/2', '''
................
.....######.....
...##......##...
..#..........#..
.#....####....#.
.#...######...#.
#....######....#
#....######....#
.#...######...#.
.#....####....#.
..#..........#..
...##......##...
.....######.....
................
................
................'''),
    ('LINUX', 'Lx', '''
......####......
.....######.....
.....#.##.#.....
.....######.....
.....##..##.....
....##.##.##....
...##......##...
...#........#...
..##........##..
..##........##..
..##........##..
...#........#...
..####....####..
.######..######.
................
................'''),
    ('BSD', 'Bs', '''
................
.#............#.
.##..........##.
..##........##..
..############..
.##############.
.###..####..###.
.###..####..###.
.##############.
..############..
...##########...
....##....##....
.....######.....
......####......
................
................'''),
    ('CDROM', 'CD', '''
................
.....######.....
...##########...
..####....####..
.####.##...####.
.###.##.....###.
####.#..##...###
###....####...##
###....####...##
####....##...###
.###.........##.
.####.......###.
..####....####..
...##########...
.....######.....
................'''),
    ('FLOPPY', 'Fd', '''
................
.#############..
.##.#######.###.
.##.#######.####
.##.###..##.####
.##.#######.####
.##.........####
.###############
.###############
.##..........###
.##..........###
.##..........###
.##..........###
.###############
................
................'''),
    ('ZIP', 'Zp', '''
................
..############..
.#............#.
.#..########..#.
.#.......##...#.
.#......##....#.
.#.....##.....#.
.#....##......#.
.#...##.......#.
.#..########..#.
.#............#.
.#..########..#.
.#..#......#..#.
..############..
................
................'''),
    ('NET', 'Nw', '''
................
.....######.....
...##..##..##...
..#...#..#...#..
.#...#....#...#.
.##############.
.#..#......#..#.
.#..#......#..#.
.##############.
.#...#....#...#.
..#...#..#...#..
...##..##..##...
.....######.....
................
................
................'''),
    ('POWER', 'Pw', '''
................
.......##.......
.......##.......
...##..##..##...
..##...##...##..
.##....##....##.
.##..........##.
.##..........##.
.##..........##.
..##........##..
...###....###...
....########....
......####......
................
................
................'''),
    ('REBOOT', 'Rb', '''
................
.....#####..#...
...##.....###...
..##......####..
.##.............
.##.............
.##.............
.##.............
.##..........##.
..##........##..
...###....###...
....########....
......####......
................
................
................'''),
    ('GEAR', 'Se', '''
................
.......##.......
...##.####.##...
...##########...
....###..###....
..###......###..
.####......####.
..###......###..
....###..###....
...##########...
...##.####.##...
.......##.......
................
................
................
................'''),
    ('STAR', '*', '''
.......##.......
.......##.......
......####......
......####......
.....######.....
.##############.
..############..
....########....
.....######.....
....###..###....
...##......##...
..#..........#..
................
................
................
................'''),
    ('LOCK', 'Lk', '''
................
......####......
.....#....#.....
....#......#....
....#......#....
....#......#....
..############..
..############..
..#####..#####..
..#####..#####..
..######.#####..
..############..
..############..
................
................
................'''),
]

ICON_ORDER = ['DISK', 'DOS', 'WIN9X', 'WINNT', 'OS2', 'LINUX', 'BSD', 'CDROM', 'FLOPPY',
              'ZIP', 'NET', 'POWER', 'REBOOT', 'GEAR', 'STAR', 'LOCK']

# Polaris star for the logo: 3 columns x 2 rows (24x32)
LOGO_STAR = '''
...........##...........
...........##...........
...........##...........
..........####..........
..........####..........
..........####..........
.........######.........
.........######.........
........########........
.......##########.......
.....##############.....
..####################..
########################
..####################..
.....##############.....
.......##########.......
........########........
.........######.........
.........######.........
..........####..........
..........####..........
..........####..........
...........##...........
...........##...........
...........##...........
........................
........................
........................
........................
........................
........................
........................'''


def render_logo(font_path):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(font_path, 34)
    img = Image.new('L', (800, 60), 0)
    d = ImageDraw.Draw(img)
    x = 2
    for ch in 'POLARIS':
        d.text((x, 0), ch, font=font, fill=255)
        x += font.getlength(ch) + 4
    img = img.crop(img.getbbox())
    img = img.resize((int(img.width * 1.04), img.height), Image.LANCZOS)
    w, h = img.size
    lines = []
    top = (32 - h) // 2
    for y in range(32):
        row = ''
        for xx in range(w):
            yy = y - top
            row += '#' if 0 <= yy < h and img.getpixel((xx, yy)) > 120 else '.'
        lines.append(row)
    LOGO.write_text('\n'.join(lines) + '\n')
    print('logo', w, 'x', h)


def bitmap_cells(lines, cols):
    """split a 32-row bitmap into 2 x cols glyph cells"""
    lines = [l.ljust(cols * 8, '.') for l in lines]
    cells = []
    for r in range(2):
        row = []
        for c in range(cols):
            row.append(g('\n'.join(l[c * 8:c * 8 + 8] for l in lines[r * 16:r * 16 + 16])))
        cells.append(row)
    return cells


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--render-logo':
        render_logo(sys.argv[2])
        return
    free = list(FREE)
    glyphs = []                       # (code, bytes)
    defs = []
    fallback = []

    def alloc(data, fb):
        for code, d in glyphs:        # share identical glyphs
            if d == data:
                return code
        code = free.pop(0)
        glyphs.append((code, data))
        fallback.append((code, fb))
        return code

    for name, data, fb in UI:
        defs.append((f'G_{name}', alloc(data, fb)))

    icon_codes = {}
    for name, fb, art in ICONS:
        left, right = wide(art)
        fb = (fb + ' ')[:2]
        icon_codes[name] = (alloc(left, ord(fb[0])), alloc(right, ord(fb[1])))

    blank = [0] * 16
    star_lines = LOGO_STAR.strip('\n').split('\n')
    logo_lines = LOGO.read_text().split('\n')[:32]
    pad = '.' * 6
    lines = [s + pad + l for s, l in zip(star_lines, logo_lines)]
    cols = (max(len(l) for l in lines) + 7) // 8
    cells = bitmap_cells(lines, cols)
    logo_map = []
    for row in cells:
        out = []
        for cell in row:
            out.append(0x20 if cell == blank else alloc(cell, ord('*')))
        logo_map.append(out)

    with OUT.open('w') as f:
        f.write('/* Generated by tools/mkglyphs.py - do not edit. */\n')
        f.write('#ifndef GLYPHS_H\n#define GLYPHS_H\n\n')
        for n, c in defs:
            f.write(f'#define {n:<12} 0x{c:02X}\n')
        f.write(f'\n#define GLYPH_COUNT {len(glyphs)}\n')
        f.write(f'#define LOGO_W {cols}\n\n')
        f.write('#ifdef GLYPHS_IMPL\n')
        f.write('/* code, 16 bytes of bitmap */\n')
        f.write('static const u8 glyph_data[GLYPH_COUNT][17] = {\n')
        for code, data in glyphs:
            f.write('    {0x%02X, %s},\n' % (code, ','.join('0x%02X' % b for b in data)))
        f.write('};\n\n')
        f.write('/* non-VGA displays: custom code -> CP437 stand-in */\n')
        f.write('static const u8 glyph_fallback[GLYPH_COUNT][2] = {\n')
        for code, fb in fallback:
            f.write('    {0x%02X, 0x%02X},\n' % (code, fb))
        f.write('};\n\n')
        f.write('static const u8 icon_glyph[%d][2] = {\n' % len(ICON_ORDER))
        for n in ICON_ORDER:
            a, b = icon_codes[n]
            f.write(f'    {{0x{a:02X}, 0x{b:02X}}},  /* {n} */\n')
        f.write('};\n\n')
        f.write('static const u8 logo_map[2][LOGO_W] = {\n')
        for row in logo_map:
            f.write('    {%s},\n' % ','.join('0x%02X' % c for c in row))
        f.write('};\n#endif\n\n#endif\n')
    print(f'{len(glyphs)} glyphs, {len(free)} code points left, logo {cols} columns')
    # preview
    try:
        from PIL import Image
        font = {code: data for code, data in glyphs}
        img = Image.new('L', (cols * 8, 32 + 48), 0)
        for r in range(2):
            for c in range(cols):
                code = logo_map[r][c]
                if code in font:
                    for y, b in enumerate(font[code]):
                        for x in range(8):
                            if b & (0x80 >> x):
                                img.putpixel((c * 8 + x, r * 16 + y), 255)
        x = 0
        for n in ICON_ORDER:
            for k, code in enumerate(icon_codes[n]):
                for y, b in enumerate(font[code]):
                    for xx in range(8):
                        if b & (0x80 >> xx) and x + k * 8 + xx < img.width:
                            img.putpixel((x + k * 8 + xx, 40 + y), 255)
            x += 18
            if x + 16 > img.width:
                break
        img.resize((img.width * 4, img.height * 5), Image.NEAREST).save(HERE / 'glyphs-preview.png')
    except ImportError:
        pass


if __name__ == '__main__':
    main()
