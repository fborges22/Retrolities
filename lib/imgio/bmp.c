/* Windows / OS/2 bitmaps. */
#include "imgint.h"

static const uint8_t vga16[16][3] = {
    { 0, 0, 0 }, { 128, 0, 0 }, { 0, 128, 0 }, { 128, 128, 0 },
    { 0, 0, 128 }, { 128, 0, 128 }, { 0, 128, 128 }, { 128, 128, 128 },
    { 192, 192, 192 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 },
    { 0, 0, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 }
};

typedef struct { uint32_t mask; int shift, bits; } FIELD;

static void field_init(FIELD *f, uint32_t mask)
{
    f->mask = mask;
    f->shift = 0;
    f->bits = 0;
    if (!mask)
        return;
    while (!((mask >> f->shift) & 1))
        f->shift++;
    while (f->shift + f->bits < 32 && ((mask >> (f->shift + f->bits)) & 1))
        f->bits++;
}

static uint8_t field_get(const FIELD *f, uint32_t v)
{
    uint32_t x = (v & f->mask) >> f->shift;

    if (f->bits == 0)
        return 0;
    if (f->bits >= 8)
        return (uint8_t)(x >> (f->bits - 8));
    return (uint8_t)(x * 255 / ((1UL << f->bits) - 1));
}

static int bmp_rle(RD *r, IMGIO *io, int w, int h, int bpp, uint8_t *out)
{
    int x = 0, y = 0, c, v, i, k;

    memset(out, 0, w);
    while (y < h) {
        c = rd_u8(r);
        v = rd_u8(r);
        if (r->eof)
            break;
        if (c > 0) {                            /* run */
            for (i = 0; i < c; i++, x++)
                if (x < w)
                    out[x] = (uint8_t)(bpp == 8 ? v : (i & 1) ? (v & 15) : (v >> 4));
        } else if (v == 0 || v == 1) {          /* end of line / bitmap */
            if (!io->put(io, h - 1 - y, 0, 1, w, out))
                return 0;
            memset(out, 0, w);
            x = 0;
            y++;
            if (v == 1)
                break;
        } else if (v == 2) {                    /* move right / up */
            int dx = rd_u8(r), dy = rd_u8(r);

            while (dy-- > 0 && y < h) {
                if (!io->put(io, h - 1 - y, 0, 1, w, out))
                    return 0;
                memset(out, 0, w);
                y++;
            }
            x += dx;
        } else {                                /* literal pixels */
            if (bpp == 8) {
                for (i = 0; i < v; i++, x++) {
                    k = rd_u8(r);
                    if (x < w)
                        out[x] = (uint8_t)k;
                }
                if (v & 1)
                    rd_u8(r);
            } else {
                k = 0;
                for (i = 0; i < v; i++, x++) {
                    if (!(i & 1))
                        k = rd_u8(r);
                    if (x < w)
                        out[x] = (uint8_t)((i & 1) ? (k & 15) : (k >> 4));
                }
                if (((v + 1) / 2) & 1)
                    rd_u8(r);
            }
        }
    }
    return 1;
}

int bmp_load(RD *r)
{
    IMGIO *io = r->io;
    IMGINFO *inf = malloc(sizeof(IMGINFO));
    uint8_t *row = NULL, *out = NULL;
    FIELD f[3];
    uint32_t hsize, offbits, comp = 0, clrused = 0, v;
    long w, h, rowbytes;
    int bpp, core = 0, topdown = 0, n, i, x, y, ok = 0;

    if (!inf)
        return img_fail(io, "Not enough memory.");
    memset(inf, 0, sizeof(*inf));
    rd_skip(r, 10);
    offbits = rd_le32(r);
    hsize = rd_le32(r);
    if (hsize == 12) {
        w = rd_le16(r);
        h = rd_le16(r);
        rd_le16(r);
        bpp = rd_le16(r);
        core = 1;
    } else if (hsize >= 40) {
        w = (int32_t)rd_le32(r);
        h = (int32_t)rd_le32(r);
        rd_le16(r);
        bpp = rd_le16(r);
        comp = rd_le32(r);
        rd_skip(r, 12);
        clrused = rd_le32(r);
        rd_le32(r);
        hsize -= 40;
        field_init(&f[0], bpp == 16 ? 0x7C00UL : 0xFF0000UL);
        field_init(&f[1], bpp == 16 ? 0x03E0UL : 0x00FF00UL);
        field_init(&f[2], bpp == 16 ? 0x001FUL : 0x0000FFUL);
        if (comp == 3 || comp == 6) {           /* BI_BITFIELDS */
            for (i = 0; i < 3; i++)
                field_init(&f[i], rd_le32(r));
            if (hsize >= 12)
                hsize -= 12;
            comp = 0;
            if (bpp != 16 && bpp != 32)
                goto done;
        }
        rd_skip(r, hsize);
    } else {
        goto done;
    }
    if (h < 0) {
        topdown = 1;
        h = -h;
    }
    if (r->eof || w < 1 || h < 1
        || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        || !(comp == 0 || (comp == 1 && bpp == 8) || (comp == 2 && bpp == 4)))
        goto done;
    if (w > IMG_MAXDIM || h > IMG_MAXDIM) {
        img_fail(io, "The picture is too large.");
        goto done;
    }
    if (bpp <= 8) {
        n = clrused ? (int)(clrused > 256 ? 256 : clrused) : 1 << bpp;
        for (i = 0; i < n; i++) {
            inf->pal[i][2] = (uint8_t)rd_u8(r);
            inf->pal[i][1] = (uint8_t)rd_u8(r);
            inf->pal[i][0] = (uint8_t)rd_u8(r);
            if (!core)
                rd_u8(r);
        }
        inf->indexed = 1;
        inf->ncolors = 1 << bpp;
    }
    inf->width = (int)w;
    inf->height = (int)h;
    inf->format = IMG_BMP;
    inf->bpp = bpp <= 1 ? 1 : bpp <= 4 ? 4 : bpp <= 8 ? 8 : 24;
    if (offbits)
        rd_seek(r, (long)offbits);
    rowbytes = (w * bpp + 31) / 32 * 4;
    row = img_alloc(rowbytes);
    out = img_alloc(w * 3);
    if (!row || !out) {
        img_fail(io, "Not enough memory.");
        goto done;
    }
    if (!img_begin(io, inf))
        goto done;

    if (comp) {
        ok = bmp_rle(r, io, (int)w, (int)h, bpp, out);
        goto done;
    }
    for (i = 0; i < h; i++) {
        if (rd_bytes(r, row, (unsigned)rowbytes) != (unsigned)rowbytes)
            break;                              /* truncated: keep what we have */
        y = topdown ? i : (int)h - 1 - i;
        switch (bpp) {
        case 1: case 4: case 8:
            img_unpack(row, bpp, (int)w, out);
            break;
        case 24:
            memcpy(out, row, (unsigned)(w * 3));
            break;
        default:
            for (x = 0; x < w; x++) {
                if (bpp == 16)
                    v = row[x * 2] | ((uint32_t)row[x * 2 + 1] << 8);
                else
                    v = row[x * 4] | ((uint32_t)row[x * 4 + 1] << 8)
                      | ((uint32_t)row[x * 4 + 2] << 16) | ((uint32_t)row[x * 4 + 3] << 24);
                out[x * 3] = field_get(&f[2], v);
                out[x * 3 + 1] = field_get(&f[1], v);
                out[x * 3 + 2] = field_get(&f[0], v);
            }
            break;
        }
        if (!io->put(io, y, 0, 1, (int)w, out))
            goto done;
    }
    ok = 1;

done:
    free(row);
    free(out);
    free(inf);
    return ok;
}

