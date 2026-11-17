/* Cross-link copy and lost-chain adoption under injected write, flush and read faults. */
#include "disk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *image, *original;
static dt_u32 sectors, bad_sector = 0xffffffffUL, bad_other = 0xffffffffUL;
static unsigned writes, fail_write, syncs, fail_sync;
static dt_u32 weak_sector = 0xffffffffUL;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *out)
{
    (void)ctx;
    if (sector >= sectors || sector == bad_sector || sector == bad_other)
        return 0;
    memcpy(out, image + (size_t)sector * 512, 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *in)
{
    (void)ctx;
    assert(sector < sectors);
    if (sector == weak_sector) {
        /* A weak sector acknowledges writes that never reach the media. */
        writes++;
        return 1;
    }
    if (++writes == fail_write) {
        memcpy(image + (size_t)sector * 512, in, 256);
        return 0;
    }
    memcpy(image + (size_t)sector * 512, in, 512);
    return 1;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    return ++syncs != fail_sync;
}

static int cancel(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)phase;
    (void)done;
    (void)total;
    return 0;
}

static void reset(void)
{
    memcpy(image, original, (size_t)sectors * 512);
    writes = syncs = fail_write = fail_sync = 0;
    bad_sector = bad_other = 0xffffffffUL;
}

static int build(DtVolume *v, int lost, DtPlan *plan, char *err)
{
    return lost ? dt_lost_files_plan(v, (dt_u32)0x5a215c00UL, plan, NULL, NULL, err)
                : dt_crosslink_plan(v, plan, NULL, NULL, err);
}

