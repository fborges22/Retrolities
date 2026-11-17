#include "disk.h"
#include <assert.h>
#include <string.h>

static unsigned reads, fail_read;
static dt_u32 changed_sector = 0xffffffffUL;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *data)
{
    (void)ctx;
    assert(sector < 10000);
    reads++;
    if (reads == fail_read)
        return 0;
    memset(data, 0, 512);
    if (sector == 63 || sector == 3100 || sector == 6200) {
        data[450] = 1;
        data[454] = 63;
        data[458] = 0x40;
        data[459] = 0x0b;
    } else if (sector == 126 || sector == 3163 || sector == 6263) {
        data[12] = 2;
        data[13] = 1;
        data[14] = 1;
        data[16] = 1;
        data[17] = 16;
        data[19] = 0x40;
        data[20] = 0x0b;
        data[21] = 0xf8;
        data[22] = 9;
    } else if (sector == 127 || sector == 3164 || sector == 6264) {
        data[0] = 0xf8;
        data[1] = data[2] = 0xff;
    }
    if (!sector || sector == 63 || sector == 3100 || sector == 6200 || sector == 126 ||
        sector == 3163 || sector == 6263) {
        data[510] = 0x55;
        data[511] = 0xaa;
    }
    if (sector == 63) {
        data[466] = 15;
        data[470] = 0xdd;
        data[471] = 0x0b;
        data[474] = 0xf4;
        data[475] = 0x1a;
    }
    if (sector == 3100) {
        data[466] = 15;
        data[470] = 0xf9;
        data[471] = 0x17;
        data[474] = 0xd8;
        data[475] = 0x0e;
    }
    if (sector == changed_sector)
        data[100] ^= 1;
    return 1;
}

static int forbidden_write(void *ctx, dt_u32 sector, const unsigned char *data)
{
    (void)ctx;
    (void)sector;
    (void)data;
    assert(!"Container preview must never write");
    return 0;
}

int main(void)
{
    DtDisk disk;
    DtContainerPlan plan;
    char err[160];
    unsigned count, i, j;

    memset(&disk, 0, sizeof(disk));
    disk.sectors = 10000;
    disk.read = read_sector;
    disk.write = forbidden_write;
    assert(dt_container_plan(&disk, 1, 63, 9937, 15, 16, 63, &plan, err));
    assert(plan.witness_count == 3 && plan.disk == &disk);
    count = reads;
    dt_container_clear(&plan);
    for (i = 1; i <= count; i++) {
        reads = 0;
        fail_read = i;
        assert(!dt_container_plan(&disk, 1, 63, 9937, 15, 16, 63, &plan, err));
        assert(err[0] && !plan.disk && !plan.witness && !plan.witness_count);
    }
    fail_read = 0;
    assert(dt_container_plan(&disk, 1, 63, 9937, 15, 16, 63, &plan, err));
    assert(plan.witness_count == 3 && dt_container_revalidate(&plan, err));
    for (i = 0; i < 512; i++) {
        plan.before[i] ^= 1;
        assert(!dt_container_revalidate(&plan, err));
        plan.before[i] ^= 1;
        plan.after[i] ^= 1;
        assert(!dt_container_revalidate(&plan, err));
        plan.after[i] ^= 1;
        for (j = 0; j < plan.witness_count; j++) {
            plan.witness[j].record[i] ^= 1;
            assert(!dt_container_revalidate(&plan, err));
            plan.witness[j].record[i] ^= 1;
            plan.witness[j].boot[i] ^= 1;
            assert(!dt_container_revalidate(&plan, err));
            plan.witness[j].boot[i] ^= 1;
        }
    }
    for (j = 0; j < plan.witness_count; j++) {
        plan.witness[j].record_sector++;
        assert(!dt_container_revalidate(&plan, err));
        plan.witness[j].record_sector--;
        plan.witness[j].volume_start++;
        assert(!dt_container_revalidate(&plan, err));
        plan.witness[j].volume_start--;
    }
    plan.disk_sectors--;
    assert(!dt_container_revalidate(&plan, err));
    plan.disk_sectors++;
    plan.witness_count--;
    assert(!dt_container_revalidate(&plan, err));
    plan.witness_count++;
    changed_sector = 6200;
    assert(!dt_container_revalidate(&plan, err));
    changed_sector = 6263;
    assert(!dt_container_revalidate(&plan, err));
    changed_sector = 0xffffffffUL;
    for (i = 1; i <= count; i++) {
        reads = 0;
        fail_read = i;
        assert(!dt_container_revalidate(&plan, err));
    }
    fail_read = 0;
    assert(dt_container_revalidate(&plan, err));
    dt_container_clear(&plan);
    assert(!plan.witness && !plan.disk);
    dt_container_clear(&plan);
    assert(!dt_container_revalidate(&plan, err));
    puts("PASS: all reviewed bytes, later-chain media changes, revalidation reads and witness "
         "disposal");
    printf("PASS: all %u container preview reads fail closed, no writes\n", count);
    return 0;
}
