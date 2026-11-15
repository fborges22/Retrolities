/* BIOS transfers use DPMI-owned conventional memory, never protected pointers. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>
#ifdef __WATCOMC__
#include <i86.h>
#include <dos.h>
#include <conio.h>
#pragma pack(push, 1)

typedef struct {
    dt_u32 edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} RealRegs;

#pragma pack(pop)

typedef struct {
    unsigned drive, segment, selector, buffer, heads, spt, cylinders, edd;
    dt_u32 sectors;
    dt_u32 write_first, write_count;
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

static int bios_transfer(void *ctx, dt_u32 lba, unsigned char *data, int write)
{
    BiosDisk *b = (BiosDisk *)ctx;
    RealRegs r;
    unsigned attempt;
    unsigned char packet[16];
    dt_u32 cyl, track;
    if (lba >= b->sectors)
        return 0;
    if (write &&
        (!b->write_count || lba < b->write_first || lba - b->write_first >= b->write_count))
        return 0;
    /* A failed write may already have changed media. Leave retry/rollback to
       the journal layer; only reads get the existing three-attempt policy. */
    for (attempt = 0; attempt < (write ? 1U : 3U); attempt++) {
        if (write)
            _fmemcpy(MK_FP(b->selector, b->buffer), data, 512);
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
            r.eax = write ? 0x4300 : 0x4200;
            r.ds = (unsigned short)b->segment;
        } else {
            track = lba / b->spt;
            cyl = track / b->heads;
            if (cyl > 1023)
                return 0;
            r.eax = write ? 0x0301 : 0x0201;
            r.ebx = b->buffer;
            r.es = (unsigned short)b->segment;
            r.ecx = ((cyl & 255) << 8) | ((cyl >> 2) & 192) | (lba % b->spt + 1);
            r.edx |= (track % b->heads) << 8;
        }
        if (interrupt13(&r)) {
            if (!write)
                _fmemcpy(data, MK_FP(b->selector, b->buffer), 512);
            return 1;
        }
        memset(&r, 0, sizeof(r));
        r.edx = b->drive;
        interrupt13(&r);
    }
    return 0;
}

static int bios_read(void *ctx, dt_u32 lba, unsigned char *out)
{
    return bios_transfer(ctx, lba, out, 0);
}

/* A normal BIOS open leaves the write range empty and installs no writer. */
static int bios_write(void *ctx, dt_u32 lba, const unsigned char *data)
{
    return bios_transfer(ctx, lba, (unsigned char *)data, 1);
}

/* Direct ATA commands are restricted to a checksummed EDD mapping of the
   two legacy IDE channels. Never guess a port from the BIOS drive number. */
