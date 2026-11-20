/* Polaris - DOS installer and configurator (POLARIS.EXE).
 *
 *   POLARIS                       interactive installer
 *   POLARIS /INSTALL 80 [/Y]      install on hard disk 80h
 *   POLARIS /FLOPPY A [/Y]        make a Polaris boot floppy in A: or B:
 *   POLARIS /UNINSTALL 80 [/Y]    restore the original MBR
 *   POLARIS /SAVE 80 FILE         save track 0 (63 sectors) to a file
 *   POLARIS /RESTORE 80 FILE [/Y] write a saved track 0 back
 *   POLARIS /STATUS               show disks and Polaris installations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "polaris.h"
#include "disk.h"
#include "atapi.h"
#include "config.h"
#include "tui.h"
#include "ui.h"

extern const unsigned char __far stage1_bin[];
extern const unsigned int stage1_bin_len;
extern const unsigned char __far stage2_bin[];
extern const unsigned int stage2_bin_len;
extern const char *ui_where;

u8 cfg_drive;
static u8 ui_on;
static u8 yes;
static u8 sec[512];

#define TRACK0 63

/* ---------------------------------------------------------------------- */

static u16 s2_sectors(void) { return (stage2_bin_len + 511) / 512; }

int cfg_store(void)
{
    u8 i;
    for (i = 0; i < PL_CONFIG_SECTS; i++)
        if (disk_write(cfg_drive, PL_LBA_CONFIG + i, (u8 FAR *)&cfg + i * 512)) return -1;
    return 0;
}

static void say(const char *title, const char *text, u8 tone)
{
    if (ui_on) tui_msg(title, text, "OK", tone);
    else printf("%s: %s\n", title, text);
}

static int confirm(const char *title, const char *text, const char *yesno)
{
    if (yes) return 1;
    if (ui_on) return tui_msg(title, text, yesno, MT_WARN) == 0;
    printf("%s\n%s\nProceed (Y/N)? ", title, text);
    {
        int c = getchar();
        return c == 'y' || c == 'Y';
    }
}

static void progress(const char *step, u16 n, u16 d)
{
    if (ui_on) tui_progress("Working", step, n, d);
    else if (n == 0) printf("%s\n", step);
}

static void progress_end(void)
{
    if (ui_on) tui_progress(0, 0, 0, 0);
}

/* is Polaris in sector 0 of this drive (sec[] holds the sector) */
static int is_polaris(void)
{
    return !memcmp(sec + PL_INFO_OFS, "PLRS", 4);
}

static void disk_label(u8 drive, char *b)
{
    struct bdisk *d = disk_find(drive);
    char t[16];
    sprintf(b, "Hard disk %02Xh  %s", drive, d ? pl_size(t, d->sectors) : "?");
    if (!disk_read(drive, 0, sec)) {
        if (is_polaris()) strcat(b, "  [Polaris installed]");
        else if (sec[510] != 0x55 || sec[511] != 0xAA) strcat(b, "  [no partition table]");
    }
}

/* ---------------------------------------------------------------------- */

static int write_stage2(u8 drive)
{
    u16 i, n = s2_sectors();
    for (i = 0; i < n; i++) {
        u16 len = (u16)(stage2_bin_len - i * 512u);
        memset(sec, 0, 512);
        _fmemcpy(sec, stage2_bin + i * 512u, len > 512 ? 512 : len);
        if (disk_write(drive, PL_LBA_STAGE2 + i, sec)) return -1;
        if (!(i & 3)) progress("Writing the boot menu (stage 2)...", i, n);
    }
    return 0;
}

static void set_info(u8 *s1, u8 media)
{
    struct pl_info *in = (struct pl_info *)(s1 + PL_INFO_OFS);
    in->stage2_lba = PL_LBA_STAGE2;
    in->stage2_count = s2_sectors();
    in->config_lba = PL_LBA_CONFIG;
    in->media = media;
}

