/* MagicDisk - 80-column DOS workspace, menus and interactive disk map. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef __WATCOMC__
#include <conio.h>
#include <i86.h>
#include <dos.h>

#define LOG_MAX 512
#define NAV_LEFT (256 + 75)
#define NAV_RIGHT (256 + 77)
#define NAV_UP (256 + 72)
#define NAV_DOWN (256 + 80)
#define NAV_PGUP (256 + 73)
#define NAV_PGDN (256 + 81)
#define NAV_HOME (256 + 71)
#define NAV_END (256 + 79)
#define KEY_F1 (256 + 59)
#define KEY_F8 (256 + 66)
#define KEY_F10 (256 + 68)

/* VGA palette: navy workspace, silver chrome, cyan focus, amber warnings. */
#define WORK 0x17
#define MUTED 0x18
#define BRIGHT 0x1f
#define CYAN 0x1b
#define GREEN 0x1a
#define AMBER 0x1e
#define RED 0x1c
#define CHROME 0x70
#define FOCUS 0x3f
#define TITLE 0x3f

typedef struct {
    char code[24];
    char *message;
    dt_u32 where;
} Finding;

typedef struct {
    DtDisk disk;
    DtVolume volume;
    DtPartition partitions[128];
    unsigned count, selected, cell, drive, recovery_source;
    int opened, bios, mounted, scanned, retested, preview, busy;
    int surfaced, surface_complete;
    dt_u32 surface_tested, surface_failed;
    DtMap map;
    DtResult result;
    DtRetest retest;
    Finding log[LOG_MAX];
    unsigned log_count, omitted;
    char source[128], status[160], phase[40];
    dt_u32 done, total;
    clock_t painted;
} Workspace;

static Workspace ui;
static unsigned short canvas[2000], previous[2000];

static void cell(int x, int y, unsigned ch, unsigned attr)
{
    if (x >= 0 && x < 80 && y >= 0 && y < 25)
        canvas[y * 80 + x] = (unsigned short)(ch | (attr << 8));
}

static void fill(int x, int y, int w, unsigned ch, unsigned attr)
{
    int i;
    for (i = 0; i < w; i++)
        cell(x + i, y, ch, attr);
}

static void text(int x, int y, unsigned attr, const char *s)
{
    while (*s && x < 80)
        cell(x++, y, (unsigned char)*s++, attr);
}

static void box(int x, int y, int w, int h, const char *title)
{
    int i;
    fill(x + 1, y, w - 2, 196, CYAN);
    fill(x + 1, y + h - 1, w - 2, 196, CYAN);
    for (i = 1; i < h - 1; i++) {
        fill(x + 1, y + i, w - 2, ' ', WORK);
        cell(x, y + i, 179, CYAN);
        cell(x + w - 1, y + i, 179, CYAN);
    }
    cell(x, y, 218, CYAN);
    cell(x + w - 1, y, 191, CYAN);
    cell(x, y + h - 1, 192, CYAN);
    cell(x + w - 1, y + h - 1, 217, CYAN);
    if (title)
        text(x + 2, y, BRIGHT, title);
}

/* VGA DAC colors for readable navy, teal and silver chrome. */
static void video_palette(void)
{
    static const unsigned char dac[] = {0, 1, 2, 3, 4, 5, 20, 7, 56, 57, 58, 59, 60, 61, 62, 63};
    static const unsigned char colors[16][3] = {{4, 7, 11},
                                                {5, 10, 18},
                                                {12, 30, 24},
                                                {7, 28, 33},
                                                {40, 14, 18},
                                                {34, 22, 38},
                                                {40, 30, 16},
                                                {43, 48, 52},
                                                {27, 34, 41},
                                                {22, 37, 52},
                                                {22, 51, 38},
                                                {25, 54, 57},
                                                {63, 28, 31},
                                                {49, 36, 54},
                                                {63, 49, 25},
                                                {59, 62, 63}};
    unsigned i;

    for (i = 0; i < 16; i++) {
        union REGS r;
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x1010;
        r.w.bx = dac[i];
        r.h.dh = colors[i][0];
        r.h.ch = colors[i][1];
        r.h.cl = colors[i][2];
        int386(0x10, &r, &r);
    }
}

static unsigned video_selector;
static unsigned old_mode, old_page, old_cursor, old_position;
static unsigned short saved_screen[2000];

static int video_open(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0002;
    r.w.bx = 0xb800;
    int386(0x31, &r, &r);
    if (r.x.cflag)
        return 0;
    video_selector = r.w.ax;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0x0f;
    int386(0x10, &r, &r);
    old_mode = r.h.al & 0x7f;
    old_page = r.h.bh;
    _fmemcpy(saved_screen, MK_FP(video_selector, 0), sizeof(saved_screen));

    memset(&r, 0, sizeof(r));
    r.h.ah = 3;
    r.h.bh = (unsigned char)old_page;
    int386(0x10, &r, &r);
    old_cursor = r.w.cx;
    old_position = r.w.dx;

    memset(&r, 0, sizeof(r));
    r.w.ax = 3;
    int386(0x10, &r, &r);
    r.h.ah = 1;
    r.w.cx = 0x2000;
    int386(0x10, &r, &r);
    video_palette();
    return 1;
}

static void video_close(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.w.ax = (unsigned short)old_mode;
    int386(0x10, &r, &r);
    if (old_mode == 3)
        _fmemcpy(MK_FP(video_selector, 0), saved_screen, sizeof(saved_screen));
    r.h.ah = 5;
    r.h.al = (unsigned char)old_page;
    int386(0x10, &r, &r);
    r.h.ah = 1;
    r.w.cx = (unsigned short)old_cursor;
    int386(0x10, &r, &r);
    r.h.ah = 2;
    r.h.bh = (unsigned char)old_page;
    r.w.dx = (unsigned short)old_position;
    int386(0x10, &r, &r);
}

/* A single text-page transfer avoids clear/redraw flicker during progress. */
static void flush(void)
{
    if (memcmp(canvas, previous, sizeof(canvas))) {
        _fmemcpy(MK_FP(video_selector, 0), canvas, sizeof(canvas));
        memcpy(previous, canvas, sizeof(canvas));
    }
}

static int key(void)
{
    int k = getch();
    if (k == 0 || k == 224)
        return 256 + getch();
    return k;
}

static void number(int x, int y, const char *label, dt_u32 n, unsigned color)
{
    char s[64];
    text(x, y, MUTED, label);
    sprintf(s, "%lu", (unsigned long)n);
    text(x, y + 1, color, s);
}

static void draw_map(void)
{
    unsigned i;
    char s[100];
    dt_u32 first, count, used = 0, free_count = 0, bad = 0, unreadable = 0, done = 0;

    box(1, 6, 67, 10, ui.map.surface ? " SURFACE READ MAP " : " ALLOCATION MAP ");
    for (i = 0; i < DT_MAP_CELLS; i++) {
        DtMapCell *c = &ui.map.cell[i];
        unsigned color = MUTED, glyph = 250;
        dt_map_range(&ui.map, i, &first, &count);
        if (i < ui.map.cells) {
            used += c->used;
            free_count += c->free;
            bad += c->bad;
            unreadable += c->unreadable;
            done += c->done;
            if (c->unreadable) {
                color = RED;
                glyph = '!';
            } else if (c->bad) {
                color = AMBER;
                glyph = 'X';
            } else if (c->done) {
                color = c->used ? CYAN : GREEN;
                glyph = c->used ? (c->used == count ? 254 : 177) : 250;
                if (c->done < count)
                    glyph = 178;
            }
        }
        if (i == ui.cell && i < ui.map.cells)
            color = FOCUS;
        cell(3 + i % 64, 8 + i / 64, glyph, color);
    }
    text(3,
         7,
         ui.volume.boot_source ? AMBER : MUTED,
         ui.volume.boot_source ? "BACKUP GEOMETRY" : "START");
    text(62, 7, MUTED, "END");
    sprintf(s,
            "%s  |  %u blocks  |  %lu %s",
            ui.map.complete ? "Complete" : "Partial / pending",
            ui.map.cells,
            (unsigned long)ui.map.count,
            ui.map.surface ? "sectors" : "clusters");
    text(3, 14, WORK, s);
    number(70, 6, ui.map.surface ? "Read OK" : "Used", used, CYAN);
    number(70, 9, ui.map.surface ? "Tested" : "Free", ui.map.surface ? done : free_count, GREEN);
    number(70, 12, ui.map.surface ? "Errors" : "Bad", ui.map.surface ? unreadable : bad, AMBER);
    number(70,
           15,
           ui.map.surface ? "Pending" : "Unknown",
           ui.map.surface ? ui.map.count - done : unreadable,
           RED);
    if (ui.map.surface) {
        text(2, 17, CYAN, "\376 Read OK");
        text(18, 17, RED, "! Read error");
        text(36, 17, MUTED, ". Pending");
    } else {
        text(2, 17, GREEN, "\372 Free");
        text(12, 17, CYAN, "\376 Used  \261 Mixed");
        text(32, 17, AMBER, "X Bad");
        text(42, 17, RED, "! Error");
        text(54, 17, MUTED, ". Pending");
    }
    text(66, 17, BRIGHT, "Focus: light");
    dt_map_range(&ui.map, ui.cell, &first, &count);
    if (count) {
        dt_u32 lba = ui.map.surface ? first : ui.volume.data + (first - 2) * ui.volume.spc;
        dt_u32 sectors = ui.map.surface ? count : count * ui.volume.spc;
        sprintf(s,
                "Block %u  %s %lu-%lu",
                ui.cell + 1,
                ui.map.surface ? "sectors" : "clusters",
                (unsigned long)first,
                (unsigned long)(first + count - 1));
        text(2, 19, BRIGHT, s);
        sprintf(s,
                "Disk LBA %lu-%lu  |  %lu KiB  |  Enter: details  +: zoom  -: whole volume",
                (unsigned long)(ui.volume.start + lba),
                (unsigned long)(ui.volume.start + lba + sectors - 1),
                (unsigned long)(sectors / 2));
        text(2, 20, WORK, s);
    }
}

