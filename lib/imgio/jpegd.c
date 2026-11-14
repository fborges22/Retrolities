/* JPEG decoder: baseline, extended and progressive Huffman coding, 8 bits per
 * sample; grayscale, YCbCr, RGB, CMYK and YCCK pictures. */
#include "imgint.h"

#define CHUNK_BLOCKS 256        /* coefficient blocks per allocation (32 KB) */

static const uint8_t zigzag[64 + 16] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};

typedef struct {
    uint8_t look_len[256], look_sym[256];
    long maxcode[18];
    int valptr[17];
    unsigned mincode[17];
    uint8_t vals[256];
} JHUF;

typedef struct {
    int id, h, v, tq, td, ta;
    int dcpred;
    int bw, bh;                 /* blocks actually covering the picture */
    int mbw, mbh;               /* blocks in the padded MCU grid */
    uint8_t *lines[32];         /* decoded samples of one MCU row */
    int16_t **chunks;           /* coefficient storage (multi-scan files) */
    long nchunks;
} JCOMP;

typedef struct {
    RD *r;
    IMGIO *io;
    uint32_t bitbuf;
    int bitcnt, marker, fail;
    int w, h, ncomp, hmax, vmax, mcux, mcuy;
    int progressive, restart, adobe;
    int ss, se, ah, al, eobrun;
    JCOMP comp[4];
    uint16_t qt[4][64];
    JHUF *huff[8];              /* 0-3 DC, 4-7 AC */
    int16_t crr[256], cbb[256], crg[256], cbg[256];
    uint8_t *out, *up[2];
} JD;

/* ---- bit stream ------------------------------------------------------------ */

static void jd_fillbits(JD *j)
{
    int c;

    while (j->bitcnt <= 24) {
        if (j->marker) {
            c = 0;
        } else {
            c = rd_u8(j->r);
            if (c == 0xFF) {
                int m = rd_u8(j->r);

                while (m == 0xFF)
                    m = rd_u8(j->r);
                if (m != 0) {
                    j->marker = m;
                    c = 0;
                }
            }
            if (j->r->eof)
                j->marker = 0xD9;
        }
        j->bitbuf = (j->bitbuf << 8) | (uint32_t)c;
        j->bitcnt += 8;
    }
}

static unsigned jd_bits(JD *j, int n)
{
    if (n == 0)
        return 0;
    if (j->bitcnt < n)
        jd_fillbits(j);
    j->bitcnt -= n;
    return (unsigned)((j->bitbuf >> j->bitcnt) & ((1UL << n) - 1));
}

