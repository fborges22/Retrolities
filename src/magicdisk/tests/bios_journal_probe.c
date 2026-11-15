/* Journal integration on signed, hidden QEMU volumes; never a production entry point. */
#include "../disk.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    static const char signature[] = "MAGICDISK BIOS TEST MEDIA";
    DtDisk disk;
    DtVolume volume;
    DtPlan *plan = NULL;
    unsigned char mbr[512];
    dt_u32 start, count;
    char err[160];
    int ok = 0, result;

    if (argc != 4 || (argv[2][0] != 'C' && argv[2][0] != 'c') || argv[2][1] != ':' ||
        argv[2][2] != '\\' || !dt_bios_open(&disk, 0x81, err))
        return 2;
    strcpy(err, "Test media signature check failed");
    if (disk.write || !disk.read(disk.ctx, 0, mbr) ||
        memcmp(mbr, signature, sizeof(signature) - 1) || dt_word(mbr + 510) != 0xaa55)
        goto done;
    start = dt_dword(mbr + 454);
    count = dt_dword(mbr + 458);
    if (!dt_dos_bios_write_check(0x81, argv[2], err))
        goto done;
    strcpy(err, "Test write range could not be enabled");
    if (start != 63 || !dt_bios_test_writes(&disk, start, count, !strcmp(argv[3], "chs")) ||
        !dt_mount(&disk, start, count, &volume, err))
        goto done;
    err[0] = 0;
    if (!strcmp(argv[1], "mark") || !strcmp(argv[1], "rollback") ||
        !strcmp(argv[1], "journal-fail")) {
        plan = dt_plan_create();
        if (!plan || !dt_bad_plan(&volume, 1000, 1, plan, NULL, NULL, err))
            goto done;
        result = dt_plan_apply(plan, argv[2], NULL, NULL, err);
        if (!strcmp(argv[1], "journal-fail"))
            ok = !result && strstr(err, "Journal storage could not be flushed") != NULL;
        else
            ok = !strcmp(argv[1], "mark")
                     ? result
                     : !result && strstr(err, "original sectors restored") != NULL;
    } else if (!strcmp(argv[1], "restore")) {
        ok = dt_plan_restore(&volume, argv[2], err);
    } else if (!strcmp(argv[1], "refuse-torn")) {
        ok = !dt_plan_restore(&volume, argv[2], err) && strstr(err, "torn write") != NULL;
    } else if (!strcmp(argv[1], "restore-torn")) {
        ok = dt_plan_restore_torn_ex(&volume, argv[2], NULL, NULL, NULL, NULL, err) &&
             dt_plan_restore(&volume, argv[2], err);
    }
done:
    if (*err)
        puts(err);
    dt_plan_free(plan);
    dt_bios_close(&disk);
    puts(ok ? "PASS: BIOS journal operation" : "FAIL: BIOS journal operation");
    return ok ? 0 : 2;
}
