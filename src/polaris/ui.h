/* Polaris - screens shared by the boot menu and the installer. */
#ifndef UI_H
#define UI_H

#include "polaris.h"
#include "disk.h"
#include "atapi.h"
#include "tui.h"

enum { DV_DISK = 1, DV_PART, DV_FLOPPY, DV_ATAPI, DV_SPECIAL };

struct devitem {
    u8 type;
    u8 idx;          /* disk/part/atapi index, floppy drive or PK_* kind */
};

#define MAX_DEVITEMS (MAX_DISKS + MAX_PARTS + MAX_ATAPI + 8)
extern struct devitem devitems[MAX_DEVITEMS];

u8   devlist_build(u8 specials);
void devitem_label(struct devitem *it, char *buf, u8 width);
void devitem_entry(struct devitem *it, struct pl_entry *e);
void entry_location(struct pl_entry *e, char *buf);
void entry_detail(struct pl_entry *e, char *buf);
void hexbyte(char *d, u8 v);

/* setup.c */
int  setup_run(u8 installer);      /* 1 if the configuration was saved */
int  setup_entry(struct pl_entry *e);
void setup_options(void);
int  devices_run(u8 pick, struct pl_entry *out);   /* explorer / picker */
void sysinfo_run(void);

/* drawing helpers (menu.c / setup.c) */
void draw_backdrop(const char *subtitle);
void draw_footer(const char *const *keys);
void draw_diskmap(u8 x, u8 y, u8 w, u8 drive, u32 lba, u8 bg);
void draw_header_dynamic(void);
void ui_idle(void);

#endif