static int jd_bit(JD *j)
{
    if (j->bitcnt < 1)
        jd_fillbits(j);
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jd_extend(JD *j, int s)
{
    unsigned v;

    if (s == 0)
        return 0;
    if (s > 15) {
        j->fail = 1;
        return 0;
    }
    v = jd_bits(j, s);
    if (v < (1U << (s - 1)))
        return (int)v - (int)((1U << s) - 1);
    return (int)v;
}

static int jd_huff(JD *j, const JHUF *h)
{
    unsigned code;
    int l, n;

    if (!h) {
        j->fail = 1;
        return 0;
    }
    if (j->bitcnt < 16)
        jd_fillbits(j);
    code = (unsigned)((j->bitbuf >> (j->bitcnt - 16)) & 0xFFFF);
    n = h->look_len[code >> 8];
    if (n) {
        j->bitcnt -= n;
        return h->look_sym[code >> 8];
    }
    for (l = 9; l <= 16; l++)
        if ((long)(code >> (16 - l)) <= h->maxcode[l]) {
            j->bitcnt -= l;
            return h->vals[(h->valptr[l] + (int)((code >> (16 - l)) - h->mincode[l])) & 255];
        }
    j->fail = 1;
    return 0;
}

static int jd_dht(JD *j, long len)
{
    uint8_t bits[17];
    JHUF *h;
    int tc, i, l, k, n;
    unsigned code;

    while (len >= 17) {
        tc = rd_u8(j->r);
        n = 0;
        for (i = 1; i <= 16; i++) {
            bits[i] = (uint8_t)rd_u8(j->r);
            n += bits[i];
        }
        len -= 17 + n;
        i = ((tc >> 4) & 1) * 4 + (tc & 3);
        if (n > 256 || (tc & 0xEC))
            return 0;
        if (!j->huff[i])
            j->huff[i] = malloc(sizeof(JHUF));
        h = j->huff[i];
        if (!h)
            return 0;
        rd_bytes(j->r, h->vals, n);
        memset(h->look_len, 0, 256);
        code = 0;
        k = 0;
        for (l = 1; l <= 16; l++) {
            h->valptr[l] = k;
            h->mincode[l] = code;
            for (i = 0; i < bits[l]; i++, k++, code++)
                if (l <= 8) {
                    unsigned first = code << (8 - l), cnt = 1U << (8 - l);

                    if (first + cnt > 256)
                        return 0;
                    memset(h->look_len + first, l, cnt);
                    memset(h->look_sym + first, h->vals[k], cnt);
                }
            h->maxcode[l] = bits[l] ? (long)code - 1 : -1;
            code <<= 1;
        }
        h->maxcode[17] = 0xFFFFFL;
    }
    return 1;
}

/* ---- inverse DCT ----------------------------------------------------------- */

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    p2 = s2; p3 = s6; p1 = (p2 + p3) * 2217L; \
    t2 = p1 + p3 * -7568L; t3 = p1 + p2 * 3135L; \
    p2 = s0; p3 = s4; t0 = (p2 + p3) * 4096L; t1 = (p2 - p3) * 4096L; \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; p5 = (p3 + p4) * 4816L; \
    t0 *= 1223L; t1 *= 8410L; t2 *= 12586L; t3 *= 6149L; \
    p1 = p5 + p1 * -3686L; p2 = p5 + p2 * -10498L; p3 *= -8034L; p4 *= -1598L; \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static uint8_t clamp8(long v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The block being transformed lives in static (near) data: this is the
 * hottest loop and far pointers are costly in 16-bit code. */
static int16_t jblk[64];
static uint16_t jqt[64];

static void jd_idct(uint8_t **lines, unsigned xoff)
{
    const int16_t *d = jblk;
    const uint16_t *q = jqt;
    long val[64], *v = val;
    long t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;
    uint8_t *o;
    int i;

    for (i = 0; i < 8; i++, d++, q++, v++) {
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 && d[40] == 0 && d[48] == 0 && d[56] == 0) {
            long dc = (long)d[0] * q[0] * 4;

            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D((long)d[0] * q[0], (long)d[8] * q[8], (long)d[16] * q[16], (long)d[24] * q[24],
                    (long)d[32] * q[32], (long)d[40] * q[40], (long)d[48] * q[48], (long)d[56] * q[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10;
            v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10;
            v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10;
            v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10;
            v[32] = (x3 - t0) >> 10;
        }
    }
    for (i = 0, v = val; i < 8; i++, v += 8) {
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536L + (128L << 17);
        x1 += 65536L + (128L << 17);
        x2 += 65536L + (128L << 17);
        x3 += 65536L + (128L << 17);
        o = lines[i] + xoff;
        o[0] = clamp8((x0 + t3) >> 17);
        o[7] = clamp8((x0 - t3) >> 17);
        o[1] = clamp8((x1 + t2) >> 17);
        o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17);
        o[5] = clamp8((x2 - t1) >> 17);
        o[3] = clamp8((x3 + t0) >> 17);
        o[4] = clamp8((x3 - t0) >> 17);
    }
}

/* ---- block decoding -------------------------------------------------------- */

/* Sequential: a whole block (blk must be zeroed). */
static void jd_block_seq(JD *j, JCOMP *c, int16_t *blk)
{
    const JHUF *ac = j->huff[4 + c->ta];
    int k, rs, s, r;

    s = jd_huff(j, j->huff[c->td]);
    c->dcpred += jd_extend(j, s);
    blk[0] = (int16_t)c->dcpred;
    for (k = 1; k < 64 && !j->fail; ) {
        rs = jd_huff(j, ac);
        s = rs & 15;
        r = rs >> 4;
        if (s == 0) {
            if (r != 15)
                break;
            k += 16;
        } else {
            k += r;
            blk[zigzag[k++]] = (int16_t)jd_extend(j, s);
        }
    }
}

static void jd_block_dc(JD *j, JCOMP *c, int16_t *blk)
{
    if (j->ah == 0) {
        int s = jd_huff(j, j->huff[c->td]);

        c->dcpred += jd_extend(j, s);
        blk[0] = (int16_t)((long)c->dcpred * (1L << j->al));
    } else if (jd_bit(j)) {
        blk[0] |= (int16_t)(1 << j->al);
    }
}

static void jd_block_ac(JD *j, JCOMP *c, int16_t *blk)
{
    const JHUF *ac = j->huff[4 + c->ta];
    int k = j->ss, rs, s, r, bit = 1 << j->al;
    int16_t *p;

    if (j->ah == 0) {
        if (j->eobrun) {
            j->eobrun--;
            return;
        }
        while (k <= j->se && !j->fail) {
            rs = jd_huff(j, ac);
            s = rs & 15;
            r = rs >> 4;
            if (s == 0) {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r)
                        j->eobrun += jd_bits(j, r);
                    break;
                }
                k += 16;
            } else {
                k += r;
                blk[zigzag[k++]] = (int16_t)((long)jd_extend(j, s) * bit);
            }
        }
        return;
    }
    /* refinement */
    if (j->eobrun) {
        j->eobrun--;
        for (; k <= j->se; k++) {
            p = &blk[zigzag[k]];
            if (*p && jd_bit(j) && !(*p & bit))
                *p += (int16_t)(*p > 0 ? bit : -bit);
        }
        return;
    }
    while (k <= j->se && !j->fail) {
        rs = jd_huff(j, ac);
        s = rs & 15;
        r = rs >> 4;
        if (s == 0) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r)
                    j->eobrun += jd_bits(j, r);
                r = 64;                 /* finish the block */
            }
        } else {
            s = jd_bit(j) ? bit : -bit;
        }
        while (k <= j->se) {
            p = &blk[zigzag[k++]];
            if (*p) {
                if (jd_bit(j) && !(*p & bit))
                    *p += (int16_t)(*p > 0 ? bit : -bit);
            } else {
                if (r == 0) {
                    *p = (int16_t)s;
                    break;
                }
                r--;
            }
        }
    }
}

