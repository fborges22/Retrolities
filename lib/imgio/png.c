/* PNG reader (all colour types, bit depths and interlacing; transparency is
 * blended onto white) and writer (8-bit RGB or palette). */
#include "imgint.h"

static uint32_t crctab[256];

static void crc_init(void)
{
    uint32_t c;
    int n, k;

    if (crctab[1])
        return;
    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crctab[n] = c;
    }
}

static uint32_t crc_update(uint32_t c, const uint8_t *p, unsigned n)
{
    while (n--)
        c = crctab[(c ^ *p++) & 255] ^ (c >> 8);
    return c;
}

/* ---- reader ---------------------------------------------------------------- */

typedef struct {
    RD *r;
    IMGIO *io;
    IMGINFO *info;
    long chunkleft;
    int w, h, ctype, depth, interlace, channels, fbpp;
    int pass, y, pw, ph;
    unsigned rowbytes, have;
    uint8_t *cur, *prev, *out;
    int haskey, done, fail;
    unsigned key[3];
} PNGD;

static const uint8_t px0[7] = { 0, 4, 0, 2, 0, 1, 0 }, py0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const uint8_t pdx[7] = { 8, 8, 4, 4, 2, 2, 1 }, pdy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static unsigned png_rowbytes(const PNGD *p, int pw)
{
    return (unsigned)(((long)pw * p->channels * p->depth + 7) / 8);
}

/* Moves to the next non-empty pass; returns 0 when the picture is complete. */
static int png_pass(PNGD *p)
{
    for (;;) {
        if (!p->interlace) {
            if (p->pass++ > 0)
                return 0;
            p->pw = p->w;
            p->ph = p->h;
        } else {
            if (p->pass >= 7)
                return 0;
            p->pw = (p->w - px0[p->pass] + pdx[p->pass] - 1) / pdx[p->pass];
            p->ph = (p->h - py0[p->pass] + pdy[p->pass] - 1) / pdy[p->pass];
            p->pass++;
        }
        if (p->pw > 0 && p->ph > 0)
            break;
    }
    p->y = 0;
    p->have = 0;
    p->rowbytes = png_rowbytes(p, p->pw);
    memset(p->prev, 0, p->rowbytes + 1);
    return 1;
}

static int blend(unsigned v, unsigned a)        /* over white */
{
    return (int)(((uint32_t)v * a + 255UL * (255 - a) + 127) / 255);
}

static int png_row(PNGD *p)
{
    uint8_t *c = p->cur + 1, *o = p->out;
    const uint8_t *b = p->prev + 1;
    unsigned i, n = p->rowbytes, bpp = p->fbpp;
    int x, pa, pb, pc, v, step = p->depth == 16 ? 2 : 1, a;

    switch (p->cur[0]) {
    case 1:
        for (i = bpp; i < n; i++) c[i] += c[i - bpp];
        break;
    case 2:
        for (i = 0; i < n; i++) c[i] += b[i];
        break;
    case 3:
        for (i = 0; i < bpp; i++) c[i] += b[i] >> 1;
        for (; i < n; i++) c[i] += (uint8_t)((c[i - bpp] + b[i]) >> 1);
        break;
    case 4:
        for (i = 0; i < bpp; i++) c[i] += b[i];
        for (; i < n; i++) {
            v = c[i - bpp] + b[i] - b[i - bpp];
            pa = abs(v - c[i - bpp]);
            pb = abs(v - b[i]);
            pc = abs(v - b[i - bpp]);
            c[i] += (pa <= pb && pa <= pc) ? c[i - bpp] : (pb <= pc) ? b[i] : b[i - bpp];
        }
        break;
    }

    switch (p->ctype) {
    case 0:
        if (p->depth <= 8)
            img_unpack(c, p->depth, p->pw, o);
        else
            for (x = 0; x < p->pw; x++) o[x] = c[x * 2];
        break;
    case 3:
        img_unpack(c, p->depth, p->pw, o);
        break;
    case 4:
        for (x = 0; x < p->pw; x++, c += 2 * step)
            o[x] = (uint8_t)blend(c[0], c[step]);
        break;
    case 2:
        for (x = 0; x < p->pw; x++, c += 3 * step, o += 3) {
            if (p->haskey && c[0] == p->key[0] && c[step] == p->key[1] && c[2 * step] == p->key[2]) {
                o[0] = o[1] = o[2] = 255;
            } else {
                o[0] = c[2 * step];
                o[1] = c[step];
                o[2] = c[0];
            }
        }
        break;
    default:
        for (x = 0; x < p->pw; x++, c += 4 * step, o += 3) {
            a = c[3 * step];
            o[0] = (uint8_t)blend(c[2 * step], a);
            o[1] = (uint8_t)blend(c[step], a);
            o[2] = (uint8_t)blend(c[0], a);
        }
        break;
    }
    if (p->interlace)
        return p->io->put(p->io, py0[p->pass - 1] + p->y * pdy[p->pass - 1],
                          px0[p->pass - 1], pdx[p->pass - 1], p->pw, p->out);
    return p->io->put(p->io, p->y, 0, 1, p->pw, p->out);
}

