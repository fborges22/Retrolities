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
    assert(sector < 2880 && sector != 372);
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
    char err[160];
    unsigned i, copy;

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
    for (copy = 0; copy < 2; copy++) {
        unsigned char *fat = image + (1 + copy * 9) * 512;

        fat[0] = 0xf0;
        fat[1] = fat[2] = 0xff;
        /* Cluster 341 is odd and its FAT12 value spans two sectors. */
        fat[511] = 0xf0;
        fat[512] = 0xff;
    }
    memcpy(image + 19 * 512, "PAYLOAD BIN", 11);
    image[19 * 512 + 11] = 32;
    word(image + 19 * 512 + 26, 341);
    word(image + 19 * 512 + 28, 512);
    memset(image + 372 * 512, 0x5a, 512);
    memset(image + 33 * 512, 0xa5, 512);
    memcpy(original, image, sizeof(image));
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    assert(!dt_relocate_plan(&volume, 341, plan, cancel, NULL, err));
    bad_sector = 372;
    assert(!dt_relocate_plan(&volume, 341, plan, NULL, NULL, err));
    assert(strstr(err, "unreadable"));
    bad_sector = 33;
    assert(!dt_relocate_plan(&volume, 341, plan, NULL, NULL, err));
    assert(writes == 0 && !memcmp(image, original, sizeof(image)));
    bad_sector = 0xffffffffUL;
    assert(dt_relocate_plan(&volume, 341, plan, NULL, NULL, err));
    assert(plan->count == 6);
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(writes == 0);
    image[372 * 512] ^= 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(writes == 0);
    image[372 * 512] ^= 1;
    for (i = 1; i <= plan->count; i++) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
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
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image + 33 * 512, original + 372 * 512, 512));
    assert(!memcmp(image + 372 * 512, original + 372 * 512, 512));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    dt_plan_free(plan);
    puts("PASS: relocation read failures, cancellation, stale source, torn writes and exact undo");
    return 0;
}
