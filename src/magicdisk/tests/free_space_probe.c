/* Test-only DOS free-space query, independent of MagicDisk's FAT scanner. */
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    union REGS r;
    struct SREGS s;
    unsigned long result[11];
    char path[] = "D:\\";

    if ((argc != 2 && argc != 3) || argv[1][1] || argv[1][0] < 'A' || argv[1][0] > 'Z')
        return 1;
    if (argc == 3) {
        memset(&r, 0, sizeof(r));
        r.h.ah = 0x36;
        r.h.dl = argv[1][0] - 'A' + 1;
        intdos(&r, &r);
        if (r.x.ax == 0xffff)
            return 1;
        printf("%lu bytes free\n", (unsigned long)r.x.ax * r.x.bx * r.x.cx);
        return 0;
    }
    path[0] = argv[1][0];
    memset(result, 0, sizeof(result));
    memset(&r, 0, sizeof(r));
    segread(&s);
    r.x.ax = 0x7303;
    r.x.cx = sizeof(result);
    r.x.dx = FP_OFF(path);
    r.x.di = FP_OFF(result);
    s.ds = FP_SEG(path);
    s.es = FP_SEG(result);
    intdosx(&r, &r, &s);
    if (r.x.cflag || (result[0] & 0xffff) < 20 || !result[1] || !result[2])
        return 1;
    printf("%lu bytes free\n", result[1] * result[2] * result[3]);
    return 0;
}