static int png_flush(INFL *s, const uint8_t *data, unsigned n)
{
    PNGD *p = s->user;
    unsigned k;
    uint8_t *t;

    while (n && !p->done) {
        k = p->rowbytes + 1 - p->have;
        if (k > n)
            k = n;
        memcpy(p->cur + p->have, data, k);
        p->have += k;
        data += k;
        n -= k;
        if (p->have < p->rowbytes + 1)
            break;
        if (!png_row(p)) {
            p->fail = p->done = 1;
            break;
        }
        t = p->cur;
        p->cur = p->prev;
        p->prev = t;
        p->have = 0;
        if (++p->y >= p->ph && !png_pass(p))
            p->done = 1;
    }
    return !p->done;
}

/* Hands out the contents of consecutive IDAT chunks. */
static int png_fill(INFL *s)
{
    PNGD *p = s->user;
    RD *r = p->r;
    unsigned n;

    while (p->chunkleft == 0) {
        rd_skip(r, 4);                          /* CRC of the previous chunk */
        p->chunkleft = (long)rd_be32(r);
        if (rd_be32(r) != 0x49444154UL || r->eof) {
            p->chunkleft = 0;
            return 0;
        }
    }
    if (r->pos >= r->len && !rd_fill(r))
        return 0;
    n = r->len - r->pos;
    if ((long)n > p->chunkleft)
        n = (unsigned)p->chunkleft;
    s->ip = r->buf + r->pos;
    s->iend = s->ip + n;
    r->pos += n;
    p->chunkleft -= n;
    return 1;
}

