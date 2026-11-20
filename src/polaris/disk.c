/* Polaris - BIOS disk access, partition tables and file system detection. */
#include "disk.h"
#include "polaris.h"

struct bdisk disks[MAX_DISKS];
u8 ndisks;
struct part parts[MAX_PARTS];
u8 nparts;
u8 nfloppies;
u8 secbuf[512];

static u8 bounce[1024];
static struct bdisk geo_cache[2];   /* floppies A: and B: */

static const struct { u8 type; const char *name; } ptypes[] = {
    {0x01, "FAT12"},          {0x04, "FAT16 <32M"},     {0x05, "Extended"},
    {0x06, "FAT16"},          {0x07, "NTFS/HPFS"},      {0x0A, "OS/2 Boot Mgr"},
    {0x0B, "FAT32"},          {0x0C, "FAT32 LBA"},      {0x0E, "FAT16 LBA"},
    {0x0F, "Extended LBA"},   {0x11, "Hidden FAT12"},   {0x12, "Diagnostics"},
    {0x14, "Hidden FAT16"},   {0x16, "Hidden FAT16"},   {0x17, "Hidden NTFS"},
    {0x1B, "Hidden FAT32"},   {0x1C, "Hidden FAT32"},   {0x1E, "Hidden FAT16"},
    {0x27, "Recovery"},       {0x39, "Plan 9"},         {0x3C, "PQ Recovery"},
    {0x42, "Dynamic disk"},   {0x4D, "QNX"},            {0x63, "Unix SysV"},
    {0x64, "NetWare"},        {0x65, "NetWare"},        {0x80, "Minix"},
    {0x81, "Minix/Linux"},    {0x82, "Linux swap"},     {0x83, "Linux"},
    {0x85, "Linux extended"}, {0x8E, "Linux LVM"},      {0x93, "Amoeba"},
    {0x9F, "BSD/OS"},         {0xA5, "FreeBSD"},        {0xA6, "OpenBSD"},
    {0xA8, "Darwin UFS"},     {0xA9, "NetBSD"},         {0xAB, "Darwin boot"},
    {0xAF, "HFS+"},           {0xBE, "Solaris boot"},   {0xBF, "Solaris"},
    {0xC0, "DR-DOS secured"}, {0xDB, "CP/M"},           {0xDE, "Dell utility"},
    {0xEB, "BeOS"},           {0xEE, "GPT protective"}, {0xEF, "EFI system"},
    {0xFB, "VMware VMFS"},    {0xFD, "Linux RAID"},
};

const char *ptype_name(u8 type)
{
    u8 i;
    for (i = 0; i < sizeof ptypes / sizeof ptypes[0]; i++)
        if (ptypes[i].type == type) return ptypes[i].name;
    return "Unknown";
}

u8 ptype_hideable(u8 t)
{
    t &= ~0x10;
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x07 || t == 0x0B ||
           t == 0x0C || t == 0x0E;
}

u8 ptype_extended(u8 t)
{
    return t == 0x05 || t == 0x0F || t == 0x85;
}

u8 ptype_icon(u8 t)
{
    switch (t) {
    case 0x01: case 0x04: case 0x06: case 0x0E: return PI_DOS;
    case 0x0B: case 0x0C: return PI_WIN9X;
    case 0x07: case 0x27: case 0x42: return PI_WINNT;
    case 0x0A: return PI_OS2;
    case 0x81: case 0x82: case 0x83: case 0x8E: case 0xFD: return PI_LINUX;
    case 0x9F: case 0xA5: case 0xA6: case 0xA9: return PI_BSD;
    }
    return PI_DISK;
}

static int bios13(rmregs *r)
{
    pl_intr(0x13, r);
    return (r->flags & CF_FLAG) ? -1 : 0;
}

