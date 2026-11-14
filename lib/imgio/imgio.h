/* imgio - small, portable image file codecs for retro targets.
 *
 * Reads BMP (1/4/8/16/24/32 bit, RLE4/RLE8, OS/2), PNG (all colour types and
 * bit depths, interlaced), JPEG (baseline and progressive Huffman, 8 bit) and
 * TIFF (uncompressed, PackBits, LZW, Deflate; strips or tiles).
 * Writes BMP (1/4/8/24 bit), PNG, baseline JPEG and LZW compressed TIFF.
 *
 * Pictures are never held in memory: the caller supplies callbacks that
 * receive decoded pixels (loading) or provide rows (saving).  Pixels are
 * either palette indices (one byte each) or B, G, R byte triplets.  No single
 * allocation exceeds 64 KB, so the code runs in 16-bit large-model programs.
 */
#ifndef IMGIO_H
#define IMGIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_MAXDIM 16000        /* largest width / height handled */

enum { IMG_NONE, IMG_BMP, IMG_PNG, IMG_JPEG, IMG_TIFF };

typedef struct {
    int width, height;
    int indexed;                /* 1: palette indices, 0: B, G, R triplets */
    int ncolors;                /* palette entries (indexed pictures) */
    uint8_t pal[256][3];        /* R, G, B */
    int format;                 /* IMG_xxx of the file (set when loading) */
    int bpp;                    /* colour depth of the file: 1, 4, 8 or 24 */
} IMGINFO;

typedef struct IMGIO IMGIO;
struct IMGIO {
    /* File access.  read / write return the number of bytes transferred,
     * seek (absolute position) returns non-zero on success. */
    unsigned (*read)(IMGIO *io, void *buf, unsigned n);
    unsigned (*write)(IMGIO *io, const void *buf, unsigned n);
    int (*seek)(IMGIO *io, long pos);

    /* Loading: begin is called once the picture's properties are known, then
     * put delivers n pixels of row y, at columns x0, x0 + step, ...  Rows
     * arrive in any order.  Both return zero to abort. */
    int (*begin)(IMGIO *io, const IMGINFO *info);
    int (*put)(IMGIO *io, int y, int x0, int step, int n, const uint8_t *px);

    /* Saving: get fills px with row y, in the format announced to img_save.
     * It may be called several times for the same row. */
    int (*get)(IMGIO *io, int y, uint8_t *px);

    void *user;
    const char *error;          /* set when a function fails */
};

/* Loads a picture, detecting its format.  Returns non-zero on success. */
int img_load(IMGIO *io);

/* Saves a picture.  param: bits per pixel for BMP (1, 4, 8, 24),
 * quality for JPEG (1-100, 0 = default); ignored otherwise.  Pictures with
 * 256 colours or fewer are stored with a palette where the format allows. */
int img_save(IMGIO *io, const IMGINFO *info, int format, int param);

/* IMG_xxx for a file name's extension, IMG_NONE if unknown. */
int img_format_from_name(const char *name);

/* ---- colour reduction (median cut) ---- */
typedef struct IMGQUANT IMGQUANT;

IMGQUANT *imgq_new(void);
void imgq_add(IMGQUANT *q, const uint8_t *bgr, int n);      /* count pixels */
int imgq_build(IMGQUANT *q, int maxcolors, uint8_t pal[][3]); /* -> ncolors */
void imgq_map(const IMGQUANT *q, const uint8_t *bgr, int n, uint8_t *idx);
void imgq_free(IMGQUANT *q);

#ifdef __cplusplus
}
#endif

#endif