int install_hdd(u8 drive)
{
    static u8 mbr[512];
    u16 need = PL_LBA_STAGE2 + s2_sectors(), i;
    u32 first = 0xFFFFFFFFUL;
    u8 reinstall, used = 0;
    char msg[400];

    if (!disk_find(drive)) { say("Install", "That hard disk does not exist.", MT_ERROR); return -1; }
    if (disk_read(drive, 0, mbr)) { say("Install", "Cannot read the master boot record.", MT_ERROR); return -1; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        if (!confirm("No partition table",
                     "This disk has no partition table. Polaris will write an empty one.\n"
                     "Use FDISK afterwards to create partitions.", "Continue|Cancel")) return -1;
        memset(mbr, 0, 512);
    }
    for (i = 0; i < 4; i++) {
        u8 *e = mbr + 0x1BE + i * 16;
        u32 lba = *(u32 *)(e + 8);
        if (e[4] && lba < first) first = lba;
    }
    if (first < need) {
        sprintf(msg, "The first partition starts at sector %lu, but Polaris needs\n"
                     "sectors 0-%u of track 0. Nothing was changed.", first, need - 1);
        say("Not enough room", msg, MT_ERROR);
        return -1;
    }
    reinstall = !memcmp(mbr + PL_INFO_OFS, "PLRS", 4);
    if (!reinstall)
        for (i = 1; i < need && !used; i++) {
            u16 k;
            if (disk_read(drive, i, sec)) break;
            for (k = 0; k < 512; k++) if (sec[k]) { used = 1; break; }
        }
    sprintf(msg, "Polaris %s will be written to sectors 0-%u of hard disk %02Xh.\n"
                 "The partition table is kept; the current boot code is saved\n"
                 "so that it can be restored with Uninstall.%s",
            PL_VERSION_STR, need - 1, drive,
            used ? "\n\nWARNING: track 0 already holds data (another boot manager?)\n"
                   "It will be overwritten." : "");
    if (!confirm(reinstall ? "Update Polaris" : "Install Polaris", msg, "Install|Cancel")) return -1;

    /* configuration: keep an existing one, otherwise detect */
    progress("Reading the configuration...", 0, 4);
    cfg_drive = drive;
    {
        u8 ok = reinstall;
        for (i = 0; ok && i < PL_CONFIG_SECTS; i++)
            if (disk_read(drive, PL_LBA_CONFIG + i, (u8 FAR *)&cfg + i * 512)) ok = 0;
        if (!ok || !cfg_valid(&cfg)) {
            cfg_defaults(&cfg);
            progress("Detecting operating systems and drives...", 1, 4);
            cfg_autodetect(&cfg);
        }
        if (!reinstall) memcpy(cfg.mbr_backup, mbr, 512);
        cfg_seal(&cfg);
    }
    progress("Writing the boot menu (stage 2)...", 2, 4);
    if (write_stage2(drive) || cfg_store()) {
        progress_end();
        say("Install", "Write error. The master boot record was not changed.", MT_ERROR);
        return -1;
    }
    progress("Writing the master boot record...", 3, 4);
    {
        static u8 s1[512];
        _fmemcpy(s1, stage1_bin, 512);
        set_info(s1, 0);
        memcpy(s1 + 0x1B8, mbr + 0x1B8, 0x48);
        s1[510] = 0x55; s1[511] = 0xAA;
        if (disk_write(drive, 0, s1) || disk_read(drive, 0, sec) || memcmp(sec, s1, 512)) {
            progress_end();
            say("Install", "The master boot record could not be written or verified.", MT_ERROR);
            return -1;
        }
    }
    progress_end();
    sprintf(msg, "Polaris is installed on hard disk %02Xh with %u menu entries.\n"
                 "Use Configure to change the menu, or press F2 in the boot menu.",
            drive, cfg.count);
    say("Done", msg, MT_OK);
    return 0;
}