int disk_geometry(u8 drive, struct bdisk *d)
{
    rmregs r;
    pl_memset(d, 0, sizeof *d);
    d->drive = drive;
    r.a.w = 0x0800;
    r.d.w = drive;
    r.es = 0; r.di.w = 0; r.b.w = 0;
    if (bios13(&r) || !(r.c.b.l & 0x3F)) {
        if (drive & 0x80) return -1;
        d->spt = 18; d->heads = 2; d->cyls = 80; d->ftype = 4;   /* assume 1.44 */
    } else {
        d->spt = r.c.b.l & 0x3F;
        d->heads = (u8)(r.d.b.h + 1);
        d->cyls = (u16)(((r.c.b.l & 0xC0) << 2 | r.c.b.h) + 1);
        d->ftype = r.b.b.l;
        if (!(drive & 0x80) && r.d.b.l == 0) return -1;         /* no floppy */
    }
    d->sectors = (u32)d->cyls * d->heads * d->spt;
    if (drive & 0x80) {
        r.a.w = 0x4100; r.b.w = 0x55AA; r.d.w = drive;
        if (!bios13(&r) && r.b.w == 0xAA55 && (r.c.w & 1)) {
            static struct { u16 size, flags; u32 cyl, heads, spt, lo, hi; u16 bps; } ep;
            d->edd = 1;
            pl_memset(&ep, 0, sizeof ep);
            ep.size = 26;
            r.a.w = 0x4800; r.d.w = drive;
            r.ds = FPSEG((void FAR *)&ep); r.si.w = (u16)&ep;
            if (!bios13(&r)) {
                if (ep.hi) d->sectors = 0xFFFFFFFFUL;
                else if (ep.lo) d->sectors = ep.lo;
            }
        }
    }
    return 0;
}

struct bdisk *disk_find(u8 drive)
{
    u8 i;
    for (i = 0; i < ndisks; i++)
        if (disks[i].drive == drive) return &disks[i];
    if (drive < 2) {
        struct bdisk *g = &geo_cache[drive];
        if (!g->spt && disk_geometry(drive, g)) {
            g->spt = 18; g->heads = 2; g->cyls = 80;
        }
        g->drive = drive;
        return g;
    }
    return NULL;
}

int disk_rw(u8 drive, u32 lba, void FAR *buf, u8 write)
{
    static struct { u8 size, res; u16 count; u16 off, seg; u32 lo, hi; } dap;
    struct bdisk *d = disk_find(drive);
    struct bdisk tmp;
    void FAR *io = buf;
    u8 try;
    u32 lin = LINEAR(buf);

    if (!d) {
        if (disk_geometry(drive, &tmp)) return -1;
        d = &tmp;
    }
    if ((lin & 0xFFFF) > 0xFE00) {          /* would cross a DMA page */
        u32 b = LINEAR((void FAR *)bounce);
        io = (b & 0xFFFF) > 0xFE00 ? (void FAR *)(bounce + 512) : (void FAR *)bounce;
        if (write) pl_fmemcpy(io, buf, 512);
    }
    for (try = 0; try < 3; try++) {
        rmregs r;
        if (d->edd) {
            dap.size = 16; dap.res = 0; dap.count = 1;
            dap.off = FPOFF(io); dap.seg = FPSEG(io);
            dap.lo = lba; dap.hi = 0;
            r.a.w = write ? 0x4300 : 0x4200;
            r.d.w = drive;
            r.ds = FPSEG((void FAR *)&dap); r.si.w = (u16)&dap;
        } else {
            u32 t;
            u16 cyl, sec, head;
            if (!d->spt || !d->heads) return -1;
            sec = (u16)(lba % d->spt) + 1;
            t = lba / d->spt;
            head = (u16)(t % d->heads);
            cyl = (u16)(t / d->heads);
            if (cyl > 1023) return -1;
            r.a.w = write ? 0x0301 : 0x0201;
            r.c.w = (u16)((cyl & 0xFF) << 8 | ((cyl >> 2) & 0xC0) | sec);
            r.d.w = (u16)(head << 8 | drive);
            r.es = FPSEG(io); r.b.w = FPOFF(io);
        }
        if (!bios13(&r)) {
            if (io != buf && !write) pl_fmemcpy(buf, io, 512);
            return 0;
        }
        r.a.w = 0; r.d.w = drive;
        bios13(&r);
    }
    return -1;
}