static void draw(void)
{
    unsigned i, percent;
    char s[160];

    for (i = 0; i < 2000; i++)
        canvas[i] = (WORK << 8) | ' ';
    fill(0, 0, 80, ' ', TITLE);
    text(2, 0, TITLE, "\004 MagicDisk");
    text(24, 0, TITLE, "DISK DIAGNOSTICS & RECOVERY");
    text(65, 0, TITLE, ui.disk.write ? "WRITE MODE 0.3" : "READ ONLY  0.3");
    fill(0, 1, 80, ' ', CHROME);
    text(2, 1, CHROME, "Disk    Diagnose    View    Reports    Help");
    text(62, 1, CHROME, "F10 Menu  F1 Help");
    text(2, 3, BRIGHT, ui.opened ? ui.source : "Welcome to MagicDisk");
    if (ui.mounted) {
        sprintf(
            s,
            "Volume %u/%u   FAT%u   %lu %s   %u sectors/cluster   LBA %lu",
            ui.selected + 1,
            ui.count,
            ui.volume.bits,
            (unsigned long)(ui.volume.total < 20480 ? ui.volume.total / 2 : ui.volume.total / 2048),
            ui.volume.total < 20480 ? "KiB" : "MiB",
            ui.volume.spc,
            (unsigned long)ui.volume.start);
        text(2, 4, CYAN, s);
        draw_map();
    } else {
        box(6, 6, 68, 14, " MAGICDISK WORKSPACE ");
        text(10, 9, BRIGHT, "Know your disk. See every region. Review every finding.");
        text(10, 12, CYAN, "Disk > Open image       Inspect a floppy or disk image");
        text(10, 14, CYAN, "Disk > BIOS device      Select a hard disk or floppy");
        text(10, 17, WORK, "F10 opens menus. Marker changes require review.");
    }
    if (ui.busy) {
        percent = ui.total ? (unsigned)((dt_u64)ui.done * 100 / ui.total) : 0;
        if (percent > 100)
            percent = 100;
        text(2, 22, CYAN, ui.phase);
        fill(23, 22, 44, 176, MUTED);
        fill(23, 22, (int)(percent * 44 / 100), 219, CYAN);
        sprintf(s, ui.busy == 2 ? "%3u%% Wait" : "%3u%% Esc", percent);
        text(69, 22, BRIGHT, s);
    } else if (ui.retested) {
        sprintf(s,
                "%s  |  %lu marked  %lu readable  %lu still failing",
                ui.retest.complete ? "RETEST COMPLETE" : "RETEST INCOMPLETE",
                (unsigned long)ui.retest.marked,
                (unsigned long)ui.retest.readable,
                (unsigned long)ui.retest.failed);
        text(2, 22, ui.retest.failed ? AMBER : CYAN, s);
    } else if (ui.scanned) {
        sprintf(s,
                "%s  |  %lu files  %lu directories  %lu issues  %lu lost clusters",
                ui.result.complete ? "CHECK COMPLETE" : "CHECK INCOMPLETE",
                (unsigned long)ui.result.files,
                (unsigned long)ui.result.dirs,
                (unsigned long)ui.result.errors,
                (unsigned long)ui.result.lost_clusters);
        text(2, 22, ui.result.errors ? AMBER : GREEN, s);
    }
    fill(0, 23, 80, ' ', CHROME);
    text(1, 23, CHROME, ui.status);
    fill(0, 24, 80, ' ', TITLE);
    text(1,
         24,
         TITLE,
         ui.busy == 2
             ? "Transaction in progress - cancellation disabled until completion"
             : "F2 Vol F3 Check F4 Surface F5 Map F6 Find F7 Save F8 Bytes F10 Menu Esc Exit");
    flush();
}

static int modal(const char *title, const char **lines, unsigned count)
{
    unsigned i;
    draw();
    box(7, 5, 66, 15, title);
    for (i = 0; i < count && i < 10; i++)
        text(10, 7 + i, i == 0 ? BRIGHT : WORK, lines[i]);
    text(25, 18, FOCUS, "  Enter / Esc to close  ");
    flush();
    while (1) {
        int k = key();
        if (k == 13 || k == 27)
            return k == 13;
    }
}

static int prompt(const char *title, char *out, unsigned capacity)
{
    unsigned n = 0;
    out[0] = 0;
    for (;;) {
        int k;
        draw();
        box(5, 8, 70, 8, title);
        text(8, 10, WORK, "Enter a path or value. Esc cancels.");
        fill(8, 12, 64, ' ', FOCUS);
        text(8, 12, FOCUS, out + (n > 62 ? n - 62 : 0));
        cell(8 + (n > 62 ? 62 : n), 12, '_', FOCUS);
        flush();
        k = key();
        if (k == 27)
            return 0;
        if (k == 13)
            return n != 0;
        if (k == 8 && n)
            out[--n] = 0;
        else if (k >= 32 && k <= 126 && n + 1 < capacity) {
            out[n++] = (char)k;
            out[n] = 0;
        }
    }
}

static int progress(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    clock_t now = clock();
    int changed = strcmp(ui.phase, phase);
    (void)ctx;
    if (kbhit() && key() == 27)
        return 0;
    ui.done = done;
    ui.total = total;
    strncpy(ui.phase, phase, sizeof(ui.phase) - 1);
    if (changed || now - ui.painted >= CLOCKS_PER_SEC / 10 || done == total) {
        ui.painted = now;
        draw();
    }
    return 1;
}

static void write_progress(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    clock_t now = clock();
    int changed = ui.busy != 2 || strcmp(ui.phase, phase);

    (void)ctx;
    /* Consume keys while writing so Esc cannot become an exit after the transaction. */
    while (kbhit())
        key();
    ui.busy = 2;
    ui.done = done;
    ui.total = total;
    strncpy(ui.phase, phase, sizeof(ui.phase) - 1);
    ui.phase[sizeof(ui.phase) - 1] = 0;
    if (!strcmp(phase, "Roll back repair"))
        strcpy(ui.status, "Repair failed. Restoring original metadata; please wait.");
    else if (!strcmp(phase, "Restore metadata"))
        strcpy(ui.status, "Restoring journaled metadata; cancellation is disabled.");
    else if (!strcmp(phase, "Commit repair"))
        strcpy(ui.status, "Writing and verifying reviewed changes; cancellation is disabled.");
    else
        strcpy(ui.status, "Preparing the recovery journal; cancellation is disabled.");
    if (changed || now - ui.painted >= CLOCKS_PER_SEC / 10 || done == total) {
        ui.painted = now;
        draw();
    }
}

static void clear_findings(void)
{
    unsigned i;

    for (i = 0; i < ui.log_count; i++)
        free(ui.log[i].message);
    ui.log_count = ui.omitted = 0;
    ui.preview = 0;
    ui.surfaced = 0;
}

static void finding(void *ctx, const char *code, dt_u32 where, const char *message)
{
    Finding *f;
    (void)ctx;
    if (ui.log_count == LOG_MAX) {
        ui.omitted++;
        return;
    }
    f = &ui.log[ui.log_count];
    f->message = (char *)malloc(strlen(message) + 1);
    if (!f->message) {
        ui.omitted++;
        return;
    }
    strcpy(f->message, message);
    strncpy(f->code, code, sizeof(f->code) - 1);
    ui.log_count++;
    f->where = where;
}

static void allocation(dt_u32 first, dt_u32 count)
{
    int ok;
    ui.busy = 1;
    ui.cell = 0;
    ui.volume.cache_sector = 0xffffffffUL;
    strcpy(ui.status, "Reading allocation table. Esc cancels; pending blocks remain visible.");
    ok = dt_map_build(&ui.volume, first, count, &ui.map, progress, NULL);
    ui.busy = 0;
    strcpy(ui.status,
           ok ? "Allocation view ready. Arrows select blocks; Enter inspects; + zooms."
              : "Map cancelled. Unvisited blocks remain pending.");
}

/* Keep primary mounting strict; only the workspace offers backup inspection. */
static int mount_recovery(DtDisk *disk, dt_u32 start, dt_u32 size, DtVolume *volume, char *err)
{
    return dt_mount(disk, start, size, volume, err) ||
           dt_mount_backup(
               disk, start, size, ui.recovery_source ? ui.recovery_source : 6, volume, err);
}

static void mount_selected(void)
{
    ui.recovery_source = 6;
    ui.scanned = ui.retested = 0;
    clear_findings();
    memset(&ui.map, 0, sizeof(ui.map));
    ui.mounted = mount_recovery(&ui.disk,
                                ui.partitions[ui.selected].start,
                                ui.partitions[ui.selected].size,
                                &ui.volume,
                                ui.status);
    if (ui.mounted)
        allocation(2, ui.volume.clusters);
}

