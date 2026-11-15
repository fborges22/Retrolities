/* Test-only DOS file API probe; source is opened read-only. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dos.h>
#include <i86.h>

static unsigned critical_code;

static int __far critical(unsigned device, unsigned code, unsigned __far *header)
{
    (void)device;
    (void)header;
    critical_code = code;
    _hardresume(_HARDERR_FAIL);
    return _HARDERR_FAIL;
}

int main(int argc, char **argv)
{
    FILE *input, *output;
    unsigned char bytes[512];
    size_t count;
    unsigned long total = 0;
    int ok;

    if (argc != 3)
        return 2;
    _harderr(critical);
    {
        static unsigned char dpb[63];
        union REGS regs;
        struct SREGS segments;
        unsigned long root;

        memset(&regs, 0, sizeof(regs));
        segread(&segments);
        dpb[0] = 61;
        regs.w.ax = 0x7302;
        regs.w.cx = sizeof(dpb);
        regs.w.dx = argv[1][0] - 'A' + 1;
        regs.w.di = (unsigned)dpb;
        segments.es = segments.ds;
        intdosx(&regs, &regs, &segments);
        memcpy(&root, dpb + 2 + 0x35, 4);
        printf("DPB carry=%u root=%lu flags=%u\n", regs.w.cflag, root, dpb[2 + 0x18]);
    }
    printf("Opening %s\n", argv[1]);
    fflush(stdout);
    input = fopen(argv[1], "rb");
    if (!input) {
        printf("Open source failed: %s (%d), critical=%u\n", strerror(errno), errno, critical_code);
        return 1;
    }
    output = fopen(argv[2], "wb");
    if (!output) {
        printf("Open output failed: %s (%d)\n", strerror(errno), errno);
        fclose(input);
        return 1;
    }
    while ((count = fread(bytes, 1, sizeof(bytes), input)) != 0) {
        if (fwrite(bytes, 1, count, output) != count)
            break;
        total += count;
    }
    ok = !ferror(input) && !ferror(output);
    if (fclose(input))
        ok = 0;
    if (fclose(output))
        ok = 0;
    printf("Copied %lu bytes; status=%d\n", total, ok);
    return ok ? 0 : 1;
}
