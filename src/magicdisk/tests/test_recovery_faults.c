/* Inject read failures, cancellation and destination rejection during export. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static DtDisk source;
static dt_u32 failed_sector = 0xffffffffUL;
static const char *mode;
static int live_export;

static int first_file(void *ctx, const DtFile *file)
{
    DtVolume *v = (DtVolume *)ctx;

    if (!file->directory && file->clusters && failed_sector == 0xffffffffUL)
        failed_sector = v->data + (file->start - 2) * v->spc;
    return 1;
}

static int read_sector(void *ctx, dt_u32 sector, unsigned char *data)
{
    (void)ctx;
    if (sector == failed_sector)
        return 0;
    return source.read(source.ctx, sector, data);
}

static int destination(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return strcmp(mode, "destination") != 0;
}

static int progress(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)total;
    if (!strcmp(mode, "cancel"))
        return strcmp(phase, live_export ? "Export live files" : "Recover lost chains") || done < 1;
    if (!strcmp(mode, "scan-cancel"))
        return 0;
    return 1;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtRecovery result;
    char err[160];
    int ok;

    assert(argc == 4 || argc == 5);
    live_export = argc == 5;
    mode = argv[3];
    assert(dt_image_open(&source, argv[1], err));
    memset(&disk, 0, sizeof(disk));
    disk.sectors = source.sectors;
    disk.read = read_sector;
    disk.separate_destination = destination;
    assert(dt_mount(&disk, 0, disk.sectors, &volume, err));
    if (!strcmp(mode, "gap")) {
        if (live_export) {
            DtResult scan;
            assert(dt_scan_files(&volume, &scan, first_file, &volume, NULL, NULL, NULL, NULL));
        } else {
            failed_sector = volume.data + (1000 - 2) * volume.spc;
        }
    }
    if (!strcmp(mode, "fat-read"))
        failed_sector = volume.fat;
    ok = live_export ? dt_export_files(&volume, argv[2], &result, progress, NULL, err)
                     : dt_recover_lost(&volume, argv[2], &result, progress, NULL, err);
    if (!strcmp(mode, "gap")) {
        assert(ok && result.complete && result.unreadable == 1);
        assert(live_export ? result.clusters > 0 : result.clusters == 13);
    } else {
        assert(!ok && !result.complete && *err);
        if (!strcmp(mode, "cancel"))
            assert(result.clusters == 1 && result.files >= 1 && *result.directory);
        else
            assert(!result.files && !*result.directory);
    }
    dt_image_close(&source);
    puts("PASS: recovery fault handled");
    return 0;
}
