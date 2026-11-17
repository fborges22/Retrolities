#include "disk.h"
#include <assert.h>
#include <string.h>

static unsigned char record[512];
static unsigned writes, syncs, barriers, reads, fail_read;
static int mode;

static void reset_source(void)
{
    memset(record, 0, sizeof(record));
    record[450] = 1;
    record[454] = 63;
    record[458] = 0x40;
    record[459] = 0x0b;
    record[510] = 0x55;
    record[511] = 0xaa;
}

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
    if (sector == 3100) {
        data[466] = 15;
        data[470] = 0xf9;
        data[471] = 0x17;
        data[474] = 0xd8;
        data[475] = 0x0e;
    }
    if (sector == 63)
        memcpy(data, record, 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *data)
{
    (void)ctx;
    assert(sector == 63 && barriers);
    writes++;
    memcpy(record, data, 512);
    if (writes == 1 && mode == 1) {
        record[455] ^= 1;
        return 0;
    }
    if (writes == 1 && mode == 3)
        record[455] ^= 1;
    if (mode == 4) {
        record[455] ^= 1;
        return 0;
    }
    return 1;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    syncs++;
    return !(syncs == 1 && mode == 2);
}

static int separate(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return mode != 6;
}

static int barrier(void *ctx, const char *path)
{
    FILE *file;
    (void)ctx;
    barriers++;
    if (mode == 7)
        record[100] ^= 1;
    if (mode == 8) {
        file = fopen(path, "r+b");
        assert(file);
        assert(fputc('X', file) != EOF);
        assert(!fclose(file));
    }
    return mode != 5;
}

static void journal_semantics(DtDisk *disk, const DtEbrLinkPlan *plan, const char *path)
{
    unsigned char saved[3664], forged[3664];
    static const unsigned offsets[] = {12, 20, 24, 44, 48, 164, 676, 1188, 1700, 2220, 2732};
    FILE *file;
    dt_u32 crc;
    unsigned i, j, bit;
    char err[160];

    file = fopen(path, "rb");
    assert(file && fread(saved, 1, sizeof(saved), file) == sizeof(saved));
    assert(!fclose(file));
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        memcpy(forged, saved, sizeof(saved));
        forged[offsets[i]] ^= 1;
        memset(forged + 60, 0, 4);
        crc = 0xffffffffUL;
        for (j = 0; j < sizeof(forged); j++) {
            crc ^= forged[j];
            for (bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
        }
        crc ^= 0xffffffffUL;
        for (j = 0; j < 4; j++)
            forged[60 + j] = (unsigned char)(crc >> (j * 8));
        file = fopen(path, "wb");
        assert(file && fwrite(forged, 1, sizeof(forged), file) == sizeof(forged));
        assert(!fclose(file));
        memcpy(record, plan->after, 512);
        writes = 0;
        assert(!dt_ebr_link_restore(disk, path, 1, err));
        assert(!writes && !memcmp(record, plan->after, 512));
    }
    file = fopen(path, "wb");
    assert(file && fwrite(saved, 1, sizeof(saved), file) == sizeof(saved));
    assert(!fclose(file));
    assert(dt_ebr_link_restore(disk, path, 0, err));
}

