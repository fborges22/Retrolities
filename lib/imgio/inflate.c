/* Deflate decompression (RFC 1950 / 1951).
 *
 * The canonical decoding follows Mark Adler's "puff"; codes of up to nine
 * bits - nearly all of them - are resolved with one table lookup.  The inner
 * loop keeps its state in local variables and the lookup tables in static
 * (near) data, which matters a great deal for 16-bit large-model code. */
#include "imgint.h"

#define WSIZE 32768U
#define MAXBITS 15
#define FASTBITS 9

typedef struct {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
} HUF;

static uint16_t lfast[1 << FASTBITS], dfast[1 << FASTBITS];     /* (symbol << 4) | length */

static const uint16_t lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Next input byte.  Past the end a few zero bytes are supplied (the decoder
 * looks ahead), then the stream is declared damaged. */
static int nextbyte(INFL *s)
{
    if (s->ip < s->iend)
        return *s->ip++;
    if (!s->ended) {
        if (s->fill(s) && s->ip < s->iend)
            return *s->ip++;
        s->ended = 1;
    }
    if (++s->pad > 8)
        s->err = 1;
    return 0;
}

static unsigned bits(INFL *s, int need)
{
    uint32_t v = s->bitbuf;

    while (s->bitcnt < need) {
        v |= (uint32_t)nextbyte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (unsigned)(v & ((1UL << need) - 1));
}

/* Decodes one symbol bit by bit (any code length). */
static int decode(INFL *s, const HUF *h)
{
    uint32_t v;
    int code = 0, first = 0, index = 0, len, count;

    while (s->bitcnt < MAXBITS) {
        s->bitbuf |= (uint32_t)nextbyte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    v = s->bitbuf;
    for (len = 1; len <= MAXBITS; len++) {
        code |= (int)(v & 1);
        v >>= 1;
        count = h->count[len];
        if (code - count < first) {
            s->bitbuf = v;
            s->bitcnt -= len;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

/* Builds the decoding tables for n symbols; fast may be NULL. */
static void construct(HUF *h, uint16_t *fast, const uint8_t *length, int n)
{
    uint16_t offs[MAXBITS + 1], next[MAXBITS + 1];
    unsigned code = 0, c, r, k;
    int i, l, b;

    for (i = 0; i <= MAXBITS; i++)
        h->count[i] = 0;
    for (i = 0; i < n; i++)
        h->count[length[i]]++;
    offs[1] = 0;
    for (i = 1; i < MAXBITS; i++)
        offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (length[i])
            h->symbol[offs[length[i]]++] = (uint16_t)i;
    if (!fast)
        return;
    for (l = 1; l <= MAXBITS; l++) {
        next[l] = (uint16_t)code;
        code = (code + h->count[l]) << 1;
    }
    memset(fast, 0, sizeof(lfast));
    for (i = 0; i < n; i++) {
        l = length[i];
        if (!l)
            continue;
        c = next[l]++;
        if (l > FASTBITS)
            continue;
        for (r = 0, b = 0; b < l; b++) {        /* codes are sent high bit first */
            r = (r << 1) | (c & 1);
            c >>= 1;
        }
        for (k = r; k < (1U << FASTBITS); k += 1U << l)
            fast[k] = (uint16_t)((i << 4) | l);
    }
}

static int flushwin(INFL *s, unsigned end)
{
    if (!s->stop && end > s->wout && !s->flush(s, s->win + s->wout, end - s->wout))
        s->stop = 1;
    s->wout = end == WSIZE ? 0 : end;
    return !s->stop;
}

/* Decodes the literals and matches of one block. */
static int codes(INFL *s, const HUF *lencode, const HUF *distcode)
{
    uint32_t bb = s->bitbuf;
    int bc = s->bitcnt, n, c, ok = 0;
    const uint8_t *ip = s->ip, *iend = s->iend;
    uint8_t *win = s->win, *p;
    const uint8_t *q;
    unsigned wpos = s->wpos, e, sym, len, dist, from;

#define NEXT() (ip < iend ? *ip++ : (s->ip = ip, c = nextbyte(s), ip = s->ip, iend = s->iend, c))
#define FILL() while (bc < 16) { bb |= (uint32_t)NEXT() << bc; bc += 8; }
#define SLOW(h) { s->bitbuf = bb; s->bitcnt = bc; s->ip = ip; n = decode(s, h); \
                  bb = s->bitbuf; bc = s->bitcnt; ip = s->ip; iend = s->iend; \
                  if (n < 0) \
                      break; \
                  sym = (unsigned)n; }

    while (!s->err) {
        FILL()
        e = lfast[(unsigned)bb & ((1U << FASTBITS) - 1)];
        if (e) {
            n = e & 15;
            bb >>= n;
            bc -= n;
            sym = e >> 4;
        } else {
            SLOW(lencode)
        }
        if (sym < 256) {
            win[wpos++] = (uint8_t)sym;
            if (wpos == WSIZE) {
                wpos = 0;
                if (!flushwin(s, WSIZE)) {
                    ok = 1;
                    break;
                }
            }
            continue;
        }
        if (sym == 256) {
            ok = 1;
            break;
        }
        sym -= 257;
        if (sym >= 29)
            break;
        len = lens[sym];
        n = lext[sym];
        if (n) {
            FILL()
            len += (unsigned)bb & ((1U << n) - 1);
            bb >>= n;
            bc -= n;
        }
        FILL()
        e = dfast[(unsigned)bb & ((1U << FASTBITS) - 1)];
        if (e) {
            n = e & 15;
            bb >>= n;
            bc -= n;
            sym = e >> 4;
        } else {
            SLOW(distcode)
        }
        if (sym >= 30)
            break;
        dist = dists[sym];
        n = dext[sym];
        if (n) {
            FILL()
            dist += (unsigned)bb & ((1U << n) - 1);
            bb >>= n;
            bc -= n;
        }
        from = (wpos - dist) & (WSIZE - 1);
        if (from + len <= WSIZE && wpos + len < WSIZE) {        /* no wrap-around */
            p = win + wpos;
            q = win + from;
            wpos += len;
            if (dist >= len && (from < wpos - len || from - (wpos - len) >= len)) {
                memcpy(p, q, len);
            } else {
                while (len--)
                    *p++ = *q++;
            }
            continue;
        }
        while (len--) {
            win[wpos++] = win[from];
            from = (from + 1) & (WSIZE - 1);
            if (wpos == WSIZE) {
                wpos = 0;
                if (!flushwin(s, WSIZE)) {
                    ok = 1;
                    len = 0;
                    s->stop = 1;
                }
            }
        }
        if (s->stop)
            break;
    }
    s->bitbuf = bb;
    s->bitcnt = bc;
    s->ip = ip;
    s->wpos = wpos;
    return ok;
}

static int dynamic(INFL *s, HUF *lencode, HUF *distcode)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    int index, sym, len, rep;

    if (nlen > 286 || ndist > 30)
        return 0;
    for (index = 0; index < ncode; index++)
        lengths[order[index]] = (uint8_t)bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    construct(lencode, NULL, lengths, 19);
    index = 0;
    while (index < nlen + ndist) {
        sym = decode(s, lencode);
        if (sym < 0)
            return 0;
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
        } else {
            len = 0;
            if (sym == 16) {
                if (index == 0)
                    return 0;
                len = lengths[index - 1];
                rep = 3 + bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + bits(s, 3);
            } else {
                rep = 11 + bits(s, 7);
            }
            if (index + rep > nlen + ndist)
                return 0;
            while (rep--)
                lengths[index++] = (uint8_t)len;
        }
    }
    construct(lencode, lfast, lengths, nlen);
    construct(distcode, dfast, lengths + nlen, ndist);
    return codes(s, lencode, distcode);
}

int infl_run(INFL *s, int zlib)
{
    HUF *h = malloc(2 * sizeof(HUF));
    uint8_t lengths[288];
    int last, type, i, ok = 1;
    unsigned len;

    s->win = img_alloc(WSIZE);
    if (!h || !s->win) {
        free(h);
        free(s->win);
        s->win = NULL;
        return 0;
    }
    s->wpos = s->wout = 0;
    s->bitbuf = 0;
    s->bitcnt = s->err = s->stop = s->ended = s->pad = 0;
    if (zlib) {
        i = nextbyte(s);
        if ((i & 15) != 8 || (nextbyte(s) & 0x20))
            ok = 0;
    }
    do {
        if (!ok)
            break;
        last = bits(s, 1);
        type = bits(s, 2);
        if (type == 0) {
            bits(s, s->bitcnt & 7);
            len = bits(s, 16);
            bits(s, 16);
            while (len-- && !s->err && !s->stop) {
                s->win[s->wpos++] = (uint8_t)bits(s, 8);
                if (s->wpos == WSIZE) {
                    s->wpos = 0;
                    flushwin(s, WSIZE);
                }
            }
        } else if (type == 1) {
            for (i = 0; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            construct(&h[0], lfast, lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            construct(&h[1], dfast, lengths, 30);
            ok = codes(s, &h[0], &h[1]);
        } else if (type == 2) {
            ok = dynamic(s, &h[0], &h[1]);
        } else {
            ok = 0;
        }
        if (s->err)
            ok = 0;
    } while (!last && !s->stop);
    flushwin(s, s->wpos);
    free(h);
    free(s->win);
    s->win = NULL;
    return ok || s->stop;
}
