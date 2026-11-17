/* Test-only BIOS sector reader, used to locate DOS metadata side effects. */
#include "../disk.h"
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    unsigned char sector[512];
    char err[160];
    int ok;

    if (argc != 3 || !dt_bios_open(&disk, (unsigned)strtoul(argv[1], NULL, 16), err))
        return 2;
    ok = disk.read(disk.ctx, (dt_u32)strtoul(argv[2], NULL, 10), sector);
    dt_bios_close(&disk);
    if (!ok)
        return 2;
    setmode(fileno(stdout), O_BINARY);
    return fwrite(sector, 1, 512, stdout) == 512 ? 0 : 2;
}
