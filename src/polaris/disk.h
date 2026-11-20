/* Polaris - BIOS disks, partitions and file system detection. */
#ifndef DISK_H
#define DISK_H

#include "pl.h"

#define MAX_DISKS 8
#define MAX_PARTS 48

struct bdisk {
    u8   drive;           /* BIOS number */
    u8   edd;             /* INT 13h extensions usable */
    u8   heads, spt;
    u16  cyls;
    u8   ftype;           /* floppy drive type from AH=08h (BL) */
    u8   pad;
    u32  sectors;
};

struct part {
    u8   disk;            /* index in disks[] */
    u8   index;           /* 1-4 primary, 5.. logical */
    u8   type;
    u8   active;
    u32  lba;
    u32  sectors;
    u8   icon;            /* PI_* guess */
    u8   bootable;        /* boot sector carries 55AAh */
    char fs[8];           /* "FAT16", "NTFS", "ext4" ... */
    char label[12];
    char os[18];          /* "MS-DOS", "Windows NT" ... */
};

extern struct bdisk disks[MAX_DISKS];
extern u8 ndisks;
extern struct part parts[MAX_PARTS];
extern u8 nparts;
extern u8 nfloppies;
extern u8 secbuf[512];           /* general-purpose sector buffer */

int  disk_geometry(u8 drive, struct bdisk *d);
struct bdisk *disk_find(u8 drive);
int  disk_rw(u8 drive, u32 lba, void FAR *buf, u8 write);
#define disk_read(d, l, b)  disk_rw((d), (l), (b), 0)
#define disk_write(d, l, b) disk_rw((d), (l), (b), 1)
void disk_scan(void);
void part_probe(struct part *p, u8 drive);
const char *ptype_name(u8 type);
u8   ptype_hideable(u8 type);
u8   ptype_extended(u8 type);
u8   ptype_icon(u8 type);

#endif
