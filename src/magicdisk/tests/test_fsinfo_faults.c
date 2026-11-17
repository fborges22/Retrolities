/* Synthetic FAT32 media: validate repair failure paths without real disks. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SECTORS 66582UL
#define FAT_SIZE 512UL
#define DATA_START 1056UL

static unsigned char *image, *original;
static unsigned writes, fail_write, syncs, fail_sync;
static dt_u32 unreadable = 0xffffffffUL;

static void put32(unsigned char *p, dt_u32 value)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

static void put16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static int read_sector(void *ctx, dt_u32 sector, unsigned char *out)
{
    (void)ctx;
    if (sector >= SECTORS || sector == unreadable)
        return 0;
    memcpy(out, image + sector * 512, 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *in)
{
    (void)ctx;
    assert(sector < SECTORS);
    writes++;
    memcpy(image + sector * 512, in, writes == fail_write ? 256 : 512);
    return writes != fail_write;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    syncs++;
    return syncs != fail_sync;
}

static int cancel(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)phase;
    (void)done;
    (void)total;
    return 0;
}

static int cancel_recount(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)done;
    (void)total;
    return strcmp(phase, "Recount selected FAT") != 0;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    char err[160];
    unsigned copy, i;
    dt_u32 cluster;

    assert(argc == 2 && plan);
    image = (unsigned char *)calloc(SECTORS, 512);
    original = (unsigned char *)malloc(SECTORS * 512);
    assert(image && original);
    memset(&disk, 0, sizeof(disk));
    disk.sectors = SECTORS;
    disk.read = read_sector;
    disk.write = write_sector;
    disk.sync = sync_disk;
    put16(image + 11, 512);
    image[13] = 1;
    put16(image + 14, 32);
    image[16] = 2;
    put32(image + 32, SECTORS);
    put32(image + 36, FAT_SIZE);
    put32(image + 44, 2);
    put16(image + 48, 1);
    put16(image + 50, 6);
    put16(image + 510, 0xaa55);
    image[21] = 0xf8;
    for (copy = 0; copy < 2; copy++) {
        put32(image + (32 + copy * FAT_SIZE) * 512, 0x0ffffff8UL);
        put32(image + (32 + copy * FAT_SIZE) * 512 + 4, 0x0fffffffUL);
    }
    memcpy(image + 6 * 512, image, 512);
    for (copy = 0; copy < 2; copy++)
        put32(image + (32 + copy * FAT_SIZE) * 512 + 8, 0x0fffffffUL);
    memcpy(original, image, SECTORS * 512);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(!dt_fsinfo_plan(&volume, plan, cancel, NULL, err));
    unreadable = 32;
    assert(!dt_fsinfo_plan(&volume, plan, NULL, NULL, err));
    unreadable = 7;
    assert(!dt_fsinfo_plan(&volume, plan, NULL, NULL, err));
    unreadable = 0xffffffffUL;
    assert(dt_fsinfo_plan(&volume, plan, NULL, NULL, err));
    assert(plan->count == 2 && plan->kind == DT_REPAIR_FSINFO);
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(writes == 0);
    image[512 + 20] = 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(strstr(err, "changed since the preview") && writes == 0);
    image[512 + 20] = 0;
    for (i = 1; i <= 2; i++) {
        writes = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, SECTORS * 512));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_dword(image + 512 + 488) == volume.clusters - 1);
    assert(dt_dword(image + 512 + 492) == 3);
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);

    /* Repair the hints, then isolate backup boot failures on a clean volume. */
    assert(dt_fsinfo_plan(&volume, plan, NULL, NULL, err));
    for (i = 0; i < plan->count; i++)
        memcpy(image + plan->patch[i].sector * 512, plan->patch[i].after, 512);
    memset(image + 6 * 512, 0x5a, 512);
    memcpy(original, image, SECTORS * 512);
    writes = 0;
    assert(!dt_boot_backup_plan(&volume, plan, cancel, NULL, err));
    unreadable = 6;
    assert(!dt_boot_backup_plan(&volume, plan, NULL, NULL, err));
    unreadable = 0xffffffffUL;
    assert(dt_boot_backup_plan(&volume, plan, NULL, NULL, err));
    assert(plan->kind == DT_REPAIR_BOOT_BACKUP && plan->count == 1);
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    image[6 * 512 + 3] ^= 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(strstr(err, "changed since the preview") && writes == 0);
    image[6 * 512 + 3] ^= 1;
    image[3] ^= 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(writes == 0);
    image[3] ^= 1;
    fail_write = 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(strstr(err, "original sectors restored"));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    fail_write = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image, image + 6 * 512, 512));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    memcpy(image + 6 * 512, image, 512);

    /* A full FAT copy spans hundreds of sectors and grows the repair plan. */
    image[21] = 0xf8;
    memcpy(image + 6 * 512, image, 512);
    put32(image + 32 * 512, 0x0ffffff8UL);
    put32(image + 32 * 512 + 4, 0x0fffffffUL);
    memset(image + (32 + FAT_SIZE) * 512, 0x5a, FAT_SIZE * 512);
    memcpy(original, image, SECTORS * 512);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(!dt_mirror_plan(&volume, 1, plan, cancel, NULL, err));
    unreadable = 32 + FAT_SIZE + 20;
    assert(!dt_mirror_plan(&volume, 1, plan, NULL, NULL, err));
    unreadable = 0xffffffffUL;
    assert(dt_mirror_plan(&volume, 1, plan, NULL, NULL, err));
    assert(plan->count == FAT_SIZE && plan->capacity >= FAT_SIZE);
    writes = 0;
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    image[(32 + FAT_SIZE) * 512 + 9] ^= 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(strstr(err, "changed since the preview") && writes == 0);
    image[(32 + FAT_SIZE) * 512 + 9] ^= 1;
    for (i = 17; i <= FAT_SIZE; i += FAT_SIZE - 17) {
        writes = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, SECTORS * 512));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image + 32 * 512, image + (32 + FAT_SIZE) * 512, FAT_SIZE * 512));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    memcpy(image + (32 + FAT_SIZE) * 512, image + 32 * 512, FAT_SIZE * 512);

    /* Primary recovery must roll back even a torn write to sector zero. */
    memset(image, 0, 512);
    memcpy(original, image, SECTORS * 512);
    writes = 0;
    assert(!dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(dt_mount_backup(&disk, 0, SECTORS, 6, &volume, err));
    assert(!dt_boot_primary_plan(&volume, plan, cancel, NULL, err));
    unreadable = DATA_START;
    assert(!dt_boot_primary_plan(&volume, plan, NULL, NULL, err));
    unreadable = 0xffffffffUL;
    assert(dt_boot_primary_plan(&volume, plan, NULL, NULL, err));
    assert(plan->count == 1 && plan->patch[0].sector == 0);
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(writes == 0);
    image[6 * 512 + 100] ^= 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(writes == 0);
    image[6 * 512 + 100] ^= 1;
    for (i = 0; i < 2; i++) {
        writes = syncs = 0;
        fail_write = i ? 0 : 1;
        fail_sync = i ? 1 : 0;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, SECTORS * 512));
        assert(remove(argv[1]) == 0);
    }
    fail_write = fail_sync = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(dt_mount_backup(&disk, 0, SECTORS, 6, &volume, err));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(remove(argv[1]) == 0);
    memcpy(image, image + 6 * 512, 512);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));

    /* Select FAT 2 while FAT 1 and both FSInfo sectors need correction. */
    memset(image + 32 * 512, 0x5a, FAT_SIZE * 512);
    memset(image + 512, 0, 512);
    memset(image + 7 * 512, 0, 512);
    memcpy(original, image, SECTORS * 512);
    writes = 0;
    assert(!dt_tree_source_plan(&volume, 2, plan, cancel_recount, NULL, err));
    assert(!writes && !memcmp(image, original, SECTORS * 512));
    assert(dt_tree_source_plan(&volume, 2, plan, NULL, NULL, err));
    assert(plan->count == FAT_SIZE + 2 && plan->mark_bad == 2);
    for (i = 1; i <= plan->count; i += plan->count - 1) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, SECTORS * 512));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_dword(image + 512 + 488) == volume.clusters - 1);
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    /* Leave valid source allocation for the full-volume fixture below. */
    memcpy(image + 32 * 512, image + (32 + FAT_SIZE) * 512, FAT_SIZE * 512);

    /* A full volume has zero free clusters and an unknown next-free hint. */
    memcpy(image + DATA_START * 512, "FULL    BIN", 11);
    image[DATA_START * 512 + 11] = 32;
    put16(image + DATA_START * 512 + 26, 3);
    put32(image + DATA_START * 512 + 28, (volume.clusters - 1) * 512);
    for (copy = 0; copy < 2; copy++)
        for (cluster = 3; cluster < volume.clusters + 2; cluster++)
            put32(image + (32 + copy * FAT_SIZE) * 512 + cluster * 4,
                  cluster == volume.clusters + 1 ? 0x0fffffffUL : cluster + 1);
    assert(dt_fsinfo_plan(&volume, plan, NULL, NULL, err));
    assert(dt_dword(plan->patch[0].after + 488) == 0);
    assert(dt_dword(plan->patch[0].after + 492) == 0xffffffffUL);
    dt_plan_free(plan);
    free(original);
    free(image);
    puts("PASS: FSInfo/backup/primary/FAT-copy read failures, cancellation, stale plans, partial "
         "writes and "
         "full volume");
    return 0;
}
