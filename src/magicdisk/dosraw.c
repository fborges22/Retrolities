/* Mounted-volume access through the DOS kernel's absolute-sector interfaces. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>
#ifdef __WATCOMC__
#include <dos.h>
#include <i86.h>

#pragma pack(push, 1)

typedef struct {
    dt_u32 edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} RealRegs;

#pragma pack(pop)

typedef struct {
    unsigned drive, segment, selector, locks, critical_segment, critical_offset;
    int legacy, freedos, writable;
    dt_u32 sectors;
} DosDisk;

static int real_interrupt(unsigned number, RealRegs *rm)
{
    union REGS r;
    struct SREGS s;

    memset(&r, 0, sizeof(r));
    segread(&s);
    r.w.ax = 0x0300;
    r.h.bl = (unsigned char)number;
    r.x.edi = (unsigned long)rm;
    s.es = s.ds;
    int386x(0x31, &r, &r, &s);
    return !r.x.cflag && !(rm->flags & 1);
}

static int __far critical_error(unsigned device, unsigned code, unsigned __far *header)
{
    (void)device;
    (void)code;
    (void)header;
    _hardresume(_HARDERR_FAIL);
    return 0;
}

static int legacy_transfer(DosDisk *d, RealRegs *rm, dt_u32 sector, int write)
{
    union REGS r;
    struct SREGS s;
    unsigned char code[] = {0xcd, 0x25, 0x5b, 0xcb};

    /* INT 25/26 leave an extra FLAGS word on the stack. POP BX discards it
       without changing the returned carry flag; RETF returns to DPMI. */
    code[1] = write ? 0x26 : 0x25;
    _fmemcpy(MK_FP(d->selector, 1024), code, sizeof(code));
    rm->eax = d->drive - 1;
    if (d->sectors > 1 && d->sectors <= 65535) {
        rm->ecx = 1;
        rm->edx = sector;
        rm->ebx = 512;
    }
    rm->cs = (unsigned short)d->segment;
    rm->ip = 1024;
    memset(&r, 0, sizeof(r));
    segread(&s);
    r.w.ax = 0x0301;
    r.x.edi = (unsigned long)rm;
    s.es = s.ds;
    int386x(0x31, &r, &r, &s);
    return !r.x.cflag && !(rm->flags & 1);
}

static int lock_call(DosDisk *d, unsigned level, int unlock)
{
    RealRegs rm;

    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x440d;
    rm.ebx = d->drive | (level << 8);
    rm.ecx = unlock ? 0x086a : 0x084a;
    return real_interrupt(0x21, &rm);
}

static int dos_sync(void *ctx)
{
    RealRegs rm;
    DosDisk *d = (DosDisk *)ctx;

    if (d->writable && !d->legacy) {
        unsigned char reset[24];

        memset(reset, 0, sizeof(reset));
        reset[0] = sizeof(reset);
        /* Raw FAT writes invalidate DOS's cached free count. Rebuild its DPB
           from disk before Reset Disk can overwrite FSInfo with that stale cache. */
        reset[4] = 2;
        _fmemcpy(MK_FP(d->selector, 0), reset, sizeof(reset));
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x7304;
        rm.ecx = sizeof(reset);
        rm.edx = d->drive;
        rm.es = (unsigned short)d->segment;
        if (!real_interrupt(0x21, &rm))
            return 0;
    }
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x0d00;
    /* DOS Reset Disk writes dirty buffers and marks its internal buffers free. */
    return real_interrupt(0x21, &rm);
}

static int transfer(DosDisk *d, dt_u32 sector, unsigned char *data, int write)
{
    RealRegs rm;
    unsigned short packet[5];

    if (sector >= d->sectors || (write && !d->writable))
        return 0;
    packet[0] = (unsigned short)sector;
    packet[1] = (unsigned short)(sector >> 16);
    packet[2] = 1;
    packet[3] = 512;
    packet[4] = (unsigned short)d->segment;
    _fmemcpy(MK_FP(d->selector, 0), packet, sizeof(packet));
    if (write)
        _fmemcpy(MK_FP(d->selector, 512), data, 512);
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x7305;
    rm.ecx = 0xffff;
    rm.edx = d->drive;
    rm.esi = write ? 1 : 0;
    rm.ds = (unsigned short)d->segment;
    if (!(d->legacy ? legacy_transfer(d, &rm, sector, write) : real_interrupt(0x21, &rm)))
        return 0;
    if (!write)
        _fmemcpy(data, MK_FP(d->selector, 512), 512);
    return 1;
}