static int release_disk_handle(void)
{
    if (ui.bios == 2)
        return dt_dos_close(&ui.disk);
    if (ui.bios)
        dt_bios_close(&ui.disk);
    else
        dt_image_close(&ui.disk);
    return 1;
}

static int reopen_target(const char *journal, char *err)
{
    if (ui.bios == 2)
        return dt_dos_open(&ui.disk, ui.drive, journal, err);
    return journal ? dt_image_open_write(&ui.disk, ui.source, err)
                   : dt_image_open(&ui.disk, ui.source, err);
}

static void close_disk(void)
{
    if (ui.opened)
        release_disk_handle();
    ui.opened = ui.mounted = ui.scanned = ui.retested = 0;
    clear_findings();
    ui.count = ui.selected = 0;
}

static int open_disk(const char *path, int bios, unsigned drive)
{
    DtDisk disk;
    char err[160];
    int ok;

    if (bios == 2 || ui.bios == 2)
        close_disk();
    if (bios == 2)
        ok = dt_dos_open(&disk, drive, NULL, err);
    else
        ok = bios ? dt_bios_open(&disk, drive, err) : dt_image_open(&disk, path, err);
    if (!ok) {
        strcpy(ui.status, err);
        return 0;
    }
    close_disk();
    ui.disk = disk;
    ui.opened = 1;
    ui.bios = bios;
    ui.drive = drive;
    if (bios == 2)
        sprintf(ui.source,
                "DOS %c:  |  Mounted volume (sector numbers start at zero)",
                'A' + drive - 1);
    else if (bios)
        sprintf(ui.source, "BIOS %02Xh  |  %s", drive, drive < 128 ? "Floppy drive" : "Hard disk");
    else {
        strncpy(ui.source, path, sizeof(ui.source) - 1);
        ui.source[sizeof(ui.source) - 1] = 0;
    }
    /* A valid superfloppy BPB takes precedence over random boot-code bytes. */
    if (dt_mount(&ui.disk, 0, ui.disk.sectors, &ui.volume, err)) {
        ui.count = 1;
        ui.partitions[0].start = 0;
        ui.partitions[0].size = ui.disk.sectors;
        ui.partitions[0].type = 0;
    } else if (!dt_partitions(&ui.disk, ui.partitions, &ui.count, ui.status) || !ui.count) {
        /* Retain the whole-device extent for explicit backup-sector selection. */
        ui.count = 1;
        ui.partitions[0].start = 0;
        ui.partitions[0].size = ui.disk.sectors;
        ui.partitions[0].type = 0;
    }
    mount_selected();
    return 1;
}

static void select_volume(void)
{
    unsigned selected = ui.selected, top = 0, i;
    if (!ui.opened || !ui.count)
        return;
    for (;;) {
        int k;
        char s[100];
        if (selected < top)
            top = selected;
        if (selected >= top + 10)
            top = selected - 9;
        draw();
        box(5, 5, 70, 15, " SELECT VOLUME ");
        text(8, 7, CYAN, " #  Type    Start LBA      Sectors      Filesystem");
        for (i = top; i < ui.count && i < top + 10; i++) {
            DtVolume v;
            char err[160], fs[24];
            DtPartition *p = &ui.partitions[i];
            if (dt_mount(&ui.disk, p->start, p->size, &v, err))
                sprintf(fs, "FAT%u", v.bits);
            else
                strcpy(fs, "Unsupported / invalid");
            sprintf(s,
                    "%2u   %02X  %11lu  %11lu  %s",
                    i + 1,
                    p->type,
                    (unsigned long)p->start,
                    (unsigned long)p->size,
                    fs);
            fill(7, 8 + i - top, 66, ' ', i == selected ? FOCUS : WORK);
            text(8, 8 + i - top, i == selected ? FOCUS : WORK, s);
        }
        flush();
        k = key();
        if (k == 27)
            return;
        if (k == NAV_UP && selected)
            selected--;
        if (k == NAV_DOWN && selected + 1 < ui.count)
            selected++;
        if (k == 13) {
            ui.selected = selected;
            mount_selected();
            return;
        }
    }
}

static void select_device(void)
{
    unsigned drives[DT_BIOS_MAX], count, selected = 0, top = 0, i;
    char err[160], path[16], *end;
    unsigned long drive;

    if (!dt_bios_devices(drives, &count, err)) {
        strcpy(ui.status, err);
        return;
    }
    for (;;) {
        int k, opened;
        char s[80];

        if (selected < top)
            top = selected;
        if (selected >= top + 9)
            top = selected - 8;
        draw();
        box(5, 4, 70, 17, " SELECT BIOS DEVICE ");
        text(8, 6, CYAN, "BIOS   Device");
        for (i = top; i <= count && i < top + 9; i++) {
            unsigned attr = i == selected ? FOCUS : WORK;

            fill(7, 8 + i - top, 66, ' ', attr);
            if (i == count)
                strcpy(s, "Other device... Enter a BIOS number manually");
            else
                sprintf(s,
                        "%02Xh    %s %u",
                        drives[i],
                        drives[i] < 128 ? "Floppy drive" : "Hard disk",
                        drives[i] < 128 ? drives[i] + 1 : drives[i] - 127);
            text(8, 8 + i - top, attr, s);
        }
        text(8, 18, CYAN, "Up/Down select | Enter opens | Esc cancels");
        text(8,
             19,
             MUTED,
             count ? "BIOS-reported devices. Media is checked when opened."
                   : "No devices reported. Try a known BIOS number manually.");
        flush();
        k = key();
        if (k == 27)
            return;
        if (k == NAV_UP && selected)
            selected--;
        if (k == NAV_DOWN && selected < count)
            selected++;
        if (k == NAV_HOME)
            selected = 0;
        if (k == NAV_END)
            selected = count;
        if (k != 13)
            continue;
        if (selected < count) {
            opened = open_disk(NULL, 1, drives[selected]);
        } else {
            if (!prompt(" BIOS DEVICE: 00=A, 01=B, 80=FIRST HARD DISK ", path, sizeof(path)))
                continue;
            drive = strtoul(path, &end, 16);
            if (!*path || *end || drive > 255) {
                strcpy(ui.status, "Enter a BIOS drive number in hex, for example 00 or 80.");
                continue;
            }
            opened = open_disk(NULL, 1, (unsigned)drive);
        }
        if (opened && ui.count > 1)
            select_volume();
        return;
    }
}

static void sector_viewer(dt_u32 sector);

static int finding_sector(const Finding *f, dt_u32 *sector)
{
    if (!strcmp(f->code, "SURFACE_READ") || !strcmp(f->code, "BAD_SECTOR")) {
        *sector = f->where;
    } else if (!strcmp(f->code, "BAD_READABLE") || !strcmp(f->code, "BAD_UNREADABLE")) {
        if (!ui.mounted || f->where < 2 || f->where >= ui.volume.clusters + 2)
            return 0;
        *sector = ui.volume.start + ui.volume.data + (f->where - 2) * ui.volume.spc;
    } else {
        return 0;
    }
    return *sector < ui.disk.sectors;
}

static void findings(void)
{
    unsigned top = 0, selected = 0, i, horizontal = 0;
    for (;;) {
        char s[100];
        int k;
        dt_u32 sector;

        draw();
        box(2,
            4,
            76,
            18,
            ui.surfaced       ? " SURFACE READ FINDINGS "
            : ui.preview == 4 ? " DIRECTORY-ENTRY PREVIEW "
            : ui.preview == 3 ? " COMBINED-REPAIR PREVIEW "
            : ui.preview == 2 ? " DIRECTORY-REPAIR PREVIEW "
            : ui.preview      ? " FILE-REPAIR PREVIEW "
                              : " INSPECTION FINDINGS ");
        sprintf(s,
                "%u stored | %u omitted | Up/Down rows | Left/Right text | Esc closes",
                ui.log_count,
                ui.omitted);
        text(4, 5, CYAN, s);
        if (!ui.log_count && !ui.scanned && !ui.retested && !ui.preview && !ui.surfaced)
            text(5, 9, WORK, "Run Diagnose > Filesystem check to collect findings.");
        else if (!ui.log_count)
            text(5,
                 9,
                 GREEN,
                 ui.surfaced && !ui.surface_complete
                     ? "Surface read incomplete; no failures in the tested sectors."
                 : ui.preview ? "No repairs are needed."
                              : "No issues found by the completed checks.");
        for (i = top; i < ui.log_count && i < top + 6; i++) {
            Finding *f = &ui.log[i];
            if (!strcmp(f->code, "BAD_READABLE"))
                sprintf(
                    s, "Cluster %lu  |  Readable now; marker retained", (unsigned long)f->where);
            else if (!strcmp(f->code, "BAD_UNREADABLE"))
                sprintf(s, "Cluster %lu  |  Still unreadable", (unsigned long)f->where);
            else if (!strcmp(f->code, "BAD_SECTOR") || !strcmp(f->code, "SURFACE_READ"))
                sprintf(s, "Disk sector %lu  |  Read failed", (unsigned long)f->where);
            else
                sprintf(s, "%s  |  Location/count: %lu", f->code, (unsigned long)f->where);
            text(4,
                 7 + (i - top) * 2,
                 i == selected                      ? FOCUS
                 : !strcmp(f->code, "BAD_READABLE") ? GREEN
                                                    : AMBER,
                 s);
            dt_display_utf8(f->message, s, 71, horizontal);
            text(4, 8 + (i - top) * 2, WORK, s);
        }
        text(4,
             20,
             CYAN,
             selected < ui.log_count && finding_sector(&ui.log[selected], &sector)
                 ? "F8: inspect selected sector (read-only)"
                 : "Selected finding has no direct sector-viewer location.");
        flush();
        k = key();
        if (k == 27 || k == 13)
            return;
        if (k == KEY_F8 && selected < ui.log_count && finding_sector(&ui.log[selected], &sector))
            sector_viewer(sector);
        if (k == NAV_UP && selected)
            selected--;
        if (k == NAV_DOWN && selected + 1 < ui.log_count)
            selected++;
        if (k == NAV_LEFT)
            horizontal = horizontal > 16 ? horizontal - 16 : 0;
        if (k == NAV_RIGHT && horizontal < 65520)
            horizontal += 16;
        if (k == NAV_PGUP)
            selected = selected > 6 ? selected - 6 : 0;
        if (k == NAV_PGDN && ui.log_count)
            selected = selected + 6 < ui.log_count ? selected + 6 : ui.log_count - 1;
        if (k == NAV_HOME)
            selected = 0;
        if (k == NAV_END && ui.log_count)
            selected = ui.log_count - 1;
        if (selected < top)
            top = selected;
        if (selected >= top + 6)
            top = selected - 5;
    }
}

