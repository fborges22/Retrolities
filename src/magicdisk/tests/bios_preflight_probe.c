/* Read-only preflight checks on the disposable QEMU topology. */
#include "../disk.h"
#include <string.h>

int main(int argc, char **argv)
{
    DtDisk disk;
    char err[160];
    int mounted, unknown, cached, result;

    if (argc != 2 || (strcmp(argv[1], "mounted") && strcmp(argv[1], "hidden") &&
                      strcmp(argv[1], "unknown") && strcmp(argv[1], "cached")))
        return 2;
    cached = !strcmp(argv[1], "cached");
    mounted = !strcmp(argv[1], "mounted");
    unknown = !strcmp(argv[1], "unknown");
    if (!dt_bios_open(&disk, 0x81, err))
        return 2;
    if (disk.write) {
        dt_bios_close(&disk);
        return 2;
    }
    dt_bios_close(&disk);
    if (unknown) {
        FILE *ram = fopen("D:\\RAMTEST.TXT", "rb");

        if (!ram) {
            puts("RAM disk fixture unavailable");
            return 2;
        }
        fclose(ram);
    }
    result = dt_dos_bios_write_check(0x81, "C:\\NEW.MDJ", err);
    if (cached) {
        if (result || !strstr(err, "Resident disk cache detected")) {
            puts(err);
            return 2;
        }
        puts("PASS: resident disk cache refused; production backend remains read-only");
        return 0;
    }
    printf("CHECK result=%d expected=%d detail=%s\n", result, !mounted && !unknown, err);
    if (result != (!mounted && !unknown) ||
        (!result &&
         !strstr(err,
                 unknown ? "Cannot establish all DOS drive mappings" : "mounted DOS volume"))) {
        puts(err);
        return 2;
    }
    if (dt_dos_bios_write_check(0x80, "C:\\NEW.MDJ", err) || !strstr(err, "mounted DOS volume"))
        return 2;
    if (dt_dos_bios_write_check(0, "C:\\NEW.MDJ", err) || !strstr(err, "hard disk"))
        return 2;
    if (dt_dos_bios_write_check(256, "C:\\NEW.MDJ", err) || !strstr(err, "hard disk"))
        return 2;
    if (!mounted) {
        if (dt_dos_bios_write_check(0x81, "Z:\\NEW.MDJ", err) ||
            !strstr(err, "different known local physical disk"))
            return 2;
        if (dt_dos_bios_write_check(0x81, "D:\\NEW.MDJ", err) ||
            !strstr(err, "different known local physical disk"))
            return 2;
    }
    puts("PASS: BIOS write preflight; production backend remains read-only");
    return 0;
}
