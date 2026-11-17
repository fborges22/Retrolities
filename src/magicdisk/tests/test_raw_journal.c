/* Raw sector edit journal: apply, refusal, rollback and undo on a RAM disk. */
#include "disk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SECTORS 64

static unsigned char image[SECTORS][512];
static unsigned writes, fail_write, corrupt_write, fail_sync;
static int fail_barrier, change_in_barrier, refuse_destination;
static const char *journal_path;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *data)
{
    (void)ctx;
    if (sector >= SECTORS)
        return 0;
    memcpy(data, image[sector], 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *data)
{
    (void)ctx;
    assert(sector < SECTORS);
    writes++;
    if (writes == fail_write)
        return 0;
    memcpy(image[sector], data, 512);
    if (writes == corrupt_write)
        image[sector][7] ^= 0x40;
    return 1;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    return !(fail_sync && writes == fail_sync);
}

static int separate(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return !refuse_destination;
}

static int barrier(void *ctx, const char *path)
{
    (void)ctx;
    assert(!strcmp(path, journal_path));
    if (change_in_barrier)
        image[12][3] ^= 1;
    return !fail_barrier;
}

static void reset(DtDisk *disk)
{
    unsigned i, j;

    for (i = 0; i < SECTORS; i++)
        for (j = 0; j < 512; j++)
            image[i][j] = (unsigned char)(i * 7 + j);
    writes = fail_write = corrupt_write = fail_sync = 0;
    fail_barrier = change_in_barrier = refuse_destination = 0;
    memset(disk, 0, sizeof(*disk));
    disk->sectors = SECTORS;
    disk->read = read_sector;
    disk->write = write_sector;
    disk->sync = sync_disk;
    disk->separate_destination = separate;
    disk->sync_journal = barrier;
}

static void plan_edit(DtDisk *disk, DtRawPlan *plan, dt_u32 first, unsigned count)
{
    char err[160];
    unsigned i;

    assert(dt_raw_plan(disk, first, count, plan, err));
    assert(plan->count == count && plan->first == first);
    assert(!dt_raw_changed(plan));
    for (i = 0; i < count; i += 2)
        memset(plan->patch[i].after + 16, 0xA5, 32);
}

static int restore(DtDisk *disk, int torn, char *err)
{
    return dt_raw_restore(disk, journal_path, torn, NULL, NULL, err);
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtRawPlan plan, reviewed;
    unsigned char saved[SECTORS][512], edited[SECTORS][512];
    char err[160], other[300];
    FILE *file;
    long size;

    assert(argc == 2);
    journal_path = argv[1];
    sprintf(other, "%s.2", journal_path);
    remove(journal_path);
    remove(other);

    /* Builder bounds. */
    reset(&disk);
    assert(!dt_raw_plan(&disk, SECTORS, 1, &plan, err));
    assert(!dt_raw_plan(&disk, SECTORS - 2, 3, &plan, err));
    assert(!dt_raw_plan(&disk, 0, 0, &plan, err));

    /* Unchanged plans and missing write access write nothing. */
    reset(&disk);
    memcpy(saved, image, sizeof(image));
    assert(dt_raw_plan(&disk, 10, 4, &plan, err));
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "No bytes changed"));
    dt_raw_clear(&plan);
    plan_edit(&disk, &plan, 10, 4);
    refuse_destination = 1;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "separate"));
    refuse_destination = 0;
    disk.write = NULL;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err));
    disk.write = write_sector;
    assert(!fopen(journal_path, "rb") && !memcmp(saved, image, sizeof(image)) && !writes);

    /* Changes after the review are refused before the journal exists. */
    image[12][0] ^= 1;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "changed since"));
    image[12][0] ^= 1;
    image[0][5] ^= 1;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "identity"));
    image[0][5] ^= 1;
    assert(!fopen(journal_path, "rb") && !writes);

    /* A journal that cannot be flushed prevents every write. */
    fail_barrier = 1;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "no sectors written"));
    assert(!writes && !memcmp(saved, image, sizeof(image)));
    remove(journal_path);
    fail_barrier = 0;

    /* A concurrent change after the journal rolls back earlier writes. */
    change_in_barrier = 1;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "restored"));
    image[12][3] ^= 1;
    assert(!memcmp(saved, image, sizeof(image)));
    remove(journal_path);
    change_in_barrier = 0;

    /* Write/verify failure on the second changed sector restores the first. */
    writes = 0;
    corrupt_write = 2;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) &&
           strstr(err, "original sectors restored"));
    assert(!memcmp(saved, image, sizeof(image)));
    remove(journal_path);
    corrupt_write = 0;

    /* Existing journals are never overwritten. */
    file = fopen(journal_path, "wb");
    fputs("keep", file);
    fclose(file);
    writes = 0;
    assert(!dt_raw_apply(&plan, journal_path, NULL, NULL, err) && strstr(err, "Cannot create"));
    assert(!writes);
    remove(journal_path);

    /* Success writes only changed sectors and leaves an exact-size journal. */
    writes = 0;
    assert(dt_raw_apply(&plan, journal_path, NULL, NULL, err));
    assert(writes == 2);
    assert(!memcmp(image[10], plan.patch[0].after, 512));
    assert(!memcmp(image[11], saved[11], 512));
    assert(!memcmp(image[12], plan.patch[2].after, 512));
    assert(!memcmp(image[13], saved[13], 512));
    file = fopen(journal_path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    size = ftell(file);
    fclose(file);
    assert(size == 64 + 512 + 4 * 1024);
    memcpy(edited, image, sizeof(image));
    dt_raw_clear(&plan);

    /* Review loads the range; identity must match this disk. */
    assert(dt_raw_review_journal(&disk, journal_path, &reviewed, err));
    assert(reviewed.first == 10 && reviewed.count == 4 && dt_raw_changed(&reviewed) == 2);
    dt_raw_clear(&reviewed);
    disk.sectors = SECTORS - 1;
    assert(!dt_raw_review_journal(&disk, journal_path, &reviewed, err) && strstr(err, "size"));
    disk.sectors = SECTORS;

    /* Undo refuses unrelated changes, accepts torn mixtures only when asked. */
    image[13][100] ^= 0xff;
    assert(!restore(&disk, 1, err) && strstr(err, "LBA 13"));
    image[13][100] ^= 0xff;
    image[10][20] = saved[10][20];
    assert(!restore(&disk, 0, err) && strstr(err, "interrupted"));
    assert(restore(&disk, 1, err));
    assert(!memcmp(saved, image, sizeof(image)));
    /* Restoring again is a no-op once the original bytes are back. */
    writes = 0;
    assert(restore(&disk, 0, err) && !writes);

    /* Redo and plain undo; a failing undo write puts current bytes back. */
    memcpy(image, edited, sizeof(image));
    fail_write = writes + 2;
    assert(!restore(&disk, 0, err) && strstr(err, "previous sectors restored"));
    assert(!memcmp(edited, image, sizeof(image)));
    fail_write = 0;
    assert(restore(&disk, 0, err) && !memcmp(saved, image, sizeof(image)));

    /* Damaged journals are rejected. */
    file = fopen(journal_path, "r+b");
    assert(file && !fseek(file, 700, SEEK_SET));
    fputc(0x5a, file);
    fclose(file);
    assert(!dt_raw_review_journal(&disk, journal_path, &reviewed, err) && strstr(err, "checksum"));
    assert(!restore(&disk, 0, err));

    /* LBA 0 edits bind themselves; the identity check does not block them. */
    journal_path = other;
    plan_edit(&disk, &plan, 0, 1);
    assert(dt_raw_apply(&plan, other, NULL, NULL, err));
    dt_raw_clear(&plan);
    assert(memcmp(saved[0], image[0], 512));
    assert(restore(&disk, 0, err) && !memcmp(saved, image, sizeof(image)));

    remove(argv[1]);
    remove(other);
    puts("raw journal tests passed");
    return 0;
}
