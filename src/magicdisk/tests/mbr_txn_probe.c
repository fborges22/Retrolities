#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtMbrPlan plan;
    char err[160], journal[32];
    unsigned char actual[512];
    FILE *output;
    int bios, ok;

    if (argc != 8)
        return 2;
    bios = !strcmp(argv[1], "BIOS81");
    ok = bios ? dt_bios_open(&disk, 0x81, err) : dt_image_open(&disk, argv[1], err);
    if (!ok) {
        puts(err);
        return 2;
    }
    ok = dt_mbr_plan(&disk,
                     (unsigned)strtoul(argv[3], NULL, 10),
                     (dt_u32)strtoul(argv[2], NULL, 10),
                     (unsigned)strtoul(argv[4], NULL, 10),
                     (unsigned)strtoul(argv[5], NULL, 10),
                     (unsigned)strtoul(argv[6], NULL, 10),
                     &plan,
                     err);
    if (ok) {
        sprintf(journal, "C:\\MBR%u.JRN", plan.bits);
        ok = bios && dt_bios_enable_mbr_write(&disk, journal, err);
        if (ok)
            ok = dt_mbr_apply(&plan, journal, err);
        if (ok)
            ok = disk.read(disk.ctx, 0, actual) && !memcmp(actual, plan.after, 512);
        if (ok)
            ok = dt_mbr_restore(&disk, journal, 0, err);
        if (ok)
            ok = disk.read(disk.ctx, 0, actual) && !memcmp(actual, plan.before, 512);
    }
    if (bios)
        dt_bios_close(&disk);
    else
        dt_image_close(&disk);
    if (!ok) {
        puts(err);
        return 2;
    }
    output = fopen(argv[7], "wb");
    if (!output)
        return 2;
    ok = fwrite(plan.after, 1, 512, output) == 512;
    if (fclose(output))
        ok = 0;
    printf("PLAN slot=%u start=%lu sectors=%lu FAT%u source=%u\n",
           plan.slot,
           (unsigned long)plan.start,
           (unsigned long)plan.total,
           plan.bits,
           plan.boot_source);
    return ok ? 0 : 2;
}
