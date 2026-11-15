/* Test-only capability probe. No raw writes; reports are saved after unlocking. */
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned ax, bx, cx, dx, result, carry;
} Call;

static Call calls[64];
static unsigned count;

#pragma pack(push, 1)

typedef struct {
    unsigned long edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} RealRegs;

#pragma pack(pop)

typedef struct {
    unsigned drive, category, carry, result;
    unsigned char data[512];
} Map;

static Map maps[9];

static int drive_map(Map *map, unsigned drive, unsigned category)
{
    union REGS r;
    struct SREGS s;
    RealRegs rm;
    unsigned segment, selector;

    memset(map, 0, sizeof(*map));
    map->drive = drive;
    map->category = category;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = 64;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        return 0;
    segment = r.w.ax;
    selector = r.w.dx;
    map->data[0] = 16;
    _fmemcpy(MK_FP(selector, 0), map->data, sizeof(map->data));
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x440d;
    rm.ebx = drive;
    rm.ecx = category | 0x6f;
    rm.ds = (unsigned short)segment;
    if (category == 0x7305) {
        unsigned short packet[5] = {0, 0, 1, 512, 0};

        packet[4] = (unsigned short)segment;
        _fmemcpy(MK_FP(selector, 0), packet, sizeof(packet));
        rm.eax = 0x7305;
        rm.ebx = 0;
        rm.ecx = 0xffff;
        rm.edx = drive;
    }
    memset(&r, 0, sizeof(r));
    segread(&s);
    r.w.ax = 0x0300;
    r.h.bl = 0x21;
    r.x.edi = (unsigned long)&rm;
    s.es = s.ds;
    int386x(0x31, &r, &r, &s);
    map->carry = r.x.cflag || (rm.flags & 1);
    map->result = (unsigned)(rm.eax & 0xffff);
    _fmemcpy(map->data, MK_FP(selector, category == 0x7305 ? 512 : 0), sizeof(map->data));
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = (unsigned short)selector;
    int386(0x31, &r, &r);
    return !r.x.cflag;
}

static int invoke(unsigned ax, unsigned bx, unsigned cx, unsigned dx)
{
    union REGS r;
    struct SREGS s;
    RealRegs rm;
    Call *call = &calls[count++];

    memset(&r, 0, sizeof(r));
    r.w.ax = (unsigned short)ax;
    r.w.bx = (unsigned short)bx;
    r.w.cx = (unsigned short)cx;
    r.w.dx = (unsigned short)dx;
    call->ax = ax;
    call->bx = bx;
    call->cx = cx;
    call->dx = dx;
    memset(&rm, 0, sizeof(rm));
    rm.eax = ax;
    rm.ebx = bx;
    rm.ecx = cx;
    rm.edx = dx;
    memset(&r, 0, sizeof(r));
    segread(&s);
    r.w.ax = 0x0300;
    r.h.bl = 0x21;
    r.x.edi = (unsigned long)&rm;
    s.es = s.ds;
    int386x(0x31, &r, &r, &s);
    call->result = (unsigned)(rm.eax & 0xffff);
    call->carry = r.x.cflag || (rm.flags & 1);
    return !call->carry;
}

int main(void)
{
    unsigned i, j, drive, category, map_count = 0;
    int failed = 0;
    FILE *report;

    invoke(0x3306, 0, 0, 0);
    /* Lock/unlock a whole physical device with Microsoft's BL=BIOS drive ABI. */
    for (i = 0; i < 2; i++) {
        drive = i ? 0x80 : 0;
        if (invoke(0x440d, drive, 0x084b, 0)) {
            if (!invoke(0x440d, drive, 0x086b, 0))
                failed = 1;
        }
    }
    /* Compare alternate register conventions without performing disk writes. */
    for (drive = 3; drive <= 4; drive++) {
        if (invoke(0x440d, drive, 0x084b, 0)) {
            if (!invoke(0x440d, drive, 0x086b, 0))
                failed = 1;
        }
        if (invoke(0x440d, drive << 8, 0x084b, 0)) {
            if (!invoke(0x440d, drive << 8, 0x086b, 0))
                failed = 1;
        }
    }
    /* The DOS 7.1 raw-access lock is requested after the outer volume lock. */
    for (drive = 3; drive <= 4; drive++) {
        if (invoke(0x440d, drive, 0x084a, 0)) {
            if (invoke(0x440d, 0x0400 | drive, 0x084a, 0)) {
                if (!invoke(0x440d, drive, 0x086a, 0))
                    failed = 1;
            }
            if (!invoke(0x440d, drive, 0x086a, 0))
                failed = 1;
        }
    }
    /* Logical C: is FAT16; D: is the generated FAT32 logical partition. */
    for (drive = 3; drive <= 4; drive++) {
        for (category = 0x0800; category <= 0x4800; category += 0x4000) {
            if (invoke(0x440d, drive, category | 0x4a, 0)) {
                if (!invoke(0x440d, drive, category | 0x6a, 0))
                    failed = 1;
            }
        }
        invoke(0x710d, 0, 0, drive);
        invoke(0x710d, 0, 1, drive);
    }
    for (drive = 1; drive <= 4; drive++) {
        if (drive == 2)
            continue;
        for (category = 0x0800; category <= 0x4800; category += 0x4000)
            if (!drive_map(&maps[map_count++], drive, category))
                failed = 1;
        if (!drive_map(&maps[map_count], drive, 0x7305) || maps[map_count].carry ||
            maps[map_count].data[510] != 0x55 || maps[map_count].data[511] != 0xaa)
            failed = 1;
        map_count++;
    }
    report = fopen("A:\\LOCKS.TXT", "wt");
    if (!report)
        return 1;
    for (i = 0; i < count; i++) {
        Call *call = &calls[i];

        fprintf(report,
                "AX=%04X BX=%04X CX=%04X DX=%04X -> AX=%04X CF=%u\n",
                call->ax,
                call->bx,
                call->cx,
                call->dx,
                call->result,
                call->carry);
    }
    for (i = 0; i < map_count; i++) {
        Map *map = &maps[i];

        fprintf(report,
                "MAP drive=%u category=%04X AX=%04X CF=%u data=",
                map->drive,
                map->category,
                map->result,
                map->carry);
        for (j = 0; j < 16; j++)
            fprintf(report, "%02X", map->data[j]);
        fprintf(report, " signature=%02X%02X\n", map->data[510], map->data[511]);
    }
    if (fclose(report))
        failed = 1;
    return failed;
}
