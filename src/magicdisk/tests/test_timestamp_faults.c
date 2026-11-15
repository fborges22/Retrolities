/* Exercise timestamp transactions against independent image fixtures in memory. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *image, *original;
static dt_u32 sectors, unreadable = 0xffffffffUL;
static unsigned writes, syncs, fail_write, fail_sync;
static int lfn;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *out)
{
    (void)ctx;
    if (sector >= sectors || sector == unreadable)
        return 0;
    memcpy(out, image + sector * 512, 512);
    return 1;
}

static int write_sector(void *ctx, dt_u32 sector, const unsigned char *data)
{
    (void)ctx;
    assert(sector < sectors);
    writes++;
    memcpy(image + sector * 512, data, writes == fail_write ? 256 : 512);
    return writes != fail_write;
}

static int sync_disk(void *ctx)
{
    (void)ctx;
    return ++syncs != fail_sync;
}

static int cancel(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    (void)ctx;
    (void)done;
    (void)total;
    return strcmp(phase, lfn ? "Check long names" : "Check timestamps") != 0;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = dt_plan_create();
    FILE *file;
    long size;
    unsigned i, count;
    char err[160];
    int (*prepare)(DtVolume *, DtPlan *, DtProgress, void *, char *);

    assert((argc == 3 || argc == 4) && plan);
    lfn = argc == 4 && !strcmp(argv[3], "lfn");
    prepare = lfn ? dt_lfn_plan : dt_timestamp_plan;
    file = fopen(argv[1], "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    size = ftell(file);
    assert(size > 0 && !(size % 512) && !fseek(file, 0, SEEK_SET));
    sectors = (dt_u32)(size / 512);
    image = (unsigned char *)malloc((size_t)size);
    original = (unsigned char *)malloc((size_t)size);
    assert(image && original && fread(image, 1, (size_t)size, file) == (size_t)size);
    assert(!fclose(file));
    memcpy(original, image, (size_t)size);
    memset(&disk, 0, sizeof(disk));
    disk.sectors = sectors;
    disk.read = read_sector;
    disk.write = write_sector;
    disk.sync = sync_disk;
    assert(dt_mount(&disk, 0, sectors, &volume, err));
    assert(!prepare(&volume, plan, cancel, NULL, err));
    assert(!writes && !memcmp(image, original, (size_t)size));
    unreadable = volume.root;
    assert(!prepare(&volume, plan, NULL, NULL, err));
    unreadable = 0xffffffffUL;
    assert(prepare(&volume, plan, NULL, NULL, err));
    count = plan->count;
    assert(count >= 3 && (lfn ? plan->cluster > 20 : plan->cluster == 6));
    for (i = 1; i <= count; i++) {
        writes = syncs = 0;
        fail_write = i;
        assert(!dt_plan_apply(plan, argv[2], NULL, NULL, err));
        assert(strstr(err, "original sectors restored"));
        assert(!memcmp(image, original, (size_t)size));
        assert(!remove(argv[2]));
    }
    fail_write = 0;
    writes = syncs = 0;
    fail_sync = 1;
    assert(!dt_plan_apply(plan, argv[2], NULL, NULL, err));
    assert(!memcmp(image, original, (size_t)size));
    assert(!remove(argv[2]));
    fail_sync = 0;
    assert(dt_plan_apply(plan, argv[2], NULL, NULL, err));
    assert(dt_plan_restore(&volume, argv[2], err));
    assert(!memcmp(image, original, (size_t)size));
    assert(!remove(argv[2]));
    dt_plan_free(plan);
    free(image);
    free(original);
    puts("PASS: metadata cleanup cancellation, directory read failure, torn writes and sync "
         "rollback");
    return 0;
}
