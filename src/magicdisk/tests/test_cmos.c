/* CMOS setup checks against synthetic IBM AT register dumps. */
#include "disk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char codes[16][24];
static unsigned found;

static void collect(void *ctx, const char *code, dt_u32 where, const char *message)
{
    (void)ctx;
    (void)where;
    (void)message;
    if (found < 16)
        strcpy(codes[found], code);
    found++;
}

static void checksum(unsigned char *c)
{
    unsigned i, sum = 0;

    for (i = 0x10; i <= 0x2d; i++)
        sum += c[i];
    c[0x2e] = (unsigned char)(sum >> 8);
    c[0x2f] = (unsigned char)sum;
}

/* 1999-12-31 23:59:58 BCD, 24-hour; one 1.44 MB drive; 640 KB + 15 MB. */
static void healthy(unsigned char *c)
{
    memset(c, 0, 128);
    c[0x00] = 0x58;
    c[0x02] = 0x59;
    c[0x04] = 0x23;
    c[0x07] = 0x31;
    c[0x08] = 0x12;
    c[0x09] = 0x99;
    c[0x0b] = 0x02;
    c[0x0d] = 0x80;
    c[0x10] = 0x40;
    c[0x14] = 0x01;
    c[0x15] = 0x80;
    c[0x16] = 0x02;
    c[0x17] = c[0x30] = 0x00;
    c[0x18] = c[0x31] = 0x3c;
    checksum(c);
}

static unsigned check(const unsigned char *c, const DtCmosHost *host, const char *expect)
{
    unsigned i, n;

    found = 0;
    n = dt_cmos_check(c, host, collect, NULL);
    assert(n == found);
    if (expect) {
        for (i = 0; i < found && i < 16; i++)
            if (!strcmp(codes[i], expect))
                break;
        if (i == found) {
            fprintf(stderr, "expected %s, got %u findings\n", expect, found);
            abort();
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    unsigned char c[128];
    DtCmosHost host = {1, 640}, unknown = {DT_CMOS_UNKNOWN, DT_CMOS_UNKNOWN};
    char command[600];
    FILE *dump;

    assert(argc == 2);
    healthy(c);
    assert(check(c, &host, NULL) == 0);
    assert(check(c, NULL, NULL) == 0);
    /* Binary mode and 12-hour PM encoding are valid too. */
    c[0x0b] = 0x04;
    c[0x00] = 58, c[0x02] = 59, c[0x04] = 0x80 | 11, c[0x07] = 31, c[0x08] = 12, c[0x09] = 99;
    assert(check(c, &host, NULL) == 0);
    c[0x04] = 0x80 | 13;
    check(c, &host, "CMOS_CLOCK");
    healthy(c);
    c[0x0d] = 0;
    check(c, &host, "CMOS_BATTERY");
    healthy(c);
    c[0x0e] = 0xc0;
    assert(check(c, &host, "CMOS_POST") == 2);
    healthy(c);
    c[0x2f] ^= 1;
    check(c, &host, "CMOS_CHECKSUM");
    healthy(c);
    c[0x07] = 0x31, c[0x08] = 0x04;
    check(c, &host, "CMOS_CLOCK");
    healthy(c);
    c[0x07] = 0x29, c[0x08] = 0x02, c[0x09] = 0x01;
    check(c, &host, "CMOS_CLOCK");
    c[0x09] = 0x00;
    assert(check(c, &host, NULL) == 0);
    healthy(c);
    c[0x00] = 0x5a;
    check(c, &host, "CMOS_CLOCK");
    healthy(c);
    c[0x10] = 0x44;
    checksum(c);
    check(c, &host, "CMOS_FLOPPY");
    assert(check(c, &unknown, "CMOS_FLOPPY") == 1);
    c[0x14] = 0x41;
    checksum(c);
    check(c, &host, "CMOS_FLOPPY");
    assert(check(c, &unknown, NULL) == 0);
    healthy(c);
    c[0x10] = 0x90;
    checksum(c);
    check(c, &host, "CMOS_FLOPPY");
    healthy(c);
    host.base_kb = 639;
    assert(check(c, &host, "CMOS_MEMORY") == 1);
    host.base_kb = 640;
    c[0x31] = 0x3b;
    check(c, &host, "CMOS_MEMORY");

    /* CLI: a dump file is checked without hardware access. */
    healthy(c);
    dump = fopen("cmos-good.bin", "wb");
    assert(dump && fwrite(c, 1, 128, dump) == 128 && !fclose(dump));
    sprintf(command, "\"%s\" --cmos cmos-good.bin > cmos.out", argv[1]);
    assert(system(command) == 0);
    c[0x0d] = 0;
    dump = fopen("cmos-bad.bin", "wb");
    assert(dump && fwrite(c, 1, 128, dump) == 128 && !fclose(dump));
    sprintf(command, "\"%s\" --cmos cmos-bad.bin | grep -q CMOS_BATTERY", argv[1]);
    assert(system(command) == 0);
    sprintf(command, "\"%s\" --cmos cmos-bad.bin > cmos.out", argv[1]);
    assert(system(command) != 0);
    remove("cmos-good.bin");
    remove("cmos-bad.bin");
    remove("cmos.out");
    puts("PASS: CMOS battery, POST status, checksum, BCD/binary/12-hour clock, calendar, "
         "floppy and memory checks");
    return 0;
}
