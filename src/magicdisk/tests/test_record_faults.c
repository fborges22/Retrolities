#include "disk.h"
#include <assert.h>
#include <string.h>

static unsigned reads, fail_read;
static dt_u32 blank_sector;
static dt_u32 changed_sector = 0xffffffffUL;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *data)
{
    (void)ctx;
    assert(sector < 10000);
    reads++;
    if (reads == fail_read)
        return 0;
    memset(data, 0, 512);
    if (!sector) {
        data[450] = 15;
        data[454] = 63;
        data[458] = 0xd1;
        data[459] = 0x26;
    } else if (sector == 63 || sector == 3100 || sector == 6200) {
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
    if (sector == blank_sector)
        memset(data, 0, 512);
    if (sector == changed_sector)
        data[100] ^= 1;
    return 1;
}

static int forbidden_write(void *ctx, dt_u32 sector, const unsigned char *data)
{
    (void)ctx;
    (void)sector;
    (void)data;
    assert(!"Record preview must never write");
    return 0;
}

static void check(unsigned index)
{
    static const dt_u32 records[] = {63, 3100, 6200};
    dt_u32 next = index < 2 ? records[index + 1] : 0;
    DtDisk disk;
    DtEbrRecordPlan plan;
    char err[160];
    unsigned count, i, j;

    blank_sector = records[index];
    reads = fail_read = 0;
    changed_sector = 0xffffffffUL;
    memset(&disk, 0, sizeof(disk));
    disk.sectors = 10000;
    disk.read = read_sector;
    disk.write = forbidden_write;
    assert(dt_ebr_record_plan(&disk, blank_sector, blank_sector + 63, 0, next, 16, 63, &plan, err));
    assert(plan.witness_count == 2 - index && plan.logical.entry.disk == &disk);
    count = reads;
    dt_ebr_record_clear(&plan);
    for (i = 1; i <= count; i++) {
        reads = 0;
        fail_read = i;
        assert(!dt_ebr_record_plan(
            &disk, blank_sector, blank_sector + 63, 0, next, 16, 63, &plan, err));
        assert(err[0] && !plan.logical.entry.disk && !plan.witness && !plan.witness_count);
    }
    fail_read = 0;
    assert(dt_ebr_record_plan(&disk, blank_sector, blank_sector + 63, 0, next, 16, 63, &plan, err));
    assert(plan.witness_count == 2 - index && dt_ebr_record_revalidate(&plan, err));
    for (i = 0; i < 512; i++) {
        plan.logical.entry.before[i] ^= 1;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.logical.entry.before[i] ^= 1;
        plan.after[i] ^= 1;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.after[i] ^= 1;
        for (j = 0; j < plan.witness_count; j++) {
            plan.witness[j].record[i] ^= 1;
            assert(!dt_ebr_record_revalidate(&plan, err));
            plan.witness[j].record[i] ^= 1;
            plan.witness[j].boot[i] ^= 1;
            assert(!dt_ebr_record_revalidate(&plan, err));
            plan.witness[j].boot[i] ^= 1;
        }
    }
    for (j = 0; j < plan.witness_count; j++) {
        plan.witness[j].record_sector++;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.witness[j].record_sector--;
        plan.witness[j].volume_start++;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.witness[j].volume_start--;
    }
    plan.logical.entry.disk_sectors--;
    assert(!dt_ebr_record_revalidate(&plan, err));
    plan.logical.entry.disk_sectors++;
    plan.witness_count--;
    assert(!dt_ebr_record_revalidate(&plan, err));
    plan.witness_count++;
    for (i = 0; i < 512; i++) {
        plan.logical.mbr[i] ^= 1;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.logical.mbr[i] ^= 1;
        plan.logical.entry.after[i] ^= 1;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.logical.entry.after[i] ^= 1;
        plan.logical.entry.volume_boot[i] ^= 1;
        assert(!dt_ebr_record_revalidate(&plan, err));
        plan.logical.entry.volume_boot[i] ^= 1;
    }
    changed_sector = 6200;
    assert(!dt_ebr_record_revalidate(&plan, err));
    changed_sector = 6263;
    assert(!dt_ebr_record_revalidate(&plan, err));
    changed_sector = 0xffffffffUL;
    for (i = 1; i <= count; i++) {
        reads = 0;
        fail_read = i;
        assert(!dt_ebr_record_revalidate(&plan, err));
    }
    fail_read = 0;
    assert(dt_ebr_record_revalidate(&plan, err));
    dt_ebr_record_clear(&plan);
    assert(!plan.witness && !plan.logical.entry.disk);
    dt_ebr_record_clear(&plan);
    assert(!dt_ebr_record_revalidate(&plan, err));
    puts("PASS: all reviewed bytes, later-chain media changes, revalidation reads and witness "
         "disposal");
    printf("PASS: all %u record preview reads fail closed, no writes\n", count);
}

int main(void)
{
    unsigned i;

    for (i = 0; i < 3; i++)
        check(i);
    return 0;
}