static void save_report(void)
{
    FILE *f;
    char path[128];
    unsigned i;
    if (!ui.scanned && !ui.retested && !ui.preview && !ui.surfaced) {
        strcpy(ui.status,
               "Run a check, surface read, retest or repair preview before exporting a report.");
        return;
    }
    if (!prompt(" SAVE REPORT TO A SEPARATE DISK ", path, sizeof(path)))
        return;
    if (ui.bios == 2 && ui.disk.separate_destination &&
        !ui.disk.separate_destination(ui.disk.ctx, path)) {
        strcpy(ui.status, "Save reports on a different volume from the source.");
        return;
    }
    f = dt_file_create(path);
    if (!f) {
        strcpy(ui.status, "Cannot create report.");
        return;
    }
    fprintf(f,
            "MagicDisk 0.3\nDeveloper: Felipe Andrade Allemand Borges\nSource: %s\n"
            "Volume LBA: %lu  FAT%u\n",
            ui.source,
            (unsigned long)ui.volume.start,
            ui.volume.bits);
    if (ui.surfaced) {
        fprintf(f,
                "Surface read: %s\nSectors tested: %lu of %lu  Unreadable: %lu\n"
                "Finding locations are absolute disk LBAs. No sectors were written.\n",
                ui.surface_complete ? "complete" : "INCOMPLETE",
                (unsigned long)ui.surface_tested,
                (unsigned long)ui.volume.total,
                (unsigned long)ui.surface_failed);
    } else if (ui.preview) {
        fprintf(f,
                "%s-repair preview: proposed metadata changes only. No repair applied.\n",
                ui.preview == 4   ? "Directory-entry"
                : ui.preview == 3 ? "Combined"
                : ui.preview == 2 ? "Directory"
                                  : "File");
    } else if (ui.retested) {
        fprintf(f,
                "Bad-cluster retest: %s\nMarked: %lu  Readable: %lu  Failed: %lu\n"
                "Sectors read: %lu\nBad markers were not changed.\n",
                ui.retest.complete ? "complete" : "INCOMPLETE",
                (unsigned long)ui.retest.marked,
                (unsigned long)ui.retest.readable,
                (unsigned long)ui.retest.failed,
                (unsigned long)ui.retest.tested_sectors);
    } else {
        fprintf(f,
                "Inspection: %s\nFiles: %lu  Directories: %lu\n"
                "Free clusters: %lu  Bad clusters: %lu  Lost clusters: %lu  Issues: %lu\n",
                ui.result.complete ? "complete" : "INCOMPLETE",
                (unsigned long)ui.result.files,
                (unsigned long)ui.result.dirs,
                (unsigned long)ui.result.free_clusters,
                (unsigned long)ui.result.bad_clusters,
                (unsigned long)ui.result.lost_clusters,
                (unsigned long)ui.result.errors);
    }
    for (i = 0; i < ui.log_count; i++)
        fprintf(
            f, "%s %lu: %s\n", ui.log[i].code, (unsigned long)ui.log[i].where, ui.log[i].message);
    fprintf(f, "Omitted findings: %u\n", ui.omitted);
    i = ferror(f);
    if (fclose(f))
        i = 1;
    strcpy(ui.status, i ? "Report write failed." : "Report saved.");
}

static dt_u32 selected_sector(void)
{
    dt_u32 first, count;

    if (!ui.mounted)
        return 0;
    dt_map_range(&ui.map, ui.cell, &first, &count);
    if (!count)
        return ui.volume.start;
    return ui.volume.start +
           (ui.map.surface ? first : ui.volume.data + (first - 2) * ui.volume.spc);
}

static void sector_viewer(dt_u32 sector)
{
    unsigned char bytes[512];
    unsigned page = 0, row, column;
    int reload = 1, readable = 0;
    char notice[80] = "";

    if (!ui.opened || !ui.disk.sectors)
        return;
    if (sector >= ui.disk.sectors)
        sector = 0;
    for (;;) {
        char line[80];
        int k;

        if (reload) {
            readable = ui.disk.read(ui.disk.ctx, sector, bytes);
            reload = 0;
        }
        draw();
        fill(0, 1, 80, ' ', CHROME);
        text(2, 1, CHROME, ui.source);
        box(0, 2, 80, 22, " READ-ONLY SECTOR VIEWER ");
        sprintf(line,
                "Disk LBA %lu / %lu  |  Page %u/2  |  512 bytes",
                (unsigned long)sector,
                (unsigned long)(ui.disk.sectors - 1),
                page + 1);
        text(3, 3, BRIGHT, line);
        text(3, 4, CYAN, "Off");
        text(8, 4, CYAN, "Hexadecimal bytes");
        text(58, 4, CYAN, "ASCII");
        if (readable) {
            for (row = 0; row < 16; row++) {
                unsigned offset = page * 256 + row * 16;

                sprintf(line, "%03X  ", offset);
                for (column = 0; column < 16; column++)
                    sprintf(line + 5 + column * 3, "%02X ", bytes[offset + column]);
                line[53] = line[54] = ' ';
                for (column = 0; column < 16; column++) {
                    unsigned value = bytes[offset + column];

                    line[55 + column] = value >= 32 && value <= 126 ? (char)value : '.';
                }
                line[71] = 0;
                text(3, 5 + row, WORK, line);
            }
        } else {
            text(5, 11, RED, "Sector read failed. No bytes are shown. Press R to retry.");
        }
        text(3,
             21,
             *notice ? AMBER : CYAN,
             *notice ? notice : "Left/Right: byte page  PgUp/PgDn: sector  Home/End: disk bounds");
        text(3, 22, CYAN, "G: go to disk LBA (decimal or 0x hex)  R: reread  Esc: return");
        fill(0, 24, 80, ' ', TITLE);
        text(1, 24, TITLE, "Read-only inspection. No editing or disk writes.");
        flush();
        k = key();
        notice[0] = 0;
        if (k == 27)
            return;
        if (k == NAV_LEFT)
            page = 0;
        else if (k == NAV_RIGHT)
            page = 1;
        else if ((k == NAV_PGUP && sector) || (k == NAV_PGDN && sector + 1 < ui.disk.sectors)) {
            sector = k == NAV_PGUP ? sector - 1 : sector + 1;
            page = 0;
            reload = 1;
        } else if (k == NAV_HOME || k == NAV_END) {
            sector = k == NAV_HOME ? 0 : ui.disk.sectors - 1;
            page = 0;
            reload = 1;
        } else if (k == 'r' || k == 'R') {
            reload = 1;
        } else if (k == 'g' || k == 'G') {
            char input[32], *end;
            unsigned long value;
            int base;

            if (!prompt(" GO TO ABSOLUTE DISK LBA ", input, sizeof(input)))
                continue;
            base = !strncmp(input, "0x", 2) || !strncmp(input, "0X", 2) ? 16 : 10;
            errno = 0;
            value = strtoul(input, &end, base);
            if (errno || *end || input[0] == '-' || value >= ui.disk.sectors) {
                strcpy(notice,
                       "Invalid disk LBA; enter a sector within the displayed disk bounds.");
                continue;
            }
            sector = (dt_u32)value;
            page = 0;
            reload = 1;
        }
    }
}

static void details(void)
{
    char a[80], b[80], c[80], d[80];
    const char *lines[7];
    dt_u32 first, count;
    DtMapCell *m = &ui.map.cell[ui.cell];
    dt_map_range(&ui.map, ui.cell, &first, &count);
    if (!count)
        return;
    sprintf(a,
            "Block %u: %s %lu through %lu",
            ui.cell + 1,
            ui.map.surface ? "sectors" : "clusters",
            (unsigned long)first,
            (unsigned long)(first + count - 1));
    sprintf(b, "Processed: %lu of %lu", (unsigned long)m->done, (unsigned long)count);
    sprintf(c,
            "%s: %lu   Free: %lu   Marked bad: %lu",
            ui.map.surface ? "Readable" : "Allocated",
            (unsigned long)m->used,
            (unsigned long)m->free,
            (unsigned long)m->bad);
    sprintf(d, "Read errors / invalid allocation entries: %lu", (unsigned long)m->unreadable);
    lines[0] = a;
    lines[1] = "";
    lines[2] = b;
    lines[3] = c;
    lines[4] = d;
    lines[5] = "";
    lines[6] = "Enter opens the first sector in this block. Esc closes.";
    if (modal(" BLOCK DETAILS ", lines, 7))
        sector_viewer(selected_sector());
}

