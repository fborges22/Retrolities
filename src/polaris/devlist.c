/* Polaris - list of everything that can be booted, and entry descriptions. */
#include "ui.h"
#include "config.h"

struct devitem devitems[MAX_DEVITEMS];

void hexbyte(char *d, u8 v)
{
    pl_hex(d, v, 2);
    d[2] = 'h';
    d[3] = 0;
}

u8 devlist_build(u8 specials)
{
    u8 n = 0, i, j;
    for (i = 0; i < ndisks; i++) {
        devitems[n].type = DV_DISK; devitems[n++].idx = i;
        for (j = 0; j < nparts; j++)
            if (parts[j].disk == i && !ptype_extended(parts[j].type)) {
                devitems[n].type = DV_PART; devitems[n++].idx = j;
            }
    }
    for (i = 0; i < nfloppies; i++) {
        devitems[n].type = DV_FLOPPY; devitems[n++].idx = i;
    }
    for (i = 0; i < natapi; i++) {
        devitems[n].type = DV_ATAPI; devitems[n++].idx = i;
    }
    if (specials) {
        static const u8 sp[] = { PK_BIOSNEXT, PK_REBOOT, PK_POWEROFF, PK_SEPARATOR };
        for (i = 0; i < sizeof sp; i++) {
            devitems[n].type = DV_SPECIAL; devitems[n++].idx = sp[i];
        }
    }
    return n;
}

static void pad_to(char *b, u8 col)
{
    u8 n = (u8)pl_strlen(b);
    while (n < col) b[n++] = ' ';
    b[n] = 0;
}

void devitem_label(struct devitem *it, char *b, u8 width)
{
    char t[24];
    b[0] = 0;
    switch (it->type) {
    case DV_DISK: {
        struct bdisk *d = &disks[it->idx];
        pl_strcat(b, "Hard disk ");
        hexbyte(t, d->drive); pl_strcat(b, t);
        pad_to(b, 18);
        pl_strcat(b, pl_size(t, d->sectors));
        if (d->edd) pl_strcat(b, "  LBA");
        break;
    }
    case DV_PART: {
        struct part *p = &parts[it->idx];
        pl_strcat(b, p->index <= 4 ? "Primary " : "Logical ");
        pl_utoa(t, p->index); pl_strcat(b, t);
        pad_to(b, 11);
        pl_strcat(b, pl_size(t, p->sectors));
        pad_to(b, 20);
        pl_strcat(b, p->fs);
        pad_to(b, 27);
        pl_strcat(b, p->os[0] ? p->os : p->label);
        break;
    }
    case DV_FLOPPY:
        pl_strcat(b, it->idx ? "Floppy drive B:" : "Floppy drive A:");
        break;
    case DV_ATAPI: {
        struct atadev *a = &atapi[it->idx];
        pl_strcat(b, atapi_kind(a));
        pad_to(b, 18);
        pl_strcat(b, a->model);
        break;
    }
    case DV_SPECIAL:
        pl_strcat(b, kind_name(it->idx));
        break;
    }
    b[width] = 0;
}

void devitem_entry(struct devitem *it, struct pl_entry *e)
{
    switch (it->type) {
    case DV_DISK:    entry_simple(e, PK_DISK, disks[it->idx].drive); break;
    case DV_PART:    entry_from_part(e, &parts[it->idx]); break;
    case DV_FLOPPY:  entry_simple(e, PK_FLOPPY, it->idx); break;
    case DV_ATAPI:   entry_from_atapi(e, &atapi[it->idx]); break;
    default:         entry_simple(e, it->idx, 0); break;
    }
}

/* "Disk 80h, partition 2" */
void entry_location(struct pl_entry *e, char *b)
{
    char t[12];
    b[0] = 0;
    switch (e->kind) {
    case PK_PART:
        pl_strcat(b, "Disk "); hexbyte(t, e->drive); pl_strcat(b, t);
        pl_strcat(b, e->pindex <= 4 ? " \xF9 P" : " \xF9 L");
        pl_utoa(t, e->pindex); pl_strcat(b, t);
        break;
    case PK_DISK:
        pl_strcat(b, "Disk "); hexbyte(t, e->drive); pl_strcat(b, t);
        pl_strcat(b, " \xF9 MBR");
        break;
    case PK_FLOPPY:
        pl_strcat(b, e->drive ? "Drive B:" : "Drive A:");
        break;
    case PK_CDROM:
    case PK_ATAPIDISK:
        pl_strcat(b, "IDE ");
        pl_hex(t, (u16)e->lba, 3); pl_strcat(b, t);
        pl_strcat(b, e->drive ? "h slave" : "h master");
        break;
    default:
        pl_strcat(b, "Firmware");
        break;
    }
}

/* "FAT16 \xF9 2.0 GB" or emulation for ATAPI */
void entry_detail(struct pl_entry *e, char *b)
{
    static const char *const emu[] = { "Auto emulation", "No emulation", "Floppy emulation",
                                       "Hard disk emul." };
    char t[12];
    b[0] = 0;
    if (e->kind == PK_PART) {
        pl_strcat(b, ptype_name(e->ptype));
        pl_strcat(b, " \xF9 ");
        pl_strcat(b, pl_size(t, e->sectors));
    } else if (e->kind == PK_CDROM || e->kind == PK_ATAPIDISK) {
        pl_strcat(b, emu[e->emu & 3]);
    } else {
        pl_strcat(b, kind_name(e->kind));
    }
}
