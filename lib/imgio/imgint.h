/* imgio internals: buffered streams, picture source adapter, codecs. */
#ifndef IMGINT_H
#define IMGINT_H

#include <stdlib.h>
#include <string.h>
#include "imgio.h"

#define IOBUF 4096

/* ---- buffered reader ---- */
typedef struct {
    IMGIO *io;
    unsigned pos, len;
    long base;                  /* file offset of buf[0] */
    int eof;
    uint8_t buf[IOBUF];
} RD;

int rd_fill(RD *r);
int rd_slow(RD *r);
#define rd_u8(r) ((r)->pos < (r)->len ? (r)->buf[(r)->pos++] : rd_slow(r))
unsigned rd_bytes(RD *r, uint8_t *dst, unsigned n);
void rd_skip(RD *r, long n);
int rd_seek(RD *r, long pos);
long rd_tell(RD *r);
unsigned rd_le16(RD *r);
unsigned rd_be16(RD *r);
uint32_t rd_le32(RD *r);
uint32_t rd_be32(RD *r);

/* ---- buffered writer ---- */
typedef struct {
    IMGIO *io;
    unsigned pos;
    long total;                 /* file offset of buf[0] */
    int err;
    uint8_t buf[IOBUF];
} WR;

void wr_flush(WR *w);
void wr_u8(WR *w, int c);
void wr_bytes(WR *w, const uint8_t *p, unsigned n);
void wr_le16(WR *w, unsigned v);
void wr_le32(WR *w, uint32_t v);
void wr_be16(WR *w, unsigned v);
void wr_be32(WR *w, uint32_t v);
long wr_tell(WR *w);
void wr_seek(WR *w, long pos);

/* ---- helpers ---- */
void *img_alloc(unsigned long n);       /* NULL above 64 KB */
int img_fail(IMGIO *io, const char *msg);       /* sets the error, returns 0 */
int img_begin(IMGIO *io, IMGINFO *info);
void img_gray_pal(IMGINFO *info, int n);
void img_unpack(const uint8_t *src, int bits, int n, uint8_t *dst);
void img_pack(const uint8_t *src, int bits, int n, uint8_t *dst);

/* ---- picture source used by the encoders ---- */
typedef struct {
    IMGIO *io;
    IMGINFO in;                 /* what the caller provides */
    IMGINFO eff;                /* what the encoder sees */
    int mode;                   /* 0 direct, 1 exact colour table, 2 quantised */
    uint8_t *row;
    uint32_t *keys;
    uint8_t *vals;
    IMGQUANT *q;
} SRC;

/* want: 0 keep as is, 1 indexed if 256 colours or fewer, 2 always indexed */
int src_open(SRC *s, IMGIO *io, const IMGINFO *in, int want);
int src_rgb(SRC *s, int y, uint8_t *bgr);
int src_idx(SRC *s, int y, uint8_t *idx);
void src_close(SRC *s);

/* ---- inflate / deflate ---- */
typedef struct INFL INFL;
struct INFL {
    const uint8_t *ip, *iend;
    int (*fill)(INFL *s);       /* sets ip / iend; 0 at end of input */
    int (*flush)(INFL *s, const uint8_t *data, unsigned n);    /* 0 = stop */
    void *user;
    uint8_t *win;
    unsigned wpos, wout;
    uint32_t bitbuf;
    int bitcnt, err, stop, ended, pad;
};
int infl_run(INFL *s, int zlib);        /* non-zero on success */

typedef struct DEFL DEFL;
DEFL *defl_new(int (*out)(void *user, const uint8_t *data, unsigned n), void *user);
int defl_write(DEFL *d, const uint8_t *data, unsigned n);
int defl_end(DEFL *d);                  /* finishes the stream and frees d */

/* ---- codecs ---- */
int bmp_load(RD *r);
int png_load(RD *r);
int jpeg_load(RD *r);
int tiff_load(RD *r);
int bmp_save(WR *w, IMGIO *io, const IMGINFO *info, int bpp);
int png_save(WR *w, IMGIO *io, const IMGINFO *info);
int jpeg_save(WR *w, IMGIO *io, const IMGINFO *info, int quality);
int tiff_save(WR *w, IMGIO *io, const IMGINFO *info);

#endif
