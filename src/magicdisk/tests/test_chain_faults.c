/* File repairs: torn FAT/directory writes, cancellation and stale entry checks. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[2880 * 512], original[2880 * 512];
static dt_u32 bad_sector = 0xffffffffUL;
static unsigned writes, fail_write, fail_from, syncs, fail_sync;
static int combined, selected_source, entries;

static int build_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    if (selected_source)
        return dt_tree_source_plan(v, 1, plan, progress, ctx, err);
    return combined ? dt_tree_plan(v, plan, progress, ctx, err)
                    : dt_chain_plan(v, plan, progress, ctx, err);
}

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

typedef struct {
    unsigned phases, completed;
    dt_u32 done, total;
    const char *phase;
    const char *cancel_phase;
    dt_u32 cancel_at;
} Events;

static void notify_write(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    static const char *names[] = {
        "Save journal", "Verify journal", "Commit repair", "Roll back repair", "Restore metadata"};
    Events *events = (Events *)ctx;
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(names[i], phase))
            break;
    assert(i < sizeof(names) / sizeof(names[0]) && done <= total);
    if (!events->phase || strcmp(events->phase, phase))
        assert(done == 0);
    else
        assert(total == events->total && done == events->done + 1);
    events->phase = names[i];
    events->done = done;
    events->total = total;
    events->phases |= 1U << i;
    if (done == total)
        events->completed |= 1U << i;
}

static int cancel_phase(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    Events *events = (Events *)ctx;

    (void)total;
    return strcmp(phase, events->cancel_phase) || done < events->cancel_at;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    char err[160];
    unsigned i, copy;
    Events events;

    assert((argc == 2 || argc == 3) && plan);
    combined = argc == 3;
    selected_source =
        combined && (!strcmp(argv[2], "--source") || !strcmp(argv[2], "--source-entries"));
    entries =
        combined && (!strcmp(argv[2], "--tree-entries") || !strcmp(argv[2], "--source-entries"));
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
    image[21] = 0xf0;
    {
        unsigned fat_copy;

        for (fat_copy = 0; fat_copy < 2; fat_copy++) {
            unsigned char *header = image + (1 + fat_copy * 9) * 512;

            header[0] = 0xf0;
            header[1] = header[2] = 0xff;
        }
    }
    for (copy = 0; copy < 2; copy++) {
        unsigned offset = (1 + copy * 9) * 512 + 511;

        image[offset] = 0x50;
        image[offset + 1] = 0x15;
    }
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    memcpy(image + volume.root * 512, "BROKEN  BIN", 11);
    image[volume.root * 512 + 11] = 32;
    word(image + volume.root * 512 + 26, 341);
    word(image + volume.root * 512 + 28, 1024);
    if (combined) {
        unsigned char *entry = image + volume.root * 512 + 32;
        unsigned char *data = image + volume.data * 512;

        memcpy(entry, "LOOPDIR    ", 11);
        entry[11] = 16;
        word(entry + 26, 2);
        memcpy(data, ".          ", 11);
        data[11] = 16;
        word(data + 26, 2);
        memcpy(data + 32, "..         ", 11);
        data[32 + 11] = 16;
        if (entries) {
            entry[28] = 1;
            data[11] = 0xc0;
            data[28] = data[60] = 1;
            word(data + 26, 999);
            word(data + 58, 998);
        }
        for (copy = 0; copy < 2; copy++)
            word(image + (1 + copy * 9) * 512 + 3, 2);
    }
    if (selected_source) {
        image[512] = 0xf0;
        image[513] = image[514] = 0xff;
        memset(image + 10 * 512, 0x5a, 9 * 512);
    }
    memcpy(original, image, sizeof(image));
    assert(!build_plan(&volume, plan, cancel, NULL, err));
    assert(!writes && !memcmp(image, original, sizeof(image)));
    bad_sector = volume.root;
    assert(!build_plan(&volume, plan, NULL, NULL, err));
    assert(!writes);
    bad_sector = 0xffffffffUL;
    assert(build_plan(&volume, plan, NULL, NULL, err));
    assert(plan->count == (unsigned)((selected_source ? 12 : 5) + entries) &&
           plan->cluster == (unsigned)(combined ? 2 : 1));
    assert(!dt_plan_apply(plan, argv[1], cancel, NULL, err));
    assert(!writes);
    for (i = 1; i <= plan->count; i++) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, sizeof(image)));
        assert(remove(argv[1]) == 0);
    }
    fail_write = 0;
    writes = syncs = 0;
    fail_sync = plan->count;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    fail_sync = 0;
    image[volume.root * 512 + 22] ^= 1;
    writes = 0;
    assert(!dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(!writes && strstr(err, "changed since the preview"));
    image[volume.root * 512 + 22] ^= 1;
    assert(dt_plan_apply(plan, argv[1], NULL, NULL, err));
    assert(dt_plan_restore(&volume, argv[1], err));
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    /* Write notifications never replace the cancellable validation callback. */
    memset(&events, 0, sizeof(events));
    writes = syncs = 0;
    fail_write = 2;
    assert(!dt_plan_apply_ex(plan, argv[1], NULL, NULL, notify_write, &events, err));
    assert(events.phases == 15 && events.completed == 11);
    assert(!memcmp(image, original, sizeof(image)));
    assert(remove(argv[1]) == 0);
    fail_write = 0;
    /* Completed progress means processed sectors, not success after failed rollback. */
    memset(&events, 0, sizeof(events));
    writes = syncs = 0;
    fail_from = 2;
    assert(!dt_plan_apply_ex(plan, argv[1], NULL, NULL, notify_write, &events, err));
    assert(events.phases == 15 && events.completed == 11 && strstr(err, "Rollback incomplete"));
    assert(remove(argv[1]) == 0);
    fail_from = 0;
    memcpy(image, original, sizeof(image));
    memset(&events, 0, sizeof(events));
    assert(dt_plan_apply_ex(plan, argv[1], NULL, NULL, notify_write, &events, err));
    assert(events.phases == 7 && events.completed == 7);
    for (i = 0; i < 4; i++) {
        static unsigned char applied[2880 * 512];

        memcpy(applied, image, sizeof(image));
        memset(&events, 0, sizeof(events));
        events.cancel_phase = i < 2 ? "Read journal" : "Check undo sectors";
        events.cancel_at = i & 1 ? plan->count : 0;
        writes = 0;
        assert(!dt_plan_restore_ex(
            &volume, argv[1], cancel_phase, &events, notify_write, &events, err));
        assert(!writes && !events.phases && strstr(err, "cancelled"));
        assert(!memcmp(image, applied, sizeof(image)));
    }
    memset(&events, 0, sizeof(events));
    events.cancel_phase = "File chains";
    events.cancel_at = 0;
    writes = 0;
    assert(
        !dt_plan_restore_ex(&volume, argv[1], cancel_phase, &events, notify_write, &events, err));
    assert(!writes && !events.phases && strstr(err, "Ownership validation cancelled"));
    memset(&events, 0, sizeof(events));
    assert(dt_plan_restore_ex(&volume, argv[1], NULL, NULL, notify_write, &events, err));
    assert(events.phases == 16 && events.completed == 16);
    assert(!memcmp(image, original, sizeof(image)));
    /* Already-restored sectors still advance the notification count. */
    memset(&events, 0, sizeof(events));
    writes = 0;
    assert(dt_plan_restore_ex(&volume, argv[1], NULL, NULL, notify_write, &events, err));
    assert(!writes && events.completed == 16);
    assert(remove(argv[1]) == 0);
    dt_plan_free(plan);
    if (entries)
        puts("PASS: combined entry metadata and chain repair, shared-sector patches and directory "
             "counts");
    puts(selected_source
             ? "PASS: selected-source FAT/tree repair faults, torn copies and exact rollback"
         : combined ? "PASS: combined file/directory repair faults, torn writes, cancellation and "
                      "exact rollback"
                    : "PASS: file repair FAT12 straddle, directory writes, scan faults, "
                      "cancellation, rollback "
                      "and stale metadata");
    return 0;
}
