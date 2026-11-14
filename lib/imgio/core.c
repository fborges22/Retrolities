/* Streams, helpers, picture source adapter and format dispatch. */
#include "imgint.h"

/* ---- reader ---------------------------------------------------------------- */

int rd_fill(RD *r)
{
    r->base += r->len;
    r->pos = 0;
    r->len = r->eof ? 0 : r->io->read(r->io, r->buf, IOBUF);
    if (r->len == 0)
        r->eof = 1;
    return r->len != 0;
}

int rd_slow(RD *r)
{
    if (!rd_fill(r))
        return 0;
    return r->buf[r->pos++];
}

unsigned rd_bytes(RD *r, uint8_t *dst, unsigned n)
{
    unsigned done = 0, k;

    while (done < n) {
        if (r->pos >= r->len && !rd_fill(r))
            break;
        k = r->len - r->pos;
        if (k > n - done)
            k = n - done;
        memcpy(dst + done, r->buf + r->pos, k);
        r->pos += k;
        done += k;
    }
    return done;
}

long rd_tell(RD *r)
{
    return r->base + r->pos;
}

int rd_seek(RD *r, long pos)
{
    if (pos >= r->base && pos <= r->base + (long)r->len) {
        r->pos = (unsigned)(pos - r->base);
        return 1;
    }
    if (!r->io->seek || !r->io->seek(r->io, pos)) {
        r->eof = 1;
        r->pos = r->len;
        return 0;
    }
    r->base = pos;
    r->pos = r->len = 0;
    r->eof = 0;
    return 1;
}

void rd_skip(RD *r, long n)
{
    if (n <= 0)
        return;
    if (n <= (long)(r->len - r->pos))
        r->pos += (unsigned)n;
    else if (!rd_seek(r, rd_tell(r) + n))
        r->eof = 1;
}

unsigned rd_le16(RD *r)
{
    unsigned a = rd_u8(r);

    return a | ((unsigned)rd_u8(r) << 8);
}

unsigned rd_be16(RD *r)
{
    unsigned a = rd_u8(r);

    return (a << 8) | (unsigned)rd_u8(r);
}

uint32_t rd_le32(RD *r)
{
    uint32_t a = rd_le16(r);

    return a | ((uint32_t)rd_le16(r) << 16);
}

uint32_t rd_be32(RD *r)
{
    uint32_t a = rd_be16(r);

    return (a << 16) | rd_be16(r);
}

/* ---- writer ---------------------------------------------------------------- */

void wr_flush(WR *w)
{
    if (w->pos && !w->err && w->io->write(w->io, w->buf, w->pos) != w->pos)
        w->err = 1;
    w->total += w->pos;
    w->pos = 0;
}

void wr_u8(WR *w, int c)
{
    if (w->pos == IOBUF)
        wr_flush(w);
    w->buf[w->pos++] = (uint8_t)c;
}

void wr_bytes(WR *w, const uint8_t *p, unsigned n)
{
    unsigned k;

    while (n) {
        if (w->pos == IOBUF)
            wr_flush(w);
        k = IOBUF - w->pos;
        if (k > n)
            k = n;
        memcpy(w->buf + w->pos, p, k);
        w->pos += k;
        p += k;
        n -= k;
    }
}

void wr_le16(WR *w, unsigned v)
{
    wr_u8(w, v & 255);
    wr_u8(w, (v >> 8) & 255);
}

void wr_le32(WR *w, uint32_t v)
{
    wr_le16(w, (unsigned)(v & 0xFFFF));
    wr_le16(w, (unsigned)(v >> 16));
}

void wr_be16(WR *w, unsigned v)
{
    wr_u8(w, (v >> 8) & 255);
    wr_u8(w, v & 255);
}

void wr_be32(WR *w, uint32_t v)
{
    wr_be16(w, (unsigned)(v >> 16));
    wr_be16(w, (unsigned)(v & 0xFFFF));
}

long wr_tell(WR *w)
{
    return w->total + w->pos;
}

void wr_seek(WR *w, long pos)
{
    wr_flush(w);
    if (!w->io->seek || !w->io->seek(w->io, pos))
        w->err = 1;
    w->total = pos;
}

/* ---- helpers --------------------------------------------------------------- */

void *img_alloc(unsigned long n)
{
    if (n == 0 || n > 65000UL)
        return NULL;
    return malloc((size_t)n);
}

int img_fail(IMGIO *io, const char *msg)
{
    if (!io->error)
        io->error = msg;
    return 0;
}

