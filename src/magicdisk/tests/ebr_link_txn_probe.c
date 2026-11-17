#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtEbrLinkPlan plan;
    char err[160], journal[32];
    unsigned char actual[512];
    FILE *output;
    int ok, bios, deny;

    if (argc != 7 && argc != 8)
        return 2;
    deny = argc == 8 && !strcmp(argv[7], "DENY");
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
    if (ok) {
        sprintf(journal, "C:\\L%lu.JRN", (unsigned long)plan.target);
        ok = bios && dt_bios_enable_writes(&disk, plan.sector, 1, journal, err);
        if (deny) {
            int refused = !ok && strstr(err, "mounted DOS volume") != NULL;

            printf("REFUSAL: %s\n", ok ? "Unexpected write access" : err);
            dt_ebr_link_clear(&plan);
            dt_bios_close(&disk);
            return refused ? 0 : 2;
        }
        if (ok)
            ok = dt_ebr_link_apply(&plan, journal, err);
        if (ok && (!disk.read(disk.ctx, plan.sector, actual) || memcmp(actual, plan.after, 512))) {
            strcpy(err, "Applied link sector did not match preview");
            ok = 0;
        }
        if (ok)
            ok = dt_ebr_link_restore(&disk, journal, 0, err);
        if (ok && (!disk.read(disk.ctx, plan.sector, actual) || memcmp(actual, plan.before, 512))) {
            strcpy(err, "Restored link sector did not match original");
            ok = 0;
        }
    }
    if (bios)
        dt_bios_close(&disk);
    else
        dt_image_close(&disk);
    if (!ok) {
        puts(err);
        dt_ebr_link_clear(&plan);
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