static int dos_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    return transfer((DosDisk *)ctx, sector, data, 0);
}

static int dos_write(void *ctx, dt_u32 sector, const unsigned char *data)
{
    return transfer((DosDisk *)ctx, sector, (unsigned char *)data, 1);
}

static int canonical_drive(DosDisk *d, const char *path)
{
    RealRegs rm;
    unsigned char result[128];

    if (strlen(path) >= sizeof(result))
        return 0;
    _fmemcpy(MK_FP(d->selector, 0), path, strlen(path) + 1);
    memset(result, 0, sizeof(result));
    _fmemcpy(MK_FP(d->selector, 256), result, sizeof(result));
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x6000;
    rm.ds = rm.es = (unsigned short)d->segment;
    rm.edi = 256;
    if (!real_interrupt(0x21, &rm))
        return 0;
    _fmemcpy(result, MK_FP(d->selector, 256), sizeof(result));
    if (result[0] < 'A' || result[0] > 'Z' || result[1] != ':' || result[2] != '\\')
        return 0;
    return result[0] - 'A' + 1;
}

static int separate_destination(void *ctx, const char *path)
{
    DosDisk *d = (DosDisk *)ctx;
    char root[4] = "A:\\";
    int source, destination;

    root[0] = (char)('A' + d->drive - 1);
    source = canonical_drive(d, root);
    destination = canonical_drive(d, path);
    return source && destination && source != destination;
}

static int local_drive(unsigned drive)
{
    RealRegs rm;

    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4409;
    rm.ebx = drive;
    return real_interrupt(0x21, &rm) && !(rm.edx & 0x1000);
}

int dt_dos_close(DtDisk *disk)
{
    DosDisk *d = (DosDisk *)disk->ctx;
    union REGS r;
    int ok = 1;

    if (d) {
        if ((d->locks || d->writable) && !dos_sync(d))
            ok = 0;
        while (d->locks) {
            if (!lock_call(d, 0, 1))
                ok = 0;
            d->locks--;
        }
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x0201;
        r.h.bl = 0x24;
        r.w.cx = (unsigned short)d->critical_segment;
        r.w.dx = (unsigned short)d->critical_offset;
        int386(0x31, &r, &r);
        if (r.x.cflag)
            ok = 0;
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x0101;
        r.w.dx = (unsigned short)d->selector;
        int386(0x31, &r, &r);
        if (r.x.cflag)
            ok = 0;
        free(d);
    }
    memset(disk, 0, sizeof(*disk));
    return ok;
}

