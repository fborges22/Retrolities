/* Polaris - configuration record helpers and automatic menu building. */
#include "polaris.h"
#include "disk.h"
#include "atapi.h"
#include "config.h"

struct pl_config cfg;

static const char *const kinds[PK_COUNT] = {
    "Empty", "Partition", "Disk MBR", "Floppy", "CD/DVD (ATAPI)", "ATAPI disk",
    "Next BIOS device", "Restart", "Power off", "Group heading"
};

const char *kind_name(u8 kind)
{
    return kind < PK_COUNT ? kinds[kind] : "?";
}

u16 cfg_checksum(struct pl_config *c)
{
    const u16 *w = (const u16 *)c;
    u16 i, sum = 0;
    for (i = 0; i < sizeof *c / 2; i++)
        if (&w[i] != &c->checksum) sum += w[i];
    return (u16)(0 - sum);
}

void cfg_seal(struct pl_config *c)
{
    pl_memcpy(c->magic, PL_CFG_MAGIC, 8);
    c->version = PL_CFG_VERSION;
    c->size = sizeof *c;
    c->checksum = cfg_checksum(c);
}

int cfg_valid(struct pl_config *c)
{
    return !pl_memcmp(c->magic, PL_CFG_MAGIC, 8) && c->version == PL_CFG_VERSION &&
           c->size == sizeof *c && c->checksum == cfg_checksum(c) &&
           c->count <= PL_MAX_ENTRIES;
}

void cfg_defaults(struct pl_config *c)
{
    pl_memset(c, 0, sizeof *c);
    c->deflt = 0;
    c->timeout = 10;
    c->theme = 0;
    c->last = PL_NO_ENTRY;
    c->oneshot = PL_NO_ENTRY;
    pl_strncpy(c->title, "Choose an operating system", sizeof c->title);
    cfg_seal(c);
}

void cfg_delete(struct pl_config *c, u8 index)
{
    u8 i;
    if (index >= c->count) return;
    for (i = index; i + 1 < c->count; i++) c->entry[i] = c->entry[i + 1];
    c->count--;
    pl_memset(&c->entry[c->count], 0, sizeof c->entry[0]);
    if (c->deflt >= c->count && c->deflt != PL_NO_ENTRY) c->deflt = c->count ? c->count - 1 : 0;
    if (c->deflt > index && c->deflt != PL_NO_ENTRY) c->deflt--;
    c->last = PL_NO_ENTRY;
    c->oneshot = PL_NO_ENTRY;
}

int cfg_insert(struct pl_config *c, u8 index, const struct pl_entry *e)
{
    u8 i;
    if (c->count >= PL_MAX_ENTRIES) return -1;
    if (index > c->count) index = c->count;
    for (i = c->count; i > index; i--) c->entry[i] = c->entry[i - 1];
    c->entry[index] = *e;
    c->count++;
    if (c->deflt >= index && c->count > 1) c->deflt++;
    c->last = PL_NO_ENTRY;
    return index;
}

void cfg_move(struct pl_config *c, u8 index, s8 dir)
{
    u8 j = (u8)(index + dir);
    struct pl_entry t;
    if (index >= c->count || j >= c->count) return;
    t = c->entry[index]; c->entry[index] = c->entry[j]; c->entry[j] = t;
    if (c->deflt == index) c->deflt = j;
    else if (c->deflt == j) c->deflt = index;
    c->last = PL_NO_ENTRY;
}

/* --- entries from discovered devices ------------------------------------ */