static void apply_reviewed_plan(DtPlan *plan, const char *journal)
{
    char err[160], reopen_err[160];
    int ok;

    release_disk_handle();
    ui.opened = reopen_target(journal, err);
    if (ui.opened) {
        ui.busy = 1;
        strcpy(ui.status, "Revalidating, saving journal, and committing the repair.");
        ok = dt_plan_apply_ex(plan, journal, progress, NULL, write_progress, NULL, err);
        ui.busy = 0;
        if (!release_disk_handle()) {
            ok = 0;
            strcpy(err, "DOS flush/unlock failed; preserve journal and stop using this volume.");
        }
    } else {
        ok = 0;
    }
    ui.opened = reopen_target(NULL, reopen_err);
    ui.mounted = ui.opened && mount_recovery(&ui.disk,
                                             ui.partitions[ui.selected].start,
                                             ui.partitions[ui.selected].size,
                                             &ui.volume,
                                             reopen_err);
    ui.scanned = ui.retested = 0;
    clear_findings();
    if (ui.mounted)
        allocation(2, ui.volume.clusters);
    if (!ui.mounted)
        strcpy(ui.status, reopen_err);
    else
        strcpy(ui.status, ok ? "Repair verified. Keep the journal for recovery." : err);
}

/* BIOS-only targets remain read-only; mounted DOS volumes use DOS locks. */
static void change_bad_marker(int mark_bad)
{
    char input[128], journal[128], err[160], *end;
    char first_line[80], second_line[80], third_line[80];
    const char *lines[8];
    const char *confirm = mark_bad ? "MARK" : "UNMARK";
    DtPlan *plan;
    unsigned long cluster, disk_sector;
    int ok, relocate = 0;
    dt_u32 allocation_value;

    if (ui.bios == 1) {
        strcpy(ui.status, "Retest is available here; open Disk > DOS volume to apply repairs.");
        return;
    }
    if (!prompt(ui.bios == 2 ? (mark_bad ? " MARK BAD: VOLUME SECTOR (LBA) "
                                         : " UNMARK BAD: VOLUME SECTOR (LBA) ")
                             : (mark_bad ? " MARK BAD: ABSOLUTE DISK SECTOR (LBA) "
                                         : " UNMARK BAD: ABSOLUTE DISK SECTOR (LBA) "),
                input,
                sizeof(input)))
        return;
    errno = 0;
    disk_sector = strtoul(input, &end, 10);
    if (*end || errno || disk_sector > 0xfffffffeUL ||
        disk_sector < ui.volume.start + ui.volume.data ||
        disk_sector - ui.volume.start - ui.volume.data >= ui.volume.clusters * ui.volume.spc) {
        strcpy(
            ui.status,
            "Enter a disk LBA inside this volume's data area. Metadata cannot be marked in FAT.");
        return;
    }
    cluster = (disk_sector - ui.volume.start - ui.volume.data) / ui.volume.spc + 2;
    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    ui.busy = 1;
    strcpy(ui.status, "Checking ownership and FAT copies before preparing a marker change.");
    if (mark_bad && dt_fat(&ui.volume, (dt_u32)cluster, &allocation_value) && allocation_value) {
        relocate = 1;
        confirm = "RELOCATE";
    }
    ok = relocate ? dt_relocate_plan(&ui.volume, (dt_u32)cluster, plan, progress, NULL, err)
                  : dt_bad_plan(&ui.volume, (dt_u32)cluster, mark_bad, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok) {
        strcpy(ui.status, err);
        dt_plan_free(plan);
        return;
    }
    sprintf(
        first_line, "%s cluster %lu: %u sectors will be journaled.", confirm, cluster, plan->count);
    sprintf(second_line,
            "Disk sectors %lu through %lu (%u sectors per cluster).",
            (unsigned long)(ui.volume.start + ui.volume.data + (cluster - 2) * ui.volume.spc),
            (unsigned long)(ui.volume.start + ui.volume.data + (cluster - 1) * ui.volume.spc - 1),
            ui.volume.spc);
    sprintf(third_line, "Type %s in the next dialog to approve this change.", confirm);
    lines[0] = first_line;
    lines[1] = second_line;
    lines[2] = "";
    lines[3] = relocate   ? "File data will move to a free cluster; the source is marked bad."
               : mark_bad ? "The cluster is free and not referenced by any file."
                          : "Every sector in the cluster passed the read retest.";
    lines[4] = relocate ? "File contents, name and size are preserved. Source data is not written."
                        : "Readability now does not guarantee long-term media health.";
    lines[5] = "A new journal will preserve all overwritten sectors.";
    lines[6] = "";
    lines[7] = third_line;
    if (!modal(" REVIEW BAD-CLUSTER CHANGE ", lines, 8)) {
        strcpy(ui.status, "Marker change cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    if (!prompt(" CONFIRM MARKER CHANGE ", input, sizeof(input)) || strcmp(input, confirm) ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Marker change cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void recover_primary_boot(void)
{
    DtVolume candidate;
    DtPlan *plan;
    char input[32], journal[128], err[160], source_line[80], target_line[80], *end;
    const char *lines[6];
    unsigned long source;
    int ok;

    if (ui.bios == 1) {
        strcpy(ui.status, "Primary recovery writes require an image or mounted DOS volume.");
        return;
    }
    if (!prompt(" BACKUP BOOT SECTOR (USUALLY 6) ", input, sizeof(input)))
        return;
    source = strtoul(input, &end, 10);
    if (!*input || *end || source < 1 || source > 65534) {
        strcpy(ui.status, "Backup sector must be numbered from 1 through 65534.");
        return;
    }
    if (!dt_mount_backup(&ui.disk,
                         ui.partitions[ui.selected].start,
                         ui.partitions[ui.selected].size,
                         (unsigned)source,
                         &candidate,
                         err)) {
        strcpy(ui.status, err);
        return;
    }
    ui.recovery_source = (unsigned)source;
    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    ui.busy = 1;
    strcpy(ui.status, "Checking backup geometry against FATs, files and directories.");
    ok = dt_boot_primary_plan(&candidate, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "Primary already matches backup; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(source_line, "Backup source disk LBA: %lu.", (unsigned long)(candidate.start + source));
    sprintf(target_line,
            "Primary target disk LBA: %lu. One 512-byte sector changes.",
            (unsigned long)candidate.start);
    lines[0] = source_line;
    lines[1] = target_line;
    lines[2] = "Backup geometry passed a complete filesystem check.";
    lines[3] = "This copies the boot sector; it does not rebuild boot code.";
    lines[4] = "The journal preserves the damaged primary for undo.";
    lines[5] = "Type RECOVER in the next dialog to approve.";
    if (!modal(" REVIEW PRIMARY BOOT RECOVERY ", lines, 6) ||
        !prompt(" TYPE RECOVER TO RESTORE PRIMARY BOOT ", input, sizeof(input)) ||
        strcmp(input, "RECOVER") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Primary recovery cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_metadata(int backup_boot)
{
    DtPlan *plan;
    char err[160], input[32], journal[128];
    char summary[80], counts[80], sectors[80];
    const char *lines[7];
    int ok;

    if (ui.bios == 1) {
        strcpy(ui.status, "Open Disk > DOS volume to apply repairs to mounted media.");
        return;
    }
    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    ui.busy = 1;
    strcpy(ui.status, "Checking the filesystem before preparing the metadata repair.");
    ok = backup_boot ? dt_boot_backup_plan(&ui.volume, plan, progress, NULL, err)
                     : dt_fsinfo_plan(&ui.volume, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "Metadata is already up to date; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    if (backup_boot) {
        strcpy(summary, "Copy the primary FAT32 boot sector to its backup.");
        sprintf(counts,
                "Source disk LBA: %lu. One 512-byte sector will change.",
                (unsigned long)ui.volume.start);
    } else {
        sprintf(summary, "Rebuild FAT32 FSInfo: %u metadata sectors will change.", plan->count);
        if (dt_dword(plan->patch[0].after + 492) == 0xffffffffUL)
            strcpy(counts, "Free clusters: 0. No next-free hint; the volume is full.");
        else
            sprintf(counts,
                    "Free clusters: %lu. Next-free hint: %lu.",
                    (unsigned long)dt_dword(plan->patch[0].after + 488),
                    (unsigned long)dt_dword(plan->patch[0].after + 492));
    }
    if (plan->count == 2)
        sprintf(sectors,
                "Disk LBAs: %lu and %lu.",
                (unsigned long)(ui.volume.start + plan->patch[0].sector),
                (unsigned long)(ui.volume.start + plan->patch[1].sector));
    else
        sprintf(
            sectors, "Disk LBA: %lu.", (unsigned long)(ui.volume.start + plan->patch[0].sector));
    lines[0] = summary;
    lines[1] = counts;
    lines[2] = sectors;
    lines[3] = "";
    lines[4] = "Allocation and directory ownership checks passed.";
    lines[5] = "A new journal will preserve the original sectors for undo.";
    lines[6] = "Type REBUILD in the next dialog to approve this repair.";
    if (!modal(backup_boot ? " REVIEW BACKUP BOOT REPAIR " : " REVIEW FSINFO REPAIR ", lines, 7) ||
        !prompt(backup_boot ? " TYPE REBUILD TO REPAIR BACKUP BOOT "
                            : " TYPE REBUILD TO REPAIR FSINFO ",
                input,
                sizeof(input)) ||
        strcmp(input, "REBUILD") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Metadata repair cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_fat_copies(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], *end;
    char summary[80], source_line[80];
    const char *lines[7];
    unsigned long source;
    int ok;

    if (ui.bios == 1) {
        strcpy(ui.status, "Open Disk > DOS volume to repair FAT copies on mounted media.");
        return;
    }
    if (!prompt(" SOURCE FAT NUMBER TO KEEP (1, 2, ...) ", input, sizeof(input)))
        return;
    errno = 0;
    source = strtoul(input, &end, 10);
    if (errno || !*input || *end || source < 1 || source > ui.volume.copies) {
        strcpy(ui.status, "Choose an existing FAT copy, numbered from 1.");
        return;
    }
    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    ui.busy = 1;
    strcpy(ui.status, "Checking the chosen FAT against all files and directories.");
    ok = dt_mirror_plan(&ui.volume, (unsigned)source, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "FAT copies and hints already agree; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary, "Rebuild FAT copies: %u metadata sectors will change.", plan->count);
    sprintf(source_line,
            "Keep source FAT %lu of %u. Its file and directory scan completed.",
            source,
            ui.volume.copies);
    lines[0] = summary;
    lines[1] = source_line;
    lines[2] = "Other FAT copies will match this source; FAT32 hints are rebuilt.";
    lines[3] = "A clean scan cannot prove which copy is the newest.";
    lines[4] = "";
    lines[5] = "A new journal preserves the changed sectors for undo.";
    lines[6] = "Type COPY in the next dialog to approve this source.";
    if (!modal(" REVIEW FAT-COPY REPAIR ", lines, 7) ||
        !prompt(" TYPE COPY TO USE THIS SOURCE FAT ", input, sizeof(input)) ||
        strcmp(input, "COPY") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "FAT-copy repair cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

enum {
    CHAIN_FILES,
    CHAIN_DIRECTORIES,
    CHAIN_TREE,
    CHAIN_SOURCE,
    CHAIN_ENTRIES
};

static void repair_chains(int mode)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80], authority[80], *end;
    unsigned long source = 0;
    int tree = mode == CHAIN_TREE || mode == CHAIN_SOURCE;
    const char *lines[9];
    int ok;

    if (ui.bios == 1) {
        strcpy(ui.status, "Open Disk > DOS volume to repair chains on mounted media.");
        return;
    }
    if (mode == CHAIN_SOURCE) {
        if (!prompt(" SOURCE FAT NUMBER TO REPAIR AND KEEP ", input, sizeof(input)))
            return;
        source = strtoul(input, &end, 10);
        if (!*input || *end || source < 1 || source > ui.volume.copies) {
            strcpy(ui.status, "Choose an existing FAT copy number starting at 1.");
            return;
        }
    }
    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    strcpy(ui.status,
           mode == CHAIN_ENTRIES       ? "Preparing dot-entry and directory-size corrections."
           : tree                      ? "Preparing combined file and directory corrections."
           : mode == CHAIN_DIRECTORIES ? "Preparing directory-chain corrections."
                                       : "Preparing file-chain and length corrections.");
    ok = mode == CHAIN_ENTRIES
             ? dt_entry_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err)
         : mode == CHAIN_SOURCE
             ? dt_tree_source_plan_ex(
                   &ui.volume, (unsigned)source, plan, finding, NULL, progress, NULL, err)
         : tree ? dt_tree_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err)
         : mode == CHAIN_DIRECTORIES
             ? dt_directory_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err)
             : dt_chain_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? (mode == CHAIN_ENTRIES ? 4 : tree ? 3 : mode + 1) : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No corrections needed; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Repair %lu %s; %u metadata sectors.",
            (unsigned long)plan->cluster,
            mode == CHAIN_ENTRIES       ? "directory entries"
            : tree                      ? "files/directories"
            : mode == CHAIN_DIRECTORIES ? "directories"
                                        : "files",
            plan->count);
    lines[0] = summary;
    lines[1] = "Keep allocated prefixes; terminate loops and invalid endings.";
    lines[2] = mode == CHAIN_DIRECTORIES
                   ? "Shared tails stay with first owner; keep unique prefixes."
                   : "Cross-links keep the first scanned owner of shared clusters.";
    lines[3] = mode == CHAIN_DIRECTORIES
                   ? "All retained directory entries and file contents stay intact."
                   : "Later files keep only their unique prefix, possibly empty.";
    lines[4] = mode == CHAIN_DIRECTORIES
                   ? "Only the last FAT link in each affected directory changes."
                   : "Lengths may shrink or expand to full cluster boundaries.";
    if (tree) {
        lines[1] = "Keep valid prefixes; correct chain endings and file lengths.";
        lines[2] = "Fully shared directories become EMPTY, independent directories.";
        lines[4] = "Move entries; grow full directories from free unowned clusters.";
        lines[3] = "First owner keeps shared tails; later files may become empty.";
    }
    lines[5] = "Names stay intact. Back up needed data before applying.";
    if (source) {
        sprintf(authority, "FAT %lu is authority; its repaired copy replaces every FAT.", source);
        lines[5] = authority;
    }
    lines[6] = "Esc cancels; F6 lists proposed changes and F7 exports them.";
    lines[7] = tree || mode == CHAIN_ENTRIES
                   ? "Journal keeps changed sectors, including original free space."
                   : "A journal preserves metadata for undo. File data is unchanged.";
    lines[8] = "Type REPAIR in the next dialog to approve these corrections.";
    if (mode == CHAIN_ENTRIES) {
        lines[1] = "Correct dot order/duplicates; rebuild missing dot references.";
        lines[2] = "Set dot attributes to directory and directory sizes to zero.";
        lines[3] = "Move entries; grow full directories from free unowned clusters.";
        lines[4] = "Rebuilt dots have no dates; other timestamps stay intact.";
    }
    if (!modal(mode == CHAIN_ENTRIES       ? " REVIEW DIRECTORY-ENTRY REPAIR "
               : tree                      ? " REVIEW FILESYSTEM TREE REPAIR "
               : mode == CHAIN_DIRECTORIES ? " REVIEW DIRECTORY-CHAIN REPAIR "
                                           : " REVIEW FILE-CHAIN REPAIR ",
               lines,
               9) ||
        !prompt(mode == CHAIN_ENTRIES       ? " TYPE REPAIR TO CORRECT DIRECTORY ENTRIES "
                : tree                      ? " TYPE REPAIR TO CORRECT FILESYSTEM TREE "
                : mode == CHAIN_DIRECTORIES ? " TYPE REPAIR TO CORRECT DIRECTORY CHAINS "
                                            : " TYPE REPAIR TO CORRECT FILE CHAINS ",
                input,
                sizeof(input)) ||
        strcmp(input, "REPAIR") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status,
               mode == CHAIN_ENTRIES ? "Entry repair cancelled; F6 shows proposed changes."
               : tree                ? "Combined repair cancelled; F6 shows proposed changes."
               : mode == CHAIN_DIRECTORIES
                   ? "Directory repair cancelled; F6 shows proposed changes."
                   : "File repair cancelled; F6 shows proposed changes. No writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void reclaim_lost(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[7];
    int ok;

    if (ui.bios == 1) {
        strcpy(ui.status, "Open Disk > DOS volume to reclaim lost allocation on mounted media.");
        return;
    }
    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    ui.busy = 1;
    strcpy(ui.status, "Checking ownership before preparing lost-allocation repair.");
    ok = dt_lost_plan(&ui.volume, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No lost allocation to reclaim; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Reclaim %lu lost clusters; %u metadata sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Recover needed data with Recover lost chains first.";
    lines[2] = "This frees unowned clusters, including cycles and shared tails.";
    lines[3] = "Live files, directories and bad-cluster markers stay allocated.";
    lines[4] = "Future writes can overwrite reclaimed data.";
    lines[5] = "The journal restores metadata, not overwritten file contents.";
    lines[6] = "Type RECLAIM in the next dialog to approve this repair.";
    if (!modal(" REVIEW LOST-ALLOCATION REPAIR ", lines, 7) ||
        !prompt(" TYPE RECLAIM TO FREE LOST ALLOCATION ", input, sizeof(input)) ||
        strcmp(input, "RECLAIM") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Reclaim cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void recover_files(int live)
{
    char destination[128], err[160];
    DtRecovery recovered;
    const char *lines[] = {"Export unowned chains without changing the source FAT.",
                           "Choose an existing directory on a separate volume.",
                           "A new RECxxxxx directory holds .CHK files and RECOVERY.TXT.",
                           "Names and lengths are unknown; full clusters are kept.",
                           "Unreadable sectors become zero-filled gaps in the manifest.",
                           "Cancelled or failed exports keep partial files for review."};
    const char *live_lines[] = {"Export live files before repairing their chains.",
                                "Choose an existing directory on a separate volume.",
                                "A new RECxxxxx directory holds numbered .CHK files.",
                                "RECOVERY.TXT maps files to their original UTF-8 paths.",
                                "Damaged files retain their known allocated prefixes.",
                                "Unreadable sectors become documented zero-filled gaps."};
    const char *result_lines[5];
    char summary[100], gaps[100], location[63];
    int ok;

    if (ui.bios == 1) {
        strcpy(ui.status, "Open Disk > DOS volume so recovery can verify a separate destination.");
        return;
    }
    if (!modal(live ? " EXPORT LIVE FILES " : " RECOVER LOST CHAINS ",
               live ? live_lines : lines,
               sizeof(lines) / sizeof(lines[0])) ||
        !prompt(" RECOVERY DESTINATION DIRECTORY ", destination, sizeof(destination)))
        return;
    ui.busy = 1;
    strcpy(ui.status,
           live ? "Checking allocation before exporting files."
                : "Checking ownership before exporting lost chains.");
    ok = live ? dt_export_files(&ui.volume, destination, &recovered, progress, NULL, err)
              : dt_recover_lost(&ui.volume, destination, &recovered, progress, NULL, err);
    ui.busy = 0;
    sprintf(summary,
            "%lu files; %lu/%lu clusters.",
            (unsigned long)recovered.files,
            (unsigned long)recovered.clusters,
            (unsigned long)recovered.lost);
    sprintf(gaps,
            "%lu unreadable; %lu damaged; %lu scan findings.",
            (unsigned long)recovered.unreadable,
            (unsigned long)recovered.damaged,
            (unsigned long)recovered.issues);
    dt_display_utf8(recovered.directory, location, sizeof(location), 0);
    result_lines[0] = ok ? "Export finished. Source allocation is unchanged."
                         : "Recovery is incomplete; retained files may be partial.";
    result_lines[1] = summary;
    result_lines[2] = gaps;
    result_lines[3] = *recovered.directory ? location : "No recovery directory created.";
    result_lines[4] =
        *recovered.directory ? "See RECOVERY.TXT for cluster provenance and gaps." : "";
    modal(live ? " LIVE-FILE EXPORT RESULTS " : " LOST-CHAIN RECOVERY RESULTS ", result_lines, 5);
    strcpy(ui.status,
           ok ? (recovered.unreadable || recovered.damaged
                     ? "Recovery finished with recorded gaps or damage."
                 : recovered.issues ? "Recovery finished with recorded scan findings."
                                    : "Recovery finished; source allocation unchanged.")
              : err);
}

static void restore_journal(int torn)
{
    char journal[128], confirm[32], err[160], reopen_err[160];
    int ok = 0;

    if (ui.bios == 1) {
        strcpy(ui.status, "Open Disk > DOS volume to restore mounted media.");
        return;
    }
    if (torn) {
        const char *lines[] = {"Restore original metadata after an interrupted write.",
                               "Every current byte must match a saved before or after byte.",
                               "Other changes are refused; ownership checks still apply.",
                               "Matching bytes alone do not prove a write was interrupted.",
                               "Use the original journal only after checking later edits."};

        if (!modal(" RECOVER INTERRUPTED WRITE ", lines, sizeof(lines) / sizeof(lines[0])))
            return;
    }
    if (!prompt(" JOURNAL TO RESTORE ", journal, sizeof(journal)) ||
        !prompt(" TYPE RESTORE TO RESTORE ORIGINAL METADATA ", confirm, sizeof(confirm)) ||
        strcmp(confirm, "RESTORE"))
        return;
    release_disk_handle();
    ui.opened = reopen_target(journal, err);
    if (ui.opened) {
        strcpy(ui.status, "Validating journal and restoring original metadata.");
        ui.busy = 1;
        draw();
        ok = torn ? dt_plan_restore_torn_ex(
                        &ui.volume, journal, progress, NULL, write_progress, NULL, err)
                  : dt_plan_restore_ex(
                        &ui.volume, journal, progress, NULL, write_progress, NULL, err);
        ui.busy = 0;
        if (!release_disk_handle()) {
            ok = 0;
            strcpy(err, "DOS flush/unlock failed; preserve journal and stop using this volume.");
        }
    }
    ui.opened = reopen_target(NULL, reopen_err);
    ui.mounted = ui.opened && mount_recovery(&ui.disk,
                                             ui.partitions[ui.selected].start,
                                             ui.partitions[ui.selected].size,
                                             &ui.volume,
                                             reopen_err);
    ui.scanned = ui.retested = 0;
    clear_findings();
    if (ui.mounted)
        allocation(2, ui.volume.clusters);
    if (!ui.mounted)
        strcpy(ui.status, reopen_err);
    else
        strcpy(ui.status, ok ? "Original metadata restored and verified. Journal retained." : err);
}

/* Menu actions are shared with shortcuts; unavailable actions remain disabled. */
enum {
    OPEN_IMAGE = 1,
    OPEN_BIOS,
    OPEN_DOS,
    VOLUMES,
    QUIT,
    CHECK,
    SURFACE,
    ALLOCATION,
    ZOOM_IN,
    ZOOM_OUT,
    FINDINGS,
    SAVE_REPORT,
    RETEST_BAD,
    MARK_BAD,
    UNMARK_BAD,
    RESTORE_JOURNAL,
    REBUILD_FSINFO,
    REBUILD_BOOT_BACKUP,
    RECOVER_PRIMARY_BOOT,
    REBUILD_FAT,
    RECOVER_LOST,
    EXPORT_FILES,
    RESTORE_TORN,
    RECLAIM_LOST,
    REPAIR_FILES,
    REPAIR_DIRECTORIES,
    REPAIR_TREE,
    REPAIR_TREE_SOURCE,
    REPAIR_ENTRIES,
    SECTOR_VIEW,
    HELP,
    ABOUT
};

typedef struct {
    const char *label;
    int action;
} MenuItem;

static const MenuItem disk_menu[] = {
    {"Open image...", OPEN_IMAGE},
    {"BIOS device...", OPEN_BIOS},
    {"DOS volume...", OPEN_DOS},
    {"Select volume...   F2", VOLUMES},
    {"Exit             Esc", QUIT},
    {NULL, 0},
};
static const MenuItem diagnose_menu[] = {
    {"Filesystem check  F3", CHECK},
    {"Surface read      F4", SURFACE},
    {"Retest marked bad...", RETEST_BAD},
    {"Mark bad sector...", MARK_BAD},
    {"Unmark bad sector...", UNMARK_BAD},
    {"Restore journal...", RESTORE_JOURNAL},
    {"Recover primary boot...", RECOVER_PRIMARY_BOOT},
    {"Repair directory entries...", REPAIR_ENTRIES},
    {"Repair from FAT copy...", REPAIR_TREE_SOURCE},
    {"Repair filesystem tree...", REPAIR_TREE},
    {"Repair directory chains...", REPAIR_DIRECTORIES},
    {"Repair file chains...", REPAIR_FILES},
    {"Reclaim lost allocation...", RECLAIM_LOST},
    {"Recover lost chains...", RECOVER_LOST},
    {"Rebuild FAT copies...", REBUILD_FAT},
    {"Rebuild backup boot...", REBUILD_BOOT_BACKUP},
    {"Rebuild FAT32 FSInfo...", REBUILD_FSINFO},
    {NULL, 0},
};
static const MenuItem view_menu[] = {
    {"Allocation map    F5", ALLOCATION},
    {"Zoom into block    +", ZOOM_IN},
    {"Whole volume       -", ZOOM_OUT},
    {"Findings          F6", FINDINGS},
    {"Sector viewer...  F8", SECTOR_VIEW},
    {NULL, 0},
};
static const MenuItem report_menu[] = {
    {"Save report...    F7", SAVE_REPORT},
    {"Export live files...", EXPORT_FILES},
    {"Recover interrupted write...", RESTORE_TORN},
    {NULL, 0},
};
static const MenuItem help_menu[] = {
    {"Keyboard / guide  F1", HELP},
    {"About MagicDisk...", ABOUT},
    {NULL, 0},
};
static const MenuItem *menus[] = {disk_menu, diagnose_menu, view_menu, report_menu, help_menu};
static const int menu_x[] = {1, 9, 21, 29, 40};

static int enabled(int action)
{
    if (action == SECTOR_VIEW)
        return ui.opened && ui.disk.sectors;
    if (action == RECOVER_PRIMARY_BOOT)
        return ui.opened && ui.count;
    if (ui.mounted && ui.volume.boot_source &&
        (action == MARK_BAD || action == UNMARK_BAD || action == REBUILD_FSINFO ||
         action == REBUILD_BOOT_BACKUP || action == REBUILD_FAT || action == RECLAIM_LOST ||
         action == REPAIR_FILES || action == REPAIR_DIRECTORIES || action == REPAIR_TREE ||
         action == REPAIR_TREE_SOURCE || action == REPAIR_ENTRIES))
        return 0;
    if (action == VOLUMES)
        return ui.opened && ui.count;
    if (action == RESTORE_TORN || action == EXPORT_FILES || action == RECOVER_LOST ||
        action == RECLAIM_LOST || action == REPAIR_FILES || action == REPAIR_DIRECTORIES ||
        action == REPAIR_TREE || action == REPAIR_ENTRIES)
        return ui.mounted;
    if (action == REBUILD_FAT || action == REPAIR_TREE_SOURCE)
        return ui.mounted && ui.volume.mirrored && ui.volume.copies > 1;
    if (action == REBUILD_FSINFO || action == REBUILD_BOOT_BACKUP)
        return ui.mounted && ui.volume.bits == 32;
    if (action >= CHECK && action <= RESTORE_JOURNAL)
        return ui.mounted;
    return 1;
}

static int menu(void)
{
    unsigned tab = 0, selected = 0, i, count;
    for (;;) {
        int k, x = menu_x[tab];
        const MenuItem *items = menus[tab];
        for (count = 0; items[count].label; count++)
            ;
        draw();
        box(x, 2, 26, (int)count + 2, NULL);
        for (i = 0; i < count; i++) {
            unsigned attr = !enabled(items[i].action) ? MUTED : (i == selected ? FOCUS : WORK);
            fill(x + 1, 3 + i, 24, ' ', attr);
            text(x + 2, 3 + i, attr, items[i].label);
        }
        flush();
        k = key();
        if (k == 27 || k == KEY_F10)
            return 0;
        if (k == NAV_LEFT || k == NAV_RIGHT) {
            tab = (tab + (k == NAV_RIGHT ? 1 : 4)) % 5;
            selected = 0;
        }
        if (k == NAV_UP)
            selected = (selected + count - 1) % count;
        if (k == NAV_DOWN)
            selected = (selected + 1) % count;
        if (k == 13 && enabled(items[selected].action))
            return items[selected].action;
    }
}

static int action(int command)
{
    static const char *about[] = {"MagicDisk 0.3 - DOS disk diagnostics",
                                  "",
                                  "Developed by Felipe Andrade Allemand Borges",
                                  "",
                                  "386 protected mode | FAT12 / FAT16 / FAT32",
                                  "Part of the Retrolities project. MIT License.",
                                  "",
                                  "Built with Open Watcom and the DOS/32A extender.",
                                  "Journaled repairs, file recovery and surface diagnostics."};
    static const char *help[] = {"Use F10, then arrows and Enter to navigate the menus.",
                                 "",
                                 "F2 Volumes  F3 Filesystem check  F4 Surface read",
                                 "F5 Allocation map  F6 Findings  F7 Save report  F8 Bytes",
                                 "",
                                 "Map: arrows select; Enter details; + zoom; - reset.",
                                 "Esc cancels an operation or closes a window.",
                                 "",
                                 "Allocation colors describe FAT entries, not file health.",
                                 "Surface read tests readability without writing to disk."};
    char path[128];
    dt_u32 first, count;
    int ok;

    if (!enabled(command)) {
        strcpy(ui.status, "Select a supported FAT volume first.");
        return 1;
    }
    switch (command) {
    case SECTOR_VIEW:
        sector_viewer(selected_sector());
        break;
    case QUIT:
        return 0;
    case OPEN_IMAGE:
        if (prompt(" OPEN DISK IMAGE ", path, sizeof(path)))
            open_disk(path, 0, 0);
        break;
    case OPEN_BIOS:
        select_device();
        break;
    case OPEN_DOS:
        if (prompt(" OPEN DOS VOLUME (DRIVE LETTER) ", path, sizeof(path))) {
            unsigned letter = (unsigned char)path[0];

            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
            if (letter < 'A' || letter > 'Z' || (path[1] && (path[1] != ':' || path[2])))
                strcpy(ui.status, "Enter one DOS drive letter, such as C.");
            else
                open_disk(NULL, 2, letter - 'A' + 1);
        }
        break;
    case VOLUMES:
        select_volume();
        break;
    case CHECK:
        ui.retested = 0;
        clear_findings();
        ui.busy = 1;
        ui.volume.cache_sector = 0xffffffffUL;
        strcpy(ui.status, "Checking filesystem structures. Esc cancels.");
        dt_scan_ex(&ui.volume, &ui.result, finding, NULL, progress, NULL);
        ui.busy = 0;
        ui.scanned = 1;
        strcpy(ui.status,
               ui.result.complete
                   ? "Check finished. F6 opens findings; F7 saves the report."
                   : "Check incomplete. F6 shows why; partial results are not a clean "
                     "bill of health.");
        break;
    case SURFACE:
        clear_findings();
        ui.scanned = ui.retested = 0;
        ui.surface_tested = ui.surface_failed = 0;
        ui.busy = 1;
        ui.cell = 0;
        strcpy(ui.status, "Reading every volume sector, including metadata. Esc cancels.");
        ok = dt_surface_ex(&ui.volume, 0, ui.volume.total, &ui.map, progress, NULL, finding, NULL);
        ui.surfaced = 1;
        ui.surface_complete = ok;
        for (first = 0; first < ui.map.cells; first++) {
            ui.surface_tested += ui.map.cell[first].done;
            ui.surface_failed += ui.map.cell[first].unreadable;
        }
        ui.busy = 0;
        strcpy(ui.status,
               ok ? "Surface read finished. F6 shows failed sectors; F7 saves the report."
                  : "Surface read cancelled. Pending blocks have not been tested.");
        break;
    case ALLOCATION:
    case ZOOM_OUT:
        allocation(2, ui.volume.clusters);
        break;
    case ZOOM_IN:
        dt_map_range(&ui.map, ui.cell, &first, &count);
        if (ui.map.surface) {
            strcpy(ui.status,
                   "Surface results cover the full volume. F5 switches to the zoomable "
                   "allocation map.");
        } else if (count > 1) {
            allocation(first, count);
        } else {
            strcpy(ui.status, "Already at one cluster per block.");
        }
        break;
    case FINDINGS:
        findings();
        break;
    case SAVE_REPORT:
        save_report();
        break;
    case RETEST_BAD:
        clear_findings();
        ui.scanned = 0;
        ui.busy = 1;
        strcpy(ui.status, "Retesting marked bad clusters; their markers remain unchanged.");
        dt_bad_retest(&ui.volume, &ui.retest, finding, NULL, progress, NULL);
        ui.busy = 0;
        ui.retested = 1;
        if (!ui.retest.complete)
            finding(NULL, "RETEST_INCOMPLETE", 0, "Retest cancelled or FAT could not be read");
        strcpy(ui.status,
               "Bad markers retained. F6 shows cluster results; F7 saves the retest report.");
        break;
    case MARK_BAD:
        change_bad_marker(1);
        break;
    case UNMARK_BAD:
        change_bad_marker(0);
        break;
    case REBUILD_FSINFO:
        repair_metadata(0);
        break;
    case RECOVER_PRIMARY_BOOT:
        recover_primary_boot();
        break;
    case REBUILD_BOOT_BACKUP:
        repair_metadata(1);
        break;
    case REPAIR_ENTRIES:
        repair_chains(CHAIN_ENTRIES);
        break;
    case REPAIR_TREE_SOURCE:
        repair_chains(CHAIN_SOURCE);
        break;
    case REPAIR_TREE:
        repair_chains(CHAIN_TREE);
        break;
    case REPAIR_DIRECTORIES:
        repair_chains(CHAIN_DIRECTORIES);
        break;
    case REPAIR_FILES:
        repair_chains(CHAIN_FILES);
        break;
    case RECLAIM_LOST:
        reclaim_lost();
        break;
    case EXPORT_FILES:
        recover_files(1);
        break;
    case RECOVER_LOST:
        recover_files(0);
        break;
    case REBUILD_FAT:
        repair_fat_copies();
        break;
    case RESTORE_TORN:
        restore_journal(1);
        break;
    case RESTORE_JOURNAL:
        restore_journal(0);
        break;
    case HELP:
        modal(" MAGICDISK GUIDE ", help, sizeof(help) / sizeof(help[0]));
        break;
    case ABOUT:
        modal(" ABOUT MAGICDISK ", about, sizeof(about) / sizeof(about[0]));
        break;
    }
    return 1;
}

int dt_ui(const char *path)
{
    int running = 1;
    memset(&ui, 0, sizeof(ui));
    memset(previous, 0, sizeof(previous));
    if (!video_open()) {
        puts("MagicDisk requires an 80-column color text display.");
        return 2;
    }
    strcpy(ui.status, "Ready. F10 opens the menu; Disk selects an image or BIOS device.");
    if (path)
        open_disk(path, 0, 0);
    while (running) {
        int k, command = 0;
        draw();
        k = key();
        if (k == KEY_F10)
            command = menu();
        else if (k == KEY_F1)
            command = HELP;
        else if (k == KEY_F1 + 1)
            command = VOLUMES;
        else if (k == KEY_F1 + 2)
            command = CHECK;
        else if (k == KEY_F1 + 3)
            command = SURFACE;
        else if (k == KEY_F1 + 4)
            command = ALLOCATION;
        else if (k == KEY_F1 + 5)
            command = FINDINGS;
        else if (k == KEY_F1 + 6)
            command = SAVE_REPORT;
        else if (k == KEY_F1 + 7)
            command = SECTOR_VIEW;
        else if (k == 27)
            command = QUIT;
        else if (k == '+' || k == '=')
            command = ZOOM_IN;
        else if (k == '-')
            command = ZOOM_OUT;
        else if (k == 13 && ui.mounted)
            details();
        else if (k == NAV_LEFT && ui.cell)
            ui.cell--;
        else if (k == NAV_RIGHT && ui.cell + 1 < ui.map.cells)
            ui.cell++;
        else if (k == NAV_UP && ui.cell >= 64)
            ui.cell -= 64;
        else if (k == NAV_DOWN && ui.cell + 64 < ui.map.cells)
            ui.cell += 64;
        if (command)
            running = action(command);
    }
    close_disk();
    video_close();
    return 0;
}
#else
int dt_ui(const char *path)
{
    (void)path;
    puts("The full-screen interface is available in MAGICDSK.EXE for DOS.");
    return 2;
}
#endif