int dt_dos_open(DtDisk *disk, unsigned drive, const char *journal, char *err)
{
    DosDisk *d;
    RealRegs rm;
    union REGS r;
    unsigned char boot[512];
    char root[4] = "A:\\";
    int target, destination, legacy, freedos;
    unsigned major, minor;

    memset(disk, 0, sizeof(*disk));
    if (!drive || drive > 26) {
        strcpy(err, "DOS drive must be a letter from A through Z");
        return 0;
    }
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3000;
    freedos = real_interrupt(0x21, &rm) && ((rm.ebx >> 8) & 255) == 0xfd;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3306;
    if (!real_interrupt(0x21, &rm)) {
        strcpy(err, "Cannot identify the DOS kernel");
        return 0;
    }
    major = rm.ebx & 255;
    minor = (rm.ebx >> 8) & 255;
    legacy = !freedos && major == 6 && minor == 22;
    if (!legacy && !freedos && !(major == 7 && minor >= 10 && minor < 100)) {
        strcpy(err, "Mounted access requires MS-DOS 6.22, DOS 7.1 or FreeDOS");
        return 0;
    }
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x1600;
    if (!real_interrupt(0x2f, &rm) || (rm.eax & 255)) {
        strcpy(err, "Mounted-volume access currently requires booting into real DOS mode");
        return 0;
    }
    d = (DosDisk *)calloc(1, sizeof(*d));
    if (!d) {
        strcpy(err, "Insufficient memory for DOS volume");
        return 0;
    }
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = 128;
    int386(0x31, &r, &r);
    if (r.x.cflag) {
        free(d);
        strcpy(err, "Cannot allocate DOS transfer buffer");
        return 0;
    }
    d->segment = r.w.ax;
    d->selector = r.w.dx;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0200;
    r.h.bl = 0x24;
    int386(0x31, &r, &r);
    if (r.x.cflag) {
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x0101;
        r.w.dx = (unsigned short)d->selector;
        int386(0x31, &r, &r);
        free(d);
        strcpy(err, "Cannot preserve the DOS critical-error handler");
        return 0;
    }
    d->critical_segment = r.w.cx;
    d->critical_offset = r.w.dx;
    _harderr(critical_error);
    d->drive = drive;
    d->legacy = legacy;
    d->freedos = freedos;
    d->sectors = 1;
    disk->ctx = d;
    disk->read = dos_read;
    disk->separate_destination = separate_destination;
    if (legacy || drive <= 2) {
        /* Absolute reads do not themselves build the DPB for newly inserted
           media. Ask DOS to establish its geometry before accessing sectors. */
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x3200;
        rm.edx = drive;
        if (!real_interrupt(0x21, &rm) || (rm.eax & 255)) {
            strcpy(err, "Cannot initialize the DOS volume geometry");
            goto failed;
        }
    }
    if (journal) {
        root[0] = (char)('A' + drive - 1);
        target = canonical_drive(d, root);
        destination = canonical_drive(d, journal);
        if (!target || !destination || target == destination || !local_drive((unsigned)target) ||
            !local_drive((unsigned)destination)) {
            strcpy(err, "Journal must resolve to a different local DOS volume");
            goto failed;
        }
        if (target != drive) {
            strcpy(err, "Select the actual DOS volume instead of a substituted drive");
            goto failed;
        }
        if (d->freedos || d->legacy) {
            /* MS-DOS 6.22 has no Windows volume locks; FreeDOS simulates them.
               Flush before raw access and let each kernel invalidate its cache
               through its absolute-write and reset services. */
            if (!dos_sync(d)) {
                strcpy(err, "Cannot flush DOS volume buffers; no writes made");
                goto failed;
            }
        } else {
            if (!lock_call(d, 0, 0)) {
                strcpy(err, "Cannot obtain exclusive DOS volume lock; no writes made");
                goto failed;
            }
            d->locks = 1;
            if (!dos_sync(d) || !lock_call(d, 4, 0)) {
                strcpy(err, "Cannot obtain DOS raw-access lock; no writes made");
                goto failed;
            }
            d->locks = 2;
        }
        d->writable = 1;
    }
    if (!dos_read(d, 0, boot) || dt_word(boot + 510) != 0xaa55 || dt_word(boot + 11) != 512) {
        strcpy(err, "Cannot read a 512-byte FAT boot sector through DOS");
        goto failed;
    }
    d->sectors = dt_word(boot + 19);
    if (!d->sectors)
        d->sectors = dt_dword(boot + 32);
    if (!d->sectors) {
        strcpy(err, "DOS volume has an invalid sector count");
        goto failed;
    }
    disk->sectors = d->sectors;
    if (journal) {
        disk->write = dos_write;
        disk->sync = dos_sync;
    }
    return 1;
failed:
    dt_dos_close(disk);
    return 0;
}
#else
int dt_dos_open(DtDisk *disk, unsigned drive, const char *journal, char *err)
{
    (void)disk;
    (void)drive;
    (void)journal;
    strcpy(err, "Mounted-volume access requires the DOS executable");
    return 0;
}

int dt_dos_close(DtDisk *disk)
{
    (void)disk;
    return 1;
}
#endif
