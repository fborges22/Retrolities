#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtEbrLinkPlan plan;
    char err[160];
    FILE *output;
    int ok, bios;

    if (argc != 7)
        return 2;
    bios = !strcmp(argv[1], "BIOS81");
    ok = bios ? dt_bios_open(&disk, 0x81, err) : dt_image_open(&disk, argv[1], err);
    if (!ok) {
        puts(err);
        return 2;
    }
    ok = dt_ebr_link_plan(&disk,
                          (dt_u32)strtoul(argv[2], NULL, 10),
                          (dt_u32)strtoul(argv[3], NULL, 10),
                          (unsigned)strtoul(argv[4], NULL, 10),
                          (unsigned)strtoul(argv[5], NULL, 10),
                          &plan,
                          err);
    if (bios)
        dt_bios_close(&disk);
    else
        dt_image_close(&disk);
    if (!ok) {
        puts(err);
        return 2;
    }
    output = fopen(argv[6], "wb");
    if (!output) {
        dt_ebr_link_clear(&plan);
        return 2;
    }
    ok = fwrite(plan.after, 1, 512, output) == 512;
    if (fclose(output))
        ok = 0;
    printf("LINK source=%lu target=%lu recovered=%u\n",
           (unsigned long)plan.sector,
           (unsigned long)plan.target,
           plan.recovered);
    dt_ebr_link_clear(&plan);
    return ok ? 0 : 2;
}