static int ata_mapping(BiosDisk *b, unsigned *base, unsigned *control, unsigned *head)
{
    RealRegs rm;
    union REGS r;
    unsigned char params[30], dpte[16];
    unsigned selector, offset, segment, i, sum = 0;
    int ok = 0;

    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4100;
    rm.ebx = 0x55aa;
    rm.edx = b->drive;
    if (b->drive < 128 || !interrupt13(&rm) || (rm.ebx & 0xffff) != 0xaa55 || !(rm.ecx & 4))
        return 0;
    memset(params, 0, sizeof(params));
    put16(params, sizeof(params));
    _fmemcpy(MK_FP(b->selector, 0), params, sizeof(params));
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4800;
    rm.edx = b->drive;
    rm.ds = (unsigned short)b->segment;
    if (!interrupt13(&rm))
        return 0;
    _fmemcpy(params, MK_FP(b->selector, 0), sizeof(params));
    if (dt_word(params) < sizeof(params) || dt_dword(params + 26) == 0xffffffffUL)
        return 0;
    offset = dt_word(params + 26);
    segment = dt_word(params + 28);
    if (offset > 0xfff0 || (dt_u32)segment * 16 + offset < 0x400)
        return 0;
    memset(&r, 0, sizeof(r));
    r.w.cx = 1;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        return 0;
    selector = r.w.ax;
    memset(&r, 0, sizeof(r));
    r.w.ax = 7;
    r.w.bx = (unsigned short)selector;
    r.w.cx = (unsigned short)(((dt_u32)segment * 16) >> 16);
    r.w.dx = (unsigned short)(segment * 16);
    int386(0x31, &r, &r);
    if (r.x.cflag)
        goto done;
    memset(&r, 0, sizeof(r));
    r.w.ax = 8;
    r.w.bx = (unsigned short)selector;
    r.w.dx = 0xffff;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        goto done;
    _fmemcpy(dpte, MK_FP(selector, offset), sizeof(dpte));
    for (i = 0; i < sizeof(dpte); i++)
        sum += dpte[i];
    if ((sum & 255) || dpte[14] != 0x11 || (dpte[4] & 0xaf) != 0xa0 || dt_word(dpte + 12) ||
        (dt_word(dpte + 10) & 0xf860))
        goto done;
    *base = dt_word(dpte);
    *control = dt_word(dpte + 2);
    *head = dpte[4];
    ok = (*base == 0x1f0 && *control == 0x3f6) || (*base == 0x170 && *control == 0x376);
done:
    memset(&r, 0, sizeof(r));
    r.w.ax = 1;
    r.w.bx = (unsigned short)selector;
    int386(0x31, &r, &r);
    return ok && !r.x.cflag;
}

static int ata_ticks(dt_u32 *ticks)
{
    union REGS r;
    struct SREGS s;
    RealRegs rm;

    memset(&rm, 0, sizeof(rm));
    memset(&r, 0, sizeof(r));
    segread(&s);
    r.w.ax = 0x0300;
    r.h.bl = 0x1a;
    r.x.edi = (unsigned long)&rm;
    s.es = s.ds;
    int386x(0x31, &r, &r, &s);
    if (r.x.cflag || (rm.flags & 1))
        return 0;
    *ticks = ((rm.ecx & 0xffff) << 16) | (rm.edx & 0xffff);
    return *ticks < 0x1800b0UL;
}

static int ata_wait(unsigned control, int data)
{
    dt_u32 start, now, elapsed;
    unsigned status, polls = 0;

    if (!ata_ticks(&start))
        return 0;
    for (;;) {
        status = inp(control);
        if (!status || status == 255)
            return 0;
        if (!(status & 0x80)) {
            if (status & 0x21)
                return 0;
            if ((status & 8) == (data ? 8U : 0U))
                return 1;
            if (!data)
                return 0;
        }
        /* A stopped BIOS timer must not turn a failed device into a hang. */
        if (polls == 0x1000000UL)
            return 0;
        if (!(++polls & 255)) {
            if (!ata_ticks(&now))
                return 0;
            elapsed = now >= start ? now - start : now + 0x1800b0UL - start;
            if (elapsed >= 600)
                return 0;
        }
    }
}

static void ata_delay(unsigned control)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        inp(control);
}

/* Hardware barrier. IDENTIFY must advertise FLUSH CACHE before E7h
   is issued. Failure is propagated to the journal engine; never fall back to
   a BIOS reset or treat read-back alone as a successful flush. */
