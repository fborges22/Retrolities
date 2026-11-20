/* Polaris - IDE/ATAPI driver used by the menu to find and inspect CD-ROM,
 * DVD, ZIP, LS-120, MO and PD drives without help from the BIOS.  The
 * resident INT 13h module (boot/resident.asm) has its own copy of the read
 * path for the operating system that is booted. */
#include "atapi.h"
#include "polaris.h"

struct atadev atapi[MAX_ATAPI];
u8 natapi;
u8 pci_present;
u8 atabuf[2048];

#define ST_BSY 0x80
#define ST_DRQ 0x08
#define ST_ERR 0x01

static void delay400(u16 ctl)
{
    inb(ctl); inb(ctl); inb(ctl); inb(ctl);
}

/* wait for BSY to clear; returns status or -1 on timeout */
static int wait_bsy(struct atadev *d, u16 ticks)
{
    u32 end = pl_ticks() + ticks;
    for (;;) {
        u8 s = inb(d->ctl);
        if (s == 0xFF) return -1;
        if (!(s & ST_BSY)) return inb(d->io + 7);
        if (pl_ticks() > end) return -1;
    }
}

static void words_to_str(char *d, const u16 *w, u8 nwords)
{
    u8 i, n = 0;
    for (i = 0; i < nwords; i++) {
        d[n++] = (char)(w[i] >> 8);
        d[n++] = (char)w[i];
    }
    while (n && (d[n - 1] == ' ' || d[n - 1] == 0)) n--;
    d[n] = 0;
    {   /* collapse leading blanks */
        u8 k = 0;
        while (d[k] == ' ') k++;
        if (k) pl_memcpy(d, d + k, n - k + 1);
    }
}

static int probe(u16 io, u16 ctl, u8 slave, struct atadev *d)
{
    u16 *id = (u16 *)atabuf;
    int s;
    pl_memset(d, 0, sizeof *d);
    d->io = io; d->ctl = ctl; d->slave = slave;
    outb(ctl, 0x02);                         /* nIEN: polling only */
    outb(io + 6, slave ? 0xB0 : 0xA0);
    delay400(ctl);
    s = inb(io + 7);
    if (s == 0xFF || s == 0x7F) return -1;   /* floating bus */
    outb(io + 7, 0xA1);                      /* IDENTIFY PACKET DEVICE */
    delay400(ctl);
    s = wait_bsy(d, 36);
    if (s < 0 || (s & ST_ERR) || !(s & ST_DRQ)) return -1;
    insw_buf(io, id, 256);
    if ((id[0] & 0xC000) != 0x8000) return -1;
    d->type = (u8)((id[0] >> 8) & 0x1F);
    d->removable = (id[0] & 0x80) != 0;
    words_to_str(d->model, id + 27, 20);
    d->blksize = d->type == AT_CDROM ? 2048 : 512;
    return 0;
}

static int packet(struct atadev *d, const u8 *pkt, u8 *buf, u16 len)
{
    int s;
    u16 got = 0;
    outb(d->ctl, 0x02);
    if (wait_bsy(d, 182) < 0) return -1;
    outb(d->io + 6, d->slave ? 0xB0 : 0xA0);
    delay400(d->ctl);
    if (wait_bsy(d, 182) < 0) return -1;
    outb(d->io + 1, 0);
    outb(d->io + 4, (u8)(len ? len : 2048));
    outb(d->io + 5, (u8)((len ? len : 2048) >> 8));
    outb(d->io + 7, 0xA0);
    delay400(d->ctl);
    s = wait_bsy(d, 182);
    if (s < 0 || (s & ST_ERR) || !(s & ST_DRQ)) return -1;
    outsw_buf(d->io, pkt, 6);
    for (;;) {
        u16 n, i;
        delay400(d->ctl);
        s = wait_bsy(d, 182);
        if (s < 0 || (s & ST_ERR)) return -1;
        if (!(s & ST_DRQ)) break;
        n = inb(d->io + 4) | (u16)inb(d->io + 5) << 8;
        for (i = 0; i < n / 2; i++) {
            u16 w = inw(d->io);
            if (got + 2 <= len) {
                buf[got] = (u8)w;
                buf[got + 1] = (u8)(w >> 8);
                got += 2;
            }
        }
    }
    return got;
}

/* Interrupts are masked (nIEN) only while Polaris polls the device: many
 * BIOSes need IRQ 14/15 for their own disk services afterwards.  Reading the
 * status register first clears a pending INTRQ. */
int atapi_probe(u16 io, u16 ctl, u8 slave, struct atadev *d)
{
    int r = probe(io, ctl, slave, d);
    if (inb(io + 7) != 0xFF) outb(ctl, 0x00);
    return r;
}

int atapi_packet(struct atadev *d, const u8 *pkt, u8 *buf, u16 len)
{
    int r = packet(d, pkt, buf, len);
    inb(d->io + 7);
    outb(d->ctl, 0x00);
    return r;
}

