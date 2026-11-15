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
    memcpy(original, image, sizeof(image));
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    assert(dt_bad_plan(&volume, 341, 1, plan, NULL, NULL, err));
    assert(plan->count == 4); /* FAT12 nibble straddles a sector in both copies. */
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(writes == 0 && !memcmp(image, original, sizeof(image)));

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
    assert(!dt_bad_retest(&volume, &retest, NULL, NULL, cancel, NULL));
    assert(!retest.complete);
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