int uninstall_hdd(u8 drive)
{
    static u8 mbr[512];
    u16 i, n;
    if (disk_read(drive, 0, mbr) || memcmp(mbr + PL_INFO_OFS, "PLRS", 4)) {
        say("Uninstall", "Polaris is not installed on that disk.", MT_ERROR);
        return -1;
    }
    for (i = 0; i < PL_CONFIG_SECTS; i++)
        if (disk_read(drive, PL_LBA_CONFIG + i, (u8 FAR *)&cfg + i * 512)) break;
    if (!confirm("Uninstall Polaris",
                 "The boot code that was in place before Polaris is restored.\n"
                 "The current partition table is kept.", "Uninstall|Cancel")) return -1;
    if (cfg.mbr_backup[510] == 0x55 && cfg.mbr_backup[511] == 0xAA &&
        memcmp(cfg.mbr_backup + PL_INFO_OFS, "PLRS", 4))
        memcpy(sec, cfg.mbr_backup, 0x1B8);
    else
        memset(sec, 0, 0x1B8);           /* no saved code: an empty (non-booting) MBR */
    memcpy(sec + 0x1B8, mbr + 0x1B8, 0x48);
    if (disk_write(drive, 0, sec)) { say("Uninstall", "Write error.", MT_ERROR); return -1; }
    n = PL_LBA_STAGE2 + ((struct pl_info *)(mbr + PL_INFO_OFS))->stage2_count;
    memset(sec, 0, 512);
    for (i = 1; i < n; i++) disk_write(drive, i, sec);
    say("Done", "Polaris was removed and the previous boot code restored.", MT_OK);
    return 0;
}

/* --- floppy ---------------------------------------------------------------- */

int install_floppy(u8 drive)
{
    static const struct { u8 type; u16 total; u8 spt, media, spc; u16 rootents; u8 fatsz; } fmt[] = {
        {2, 2400, 15, 0xF9, 1, 224, 7}, {3, 1440, 9, 0xF9, 2, 112, 3},
        {4, 2880, 18, 0xF0, 1, 224, 9}, {6, 5760, 36, 0xF0, 2, 240, 9},
    };
    struct bdisk g;
    static u8 s1[512];
    u8 f = 2, i;
    u16 lba, rootsecs;
    char msg[200];

    if (disk_geometry(drive, &g)) { say("Floppy", "That floppy drive does not exist.", MT_ERROR); return -1; }
    for (i = 0; i < 4; i++) if (fmt[i].type == g.ftype) f = i;
    sprintf(msg, "Everything on the disk in drive %c: will be erased.\n"
                 "The disk must already be formatted (%u KB).",
            'A' + drive, fmt[f].total / 2);
    if (!confirm("Create a Polaris floppy", msg, "Write|Cancel")) return -1;
    /* the disk geometry must be known before the first sector is written */
    {
        struct bdisk *d = disk_find(drive);
        d->spt = fmt[f].spt; d->heads = 2; d->cyls = 80; d->sectors = fmt[f].total;
    }
    _fmemcpy(s1, stage1_bin, 512);
    set_info(s1, 1);
    /* BPB */
    memcpy(s1 + 3, "POLARIS ", 8);
    *(u16 *)(s1 + 0x0B) = 512;
    s1[0x0D] = fmt[f].spc;
    *(u16 *)(s1 + 0x0E) = PL_RESERVED;
    s1[0x10] = 2;
    *(u16 *)(s1 + 0x11) = fmt[f].rootents;
    *(u16 *)(s1 + 0x13) = fmt[f].total;
    s1[0x15] = fmt[f].media;
    *(u16 *)(s1 + 0x16) = fmt[f].fatsz;
    *(u16 *)(s1 + 0x18) = fmt[f].spt;
    *(u16 *)(s1 + 0x1A) = 2;
    *(u32 *)(s1 + 0x1C) = 0;
    s1[0x24] = 0; s1[0x26] = 0x29;
    *(u32 *)(s1 + 0x27) = 0x504C5253UL;
    memcpy(s1 + 0x2B, "POLARIS    FAT12   ", 19);
    s1[510] = 0x55; s1[511] = 0xAA;

    cfg_defaults(&cfg);                     /* empty: detect on every machine */
    cfg_drive = drive;
    progress("Writing the boot sector...", 0, 4);
    if (disk_write(drive, 0, s1)) { progress_end(); say("Floppy", "Write error (write protected?).", MT_ERROR); return -1; }
    memset(sec, 0, 512);
    for (lba = 1; lba < PL_LBA_STAGE2; lba++) disk_write(drive, lba, sec);
    cfg_seal(&cfg);
    if (cfg_store() || write_stage2(drive)) { progress_end(); say("Floppy", "Write error.", MT_ERROR); return -1; }
    progress("Creating the FAT12 file system...", 3, 4);
    rootsecs = fmt[f].rootents / 16;
    lba = PL_RESERVED;
    for (i = 0; i < 2; i++) {
        u8 k;
        for (k = 0; k < fmt[f].fatsz; k++) {
            memset(sec, 0, 512);
            if (!k) { sec[0] = fmt[f].media; sec[1] = 0xFF; sec[2] = 0xFF; }
            if (disk_write(drive, lba++, sec)) { progress_end(); say("Floppy", "Write error.", MT_ERROR); return -1; }
        }
    }
    for (i = 0; i < rootsecs; i++) {
        memset(sec, 0, 512);
        if (!i) { memcpy(sec, "POLARIS    ", 11); sec[11] = 0x08; }
        disk_write(drive, lba++, sec);
    }
    progress_end();
    sprintf(msg, "Drive %c: now starts Polaris. The disk is still an ordinary\n"
                 "FAT12 disk that DOS can store files on.\n"
                 "Its menu is built automatically on every computer.", 'A' + drive);
    say("Done", msg, MT_OK);
    return 0;
}