/* Every write position fails once: the transaction must roll back exactly. */
static void exercise(DtDisk *disk, DtVolume *v, int lost, const char *journal)
{
    DtPlan *plan = dt_plan_create();
    char err[160];
    unsigned position, total;

    assert(plan);
    reset();
    assert(build(v, lost, plan, err));
    assert(plan->count && plan->cluster);
    total = plan->count;
    for (position = 1; position <= total; position++) {
        reset();
        remove(journal);
        assert(build(v, lost, plan, err));
        fail_write = position;
        assert(!dt_plan_apply(plan, journal, NULL, NULL, err));
        assert(strstr(err, "restored and verified"));
        assert(!memcmp(image, original, (size_t)sectors * 512));
    }
    /* A failed flush of the first commit write also rolls back. */
    reset();
    remove(journal);
    assert(build(v, lost, plan, err));
    fail_sync = 1;
    assert(!dt_plan_apply(plan, journal, NULL, NULL, err));
    assert(!memcmp(image, original, (size_t)sectors * 512));
    /* Cancellation during preparation writes nothing. */
    reset();
    assert(!(lost ? dt_lost_files_plan(v, (dt_u32)0x5a215c00UL, plan, cancel, NULL, err)
                  : dt_crosslink_plan(v, plan, cancel, NULL, err)));
    assert(!writes && !memcmp(image, original, (size_t)sectors * 512));
    /* Changed metadata after the preview is refused before any write. */
    reset();
    remove(journal);
    assert(build(v, lost, plan, err));
    image[(size_t)(v->start + plan->patch[0].sector) * 512 + 511] ^= 0x55;
    assert(!dt_plan_apply(plan, journal, NULL, NULL, err));
    assert(!writes);
    /* A committed transaction can be undone exactly, even when interrupted. */
    reset();
    remove(journal);
    assert(build(v, lost, plan, err));
    assert(dt_plan_apply(plan, journal, NULL, NULL, err));
    assert(dt_plan_restore(v, journal, err));
    assert(!memcmp(image, original, (size_t)sectors * 512));
    (void)disk;
    dt_plan_free(plan);
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan;
    FILE *file;
    long length;
    char err[160];
    dt_u32 source;

    assert(argc == 7);
    file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    length = ftell(file);
    assert(length > 0 && length % 512 == 0);
    sectors = (dt_u32)(length / 512);
    image = (unsigned char *)malloc((size_t)length);
    original = (unsigned char *)malloc((size_t)length);
    assert(image && original && !fseek(file, 0, SEEK_SET));
    assert(fread(original, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    reset();
    memset(&disk, 0, sizeof(disk));
    disk.sectors = sectors;
    disk.read = read_sector;
    disk.write = write_sector;
    disk.sync = sync_disk;
    assert(dt_mount(&disk, 0, sectors, &volume, err));
    exercise(&disk, &volume, 0, argv[3]);

    /* An unreadable shared source cluster refuses copying instead of zero-filling. */
    plan = dt_plan_create();
    assert(plan);
    reset();
    source = (dt_u32)strtoul(argv[2], NULL, 10);
    bad_sector = volume.data + (source - 2) * volume.spc;
    assert(!dt_crosslink_plan(&volume, plan, NULL, NULL, err));
    assert(strstr(err, "unreadable"));
    assert(!writes);
    dt_plan_free(plan);

    /* The copied image (lost chains remaining) exercises adoption. */
    file = fopen(argv[4], "rb");
    assert(file);
    assert(fread(original, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    reset();
    assert(dt_mount(&disk, 0, sectors, &volume, err));
    exercise(&disk, &volume, 1, argv[3]);
    remove(argv[3]);

    /* Autofix with surface retirement: a free cluster is marked bad and a live
       file cluster with a persistently unreadable sector is salvaged. */
    file = fopen(argv[1], "rb");
    assert(file);
    assert(fread(original, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    reset();
    assert(dt_mount(&disk, 0, sectors, &volume, err));
    {
        DtAutofixOptions options;
        DtAutofix result;
        dt_u32 delta = (dt_u32)strtoul(argv[5], NULL, 10), spare = volume.clusters, value;
        char path[300];
        unsigned n;

        bad_sector = volume.data + (spare - 2) * volume.spc;
        bad_other = volume.data + (delta - 2) * volume.spc;
        memset(&options, 0, sizeof(options));
        options.surface = 1;
        options.stamp = (dt_u32)0x5a215c00UL;
        options.journal_dir = argv[6];
        /* Preview writes nothing and reports both damaged clusters. */
        assert(dt_autofix(&volume, &options, &result, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                          NULL, err));
        assert(!writes && result.surface_failed == 2 && result.planned);
        /* Without --salvage the unreadable live cluster stays unresolved. */
        options.apply = 1;
        assert(dt_autofix(&volume, &options, &result, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                          NULL, err));
        assert(result.unresolved_bad == 1 && result.final.complete && !result.final.errors);
        assert(dt_fat(&volume, spare, &value) && value == 0xff7);
        n = result.journal_number;
        options.salvage = 1;
        assert(dt_autofix(&volume, &options, &result, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                          NULL, err));
        assert(result.applied == 1 && !result.unresolved_bad && !result.final.errors);
        assert(result.final.bad_clusters == 2);
        assert(dt_fat(&volume, delta, &value) && value == 0xff7);
        n = result.journal_number;
        while (n) {
            FILE *probe;

            sprintf(path, "%s/MDFX%04u.MDJ", argv[6], n--);
            probe = fopen(path, "rb");
            if (!probe)
                continue;
            fclose(probe);
            assert(dt_plan_restore(&volume, path, err));
            remove(path);
        }
        assert(!memcmp(image, original, (size_t)sectors * 512));

        /* Write test: a weak free sector is retired; all free data is restored. */
        reset();
        assert(dt_mount(&disk, 0, sectors, &volume, err));
        weak_sector = volume.data + (volume.clusters - 2) * volume.spc;
        memset(&options, 0, sizeof(options));
        options.apply = options.write_test = 1;
        options.stamp = (dt_u32)0x5a215c00UL;
        options.journal_dir = argv[6];
        assert(dt_autofix(&volume, &options, &result, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                          NULL, err));
        assert(result.write_complete && result.write_failed == 1 && !result.write_unrestored);
        assert(result.write_tested > 100);
        assert(dt_fat(&volume, volume.clusters, &value) && value == 0xff7);
        n = result.journal_number;
        while (n) {
            FILE *probe;

            sprintf(path, "%s/MDFX%04u.MDJ", argv[6], n--);
            probe = fopen(path, "rb");
            if (!probe)
                continue;
            fclose(probe);
            assert(dt_plan_restore(&volume, path, err));
            remove(path);
        }
        assert(!memcmp(image, original, (size_t)sectors * 512));
        weak_sector = 0xffffffffUL;
    }
    free(image);
    free(original);
    puts("Cross-link and adoption fault tests passed");
    return 0;
}
