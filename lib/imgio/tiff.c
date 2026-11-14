/* TIFF reader (bilevel, grayscale, palette, RGB and CMYK pictures stored in
 * strips or tiles; uncompressed, PackBits, LZW or Deflate) and writer
 * (LZW compressed RGB or palette pictures). */
#include "imgint.h"

/* ---- reader ---------------------------------------------------------------- */

typedef struct {
    RD *r;
    IMGIO *io;
    int be;                     /* big-endian file */
    int w, h, bps, spp, photo, pred, extra;
    int tw, x0, y, rows;        /* current strip / tile */
    unsigned rowbytes, have;
    uint8_t *row, *out;
    int fail;
} TIFD;

static unsigned t16(TIFD *t) { return t->be ? rd_be16(t->r) : rd_le16(t->r); }
static uint32_t t32(TIFD *t) { return t->be ? rd_be32(t->r) : rd_le32(t->r); }

/* Reads one element of a tag's value array (type 3 = SHORT, 4 = LONG). */
static uint32_t tval(TIFD *t, int type)
{
    return type == 3 ? t16(t) : type == 4 ? t32(t) : (uint32_t)rd_u8(t->r);
}

static int tiff_row(TIFD *t)
{
    uint8_t *p = t->row, *o = t->out;
    int n = t->tw, spp = t->spp, x, hi = t->be ? 0 : 1, a, c, m, k, max;
    unsigned i;

    if (t->x0 + n > t->w)
        n = t->w - t->x0;
    if (t->pred == 2) {
        if (t->bps == 8) {
            for (i = spp; i < t->rowbytes; i++)
                p[i] += p[i - spp];
        } else if (t->bps == 16) {
            for (i = spp * 2; i + 1 < t->rowbytes; i += 2) {
                unsigned v = (((unsigned)p[i + hi] << 8) | p[i + 1 - hi])
                           + (((unsigned)p[i - spp * 2 + hi] << 8) | p[i - spp * 2 + 1 - hi]);

                p[i + hi] = (uint8_t)(v >> 8);
                p[i + 1 - hi] = (uint8_t)v;
            }
        }
    }
    if (t->bps == 16) {                 /* keep the high byte of every sample */
        for (i = 0; i * 2 + 1 < t->rowbytes; i++)
            p[i] = p[i * 2 + hi];
    }
    switch (t->photo) {
    case 0: case 1: case 3:
        if (t->bps < 8) {
            img_unpack(p, t->bps, n, o);
        } else {
            for (x = 0; x < n; x++)
                o[x] = p[x * spp];
        }
        if (t->photo == 0) {
            max = t->bps < 8 ? (1 << t->bps) - 1 : 255;
            for (x = 0; x < n; x++)
                o[x] = (uint8_t)(max - o[x]);
        }
        break;
    case 2:
        for (x = 0; x < n; x++, p += spp, o += 3) {
            o[0] = p[2];
            o[1] = p[1];
            o[2] = p[0];
            if (t->extra && spp >= 4) {
                a = p[3];
                for (k = 0; k < 3; k++) {
                    c = t->extra == 2 ? (int)(((uint32_t)o[k] * a + 127) / 255) : o[k];
                    c += 255 - a;       /* onto white */
                    o[k] = (uint8_t)(c > 255 ? 255 : c);
                }
            }
        }
        break;
    default:                            /* CMYK */
        for (x = 0; x < n; x++, p += spp, o += 3) {
            m = 255 - p[3];
            o[0] = (uint8_t)(((uint32_t)(255 - p[2]) * m + 127) / 255);
            o[1] = (uint8_t)(((uint32_t)(255 - p[1]) * m + 127) / 255);
            o[2] = (uint8_t)(((uint32_t)(255 - p[0]) * m + 127) / 255);
        }
        break;
    }
    if (t->y < t->h && n > 0 && !t->io->put(t->io, t->y, t->x0, 1, n, t->out)) {
        t->fail = 1;
        return 0;
    }
    return 1;
}

/* Receives decompressed bytes; returns 0 once the strip / tile is complete. */
static int tiff_feed(TIFD *t, const uint8_t *data, unsigned n)
{
    unsigned k;

    while (n && t->rows > 0) {
        k = t->rowbytes - t->have;
        if (k > n)
            k = n;
        memcpy(t->row + t->have, data, k);
        t->have += k;
        data += k;
        n -= k;
        if (t->have < t->rowbytes)
            break;
        t->have = 0;
        if (!tiff_row(t)) {
            t->rows = 0;
            break;
        }
        t->y++;
        t->rows--;
    }
    return t->rows > 0;
}

