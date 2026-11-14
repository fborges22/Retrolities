/* Colour reduction by median cut over a 5-5-4 bit RGB histogram. */
#include "imgint.h"

#define RB 5
#define GB 5
#define BB 4
#define CELLS (1U << (RB + GB + BB))
#define CELL(r, g, b) (((unsigned)(r) << (GB + BB)) | ((unsigned)(g) << BB) | (unsigned)(b))

typedef struct {
    uint8_t lo[3], hi[3];       /* inclusive cell ranges: r, g, b */
    uint32_t count;
} BOX;

struct IMGQUANT {
    uint16_t *hist;             /* saturating pixel counts */
    uint8_t *map;               /* cell -> palette index */
};

IMGQUANT *imgq_new(void)
{
    IMGQUANT *q = malloc(sizeof(*q));

    if (!q)
        return NULL;
    q->hist = img_alloc(CELLS * sizeof(uint16_t));
    q->map = img_alloc(CELLS);
    if (!q->hist || !q->map) {
        imgq_free(q);
        return NULL;
    }
    memset(q->hist, 0, CELLS * sizeof(uint16_t));
    memset(q->map, 0, CELLS);
    return q;
}

void imgq_free(IMGQUANT *q)
{
    free(q->hist);
    free(q->map);
    free(q);
}

void imgq_add(IMGQUANT *q, const uint8_t *bgr, int n)
{
    unsigned c;

    for (; n > 0; n--, bgr += 3) {
        c = CELL(bgr[2] >> (8 - RB), bgr[1] >> (8 - GB), bgr[0] >> (8 - BB));
        if (q->hist[c] != 0xFFFF)
            q->hist[c]++;
    }
}

void imgq_map(const IMGQUANT *q, const uint8_t *bgr, int n, uint8_t *idx)
{
    for (; n > 0; n--, bgr += 3)
        *idx++ = q->map[CELL(bgr[2] >> (8 - RB), bgr[1] >> (8 - GB), bgr[0] >> (8 - BB))];
}

/* Shrinks a box to the cells in use and totals its pixels. */
static void shrink(const IMGQUANT *q, BOX *b)
{
    uint8_t lo[3], hi[3];
    int r, g, bl, k, c[3];
    unsigned n;

    for (k = 0; k < 3; k++) {
        lo[k] = 255;
        hi[k] = 0;
    }
    b->count = 0;
    for (r = b->lo[0]; r <= b->hi[0]; r++)
        for (g = b->lo[1]; g <= b->hi[1]; g++)
            for (bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                n = q->hist[CELL(r, g, bl)];
                if (!n)
                    continue;
                b->count += n;
                c[0] = r; c[1] = g; c[2] = bl;
                for (k = 0; k < 3; k++) {
                    if (c[k] < lo[k]) lo[k] = (uint8_t)c[k];
                    if (c[k] > hi[k]) hi[k] = (uint8_t)c[k];
                }
            }
    if (b->count)
        for (k = 0; k < 3; k++) {
            b->lo[k] = lo[k];
            b->hi[k] = hi[k];
        }
}

/* Splits box b at the median of its longest axis; the upper part goes to nb. */
static int split(const IMGQUANT *q, BOX *b, BOX *nb)
{
    static const int scale[3] = { 1 << (8 - RB), 1 << (8 - GB), 1 << (8 - BB) };
    uint32_t sums[32], half, acc;
    int axis = 0, best = -1, k, r, g, bl, c[3], cut;

    for (k = 0; k < 3; k++) {
        int len = (b->hi[k] - b->lo[k]) * scale[k];

        if (len > best) {
            best = len;
            axis = k;
        }
    }
    if (best <= 0)
        return 0;
    memset(sums, 0, sizeof(sums));
    for (r = b->lo[0]; r <= b->hi[0]; r++)
        for (g = b->lo[1]; g <= b->hi[1]; g++)
            for (bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                c[0] = r; c[1] = g; c[2] = bl;
                sums[c[axis]] += q->hist[CELL(r, g, bl)];
            }
    half = b->count / 2;
    acc = 0;
    for (cut = b->lo[axis]; cut < b->hi[axis]; cut++) {
        acc += sums[cut];
        if (acc >= half)
            break;
    }
    if (cut >= b->hi[axis])
        cut = b->hi[axis] - 1;
    *nb = *b;
    b->hi[axis] = (uint8_t)cut;
    nb->lo[axis] = (uint8_t)(cut + 1);
    shrink(q, b);
    shrink(q, nb);
    return 1;
}

int imgq_build(IMGQUANT *q, int maxcolors, uint8_t pal[][3])
{
    BOX *boxes = malloc(256 * sizeof(BOX));
    int n = 1, i, k, pick, r, g, bl;
    uint32_t best, sum[3], cnt;

    if (maxcolors > 256)
        maxcolors = 256;
    if (!boxes) {
        pal[0][0] = pal[0][1] = pal[0][2] = 0;
        return 1;
    }
    boxes[0].lo[0] = boxes[0].lo[1] = boxes[0].lo[2] = 0;
    boxes[0].hi[0] = (1 << RB) - 1;
    boxes[0].hi[1] = (1 << GB) - 1;
    boxes[0].hi[2] = (1 << BB) - 1;
    shrink(q, &boxes[0]);
    while (n < maxcolors) {
        pick = -1;
        best = 0;
        for (i = 0; i < n; i++) {       /* most populated box that can be split */
            BOX *b = &boxes[i];

            if (b->count > best && (b->lo[0] != b->hi[0] || b->lo[1] != b->hi[1] || b->lo[2] != b->hi[2])) {
                best = b->count;
                pick = i;
            }
        }
        if (pick < 0 || !split(q, &boxes[pick], &boxes[n]))
            break;
        n++;
    }
    for (i = 0; i < n; i++) {
        const BOX *b = &boxes[i];

        sum[0] = sum[1] = sum[2] = cnt = 0;
        for (r = b->lo[0]; r <= b->hi[0]; r++)
            for (g = b->lo[1]; g <= b->hi[1]; g++)
                for (bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                    uint32_t c = q->hist[CELL(r, g, bl)];

                    if (c)
                        c = (c >> 7) + 1;       /* keeps the sums within 32 bits */
                    q->map[CELL(r, g, bl)] = (uint8_t)i;
                    cnt += c;
                    sum[0] += c * (uint32_t)((r << (8 - RB)) | (r >> (2 * RB - 8)));
                    sum[1] += c * (uint32_t)((g << (8 - GB)) | (g >> (2 * GB - 8)));
                    sum[2] += c * (uint32_t)((bl << (8 - BB)) | bl);
                }
        for (k = 0; k < 3; k++)
            pal[i][k] = (uint8_t)(cnt ? (sum[k] + cnt / 2) / cnt : 0);
    }
    free(boxes);
    return n;
}
