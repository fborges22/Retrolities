/* Inject media and write faults; no real disk is opened by this test. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[2880 * 512], original[2880 * 512];
static dt_u32 bad_sector = 0xffffffffUL;
static unsigned writes, fail_write, fail_from, syncs, fail_sync;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *out)
{
    (void)ctx;
    if (sector >= 2880 || sector == bad_sector)
        return 0;
    memcpy(out, image + sector * 512, 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *in)
{
    (void)ctx;
    assert(sector < 2880);
    writes++;
    if (writes == fail_write || (fail_from && writes >= fail_from)) {
        /* A failed operation can have partially written a sector. */
        memcpy(image + sector * 512, in, 256);
        return 0;
    }
    memcpy(image + sector * 512, in, 512);
    return 1;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    return ++syncs != fail_sync;
}

static int journal_ready, journal_failure;

static int sync_journal(void *ctx, const char *path)
{
    FILE *file;
    unsigned char signature[8];

    (void)ctx;
    assert(!writes && !syncs);
    file = fopen(path, "rb");
    assert(file && fread(signature, 1, 8, file) == 8);
    assert(!memcmp(signature, "MDJNL001", 8));
    assert(!fseek(file, 0, SEEK_END) && ftell(file) == 1088 + 4 * 1032);
    assert(!fclose(file));
    journal_ready++;
    return !journal_failure;
}

static void word(unsigned char *p, unsigned n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static int cancel(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)phase;
    (void)done;
    (void)total;
    return 0;
}

static unsigned incomplete_reports, sector_reports;
static dt_u32 incomplete_cluster;

static void retest_report(void *ctx, const char *code, dt_u32 where, const char *message)
{
    (void)ctx;
    (void)message;
    if (!strcmp(code, "RETEST_INCOMPLETE")) {
        incomplete_reports++;
        incomplete_cluster = where;
    }
    if (!strcmp(code, "BAD_SECTOR"))
        sector_reports++;
}

static int cancel_partial(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)total;
    return strcmp(phase, "Retest cluster") || !done;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    DtRetest retest;
    char err[160];
    unsigned i;
    dt_u32 value;

    assert(argc == 2 && plan);
    memset(&disk, 0, sizeof(disk));
    disk.sectors = 2880;
    disk.read = read_sector;
    disk.write = write_sector;
    disk.sync = sync_disk;
    word(image + 11, 512);
    image[13] = 1;
    word(image + 14, 1);
    image[16] = 2;
    word(image + 17, 224);
    word(image + 19, 2880);
    word(image + 22, 9);
    word(image + 510, 0xaa55);
    image[21] = 0xf0;
    {
        unsigned fat_copy;

        for (fat_copy = 0; fat_copy < 2; fat_copy++) {
            unsigned char *header = image + (1 + fat_copy * 9) * 512;

            header[0] = 0xf0;
            header[1] = header[2] = 0xff;
        }
    }
    memcpy(original, image, sizeof(image));
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    assert(dt_bad_plan(&volume, 341, 1, plan, NULL, NULL, err));
    assert(plan->count == 4); /* FAT12 nibble straddles a sector in both copies. */
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(writes == 0 && !memcmp(image, original, sizeof(image)));

    disk.sync_journal = sync_journal;
    journal_failure = 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(strstr(err, "Journal storage could not be flushed"));
    assert(journal_ready == 1 && !writes && !syncs);
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    journal_failure = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(journal_ready == 2 && writes == 4);
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(journal_ready == 2 && !memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    disk.sync_journal = NULL;

    for (i = 1; i <= 4; i++) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, sizeof(image)));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    writes = syncs = 0;
    fail_sync = 2;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    fail_sync = 0;
    writes = syncs = 0;
    fail_from = 2;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(strstr(err, "Rollback incomplete"));
    fail_from = 0;
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_fat(&volume, 341, &value) && value == 0xff7);
    bad_sector = volume.data + 339;
    assert(dt_bad_retest(&volume, &retest, NULL, NULL, NULL, NULL));
    assert(retest.marked == 1 && retest.failed == 1 && !retest.readable);
    assert(retest.tested_sectors == 1 && retest.failed_sectors == 1);
    assert(!dt_bad_plan(&volume, 341, 0, plan, NULL, NULL, err));
    assert(strstr(err, "unreadable"));
    bad_sector = 0xffffffffUL;
    /* A file that references a bad marker must be recovered, never freed here. */
    memcpy(image + volume.root * 512, "DAMAGED BIN", 11);
    image[volume.root * 512 + 11] = 32;
    word(image + volume.root * 512 + 26, 341);
    word(image + volume.root * 512 + 28, 512);
    assert(!dt_bad_plan(&volume, 341, 0, plan, NULL, NULL, err));
    assert(strstr(err, "Filesystem check"));
    memset(image + volume.root * 512, 0, 32);
    assert(dt_bad_retest(&volume, &retest, NULL, NULL, NULL, NULL));
    assert(retest.readable == 1 && !retest.failed);
    assert(retest.tested_sectors == 1 && !retest.failed_sectors);
    assert(!dt_bad_retest(&volume, &retest, NULL, NULL, cancel, NULL));
    assert(!retest.complete);
    {
        DtVolume partial = volume;

        /* One failed sector followed by cancellation in a two-sector cluster.
           The observed failure must survive even though the cluster is partial. */
        partial.spc = 2;
        bad_sector = partial.data + 339 * 2;
        incomplete_reports = sector_reports = 0;
        assert(!dt_bad_retest(&partial, &retest, retest_report, NULL, cancel_partial, NULL));
        assert(!retest.complete && retest.marked == 1 && retest.failed == 1);
        assert(!retest.readable && retest.tested_sectors == 1 && retest.failed_sectors == 1);
        assert(incomplete_reports == 1 && sector_reports == 1 && incomplete_cluster == 341);
        bad_sector = 0xffffffffUL;
        incomplete_reports = sector_reports = 0;
        assert(!dt_bad_retest(&partial, &retest, retest_report, NULL, cancel_partial, NULL));
        assert(!retest.complete && retest.marked == 1 && !retest.failed && !retest.readable);
        assert(retest.tested_sectors == 1 && !retest.failed_sectors);
        assert(incomplete_reports == 1 && !sector_reports && incomplete_cluster == 341);
        incomplete_reports = 0;
        assert(!dt_bad_retest(&partial, &retest, retest_report, NULL, cancel, NULL));
        assert(incomplete_reports == 1 && !retest.marked && !retest.tested_sectors);
        bad_sector = volume.fat;
        incomplete_reports = 0;
        assert(!dt_bad_retest(&volume, &retest, retest_report, NULL, NULL, NULL));
        assert(incomplete_reports == 1 && !retest.complete && !retest.tested_sectors);
        bad_sector = 0xffffffffUL;
    }
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);

    /* Refuse a plan after the target metadata has changed since its preview. */
    assert(dt_bad_plan(&volume, 341, 1, plan, NULL, NULL, err));
    image[512 + 510] = 0x55;
    writes = 0;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(writes == 0);
    dt_plan_free(plan);
    puts("PASS: bad-cluster read failures, cancellation, stale plans and torn-write rollback");
    return 0;
}