static void tiff_raw(TIFD *t)
{
    uint8_t buf[256];
    unsigned n;

    do {
        n = rd_bytes(t->r, buf, sizeof(buf));
    } while (n && tiff_feed(t, buf, n));
}

static void tiff_packbits(TIFD *t)
{
    uint8_t buf[128];
    int c, n;

    while (!t->r->eof) {
        c = rd_u8(t->r);
        if (c < 128) {
            n = c + 1;
            if (rd_bytes(t->r, buf, n) != (unsigned)n)
                break;
        } else if (c > 128) {
            n = 257 - c;
            memset(buf, rd_u8(t->r), n);
        } else {
            continue;
        }
        if (!tiff_feed(t, buf, n))
            break;
    }
}

static void tiff_lzw(TIFD *t)
{
    uint16_t *prefix = img_alloc(4096 * sizeof(uint16_t));
    uint8_t *suffix = img_alloc(4096), *stack = img_alloc(4096);
    uint32_t bitbuf = 0;
    int bitcnt = 0, width = 9, next = 258, old = -1, code, c, first = 0, sp;
    uint8_t one;

    if (!prefix || !suffix || !stack) {
        t->fail = 1;
        goto done;
    }
    for (;;) {
        while (bitcnt < width) {
            bitbuf = (bitbuf << 8) | (uint32_t)rd_u8(t->r);
            bitcnt += 8;
        }
        code = (int)((bitbuf >> (bitcnt - width)) & ((1U << width) - 1));
        bitcnt -= width;
        if (code == 257 || t->r->eof)
            break;
        if (code == 256) {
            width = 9;
            next = 258;
            old = -1;
            continue;
        }
        if (old < 0) {
            if (code > 255)
                break;
            one = (uint8_t)code;
            first = code;
            old = code;
            if (!tiff_feed(t, &one, 1))
                break;
            continue;
        }
        sp = 4096;
        c = code;
        if (code >= next) {             /* the string being defined right now */
            if (code > next)
                break;
            stack[--sp] = (uint8_t)first;
            c = old;
        }
        while (c > 255 && sp > 0) {
            stack[--sp] = suffix[c];
            c = prefix[c];
        }
        if (sp == 0)
            break;
        stack[--sp] = (uint8_t)c;
        first = c;
        if (next < 4096) {
            prefix[next] = (uint16_t)old;
            suffix[next] = (uint8_t)first;
            next++;
            if (next == (1 << width) - 1 && width < 12)
                width++;
        }
        old = code;
        if (!tiff_feed(t, stack + sp, 4096 - sp))
            break;
    }
done:
    free(prefix);
    free(suffix);
    free(stack);
}

static int tiff_zfill(INFL *s)
{
    TIFD *t = s->user;
    RD *r = t->r;

    if (r->pos >= r->len && !rd_fill(r))
        return 0;
    s->ip = r->buf + r->pos;
    s->iend = r->buf + r->len;
    r->pos = r->len;
    return 1;
}

static int tiff_zflush(INFL *s, const uint8_t *data, unsigned n)
{
    return tiff_feed(s->user, data, n);
}

