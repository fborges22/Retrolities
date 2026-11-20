/* Polaris - starting the selected entry.
 *
 * Partitions are started BootIt-style: primaries can be hidden, unhidden and
 * activated on the fly, the disk can be swapped to 80h and the BPB patched in
 * memory.  CD/DVD and ATAPI removable drives are started through the
 * resident INT 13h module, which turns them into a BIOS drive even on
 * machines whose BIOS cannot boot from them.
 */
#include "polaris.h"
#include "disk.h"
#include "atapi.h"
#include "config.h"
#include "tui.h"
#include "boot.h"

extern u8 resident_bin[], resident_end[];

/* offsets in the resident parameter block (boot/resident.asm) */
#define RP_OLD13    0x08
#define RP_MAP      0x0C
#define RP_MAPCOUNT 0x14
#define RP_VDRIVE   0x15
#define RP_MODE     0x16
#define RP_UNITSEL  0x17
#define RP_IOBASE   0x18
#define RP_CTLBASE  0x1A
#define RP_BLKSIZE  0x1C
#define RP_SHIFT    0x1E
#define RP_HDADJ    0x1F
#define RP_START    0x20
#define RP_SECTORS  0x24
#define RP_CYLS     0x28
#define RP_HEADS    0x2A
#define RP_SPT      0x2B
#define RP_FTYPE    0x2C
#define RP_MEDIA    0x2D
#define RP_LOADSEG  0x2E
#define RP_LOADCNT  0x30
#define RP_CATLBA   0x32
#define RP_CACHED   0x36
#define RP_MEMKB    0x3A

static u8 FAR *res;              /* installed resident module, or NULL */
static u16 res_kb;

#define RB(o) (*(u8 FAR *)(res + (o)))
#define RW(o) (*(u16 FAR *)(res + (o)))
#define RD(o) (*(u32 FAR *)(res + (o)))

static u8 FAR *ivt13(void) { return MKFP(0, 0x13 * 4); }

/* copy the module below the top of conventional memory and hook INT 13h */
static int res_install(void)
{
    u16 FAR *memsize = MKFP(0x40, 0x13);
    u16 len = (u16)(resident_end - resident_bin), seg;
    if (res) return 0;
    res_kb = (u16)((len + 2048 + 1023) / 1024);
    if (*memsize < 256) return -1;
    *memsize -= res_kb;
    seg = *memsize * 64;
    res = MKFP(seg, 0);
    pl_fmemcpy(res, (void FAR *)resident_bin, len);
    RD(RP_OLD13) = *(u32 FAR *)ivt13();
    RW(RP_MEMKB) = res_kb;
    RD(RP_CACHED) = 0xFFFFFFFFUL;
    cli();
    *(u32 FAR *)ivt13() = (u32)seg << 16;
    sti();
    return 0;
}

static void res_remove(void)
{
    u16 FAR *memsize = MKFP(0x40, 0x13);
    if (!res) return;
    if (*(u32 FAR *)ivt13() == (u32)FPSEG(res) << 16) {
        cli();
        *(u32 FAR *)ivt13() = RD(RP_OLD13);
        sti();
        *memsize += res_kb;
    }
    res = NULL;
}

static void res_map(u8 from, u8 to)
{
    u8 n = RB(RP_MAPCOUNT);
    if (n >= 4) return;
    RB(RP_MAP + n * 2) = from;
    RB(RP_MAP + n * 2 + 1) = to;
    RB(RP_MAPCOUNT) = (u8)(n + 1);
}

/* --- helpers ------------------------------------------------------------ */

static void fail(const char *text)
{
    res_remove();
    tui_msg("Cannot start", text, "OK", MT_ERROR);
}

static void splash(const char *what, const char *name, u32 num, u32 den)
{
    char line[64];
    pl_strncpy(line, what, sizeof line);
    pl_strcat(line, name);
    tui_progress("Starting", line, num, den);
}

static u8 present_drive(struct pl_entry *e)
{
    if ((e->flags & PF_SWAP) && e->vdrive != e->drive) {
        if (res_install()) return e->drive;
        res_map(e->vdrive, e->drive);
        res_map(e->drive, e->vdrive);
        return e->vdrive;
    }
    return e->drive;
}

static void finish(u8 drive, u16 si, u16 seg, u16 off)
{
    boot_before_jump();
    tui_done();
    pl_boot_jump(drive, si, seg, off);
}