int img_begin(IMGIO *io, IMGINFO *info)
{
    if (info->width < 1 || info->height < 1)
        return img_fail(io, "The file is damaged.");
    if (info->width > IMG_MAXDIM || info->height > IMG_MAXDIM)
        return img_fail(io, "The picture is too large.");
    if (!io->begin(io, info))
        return img_fail(io, "Not enough memory for this picture.");
    return 1;
}

void img_gray_pal(IMGINFO *info, int n)
{
    int i;

    info->indexed = 1;
    info->ncolors = n;
    for (i = 0; i < n; i++)
        info->pal[i][0] = info->pal[i][1] = info->pal[i][2] = (uint8_t)((long)i * 255 / (n - 1));
}

/* bits-per-pixel samples (most significant first) -> one byte each */
void img_unpack(const uint8_t *src, int bits, int n, uint8_t *dst)
{
    int i, shift = 8 - bits, mask = (1 << bits) - 1;

    if (bits == 8) {
        memcpy(dst, src, n);
        return;
    }
    for (i = 0; i < n; i++) {
        dst[i] = (uint8_t)((*src >> shift) & mask);
        shift -= bits;
        if (shift < 0) {
            shift = 8 - bits;
            src++;
        }
    }
}

void img_pack(const uint8_t *src, int bits, int n, uint8_t *dst)
{
    int i, shift = 8 - bits, acc = 0;

    if (bits == 8) {
        memcpy(dst, src, n);
        return;
    }
    for (i = 0; i < n; i++) {
        acc |= (src[i] & ((1 << bits) - 1)) << shift;
        shift -= bits;
        if (shift < 0) {
            *dst++ = (uint8_t)acc;
            acc = 0;
            shift = 8 - bits;
        }
    }
    if (shift != 8 - bits)
        *dst = (uint8_t)acc;
}

/* ---- picture source -------------------------------------------------------- */

#define HASHSIZE 1024
#define EMPTY 0xFFFFFFFFUL

static unsigned hash_slot(const SRC *s, uint32_t key, int *found)
{
    unsigned h = (unsigned)((key ^ (key >> 7) ^ (key >> 13)) & (HASHSIZE - 1));

    while (s->keys[h] != EMPTY && s->keys[h] != key)
        h = (h + 1) & (HASHSIZE - 1);
    *found = s->keys[h] == key;
    return h;
}

int src_open(SRC *s, IMGIO *io, const IMGINFO *in, int want)
{
    int x, y, n = 0, found, over = 0;
    unsigned h;
    uint32_t key, last = EMPTY;
    const uint8_t *p;

    memset(s, 0, sizeof(*s));
    s->io = io;
    s->in = *in;
    s->eff = *in;
    s->row = img_alloc((unsigned long)in->width * 3);
    if (!s->row)
        return img_fail(io, "Not enough memory.");
    if (in->indexed || want == 0)
        return 1;

    /* an RGB picture: does it have few enough colours for a palette? */
    s->keys = img_alloc(HASHSIZE * sizeof(uint32_t));
    s->vals = img_alloc(HASHSIZE);
    if (!s->keys || !s->vals)
        return img_fail(io, "Not enough memory.");
    for (h = 0; h < HASHSIZE; h++)
        s->keys[h] = EMPTY;
    for (y = 0; y < in->height && !over; y++) {
        if (!io->get(io, y, s->row))
            return img_fail(io, "The picture could not be read.");
        for (x = 0, p = s->row; x < in->width; x++, p += 3) {
            key = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            if (key == last)
                continue;
            last = key;
            h = hash_slot(s, key, &found);
            if (found)
                continue;
            if (n == 256) {
                over = 1;
                break;
            }
            s->keys[h] = key;
            s->vals[h] = (uint8_t)n;
            s->eff.pal[n][0] = p[2];
            s->eff.pal[n][1] = p[1];
            s->eff.pal[n][2] = p[0];
            n++;
        }
    }
    if (!over) {
        s->mode = 1;
        s->eff.indexed = 1;
        s->eff.ncolors = n;
        return 1;
    }
    if (want < 2)
        return 1;

    s->q = imgq_new();
    if (!s->q)
        return img_fail(io, "Not enough memory.");
    for (y = 0; y < in->height; y++) {
        if (!io->get(io, y, s->row))
            return img_fail(io, "The picture could not be read.");
        imgq_add(s->q, s->row, in->width);
    }
    s->mode = 2;
    s->eff.indexed = 1;
    s->eff.ncolors = imgq_build(s->q, 256, s->eff.pal);
    return 1;
}

