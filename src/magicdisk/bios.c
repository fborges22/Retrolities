/* BIOS transfers use DPMI-owned conventional memory, never protected pointers. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>
#ifdef __WATCOMC__
#include <i86.h>
#include <dos.h>
#pragma pack(push, 1)

typedef struct {
    dt_u32 edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} RealRegs;

#pragma pack(pop)

typedef struct {
    unsigned drive, segment, selector, buffer, heads, spt, cylinders, edd;
    dt_u32 sectors;
} BiosDisk;

static int interrupt13(RealRegs *rm)
{
    union REGS r;
    struct SREGS s;
    memset(&r, 0, sizeof(r));
    segread(&s);
    r.w.ax = 0x0300;
    r.h.bl = 0x13;
    r.x.edi = (unsigned long)rm;
    s.es = s.ds;
    int386x(0x31, &r, &r, &s);
    return !r.x.cflag && !(rm->flags & 1);
}

static void put16(unsigned char *p, unsigned n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

int dt_bios_devices(unsigned *drives, unsigned *count, char *err)
{
    RealRegs rm;
    unsigned kind, i, available, base;

    *count = 0;
    err[0] = 0;
    for (kind = 0; kind < 2; kind++) {
        base = kind ? 0x80 : 0;
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x0800;
        rm.edx = base;
        /* AH=08 returns the drive count in DL, not the maximum drive ID.
           No media read is needed, so an empty floppy can still be selected. */
        if (!interrupt13(&rm))
            continue;
        available = rm.edx & 255;
        if (available > (kind ? 128 : 4))
            available = kind ? 128 : 4;
        for (i = 0; i < available; i++)
            drives[(*count)++] = base + i;
    }
    return 1;
}

static void put32(unsigned char *p, dt_u32 n)
{
    put16(p, (unsigned)n);
    put16(p + 2, (unsigned)(n >> 16));
}

static int bios_read(void *ctx, dt_u32 lba, unsigned char *out)
{
    BiosDisk *b = (BiosDisk *)ctx;
    RealRegs r;
    unsigned attempt;
    unsigned char packet[16];
    dt_u32 cyl, track;
    if (lba >= b->sectors)
        return 0;
    for (attempt = 0; attempt < 3; attempt++) {
        memset(&r, 0, sizeof(r));
        r.edx = b->drive;
        if (b->edd) {
            memset(packet, 0, sizeof(packet));
            packet[0] = 16;
            put16(packet + 2, 1);
            put16(packet + 4, b->buffer);
            put16(packet + 6, b->segment);
            put32(packet + 8, lba);
            _fmemcpy(MK_FP(b->selector, 0), packet, 16);
            r.eax = 0x4200;
            r.ds = (unsigned short)b->segment;
        } else {
            track = lba / b->spt;
            cyl = track / b->heads;
            if (cyl > 1023)
                return 0;
            r.eax = 0x0201;
            r.ebx = b->buffer;
            r.es = (unsigned short)b->segment;
            r.ecx = ((cyl & 255) << 8) | ((cyl >> 2) & 192) | (lba % b->spt + 1);
            r.edx |= (track % b->heads) << 8;
        }
        if (interrupt13(&r)) {
            _fmemcpy(out, MK_FP(b->selector, b->buffer), 512);
            return 1;
        }
        memset(&r, 0, sizeof(r));
        r.edx = b->drive;
        interrupt13(&r);
    }
    return 0;
}

void dt_bios_close(DtDisk *d)
{
    BiosDisk *b = (BiosDisk *)d->ctx;
    union REGS r;
    if (b) {
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x0101;
        r.w.dx = (unsigned short)b->selector;
        int386(0x31, &r, &r);
        free(b);
    }
    memset(d, 0, sizeof(*d));
}

static int separate_destination(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    /* DOS-to-BIOS mapping is not available in real DOS; select the DOS volume. */
    return 0;
}

