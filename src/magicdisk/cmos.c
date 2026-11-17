/* Read-only CMOS setup diagnostics (Norton Disk Doctor style), IBM AT layout:
   0x00-0x09 RTC, 0x0B status B, 0x0D status D, 0x0E POST diagnostic status,
   0x10 floppy types, 0x14 equipment, 0x15-0x18 memory, 0x2E-0x2F checksum
   of 0x10-0x2D (high byte first), 0x30-0x31 POST extended memory. */
#include "disk.h"
#include <stdio.h>
#include <string.h>
#ifdef __WATCOMC__
#include <conio.h>
#include <i86.h>
#endif

static unsigned decode(const unsigned char *c, unsigned reg, int *valid)
{
    unsigned value = c[reg];

    if (c[0x0b] & 4)
        return value;
    if ((value & 15) > 9 || (value >> 4) > 9)
        *valid = 0;
    return (value >> 4) * 10 + (value & 15);
}

static void issue(unsigned *count, DtReport report, void *ctx, const char *code, unsigned reg,
                  const char *message)
{
    (*count)++;
    if (report)
        report(ctx, code, reg, message);
}

static unsigned floppy_count(unsigned types)
{
    return ((types >> 4) != 0) + ((types & 15) != 0);
}

unsigned dt_cmos_check(const unsigned char cmos[128], const DtCmosHost *host, DtReport report,
                       void *ctx)
{
    static const unsigned char days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const char *const diagnostic[] = {"", "", "time/date invalid", "fixed disk failed",
                                             "memory size mismatch", "configuration mismatch",
                                             "checksum bad", "power lost"};
    unsigned count = 0, i, sum = 0, stored, hour, day, month, year, sec, min, types;
    int valid = 1;
    char message[128];

    if (!(cmos[0x0d] & 0x80))
        issue(&count, report, ctx, "CMOS_BATTERY", 0x0d,
              "Real-time clock reports a dead or removed battery (status D bit 7 clear)");
    for (i = 2; i < 8; i++) {
        if (cmos[0x0e] & (1u << i)) {
            sprintf(message, "POST diagnostic status 0Eh: %s", diagnostic[i]);
            issue(&count, report, ctx, "CMOS_POST", 0x0e, message);
        }
    }
    for (i = 0x10; i <= 0x2d; i++)
        sum += cmos[i];
    stored = ((unsigned)cmos[0x2e] << 8) | cmos[0x2f];
    if ((sum & 0xffff) != stored) {
        sprintf(message, "Setup checksum %04Xh does not match bytes 10h-2Dh (%04Xh)", stored,
                sum & 0xffff);
        issue(&count, report, ctx, "CMOS_CHECKSUM", 0x2e, message);
    }
    sec = decode(cmos, 0x00, &valid);
    min = decode(cmos, 0x02, &valid);
    hour = cmos[0x04];
    if (!(cmos[0x0b] & 2)) {
        unsigned pm = hour & 0x80;

        hour &= 0x7f;
        hour = (cmos[0x0b] & 4) ? hour : (hour >> 4) * 10 + (hour & 15);
        if ((!(cmos[0x0b] & 4) && (cmos[0x04] & 0x0f) > 9) || hour < 1 || hour > 12)
            valid = 0;
        hour = hour % 12 + (pm ? 12 : 0);
    } else
        hour = decode(cmos, 0x04, &valid);
    day = decode(cmos, 0x07, &valid);
    month = decode(cmos, 0x08, &valid);
    year = decode(cmos, 0x09, &valid);
    if (!valid || sec > 59 || min > 59 || hour > 23 || month < 1 || month > 12 || day < 1 ||
        day > days[month - 1] || year > 99 || (month == 2 && day == 29 && year % 4)) {
        sprintf(message, "Clock holds an invalid date/time: %02X-%02X-%02X %02X:%02X:%02X",
                cmos[0x09], cmos[0x08], cmos[0x07], cmos[0x04], cmos[0x02], cmos[0x00]);
        issue(&count, report, ctx, "CMOS_CLOCK", 0x00, message);
    }
    types = cmos[0x10];
    if ((types >> 4) > 6 || (types & 15) > 6) {
        sprintf(message, "Unknown floppy drive type byte %02Xh", types);
        issue(&count, report, ctx, "CMOS_FLOPPY", 0x10, message);
    } else {
        unsigned equipment = (cmos[0x14] & 1) ? ((cmos[0x14] >> 6) + 1u) : 0;

        if (equipment != floppy_count(types)) {
            sprintf(message, "Equipment byte lists %u floppy drives; drive types list %u",
                    equipment, floppy_count(types));
            issue(&count, report, ctx, "CMOS_FLOPPY", 0x14, message);
        }
        if (host && host->floppies != DT_CMOS_UNKNOWN && host->floppies != floppy_count(types)) {
            sprintf(message, "Setup lists %u floppy drives; the BIOS reports %u",
                    floppy_count(types), host->floppies);
            issue(&count, report, ctx, "CMOS_FLOPPY", 0x10, message);
        }
    }
    if (host && host->base_kb != DT_CMOS_UNKNOWN &&
        (((unsigned)cmos[0x16] << 8) | cmos[0x15]) != host->base_kb) {
        sprintf(message, "Setup base memory %u KB differs from the BIOS count %u KB",
                ((unsigned)cmos[0x16] << 8) | cmos[0x15], host->base_kb);
        issue(&count, report, ctx, "CMOS_MEMORY", 0x15, message);
    }
    if (cmos[0x17] != cmos[0x30] || cmos[0x18] != cmos[0x31]) {
        sprintf(message, "Setup extended memory %u KB differs from POST-detected %u KB",
                ((unsigned)cmos[0x18] << 8) | cmos[0x17], ((unsigned)cmos[0x31] << 8) | cmos[0x30]);
        issue(&count, report, ctx, "CMOS_MEMORY", 0x17, message);
    }
    return count;
}

#ifdef __WATCOMC__
static unsigned char register_read(unsigned reg)
{
    /* Keep NMI enabled (bit 7 clear), as the BIOS leaves it. */
    outp(0x70, reg & 0x7f);
    return (unsigned char)inp(0x71);
}

int dt_cmos_read(unsigned char cmos[128], DtCmosHost *host, char *err)
{
    union REGS r;
    unsigned i, attempt;

    /* Avoid reading the clock while it updates (status A bit 7). */
    for (attempt = 0; attempt < 100000U && (register_read(0x0a) & 0x80); attempt++)
        ;
    _disable();
    for (i = 0; i < 128; i++)
        cmos[i] = register_read(i);
    outp(0x70, 0x0d);
    _enable();
    memset(&r, 0, sizeof(r));
    int386(0x12, &r, &r);
    host->base_kb = r.w.ax;
    memset(&r, 0, sizeof(r));
    r.h.ah = 8;
    r.h.dl = 0;
    int386(0x13, &r, &r);
    host->floppies = r.x.cflag ? DT_CMOS_UNKNOWN : r.h.dl;
    (void)err;
    return 1;
}
#else
int dt_cmos_read(unsigned char cmos[128], DtCmosHost *host, char *err)
{
    (void)cmos;
    (void)host;
    strcpy(err, "CMOS access needs MAGICDSK.EXE under DOS; pass a 128-byte dump file instead");
    return 0;
}
#endif
