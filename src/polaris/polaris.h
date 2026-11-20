/* Polaris boot manager - on-disk layout and configuration record.
 *
 * Layout on a hard disk (track 0, before the first partition) and on a
 * Polaris floppy (FAT12 reserved sectors):
 *
 *   LBA 0        stage 1 (MBR / floppy boot sector, keeps partition table/BPB)
 *   LBA 1..4     configuration (struct pl_config, 2048 bytes), which also
 *                holds the original MBR saved at install time
 *   LBA 5..62    stage 2: unpacker + PLZ-packed boot menu; length in stage 1
 *
 * tools/plimage.py mirrors these structures; keep both in sync.
 */
#ifndef POLARIS_H
#define POLARIS_H

#include "pl.h"

#define PL_VERSION_MAJOR 1
#define PL_VERSION_MINOR 0
#define PL_VERSION_STR   "1.0"

#define PL_LBA_CONFIG    1
#define PL_CONFIG_SECTS  4
#define PL_LBA_STAGE2    5
#define PL_MAX_STAGE2    58          /* sectors 5..62 */
#define PL_RESERVED      64          /* sectors reserved on a Polaris floppy */

/* stage-1 info block, at offset 0x1A0 of sector 0 */
#define PL_INFO_OFS      0x1A0
struct pl_info {
    char magic[4];                   /* "PLRS" */
    u8   major, minor;
    u16  stage2_lba;
    u16  stage2_count;
    u16  config_lba;
    u8   media;                      /* 0 = hard disk, 1 = floppy */
    u8   pad;
};

/* stage-2 image header (first bytes after the entry jump) */
#define PL_STAGE2_MAGIC  "POLARIS2"

/* --- entry kinds ------------------------------------------------------- */
enum {
    PK_NONE = 0,
    PK_PART,          /* partition boot sector on a BIOS disk            */
    PK_DISK,          /* master boot record of a BIOS disk (chain)        */
    PK_FLOPPY,        /* floppy drive A: or B:                            */
    PK_CDROM,         /* ATAPI CD/DVD via the built-in El Torito driver   */
    PK_ATAPIDISK,     /* ATAPI removable disk: ZIP, LS-120, MO, PD        */
    PK_BIOSNEXT,      /* INT 18h - next BIOS boot device / network        */
    PK_REBOOT,
    PK_POWEROFF,
    PK_SEPARATOR,     /* menu group heading                               */
    PK_COUNT
};

/* entry flags */
#define PF_ACTIVE    0x01    /* mark the partition active when booting      */
#define PF_SWAP      0x02    /* present the disk as drive 80h (swap)        */
#define PF_FIXBPB    0x04    /* patch hidden sectors / drive no. in memory  */
#define PF_PASSWORD  0x08    /* ask for the entry password                  */
#define PF_UNHIDE    0x10    /* unhide the target partition before boot     */
#define PF_NOCHECK   0x20    /* boot even without the 55AA signature        */

/* ATAPI emulation choices (entry.emu) */
enum { EMU_AUTO = 0, EMU_NONE, EMU_FLOPPY, EMU_HDD };

/* icons */
enum {
    PI_DISK = 0, PI_DOS, PI_WIN9X, PI_WINNT, PI_OS2, PI_LINUX, PI_BSD,
    PI_CDROM, PI_FLOPPY, PI_ZIP, PI_NET, PI_POWER, PI_REBOOT, PI_GEAR,
    PI_STAR, PI_LOCK, PI_COUNT
};

struct pl_entry {                 /* 64 bytes */
    char name[28];
    u8   kind;
    u8   icon;
    u8   hotkey;                  /* upper-case ASCII or 0 */
    u8   flags;
    u8   drive;                   /* BIOS drive, or ATAPI slot (see atapi.h) */
    u8   ptype;                   /* partition type when the entry was made */
    u8   pindex;                  /* 1-4 primary, 5.. logical (0 = n/a) */
    u8   emu;                     /* ATAPI emulation mode */
    u32  lba;                     /* partition start (absolute LBA) */
    u32  sectors;                 /* partition length */
    u16  pw;                      /* password hash */
    u8   hidemask;                /* bit n: hide primary n+1 when booting */
    u8   vdrive;                  /* drive number presented with PF_SWAP */
    u8   reserved[16];
};

#define PL_MAX_ENTRIES   20
#define PL_CFG_MAGIC     "POLARIS\x1A"
#define PL_CFG_VERSION   1

/* config flags */
#define CF_REMEMBER  0x01        /* default = last booted entry */
#define CF_QUIET     0x02        /* hide menu until a key is pressed */
#define CF_NOSTARS   0x04        /* no header animation */
#define CF_NOFONT    0x08        /* standard VGA font only */
#define CF_SOUND     0x10        /* beep when the menu appears */

#define PL_NO_ENTRY      0xFF
#define PL_TIMEOUT_NEVER 0xFF

struct pl_config {               /* exactly 2048 bytes */
    char magic[8];
    u16  version;
    u16  size;
    u8   count;
    u8   deflt;
    u8   timeout;                /* seconds, PL_TIMEOUT_NEVER = wait */
    u8   theme;
    u8   flags;
    u8   last;
    u8   oneshot;                /* boot this entry once, then clear */
    u8   pad;
    u16  setup_pw;               /* hash, 0 = none */
    u16  checksum;               /* sum of all words except this = 0 */
    char title[32];              /* shown under the logo */
    u8   reserved1[8];
    struct pl_entry entry[PL_MAX_ENTRIES];
    u8   reserved2[2048 - 64 - PL_MAX_ENTRIES * 64 - 512];
    u8   mbr_backup[512];        /* original MBR (hard disk installs) */
};

/* compile-time layout checks */
typedef char pl_entry_is_64_bytes[sizeof(struct pl_entry) == 64 ? 1 : -1];
typedef char pl_config_is_2048_bytes[sizeof(struct pl_config) == 2048 ? 1 : -1];
typedef char pl_info_is_14_bytes[sizeof(struct pl_info) == 14 ? 1 : -1];

/* config.c */
extern struct pl_config cfg;
void cfg_defaults(struct pl_config *c);
u16  cfg_checksum(struct pl_config *c);
int  cfg_valid(struct pl_config *c);
void cfg_seal(struct pl_config *c);
void cfg_delete(struct pl_config *c, u8 index);
int  cfg_insert(struct pl_config *c, u8 index, const struct pl_entry *e);
void cfg_move(struct pl_config *c, u8 index, s8 dir);
const char *kind_name(u8 kind);

#endif
