/* Cancel at every directory-read boundary after growing per-directory name indexes. */
#include "disk.h"
#include <assert.h>
#include <string.h>

typedef struct {
    unsigned seen, stop;
} Cancel;

static int progress(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    Cancel *cancel = (Cancel *)ctx;

    (void)done;
    (void)total;
    if (!strcmp(phase, "Directories"))
        return ++cancel->seen != cancel->stop;
    return 1;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtResult result;
    Cancel cancel;
    char err[160];
    unsigned count, stop;

    assert(argc == 2);
    assert(dt_image_open(&disk, argv[1], err));
    assert(dt_mount(&disk, 0, disk.sectors, &volume, err));
    memset(&cancel, 0, sizeof(cancel));
    assert(dt_scan_ex(&volume, &result, NULL, NULL, progress, &cancel));
    assert(result.complete && !result.errors && cancel.seen);
    count = cancel.seen;
    for (stop = 1; stop <= count; stop++) {
        cancel.seen = 0;
        cancel.stop = stop;
        assert(!dt_scan_ex(&volume, &result, NULL, NULL, progress, &cancel));
        assert(!result.complete && result.errors == 1 && cancel.seen == stop);
    }
    dt_image_close(&disk);
    puts("PASS: name-index cancellation at every directory-read boundary");
    return 0;
}