int tiff_load(RD *r)
{
    IMGIO *io = r->io;
    TIFD *t = malloc(sizeof(TIFD));
    IMGINFO *inf = malloc(sizeof(IMGINFO));
    uint32_t *offs = NULL;
    uint32_t count, val, ifd, cmap = 0, offpos = 0, noffs = 0;
    long rps = 0x7FFFFFFFL, rb, tw = 0, tl = 0, pos, across, k;
    int ntags, i, tag, type, comp = 1, planar = 1, offtype = 4, ok = 0, tiles = 0, n;
    INFL s;

    if (!t || !inf) {
        free(t);
        free(inf);
        return img_fail(io, "Not enough memory.");
    }
    memset(t, 0, sizeof(*t));
    memset(inf, 0, sizeof(*inf));
    t->r = r;
    t->io = io;
    t->be = r->buf[0] == 'M';
    t->bps = 1;
    t->spp = 1;
    t->pred = 1;
    rd_skip(r, 4);
    ifd = t32(t);
    if (!rd_seek(r, (long)ifd))
        goto done;
    ntags = t16(t);
    for (i = 0; i < ntags && !r->eof; i++) {
        tag = t16(t);
        type = t16(t);
        count = t32(t);
        pos = rd_tell(r);
        val = (type == 3) ? t16(t) : (type == 1) ? (uint32_t)rd_u8(r) : t32(t);
        switch (tag) {
        case 256: t->w = val > 32767 ? 32767 : (int)val; break;
        case 257: t->h = val > 32767 ? 32767 : (int)val; break;
        case 258:
            if (count > 2) {            /* values stored elsewhere: take the first */
                rd_seek(r, pos);
                rd_seek(r, (long)t32(t));
                val = t16(t);
            }
            t->bps = (int)val;
            break;
        case 259: comp = (int)val; break;
        case 262: t->photo = (int)val; break;
        case 273: case 324:
            noffs = count;
            offtype = type;
            rd_seek(r, pos);
            offpos = count == 1 ? (uint32_t)pos : t32(t);
            if (type == 3 && count == 2)
                offpos = (uint32_t)pos;
            tiles = tag == 324;
            break;
        case 277: t->spp = (int)val; break;
        case 278: rps = (long)val; break;
        case 284: planar = (int)val; break;
        case 317: t->pred = (int)val; break;
        case 320: rd_seek(r, pos); cmap = t32(t); break;
        case 322: tw = (long)val; break;
        case 323: tl = (long)val; break;
        case 338: t->extra = (int)val; break;
        }
        rd_seek(r, pos + 4);
    }

    if (t->w < 1 || t->h < 1 || !noffs || noffs > 16000)
        goto done;
    if (t->w > IMG_MAXDIM || t->h > IMG_MAXDIM) {
        img_fail(io, "The picture is too large.");
        goto done;
    }
    if (planar != 1 || (comp != 1 && comp != 5 && comp != 8 && comp != 32946U && comp != 32773U)
        || t->spp < 1 || t->spp > 8
        || !((t->photo <= 1 && (t->bps == 1 || t->bps == 2 || t->bps == 4 || t->bps == 8 || t->bps == 16))
             || (t->photo == 3 && t->spp == 1 && cmap && (t->bps == 1 || t->bps == 2 || t->bps == 4 || t->bps == 8))
             || (t->photo == 2 && t->spp >= 3 && (t->bps == 8 || t->bps == 16))
             || (t->photo == 5 && t->spp >= 4 && t->bps == 8))) {
        img_fail(io, "This kind of TIFF file is not supported.");
        goto done;
    }
    if (tiles && (tw < 1 || tl < 1 || tw > IMG_MAXDIM))
        goto done;
    if (!tiles) {
        tw = t->w;
        tl = (rps < 1 || rps > t->h) ? t->h : rps;
    }
    t->tw = (int)tw;
    rb = (tw * t->spp * t->bps + 7) / 8;
    t->rowbytes = (unsigned)rb;
    t->row = img_alloc(rb);
    t->out = img_alloc(tw * 3);
    offs = img_alloc((unsigned long)noffs * sizeof(uint32_t));
    if (!t->row || !t->out || !offs) {
        img_fail(io, rb > 65000L ? "The picture is too wide for this format." : "Not enough memory.");
        goto done;
    }
    rd_seek(r, (long)offpos);
    for (k = 0; k < (long)noffs; k++)
        offs[k] = tval(t, offtype);

    inf->width = t->w;
    inf->height = t->h;
    inf->format = IMG_TIFF;
    inf->bpp = 24;
    if (t->photo == 3) {
        n = 1 << t->bps;
        inf->indexed = 1;
        inf->ncolors = n;
        inf->bpp = t->bps <= 1 ? 1 : t->bps <= 4 ? 4 : 8;
        rd_seek(r, (long)cmap);
        for (i = 0; i < 3 * n; i++)
            inf->pal[i % n][i / n] = (uint8_t)(t16(t) >> 8);
    } else if (t->photo <= 1) {
        img_gray_pal(inf, t->bps < 8 ? 1 << t->bps : 256);
        inf->bpp = t->bps <= 1 ? 1 : t->bps <= 4 ? 4 : 8;
    }
    if (!img_begin(io, inf))
        goto done;

    across = tiles ? (t->w + tw - 1) / tw : 1;
    for (k = 0; k < (long)noffs && !t->fail; k++) {
        t->y = (int)((k / across) * tl);
        t->x0 = (int)((k % across) * tw);
        if (t->y >= t->h)
            break;
        t->rows = (int)tl;
        if (!tiles && t->rows > t->h - t->y)
            t->rows = t->h - t->y;
        t->have = 0;
        if (!rd_seek(r, (long)offs[k]))
            break;
        switch (comp) {
        case 1:  tiff_raw(t); break;
        case 5:  tiff_lzw(t); break;
        case 32773U: tiff_packbits(t); break;
        default:
            memset(&s, 0, sizeof(s));
            s.fill = tiff_zfill;
            s.flush = tiff_zflush;
            s.user = t;
            infl_run(&s, 1);
            break;
        }
    }
    ok = !t->fail;

done:
    free(offs);
    free(t->row);
    free(t->out);
    free(t);
    free(inf);
    return ok;
}

