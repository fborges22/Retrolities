#include "disk.h"
#include <stdlib.h>
#include <string.h>

static int stop_visit;

static int fault_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    DtDisk *source = (DtDisk *)ctx;

    return sector != 17 && source->read(source->ctx, sector, data);
}

static int found(void *ctx, const DtVolume *volume)
{
    unsigned *count = ctx;

    ++*count;
    printf("CANDIDATE %lu %lu %u %u\n",
           (unsigned long)volume->start,
           (unsigned long)volume->total,
           volume->bits,
           volume->boot_source);
    return !stop_visit;
}

static int cancelled(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)phase;
    (void)done;
    (void)total;
    return 0;
}

int main(int argc, char **argv)
{
    DtDisk disk, original;
    char err[160];
    unsigned count = 0;
    int result;
    dt_u32 first, length;

    if (argc < 4 || !dt_image_open(&disk, argv[1], err))
        return 2;
    first = (dt_u32)strtoul(argv[2], NULL, 10);
    length = (dt_u32)strtoul(argv[3], NULL, 10);
    original = disk;
    if (argc > 4 && !strcmp(argv[4], "fault")) {
        disk.ctx = &original;
        disk.read = fault_read;
    }
    stop_visit = argc > 4 && !strcmp(argv[4], "stop");
    result = dt_find_volumes(&disk,
                             first,
                             length,
                             found,
                             &count,
                             argc > 4 && !strcmp(argv[4], "cancel") ? cancelled : NULL,
                             NULL,
                             err);
    dt_image_close(&original);
    printf("COMPLETE %d COUNT %u %s\n", result, count, err);
    return result ? 0 : 1;
}