static int match(const u8 *p, const char *s)
{
    while (*s) if (*p++ != (u8)*s++) return 0;
    return 1;
}

static int find(const u8 *buf, u16 len, const char *s)
{
    u16 n = pl_strlen(s), i;
    for (i = 0; i + n <= len; i++)
        if (buf[i] == (u8)s[0] && match(buf + i, s)) return 1;
    return 0;
}

static void copy_label(char *d, const u8 *s, u8 n)
{
    u8 i;
    for (i = 0; i < n && s[i] >= 32 && s[i] < 127; i++) d[i] = s[i];
    while (i && d[i - 1] == ' ') i--;
    d[i] = 0;
    if (!pl_strcmp(d, "NO NAME")) d[0] = 0;
}

/* Inspect the boot sector (and superblock) of a partition */
void part_probe(struct part *p, u8 drive)
{
    const u8 *b = secbuf;
    p->icon = ptype_icon(p->type);
    pl_strncpy(p->fs, ptype_name(p->type), sizeof p->fs);
    p->label[0] = 0;
    p->os[0] = 0;
    p->bootable = 0;
    if (ptype_extended(p->type) || !p->sectors) return;
    if (disk_read(drive, p->lba, secbuf)) return;
    p->bootable = secbuf[510] == 0x55 && secbuf[511] == 0xAA;

    if (match(b + 3, "NTFS    ")) {
        pl_strncpy(p->fs, "NTFS", sizeof p->fs);
        p->icon = PI_WINNT;
        pl_strncpy(p->os, find(b, 512, "BOOTMGR") ? "Windows" : "Windows NT", sizeof p->os);
        return;
    }
    if (match(b + 3, "EXFAT   ")) {
        pl_strncpy(p->fs, "exFAT", sizeof p->fs);
        return;
    }
    if (b[11] == 0 && b[12] == 2 && (match(b + 0x36, "FAT1") || match(b + 0x52, "FAT32") ||
                                     match(b + 0x36, "HPFS"))) {
        u8 fat32 = match(b + 0x52, "FAT32");
        if (match(b + 0x36, "HPFS")) {
            pl_strncpy(p->fs, "HPFS", sizeof p->fs);
            p->icon = PI_OS2;
            pl_strncpy(p->os, "OS/2", sizeof p->os);
        } else {
            pl_strncpy(p->fs, fat32 ? "FAT32" : match(b + 0x36, "FAT12") ? "FAT12" : "FAT16",
                       sizeof p->fs);
        }
        if (b[fat32 ? 0x42 : 0x26] == 0x29)
            copy_label(p->label, b + (fat32 ? 0x47 : 0x2B), 11);
        if (find(b, 512, "BOOTMGR")) { p->icon = PI_WINNT; pl_strncpy(p->os, "Windows", 18); }
        else if (find(b, 512, "NTLDR")) { p->icon = PI_WINNT; pl_strncpy(p->os, "Windows NT", 18); }
        else if (find(b, 512, "KERNEL  SYS") || match(b + 3, "FRDOS") || match(b + 3, "FreeDOS"))
            { p->icon = PI_DOS; pl_strncpy(p->os, "FreeDOS", 18); }
        else if (find(b, 512, "IBMBIO  COM")) { p->icon = PI_DOS; pl_strncpy(p->os, "PC DOS", 18); }
        else if (find(b, 512, "OS2BOOT") || find(b, 512, "OS2LDR"))
            { p->icon = PI_OS2; pl_strncpy(p->os, "OS/2", 18); }
        else if (match(b + 3, "MSWIN4")) { p->icon = PI_WIN9X; pl_strncpy(p->os, "Windows 9x", 18); }
        else if (find(b, 512, "IO      SYS") || match(b + 3, "MSDOS"))
            { p->icon = PI_DOS; pl_strncpy(p->os, "MS-DOS", 18); }
        else if (find(b, 512, "SYSLINUX")) { p->icon = PI_LINUX; pl_strncpy(p->os, "SYSLINUX", 18); }
        return;
    }
    if (find(b, 512, "GRUB")) { p->icon = PI_LINUX; pl_strncpy(p->os, "GRUB", 18); }
    else if (find(b, 512, "LILO")) { p->icon = PI_LINUX; pl_strncpy(p->os, "LILO", 18); }
    if (p->type == 0x83 && !disk_read(drive, p->lba + 2, secbuf) &&
        secbuf[56] == 0x53 && secbuf[57] == 0xEF) {
        u32 compat = *(u32 *)(secbuf + 92), incompat = *(u32 *)(secbuf + 96);
        pl_strncpy(p->fs, (incompat & 0x40) ? "ext4" : (compat & 4) ? "ext3" : "ext2",
                   sizeof p->fs);
        copy_label(p->label, secbuf + 120, 11);
        if (!p->os[0]) pl_strncpy(p->os, "Linux", sizeof p->os);
    }
}