/* --- track 0 backup -------------------------------------------------------- */

int save_track0(u8 drive, const char *file)
{
    FILE *fp = fopen(file, "wb");
    u16 i;
    if (!fp) { say("Backup", "Cannot create the file.", MT_ERROR); return -1; }
    for (i = 0; i < TRACK0; i++) {
        if (disk_read(drive, i, sec) || fwrite(sec, 512, 1, fp) != 1) {
            fclose(fp);
            say("Backup", "Read or write error.", MT_ERROR);
            return -1;
        }
        progress("Saving track 0...", i, TRACK0);
    }
    fclose(fp);
    progress_end();
    say("Backup", "Track 0 (63 sectors) was saved.", MT_OK);
    return 0;
}

int restore_track0(u8 drive, const char *file)
{
    FILE *fp = fopen(file, "rb");
    static u8 mbr[512];
    u16 i;
    if (!fp) { say("Restore", "Cannot open the file.", MT_ERROR); return -1; }
    if (!confirm("Restore track 0", "Track 0 of the disk is replaced by the saved copy.\n"
                 "The current partition table is kept.", "Restore|Cancel")) { fclose(fp); return -1; }
    disk_read(drive, 0, mbr);
    for (i = 0; i < TRACK0; i++) {
        if (fread(sec, 512, 1, fp) != 1) break;
        if (!i) memcpy(sec + 0x1B8, mbr + 0x1B8, 0x48);
        if (disk_write(drive, i, sec)) { fclose(fp); say("Restore", "Write error.", MT_ERROR); return -1; }
        progress("Restoring track 0...", i, TRACK0);
    }
    fclose(fp);
    progress_end();
    say("Restore", "Track 0 was restored.", MT_OK);
    return 0;
}

/* --- interactive ------------------------------------------------------------ */

static int pick_disk(const char *title, u8 need_polaris)
{
    static char labels[MAX_DISKS][48];
    const char *items[MAX_DISKS];
    u8 map[MAX_DISKS], n = 0, i;
    int r;
    for (i = 0; i < ndisks; i++) {
        if (need_polaris && (disk_read(disks[i].drive, 0, sec) || !is_polaris())) continue;
        disk_label(disks[i].drive, labels[n]);
        items[n] = labels[n];
        map[n++] = disks[i].drive;
    }
    if (!n) {
        say(title, need_polaris ? "Polaris is not installed on any hard disk." : "No hard disks found.", MT_WARN);
        return -1;
    }
    r = tui_choose(title, items, n, 0);
    return r < 0 ? -1 : map[r];
}

