/* Baseline JPEG encoder (JFIF): grayscale or YCbCr, 4:4:4 at high quality
 * settings and 4:2:0 below, standard Huffman tables. */
#include "imgint.h"

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t qlum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};
static const uint8_t qchr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

/* Standard tables of ISO 10918-1 annex K: code counts per length, then values. */
static const uint8_t dc_lum[] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t dc_chr[] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_lum[] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };
static const uint8_t ac_chr[] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };

typedef struct { uint16_t code[256]; uint8_t size[256]; } EHUF;

typedef struct {
    WR *w;
    uint32_t bitbuf;
    int bitcnt;
    uint16_t q[2][64];          /* zigzag order, already multiplied by 8 */
    EHUF huff[4];               /* DC lum, DC chroma, AC lum, AC chroma */
    int16_t yr[256], yg[256], yb[256], cbr[256], cbg[256], half[256], crg[256], crb[256];
    uint8_t *lines[3][16];
    int dc[3];
} JE;

static void je_bits(JE *e, unsigned code, int n)
{
    int c;

    e->bitbuf = (e->bitbuf << n) | (uint32_t)code;
    e->bitcnt += n;
    while (e->bitcnt >= 8) {
        c = (int)((e->bitbuf >> (e->bitcnt - 8)) & 255);
        wr_u8(e->w, c);
        if (c == 0xFF)
            wr_u8(e->w, 0);
        e->bitcnt -= 8;
    }
}