static int bios_sync(void *ctx)
{
    BiosDisk *b = (BiosDisk *)ctx;
    unsigned base, control, head, saved, i;
    unsigned short identify[256];
    dt_u64 sectors;
    int ok = 0;

    if (!ata_mapping(b, &base, &control, &head))
        return 0;
    if (inp(control) & 0x88)
        return 0;
    saved = inp(base + 6);
    outp(base + 6, head);
    ata_delay(control);
    if (inp(control) & 0x88)
        goto done;
    outp(base + 7, 0xec);
    ata_delay(control);
    if (!ata_wait(control, 1))
        goto done;
    for (i = 0; i < 256; i++)
        identify[i] = (unsigned short)inpw(base);
    if (!ata_wait(control, 0) || (identify[0] & 0x8000) || (identify[83] & 0xc000) != 0x4000 ||
        !(identify[83] & 0x1000))
        goto done;
    sectors = identify[60] | ((dt_u32)identify[61] << 16);
    if (identify[83] & 0x400) {
        sectors = identify[100] | ((dt_u64)identify[101] << 16) | ((dt_u64)identify[102] << 32) |
                  ((dt_u64)identify[103] << 48);
    }
    if (sectors != b->sectors)
        goto done;
    outp(base + 7, 0xe7);
    ata_delay(control);
    ok = ata_wait(control, 0);
done:
    /* Do not touch device selection while an uncertain command is still busy. */
    if (!(inp(control) & 0x88)) {
        inp(base + 7);
        outp(base + 6, saved);
        ata_delay(control);
    }
    return ok;
}

static int bios_journal_sync(void *ctx, const char *path)
{
    BiosDisk *source = (BiosDisk *)ctx;
    DtDisk journal;
    unsigned drive;
    char err[160];
    int ok;

    if (!dt_dos_bios_write_check(source->drive, path, err) || !dt_dos_bios_path(path, &drive) ||
        drive == source->drive || !dt_dos_flush_buffers() || !dt_bios_open(&journal, drive, err))
        return 0;
    ok = bios_sync(journal.ctx);
    dt_bios_close(&journal);
    return ok;
}

int dt_bios_enable_writes(DtDisk *disk, dt_u32 first, dt_u32 count, const char *journal, char *err)
{
    BiosDisk *b = (BiosDisk *)disk->ctx;

    if (!b || disk->read != bios_read || disk->write || !journal || !*journal || !first || !count ||
        first >= b->sectors || count > b->sectors - first) {
        strcpy(err, "BIOS writes require a bounded, nonzero partition range");
        return 0;
    }
    if (!dt_dos_bios_write_check(b->drive, journal, err))
        return 0;
    if (!dt_dos_flush_buffers() || !bios_sync(b)) {
        strcpy(err, "Cannot establish ATA flush support for this BIOS disk; no writes made");
        return 0;
    }
    b->write_first = first;
    b->write_count = count;
    disk->write = bios_write;
    disk->sync = bios_sync;
    disk->sync_journal = bios_journal_sync;
    return 1;
}

#ifdef MAGICDISK_BIOS_TEST_WRITES
int dt_bios_test_writes(DtDisk *disk, dt_u32 first, dt_u32 count, int force_chs)
{
    BiosDisk *b = (BiosDisk *)disk->ctx;
    RealRegs rm;

    if (!b || disk->read != bios_read || !first || !count || first >= b->sectors ||
        count > b->sectors - first)
        return 0;
    if (force_chs) {
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x0800;
        rm.edx = b->drive;
        if (!interrupt13(&rm))
            return 0;
        b->spt = rm.ecx & 63;
        b->heads = ((rm.edx >> 8) & 255) + 1;
        b->cylinders = (((rm.ecx >> 8) & 255) | ((rm.ecx & 192) << 2)) + 1;
        if (!b->spt || !b->heads ||
            (dt_u64)first + count > (dt_u64)b->spt * b->heads * b->cylinders)
            return 0;
        b->edd = 0;
    }
    b->write_first = first;
    b->write_count = count;
    disk->write = bios_write;
    disk->sync = bios_sync;
    disk->sync_journal = bios_journal_sync;
    return 1;
}
#endif

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
    BiosDisk *b = (BiosDisk *)ctx;

    return dt_dos_bios_destination(b->drive, path);
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
int dt_bios_enable_writes(DtDisk *disk, dt_u32 first, dt_u32 count, const char *journal, char *err)
{
    (void)disk;
    (void)first;
    (void)count;
    (void)journal;
    strcpy(err, "BIOS writes require the DOS executable");
    return 0;
}

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
