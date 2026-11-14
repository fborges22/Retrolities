/* Deflate compression: LZ77 with hash chains over independent 16 KB blocks,
 * each coded with its own (dynamic) Huffman tables. */
#include "imgint.h"

#define BLOCK   16384U
#define HBITS   12
#define HSIZE   (1U << HBITS)
#define OBUF    4096
#define CHAIN   24
#define NIL     0xFFFF

typedef struct {
    uint16_t freq[288];
    uint16_t code[288];
    uint8_t len[288];
} TREE;

struct DEFL {
    int (*out)(void *user, const uint8_t *data, unsigned n);
    void *user;
    uint8_t *in;
    uint16_t *head, *prev;
    uint16_t *tlit, *tdist;     /* tokens: literal or length, distance (0 = literal) */
    TREE *lt, *dt, *bt;
    unsigned inlen, opos, ntok;
    uint32_t bitbuf, a, b;
    int bitcnt, err;
    uint8_t obuf[OBUF];
};

static void outbyte(DEFL *d, int c)
{
    d->obuf[d->opos++] = (uint8_t)c;
    if (d->opos == OBUF) {
        if (!d->out(d->user, d->obuf, OBUF))
            d->err = 1;
        d->opos = 0;
    }
}

static void putbits(DEFL *d, unsigned v, int n)
{
    d->bitbuf |= (uint32_t)v << d->bitcnt;
    d->bitcnt += n;
    while (d->bitcnt >= 8) {
        outbyte(d, (int)(d->bitbuf & 255));
        d->bitbuf >>= 8;
        d->bitcnt -= 8;
    }
}

