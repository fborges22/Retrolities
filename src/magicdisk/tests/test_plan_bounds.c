/* The shared FAT mutation primitive must reject invalid requests before I/O. */
#include "disk.h"
#include <assert.h>
#include <string.h>

static unsigned char media[16 * 512];
static unsigned reads;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *bytes)
{
    (void)ctx;
    assert(sector < 16);
    reads++;
    memcpy(bytes, media + sector * 512, 512);
    return 1;
}

static void refused(DtPlan *plan, dt_u32 cluster, dt_u32 value, unsigned copy)
{
    char err[160];
    unsigned before = reads;
    DtPatch saved;
    unsigned count = plan ? plan->count : 0;

    if (count)
        saved = plan->patch[0];
    assert(!dt_plan_set_fat(plan, cluster, value, copy, err));
    assert(*err && reads == before);
    if (plan) {
        assert(plan->count == count);
        if (count)
            assert(!memcmp(&saved, plan->patch, sizeof(saved)));
    }
}

int main(void)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    char err[160];
    unsigned format, bits;
    dt_u32 maximum;

    assert(plan);
    memset(&disk, 0, sizeof(disk));
    disk.read = read_sector;
    disk.sectors = 16;
    refused(NULL, 2, 0, 0);
    refused(plan, 2, 0, 0);
    for (format = 0; format < 3; format++) {
        bits = format == 0 ? 12 : format == 1 ? 16 : 32;
        maximum = bits == 12 ? 0xfff : bits == 16 ? 0xffff : 0x0fffffffUL;
        memset(&volume, 0, sizeof(volume));
        volume.disk = &disk;
        volume.bits = bits;
        volume.clusters = 10;
        volume.fat = volume.fatsz = 1;
        volume.copies = 2;
        volume.root = 3;
        volume.total = 16;
        volume.mirrored = 1;
        memset(media, 0xa5, sizeof(media));
        assert(dt_plan_init(plan, &volume, err));
        /* A tree plan permits directory sectors; an excessive copy number
           must still never turn a FAT update into a directory write. */
        plan->kind = DT_REPAIR_TREE;
        assert(dt_plan_set_fat(plan, 2, maximum, 0, err));
        assert(plan->count == 1);
        refused(plan, 0, 0, 0);
        refused(plan, 1, 0, 0);
        refused(plan, 12, 0, 0);
        refused(plan, 0xffffffffUL, 0, 0);
        refused(plan, 2, maximum + 1, 0);
        refused(plan, 2, 0xffffffffUL, 0);
        refused(plan, 2, 0, 2);
        refused(plan, 2, 0, 65535);
        volume.mirrored = 0;
        volume.active = 1;
        refused(plan, 2, 0, 0);
        assert(dt_plan_set_fat(plan, 11, 0, 1, err));
        assert(plan->count == 2);
        /* FAT32 upper bits and FAT12 adjacent entry nibbles stay intact. */
        if (bits == 32)
            assert(plan->patch[0].after[11] == 0xaf);
        if (bits == 12)
            assert(plan->patch[0].after[4] == 0xaf);
        volume.bits = 24;
        refused(plan, 2, 0, 1);
        volume.bits = bits;
        volume.clusters = 1000;
        refused(plan, 1000, 0, 1);
        /* Last entry that fits, then an entry whose bytes cross the FAT end. */
        volume.clusters = bits == 12 ? 339 : bits == 16 ? 254 : 126;
        assert(dt_plan_set_fat(plan, volume.clusters + 1, maximum, 1, err));
        volume.clusters++;
        refused(plan, volume.clusters + 1, 0, 1);
        assert(!memcmp(media, plan->disk_boot, 512));
    }
    dt_plan_free(plan);
    puts("PASS: FAT12/16/32 mutation bounds, inactive copies and preserved neighbor bits");
    return 0;
}
