#include "disk.h"
#include <assert.h>
#include <string.h>

#define START 0xffffff00UL

static unsigned visits, reads;
static int inject;

static int read_sector(void *ctx, dt_u32 sector, unsigned char *data)
{
    (void)ctx;
    assert(sector >= START && sector < 0xffffffffUL);
    reads++;
    memset(data, 0, 512);
    if ((inject == 1 && sector == START + 10) || (inject == 2 && sector == START + 1) ||
        (inject == 3 && sector == START && reads == 2))
        return 0;
    if (sector == START) {
        data[11] = 0;
        data[12] = 2;
        data[13] = 1;
        data[14] = 1;
        data[16] = 1;
        data[17] = 16;
        data[19] = 255;
        data[21] = 0xf8;
        data[22] = 1;
        data[510] = 0x55;
        data[511] = 0xaa;
    } else if (sector == START + 1) {
        data[0] = 0xf8;
        data[1] = data[2] = 0xff;
    }
    return 1;
}

static int found(void *ctx, const DtVolume *volume)
{
    (void)ctx;
    assert(volume->start == START && volume->total == 255 && volume->bits == 12);
    visits++;
    return 1;
}

int main(void)
{
    DtDisk disk;
    char err[160];

    memset(&disk, 0, sizeof(disk));
    disk.sectors = 0xffffffffUL;
    disk.read = read_sector;
    assert(dt_find_volumes(&disk, START, 255, found, NULL, NULL, NULL, err));
    assert(visits == 1 && reads == 257);
    visits = reads = 0;
    inject = 1;
    assert(!dt_find_volumes(&disk, START, 255, found, NULL, NULL, NULL, err));
    assert(visits == 1 && reads == 257 && strstr(err, "incomplete"));
    visits = reads = 0;
    inject = 2;
    assert(!dt_find_volumes(&disk, START, 1, found, NULL, NULL, NULL, err));
    assert(!visits && reads == 3 && strstr(err, "incomplete"));
    reads = 0;
    inject = 3;
    assert(!dt_find_volumes(&disk, START, 1, found, NULL, NULL, NULL, err));
    assert(!visits && reads == 2 && strstr(err, "incomplete"));
    reads = 0;
    assert(!dt_find_volumes(&disk, START, 256, found, NULL, NULL, NULL, err));
    assert(!dt_find_volumes(&disk, 0xffffffffUL, 1, found, NULL, NULL, NULL, err));
    assert(!reads);
    puts("PASS: end-of-disk 32-bit range, no wraparound, continued discovery after read gap");
    return 0;
}