static void je_table(EHUF *h, const uint8_t *spec)
{
    unsigned code = 0;
    int l, i, k = 0;

    memset(h, 0, sizeof(*h));
    for (l = 1; l <= 16; l++) {
        for (i = 0; i < spec[l - 1]; i++, k++) {
            h->code[spec[16 + k]] = (uint16_t)code++;
            h->size[spec[16 + k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

static void je_dht(WR *w, int id, const uint8_t *spec)
{
    int i, n = 0;

    for (i = 0; i < 16; i++)
        n += spec[i];
    wr_be16(w, 0xFFC4);
    wr_be16(w, 19 + n);
    wr_u8(w, id);
    wr_bytes(w, spec, 16 + n);
}

#define DESCALE(x, n) (((x) + (1L << ((n) - 1))) >> (n))

/* Work areas in static (near) data: far pointers are costly in 16-bit code. */
static long fdct[64];
static uint16_t fq[64];

static void je_fdct(void)
{
    long *data = fdct;
    long t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z1, z2, z3, z4, z5;
    long *d;
    int i, pass, s;

    for (pass = 0; pass < 2; pass++) {
        s = pass ? 8 : 1;
        for (i = 0, d = data; i < 8; i++, d += pass ? 1 : 8) {
            t0 = d[0] + d[7 * s]; t7 = d[0] - d[7 * s];
            t1 = d[s] + d[6 * s]; t6 = d[s] - d[6 * s];
            t2 = d[2 * s] + d[5 * s]; t5 = d[2 * s] - d[5 * s];
            t3 = d[3 * s] + d[4 * s]; t4 = d[3 * s] - d[4 * s];
            t10 = t0 + t3; t13 = t0 - t3;
            t11 = t1 + t2; t12 = t1 - t2;
            z1 = (t12 + t13) * 4433L;
            if (!pass) {
                d[0] = (t10 + t11) * 4;
                d[4 * s] = (t10 - t11) * 4;
                d[2 * s] = DESCALE(z1 + t13 * 6270L, 11);
                d[6 * s] = DESCALE(z1 - t12 * 15137L, 11);
            } else {
                d[0] = DESCALE(t10 + t11, 2);
                d[4 * s] = DESCALE(t10 - t11, 2);
                d[2 * s] = DESCALE(z1 + t13 * 6270L, 15);
                d[6 * s] = DESCALE(z1 - t12 * 15137L, 15);
            }
            z1 = t4 + t7; z2 = t5 + t6; z3 = t4 + t6; z4 = t5 + t7;
            z5 = (z3 + z4) * 9633L;
            t4 *= 2446L; t5 *= 16819L; t6 *= 25172L; t7 *= 12299L;
            z1 *= -7373L; z2 *= -20995L; z3 *= -16069L; z4 *= -3196L;
            z3 += z5; z4 += z5;
            d[7 * s] = DESCALE(t4 + z1 + z3, pass ? 15 : 11);
            d[5 * s] = DESCALE(t5 + z2 + z4, pass ? 15 : 11);
            d[3 * s] = DESCALE(t6 + z2 + z3, pass ? 15 : 11);
            d[s] = DESCALE(t7 + z1 + z4, pass ? 15 : 11);
        }
    }
}

/* Transforms, quantises and entropy-codes one block of samples. */
static void je_block(JE *e, uint8_t **lines, unsigned x, int comp)
{
    static int loaded = -1;
    const EHUF *dc = &e->huff[comp ? 1 : 0], *ac = &e->huff[comp ? 3 : 2];
    long v, qv;
    int zz[64], i, k, run, n, t;
    uint8_t px[8];

    if (loaded != (comp ? 1 : 0) || x == 0) {
        loaded = comp ? 1 : 0;
        memcpy(fq, e->q[loaded], sizeof(fq));
    }
    for (i = 0; i < 8; i++) {
        memcpy(px, lines[i] + x, 8);
        for (k = 0; k < 8; k++)
            fdct[i * 8 + k] = (long)px[k] - 128;
    }
    je_fdct();
    for (i = 0; i < 64; i++) {
        v = fdct[zigzag[i]];
        qv = fq[i];
        if (v < 0)
            v = -v;
        v += qv >> 1;
        if (v < qv) {                   /* most coefficients quantise to zero */
            zz[i] = 0;
            continue;
        }
        k = (int)(v / qv);
        zz[i] = fdct[zigzag[i]] < 0 ? -k : k;
    }

    t = zz[0] - e->dc[comp];
    e->dc[comp] = zz[0];
    k = t < 0 ? -t : t;
    for (n = 0; k; n++)
        k >>= 1;
    je_bits(e, dc->code[n], dc->size[n]);
    if (n)
        je_bits(e, (unsigned)(t < 0 ? t - 1 : t) & ((1U << n) - 1), n);

    run = 0;
    for (i = 1; i < 64; i++) {
        t = zz[i];
        if (t == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            je_bits(e, ac->code[0xF0], ac->size[0xF0]);
            run -= 16;
        }
        k = t < 0 ? -t : t;
        for (n = 0; k; n++)
            k >>= 1;
        je_bits(e, ac->code[run * 16 + n], ac->size[run * 16 + n]);
        je_bits(e, (unsigned)(t < 0 ? t - 1 : t) & ((1U << n) - 1), n);
        run = 0;
    }
    if (run)
        je_bits(e, ac->code[0], ac->size[0]);
}

int jpeg_save(WR *w, IMGIO *io, const IMGINFO *info, int quality)
{
    SRC s;
    JE *e = malloc(sizeof(JE));
    uint8_t *row = NULL;
    int width = info->width, height = info->height;
    int gray = 0, sub, mcu, ncomp, i, k, x, y, l, my, ok = 0;
    unsigned wpad, bx;
    long scale, v;

    memset(&s, 0, sizeof(s));
    if (!e)
        return img_fail(io, "Not enough memory.");
    memset(e, 0, sizeof(*e));
    e->w = w;
    if (quality < 1 || quality > 100)
        quality = 90;
    if (!src_open(&s, io, info, 0))
        goto done;
    if (info->indexed) {                /* a gray palette becomes a grayscale file */
        gray = 1;
        for (i = 0; i < info->ncolors; i++)
            if (info->pal[i][0] != info->pal[i][1] || info->pal[i][1] != info->pal[i][2])
                gray = 0;
    }
    ncomp = gray ? 1 : 3;
    sub = !gray && quality < 90;        /* 4:2:0 chroma */
    mcu = sub ? 16 : 8;
    wpad = (unsigned)((width + mcu - 1) / mcu) * mcu;
    row = img_alloc((unsigned long)width * 3);
    for (k = 0; k < ncomp; k++)
        for (l = 0; l < mcu; l++) {
            e->lines[k][l] = img_alloc(wpad);
            if (!e->lines[k][l])
                row = (free(row), NULL);
        }
    if (!row) {
        img_fail(io, "Not enough memory.");
        goto done;
    }

    scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (i = 0; i < 64; i++)
        for (k = 0; k < 2; k++) {
            v = ((k ? qchr : qlum)[zigzag[i]] * scale + 50) / 100;
            e->q[k][i] = (uint16_t)(v < 1 ? 1 : v > 255 ? 255 : v);
        }
    for (i = 0; i < 256; i++) {         /* colour conversion, scaled by 64 */
        e->yr[i] = (int16_t)((1225L * i + 32) >> 6);
        e->yg[i] = (int16_t)((2404L * i + 32) >> 6);
        e->yb[i] = (int16_t)((467L * i + 32) >> 6);
        e->cbr[i] = (int16_t)(-((691L * i + 32) >> 6));
        e->cbg[i] = (int16_t)(-((1357L * i + 32) >> 6));
        e->half[i] = (int16_t)(32 * i);
        e->crg[i] = (int16_t)(-((1715L * i + 32) >> 6));
        e->crb[i] = (int16_t)(-((333L * i + 32) >> 6));
    }
    je_table(&e->huff[0], dc_lum);
    je_table(&e->huff[1], dc_chr);
    je_table(&e->huff[2], ac_lum);
    je_table(&e->huff[3], ac_chr);

    wr_be16(w, 0xFFD8);
    wr_be16(w, 0xFFE0);
    wr_be16(w, 16);
    wr_bytes(w, (const uint8_t *)"JFIF", 5);
    wr_be16(w, 0x0101);
    wr_u8(w, 0);
    wr_be16(w, 1);
    wr_be16(w, 1);
    wr_be16(w, 0);
    for (k = 0; k < (gray ? 1 : 2); k++) {
        wr_be16(w, 0xFFDB);
        wr_be16(w, 67);
        wr_u8(w, k);
        for (i = 0; i < 64; i++)
            wr_u8(w, e->q[k][i]);
    }
    wr_be16(w, 0xFFC0);
    wr_be16(w, 8 + 3 * ncomp);
    wr_u8(w, 8);
    wr_be16(w, height);
    wr_be16(w, width);
    wr_u8(w, ncomp);
    for (k = 0; k < ncomp; k++) {
        wr_u8(w, k + 1);
        wr_u8(w, (k == 0 && sub) ? 0x22 : 0x11);
        wr_u8(w, k ? 1 : 0);
    }
    je_dht(w, 0x00, dc_lum);
    je_dht(w, 0x10, ac_lum);
    if (!gray) {
        je_dht(w, 0x01, dc_chr);
        je_dht(w, 0x11, ac_chr);
    }
    wr_be16(w, 0xFFDA);
    wr_be16(w, 6 + 2 * ncomp);
    wr_u8(w, ncomp);
    for (k = 0; k < ncomp; k++) {
        wr_u8(w, k + 1);
        wr_u8(w, k ? 0x11 : 0x00);
    }
    wr_u8(w, 0);
    wr_u8(w, 63);
    wr_u8(w, 0);
    for (i = 0; i < 64; i++)            /* the DCT output is scaled by 8 */
        for (k = 0; k < 2; k++)
            e->q[k][i] *= 8;

    for (my = 0; my < height && !w->err; my += mcu) {
        for (l = 0; l < mcu; l++) {     /* fetch and convert one MCU row */
            y = my + l < height ? my + l : height - 1;
            if (gray) {
                if (!s.io->get(s.io, y, row))
                    goto done;
                for (x = 0; x < width; x++)
                    e->lines[0][l][x] = info->pal[row[x]][0];
            } else {
                uint8_t *py = e->lines[0][l], *pb = e->lines[1][l], *pr = e->lines[2][l];
                const uint8_t *p = row;

                if (!src_rgb(&s, y, row))
                    goto done;
                for (x = 0; x < width; x++, p += 3) {
                    py[x] = (uint8_t)((e->yr[p[2]] + e->yg[p[1]] + e->yb[p[0]] + 32) >> 6);
                    pb[x] = (uint8_t)((e->cbr[p[2]] + e->cbg[p[1]] + e->half[p[0]] + 8223) >> 6);
                    pr[x] = (uint8_t)((e->half[p[2]] + e->crg[p[1]] + e->crb[p[0]] + 8223) >> 6);
                }
            }
            for (k = 0; k < ncomp; k++)
                memset(e->lines[k][l] + width, e->lines[k][l][width - 1], wpad - width);
        }
        if (sub) {                      /* average the chroma 2x2, in place */
            for (k = 1; k < 3; k++)
                for (l = 0; l < 8; l++) {
                    uint8_t *a = e->lines[k][l * 2], *b = e->lines[k][l * 2 + 1], *o = e->lines[k][l];

                    for (bx = 0; bx < wpad / 2; bx++)
                        o[bx] = (uint8_t)((a[bx * 2] + a[bx * 2 + 1] + b[bx * 2] + b[bx * 2 + 1] + 2) >> 2);
                }
            for (bx = 0; bx < wpad; bx += 16) {
                je_block(e, e->lines[0], bx, 0);
                je_block(e, e->lines[0], bx + 8, 0);
                je_block(e, e->lines[0] + 8, bx, 0);
                je_block(e, e->lines[0] + 8, bx + 8, 0);
                je_block(e, e->lines[1], bx / 2, 1);
                je_block(e, e->lines[2], bx / 2, 2);
            }
        } else {
            for (bx = 0; bx < wpad; bx += 8)
                for (k = 0; k < ncomp; k++)
                    je_block(e, e->lines[k], bx, k);
        }
    }
    if (e->bitcnt)
        je_bits(e, (1U << (8 - e->bitcnt)) - 1, 8 - e->bitcnt);
    wr_be16(w, 0xFFD9);
    ok = 1;

done:
    if (!ok)
        img_fail(io, "The picture could not be read.");
    for (k = 0; k < 3; k++)
        for (l = 0; l < 16; l++)
            free(e->lines[k][l]);
    free(row);
    free(e);
    src_close(&s);
    return ok;
}