int png_load(RD *r)
{
    IMGIO *io = r->io;
    PNGD *p = malloc(sizeof(PNGD));
    IMGINFO *inf = malloc(sizeof(IMGINFO));
    INFL s;
    uint8_t alpha[256];
    uint32_t len, type;
    long maxrow;
    int i, n, nalpha = 0, ok = 0, gotpal = 0;

    if (!p || !inf) {
        free(p);
        free(inf);
        return img_fail(io, "Not enough memory.");
    }
    memset(p, 0, sizeof(*p));
    memset(inf, 0, sizeof(*inf));
    p->r = r;
    p->io = io;
    p->info = inf;
    rd_skip(r, 8);
    for (;;) {
        len = rd_be32(r);
        type = rd_be32(r);
        if (r->eof)
            goto done;
        if (type == 0x49484452UL) {             /* IHDR */
            uint32_t w = rd_be32(r), h = rd_be32(r);

            p->depth = rd_u8(r);
            p->ctype = rd_u8(r);
            rd_u8(r);
            rd_u8(r);
            p->interlace = rd_u8(r);
            rd_skip(r, 4);
            if (w > IMG_MAXDIM || h > IMG_MAXDIM) {
                img_fail(io, "The picture is too large.");
                goto done;
            }
            p->w = (int)w;
            p->h = (int)h;
        } else if (type == 0x504C5445UL) {      /* PLTE */
            n = (int)(len / 3);
            for (i = 0; i < n; i++) {
                uint8_t c[3];

                rd_bytes(r, c, 3);
                if (i < 256)
                    memcpy(inf->pal[i], c, 3);
            }
            rd_skip(r, (long)(len % 3) + 4);
            gotpal = 1;
        } else if (type == 0x74524E53UL) {      /* tRNS */
            if (p->ctype == 3) {
                nalpha = len > 256 ? 256 : (int)len;
                rd_bytes(r, alpha, nalpha);
                rd_skip(r, (long)len - nalpha + 4);
            } else {
                for (i = 0; i < 3 && (unsigned)(i * 2) < len; i++)
                    p->key[i] = rd_be16(r);
                p->haskey = 1;
                rd_skip(r, (long)len - i * 2 + 4);
            }
        } else if (type == 0x49444154UL) {      /* IDAT */
            p->chunkleft = (long)len;
            break;
        } else {
            rd_skip(r, (long)len + 4);
        }
    }

    switch (p->ctype) {
    case 0: p->channels = 1; break;
    case 2: p->channels = 3; break;
    case 3: p->channels = 1; break;
    case 4: p->channels = 2; break;
    case 6: p->channels = 4; break;
    default: goto done;
    }
    if (p->w < 1 || p->h < 1 || p->interlace > 1
        || (p->depth != 1 && p->depth != 2 && p->depth != 4 && p->depth != 8 && p->depth != 16)
        || (p->ctype != 0 && p->ctype != 3 && p->depth < 8) || (p->ctype == 3 && p->depth > 8)
        || (p->ctype == 3 && !gotpal))
        goto done;
    p->fbpp = p->channels * p->depth / 8;
    if (p->fbpp < 1)
        p->fbpp = 1;
    maxrow = ((long)p->w * p->channels * p->depth + 7) / 8 + 1;
    p->cur = img_alloc(maxrow);
    p->prev = img_alloc(maxrow);
    p->out = img_alloc((long)p->w * 3);
    if (!p->cur || !p->prev || !p->out) {
        img_fail(io, maxrow > 65000L ? "The picture is too wide for this format."
                                     : "Not enough memory.");
        goto done;
    }

    inf->width = p->w;
    inf->height = p->h;
    inf->format = IMG_PNG;
    inf->bpp = 24;
    if (p->ctype == 3) {
        inf->indexed = 1;
        inf->ncolors = 1 << p->depth;
        inf->bpp = p->depth <= 1 ? 1 : p->depth <= 4 ? 4 : 8;
        for (i = 0; i < nalpha; i++) {
            inf->pal[i][0] = (uint8_t)blend(inf->pal[i][0], alpha[i]);
            inf->pal[i][1] = (uint8_t)blend(inf->pal[i][1], alpha[i]);
            inf->pal[i][2] = (uint8_t)blend(inf->pal[i][2], alpha[i]);
        }
    } else if (p->ctype == 0 || p->ctype == 4) {
        img_gray_pal(inf, p->depth < 8 ? 1 << p->depth : 256);
        inf->bpp = 8;
        if (p->haskey && p->ctype == 0 && p->depth <= 8 && p->key[0] < (unsigned)inf->ncolors)
            inf->pal[p->key[0]][0] = inf->pal[p->key[0]][1] = inf->pal[p->key[0]][2] = 255;
    }
    if (p->depth == 16 || p->ctype != 2)
        p->haskey = 0;                          /* colour keys: 8-bit RGB only */
    if (!img_begin(io, inf))
        goto done;

    png_pass(p);
    memset(&s, 0, sizeof(s));
    s.fill = png_fill;
    s.flush = png_flush;
    s.user = p;
    infl_run(&s, 1);
    ok = !p->fail;      /* a truncated file keeps the rows decoded so far */

done:
    free(p->cur);
    free(p->prev);
    free(p->out);
    free(p);
    free(inf);
    return ok;
}

/* ---- writer ---------------------------------------------------------------- */

static void chunk(WR *w, const char *type, const uint8_t *data, unsigned n)
{
    uint32_t c;

    wr_be32(w, n);
    wr_bytes(w, (const uint8_t *)type, 4);
    wr_bytes(w, data, n);
    c = crc_update(0xFFFFFFFFUL, (const uint8_t *)type, 4);
    c = crc_update(c, data, n);
    wr_be32(w, c ^ 0xFFFFFFFFUL);
}

static int idat_out(void *user, const uint8_t *data, unsigned n)
{
    WR *w = user;

    chunk(w, "IDAT", data, n);
    return !w->err;
}

