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
static int mirror_root_on_sync, root_written;
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
    assert(sector < SECTORS && sector != DATA_START);
    writes++;
    if (!sector)
        root_written = 1;
    memcpy(image + sector * 512, in, writes == fail_write ? 256 : 512);
    return writes != fail_write;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    syncs++;
    if (mirror_root_on_sync && root_written) {
        memcpy(image + 6 * 512 + 44, image + 44, 4);
        root_written = 0;
    }
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

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    char err[160];
    unsigned copy, i, before;

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
    image[21] = 0xf8;
    put32(image + 32, SECTORS);
    put32(image + 36, FAT_SIZE);
    put32(image + 44, 2);
    put16(image + 48, 1);
    put16(image + 50, 6);
    put16(image + 510, 0xaa55);
    memcpy(image + 6 * 512, image, 512);
    for (copy = 0; copy < 2; copy++) {
        unsigned char *fat = image + (32 + copy * FAT_SIZE) * 512;
        unsigned char *info = image + (copy ? 7 : 1) * 512;

        put32(fat, 0x0ffffff8UL);
        put32(fat + 4, 0x0fffffffUL);
        put32(fat + 8, 0x0fffffffUL);
        put32(info, 0x41615252UL);
        put32(info + 484, 0x61417272UL);
        put32(info + 488, SECTORS - DATA_START - 1);
        put32(info + 492, 0xffffffffUL);
        put32(info + 508, 0xaa550000UL);
    }
    memcpy(original, image, SECTORS * 512);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(!dt_relocate_plan(&volume, 2, plan, cancel, NULL, err));
    unreadable = DATA_START + 1;
    assert(dt_relocate_plan(&volume, 2, plan, NULL, NULL, err));
    assert(writes == 0);
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_dword(image + 44) == 4 && dt_dword(image + 6 * 512 + 44) == 4);
    assert(!memcmp(image + (DATA_START + 1) * 512, original + (DATA_START + 1) * 512, 512));
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    unreadable = 0xffffffffUL;
    assert(dt_relocate_plan(&volume, 2, plan, NULL, NULL, err));
    assert(plan->kind == DT_REPAIR_RELOCATE_ROOT && plan->count == 7);
    for (i = 1; i <= plan->count; i++) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(!memcmp(image, original, SECTORS * 512));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    writes = syncs = 0;
    fail_sync = 2;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    fail_sync = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_dword(image + 44) == 3 && dt_dword(image + 6 * 512 + 44) == 3);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err) && volume.rootclus == 3);
    before = writes;
    image[3] ^= 1;
    assert(!dt_plan_restore(&volume, argv[1], err) && writes == before);
    image[3] ^= 1;
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    assert(dt_relocate_plan(&volume, 2, plan, NULL, NULL, err));
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_mount(&disk, 0, SECTORS, &volume, err));
    mirror_root_on_sync = 1;
    root_written = 0;
    assert(dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
    assert(!memcmp(image, original, SECTORS * 512));
    assert(remove(argv[1]) == 0);
    dt_plan_free(plan);
    free(image);
    free(original);
    puts("PASS: FAT32 root relocation cancellation, read errors, torn boot/FAT writes, identity "
         "guards and exact undo");
    return 0;
}