static int nearest(const uint8_t pal[][3], int n, const uint8_t *c)
{
    long best = 0x7FFFFFFFL, d;
    int i, k = 0, dr, dg, db;

    for (i = 0; i < n; i++) {
        dr = pal[i][0] - c[0];
        dg = pal[i][1] - c[1];
        db = pal[i][2] - c[2];
        d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (d < best) {
            best = d;
            k = i;
        }
    }
    return k;
}

int bmp_save(WR *w, IMGIO *io, const IMGINFO *info, int bpp)
{
    static const uint8_t bw[2][3] = { { 0, 0, 0 }, { 255, 255, 255 } };
    SRC s;
    uint8_t (*pal)[3] = NULL;
    uint8_t map[256], *row = NULL, *out = NULL;
    int width = info->width, height = info->height, ncol, i, x, y, ok = 0, remap = 0;
    long rowbytes;
    uint32_t tablesize;

    if (bpp != 1 && bpp != 4 && bpp != 8)
        bpp = 24;
    if (!src_open(&s, io, info, bpp == 24 ? 0 : 2))
        goto done;
    ncol = bpp == 24 ? 0 : 1 << bpp;
    rowbytes = ((long)width * bpp + 31) / 32 * 4;
    row = img_alloc((unsigned long)width * 3);
    out = img_alloc(rowbytes + 4);
    pal = malloc(256 * 3);
    if (!row || !out || !pal) {
        img_fail(io, "Not enough memory.");
        goto done;
    }
    memset(pal, 0, 256 * 3);
    if (bpp != 24) {
        if (s.eff.ncolors <= ncol) {
            memcpy(pal, s.eff.pal, s.eff.ncolors * 3);
        } else {                        /* reduce to the fixed 16 / 2 colours */
            remap = 1;
            memcpy(pal, bpp == 4 ? vga16 : bw, ncol * 3);
            for (i = 0; i < s.eff.ncolors; i++) {
                const uint8_t *c = s.eff.pal[i];

                if (bpp == 4)
                    map[i] = (uint8_t)nearest(vga16, 16, c);
                else
                    map[i] = (uint8_t)((c[0] * 30 + c[1] * 59 + c[2] * 11) / 100 >= 128);
            }
        }
    }
    tablesize = (uint32_t)ncol * 4;
    wr_u8(w, 'B');
    wr_u8(w, 'M');
    wr_le32(w, 54 + tablesize + (uint32_t)rowbytes * height);
    wr_le32(w, 0);
    wr_le32(w, 54 + tablesize);
    wr_le32(w, 40);
    wr_le32(w, width);
    wr_le32(w, height);
    wr_le16(w, 1);
    wr_le16(w, bpp);
    wr_le32(w, 0);
    wr_le32(w, (uint32_t)rowbytes * height);
    wr_le32(w, 2835);
    wr_le32(w, 2835);
    wr_le32(w, 0);
    wr_le32(w, 0);
    for (i = 0; i < ncol; i++) {
        wr_u8(w, pal[i][2]);
        wr_u8(w, pal[i][1]);
        wr_u8(w, pal[i][0]);
        wr_u8(w, 0);
    }
    memset(out, 0, (unsigned)rowbytes + 4);
    for (y = height - 1; y >= 0 && !w->err; y--) {
        if (bpp == 24) {
            if (!src_rgb(&s, y, out))
                goto done;
        } else {
            if (!src_idx(&s, y, row))
                goto done;
            if (remap)
                for (x = 0; x < width; x++)
                    row[x] = map[row[x]];
            img_pack(row, bpp, width, out);
        }
        wr_bytes(w, out, (unsigned)rowbytes);
    }
    ok = 1;

done:
    if (!ok)
        img_fail(io, "The picture could not be read.");
    free(row);
    free(out);
    free(pal);
    src_close(&s);
    return ok;
}