/* ---- writer ---------------------------------------------------------------- */

#define LZW_HASH 5003

typedef struct {
    WR *w;
    uint32_t *keys;
    uint16_t *codes;
    uint32_t bitbuf;
    int bitcnt, width, next, prefix;
} LZWE;

static void lzw_put(LZWE *e, int code)
{
    e->bitbuf = (e->bitbuf << e->width) | (uint32_t)code;
    e->bitcnt += e->width;
    while (e->bitcnt >= 8) {
        wr_u8(e->w, (int)((e->bitbuf >> (e->bitcnt - 8)) & 255));
        e->bitcnt -= 8;
    }
}

static void lzw_reset(LZWE *e)
{
    unsigned i;

    for (i = 0; i < LZW_HASH; i++)
        e->keys[i] = 0xFFFFFFFFUL;
    e->width = 9;
    e->next = 258;
}

static void lzw_begin(LZWE *e)
{
    e->bitbuf = 0;
    e->bitcnt = 0;
    e->prefix = -1;
    lzw_reset(e);
    lzw_put(e, 256);
}

static void lzw_grow(LZWE *e)
{
    e->next++;
    if (e->next == 4094) {
        lzw_put(e, 256);
        lzw_reset(e);
    } else if (e->next > (1 << e->width) - 1) {
        e->width++;
    }
}

static void lzw_write(LZWE *e, const uint8_t *p, unsigned n)
{
    uint32_t key;
    unsigned h;

    for (; n; n--, p++) {
        if (e->prefix < 0) {
            e->prefix = *p;
            continue;
        }
        key = ((uint32_t)e->prefix << 8) | *p;
        h = (unsigned)(key % LZW_HASH);
        while (e->keys[h] != 0xFFFFFFFFUL && e->keys[h] != key)
            h = h + 1 == LZW_HASH ? 0 : h + 1;
        if (e->keys[h] == key) {
            e->prefix = e->codes[h];
            continue;
        }
        lzw_put(e, e->prefix);
        e->keys[h] = key;
        e->codes[h] = (uint16_t)e->next;
        lzw_grow(e);
        e->prefix = *p;
    }
}

static void lzw_end(LZWE *e)
{
    if (e->prefix >= 0) {
        lzw_put(e, e->prefix);
        lzw_grow(e);
    }
    lzw_put(e, 257);
    if (e->bitcnt)
        wr_u8(e->w, (int)((e->bitbuf << (8 - e->bitcnt)) & 255));
}

static void tag(WR *w, int id, int type, uint32_t count, uint32_t value)
{
    wr_le16(w, id);
    wr_le16(w, type);
    wr_le32(w, count);
    if (type == 3 && count == 1) {
        wr_le16(w, (unsigned)value);
        wr_le16(w, 0);
    } else {
        wr_le32(w, value);
    }
}

