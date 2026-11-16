#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtContainerPlan plan;
    FILE *output;
    char err[160];
    int ok, bios;

    if (argc != 9)
        return 2;
    bios = !strcmp(argv[1], "BIOS81");
    ok = bios ? dt_bios_open(&disk, 0x81, err) : dt_image_open(&disk, argv[1], err);
    if (!ok) {
        puts(err);
        return 2;
    }
    ok = dt_container_plan(&disk,
                           (unsigned)strtoul(argv[2], NULL, 10),
                           (dt_u32)strtoul(argv[3], NULL, 10),
                           (dt_u32)strtoul(argv[4], NULL, 10),
                           (unsigned)strtoul(argv[5], NULL, 10),
                           (unsigned)strtoul(argv[6], NULL, 10),
                           (unsigned)strtoul(argv[7], NULL, 10),
                           &plan,
                           err);
    if (ok)
        ok = dt_container_revalidate(&plan, err);
    if (bios)
        dt_bios_close(&disk);
    else
        dt_image_close(&disk);
    if (!ok) {
        puts(err);
        dt_container_clear(&plan);
        return 2;
    }
    output = fopen(argv[8], "wb");
    ok = output && fwrite(plan.after, 1, 512, output) == 512;
    if (output && fclose(output))
        ok = 0;
    printf("CONTAINER base=%lu span=%lu recovered=%u\n",
           (unsigned long)plan.base,
           (unsigned long)plan.span,
           plan.witness_count);
    dt_container_clear(&plan);
    return ok ? 0 : 2;
}
