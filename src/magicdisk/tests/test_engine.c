/* Fault injection for map coverage, cancellation and surface-read accounting. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *image;
static dt_u32 fail_sector = 0xffffffffUL;
static unsigned reads, reports;
static dt_u32 disk_offset;
static const char *expected_location;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *out)
{
    (void)ctx;
    reads++;
    sector -= disk_offset;
    if (sector == fail_sector || sector >= 2880)
        return 0;
    memcpy(out, image + sector * 512, 512);
    return 1;
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

static void surface_finding(void *ctx, const char *code, dt_u32 where, const char *message)
{
    assert(ctx == &reports);
    assert(!strcmp(code, "SURFACE_READ"));
    assert(where == disk_offset + fail_sector);
    assert(strstr(message, expected_location));
    reports++;
}

int main(void)
{
    DtDisk disk;
    DtVolume volume;
    DtMap map;
    DtResult result;
    dt_u32 start, count, next, free_count = 0, tested = 0, errors = 0;
    unsigned i;
    char err[160];

    image = (unsigned char *)calloc(2880, 512);
    assert(image);
    word(image + 11, 512);
    image[13] = 1;
    word(image + 14, 1);
    image[16] = 2;
    word(image + 17, 224);
    word(image + 19, 2880);
    word(image + 22, 9);
    word(image + 510, 0xaa55);
    disk.ctx = NULL;
    disk.sectors = 2880;
    disk.read = read_sector;
    assert(dt_mount(&disk, 0, 2880, &volume, err));
    assert(dt_map_build(&volume, 2, volume.clusters, &map, NULL, NULL));
    assert(map.complete && map.cells == DT_MAP_CELLS);
    next = 2;
    for (i = 0; i < map.cells; i++) {
        dt_map_range(&map, i, &start, &count);
        assert(start == next && count > 0);
        assert(map.cell[i].done == count);
        free_count += map.cell[i].free;
        next += count;
    }
    assert(next == volume.clusters + 2 && free_count == volume.clusters);
    assert(dt_map_build(&volume, 343, 7, &map, NULL, NULL));
    assert(map.cells == 7);
    for (i = 0; i < map.cells; i++) {
        dt_map_range(&map, i, &start, &count);
        assert(start == 343 + i && count == 1);
    }
    reads = 0;
    assert(!dt_map_build(&volume, 2, volume.clusters, &map, cancel, NULL));
    assert(!map.complete && reads == 0);

    fail_sector = 100;
    assert(dt_surface(&volume, 0, volume.total, &map, NULL, NULL));
    for (i = 0; i < map.cells; i++) {
        tested += map.cell[i].done;
        errors += map.cell[i].unreadable;
    }
    assert(tested == 2880 && errors == 1);
    reads = 0;
    assert(!dt_surface(&volume, 0, volume.total, &map, cancel, NULL));
    assert(!map.complete && reads == 0);
    assert(!dt_scan_ex(&volume, &result, NULL, NULL, cancel, NULL));
    assert(!result.complete && result.errors == 1);

    /* Disk LBAs must include the partition offset; classification uses volume sectors. */
    disk_offset = volume.start = 63;
    for (i = 0; i < 4; i++) {
        static const dt_u32 sectors[] = {0, 1, 19, 100};
        static const char *locations[] = {"reserved metadata",
                                          "FAT metadata",
                                          "fixed root directory",
                                          "data cluster 69, sector 0"};

        fail_sector = sectors[i];
        expected_location = locations[i];
        reports = 0;
        assert(dt_surface_ex(&volume, fail_sector, 1, &map, NULL, NULL, surface_finding, &reports));
        assert(reports == 1 && map.complete && map.cell[0].unreadable == 1);
    }
    reports = reads = 0;
    assert(!dt_surface_ex(&volume, 0, volume.total, &map, cancel, NULL, surface_finding, &reports));
    assert(!map.complete && !reports && !reads);
    assert(!dt_surface_ex(&volume, volume.total, 1, &map, NULL, NULL, surface_finding, &reports));
    assert(!map.complete && !map.cells && !reports && !reads);
    disk_offset = volume.start = 0;

    fail_sector = 1;
    volume.cache_sector = 0xffffffffUL;
    assert(dt_map_build(&volume, 2, 1, &map, NULL, NULL));
    assert(map.cell[0].unreadable == 1 && map.cell[0].free == 0);
    assert(!dt_scan(&volume, &result, NULL, NULL));
    assert(!result.complete);
    free(image);
    puts("PASS: map coverage, zoom ranges, I/O failures and cancellation");
    return 0;
}
