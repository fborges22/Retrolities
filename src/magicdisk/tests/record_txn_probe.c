#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtEbrRecordPlan plan;
    unsigned char actual[512];
    char err[160];
    FILE *output;
    int ok, restore, deny;

    if (argc != 9 || strcmp(argv[1], "BIOS81"))
        return 2;
    restore = !strcmp(argv[6], "UNDO") || !strcmp(argv[6], "DENYUNDO");
    deny = !strcmp(argv[6], "DENY") || !strcmp(argv[6], "DENYUNDO");
    if (!dt_bios_open(&disk, 0x81, err)) {
        puts(err);
        return 2;
    }
    ok = restore ? dt_ebr_record_review_journal(&disk, argv[7], &plan, err)
                 : dt_ebr_record_plan(&disk,
                                      (dt_u32)strtoul(argv[2], NULL, 10),
                                      (dt_u32)strtoul(argv[3], NULL, 10),
                                      (unsigned)strtoul(argv[5], NULL, 10),
                                      (dt_u32)strtoul(argv[4], NULL, 10),
                                      16,
                                      63,
                                      &plan,
                                      err);
    if (ok) {
        ok = dt_bios_enable_writes(&disk, plan.logical.sector, 1, argv[7], err);
        if (deny) {
            int refused = !ok && strstr(err, "mounted DOS volume") != NULL;

            printf("REFUSAL: %s\n", ok ? "Unexpected write access" : err);
            dt_ebr_record_clear(&plan);
            dt_bios_close(&disk);
            return refused ? 0 : 2;
        }
    }
    if (ok)
        ok = restore ? dt_ebr_record_restore(&disk, argv[7], 0, err)
                     : dt_ebr_record_apply(&plan, argv[7], err);
    if (ok && (!disk.read(disk.ctx, plan.logical.sector, actual) ||
               memcmp(actual, restore ? plan.logical.entry.before : plan.after, 512))) {
        strcpy(err, "Record transaction did not match preview");
        ok = 0;
    }
    if (ok && !strcmp(argv[6], "ROUND")) {
        ok = dt_ebr_record_restore(&disk, argv[7], 0, err);
        if (ok && (!disk.read(disk.ctx, plan.logical.sector, actual) ||
                   memcmp(actual, plan.logical.entry.before, 512))) {
            strcpy(err, "Record undo did not match original sector");
            ok = 0;
        }
    }
    dt_bios_close(&disk);
    if (!ok) {
        puts(err);
        dt_ebr_record_clear(&plan);
        return 2;
    }
    output = fopen(argv[8], "wb");
    ok = output && fwrite(plan.after, 1, 512, output) == 512;
    if (output && fclose(output))
        ok = 0;
    printf("RECORD tail=%u verified\n", plan.witness_count);
    dt_ebr_record_clear(&plan);
    return ok ? 0 : 2;
}
