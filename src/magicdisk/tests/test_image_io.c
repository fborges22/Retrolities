/* Exercise production image I/O across signed-offset and EOF boundaries. */
#include "disk.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void pattern(unsigned char *bytes, dt_u32 sector, int changed)
{
    unsigned i;

    for (i = 0; i < 512; i++) {
        bytes[i] = (unsigned char)((sector >> ((i % 4) * 8)) ^ (i * 29 + 17));
        if (changed)
            bytes[i] ^= 0xff;
    }
}

int main(int argc, char **argv)
{
    DtDisk disk;
    dt_u32 sectors, positions[6];
    unsigned count = 2, i;
    unsigned char expected[512], bytes[512];
    char err[160];

    assert(argc == 3);
    sectors = (dt_u32)strtoul(argv[2], NULL, 10);
    positions[0] = 0;
    positions[1] = sectors - 1;
    if (sectors > 0x400001UL) {
        positions[count++] = 0x3fffffUL;
        positions[count++] = 0x400000UL;
        positions[count++] = 0x400001UL;
    }
    if (!dt_image_open(&disk, argv[1], err)) {
        puts(err);
        return 1;
    }
    assert(disk.sectors == sectors && !disk.write);
    for (i = 0; i < count; i++) {
        pattern(expected, positions[i], 0);
        assert(disk.read(disk.ctx, positions[i], bytes));
        assert(!memcmp(expected, bytes, 512));
    }
    assert(!disk.read(disk.ctx, sectors, bytes));
    assert(!disk.read(disk.ctx, 0xffffffffUL, bytes));
    dt_image_close(&disk);
    if (!dt_image_open_write(&disk, argv[1], err)) {
        puts(err);
        return 1;
    }
    assert(disk.sectors == sectors);
    assert(!disk.write(disk.ctx, sectors, bytes));
    assert(!disk.write(disk.ctx, 0xffffffffUL, bytes));
    for (i = 0; i < count; i++) {
        pattern(expected, positions[i], 1);
        assert(disk.write(disk.ctx, positions[i], expected));
        assert(disk.sync(disk.ctx));
        assert(disk.read(disk.ctx, positions[i], bytes));
        assert(!memcmp(expected, bytes, 512));
    }
    dt_image_close(&disk);
    assert(dt_image_open(&disk, argv[1], err));
    assert(disk.sectors == sectors);
    for (i = 0; i < count; i++) {
        pattern(expected, positions[i], 1);
        assert(disk.read(disk.ctx, positions[i], bytes));
        assert(!memcmp(expected, bytes, 512));
    }
    dt_image_close(&disk);
    puts("PASS: image reads, writes, commits, reopen and EOF bounds");
    return 0;
}