/* read via INT 13h (possibly through the resident module) to 0000:7C00 */
static int load_sector0(u8 drive)
{
    u8 t;
    for (t = 0; t < 4; t++) {
        rmregs r;
        r.a.w = 0x0201; r.c.w = 0x0001; r.d.w = drive;
        r.es = 0; r.b.w = 0x7C00;
        pl_intr(0x13, &r);
        if (!(r.flags & CF_FLAG)) return 0;
        r.a.w = 0; r.d.w = drive;
        pl_intr(0x13, &r);
    }
    return -1;
}

static int has_sig(void)
{
    u8 FAR *b = MKFP(0, 0x7C00);
    return b[510] == 0x55 && b[511] == 0xAA;
}

/* --- partitions --------------------------------------------------------- */

static int boot_part(struct pl_entry *e)
{
    static u8 mbr[512];
    struct part *p = NULL;
    u8 i, changed = 0, drv;
    u16 si;

    if (!disk_find(e->drive)) { fail("The disk of this entry is not present."); return -1; }
    for (i = 0; i < nparts; i++)
        if (disks[parts[i].disk].drive == e->drive && parts[i].lba == e->lba) { p = &parts[i]; break; }
    if (!p)
        for (i = 0; i < nparts; i++)
            if (disks[parts[i].disk].drive == e->drive && parts[i].index == e->pindex) {
                p = &parts[i]; break;
            }
    if (!p) { fail("The partition of this entry no longer exists.\n"
                   "Edit the entry in Setup (F2) and choose it again."); return -1; }

    if (disk_read(e->drive, 0, mbr)) { fail("Cannot read the partition table."); return -1; }
    for (i = 0; i < 4; i++) {
        u8 *pe = mbr + 0x1BE + i * 16;
        u8 t = pe[4];
        if (!t) continue;
        if (i + 1 == p->index) {
            if ((e->flags & PF_UNHIDE) && ptype_hideable(t) && (t & 0x10)) { pe[4] = t & ~0x10; changed = 1; }
            if ((e->flags & PF_ACTIVE) && pe[0] != 0x80) { pe[0] = 0x80; changed = 1; }
        } else {
            if ((e->flags & PF_ACTIVE) && p->index <= 4 && pe[0]) { pe[0] = 0; changed = 1; }
            if ((e->hidemask & (1 << i)) && ptype_hideable(t) && !(t & 0x10)) {
                pe[4] = t | 0x10; changed = 1;
            }
        }
    }
    if (changed && disk_write(e->drive, 0, mbr)) {
        fail("Cannot update the partition table (write protected?)."); return -1;
    }
    if (p->index <= 4 && (e->flags & PF_UNHIDE)) p->type = mbr[0x1BE + (p->index - 1) * 16 + 4];

    if (disk_read(e->drive, p->lba, secbuf)) { fail("Cannot read the partition boot sector."); return -1; }
    if (!(e->flags & PF_NOCHECK) && (secbuf[510] != 0x55 || secbuf[511] != 0xAA)) {
        fail("The partition has no boot sector (no 55AAh signature).\n"
             "Format it with a system, or enable \"Boot without signature\".");
        return -1;
    }
    drv = present_drive(e);
    if ((e->flags & PF_FIXBPB) && secbuf[11] == 0 && secbuf[12] == 2) {
        u8 fat32 = secbuf[0x52] == 'F' && secbuf[0x53] == 'A' && secbuf[0x56] == '3';
        *(u32 *)(secbuf + 0x1C) = p->lba;
        secbuf[fat32 ? 0x40 : 0x24] = drv;
    }
    pl_fmemcpy(MKFP(0, 0x7C00), (void FAR *)secbuf, 512);

    /* DS:SI -> partition entry in a relocated MBR copy at 0000:0600 */
    pl_fmemcpy(MKFP(0, 0x600), (void FAR *)mbr, 512);
    if (p->index <= 4) {
        si = 0x7BE + (p->index - 1) * 16;
    } else {
        u8 FAR *pe = MKFP(0, 0x7BE);
        si = 0x7BE;
        pe[0] = 0x80; pe[4] = p->type;
        *(u32 FAR *)(pe + 8) = p->lba;
        *(u32 FAR *)(pe + 12) = p->sectors;
    }
    *(u8 FAR *)MKFP(0, si) = 0x80;
    finish(drv, si, 0, 0x7C00);
    return 0;
}

