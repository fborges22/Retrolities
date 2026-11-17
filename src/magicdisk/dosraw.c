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
    dt_u32 sectors, root_update;
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

/* The first six DDT bytes are shared by MS-DOS and FreeDOS. Do not infer
   physical identity from a BPB, serial number or matching sector contents. */
int dt_dos_bios_map(unsigned drives[26], char *err)
{
    RealRegs rm;
    union REGS r;
    unsigned found[26], selector = 0, count = 0, i;
    dt_u32 visited[26], address;
    unsigned char node[6];
    unsigned segment, offset, major, minor;
    int freedos, ok = 0;

    for (i = 0; i < 26; i++) {
        drives[i] = DT_BIOS_UNKNOWN;
        found[i] = DT_BIOS_UNKNOWN;
    }
    strcpy(err, "DOS physical drive mapping is unavailable");
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3000;
    freedos = real_interrupt(0x21, &rm) && ((rm.ebx >> 8) & 255) == 0xfd;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x3306;
    if (!real_interrupt(0x21, &rm))
        return 0;
    major = rm.ebx & 255;
    minor = (rm.ebx >> 8) & 255;
    if (!freedos && !(major == 6 && minor == 22) && !(major == 7 && minor == 10))
        return 0;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x1600;
    if (!real_interrupt(0x2f, &rm) || (rm.eax & 255))
        return 0;
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x0803;
    if (!real_interrupt(0x2f, &rm))
        return 0;
    segment = rm.ds;
    offset = rm.edi & 0xffff;

    /* Own a temporary selector covering real-mode memory, including the HMA.
       Every address is bounded before reading; no DOS memory is modified. */
    memset(&r, 0, sizeof(r));
    r.w.cx = 1;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        return 0;
    selector = r.w.ax;
    memset(&r, 0, sizeof(r));
    r.w.ax = 7;
    r.w.bx = (unsigned short)selector;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        goto done;
    memset(&r, 0, sizeof(r));
    r.w.ax = 8;
    r.w.bx = (unsigned short)selector;
    r.w.cx = 0x10;
    r.w.dx = 0xffff;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        goto done;
    while (offset != 0xffff) {
        address = (dt_u32)segment * 16 + offset;
        if (count == 26 || address < 0x400 || address > 0x10ffea ||
            (address >= 0x9fffb && address < 0x100000))
            goto done;
        for (i = 0; i < count; i++) {
            if (visited[i] == address)
                goto done;
        }
        visited[count++] = address;
        _fmemcpy(node, MK_FP(selector, address), sizeof(node));
        if (node[5] >= 26 || found[node[5]] != DT_BIOS_UNKNOWN)
            goto done;
        found[node[5]] = node[4];
        offset = dt_word(node);
        segment = dt_word(node + 2);
    }
    if (count) {
        memcpy(drives, found, sizeof(found));
        err[0] = 0;
        ok = 1;
    }
done:
    memset(&r, 0, sizeof(r));
    r.w.ax = 1;
    r.w.bx = (unsigned short)selector;
    int386(0x31, &r, &r);
    return ok;
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
    /* Keep this pending through close: each DPB refresh can reload the old
       root pointer on Windows 98, even after the boot sector has changed. */
    if (d->root_update) {
        unsigned char reset[24];
        unsigned i;

        memset(reset, 0, sizeof(reset));
        reset[0] = sizeof(reset);
        reset[4] = 4;
        for (i = 0; i < 4; i++)
            reset[8 + i] = (unsigned char)(d->root_update >> (i * 8));
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
    if (!real_interrupt(0x21, &rm))
        return 0;
    return 1;
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
    DosDisk *d = (DosDisk *)ctx;
    dt_u32 root = 0;
    unsigned char before[512];
    unsigned i;

    if (!sector && !d->legacy && !d->freedos && !dt_word(data + 17) && !dt_word(data + 22)) {
        if (!transfer(d, 0, before, 0))
            return 0;
        for (i = 0; i < 512; i++) {
            if ((i < 44 || i >= 48) && before[i] != data[i])
                break;
        }
        if (i == 512 && dt_dword(before + 44) != dt_dword(data + 44))
            root = dt_dword(data + 44);
    }
    if (!transfer(d, sector, (unsigned char *)data, 1))
        return 0;
    if (root) {
        d->root_update = root;
    }
    return 1;
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

/* Resolve aliases through DOS before consulting physical identity. Unknown
   devices and remote paths never produce a usable BIOS ID. */
int dt_dos_bios_path(const char *path, unsigned *drive)
{
    DosDisk buffer;
    union REGS r;
    unsigned drives[26];
    char err[160];
    int destination, ok = 0;

    *drive = DT_BIOS_UNKNOWN;
    if (!dt_dos_bios_map(drives, err))
        return 0;
    memset(&buffer, 0, sizeof(buffer));
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = 32;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        return 0;
    buffer.segment = r.w.ax;
    buffer.selector = r.w.dx;
    destination = canonical_drive(&buffer, path);
    if (destination && local_drive((unsigned)destination) &&
        drives[destination - 1] != DT_BIOS_UNKNOWN)
        ok = 1;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = (unsigned short)buffer.selector;
    int386(0x31, &r, &r);
    if (ok && !r.x.cflag) {
        *drive = drives[destination - 1];
        return 1;
    }
    return 0;
}

int dt_dos_bios_destination(unsigned source, const char *path)
{
    unsigned drive;

    return source <= 255 && dt_dos_bios_path(path, &drive) && drive != source;
}

int dt_dos_flush_buffers(void)
{
    RealRegs rm;

    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x0d00;
    return real_interrupt(0x21, &rm);
}

/* An inventory is not enough to prove absence: query every DOS letter too.
   This is a preflight only; it does not lock a disk or install write callbacks. */
int dt_dos_bios_write_check(unsigned source, const char *journal, char *err)
{
    unsigned drives[26], i;
    DosDisk buffer;
    RealRegs rm;
    union REGS r;
    char root[4] = "A:\\";
    int resolved, ok = 0;

    if (source < 128 || source > 255) {
        strcpy(err, "Raw BIOS write preflight requires a hard disk");
        return 0;
    }
    if (!dt_dos_bios_map(drives, err))
        return 0;
    for (i = 0; i < 26; i++) {
        if (drives[i] == source) {
            strcpy(err, "BIOS disk has a mounted DOS volume; use its DOS drive letter");
            return 0;
        }
    }
    if (!dt_dos_bios_destination(source, journal)) {
        strcpy(err, "Journal must resolve to a different known local physical disk");
        return 0;
    }
    /* The EBAB signature avoids the SMARTDRV/DoubleSpace compatibility trap.
       Do not issue cache-reconfiguration or explicit flush subfunctions. */
    memset(&rm, 0, sizeof(rm));
    rm.eax = 0x4a10;
    rm.ecx = 0xebab;
    if (!real_interrupt(0x2f, &rm)) {
        strcpy(err, "Cannot query the DOS disk cache; BIOS writes refused");
        return 0;
    }
    if ((rm.eax & 0xffff) == 0xbabe || (rm.eax & 0xffff) == 0x6756) {
        strcpy(err, "Resident disk cache detected; boot without disk caches for BIOS writes");
        return 0;
    }
    if ((rm.eax & 0xffff) != 0x4a10) {
        strcpy(err, "Unrecognized disk-cache response; BIOS writes refused");
        return 0;
    }
    strcpy(err, "Cannot establish all DOS drive mappings; BIOS writes refused");
    memset(&buffer, 0, sizeof(buffer));
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = 32;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        return 0;
    buffer.segment = r.w.ax;
    buffer.selector = r.w.dx;
    for (i = 0; i < 26; i++) {
        memset(&rm, 0, sizeof(rm));
        rm.eax = 0x4409;
        rm.ebx = i + 1;
        if (!real_interrupt(0x21, &rm)) {
            /* Only DOS's explicit invalid-drive response establishes absence.
               Unsupported IOCTLs and DPMI failures are not an empty drive. */
            if ((rm.flags & 1) && (rm.eax & 0xffff) == 15)
                continue;
            goto done;
        }
        /* Known DDT entries were checked above. Canonicalizing the virtual
           B: alias can prompt for media on a one-floppy MS-DOS machine. */
        if ((rm.edx & 0x1000) || drives[i] != DT_BIOS_UNKNOWN)
            continue;
        root[0] = (char)('A' + i);
        resolved = canonical_drive(&buffer, root);
        if (!resolved || drives[resolved - 1] == DT_BIOS_UNKNOWN || drives[resolved - 1] == source)
            goto done;
    }
    ok = 1;
    err[0] = 0;
done:
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = (unsigned short)buffer.selector;
    int386(0x31, &r, &r);
    if (r.x.cflag) {
        strcpy(err, "Cannot release BIOS preflight memory");
        return 0;
    }
    return ok;
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
int dt_dos_bios_path(const char *path, unsigned *drive)
{
    (void)path;
    *drive = DT_BIOS_UNKNOWN;
    return 0;
}

int dt_dos_flush_buffers(void)
{
    return 0;
}

int dt_dos_bios_write_check(unsigned source, const char *journal, char *err)
{
    (void)source;
    (void)journal;
    strcpy(err, "BIOS write preflight requires the DOS executable");
    return 0;
}

int dt_dos_bios_destination(unsigned source, const char *path)
{
    (void)source;
    (void)path;
    return 0;
}

int dt_dos_bios_map(unsigned drives[26], char *err)
{
    unsigned i;

    for (i = 0; i < 26; i++)
        drives[i] = DT_BIOS_UNKNOWN;
    strcpy(err, "DOS physical drive mapping requires the DOS executable");
    return 0;
}

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