static int16_t *jd_coef(JCOMP *c, int bx, int by)
{
    long idx = (long)by * c->mbw + bx;

    return c->chunks[idx / CHUNK_BLOCKS] + (unsigned)(idx % CHUNK_BLOCKS) * 64;
}

/* Handles a restart marker; called between MCUs. */
static void jd_restart(JD *j)
{
    int i, m;

    j->bitcnt = 0;
    j->bitbuf = 0;
    if (!j->marker) {                   /* not seen yet: look for it */
        m = rd_u8(j->r);
        while (!j->r->eof) {
            if (m == 0xFF) {
                m = rd_u8(j->r);
                if (m != 0 && m != 0xFF) {
                    j->marker = m;
                    break;
                }
            } else {
                m = rd_u8(j->r);
            }
        }
    }
    if (j->marker >= 0xD0 && j->marker <= 0xD7)
        j->marker = 0;
    for (i = 0; i < 4; i++)
        j->comp[i].dcpred = 0;
    j->eobrun = 0;
}

/* ---- output ---------------------------------------------------------------- */

static int jd_output(JD *j, int my)
{
    int l, y, x, k, w = j->w, cr, cb, yy, v;
    const uint8_t *s[4];
    uint8_t *o;
    int half[4];

    for (l = 0; l < j->vmax * 8; l++) {
        y = my * j->vmax * 8 + l;
        if (y >= j->h)
            break;
        for (k = 0; k < j->ncomp; k++) {
            s[k] = j->comp[k].lines[l * j->comp[k].v / j->vmax];
            half[k] = j->hmax / j->comp[k].h;
            if (half[k] == 2 && k > 0 && k < 3) {       /* smooth 2:1 chroma */
                const uint8_t *in = s[k];
                uint8_t *up = j->up[k - 1];
                int n = (w + 1) / 2;

                for (x = 0; x < n; x++) {
                    v = 3 * in[x] + 2;
                    up[2 * x] = (uint8_t)((v + in[x > 0 ? x - 1 : 0]) >> 2);
                    up[2 * x + 1] = (uint8_t)((v + in[x + 1 < n ? x + 1 : x]) >> 2);
                }
                s[k] = up;
                half[k] = 1;
            }
        }
        if (j->ncomp == 1) {
            if (!j->io->put(j->io, y, 0, 1, w, s[0]))
                return 0;
            continue;
        }
        o = j->out;
        for (x = 0; x < w; x++, o += 3) {
            yy = half[0] == 1 ? s[0][x] : s[0][x / half[0]];
            cb = half[1] == 1 ? s[1][x] : half[1] == 2 ? s[1][x >> 1] : s[1][x / half[1]];
            cr = half[2] == 1 ? s[2][x] : half[2] == 2 ? s[2][x >> 1] : s[2][x / half[2]];
            if (j->ncomp == 3 && j->adobe == 0) {       /* plain RGB */
                o[0] = (uint8_t)cr;
                o[1] = (uint8_t)cb;
                o[2] = (uint8_t)yy;
                continue;
            }
            if (j->ncomp == 4 && j->adobe != 2) {       /* CMYK, stored inverted */
                o[2] = (uint8_t)yy;
                o[1] = (uint8_t)cb;
                o[0] = (uint8_t)cr;
            } else {
                v = yy + j->crr[cr];
                o[2] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                v = yy + ((j->cbg[cb] + j->crg[cr]) >> 6);
                o[1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                v = yy + j->cbb[cb];
                o[0] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            if (j->ncomp == 4 && j->adobe == 2)         /* YCCK */
                for (k = 0; k < 3; k++)
                    o[k] = (uint8_t)(255 - o[k]);
            if (j->ncomp == 4) {
                unsigned kk = half[3] == 1 ? s[3][x] : s[3][x / half[3]];

                for (k = 0; k < 3; k++)
                    o[k] = (uint8_t)(((uint32_t)o[k] * kk + 127) / 255);
            }
        }
        if (!j->io->put(j->io, y, 0, 1, w, j->out))
            return 0;
    }
    return 1;
}

/* ---- scans ----------------------------------------------------------------- */

/* Single-scan baseline pictures are decoded and written out MCU row by row. */
static int jd_scan_stream(JD *j)
{
    int mx, my, k, bx, by, count = 0;
    JCOMP *c;

    for (my = 0; my < j->mcuy; my++) {
        for (mx = 0; mx < j->mcux; mx++) {
            if (j->restart && count == j->restart) {
                jd_restart(j);
                count = 0;
            }
            count++;
            for (k = 0; k < j->ncomp; k++) {
                c = &j->comp[k];
                memcpy(jqt, j->qt[c->tq], sizeof(jqt));
                for (by = 0; by < c->v; by++)
                    for (bx = 0; bx < c->h; bx++) {
                        memset(jblk, 0, sizeof(jblk));
                        jd_block_seq(j, c, jblk);
                        jd_idct(c->lines + by * 8, (unsigned)(mx * c->h + bx) * 8);
                    }
            }
            if (j->fail)
                return 0;
        }
        if (!jd_output(j, my))
            return 0;
    }
    return 1;
}

/* Multi-scan pictures: the scan's coefficients are stored for later. */
static void jd_scan_store(JD *j, int ns, const int *which)
{
    int mx, my, k, bx, by, count = 0;
    JCOMP *c;
    int16_t *blk;

    j->eobrun = 0;
    if (ns == 1) {
        c = &j->comp[which[0]];
        for (by = 0; by < c->bh; by++)
            for (bx = 0; bx < c->bw; bx++) {
                if (j->restart && count == j->restart) {
                    jd_restart(j);
                    count = 0;
                }
                count++;
                blk = jd_coef(c, bx, by);
                if (!j->progressive)
                    jd_block_seq(j, c, blk);
                else if (j->ss == 0)
                    jd_block_dc(j, c, blk);
                else
                    jd_block_ac(j, c, blk);
                if (j->fail)
                    return;
            }
        return;
    }
    for (my = 0; my < j->mcuy; my++)
        for (mx = 0; mx < j->mcux; mx++) {
            if (j->restart && count == j->restart) {
                jd_restart(j);
                count = 0;
            }
            count++;
            for (k = 0; k < ns; k++) {
                c = &j->comp[which[k]];
                for (by = 0; by < c->v; by++)
                    for (bx = 0; bx < c->h; bx++) {
                        blk = jd_coef(c, mx * c->h + bx, my * c->v + by);
                        if (!j->progressive)
                            jd_block_seq(j, c, blk);
                        else
                            jd_block_dc(j, c, blk);
                    }
            }
            if (j->fail)
                return;
        }
}

/* Turns the stored coefficients into pixels. */
static int jd_render(JD *j)
{
    int my, k, bx, by;
    JCOMP *c;

    for (my = 0; my < j->mcuy; my++) {
        for (k = 0; k < j->ncomp; k++) {
            c = &j->comp[k];
            memcpy(jqt, j->qt[c->tq], sizeof(jqt));
            for (by = 0; by < c->v; by++)
                for (bx = 0; bx < c->mbw; bx++) {
                    memcpy(jblk, jd_coef(c, bx, my * c->v + by), sizeof(jblk));
                    jd_idct(c->lines + by * 8, (unsigned)bx * 8);
                }
        }
        if (!jd_output(j, my))
            return 0;
    }
    return 1;
}

/* ---- headers --------------------------------------------------------------- */

static int jd_frame(JD *j)
{
    JCOMP *c;
    int i, l;
    unsigned w, h;

    if (rd_u8(j->r) != 8)
        return img_fail(j->io, "JPEG files with 12 bits per sample are not supported.");
    h = rd_be16(j->r);
    w = rd_be16(j->r);
    j->ncomp = rd_u8(j->r);
    if (w < 1 || h < 1 || (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4))
        return 0;
    if (w > IMG_MAXDIM || h > IMG_MAXDIM)
        return img_fail(j->io, "The picture is too large.");
    j->w = (int)w;
    j->h = (int)h;
    j->hmax = j->vmax = 1;
    for (i = 0; i < j->ncomp; i++) {
        c = &j->comp[i];
        c->id = rd_u8(j->r);
        l = rd_u8(j->r);
        c->h = l >> 4;
        c->v = l & 15;
        c->tq = rd_u8(j->r) & 3;
        if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
            return 0;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    if (j->ncomp == 1)
        j->comp[0].h = j->comp[0].v = j->hmax = j->vmax = 1;
    j->mcux = (int)(((long)j->w + 8L * j->hmax - 1) / (8L * j->hmax));
    j->mcuy = (int)(((long)j->h + 8L * j->vmax - 1) / (8L * j->vmax));
    for (i = 0; i < j->ncomp; i++) {
        c = &j->comp[i];
        c->bw = (int)((((long)j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8);
        c->bh = (int)((((long)j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8);
        c->mbw = j->mcux * c->h;
        c->mbh = j->mcuy * c->v;
        for (l = 0; l < c->v * 8; l++) {
            c->lines[l] = img_alloc((unsigned long)c->mbw * 8);
            if (!c->lines[l])
                return img_fail(j->io, "Not enough memory.");
        }
    }
    j->out = img_alloc((unsigned long)j->w * 3);
    j->up[0] = img_alloc((unsigned long)j->w + 2);
    j->up[1] = img_alloc((unsigned long)j->w + 2);
    if (!j->out || !j->up[0] || !j->up[1])
        return img_fail(j->io, "Not enough memory.");
    return 1;
}

static int jd_alloc_coefs(JD *j)
{
    JCOMP *c;
    int i;
    long k;

    for (i = 0; i < j->ncomp; i++) {
        c = &j->comp[i];
        c->nchunks = ((long)c->mbw * c->mbh + CHUNK_BLOCKS - 1) / CHUNK_BLOCKS;
        c->chunks = img_alloc(c->nchunks * sizeof(int16_t *));
        if (!c->chunks)
            return 0;
        memset(c->chunks, 0, (unsigned)(c->nchunks * sizeof(int16_t *)));
        for (k = 0; k < c->nchunks; k++) {
            c->chunks[k] = calloc(CHUNK_BLOCKS, 64 * sizeof(int16_t));
            if (!c->chunks[k])
                return 0;
        }
    }
    return 1;
}

static int jd_marker(JD *j)
{
    int c;

    if (j->marker) {
        c = j->marker;
        j->marker = 0;
        return c;
    }
    for (;;) {
        c = rd_u8(j->r);
        if (j->r->eof)
            return 0xD9;
        if (c != 0xFF)
            continue;
        do {
            c = rd_u8(j->r);
        } while (c == 0xFF && !j->r->eof);
        if (c != 0)
            return c;
    }
}

int jpeg_load(RD *r)
{
    IMGIO *io = r->io;
    JD *j = malloc(sizeof(JD));
    IMGINFO *inf = malloc(sizeof(IMGINFO));
    int which[4];
    int m, i, k, ns, t, ok = 0, begun = 0, stored = 0, gotframe = 0;
    long len;

    if (!j || !inf) {
        free(j);
        free(inf);
        return img_fail(io, "Not enough memory.");
    }
    memset(j, 0, sizeof(*j));
    memset(inf, 0, sizeof(*inf));
    j->r = r;
    j->io = io;
    j->adobe = -1;
    for (i = 0; i < 256; i++) {
        j->crr[i] = (int16_t)((359L * (i - 128) + 128) >> 8);
        j->cbb[i] = (int16_t)((454L * (i - 128) + 128) >> 8);
        j->crg[i] = (int16_t)(-46L * (i - 128) + 32);          /* / 64 */
        j->cbg[i] = (int16_t)(-22L * (i - 128));
    }
    rd_skip(r, 2);
    for (;;) {
        m = jd_marker(j);
        if (m == 0xD9)
            break;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01)
            continue;
        len = (long)rd_be16(r) - 2;
        if (len < 0)
            break;
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2:
            if (gotframe || !jd_frame(j))
                goto done;
            gotframe = 1;
            j->progressive = m == 0xC2;
            break;

        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
        case 0xCD: case 0xCE: case 0xCF:
            img_fail(io, "This kind of JPEG file (lossless or arithmetic coded) is not supported.");
            goto done;

        case 0xC4:
            if (!jd_dht(j, len))
                goto done;
            break;

        case 0xDB:
            while (len >= 65) {
                t = rd_u8(r);
                for (i = 0; i < 64; i++)
                    j->qt[t & 3][zigzag[i]] = (uint16_t)((t >> 4) ? rd_be16(r) : (unsigned)rd_u8(r));
                len -= 65 + ((t >> 4) ? 64 : 0);
            }
            break;

        case 0xDD:
            j->restart = (int)rd_be16(r);
            break;

        case 0xEE:
            if (len >= 12) {
                uint8_t app[12];

                rd_bytes(r, app, 12);
                if (!memcmp(app, "Adobe", 5))
                    j->adobe = app[11];
                rd_skip(r, len - 12);
            } else {
                rd_skip(r, len);
            }
            break;

        case 0xDA:
            if (!gotframe)
                goto done;
            ns = rd_u8(r);
            if (ns < 1 || ns > j->ncomp)
                goto done;
            for (i = 0; i < ns; i++) {
                int id = rd_u8(r);

                t = rd_u8(r);
                for (k = 0; k < j->ncomp && j->comp[k].id != id; k++)
                    ;
                if (k == j->ncomp)
                    goto done;
                which[i] = k;
                j->comp[k].td = (t >> 4) & 3;
                j->comp[k].ta = t & 3;
            }
            j->ss = rd_u8(r);
            j->se = rd_u8(r);
            t = rd_u8(r);
            j->ah = t >> 4;
            j->al = t & 15;
            if (j->se > 63 || j->ss > j->se || j->al > 13)
                goto done;
            if (!begun) {
                inf->width = j->w;
                inf->height = j->h;
                inf->format = IMG_JPEG;
                inf->bpp = 24;
                if (j->ncomp == 1) {
                    img_gray_pal(inf, 256);
                    inf->bpp = 8;
                }
                if (!img_begin(io, inf))
                    goto done;
                begun = 1;
            }
            for (i = 0; i < 4; i++)
                j->comp[i].dcpred = 0;
            j->bitcnt = 0;
            j->bitbuf = 0;
            j->marker = 0;
            if (!stored && !j->progressive && ns == j->ncomp) {
                ok = jd_scan_stream(j);
                goto done;
            }
            if (!stored) {
                if (!jd_alloc_coefs(j)) {
                    img_fail(io, "Not enough memory to decode this JPEG file.");
                    goto done;
                }
                stored = 1;
            }
            jd_scan_store(j, ns, which);
            j->fail = 0;                /* a damaged scan still leaves a picture */
            continue;

        default:
            rd_skip(r, len);
            break;
        }
        if (r->eof)
            break;
    }
    if (stored)
        ok = jd_render(j);

done:
    for (i = 0; i < 4; i++) {
        JCOMP *c = &j->comp[i];

        for (k = 0; k < 32; k++)
            free(c->lines[k]);
        if (c->chunks) {
            for (len = 0; len < c->nchunks; len++)
                free(c->chunks[len]);
            free(c->chunks);
        }
    }
    for (i = 0; i < 8; i++)
        free(j->huff[i]);
    free(j->out);
    free(j->up[0]);
    free(j->up[1]);
    free(j);
    free(inf);
    return ok;
}