/* --- whole disks and floppies -------------------------------------------- */

static int boot_disk(struct pl_entry *e)
{
    u8 drv;
    if (e->kind == PK_DISK && !disk_find(e->drive)) { fail("This disk is not present."); return -1; }
    for (;;) {
        if (!disk_read(e->drive, 0, secbuf)) break;
        if (e->kind != PK_FLOPPY ||
            tui_msg("Floppy disk", "The drive is empty or the disk is unreadable.\n"
                    "Insert a bootable disk and choose Retry.", "Retry|Cancel", MT_WARN) != 0)
        {
            if (e->kind != PK_FLOPPY) fail("Cannot read the first sector of the disk.");
            return -1;
        }
    }
    if (!(e->flags & PF_NOCHECK) && (secbuf[510] != 0x55 || secbuf[511] != 0xAA)) {
        fail("The disk is not bootable (no 55AAh signature).");
        return -1;
    }
    drv = present_drive(e);
    pl_fmemcpy(MKFP(0, 0x7C00), (void FAR *)secbuf, 512);
    finish(drv, 0x7BE, 0, 0x7C00);
    return 0;
}

/* --- ATAPI devices -------------------------------------------------------- */

static int find_dev(struct pl_entry *e, struct atadev *d)
{
    u16 io = (u16)e->lba, ctl = (u16)(e->lba >> 16);
    if (atapi_probe(io, ctl, e->drive, d)) {
        fail("The ATAPI drive of this entry was not found.");
        return -1;
    }
    if (atapi_capacity(d)) {
        fail(d->type == AT_CDROM ? "There is no disc in the drive, or it is not ready."
                                 : "There is no disk in the drive, or it is not ready.");
        return -1;
    }
    return 0;
}

static void res_device(struct atadev *d, u8 mode, u8 vdrive)
{
    RB(RP_MODE) = mode;
    RB(RP_VDRIVE) = vdrive;
    RB(RP_UNITSEL) = d->slave ? 0xB0 : 0xA0;
    RW(RP_IOBASE) = d->io;
    RW(RP_CTLBASE) = d->ctl;
    RW(RP_BLKSIZE) = d->blksize;
    RB(RP_SHIFT) = d->blksize == 2048 ? 2 : 0;
}

static void res_geometry(u16 cyls, u8 heads, u8 spt)
{
    RW(RP_CYLS) = cyls; RB(RP_HEADS) = heads; RB(RP_SPT) = spt;
}

/* emulate a floppy: real floppies move up one drive number */
static void emulate_floppy(void)
{
    u16 FAR *equip = MKFP(0x40, 0x10);
    if (nfloppies) res_map(0x01, 0x00);
    else *equip = (*equip & ~0xC0) | 1;
}

/* emulate a hard disk as 80h: real disks move up one number */
static void emulate_hdd(void)
{
    u8 i, n = *(u8 FAR *)MKFP(0x40, 0x75);
    for (i = 0; i < n && i < 4; i++) res_map((u8)(0x81 + i), (u8)(0x80 + i));
    RB(RP_HDADJ) = 1;
    *(u8 FAR *)MKFP(0x40, 0x75) = (u8)(n + 1);
}

/* geometry of a hard disk image from its partition table */
static void image_geometry(const u8 *mbr, u32 total, u16 *cyls, u8 *heads, u8 *spt)
{
    u8 i;
    *heads = 64; *spt = 32;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (i = 0; i < 4; i++) {
            const u8 *pe = mbr + 0x1BE + i * 16;
            if (pe[4] && (pe[6] & 0x3F) && pe[5]) {
                *heads = (u8)(pe[5] + 1);
                *spt = pe[6] & 0x3F;
                break;
            }
        }
    *cyls = (u16)(total / ((u32)*heads * *spt));
    if (!*cyls) *cyls = 1;
}