static long filter_row(int type, const uint8_t *c, const uint8_t *b, unsigned n, unsigned bpp, uint8_t *o)
{
    unsigned i;
    int a, bb, cc, v, pa, pb, pc;
    long sum = 0;

    for (i = 0; i < n; i++) {
        a = i >= bpp ? c[i - bpp] : 0;
        bb = b[i];
        cc = i >= bpp ? b[i - bpp] : 0;
        switch (type) {
        case 1: v = a; break;
        case 2: v = bb; break;
        case 3: v = (a + bb) >> 1; break;
        case 4:
            v = a + bb - cc;
            pa = abs(v - a);
            pb = abs(v - bb);
            pc = abs(v - cc);
            v = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? bb : cc;
            break;
        default: v = 0; break;
        }
        o[i] = (uint8_t)(c[i] - v);
        sum += o[i] < 128 ? o[i] : 256 - o[i];
    }
    return sum;
}

int png_save(WR *w, IMGIO *io, const IMGINFO *info)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    SRC s;
    DEFL *d = NULL;
    uint8_t hdr[13], *cur = NULL, *prev = NULL, *tmp = NULL, *best = NULL, *t, ftype;
    uint8_t (*pal)[3] = NULL;
    int width = info->width, height = info->height, depth = 8, x, y, f, bf, ok = 0;
    unsigned n;
    long sum, bsum;

    crc_init();
    if (!src_open(&s, io, info, 1))
        goto done;
    if (s.eff.indexed)
        depth = s.eff.ncolors <= 2 ? 1 : s.eff.ncolors <= 4 ? 2 : s.eff.ncolors <= 16 ? 4 : 8;
    n = s.eff.indexed ? (unsigned)(((long)width * depth + 7) / 8) : (unsigned)width * 3;
    cur = img_alloc((unsigned long)width * 3 + 1);
    prev = img_alloc((unsigned long)width * 3 + 1);
    tmp = img_alloc((unsigned long)width * 3 + 1);
    best = img_alloc((unsigned long)width * 3 + 1);
    pal = malloc(256 * 3);
    if (!cur || !prev || !tmp || !best || !pal) {
        img_fail(io, "Not enough memory.");
        goto done;
    }

    wr_bytes(w, sig, 8);
    hdr[0] = 0; hdr[1] = 0; hdr[2] = (uint8_t)(width >> 8); hdr[3] = (uint8_t)width;
    hdr[4] = 0; hdr[5] = 0; hdr[6] = (uint8_t)(height >> 8); hdr[7] = (uint8_t)height;
    hdr[8] = (uint8_t)depth;
    hdr[9] = s.eff.indexed ? 3 : 2;
    hdr[10] = hdr[11] = hdr[12] = 0;
    chunk(w, "IHDR", hdr, 13);
    if (s.eff.indexed) {
        memcpy(pal, s.eff.pal, 256 * 3);
        chunk(w, "PLTE", (const uint8_t *)pal, s.eff.ncolors * 3);
    }
    d = defl_new(idat_out, w);
    if (!d) {
        img_fail(io, "Not enough memory.");
        goto done;
    }
    memset(prev, 0, n);
    for (y = 0; y < height && !w->err; y++) {
        if (s.eff.indexed) {
            if (!src_idx(&s, y, tmp))
                goto done;
            img_pack(tmp, depth, width, cur);
            ftype = 0;
            defl_write(d, &ftype, 1);
            defl_write(d, cur, n);
            continue;
        }
        if (!src_rgb(&s, y, cur))
            goto done;
        for (x = 0; x < width; x++) {           /* B, G, R -> R, G, B */
            uint8_t c = cur[x * 3];

            cur[x * 3] = cur[x * 3 + 2];
            cur[x * 3 + 2] = c;
        }
        bf = 0;
        bsum = 0x7FFFFFFFL;
        for (f = 0; f < 5; f++) {               /* keep the filter with the smallest output */
            sum = filter_row(f, cur, prev, n, 3, tmp);
            if (sum < bsum) {
                bsum = sum;
                bf = f;
                t = best;
                best = tmp;
                tmp = t;
            }
        }
        ftype = (uint8_t)bf;
        defl_write(d, &ftype, 1);
        defl_write(d, best, n);
        t = cur;
        cur = prev;
        prev = t;
    }
    ok = defl_end(d);
    d = NULL;
    chunk(w, "IEND", hdr, 0);

done:
    if (d)
        defl_end(d);
    if (!ok)
        img_fail(io, "The picture could not be read.");
    free(cur);
    free(prev);
    free(tmp);
    free(best);
    free(pal);
    src_close(&s);
    return ok;
}