static void read_failures(DtDisk *disk, const char *path)
{
    DtEbrLinkPlan plan;
    char err[160];
    unsigned apply_reads, restore_reads, i;

    mode = 0;
    fail_read = 0;
    reset_source();
    remove(path);
    assert(dt_ebr_link_plan(disk, 63, 3100, 16, 63, &plan, err));
    reads = writes = syncs = barriers = 0;
    assert(dt_ebr_link_apply(&plan, path, err));
    apply_reads = reads;
    reads = writes = syncs = 0;
    assert(dt_ebr_link_restore(disk, path, 0, err));
    restore_reads = reads;
    for (i = 1; i <= restore_reads; i++) {
        memcpy(record, plan.after, 512);
        reads = writes = syncs = 0;
        fail_read = i;
        assert(!dt_ebr_link_restore(disk, path, 0, err));
        assert(!memcmp(record, plan.after, 512));
    }
    for (i = 1; i <= apply_reads; i++) {
        remove(path);
        memcpy(record, plan.before, 512);
        reads = writes = syncs = barriers = 0;
        fail_read = i;
        assert(!dt_ebr_link_apply(&plan, path, err));
        assert(!memcmp(record, plan.before, 512));
    }
    fail_read = 0;
    dt_ebr_link_clear(&plan);
    remove(path);
    printf("PASS: every apply (%u) and undo (%u) read failure preserves its starting sector\n",
           apply_reads,
           restore_reads);
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtEbrLinkPlan plan;
    unsigned char before[512], torn[512];
    char err[160];
    int i, failure;

    assert(argc == 2);
    memset(&disk, 0, sizeof(disk));
    disk.sectors = 10000;
    disk.read = read_sector;
    disk.write = write_sector;
    disk.sync = sync_disk;
    disk.separate_destination = separate;
    disk.sync_journal = barrier;
    for (i = 0; i <= 8; i++) {
        remove(argv[1]);
        reset_source();
        record[100] = 42;
        memcpy(before, record, 512);
        mode = i;
        writes = syncs = barriers = 0;
        assert(dt_ebr_link_plan(&disk, 63, 3100, 16, 63, &plan, err));
        if (!i) {
            assert(dt_ebr_link_apply(&plan, argv[1], err));
            assert(writes == 1 && syncs == 1 && barriers == 1);
            assert(!memcmp(record, plan.after, 512));
            assert(dt_ebr_link_restore(&disk, argv[1], 0, err));
            assert(!memcmp(record, before, 512));
            assert(dt_ebr_link_restore(&disk, argv[1], 0, err));
            assert(writes == 2);
            /* Existing journals are never replaced. */
            assert(!dt_ebr_link_apply(&plan, argv[1], err));
            assert(writes == 2);
            memcpy(record, plan.after, 512);
            record[466] = before[466];
            memcpy(torn, record, 512);
            assert(!dt_ebr_link_restore(&disk, argv[1], 0, err));
            assert(!memcmp(record, torn, 512));
            assert(dt_ebr_link_restore(&disk, argv[1], 1, err));
            assert(!memcmp(record, before, 512));
            for (failure = 1; failure <= 4; failure++) {
                memcpy(record, plan.after, 512);
                mode = failure;
                writes = syncs = 0;
                assert(!dt_ebr_link_restore(&disk, argv[1], 0, err));
                assert(writes == 2 && syncs == 2);
                if (failure != 4)
                    assert(!memcmp(record, plan.after, 512));
                else
                    assert(strstr(err, "rollback incomplete"));
            }
            mode = 0;
            journal_semantics(&disk, &plan, argv[1]);
            memcpy(record, plan.after, 512);
            record[100] ^= 1;
            memcpy(torn, record, 512);
            assert(!dt_ebr_link_restore(&disk, argv[1], 1, err));
            assert(!memcmp(record, torn, 512));
        } else {
            assert(!dt_ebr_link_apply(&plan, argv[1], err));
            assert(err[0]);
            if (i <= 3) {
                assert(writes == 2 && syncs == 2);
                assert(!memcmp(record, before, 512));
            } else if (i == 4) {
                assert(writes == 2 && strstr(err, "rollback incomplete"));
            } else {
                assert(!writes);
                if (i != 7)
                    assert(!memcmp(record, before, 512));
            }
        }
        dt_ebr_link_clear(&plan);
    }
    read_failures(&disk, argv[1]);
    remove(argv[1]);
    puts("PASS: EBR apply, durable barrier, exact/torn undo, write/flush/verify rollback, stale "
         "media, journal corruption and exclusion");
    return 0;
}