int tiff_save(WR *w, IMGIO *io, const IMGINFO *info)
{
    SRC s;
    LZWE e;
    uint32_t *offs = NULL, *counts = NULL;
    uint32_t offpos, cntpos, bpspos = 0, cmappos = 0, respos, ifd;
    uint8_t *row = NULL;
    int width = info->width, height = info->height, x, y, i, k, ok = 0, ntags;
    long rps, nstrips, st;
    unsigned n;

    memset(&e, 0, sizeof(e));
    if (!src_open(&s, io, info, 1))
        goto done;
    n = s.eff.indexed ? (unsigned)width : (unsigned)width * 3;
    rps = 8192 / n;
    if (rps < 1)
        rps = 1;
    nstrips = (height + rps - 1) / rps;
    row = img_alloc((unsigned long)width * 3);
    offs = img_alloc(nstrips * sizeof(uint32_t));
    counts = img_alloc(nstrips * sizeof(uint32_t));
    e.keys = img_alloc(LZW_HASH * sizeof(uint32_t));
    e.codes = img_alloc(LZW_HASH * sizeof(uint16_t));
    e.w = w;
    if (!row || !offs || !counts || !e.keys || !e.codes) {
        img_fail(io, "Not enough memory.");
        goto done;
    }

    wr_u8(w, 'I');
    wr_u8(w, 'I');
    wr_le16(w, 42);
    wr_le32(w, 0);                              /* IFD offset, patched below */
    for (st = 0, y = 0; st < nstrips && !w->err; st++) {
        offs[st] = (uint32_t)wr_tell(w);
        lzw_begin(&e);
        for (i = 0; i < rps && y < height; i++, y++) {
            if (s.eff.indexed) {
                if (!src_idx(&s, y, row))
                    goto done;
            } else {
                if (!src_rgb(&s, y, row))
                    goto done;
                for (x = 0; x < width; x++) {
                    uint8_t c = row[x * 3];

                    row[x * 3] = row[x * 3 + 2];
                    row[x * 3 + 2] = c;
                }
                for (k = (int)n - 1; k >= 3; k--)       /* horizontal predictor */
                    row[k] -= row[k - 3];
            }
            lzw_write(&e, row, n);
        }
        lzw_end(&e);
        counts[st] = (uint32_t)wr_tell(w) - offs[st];
    }
    if (wr_tell(w) & 1)
        wr_u8(w, 0);

    offpos = (uint32_t)wr_tell(w);
    for (st = 0; st < nstrips; st++)
        wr_le32(w, offs[st]);
    cntpos = (uint32_t)wr_tell(w);
    for (st = 0; st < nstrips; st++)
        wr_le32(w, counts[st]);
    respos = (uint32_t)wr_tell(w);
    wr_le32(w, 72);
    wr_le32(w, 1);
    if (s.eff.indexed) {
        cmappos = (uint32_t)wr_tell(w);
        for (k = 0; k < 3; k++)
            for (i = 0; i < 256; i++)
                wr_le16(w, i < s.eff.ncolors ? s.eff.pal[i][k] * 257U : 0);
    } else {
        bpspos = (uint32_t)wr_tell(w);
        wr_le16(w, 8);
        wr_le16(w, 8);
        wr_le16(w, 8);
        wr_le16(w, 0);
    }

    ifd = (uint32_t)wr_tell(w);
    ntags = 14;
    wr_le16(w, ntags);
    tag(w, 256, 3, 1, width);
    tag(w, 257, 3, 1, height);
    if (s.eff.indexed)
        tag(w, 258, 3, 1, 8);
    else
        tag(w, 258, 3, 3, bpspos);
    tag(w, 259, 3, 1, 5);
    tag(w, 262, 3, 1, s.eff.indexed ? 3 : 2);
    tag(w, 273, 4, nstrips, nstrips == 1 ? offs[0] : offpos);
    tag(w, 277, 3, 1, s.eff.indexed ? 1 : 3);
    tag(w, 278, 3, 1, (uint32_t)rps);
    tag(w, 279, 4, nstrips, nstrips == 1 ? counts[0] : cntpos);
    tag(w, 282, 5, 1, respos);
    tag(w, 283, 5, 1, respos);
    tag(w, 284, 3, 1, 1);
    tag(w, 296, 3, 1, 2);
    if (s.eff.indexed)
        tag(w, 320, 3, 768, cmappos);
    else
        tag(w, 317, 3, 1, 2);
    wr_le32(w, 0);
    wr_seek(w, 4);
    wr_le32(w, ifd);
    ok = 1;

done:
    if (!ok)
        img_fail(io, "The picture could not be read.");
    free(row);
    free(offs);
    free(counts);
    free(e.keys);
    free(e.codes);
    src_close(&s);
    return ok;
}
