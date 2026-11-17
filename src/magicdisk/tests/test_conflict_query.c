/* Read-only conflict classification against independent directory fixtures. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>

static int cancel(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)phase;
    (void)done;
    (void)total;
    return 0;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    unsigned char data[512], lfn[640];
    char err[160], text[768], tiny[1];
    FILE *file;
    size_t size;
    dt_u32 directory, offset;
    int expected;

    assert(argc == 6);
    directory = (dt_u32)strtoul(argv[2], NULL, 10);
    offset = (dt_u32)strtoul(argv[3], NULL, 10);
    expected = atoi(argv[4]);
    file = fopen(argv[5], "rb");
    assert(file);
    size = fread(lfn, 1, sizeof(lfn), file);
    assert(!ferror(file) && !(size % 32));
    assert(!fclose(file));
    assert(dt_image_open(&disk, argv[1], err));
    assert(dt_mount(&disk, 0, disk.sectors, &volume, err));
    assert(dt_read(&volume, offset / 512, data));
    if (size) {
        assert(dt_lfn_text(lfn, (unsigned)(size / 32), data + offset % 512, text, sizeof(text)));
        assert(text[0]);
        assert(!dt_lfn_text(lfn, (unsigned)(size / 32), data + offset % 512, tiny, sizeof(tiny)) &&
               !tiny[0]);
        lfn[13] ^= 1;
        assert(!dt_lfn_text(lfn, (unsigned)(size / 32), data + offset % 512, text, sizeof(text)) &&
               !text[0]);
        lfn[13] ^= 1;
    }
    assert(dt_entry_name_conflicts(&volume,
                                   directory,
                                   data + offset % 512,
                                   lfn,
                                   (unsigned)(size / 32),
                                   offset / 512,
                                   (unsigned)(offset % 512),
                                   NULL,
                                   NULL) == expected);
    assert(dt_entry_name_conflicts(&volume,
                                   directory,
                                   data + offset % 512,
                                   lfn,
                                   (unsigned)(size / 32),
                                   offset / 512,
                                   (unsigned)(offset % 512),
                                   cancel,
                                   NULL) == -1);
    dt_image_close(&disk);
    return 0;
}