static void add_part(u8 di, u8 index, const u8 *e, u32 base)
{
    struct part *p;
    if (nparts >= MAX_PARTS) return;
    p = &parts[nparts++];
    pl_memset(p, 0, sizeof *p);
    p->disk = di;
    p->index = index;
    p->type = e[4];
    p->active = e[0] == 0x80;
    p->lba = base + *(u32 *)(e + 8);
    p->sectors = *(u32 *)(e + 12);
}

static void scan_disk(u8 di)
{
    u8 drive = disks[di].drive, i, logical = 5, guard;
    u8 mbr[64];
    u32 ext = 0, ebr;
    u8 first = nparts;

    if (disk_read(drive, 0, secbuf) || secbuf[510] != 0x55 || secbuf[511] != 0xAA) return;
    pl_memcpy(mbr, secbuf + 0x1BE, 64);
    for (i = 0; i < 4; i++) {
        const u8 *e = mbr + i * 16;
        if (!e[4]) continue;
        add_part(di, (u8)(i + 1), e, 0);
        if (ptype_extended(e[4]) && !ext) ext = *(u32 *)(e + 8);
    }
    ebr = ext;
    for (guard = 0; ext && guard < 32; guard++) {
        u8 next = 0;
        u8 ent[32];
        if (disk_read(drive, ebr, secbuf) || secbuf[510] != 0x55) break;
        pl_memcpy(ent, secbuf + 0x1BE, 32);
        if (ent[4] && !ptype_extended(ent[4])) add_part(di, logical++, ent, ebr);
        if (ptype_extended(ent[16 + 4])) {
            ebr = ext + *(u32 *)(ent + 16 + 8);
            next = 1;
        }
        if (!next) break;
    }
    for (i = first; i < nparts; i++) part_probe(&parts[i], drive);
}

void disk_scan(void)
{
    u8 n, i;
    rmregs r;
    ndisks = 0;
    nparts = 0;
    n = *(u8 FAR *)MKFP(0x40, 0x75);
    if (n > MAX_DISKS) n = MAX_DISKS;
    for (i = 0; i < n; i++) {
        if (disk_geometry((u8)(0x80 + i), &disks[ndisks])) continue;
        ndisks++;
    }
    pl_intr(0x11, &r);
    nfloppies = (r.a.w & 1) ? (u8)(((r.a.w >> 6) & 3) + 1) : 0;
    for (i = 0; i < ndisks; i++) scan_disk(i);
}