static int boot_cdrom(struct pl_entry *e)
{
    static struct atadev dev;
    struct atadev *d = &dev;
    u32 cat, rba;
    u8 media, *ent;
    u16 loadseg, count;
    u8 i;

    splash("Reading ", e->name, 1, 8);
    if (find_dev(e, d)) return -1;
    splash("El Torito boot record on ", e->name, 2, 8);
    if (atapi_read(d, 17, atabuf) || atabuf[0] != 0 ||
        pl_memcmp(atabuf + 1, "CD001", 5) || pl_memcmp(atabuf + 7, "EL TORITO SPECIFICATION", 23)) {
        fail("This disc is not bootable (no El Torito boot record).");
        return -1;
    }
    cat = *(u32 *)(atabuf + 0x47);
    if (atapi_read(d, cat, atabuf) || atabuf[0] != 1 || atabuf[30] != 0x55 || atabuf[31] != 0xAA) {
        fail("The boot catalog of this disc is damaged.");
        return -1;
    }
    ent = atabuf + 32;                       /* initial/default entry */
    if (ent[0] != 0x88) {                    /* look for a bootable section entry */
        for (i = 2; i < 64; i++) {
            u8 *x = atabuf + i * 32;
            if (x[0] == 0x88 && (atabuf[(i - 1) * 32] == 0x90 || atabuf[(i - 1) * 32] == 0x91 ||
                                 atabuf[(i - 1) * 32] == 0x88)) { ent = x; break; }
        }
        if (ent[0] != 0x88) { fail("The disc has no bootable image."); return -1; }
    }
    media = ent[1] & 0x0F;
    loadseg = *(u16 *)(ent + 2);
    if (!loadseg) loadseg = 0x07C0;
    count = *(u16 *)(ent + 6);
    rba = *(u32 *)(ent + 8);
    if (e->emu == EMU_NONE) media = 0;

    if (res_install()) { fail("Not enough conventional memory."); return -1; }
    res_device(d, 1, e->vdrive ? e->vdrive : 0xE0);
    RD(RP_START) = rba;
    RD(RP_SECTORS) = d->blocks * 4;
    RW(RP_LOADSEG) = loadseg;
    RW(RP_LOADCNT) = count;
    RD(RP_CATLBA) = cat;
    RB(RP_MEDIA) = media;

    if (media == 0) {                       /* no emulation */
        u32 bytes = (u32)(count ? count : 4) * 512, done = 0;
        u8 FAR *dst = MKFP(loadseg, 0);
        if ((u32)loadseg * 16 + bytes > 0x20000UL) { fail("Boot image too large."); return -1; }
        while (done < bytes) {
            u16 n = bytes - done > 2048 ? 2048 : (u16)(bytes - done);
            splash("Loading boot image from ", e->name, 3 + done * 4 / bytes, 8);
            if (atapi_read(d, rba++, atabuf)) { fail("Read error while loading the boot image."); return -1; }
            pl_fmemcpy(MKFP(FPSEG(dst) + (u16)(done >> 4), 0), (void FAR *)atabuf, n);
            done += n;
        }
        splash("Starting ", e->name, 8, 8);
        finish(RB(RP_VDRIVE), 0, loadseg, 0);
        return 0;
    }
    if (media <= 3) {                       /* 1.2, 1.44, 2.88 MB floppy */
        static const u8 spts[4] = {0, 15, 18, 36}, ftypes[4] = {0, 2, 4, 6};
        RB(RP_MODE) = 2;
        RB(RP_VDRIVE) = 0x00;
        res_geometry(80, 2, spts[media]);
        RB(RP_FTYPE) = ftypes[media];
        RD(RP_SECTORS) = 160UL * spts[media];
        emulate_floppy();
    } else {                                /* hard disk image */
        u16 cyls; u8 heads, spt;
        if (atapi_read(d, rba, atabuf)) { fail("Read error on the disc."); return -1; }
        RB(RP_MODE) = 3;
        RB(RP_VDRIVE) = 0x80;
        image_geometry(atabuf, (d->blocks - rba) * 4, &cyls, &heads, &spt);
        res_geometry(cyls, heads, spt);
        RD(RP_SECTORS) = (u32)cyls * heads * spt;
        emulate_hdd();
    }
    splash("Loading emulated disk from ", e->name, 6, 8);
    if (load_sector0(RB(RP_VDRIVE)) || !has_sig()) {
        fail("The emulated disk image is not bootable.");
        return -1;
    }
    splash("Starting ", e->name, 8, 8);
    finish(RB(RP_VDRIVE), 0x7BE, 0, 0x7C00);
    return 0;
}