static void draw_status(void)
{
    u8 i, y = 8;
    char t[48];
    tui_card(46, 6, 32, 17, "This computer", C_BORDER, C_PANEL);
    tui_shadow(46, 6, 32, 17);
    for (i = 0; i < ndisks && y < 19; i++, y += 2) {
        u8 pol = !disk_read(disks[i].drive, 0, sec) && is_polaris();
        tui_icon(48, y, PI_DISK, A(pol ? C_GOLD : C_DIM, C_PANEL));
        sprintf(t, "Disk %02Xh  ", disks[i].drive);
        pl_size(t + strlen(t), disks[i].sectors);
        tui_puts(51, y, t, A(C_TEXT, C_PANEL));
        tui_puts(51, y + 1, pol ? "Polaris installed" : "Other boot code", A(pol ? C_LGREEN : C_DIM, C_PANEL));
    }
    for (i = 0; i < nfloppies && y < 21; i++, y++) {
        tui_icon(48, y, PI_FLOPPY, A(C_ICE, C_PANEL));
        sprintf(t, "Floppy drive %c:", 'A' + i);
        tui_puts(51, y, t, A(C_TEXT, C_PANEL));
    }
    for (i = 0; i < natapi && y < 22; i++, y++) {
        tui_icon(48, y, atapi_icon(&atapi[i]), A(C_GOLD, C_PANEL));
        tui_putsn(51, y, atapi[i].model, A(C_TEXT, C_PANEL), 25);
    }
}

static void interactive(void)
{
    static const char *const items[] = {
        "Install Polaris on a hard disk", "Create a Polaris boot floppy",
        "Configure the boot menu", "Save track 0 to a file", "Restore track 0 from a file",
        "Uninstall Polaris", "Exit to DOS"
    };
    static const char *const hints[] = {
        "Writes the boot manager to track 0 of a hard disk.",
        "Makes a boot floppy that still stores DOS files.",
        "Edits the menu that is stored on a hard disk.",
        "Keeps a copy of the MBR and track 0 in a file.",
        "Writes a saved copy of track 0 back to a disk.",
        "Puts the previous boot code back in the MBR.",
        "Leaves the installer."
    };
    static const u8 icons[] = { PI_DISK, PI_FLOPPY, PI_GEAR, PI_ZIP, PI_REBOOT, PI_POWER, PI_STAR };
    static const char *const keys[] = { "\x18\x19", "Choose", "Enter", "Run", "Esc", "Exit", 0, 0 };
    u8 sel = 0, i;
    for (;;) {
        u16 k;
        draw_backdrop("Installer and configuration");
        tui_card(2, 6, 42, 17, "What would you like to do?", C_ICE, C_PANEL);
        tui_shadow(2, 6, 42, 17);
        for (i = 0; i < 7; i++) {
            u8 y = 8 + i * 2, on = i == sel, bg = on ? C_SELECT : C_PANEL;
            tui_fill(3, y, 40, 1, ' ', A(C_TEXT, bg));
            if (on) tui_put(3, y, G_ACCENT, A(C_GOLD, bg));
            tui_icon(5, y, icons[i], A(on ? C_TEXT : C_ICE, bg));
            tui_puts(8, y, items[i], A(C_TEXT, bg));
        }
        draw_status();
        tui_fill(0, 23, SW, 1, ' ', A(C_TEXT, C_BG));
        tui_puts(3, 23, hints[sel], A(C_DIM, C_BG));
        draw_footer(keys);
        tui_flush();
        k = tui_getkey();
        if (k == K_UP) sel = sel ? sel - 1 : 6;
        else if (k == K_DOWN || k == K_TAB) sel = (u8)((sel + 1) % 7);
        else if (k == K_ESC) return;
        else if (k == K_ENTER) {
            int d;
            char name[64];
            switch (sel) {
            case 0:
                if ((d = pick_disk("Install on", 0)) >= 0 && !install_hdd((u8)d) &&
                    tui_msg("Configure", "Review the boot menu now?", "Configure|Later", MT_INFO) == 0) {
                    disk_scan(); atapi_scan(0);
                    setup_run(1);
                }
                break;
            case 1: {
                static const char *const fl[] = { "Drive A:", "Drive B:" };
                int r = tui_choose("Floppy drive", fl, (u8)(nfloppies > 1 ? 2 : 1), 0);
                if (r >= 0) install_floppy((u8)r);
                break;
            }
            case 2:
                if ((d = pick_disk("Configure", 1)) >= 0) {
                    cfg_drive = (u8)d;
                    for (i = 0; i < PL_CONFIG_SECTS; i++)
                        disk_read(cfg_drive, PL_LBA_CONFIG + i, (u8 FAR *)&cfg + i * 512);
                    if (!cfg_valid(&cfg)) { cfg_defaults(&cfg); cfg_autodetect(&cfg); }
                    tui_theme(cfg.theme);
                    setup_run(1);
                }
                break;
            case 3:
                if ((d = pick_disk("Save track 0 of", 0)) >= 0) {
                    sprintf(name, "TRACK0%02X.BIN", d);
                    if (tui_ask("Save track 0", "File name:", name, 60, 0)) save_track0((u8)d, name);
                }
                break;
            case 4:
                if ((d = pick_disk("Restore track 0 of", 0)) >= 0) {
                    sprintf(name, "TRACK0%02X.BIN", d);
                    if (tui_ask("Restore track 0", "File name:", name, 60, 0)) restore_track0((u8)d, name);
                }
                break;
            case 5:
                if ((d = pick_disk("Uninstall from", 1)) >= 0) uninstall_hdd((u8)d);
                break;
            case 6:
                return;
            }
        }
    }
}

