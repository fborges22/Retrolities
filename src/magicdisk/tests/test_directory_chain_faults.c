/* Directory repairs: FAT12 boundary writes, loop classification and stale ownership. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[2880 * 512], original[2880 * 512];
static dt_u32 bad_sector = 0xffffffffUL;
static int entries, missing, reversed, pairs, space, lfn, child, tree, cross, grow, shared,
    empty_shared;
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

static int cancel_classification(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)done;
    (void)total;
    return strcmp(phase, "Classify chain loop") != 0;
}

static int cancel_relocation(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)done;
    (void)total;
    return strcmp(phase, "Prepare directory relocation") != 0 &&
           strcmp(phase, "Prepare empty directory") != 0;
}

static int fail_relocation_read(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    DtVolume *volume = (DtVolume *)ctx;

    (void)done;
    (void)total;
    if (!strcmp(phase, "Prepare directory relocation"))
        bad_sector = volume->data + (grow ? 0 : 343);
    return 1;
}

static void mixed_sectors(DtPlan *plan)
{
    unsigned i, j, changed;

    memcpy(image, original, sizeof(image));
    for (i = 0; i < plan->count; i++) {
        DtPatch *patch = &plan->patch[i];

        changed = 0;
        for (j = 0; j < 512; j++) {
            if (patch->before[j] != patch->after[j] && (++changed & 1))
                image[patch->sector * 512 + j] = patch->after[j];
        }
    }
}

static int change_after_validation(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    DtPlan *plan = (DtPlan *)ctx;
    DtPatch *patch = &plan->patch[plan->count - 1];
    unsigned i;

    if (!strcmp(phase, "Check undo sectors") && done == total) {
        for (i = 0; i < 512; i++) {
            unsigned char *byte = image + patch->sector * 512 + i;

            if (patch->before[i] != patch->after[i]) {
                *byte = *byte == patch->before[i] ? patch->after[i] : patch->before[i];
                break;
            }
        }
    }
    return 1;
}

static int prepare(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    if (tree)
        return dt_tree_plan(volume, plan, progress, ctx, err);
    return entries ? dt_entry_plan(volume, plan, progress, ctx, err)
                   : dt_directory_plan(volume, plan, progress, ctx, err);
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    char err[160];
    unsigned i, copy, patches;

    assert((argc == 2 || argc == 3) && plan);
    empty_shared = argc == 3 && !strcmp(argv[2], "--empty-shared");
    shared = empty_shared || (argc == 3 && !strcmp(argv[2], "--shared"));
    entries = argc == 3 && !shared;
    missing = entries && !strcmp(argv[2], "--missing");
    reversed = entries && !strcmp(argv[2], "--order");
    pairs = entries && !strcmp(argv[2], "--pairs");
    space = entries && !strcmp(argv[2], "--space");
    grow = entries && (!strcmp(argv[2], "--grow-child") || !strcmp(argv[2], "--grow-tree"));
    cross = entries && (!strcmp(argv[2], "--cross-child") || !strcmp(argv[2], "--cross-tree"));
    tree = empty_shared || (grow && !strcmp(argv[2], "--grow-tree")) ||
           (cross && !strcmp(argv[2], "--cross-tree")) ||
           (entries && (!strcmp(argv[2], "--tree-child") || !strcmp(argv[2], "--tree-lfn")));
    child = grow || cross ||
            (entries && (!strcmp(argv[2], "--child") || !strcmp(argv[2], "--tree-child")));
    lfn = tree || child || (entries && !strcmp(argv[2], "--lfn"));
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
        unsigned offset = (1 + copy * 9) * 512 + 511;

        image[offset] = entries ? 0xf0 : 0x50;
        image[offset + 1] = entries ? 0xff : 0x15;
    }
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    memcpy(image + volume.root * 512, "LOOPDIR    ", 11);
    image[volume.root * 512 + 11] = 16;
    word(image + volume.root * 512 + 26, 341);
    {
        unsigned char *directory = image + (volume.data + 339) * 512;

        memcpy(directory, ".          ", 11);
        directory[11] = 16;
        word(directory + 26, 341);
        memcpy(directory + 32, "..         ", 11);
        directory[43] = 16;
    }
    if (entries) {
        unsigned char *directory = image + (volume.data + 339) * 512;

        image[volume.root * 512 + 28] = 1;
        directory[11] = 0xc0;
        directory[28] = directory[60] = 1;
        word(directory + 26, 1000);
        word(directory + 58, 999);
        if (space) {
            memset(directory, 0, 64);
            memcpy(directory, "ALPHA   TXT", 11);
            directory[11] = 32;
            memcpy(directory + 32, "BETA    TXT", 11);
            directory[43] = 32;
        }
        if (lfn) {
            static const unsigned positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            unsigned n, checksum = 0;
            const char *name = "Long.txt";

            memset(directory, 0xff, 32);
            memset(directory + 32, 0, 32);
            memcpy(directory + 32, "LONG    TXT", 11);
            directory[43] = 32;
            for (n = 0; n < 11; n++)
                checksum = (((checksum >> 1) | ((checksum & 1) << 7)) + directory[32 + n]) & 255;
            directory[0] = 0x41;
            directory[11] = 15;
            directory[12] = directory[26] = directory[27] = 0;
            directory[13] = (unsigned char)checksum;
            for (n = 0; n < 13; n++)
                word(directory + positions[n],
                     n < 8    ? (unsigned char)name[n]
                     : n == 8 ? 0
                              : 0xffff);
            /* Put the relocated group beyond the injected 256-byte write boundary. */
            for (n = 2; n < 14; n++) {
                char short_name[12];

                sprintf(short_name, "F%07uTXT", n);
                memcpy(directory + n * 32, short_name, 11);
                directory[n * 32 + 11] = 32;
            }
            if (cross || grow) {
                for (n = 14; n < 16; n++) {
                    memcpy(directory + n * 32, n == 14 ? "LASTA   TXT" : "LASTB   TXT", 11);
                    directory[n * 32 + 11] = 32;
                }
                for (copy = 0; copy < 2; copy++) {
                    unsigned char *fat = image + (1 + copy * 9) * 512;

                    if (!cross)
                        continue;
                    /* Fragmented FAT12 chain: 341 -> 345 -> EOC. */
                    fat[511] = (unsigned char)(345 << 4);
                    fat[512] = (unsigned char)(345 >> 4);
                    fat[517] = 0xf0;
                    fat[518] = 0xff;
                }
            } else {
                directory[14 * 32] = directory[15 * 32] = 0xe5;
            }
            if (tree)
                directory[60] = 1;
            if (child) {
                unsigned char *nested = directory + 512;

                directory[43] = 16;
                directory[60] = 1;
                word(directory + 58, 342);
                for (copy = 0; copy < 2; copy++)
                    word(image + (1 + copy * 9) * 512 + 513, 0x0fff);
                memcpy(nested, ".          ", 11);
                nested[11] = 16;
                memcpy(nested + 32, "..         ", 11);
                nested[43] = 16;
                word(nested + 58, 341);
            }
        }
        if (pairs)
            memcpy(directory, "..         ", 11);
        if (missing)
            directory[0] = directory[32] = 0xe5;
        if (reversed) {
            unsigned char temporary[32];

            memcpy(temporary, directory, 32);
            memcpy(directory, directory + 32, 32);
            memcpy(directory + 32, temporary, 32);
        }
    }
    if (shared) {
        unsigned char *root = image + volume.root * 512;
        unsigned char *owner = image + (volume.data + 343) * 512;

        memcpy(root + 32, root, 32);
        memcpy(root, "EARLIER    ", 11);
        word(root + 26, 345);
        if (empty_shared)
            word(root + 58, 345);
        memcpy(owner, ".          ", 11);
        owner[11] = 16;
        word(owner + 26, 345);
        memcpy(owner + 32, "..         ", 11);
        owner[43] = 16;
        for (copy = 0; copy < 2; copy++) {
            unsigned char *fat = image + (1 + copy * 9) * 512;

            fat[511] = (unsigned char)(345 << 4);
            fat[512] = (unsigned char)(345 >> 4);
            fat[517] = 0xf0;
            fat[518] = 0xff;
        }
    }
    memcpy(original, image, sizeof(image));
    assert(!prepare(&volume, plan, cancel, NULL, err));
    assert(!writes && !memcmp(image, original, sizeof(image)));
    bad_sector = volume.root;
    assert(!prepare(&volume, plan, NULL, NULL, err));
    assert(!writes);
    bad_sector = 0xffffffffUL;
    if (empty_shared) {
        assert(!prepare(&volume, plan, cancel_relocation, NULL, err));
        bad_sector = volume.data;
        assert(!prepare(&volume, plan, NULL, NULL, err));
        bad_sector = 0xffffffffUL;
    }
    if (cross || grow) {
        assert(!prepare(&volume, plan, cancel_relocation, NULL, err));
        assert(!prepare(&volume, plan, fail_relocation_read, &volume, err));
        bad_sector = 0xffffffffUL;
    }
    if (!entries && !empty_shared)
        assert(!prepare(&volume, plan, cancel_classification, NULL, err));
    assert(!writes && !memcmp(image, original, sizeof(image)));
    assert(prepare(&volume, plan, NULL, NULL, err));
    patches = grow ? 8 : cross ? 4 : child ? 3 : entries ? 2 : 4;
    assert(plan->count == patches && plan->cluster == (empty_shared ? 1 : child || tree ? 2 : 1));
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(!writes);
    for (i = 1; i <= patches; i++) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, sizeof(image)));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    writes = syncs = 0;
    fail_sync = patches;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    fail_sync = 0;
    word(image + volume.root * 512 + (shared ? 32 : 0) + 26, 342);
    writes = 0;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!writes && (child ? *err != 0 : strstr(err, "refuses") != NULL));
    word(image + volume.root * 512 + (shared ? 32 : 0) + 26, empty_shared ? 345 : 341);
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    if (entries) {
        mixed_sectors(plan);
        writes = 0;
        assert(!dt_plan_restore_torn_ex(&volume, argv[1], cancel, NULL, NULL, NULL, err));
        assert(!writes);
        assert(!dt_plan_restore_torn_ex(
            &volume, argv[1], change_after_validation, plan, NULL, NULL, err));
        assert(!writes && strstr(err, "changed during restore"));
        mixed_sectors(plan);
        fail_write = 1;
        writes = syncs = 0;
        assert(!dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
        fail_write = 0;
        assert(dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
        assert(!memcmp(image, original, sizeof(image)));
        mixed_sectors(plan);
        writes = syncs = 0;
        fail_sync = 1;
        assert(!dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
        fail_sync = 0;
        assert(dt_plan_restore_torn_ex(&volume, argv[1], NULL, NULL, NULL, NULL, err));
        assert(!memcmp(image, original, sizeof(image)));
    }
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    dt_plan_free(plan);
    if (pairs)
        puts("PASS: duplicate dot replacement, torn-write rollback and exact undo");
    if (reversed)
        puts("PASS: reversed dot pair, torn-write rollback and exact undo");
    if (missing)
        puts("PASS: missing-dot reconstruction, torn-write rollback and exact undo");
    puts(entries ? "PASS: directory entry scan faults, cancellation, torn writes, rollback and "
                   "stale ownership"
                 : "PASS: directory repair FAT12 straddle, scan faults, loop cancellation, "
                   "rollback and stale ownership");
    return 0;
}