static int boot_atapidisk(struct pl_entry *e)
{
    static struct atadev dev;
    struct atadev *d = &dev;
    u8 emu = e->emu;

    splash("Reading ", e->name, 1, 4);
    if (find_dev(e, d)) return -1;
    if (d->blksize != 512) { fail("Unsupported block size on this disk."); return -1; }
    if (emu == EMU_AUTO || emu == EMU_NONE) emu = d->blocks <= 5760 ? EMU_FLOPPY : EMU_HDD;
    if (res_install()) { fail("Not enough conventional memory."); return -1; }
    res_device(d, emu == EMU_FLOPPY ? 2 : 3, emu == EMU_FLOPPY ? 0x00 : 0x80);
    RD(RP_START) = 0;
    if (emu == EMU_FLOPPY) {
        u8 spt = d->blocks <= 1440 ? 9 : d->blocks <= 2400 ? 15 : d->blocks <= 2880 ? 18 : 36;
        u16 cyls = (u16)(d->blocks / (2u * spt));
        res_geometry(cyls ? cyls : 80, 2, spt);
        RB(RP_FTYPE) = spt == 9 ? 3 : spt == 15 ? 2 : spt == 18 ? 4 : 6;
        RD(RP_SECTORS) = d->blocks;
        emulate_floppy();
    } else {
        u16 cyls; u8 heads, spt;
        if (atapi_read(d, 0, atabuf)) { fail("Read error on the disk."); return -1; }
        image_geometry(atabuf, d->blocks, &cyls, &heads, &spt);
        res_geometry(cyls, heads, spt);
        RD(RP_SECTORS) = d->blocks;
        emulate_hdd();
    }
    splash("Loading boot sector from ", e->name, 3, 4);
    if (load_sector0(RB(RP_VDRIVE)) || !has_sig()) {
        fail("The disk in this drive is not bootable.");
        return -1;
    }
    finish(RB(RP_VDRIVE), 0x7BE, 0, 0x7C00);
    return 0;
}

/* --- power ----------------------------------------------------------------- */

void boot_reboot(void)
{
    *(u16 FAR *)MKFP(0x40, 0x72) = 0x1234;        /* warm boot */
    tui_done();
    outb(0x64, 0xFE);                             /* keyboard controller reset */
    pl_delay(4);
    pl_boot_jump(0, 0, 0xFFFF, 0x0000);
}

int boot_poweroff(void)
{
    rmregs r;
    r.a.w = 0x5300; r.b.w = 0;
    pl_intr(0x15, &r);
    if (!(r.flags & CF_FLAG) && r.b.w == 0x504D) {
        r.a.w = 0x5304; r.b.w = 0; pl_intr(0x15, &r);
        r.a.w = 0x5301; r.b.w = 0; pl_intr(0x15, &r);
        r.a.w = 0x530E; r.b.w = 0; r.c.w = 0x0102; pl_intr(0x15, &r);
        r.a.w = 0x5308; r.b.w = 1; r.c.w = 1; pl_intr(0x15, &r);
        r.a.w = 0x5307; r.b.w = 1; r.c.w = 3; pl_intr(0x15, &r);
    }
    tui_msg("Power off", "This computer does not offer APM power-off.\n"
            "It is now safe to switch it off by hand.", "OK", MT_WARN);
    return -1;
}

int boot_entry(struct pl_entry *e)
{
    if (e->flags & PF_PASSWORD) {
        u8 t;
        for (t = 0; ; t++) {
            char pw[24];
            pw[0] = 0;
            if (!tui_ask("Protected entry", "Enter the password for this entry:", pw, 20, 1)) return -1;
            if (pl_hash(pw) == e->pw) break;
            if (t == 2) { tui_msg("Access denied", "Wrong password.", "OK", MT_ERROR); return -1; }
        }
    }
    switch (e->kind) {
    case PK_PART:      return boot_part(e);
    case PK_DISK:
    case PK_FLOPPY:    return boot_disk(e);
    case PK_CDROM:     return boot_cdrom(e);
    case PK_ATAPIDISK: return boot_atapidisk(e);
    case PK_BIOSNEXT: {
        rmregs r;
        boot_before_jump();
        tui_done();
        pl_intr(0x18, &r);
        return -1;
    }
    case PK_REBOOT:    boot_reboot(); return -1;
    case PK_POWEROFF:  return boot_poweroff();
    }
    return -1;
}