static void putcode(DEFL *d, unsigned code, int n)      /* Huffman codes go MSB first */
{
    unsigned r = 0;
    int i;

    for (i = 0; i < n; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    putbits(d, r, n);
}

static const uint16_t lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int lencode(unsigned len)
{
    int i;

    for (i = 28; lens[i] > len; i--)
        ;
    return i;
}

static int distcode(unsigned dist)
{
    int i;

    for (i = 29; dists[i] > dist; i--)
        ;
    return i;
}

/* Builds length-limited Huffman codes for the n symbols of t from t->freq. */
static void build(TREE *t, int n, int limit)
{
    uint32_t w[288];            /* weights of the live nodes */
    int16_t parent[2 * 288];
    int16_t node[288];          /* live nodes */
    uint16_t count[33], next[17];
    int live = 0, nodes = n, i, j, a, b, l, used = 0;
    unsigned code;

    for (i = 0; i < n; i++) {
        t->len[i] = 0;
        t->code[i] = 0;
        if (t->freq[i]) {
            node[live] = (int16_t)i;
            w[live++] = t->freq[i];
        }
    }
    if (live == 0)
        return;
    if (live == 1) {
        t->len[node[0]] = 1;
        return;
    }
    while (live > 1) {          /* join the two lightest nodes */
        a = w[0] <= w[1] ? 0 : 1;
        b = 1 - a;
        for (i = 2; i < live; i++) {
            if (w[i] < w[a]) {
                b = a;
                a = i;
            } else if (w[i] < w[b]) {
                b = i;
            }
        }
        parent[node[a]] = parent[node[b]] = (int16_t)nodes;
        w[a] += w[b];
        node[a] = (int16_t)nodes++;
        live--;
        w[b] = w[live];
        node[b] = node[live];
    }
    parent[nodes - 1] = -1;
    memset(count, 0, sizeof(count));
    for (i = 0; i < n; i++) {
        if (!t->freq[i])
            continue;
        l = 0;
        for (j = i; parent[j] >= 0; j = parent[j])
            l++;
        count[l > 32 ? 32 : l]++;
    }
    for (i = 32; i > limit; i--)        /* shorten over-long codes (as libjpeg does) */
        while (count[i] > 0) {
            j = i - 2;
            while (count[j] == 0)
                j--;
            count[i] -= 2;
            count[i - 1]++;
            count[j + 1] += 2;
            count[j]--;
        }
    /* hand out the lengths, shortest codes to the most frequent symbols */
    used = 0;
    for (i = 0; i < n; i++)
        if (t->freq[i]) {
            for (j = used; j > 0 && t->freq[node[j - 1]] < t->freq[i]; j--)
                node[j] = node[j - 1];
            node[j] = (int16_t)i;
            used++;
        }
    j = 0;
    for (l = 1; l <= limit; l++)
        for (i = 0; i < count[l]; i++)
            t->len[node[j++]] = (uint8_t)l;
    code = 0;
    for (l = 1; l <= limit; l++) {
        next[l] = (uint16_t)code;
        code = (code + count[l]) << 1;
    }
    for (i = 0; i < n; i++)
        if (t->len[i])
            t->code[i] = next[t->len[i]]++;
}

static void fixed_trees(DEFL *d)
{
    int i;

    for (i = 0; i < 288; i++) {
        d->lt->len[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        d->lt->code[i] = (uint16_t)(i < 144 ? 0x30 + i : i < 256 ? 0x190 + (i - 144)
                                    : i < 280 ? i - 256 : 0xC0 + (i - 280));
    }
    for (i = 0; i < 30; i++) {
        d->dt->len[i] = 5;
        d->dt->code[i] = (uint16_t)i;
    }
}

/* Sends the code lengths of the dynamic trees. */
static void send_trees(DEFL *d)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t all[288 + 30], sym[288 + 30], extra[288 + 30];
    int nl = 286, nd = 30, nb = 19, n, i, k, run;

    while (nl > 257 && !d->lt->len[nl - 1]) nl--;
    while (nd > 1 && !d->dt->len[nd - 1]) nd--;
    memcpy(all, d->lt->len, nl);
    memcpy(all + nl, d->dt->len, nd);
    n = 0;
    for (i = 0; i < nl + nd; i += run) {        /* runs of zeros use codes 17 and 18 */
        run = 1;
        if (all[i] == 0) {
            while (i + run < nl + nd && all[i + run] == 0 && run < 138)
                run++;
            if (run >= 11) {
                sym[n] = 18;
                extra[n++] = (uint8_t)(run - 11);
                continue;
            }
            if (run >= 3) {
                sym[n] = 17;
                extra[n++] = (uint8_t)(run - 3);
                continue;
            }
            run = 1;
        }
        sym[n] = all[i];
        extra[n++] = 0;
    }
    memset(d->bt->freq, 0, sizeof(d->bt->freq));
    for (i = 0; i < n; i++)
        d->bt->freq[sym[i]]++;
    build(d->bt, 19, 7);
    while (nb > 4 && !d->bt->len[order[nb - 1]]) nb--;
    putbits(d, nl - 257, 5);
    putbits(d, nd - 1, 5);
    putbits(d, nb - 4, 4);
    for (i = 0; i < nb; i++)
        putbits(d, d->bt->len[order[i]], 3);
    for (i = 0; i < n; i++) {
        k = sym[i];
        putcode(d, d->bt->code[k], d->bt->len[k]);
        if (k == 17)
            putbits(d, extra[i], 3);
        else if (k == 18)
            putbits(d, extra[i], 7);
    }
}

#define HASH(p) ((((unsigned)(p)[0] << 8) ^ ((unsigned)(p)[1] << 4) ^ (p)[2]) & (HSIZE - 1))

static void block(DEFL *d, int final)
{
    const uint8_t *in = d->in;
    unsigned n = d->inlen, i = 0, h, cand, len, best, bestpos, max, k, t;
    int chain, c;

    /* 1. LZ77: turn the input into literal and match tokens */
    memset(d->head, 0xFF, HSIZE * sizeof(uint16_t));
    memset(d->lt->freq, 0, sizeof(d->lt->freq));
    memset(d->dt->freq, 0, sizeof(d->dt->freq));
    d->ntok = 0;
    while (i < n) {
        best = 0;
        bestpos = 0;
        if (i + 3 <= n) {
            h = HASH(in + i);
            max = n - i > 258 ? 258 : n - i;
            for (cand = d->head[h], chain = CHAIN; cand != NIL && chain; cand = d->prev[cand], chain--) {
                if (in[cand + best] != in[i + best])
                    continue;
                for (len = 0; len < max && in[cand + len] == in[i + len]; len++)
                    ;
                if (len > best) {
                    best = len;
                    bestpos = cand;
                    if (len == max)
                        break;
                }
            }
            d->prev[i] = d->head[h];
            d->head[h] = (uint16_t)i;
        }
        if (best >= 3) {
            d->tlit[d->ntok] = (uint16_t)best;
            d->tdist[d->ntok++] = (uint16_t)(i - bestpos);
            d->lt->freq[257 + lencode(best)]++;
            d->dt->freq[distcode(i - bestpos)]++;
            for (k = 1; k < best; k++)
                if (i + k + 3 <= n) {
                    h = HASH(in + i + k);
                    d->prev[i + k] = d->head[h];
                    d->head[h] = (uint16_t)(i + k);
                }
            i += best;
        } else {
            d->tlit[d->ntok] = in[i];
            d->tdist[d->ntok++] = 0;
            d->lt->freq[in[i]]++;
            i++;
        }
    }
    d->lt->freq[256] = 1;

    /* 2. Huffman coding: own tables, except for very small blocks */
    putbits(d, final ? 1 : 0, 1);
    if (n < 400) {
        putbits(d, 1, 2);
        fixed_trees(d);
    } else {
        putbits(d, 2, 2);
        build(d->lt, 286, 15);
        build(d->dt, 30, 15);
        send_trees(d);
    }
    for (t = 0; t < d->ntok; t++) {
        if (d->tdist[t] == 0) {
            c = d->tlit[t];
            putcode(d, d->lt->code[c], d->lt->len[c]);
        } else {
            c = lencode(d->tlit[t]);
            putcode(d, d->lt->code[257 + c], d->lt->len[257 + c]);
            putbits(d, d->tlit[t] - lens[c], lext[c]);
            c = distcode(d->tdist[t]);
            putcode(d, d->dt->code[c], d->dt->len[c]);
            putbits(d, d->tdist[t] - dists[c], dext[c]);
        }
    }
    putcode(d, d->lt->code[256], d->lt->len[256]);
    d->inlen = 0;
}

static void defl_free(DEFL *d)
{
    free(d->in);
    free(d->head);
    free(d->prev);
    free(d->tlit);
    free(d->tdist);
    free(d->lt);
    free(d->dt);
    free(d->bt);
    free(d);
}

DEFL *defl_new(int (*out)(void *user, const uint8_t *data, unsigned n), void *user)
{
    DEFL *d = malloc(sizeof(DEFL));

    if (!d)
        return NULL;
    memset(d, 0, sizeof(*d));
    d->out = out;
    d->user = user;
    d->in = img_alloc(BLOCK);
    d->head = img_alloc(HSIZE * sizeof(uint16_t));
    d->prev = img_alloc(BLOCK * sizeof(uint16_t));
    d->tlit = img_alloc(BLOCK * sizeof(uint16_t));
    d->tdist = img_alloc(BLOCK * sizeof(uint16_t));
    d->lt = malloc(sizeof(TREE));
    d->dt = malloc(sizeof(TREE));
    d->bt = malloc(sizeof(TREE));
    if (!d->in || !d->head || !d->prev || !d->tlit || !d->tdist || !d->lt || !d->dt || !d->bt) {
        defl_free(d);
        return NULL;
    }
    d->a = 1;
    outbyte(d, 0x78);
    outbyte(d, 0x01);
    return d;
}

int defl_write(DEFL *d, const uint8_t *data, unsigned n)
{
    unsigned k, i;

    while (n) {
        k = BLOCK - d->inlen;
        if (k > n)
            k = n;
        memcpy(d->in + d->inlen, data, k);
        for (i = 0; i < k; i++) {               /* Adler-32 */
            d->a += data[i];
            if (d->a >= 65521UL) d->a -= 65521UL;
            d->b += d->a;
            if (d->b >= 65521UL) d->b -= 65521UL;
        }
        d->inlen += k;
        data += k;
        n -= k;
        if (d->inlen == BLOCK)
            block(d, 0);
    }
    return !d->err;
}

int defl_end(DEFL *d)
{
    int ok;

    block(d, 1);
    if (d->bitcnt)
        putbits(d, 0, 8 - d->bitcnt);
    outbyte(d, (int)(d->b >> 8));
    outbyte(d, (int)(d->b & 255));
    outbyte(d, (int)(d->a >> 8));
    outbyte(d, (int)(d->a & 255));
    if (d->opos && !d->out(d->user, d->obuf, d->opos))
        d->err = 1;
    ok = !d->err;
    defl_free(d);
    return ok;
}