int atapi_read(struct atadev *d, u32 lba, u8 *buf)
{
    u8 pkt[12];
    u8 t;
    pl_memset(pkt, 0, 12);
    pkt[0] = 0x28;
    pkt[2] = (u8)(lba >> 24); pkt[3] = (u8)(lba >> 16);
    pkt[4] = (u8)(lba >> 8);  pkt[5] = (u8)lba;
    pkt[8] = 1;
    for (t = 0; t < 4; t++)
        if (atapi_packet(d, pkt, buf, d->blksize) == (int)d->blksize) return 0;
    return -1;
}

int atapi_capacity(struct atadev *d)
{
    u8 pkt[12], t;
    pl_memset(pkt, 0, 12);
    d->media = 0;
    for (t = 0; t < 4; t++) {                  /* clears UNIT ATTENTION */
        pkt[0] = 0x00;                         /* TEST UNIT READY */
        if (atapi_packet(d, pkt, atabuf, 0) >= 0) break;
    }
    pkt[0] = 0x25;                             /* READ CAPACITY */
    for (t = 0; t < 3; t++)
        if (atapi_packet(d, pkt, atabuf, 8) == 8) {
            u32 bl = (u32)atabuf[4] << 24 | (u32)atabuf[5] << 16 | (u16)atabuf[6] << 8 | atabuf[7];
            d->blocks = ((u32)atabuf[0] << 24 | (u32)atabuf[1] << 16 |
                         (u16)atabuf[2] << 8 | atabuf[3]) + 1;
            if (bl == 512 || bl == 2048) d->blksize = (u16)bl;
            d->media = 1;
            return 0;
        }
    return -1;
}

const char *atapi_kind(struct atadev *d)
{
    if (d->type == AT_CDROM) return "CD/DVD-ROM";
    if (d->type == AT_OPTICAL) return "Optical disk";
    return d->removable ? "Removable disk" : "Packet disk";
}

u8 atapi_icon(struct atadev *d)
{
    return d->type == AT_CDROM ? PI_CDROM : PI_ZIP;
}

/* --- controller discovery ------------------------------------------------ */

static u32 pci_read(u8 bus, u8 dev, u8 fn, u8 reg)
{
    outl(0xCF8, 0x80000000UL | (u32)bus << 16 | (u32)dev << 11 | (u32)fn << 8 | reg);
    return inl(0xCFC);
}

static u8 nctl;
static u16 ctl_io[8], ctl_ctl[8];
static u8 ctl_pci[8];

static void add_ctl(u16 io, u16 ctl, u8 pci)
{
    u8 i;
    if (!io || nctl >= 8) return;
    for (i = 0; i < nctl; i++) if (ctl_io[i] == io) return;
    ctl_io[nctl] = io; ctl_ctl[nctl] = ctl; ctl_pci[nctl] = pci; nctl++;
}

static void pci_scan(void)
{
    u8 bus, dev, fn;
    outl(0xCF8, 0x80000000UL);
    if (inl(0xCF8) != 0x80000000UL) return;
    pci_present = 1;
    for (bus = 0; bus < 4; bus++)
        for (dev = 0; dev < 32; dev++)
            for (fn = 0; fn < 8; fn++) {
                u32 id = pci_read(bus, dev, fn, 0), cls, hdr;
                if ((u16)id == 0xFFFF) { if (!fn) break; continue; }
                cls = pci_read(bus, dev, fn, 8);
                hdr = pci_read(bus, dev, fn, 0x0C);
                if ((cls >> 16) == 0x0101) {
                    u8 pif = (u8)(cls >> 8);
                    if (pif & 0x01)
                        add_ctl((u16)(pci_read(bus, dev, fn, 0x10) & 0xFFFC),
                                (u16)((pci_read(bus, dev, fn, 0x14) & 0xFFFC) + 2), 1);
                    else add_ctl(0x1F0, 0x3F6, 0);
                    if (pif & 0x04)
                        add_ctl((u16)(pci_read(bus, dev, fn, 0x18) & 0xFFFC),
                                (u16)((pci_read(bus, dev, fn, 0x1C) & 0xFFFC) + 2), 1);
                    else add_ctl(0x170, 0x376, 0);
                }
                if (!fn && !(hdr & 0x00800000UL)) break;   /* single function */
            }
}

void atapi_scan(u8 media)
{
    u8 i, s;
    natapi = 0;
    nctl = 0;
    pci_scan();
    add_ctl(0x1F0, 0x3F6, 0);
    add_ctl(0x170, 0x376, 0);
    add_ctl(0x1E8, 0x3EE, 0);
    add_ctl(0x168, 0x36E, 0);
    for (i = 0; i < nctl && natapi < MAX_ATAPI; i++)
        for (s = 0; s < 2 && natapi < MAX_ATAPI; s++)
            if (!atapi_probe(ctl_io[i], ctl_ctl[i], s, &atapi[natapi])) {
                atapi[natapi].pci = ctl_pci[i];
                if (media) atapi_capacity(&atapi[natapi]);
                natapi++;
            }
}