int dt_bios_open(DtDisk *d, unsigned drive, char *err)
{
    BiosDisk *b;
    RealRegs rm;
    union REGS r;
    unsigned char params[30], boot[512];
    dt_u64 sectors;
    memset(d, 0, sizeof(*d));
    if (drive > 255) {
        strcpy(err, "Invalid BIOS drive number");
        return 0;
    }
    b = (BiosDisk *)calloc(1, sizeof(*b));
    if (!b) {
        strcpy(err, "Insufficient memory");
        return 0;
    }
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = 128;
    int386(0x31, &r, &r);
    if (r.x.cflag) {
        free(b);
        strcpy(err, "Cannot allocate BIOS transfer buffer");
        return 0;
    }
    b->segment = r.w.ax;
    b->selector = r.w.dx;
    b->drive = drive;
    /* 512-byte alignment prevents floppy DMA transfers crossing 64 KiB. */
    b->buffer = 512 + ((512 - (b->segment * 16UL) % 512) % 512);
    d->ctx = b;
    d->read = bios_read;
    d->separate_destination = separate_destination;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4100;
    rm.ebx = 0x55aa;
    rm.edx = drive;
    if (drive >= 128 && interrupt13(&rm) && (rm.ebx & 0xffff) == 0xaa55 && (rm.ecx & 1)) {
        memset(params, 0, sizeof(params));
        put16(params, sizeof(params));
        _fmemcpy(MK_FP(b->selector, 0), params, sizeof(params));
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x4800;
        rm.edx = drive;
        rm.ds = (unsigned short)b->segment;
        if (!interrupt13(&rm))
            goto unavailable;
        _fmemcpy(params, MK_FP(b->selector, 0), sizeof(params));
        sectors = dt_dword(params + 16) | ((dt_u64)dt_dword(params + 20) << 32);
        if (dt_word(params) < 26 || dt_word(params + 24) != 512 || !sectors ||
            sectors > 0xffffffffUL) {
            strcpy(err, "BIOS disk requires 512-byte sectors and at most 2 TiB");
            dt_bios_close(d);
            return 0;
        }
        b->edd = 1;
        b->sectors = (dt_u32)sectors;
    } else {
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x0800;
        rm.edx = drive;
        if (!interrupt13(&rm))
            goto unavailable;
        b->spt = rm.ecx & 63;
        b->heads = ((rm.edx >> 8) & 255) + 1;
        b->cylinders = ((rm.ecx >> 8) & 255) | ((rm.ecx & 192) << 2);
        b->cylinders++;
        if (!b->spt || !b->heads)
            goto unavailable;
        b->sectors = (dt_u32)b->spt * b->heads * b->cylinders;
    }
    if (!bios_read(b, 0, boot))
        goto unavailable;
    if (drive < 128 && dt_word(boot + 510) == 0xaa55 && dt_word(boot + 11) == 512) {
        unsigned spt = dt_word(boot + 24), heads = dt_word(boot + 26);
        dt_u32 total = dt_word(boot + 19);
        if (!total)
            total = dt_dword(boot + 32);
        /* The media BPB may describe a smaller floppy than the drive maximum. */
        if (spt && spt <= b->spt && heads && heads <= b->heads && total &&
            total <= (dt_u32)spt * heads * b->cylinders) {
            b->spt = spt;
            b->heads = heads;
            b->sectors = total;
        }
    }
    d->sectors = b->sectors;
    return 1;
unavailable:
    strcpy(err, "BIOS drive unavailable or media unreadable");
    dt_bios_close(d);
    return 0;
}
#else
int dt_bios_devices(unsigned *drives, unsigned *count, char *err)
{
    (void)drives;
    *count = 0;
    strcpy(err, "BIOS access requires the DOS executable");
    return 0;
}

int dt_bios_open(DtDisk *d, unsigned drive, char *err)
{
    (void)d;
    (void)drive;
    strcpy(err, "BIOS access requires the DOS executable");
    return 0;
}

void dt_bios_close(DtDisk *d)
{
    (void)d;
}
#endif
