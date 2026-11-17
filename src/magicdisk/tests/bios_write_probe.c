/* Disposable QEMU media only: this binary is never part of MagicDisk's UI. */
#include "../disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    static const char signature[] = "MAGICDISK BIOS TEST MEDIA";
    DtDisk disk;
    dt_u32 first, count, i;
    unsigned drive, byte;
    unsigned char before[512], data[512], verify[512];
    char err[160];
    FILE *saved;
    int restore, force_chs, ok = 0;

    if (argc != 7)
        return 2;
    drive = (unsigned)strtoul(argv[1], NULL, 16);
    first = (dt_u32)strtoul(argv[2], NULL, 10);
    count = (dt_u32)strtoul(argv[3], NULL, 10);
    restore = !strcmp(argv[4], "restore");
    force_chs = !strcmp(argv[6], "chs");
    if ((drive != 1 && drive != 0x81) || !count || count > 16 ||
        (!restore && strcmp(argv[4], "write")) || !dt_bios_open(&disk, drive, err))
        return 2;
    if (disk.write || !disk.read(disk.ctx, 0, before) ||
        memcmp(before, signature, sizeof(signature) - 1) ||
        dt_bios_test_writes(&disk, 0, count, force_chs) ||
        dt_bios_test_writes(&disk, disk.sectors, 1, force_chs) ||
        dt_bios_test_writes(&disk, first, 0xffffffffUL, force_chs) ||
        !dt_bios_test_writes(&disk, first, count, force_chs))
        goto done;
    /* Adjacent sectors, sector zero, device end and unsigned wrap are excluded. */
    if (disk.write(disk.ctx, first - 1, before) || disk.write(disk.ctx, first + count, before) ||
        disk.write(disk.ctx, disk.sectors, before) || disk.write(disk.ctx, 0xffffffffUL, before))
        goto done;
    printf("TEST drive=%02X first=%lu count=%lu mode=%s operation=%s\n",
           drive,
           (unsigned long)first,
           (unsigned long)count,
           argv[6],
           argv[4]);
    fflush(stdout);
    saved = restore ? fopen(argv[5], "rb") : dt_file_create(argv[5]);
    if (!saved)
        goto done;
    if (!restore) {
        for (i = 0; i < count; i++) {
            if (!disk.read(disk.ctx, first + i, before) || fwrite(before, 1, 512, saved) != 512)
                break;
        }
        if (i != count || !dt_file_commit(saved)) {
            fclose(saved);
            goto done;
        }
    }
    ok = 1;
    for (i = 0; i < count; i++) {
        if (restore) {
            if (fread(data, 1, 512, saved) != 512) {
                ok = 0;
                break;
            }
        } else {
            for (byte = 0; byte < 512; byte++)
                data[byte] = (unsigned char)((first + i) * 17 + byte * 13 + drive);
        }
        if (!disk.write(disk.ctx, first + i, data)) {
            printf("WRITE_FAILED sector=%lu\n", (unsigned long)(first + i));
            ok = 0;
            break;
        }
        if (!disk.read(disk.ctx, first + i, verify) || memcmp(data, verify, 512)) {
            ok = 0;
            break;
        }
    }
    if (fclose(saved))
        ok = 0;
done:
    dt_bios_close(&disk);
    puts(ok ? "PASS: bounded BIOS transfer verified" : "FAIL: BIOS transfer test");
    return ok ? 0 : 2;
}