void entry_from_part(struct pl_entry *e, struct part *p)
{
    struct bdisk *d = &disks[p->disk];
    pl_memset(e, 0, sizeof *e);
    e->kind = PK_PART;
    e->icon = p->icon;
    e->drive = d->drive;
    e->ptype = p->type;
    e->pindex = p->index;
    e->lba = p->lba;
    e->sectors = p->sectors;
    e->vdrive = 0x80;
    if (p->os[0]) pl_strncpy(e->name, p->os, sizeof e->name);
    else if (p->label[0]) pl_strncpy(e->name, p->label, sizeof e->name);
    else pl_strncpy(e->name, p->fs, sizeof e->name);
    if (p->label[0] && p->os[0] && pl_strlen(e->name) + pl_strlen(p->label) + 3 < sizeof e->name) {
        pl_strcat(e->name, " (");
        pl_strcat(e->name, p->label);
        pl_strcat(e->name, ")");
    }
    if (d->drive != 0x80) e->flags |= PF_SWAP | PF_FIXBPB;
    if (p->index <= 4 && ptype_hideable(p->type)) e->flags |= PF_ACTIVE | PF_UNHIDE;
}

void entry_from_atapi(struct pl_entry *e, struct atadev *a)
{
    pl_memset(e, 0, sizeof *e);
    e->kind = a->type == AT_CDROM ? PK_CDROM : PK_ATAPIDISK;
    e->icon = atapi_icon(a);
    e->drive = a->slave;
    e->lba = (u32)a->io | (u32)a->ctl << 16;
    e->emu = EMU_AUTO;
    e->vdrive = a->type == AT_CDROM ? 0xE0 : 0x80;
    pl_strncpy(e->name, a->type == AT_CDROM ? "CD-ROM: " : "Disk: ", sizeof e->name);
    pl_strcat(e->name, a->model);
    e->name[sizeof e->name - 1] = 0;
}

void entry_simple(struct pl_entry *e, u8 kind, u8 drive)
{
    pl_memset(e, 0, sizeof *e);
    e->kind = kind;
    e->drive = drive;
    switch (kind) {
    case PK_FLOPPY:
        e->icon = PI_FLOPPY;
        pl_strncpy(e->name, drive ? "Floppy disk B:" : "Floppy disk A:", sizeof e->name);
        break;
    case PK_DISK:
        e->icon = PI_DISK;
        pl_strncpy(e->name, "Hard disk 0 (MBR)", sizeof e->name);
        e->name[10] = (char)('0' + (drive & 0x7F));
        e->vdrive = 0x80;
        if (drive != 0x80) e->flags = PF_SWAP;
        break;
    case PK_BIOSNEXT:
        e->icon = PI_NET;
        pl_strncpy(e->name, "Next BIOS boot device", sizeof e->name);
        break;
    case PK_REBOOT:
        e->icon = PI_REBOOT;
        pl_strncpy(e->name, "Restart", sizeof e->name);
        break;
    case PK_POWEROFF:
        e->icon = PI_POWER;
        pl_strncpy(e->name, "Power off", sizeof e->name);
        break;
    case PK_SEPARATOR:
        e->icon = PI_STAR;
        pl_strncpy(e->name, "Group", sizeof e->name);
        break;
    }
}

/* Build a menu from everything that looks bootable. */
void cfg_autodetect(struct pl_config *c)
{
    struct pl_entry e;
    u8 i, hot = '1';
    c->count = 0;
    for (i = 0; i < nparts && c->count < PL_MAX_ENTRIES - 4; i++) {
        struct part *p = &parts[i];
        if (ptype_extended(p->type) || p->type == 0x82 || p->type == 0xEE ||
            p->type == 0xEF || p->type == 0x8E || p->type == 0xFD)
            continue;
        if (!p->bootable) continue;
        entry_from_part(&e, p);
        if (hot <= '9') e.hotkey = hot++;
        cfg_insert(c, c->count, &e);
    }
    for (i = 0; i < natapi && c->count < PL_MAX_ENTRIES - 2; i++) {
        entry_from_atapi(&e, &atapi[i]);
        if (hot <= '9') e.hotkey = hot++;
        cfg_insert(c, c->count, &e);
    }
    if (nfloppies) {
        entry_simple(&e, PK_FLOPPY, 0);
        e.hotkey = 'A';
        cfg_insert(c, c->count, &e);
    }
    c->deflt = 0;
    c->last = PL_NO_ENTRY;
    c->oneshot = PL_NO_ENTRY;
}
