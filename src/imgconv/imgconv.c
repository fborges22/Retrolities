/* imgconv - converts pictures between BMP, PNG, JPEG and TIFF. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgio.h"

typedef struct {
    FILE *f;
    IMGINFO info;
    uint8_t **rows;
} CONV;

static unsigned io_read(IMGIO *io, void *buf, unsigned n)
{
    return (unsigned)fread(buf, 1, n, ((CONV *)io->user)->f);
}

static unsigned io_write(IMGIO *io, const void *buf, unsigned n)
{
    return (unsigned)fwrite(buf, 1, n, ((CONV *)io->user)->f);
}

static int io_seek(IMGIO *io, long pos)
{
    return fseek(((CONV *)io->user)->f, pos, SEEK_SET) == 0;
}

static int io_begin(IMGIO *io, const IMGINFO *info)
{
    CONV *c = io->user;
    unsigned n = (unsigned)info->width * (info->indexed ? 1 : 3);
    int y;

    c->info = *info;
    c->rows = calloc(info->height, sizeof(uint8_t *));
    if (!c->rows)
        return 0;
    for (y = 0; y < info->height; y++) {
        c->rows[y] = malloc(n);
        if (!c->rows[y])
            return 0;
        memset(c->rows[y], info->indexed ? 0 : 255, n);
    }
    return 1;
}

static int io_put(IMGIO *io, int y, int x0, int step, int n, const uint8_t *px)
{
    CONV *c = io->user;
    int size = c->info.indexed ? 1 : 3, i;
    uint8_t *dst = c->rows[y] + (unsigned)x0 * size;

    if (step == 1) {
        memcpy(dst, px, (unsigned)n * size);
    } else {
        for (i = 0; i < n; i++, dst += step * size, px += size)
            memcpy(dst, px, size);
    }
    return 1;
}

static int io_get(IMGIO *io, int y, uint8_t *px)
{
    CONV *c = io->user;

    memcpy(px, c->rows[y], (unsigned)c->info.width * (c->info.indexed ? 1 : 3));
    return 1;
}

int main(int argc, char *argv[])
{
    static const char *names[] = { "?", "BMP", "PNG", "JPEG", "TIFF" };
    CONV c;
    IMGIO io;
    int format, param = 0, i;

    if (argc < 3) {
        printf("imgconv - converts pictures between BMP, PNG, JPEG and TIFF\n\n"
               "usage: imgconv input output [n]\n\n"
               "The output format follows the file extension (.bmp .png .jpg .tif).\n"
               "n is the JPEG quality (1-100) or the BMP colour depth (1, 4, 8, 24).\n");
        return 1;
    }
    format = img_format_from_name(argv[2]);
    if (format == IMG_NONE) {
        printf("imgconv: unknown output format: %s\n", argv[2]);
        return 1;
    }
    if (argc > 3)
        param = atoi(argv[3]);

    memset(&c, 0, sizeof(c));
    memset(&io, 0, sizeof(io));
    io.read = io_read;
    io.write = io_write;
    io.seek = io_seek;
    io.begin = io_begin;
    io.put = io_put;
    io.get = io_get;
    io.user = &c;

    c.f = fopen(argv[1], "rb");
    if (!c.f) {
        printf("imgconv: cannot open %s\n", argv[1]);
        return 1;
    }
    i = img_load(&io);
    fclose(c.f);
    if (!i) {
        printf("imgconv: %s: %s\n", argv[1], io.error ? io.error : "error");
        return 1;
    }
    if (format == IMG_BMP && param == 0)
        param = c.info.bpp;

    c.f = fopen(argv[2], "wb");
    if (!c.f) {
        printf("imgconv: cannot create %s\n", argv[2]);
        return 1;
    }
    i = img_save(&io, &c.info, format, param);
    if (fclose(c.f) != 0)
        i = 0;
    if (!i) {
        printf("imgconv: %s: %s\n", argv[2], io.error ? io.error : "error");
        remove(argv[2]);
        return 1;
    }
    printf("%s (%s) -> %s (%s), %d x %d\n", argv[1], names[c.info.format],
           argv[2], names[format], c.info.width, c.info.height);
    for (i = 0; i < c.info.height; i++)
        free(c.rows[i]);
    free(c.rows);
    return 0;
}
