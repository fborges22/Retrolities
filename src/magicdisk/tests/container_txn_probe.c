#include "disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    DtContainerPlan plan;
    unsigned char actual[512];
    char err[160];
    FILE *output;
    int ok, restore, deny;

    if (argc != 8 || strcmp(argv[1], "BIOS81"))
        return 2;
    restore = !strcmp(argv[5], "UNDO") || !strcmp(argv[5], "DENY");
    deny = !strcmp(argv[5], "DENY");
    if (!dt_bios_open(&disk, 0x81, err)) {
        puts(err);
        return 2;
    }
    ok = restore ? dt_container_review_journal(&disk, argv[6], &plan, err)
                 : dt_container_plan(&disk,
                                     1,
                                     (dt_u32)strtoul(argv[2], NULL, 10),
                                     (dt_u32)strtoul(argv[3], NULL, 10),
                                     (unsigned)strtoul(argv[4], NULL, 10),
                                     16,
                                     63,
                                     &plan,
                                     err);
    if (ok) {
        ok = dt_bios_enable_mbr_write(&disk, argv[6], err);
        if (deny) {
            int refused = !ok && strstr(err, "mounted DOS volume") != NULL;

            printf("REFUSAL: %s\n", ok ? "Unexpected write access" : err);
            dt_container_clear(&plan);
            dt_bios_close(&disk);
            return refused ? 0 : 2;
        }
    }
    if (ok)
        ok = restore ? dt_container_restore(&disk, argv[6], 0, err)
                     : dt_container_apply(&plan, argv[6], err);
    if (ok && (!disk.read(disk.ctx, 0, actual) ||
               memcmp(actual, restore ? plan.before : plan.after, 512))) {
        strcpy(err, "Container transaction did not match reviewed MBR");
        ok = 0;
    }
    if (ok && !strcmp(argv[5], "ROUND")) {
        ok = dt_container_restore(&disk, argv[6], 0, err);
        if (ok && (!disk.read(disk.ctx, 0, actual) || memcmp(actual, plan.before, 512))) {
            strcpy(err, "Container undo did not match original MBR");
            ok = 0;
        }
    }
    dt_bios_close(&disk);
    if (!ok) {
        puts(err);
        dt_container_clear(&plan);
        return 2;
    }
    output = fopen(argv[7], "wb");
    ok = output && fwrite(plan.after, 1, 512, output) == 512;
    if (output && fclose(output))
        ok = 0;
    printf("CONTAINER recovered=%u verified\n", plan.witness_count);
    dt_container_clear(&plan);
    return ok ? 0 : 2;
}