static void usage(void)
{
    printf("Polaris boot manager %s - installer\n\n"
           "  POLARIS                       interactive installer\n"
           "  POLARIS /INSTALL 80 [/Y]      install on hard disk 80h\n"
           "  POLARIS /FLOPPY A [/Y]        create a Polaris boot floppy\n"
           "  POLARIS /UNINSTALL 80 [/Y]    restore the previous boot code\n"
           "  POLARIS /SAVE 80 FILE         save track 0 to a file\n"
           "  POLARIS /RESTORE 80 FILE [/Y] write a saved track 0 back\n"
           "  POLARIS /STATUS               list disks and installations\n", PL_VERSION_STR);
}

static u8 parse_drive(const char *s)
{
    if ((s[0] == 'A' || s[0] == 'a' || s[0] == 'B' || s[0] == 'b') && (!s[1] || s[1] == ':'))
        return (u8)((s[0] | 0x20) - 'a');
    return (u8)strtoul(s, 0, 16);
}

int main(int argc, char **argv)
{
    int i, r = 0;
    const char *op = 0, *a1 = 0, *a2 = 0;
    for (i = 1; i < argc; i++) {
        if (!stricmp(argv[i], "/Y")) yes = 1;
        else if (argv[i][0] == '/' || argv[i][0] == '-') op = argv[i] + 1;
        else if (!a1) a1 = argv[i];
        else a2 = argv[i];
    }
    ui_where = "the installer";
    disk_scan();
    atapi_scan(0);
    if (!op) {
        cfg_defaults(&cfg);
        tui_init(0, 0);
        tui_idle = ui_idle;
        ui_on = 1;
        interactive();
        tui_done();
        printf("Polaris installer finished.\n");
        return 0;
    }
    if (!stricmp(op, "?") || !stricmp(op, "H")) { usage(); return 0; }
    if (!stricmp(op, "STATUS")) {
        u8 k;
        for (k = 0; k < ndisks; k++) {
            char b[64];
            disk_label(disks[k].drive, b);
            printf("%s\n", b);
        }
        for (k = 0; k < natapi; k++)
            printf("ATAPI %03Xh %s: %s (%s)\n", atapi[k].io, atapi[k].slave ? "slave" : "master",
                   atapi[k].model, atapi_kind(&atapi[k]));
        return 0;
    }
    if (!a1) { usage(); return 1; }
    if (!stricmp(op, "INSTALL")) r = install_hdd(parse_drive(a1));
    else if (!stricmp(op, "FLOPPY")) r = install_floppy(parse_drive(a1));
    else if (!stricmp(op, "UNINSTALL")) r = uninstall_hdd(parse_drive(a1));
    else if (!stricmp(op, "SAVE") && a2) r = save_track0(parse_drive(a1), a2);
    else if (!stricmp(op, "RESTORE") && a2) r = restore_track0(parse_drive(a1), a2);
    else { usage(); return 1; }
    return r ? 2 : 0;
}
