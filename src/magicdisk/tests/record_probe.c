#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtEbrRecordPlan plan;
    char err[160];
    FILE *output;
    int ok, bios;

    if (argc != 9)
        return 2;
    bios = !strcmp(argv[1], "BIOS81");
    ok = bios ? dt_bios_open(&disk, 0x81, err) : dt_image_open(&disk, argv[1], err);
    if (!ok) {
        puts(err);
        return 2;
    }
    ok = dt_ebr_record_plan(&disk,
                            (dt_u32)strtoul(argv[2], NULL, 10),
                            (dt_u32)strtoul(argv[3], NULL, 10),
                            (unsigned)strtoul(argv[5], NULL, 10),
                            (dt_u32)strtoul(argv[4], NULL, 10),
                            (unsigned)strtoul(argv[6], NULL, 10),
                            (unsigned)strtoul(argv[7], NULL, 10),
                            &plan,
                            err);
    if (ok)
        ok = dt_ebr_record_revalidate(&plan, err);
    if (bios)
        dt_bios_close(&disk);
    else
        dt_image_close(&disk);
    if (!ok) {
        puts(err);
        dt_ebr_record_clear(&plan);
        return 2;
    }
    output = fopen(argv[8], "wb");
    ok = output && fwrite(plan.after, 1, 512, output) == 512;
    if (output && fclose(output))
        ok = 0;
    printf("RECORD sector=%lu volume=%lu next=%lu tail=%u\n",
           (unsigned long)plan.logical.sector,
           (unsigned long)plan.logical.entry.start,
           (unsigned long)plan.next,
           plan.witness_count);
    dt_ebr_record_clear(&plan);
    return ok ? 0 : 2;
}
