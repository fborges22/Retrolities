/* Polaris - IDE/ATAPI device discovery and PIO packet commands. */
#ifndef ATAPI_H
#define ATAPI_H

#include "pl.h"

#define MAX_ATAPI 8

enum { AT_CDROM = 5, AT_DIRECT = 0, AT_OPTICAL = 7 };

struct atadev {
    u16  io, ctl;
    u8   slave;
    u8   type;            /* SCSI peripheral type from IDENTIFY PACKET */
    u8   removable;
    u8   media;           /* READ CAPACITY succeeded */
    u16  blksize;
    u32  blocks;
    char model[41];
    u8   pci;             /* found through a PCI native-mode controller */
};

extern struct atadev atapi[MAX_ATAPI];
extern u8 natapi;
extern u8 pci_present;
extern u8 atabuf[2048];

void atapi_scan(u8 media);     /* media: also check for a disc (slow) */
int  atapi_probe(u16 io, u16 ctl, u8 slave, struct atadev *d);
int  atapi_packet(struct atadev *d, const u8 *pkt, u8 *buf, u16 len);
int  atapi_read(struct atadev *d, u32 lba, u8 *buf);
int  atapi_capacity(struct atadev *d);
const char *atapi_kind(struct atadev *d);
u8   atapi_icon(struct atadev *d);

#endif
