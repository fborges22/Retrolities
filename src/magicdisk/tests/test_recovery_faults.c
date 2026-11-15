/* Inject read failures, cancellation and destination rejection during export. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static DtDisk source;
static dt_u32 failed_sector = 0xffffffffUL, directory_sector;
static const char *mode;
static int live_export, deleted_export, selected_export, orphan_export;
static dt_u32 selected_cluster;
static unsigned failed_reads;

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
    if (sector == failed_sector) {
        failed_reads++;
        if ((strcmp(mode, "transient") && strcmp(mode, "catalog-transient") &&
             strcmp(mode, "directory-transient") && strcmp(mode, "orphan-transient")) ||
            failed_reads < 3)
            return 0;
    }
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
    if (orphan_export && !strcmp(phase, "Find orphan directories")) {
        if (!strcmp(mode, "orphan-cancel"))
            return 0;
        failed_sector = directory_sector;
    }
    if (!strcmp(mode, "catalog-cancel") && !strcmp(phase, "Retry directory sector"))
        return 0;
    if (!strcmp(mode, "directory-cancel") && !strcmp(phase, "Recover deleted directories"))
        return 0;
    if (!strcmp(mode, "directory-retry-cancel") && !strcmp(phase, "Retry deleted directory sector"))
        return 0;
    if ((!strcmp(mode, "directory-read") || !strcmp(mode, "directory-transient") ||
         !strcmp(mode, "directory-retry-cancel")) &&
        !strcmp(phase, "Recover deleted directories"))
        failed_sector = directory_sector;
    if (!strcmp(mode, "selection-cancel") && !strcmp(phase, "Locate selected file"))
        return 0;
    if (!strcmp(mode, "retry-cancel"))
        return strcmp(phase, "Retry recovery sector") != 0;
    if (!strcmp(mode, "cancel"))
        return strcmp(phase,
                      deleted_export ? "Export deleted candidates"
                      : live_export  ? "Export live files"
                                     : "Recover lost chains") ||
               done < 1;
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

    assert(argc >= 4 && argc <= 7);
    orphan_export = argc >= 5 && !strcmp(argv[4], "orphan");
    deleted_export = argc >= 5 && !strcmp(argv[4], "deleted");
    live_export = argc >= 5 && !deleted_export && !orphan_export;
    selected_export = argc >= 6 && !strcmp(argv[4], "selected");
    if (selected_export)
        selected_cluster = (dt_u32)strtoul(argv[5], NULL, 10);
    mode = argv[3];
    if (argc == 6)
        directory_sector = (dt_u32)strtoul(argv[5], NULL, 10);
    if (argc == 7)
        directory_sector = (dt_u32)strtoul(argv[6], NULL, 10);
    assert(dt_image_open(&source, argv[1], err));
    memset(&disk, 0, sizeof(disk));
    disk.sectors = source.sectors;
    disk.read = read_sector;
    disk.separate_destination = destination;
    assert(dt_mount(&disk, 0, disk.sectors, &volume, err));
    if (!strcmp(mode, "gap") || !strcmp(mode, "transient") || !strcmp(mode, "retry-cancel")) {
        if (selected_export) {
            failed_sector = volume.data + (selected_cluster - 2) * volume.spc;
        } else if (live_export) {
            DtResult scan;
            assert(dt_scan_files(&volume, &scan, first_file, &volume, NULL, NULL, NULL, NULL));
        } else {
            failed_sector = volume.data + (1000 - 2) * volume.spc;
        }
    }
    if (!strcmp(mode, "fat-read"))
        failed_sector = volume.fat;
    if (!strncmp(mode, "catalog-", 8)) {
        DtResult scan;

        failed_sector = directory_sector;
        if (!strcmp(mode, "catalog-gap")) {
            assert(!dt_scan(&volume, &scan, NULL, NULL) && !scan.complete);
            failed_reads = 0;
        }
    }
    ok = selected_export  ? dt_export_cluster_file(
                                &volume, selected_cluster, argv[2], &result, progress, NULL, err)
         : orphan_export  ? dt_recover_directories(&volume, argv[2], &result, progress, NULL, err)
         : deleted_export ? dt_recover_deleted(&volume, argv[2], &result, progress, NULL, err)
         : live_export    ? dt_export_files(&volume, argv[2], &result, progress, NULL, err)
                          : dt_recover_lost(&volume, argv[2], &result, progress, NULL, err);
    if (!strcmp(mode, "catalog-gap")) {
        assert(!ok && !result.complete && result.files && *result.directory);
        assert(result.catalog_incomplete);
        assert(!result.unreadable && result.issues && failed_reads == 6);
    } else if (!strcmp(mode, "catalog-missing")) {
        assert(!ok && !result.complete && !result.files && !*result.directory);
        assert(result.catalog_incomplete);
        assert(strstr(err, "unreadable directory entries remain unknown") && failed_reads == 3);
    } else if (!strcmp(mode, "catalog-cancel")) {
        assert(!ok && !result.complete && !result.files && !*result.directory);
        assert(!result.catalog_incomplete);
        assert(failed_reads == 1);
    } else if (!strcmp(mode, "catalog-transient")) {
        assert(ok && result.complete && result.files && !result.unreadable);
        assert(!result.catalog_incomplete);
        assert(failed_reads == 4);
    } else if (!strcmp(mode, "orphan-cancel")) {
        assert(!ok && !result.complete && !result.files && *result.directory && !failed_reads);
    } else if (!strcmp(mode, "orphan-read")) {
        assert(!ok && !result.complete && result.files && result.unreadable == 1);
        assert(failed_reads == 3);
    } else if (!strcmp(mode, "orphan-transient")) {
        assert(ok && result.complete && result.files && !result.unreadable && failed_reads >= 3);
    } else if (!strcmp(mode, "directory-cancel")) {
        assert(!ok && !result.complete && !result.files && *result.directory);
    } else if (!strcmp(mode, "directory-read")) {
        assert(ok && result.complete && !result.files && result.unreadable == 1 && result.damaged);
        assert(failed_reads == 3);
    } else if (!strcmp(mode, "directory-transient")) {
        assert(ok && result.complete && result.files && !result.unreadable && failed_reads == 4);
    } else if (!strcmp(mode, "directory-retry-cancel")) {
        assert(!ok && !result.complete && !result.files && *result.directory && failed_reads == 1);
    } else if (!strcmp(mode, "transient")) {
        assert(ok && result.complete && !result.unreadable && failed_reads == 3);
    } else if (!strcmp(mode, "retry-cancel")) {
        assert(!ok && !result.complete && !result.unreadable && failed_reads == 1);
        assert(*result.directory && result.files == 1 && !result.clusters);
    } else if (!strcmp(mode, "gap")) {
        assert(failed_reads == 3);
        assert(ok && result.complete && result.unreadable == 1);
        assert(live_export || deleted_export ? result.clusters > 0 : result.clusters == 13);
    } else {
        assert(!ok && !result.complete && *err);
        if (!strcmp(mode, "cancel"))
            assert((deleted_export ? result.clusters <= 1 : result.clusters == 1) &&
                   result.files >= 1 && *result.directory);
        else
            assert(!result.files && !*result.directory);
    }
    dt_image_close(&source);
    puts("PASS: recovery fault handled");
    return 0;
}