int src_rgb(SRC *s, int y, uint8_t *bgr)
{
    int x;
    const uint8_t *c;

    if (!s->in.indexed)
        return s->io->get(s->io, y, bgr);
    if (!s->io->get(s->io, y, s->row))
        return 0;
    for (x = 0; x < s->in.width; x++, bgr += 3) {
        c = s->in.pal[s->row[x]];
        bgr[0] = c[2];
        bgr[1] = c[1];
        bgr[2] = c[0];
    }
    return 1;
}

int src_idx(SRC *s, int y, uint8_t *idx)
{
    int x, found;
    uint32_t key, last = EMPTY;
    uint8_t lastv = 0;
    const uint8_t *p = s->row;

    if (s->mode == 0)
        return s->io->get(s->io, y, idx);
    if (!s->io->get(s->io, y, s->row))
        return 0;
    if (s->mode == 2) {
        imgq_map(s->q, s->row, s->in.width, idx);
        return 1;
    }
    for (x = 0; x < s->in.width; x++, p += 3) {
        key = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        if (key != last) {
            last = key;
            lastv = s->vals[hash_slot(s, key, &found)];
        }
        idx[x] = lastv;
    }
    return 1;
}

void src_close(SRC *s)
{
    free(s->row);
    free(s->keys);
    free(s->vals);
    if (s->q)
        imgq_free(s->q);
    memset(s, 0, sizeof(*s));
}

/* ---- dispatch -------------------------------------------------------------- */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int img_format_from_name(const char *name)
{
    static const struct { const char *ext; int fmt; } tab[] = {
        { "bmp", IMG_BMP }, { "dib", IMG_BMP }, { "rle", IMG_BMP },
        { "png", IMG_PNG },
        { "jpg", IMG_JPEG }, { "jpeg", IMG_JPEG }, { "jpe", IMG_JPEG }, { "jfif", IMG_JPEG },
        { "tif", IMG_TIFF }, { "tiff", IMG_TIFF }
    };
    const char *dot = NULL, *p;
    unsigned i, k;

    for (p = name; *p; p++) {
        if (*p == '.')
            dot = p + 1;
        else if (*p == '\\' || *p == '/' || *p == ':')
            dot = NULL;
    }
    if (!dot)
        return IMG_NONE;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        for (k = 0; tab[i].ext[k] && lower(dot[k]) == tab[i].ext[k]; k++)
            ;
        if (!tab[i].ext[k] && !dot[k])
            return tab[i].fmt;
    }
    return IMG_NONE;
}

int img_load(IMGIO *io)
{
    RD *r = malloc(sizeof(RD));
    const uint8_t *b;
    int ok;

    io->error = NULL;
    if (!r)
        return img_fail(io, "Not enough memory.");
    memset(r, 0, sizeof(*r));
    r->io = io;
    rd_fill(r);
    b = r->buf;
    if (r->len < 8)
        ok = img_fail(io, "The file is not a picture.");
    else if (b[0] == 'B' && b[1] == 'M')
        ok = bmp_load(r);
    else if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
        ok = png_load(r);
    else if (b[0] == 0xFF && b[1] == 0xD8)
        ok = jpeg_load(r);
    else if ((b[0] == 'I' && b[1] == 'I' && b[2] == 42 && b[3] == 0)
             || (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 42))
        ok = tiff_load(r);
    else
        ok = img_fail(io, "This file format is not supported.");
    free(r);
    if (!ok)
        img_fail(io, "The file is damaged or not supported.");
    return ok;
}

int img_save(IMGIO *io, const IMGINFO *info, int format, int param)
{
    WR *w = malloc(sizeof(WR));
    int ok;

    io->error = NULL;
    if (!w)
        return img_fail(io, "Not enough memory.");
    memset(w, 0, sizeof(*w));
    w->io = io;
    switch (format) {
    case IMG_BMP:  ok = bmp_save(w, io, info, param); break;
    case IMG_PNG:  ok = png_save(w, io, info); break;
    case IMG_JPEG: ok = jpeg_save(w, io, info, param); break;
    case IMG_TIFF: ok = tiff_save(w, io, info); break;
    default:       ok = img_fail(io, "This file format is not supported."); break;
    }
    wr_flush(w);
    if (ok && w->err)
        ok = img_fail(io, "The file could not be written (is the disk full?).");
    free(w);
    if (!ok)
        img_fail(io, "The picture could not be saved.");
    return ok;
}
