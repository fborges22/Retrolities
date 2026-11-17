#include "disk.h"
#include <assert.h>
#include <string.h>

static unsigned reads, fail_read, change_media;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *data)
{
    (void)ctx;
    assert(sector < 3000);
    reads++;
    if (reads == fail_read)
        return 0;
    memset(data, 0, 512);
    if (sector == 63) {
        data[11] = 0;
        data[12] = 2;
        data[13] = 1;
        data[14] = 1;
        data[16] = 1;
        data[17] = 16;
        data[19] = 0x40;
        data[20] = 0x0b;
        data[21] = 0xf8;
        data[22] = 9;
        data[510] = 0x55;
        data[511] = 0xaa;
    } else if (sector == 64) {
        data[0] = 0xf8;
        data[1] = data[2] = 0xff;
    }
    if (change_media == 1 && !sector)
        data[100] ^= 1;
    if (change_media == 2 && sector == 63)
        data[100] ^= 1;
    return 1;
}

static int forbidden_write(void *ctx, dt_u32 sector, const unsigned char *data)
{
    (void)ctx;
    (void)sector;
    (void)data;
    assert(!"Preview must never write");
    return 0;
}

int main(void)
{
    DtDisk disk;
    DtMbrPlan plan;
    char err[160];
    unsigned total, i;
    DtMbrPlan altered;

    memset(&disk, 0, sizeof(disk));
    disk.sectors = 3000;
    disk.read = read_sector;
    disk.write = forbidden_write;
    assert(dt_mbr_plan(&disk, 1, 63, 0, 16, 63, &plan, err));
    total = reads;
    for (i = 1; i <= total; i++) {
        reads = 0;
        fail_read = i;
        if (dt_mbr_plan(&disk, 1, 63, 0, 16, 63, &plan, err)) {
            printf("FAIL: accepted read failure %u of %u\n", i, total);
            return 1;
        }
        assert(err[0]);
    }
    fail_read = 0;
    assert(dt_mbr_plan(&disk, 1, 63, 0, 16, 63, &plan, err));
    assert(plan.disk == &disk);
    assert(dt_mbr_revalidate(&plan, err));
    for (i = 0; i < 512; i++) {
        altered = plan;
        altered.before[i] ^= 1;
        assert(!dt_mbr_revalidate(&altered, err));
        altered = plan;
        altered.after[i] ^= 1;
        assert(!dt_mbr_revalidate(&altered, err));
        altered = plan;
        altered.volume_boot[i] ^= 1;
        assert(!dt_mbr_revalidate(&altered, err));
    }
    altered = plan;
    altered.total++;
    assert(!dt_mbr_revalidate(&altered, err));
    altered = plan;
    altered.disk_sectors++;
    assert(!dt_mbr_revalidate(&altered, err));
    altered = plan;
    altered.bits = 16;
    assert(!dt_mbr_revalidate(&altered, err));
    assert(!dt_mbr_revalidate(NULL, err));
    for (change_media = 1; change_media <= 2; change_media++)
        assert(!dt_mbr_revalidate(&plan, err));
    change_media = 0;
    for (i = 1; i <= total; i++) {
        reads = 0;
        fail_read = i;
        assert(!dt_mbr_revalidate(&plan, err));
    }
    printf("PASS: all %u preview reads fail closed; no writes\n", total);
    puts("PASS: reviewed MBR and boot bytes are independently revalidated");
    return 0;
}
