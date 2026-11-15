/* Synthetic media: no real disk is opened by this test. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[2880 * 512], original[2880 * 512];
static unsigned writes, fail_write, fail_from, gap_reads, reports;
static dt_u32 unreadable = 712;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *out)
{
    (void)ctx;
    if (sector == 712)
        gap_reads++;
    if (sector >= 2880 || sector == unreadable)
        return 0;
    memcpy(out, image + sector * 512, 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *in)
{
    (void)ctx;
    assert(sector < 2880 && sector != 711 && sector != 712);
    writes++;
    if (writes == fail_write || (fail_from && writes >= fail_from)) {
        memcpy(image + sector * 512, in, 256);
        return 0;
    }
    memcpy(image + sector * 512, in, 512);
    return 1;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    return 1;
}

static void word(unsigned char *p, unsigned n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static int cancel_retry(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)done;
    (void)total;
    return strcmp(phase, "Retry relocation sector") != 0;
}

static int cancel_undo(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)done;
    (void)total;
    return strcmp(phase, "Check undo sectors") != 0;
}

static void report(void *ctx, const char *code, dt_u32 where, const char *message)
{
    (void)ctx;
    if (!strcmp(code, "SALVAGE_FILE")) {
        assert(where == 341 && strstr(message, "PAYLOAD.BIN"));
        return;
    }
    assert(!strcmp(code, "SALVAGE_GAP") && where == 712);
    assert(strstr(message, "ZERO") && strstr(message, "sector 1"));
    reports++;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    unsigned copy, i, before;
    unsigned char header[64];
    FILE *journal;
    char err[160];

    assert(argc == 2 && plan);
    memset(&disk, 0, sizeof(disk));
    disk.sectors = 2880;
    disk.read = read_sector;
    disk.write = write_sector;
    disk.sync = sync_disk;
    word(image + 11, 512);
    image[13] = 2;
    word(image + 14, 1);
    image[16] = 2;
    word(image + 17, 224);
    word(image + 19, 2880);
    word(image + 22, 9);
    word(image + 510, 0xaa55);
    image[21] = 0xf0;
    for (copy = 0; copy < 2; copy++) {
        unsigned char *fat = image + (1 + copy * 9) * 512;

        fat[0] = 0xf0;
        fat[1] = fat[2] = 0xff;
        fat[511] = 0xf0;
        fat[512] = 0xff;
    }
    memcpy(image + 19 * 512, "PAYLOAD BIN", 11);
    image[19 * 512 + 11] = 32;
    word(image + 19 * 512 + 26, 341);
    word(image + 19 * 512 + 28, 1024);
    memset(image + 711 * 512, 0x5a, 512);
    memset(image + 712 * 512, 0x6b, 512);
    memset(image + 33 * 512, 0xa5, 1024);
    memcpy(original, image, sizeof(image));
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    assert(!dt_relocate_plan(&volume, 341, plan, NULL, NULL, err));
    assert(!dt_salvage_plan(&volume, 341, plan, NULL, NULL, cancel_retry, NULL, err));
    assert(strstr(err, "cancelled") && writes == 0);
    gap_reads = 0;
    assert(dt_salvage_plan(&volume, 341, plan, report, NULL, NULL, NULL, err));
    assert(gap_reads == 3 && reports == 1 && plan->gaps[0] == 2);
    assert(plan->kind == DT_REPAIR_SALVAGE);
    assert(!memcmp(image, original, sizeof(image)));
    plan->gaps[0] = 1;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err) && writes == 0);
    plan->gaps[0] = 2;
    unreadable = 0xffffffffUL;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err) && writes == 0);
    unreadable = 712;
    for (i = 1; i <= plan->count; i++) {
        writes = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(!memcmp(image, original, sizeof(image)));
        assert(remove(argv[1]) == 0);
    }
    fail_write = writes = 0;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_word(image + 19 * 512 + 26) == 2);
    assert(!memcmp(image + 33 * 512, original + 711 * 512, 512));
    for (i = 0; i < 512; i++)
        assert(image[34 * 512 + i] == 0);
    assert(!memcmp(image + 711 * 512, original + 711 * 512, 1024));
    journal = fopen(argv[1], "rb");
    assert(journal && fread(header, 1, sizeof(header), journal) == sizeof(header));
    assert(fclose(journal) == 0);
    assert(dt_dword(header + 8) == 16 && header[40] == 2);
    before = writes;
    image[711 * 512] ^= 1;
    assert(!dt_plan_restore(&volume, argv[1], err) && writes == before);
    image[711 * 512] ^= 1;
    unreadable = 0xffffffffUL;
    assert(!dt_plan_restore(&volume, argv[1], err) && writes == before);
    unreadable = 712;
    /* A replacement sector can contain a mixture after interrupted I/O. */
    memcpy(image + 34 * 512, original + 34 * 512, 256);
    assert(!dt_plan_restore(&volume, argv[1], err) && writes == before);
    image[34 * 512 + 300] = 0x7e;
    assert(!dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
    assert(writes == before);
    image[34 * 512 + 300] = 0;
    assert(!dt_plan_restore_torn_ex(&volume, argv[1], cancel_undo, NULL, NULL, NULL, err));
    assert(strstr(err, "cancelled") && writes == before);
    assert(dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    /* Persistent write faults can also defeat rollback; the retained journal
       must recover every possible interruption point once writes work again. */
    for (i = 1; i <= plan->count; i++) {
        writes = 0;
        fail_from = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        fail_from = 0;
        assert(dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
        assert(!memcmp(image, original, sizeof(image)));
        assert(remove(argv[1]) == 0);
    }
    unreadable = 0xffffffffUL;
    image[19 * 512 + 11] = 16;
    word(image + 19 * 512 + 28, 0);
    memset(image + 711 * 512, 0, 1024);
    memcpy(image + 711 * 512, ".          ", 11);
    image[711 * 512 + 11] = 16;
    word(image + 711 * 512 + 26, 341);
    memcpy(image + 711 * 512 + 32, "..         ", 11);
    image[711 * 512 + 43] = 16;
    before = writes;
    assert(!dt_salvage_plan(&volume, 341, plan, NULL, NULL, NULL, NULL, err));
    assert(strstr(err, "regular file data only") && writes == before);
    dt_plan_free(plan);
    puts("PASS: salvage gaps, bounded retries, cancellation, changed sources, torn writes and "
         "exact undo");
    return 0;
}
