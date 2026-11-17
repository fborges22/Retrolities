/* MagicDisk - 80-column DOS workspace, menus and interactive disk map. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__WATCOMC__) || defined(MAGICDISK_SCRIPTED_UI)
#ifdef __WATCOMC__
#include <conio.h>
#include <i86.h>
#include <dos.h>
#endif

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
    unsigned candidate_boot[128];
    int candidates;
    int opened, bios, mounted, scanned, retested, preview, busy;
    int surfaced, surface_complete;
    dt_u32 surface_tested, surface_failed, surface_first, surface_count;
    DtMap map;
    DtResult result;
    DtRetest retest;
    Finding *log;
    unsigned log_count, log_capacity, omitted;
    int log_full;
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

#ifdef __WATCOMC__
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

#else
/* Scripted host build of the real workspace for automated UI tests.
   MAGICDISK_KEYS names a script with one action per line:
     f1..f10, up, down, left, right, home, end, pgup, pgdn, enter, esc, bs,
     type:TEXT (characters), key:C (one character),
     wait:TEXT (fail with status 3 unless the screen shows TEXT),
     absent:TEXT (fail unless the screen does not show TEXT).
   MAGICDISK_SCREEN receives the final screen; an exhausted script exits 4. */
static FILE *script;
static char pending[256];
static unsigned pending_at, pending_len;

static int video_open(void)
{
    const char *path = getenv("MAGICDISK_KEYS");

    script = path ? fopen(path, "r") : NULL;
    return script != NULL;
}

static void screen_text(char *out)
{
    unsigned i;

    for (i = 0; i < 2000; i++) {
        unsigned ch = canvas[i] & 255;

        out[i + i / 80] = (char)(ch >= 32 && ch < 127 ? ch : ch ? '#' : ' ');
        if (i % 80 == 79)
            out[i + i / 80 + 1] = '\n';
    }
    out[2025] = 0;
}

static void dump_screen(void)
{
    const char *path = getenv("MAGICDISK_SCREEN");
    char screen[2026];
    FILE *file;

    if (!path)
        return;
    screen_text(screen);
    file = fopen(path, "w");
    if (file) {
        fputs(screen, file);
        fclose(file);
    }
}

static void video_close(void)
{
    dump_screen();
    if (script)
        fclose(script);
}

static void flush(void)
{
    memcpy(previous, canvas, sizeof(canvas));
}

static int kbhit(void)
{
    return 0;
}

static void script_fail(const char *why, const char *line)
{
    dump_screen();
    fprintf(stderr, "UI script: %s: %s\n", why, line);
    exit(3);
}

static int key(void)
{
    static const struct {
        const char *name;
        int code;
    } names[] = {{"f1", KEY_F1},          {"f2", KEY_F1 + 1},   {"f3", KEY_F1 + 2},
                 {"f4", KEY_F1 + 3},      {"f5", KEY_F1 + 4},   {"f6", KEY_F1 + 5},
                 {"f7", KEY_F1 + 6},      {"f8", KEY_F1 + 7},   {"f9", KEY_F1 + 8},
                 {"f10", KEY_F10},        {"up", NAV_UP},       {"down", NAV_DOWN},
                 {"left", NAV_LEFT},      {"right", NAV_RIGHT}, {"home", NAV_HOME},
                 {"end", NAV_END},        {"pgup", NAV_PGUP},   {"pgdn", NAV_PGDN},
                 {"enter", 13},           {"esc", 27},          {"bs", 8}};
    char line[256], screen[2026];
    unsigned i;

    for (;;) {
        if (pending_at < pending_len)
            return (unsigned char)pending[pending_at++];
        if (!fgets(line, sizeof(line), script)) {
            dump_screen();
            fputs("UI script exhausted\n", stderr);
            exit(4);
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        if (!strncmp(line, "wait:", 5) || !strncmp(line, "absent:", 7)) {
            int want = line[0] == 'w';

            screen_text(screen);
            if ((strstr(screen, strchr(line, ':') + 1) != NULL) != want)
                script_fail(want ? "missing screen text" : "unexpected screen text", line);
            continue;
        }
        if (!strncmp(line, "type:", 5)) {
            strcpy(pending, line + 5);
            pending_at = 0;
            pending_len = (unsigned)strlen(pending);
            continue;
        }
        if (!strncmp(line, "key:", 4))
            return (unsigned char)line[4];
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(line, names[i].name))
                return names[i].code;
        script_fail("unknown action", line);
    }
}
#endif

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
        char size[32];

        dt_format_sectors(sectors, size, sizeof(size));
        sprintf(s,
                "Block %u  %s %lu-%lu",
                ui.cell + 1,
                ui.map.surface ? "sectors" : "clusters",
                (unsigned long)first,
                (unsigned long)(first + count - 1));
        text(2, 19, BRIGHT, s);
        sprintf(s,
                "Disk LBA %lu-%lu  |  %s",
                (unsigned long)(ui.volume.start + lba),
                (unsigned long)(ui.volume.start + lba + sectors - 1),
                size);
        text(2, 20, WORK, s);
        text(2,
             21,
             CYAN,
             ui.map.surface ? "Enter: details   F5: allocation map   O: owner   T: retest"
                            : "Enter: details   +: zoom   -: whole volume   O: owner   T: retest");
    }
}

static void draw_workspace(void)
{
    unsigned i, percent;
    char s[160];

    for (i = 0; i < 2000; i++)
        canvas[i] = (WORK << 8) | ' ';
    fill(0, 0, 80, ' ', TITLE);
    text(2, 0, TITLE, "\004 MagicDisk");
    text(24, 0, TITLE, "DISK DIAGNOSTICS & RECOVERY");
    text(65, 0, TITLE, !ui.candidates && ui.disk.write ? "WRITE MODE 0.3" : "READ ONLY  0.3");
    fill(0, 1, 80, ' ', CHROME);
    text(2, 1, CHROME, "Disk    Diagnose    View    Reports    Help    Fix");
    text(62, 1, CHROME, "F10 Menu  F1 Help");
    text(2, 3, BRIGHT, ui.opened ? ui.source : "Welcome to MagicDisk");
    if (ui.mounted) {
        char size[32];

        dt_format_sectors(ui.volume.total, size, sizeof(size));
        sprintf(s,
                "Volume %u/%u  FAT%u  %s  %u sec/cluster  LBA %lu",
                ui.selected + 1,
                ui.count,
                ui.volume.bits,
                size,
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
    } else if (ui.surfaced) {
        sprintf(s,
                "%s | %lu/%lu read | %lu failed",
                ui.surface_count == ui.volume.total
                    ? (ui.surface_complete ? "SURFACE COMPLETE" : "SURFACE INCOMPLETE")
                    : (ui.surface_complete ? "RANGE COMPLETE" : "RANGE INCOMPLETE"),
                (unsigned long)ui.surface_tested,
                (unsigned long)ui.surface_count,
                (unsigned long)ui.surface_failed);
        text(2, 22, !ui.surface_complete || ui.surface_failed ? AMBER : GREEN, s);
    } else if (ui.preview) {
        text(2,
             22,
             AMBER,
             ui.preview == 6 ? "SALVAGE PREVIEW: missing sectors become ZERO. No repair applied."
                             : "REPAIR PREVIEW: proposed changes only. No repair applied.");
    } else if (ui.retested) {
        sprintf(s,
                "%s  |  %lu marked  %lu readable  %lu still failing",
                ui.retest.complete ? "RETEST COMPLETE" : "RETEST INCOMPLETE",
                (unsigned long)ui.retest.marked,
                (unsigned long)ui.retest.readable,
                (unsigned long)ui.retest.failed);
        text(2, 22, !ui.retest.complete || ui.retest.failed ? AMBER : CYAN, s);
    } else if (ui.scanned) {
        sprintf(s,
                "%s  |  %lu files  %lu directories  %lu issues  %lu lost clusters",
                ui.result.complete ? "CHECK COMPLETE" : "CHECK INCOMPLETE",
                (unsigned long)ui.result.files,
                (unsigned long)ui.result.dirs,
                (unsigned long)ui.result.errors,
                (unsigned long)ui.result.lost_clusters);
        text(2, 22, !ui.result.complete || ui.result.errors ? AMBER : GREEN, s);
    } else if (ui.mounted) {
        text(2, 22, MUTED, "No current check results. F3 checks files; F4 reads the surface.");
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
}

static void draw(void)
{
    draw_workspace();
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
    free(ui.log);
    ui.log = NULL;
    ui.log_count = ui.log_capacity = ui.omitted = 0;
    ui.log_full = 0;
    ui.preview = 0;
    ui.surfaced = 0;
}

static void finding(void *ctx, const char *code, dt_u32 where, const char *message)
{
    Finding *f;
    (void)ctx;
    if (!ui.log_full && ui.log_count == ui.log_capacity) {
        unsigned capacity = ui.log_capacity ? ui.log_capacity * 2 : 128;
        Finding *grown = NULL;

        if (capacity > ui.log_capacity && (dt_u64)capacity * sizeof(*grown) <= (size_t)-1)
            grown = (Finding *)realloc(ui.log, (size_t)capacity * sizeof(*grown));
        if (!grown)
            ui.log_full = 1;
        else {
            ui.log = grown;
            ui.log_capacity = capacity;
        }
    }
    if (ui.log_full) {
        ui.omitted++;
        return;
    }
    f = &ui.log[ui.log_count];
    f->message = (char *)malloc(strlen(message) + 1);
    if (!f->message) {
        ui.log_full = 1;
        ui.omitted++;
        return;
    }
    strcpy(f->message, message);
    strncpy(f->code, code, sizeof(f->code) - 1);
    f->code[sizeof(f->code) - 1] = 0;
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
    ui.recovery_source = ui.candidates ? ui.candidate_boot[ui.selected] : 6;
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
    if (journal && ui.candidates) {
        strcpy(err, "Discovered volumes are read-only; no partition repair has been approved.");
        return 0;
    }
    if (ui.bios == 2)
        return dt_dos_open(&ui.disk, ui.drive, journal, err);
    if (ui.bios == 1) {
        DtVolume current;

        if (!dt_bios_open(&ui.disk, ui.drive, err))
            return 0;
        if (journal &&
            (!mount_recovery(&ui.disk,
                             ui.partitions[ui.selected].start,
                             ui.partitions[ui.selected].size,
                             &current,
                             err) ||
             !dt_bios_enable_writes(&ui.disk, current.start, current.total, journal, err))) {
            dt_bios_close(&ui.disk);
            return 0;
        }
        return 1;
    }
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
    ui.candidates = 0;
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
        box(5, 5, 70, 15, ui.candidates ? " DISCOVERED VOLUMES (READ ONLY) " : " SELECT VOLUME ");
        text(8, 7, CYAN, " #  Type    Start LBA      Sectors      Filesystem");
        for (i = top; i < ui.count && i < top + 10; i++) {
            DtVolume v;
            char err[160], fs[24];
            DtPartition *p = &ui.partitions[i];
            if (dt_mount(&ui.disk, p->start, p->size, &v, err) ||
                (ui.candidates && ui.candidate_boot[i] &&
                 dt_mount_backup(&ui.disk, p->start, p->size, ui.candidate_boot[i], &v, err)))
                sprintf(fs, "FAT%u%s", v.bits, v.boot_source ? " backup" : "");
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

typedef struct {
    DtPartition partitions[128];
    unsigned boot[128], bits[128], count;
} CandidateList;

static int collect_candidate(void *ctx, const DtVolume *volume)
{
    CandidateList *list = (CandidateList *)ctx;
    unsigned index = list->count;

    if (index == 128)
        return 0;
    list->partitions[index].start = volume->start;
    list->partitions[index].size = volume->total;
    list->partitions[index].type = 0;
    list->boot[index] = volume->boot_source;
    list->bits[index] = volume->bits;
    list->count++;
    return 1;
}

static void search_volumes(void)
{
    static CandidateList list;
    char input[32], err[160], line[100];
    dt_u32 first, count;
    unsigned selected = 0, top = 0, i;
    int complete, k;

    if (!prompt(" VOLUME SEARCH: FIRST DISK LBA ", input, sizeof(input)))
        return;
    if (!dt_parse_u32(input, 10, &first) || first >= ui.disk.sectors) {
        strcpy(ui.status, "Search start is outside the disk.");
        return;
    }
    if (!prompt(" VOLUME SEARCH: SECTOR COUNT (0 = TO END) ", input, sizeof(input)))
        return;
    if (!dt_parse_u32(input, 10, &count) || count > ui.disk.sectors - first) {
        strcpy(ui.status, "Search range is outside the disk.");
        return;
    }
    if (!count)
        count = ui.disk.sectors - first;
    memset(&list, 0, sizeof(list));
    ui.busy = 1;
    strcpy(ui.status, "Searching for FAT boot sectors and matching FAT headers. Esc cancels.");
    complete =
        dt_find_volumes(&ui.disk, first, count, collect_candidate, &list, progress, NULL, err);
    ui.busy = 0;
    if (!list.count) {
        strcpy(ui.status, complete ? "Search complete; no supported FAT candidates found." : err);
        return;
    }
    if (complete)
        sprintf(ui.status,
                "Volume search complete: %u candidates available for inspection.",
                list.count);
    else
        strcpy(ui.status, err);
    for (;;) {
        if (selected < top)
            top = selected;
        if (selected >= top + 10)
            top = selected - 9;
        draw();
        box(3, 4, 74, 19, " FOUND FAT VOLUMES - READ ONLY ");
        sprintf(line,
                "%s: %u candidates. Contents may overlap or be stale.",
                complete ? "Search complete" : "Search incomplete",
                list.count);
        text(5, 6, complete ? CYAN : AMBER, line);
        cell(73, 4, top ? '^' : 196, CYAN);
        cell(73, 22, top + 10 < list.count ? 'v' : 196, CYAN);
        text(5, 7, WORK, "Enter: inspect  Esc: back  Arrows/PgUp/PgDn/Home/End: select");
        text(5, 8, CYAN, " #   FAT      Start LBA        Sectors       Boot source");
        for (i = top; i < list.count && i < top + 10; i++) {
            sprintf(line,
                    "%3u   %2u   %12lu   %12lu   %u",
                    i + 1,
                    list.bits[i],
                    (unsigned long)list.partitions[i].start,
                    (unsigned long)list.partitions[i].size,
                    list.boot[i]);
            fill(5, 9 + i - top, 70, ' ', selected == i ? FOCUS : WORK);
            text(5, 9 + i - top, selected == i ? FOCUS : WORK, line);
        }
        text(5, 20, WORK, "Inspect and export files before considering partition recovery.");
        text(5, 21, WORK, "This search does not change the partition table or volume data.");
        flush();
        k = key();
        if (k == 27) {
            strcpy(ui.status, "Volume search dismissed; current selection unchanged.");
            return;
        }
        if (k == NAV_UP && selected)
            selected--;
        if (k == NAV_DOWN && selected + 1 < list.count)
            selected++;
        if (k == NAV_PGUP)
            selected = selected > 10 ? selected - 10 : 0;
        if (k == NAV_PGDN)
            selected = selected + 10 < list.count ? selected + 10 : list.count - 1;
        if (k == NAV_HOME)
            selected = 0;
        if (k == NAV_END)
            selected = list.count - 1;
        if (k == 13) {
            memcpy(ui.partitions, list.partitions, sizeof(ui.partitions));
            memcpy(ui.candidate_boot, list.boot, sizeof(ui.candidate_boot));
            ui.count = list.count;
            ui.selected = selected;
            ui.candidates = 1;
            mount_selected();
            if (ui.mounted)
                strcpy(ui.status,
                       "Discovered volume: read-only inspection and file export. F2 selects "
                       "candidates.");
            return;
        }
    }
}

static void select_device(void)
{
    unsigned drives[DT_BIOS_MAX], count, selected = 0, top = 0, i;
    char err[160], path[16];
    dt_u32 drive;

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
            if (!dt_parse_u32(path, 16, &drive) || drive > 255) {
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
static void change_bad_marker(int mark_bad, dt_u32 selected);
static void recover_files(int live, dt_u32 cluster);
static int finding_repair(const char *code, int run);

static int finding_sector(const Finding *f, dt_u32 *sector)
{
    if (!strcmp(f->code, "SURFACE_READ") || !strcmp(f->code, "BAD_SECTOR") ||
        !strcmp(f->code, "SALVAGE_GAP")) {
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

/* Findings use absolute LBAs, including when a DOS volume starts at zero.
   Only data clusters can carry FAT bad markers; metadata stays inspect-only. */
static int finding_marker_action(const Finding *finding, dt_u32 *sector)
{
    dt_u32 cluster, value, bad;

    if (!ui.mounted || ui.volume.boot_source || !finding_sector(finding, sector) ||
        *sector < ui.volume.start + ui.volume.data ||
        *sector - ui.volume.start - ui.volume.data >= ui.volume.clusters * ui.volume.spc)
        return 0;
    cluster = 2 + (*sector - ui.volume.start - ui.volume.data) / ui.volume.spc;
    if (!dt_fat(&ui.volume, cluster, &value))
        return 0;
    bad = ui.volume.bits == 12 ? 0xff7 : ui.volume.bits == 16 ? 0xfff7 : 0x0ffffff7UL;
    return value == bad ? -1 : 1;
}

static void findings(void)
{
    unsigned top = 0, selected = 0, i, horizontal = 0;
    for (;;) {
        char s[100];
        int k, marker = 0;
        dt_u32 sector, marker_sector = 0;

        draw();
        box(2,
            4,
            76,
            18,
            ui.surfaced        ? " SURFACE READ FINDINGS "
            : ui.preview == 13 ? " LABEL TEXT REPAIR PREVIEW "
            : ui.preview == 12 ? " EXTRA DOT CLEANUP PREVIEW "
            : ui.preview == 11 ? " ATTRIBUTE REPAIR PREVIEW "
            : ui.preview == 10 ? " NAME CONFLICT REPAIR PREVIEW "
            : ui.preview == 9  ? " VOLUME LABEL REPAIR PREVIEW "
            : ui.preview == 8  ? " DOS ALIAS REPAIR PREVIEW "
            : ui.preview == 7  ? " LONG-NAME CLEANUP PREVIEW "
            : ui.preview == 6  ? " FILE SALVAGE PREVIEW "
            : ui.preview == 5  ? " TIMESTAMP PREVIEW "
            : ui.preview == 4  ? " DIRECTORY-ENTRY PREVIEW "
            : ui.preview == 3  ? " COMBINED-REPAIR PREVIEW "
            : ui.preview == 2  ? " DIRECTORY-REPAIR PREVIEW "
            : ui.preview       ? " FILE-REPAIR PREVIEW "
                               : " INSPECTION FINDINGS ");
        sprintf(s, "Stored: %u  Omitted: %u  Arrows: browse  Esc: close", ui.log_count, ui.omitted);
        text(4, 5, CYAN, s);
        if (!ui.log_count && !ui.scanned && !ui.retested && !ui.preview && !ui.surfaced)
            text(5, 9, WORK, "Run Diagnose > Filesystem check to collect findings.");
        else if (!ui.log_count)
            text(5,
                 9,
                 GREEN,
                 ui.surfaced && !ui.surface_complete
                     ? "Surface read incomplete; no failures in the tested sectors."
                 : ui.preview == 6 ? "No missing sectors observed; relocation is still proposed."
                 : ui.preview      ? "No repairs are needed."
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
        if (selected < ui.log_count)
            marker = finding_marker_action(&ui.log[selected], &marker_sector);
        if (marker)
            text(4, 19, CYAN, "R: find and export the regular file owning this sector");
        else if (selected < ui.log_count && finding_repair(ui.log[selected].code, 0))
            text(4, 19, CYAN, "P: review volume repair for this type of finding");
        text(4,
             20,
             CYAN,
             marker > 0   ? "F8: inspect sector | M: review marking this cluster bad"
             : marker < 0 ? "F8: inspect sector | U: retest and review unmarking this cluster"
             : selected < ui.log_count && finding_sector(&ui.log[selected], &sector)
                 ? "F8: inspect selected sector (read-only)"
                 : "Selected finding has no direct sector-viewer location.");
        flush();
        k = key();
        if (k == 27 || k == 13)
            return;
        if ((k == 'p' || k == 'P') && selected < ui.log_count &&
            finding_repair(ui.log[selected].code, 1))
            return;
        if (marker && (k == 'r' || k == 'R')) {
            recover_files(1,
                          2 + (marker_sector - ui.volume.start - ui.volume.data) / ui.volume.spc);
            return;
        }
        if ((marker > 0 && (k == 'm' || k == 'M')) || (marker < 0 && (k == 'u' || k == 'U'))) {
            change_bad_marker(marker > 0, marker_sector);
            return;
        }
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
    if (ui.bios && ui.disk.separate_destination &&
        !ui.disk.separate_destination(ui.disk.ctx, path)) {
        strcpy(ui.status,
               ui.bios == 1 ? "Save reports on a different physical disk."
                            : "Save reports on a different volume from the source.");
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
    fprintf(f, "OEM code page: %u (0 means unknown)\n", dt_oem_codepage());
    if (ui.surfaced) {
        fprintf(f,
                "Surface read: %s\nSectors tested: %lu of %lu  Unreadable: %lu\n"
                "Finding locations are absolute disk LBAs. No sectors were written.\n",
                ui.surface_complete ? "complete" : "INCOMPLETE",
                (unsigned long)ui.surface_tested,
                (unsigned long)ui.surface_count,
                (unsigned long)ui.surface_failed);
        fprintf(f,
                "Scope: %s\nVolume sectors: %lu-%lu\nDisk LBAs: %lu-%lu\n",
                ui.surface_count == ui.volume.total ? "whole volume" : "selected map block",
                (unsigned long)ui.surface_first,
                (unsigned long)(ui.surface_first + ui.surface_count - 1),
                (unsigned long)(ui.volume.start + ui.surface_first),
                (unsigned long)(ui.volume.start + ui.surface_first + ui.surface_count - 1));
    } else if (ui.preview == 6) {
        fprintf(f,
                "File salvage preview: unreadable sectors will become ZERO in the replacement.\n"
                "Missing bytes are NOT recovered. No repair applied.\n");
    } else if (ui.preview) {
        fprintf(f,
                "%s-repair preview: proposed metadata changes only. No repair applied.\n",
                ui.preview == 5   ? "Timestamp"
                : ui.preview == 4 ? "Directory-entry"
                : ui.preview == 3 ? "Combined"
                : ui.preview == 2 ? "Directory"
                                  : "File");
    } else if (ui.retested) {
        fprintf(f,
                "Bad-cluster retest: %s\nMarked: %lu  Readable: %lu  Failed: %lu\n"
                "Sectors attempted: %lu  Failed reads: %lu\nBad markers were not changed.\n",
                ui.retest.complete ? "complete" : "INCOMPLETE",
                (unsigned long)ui.retest.marked,
                (unsigned long)ui.retest.readable,
                (unsigned long)ui.retest.failed,
                (unsigned long)ui.retest.tested_sectors,
                (unsigned long)ui.retest.failed_sectors);
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
    if (!dt_file_commit(f))
        i = 1;
    if (fclose(f))
        i = 1;
    if (!dt_file_commit_directory(path))
        i = 1;
    strcpy(ui.status,
           i            ? "Report write or commit failed."
           : ui.omitted ? "Report saved with omitted findings; see the omitted count."
                        : "Report saved.");
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

typedef struct {
    dt_u32 target;
    DtFile file;
    char *path;
} OwnerLookup;

static int owner_visit(void *ctx, const DtFile *file)
{
    OwnerLookup *owner = (OwnerLookup *)ctx;
    dt_u32 cluster = file->start, next, index;

    for (index = 0; index < file->clusters; index++) {
        if (!(index & 31) && !progress(NULL, "Locate cluster owner", index, file->clusters))
            return 0;
        if (cluster == owner->target && !owner->path) {
            const char *path = *file->path ? file->path : "/";

            owner->path = (char *)malloc(strlen(path) + 1);
            if (!owner->path)
                return 0;
            strcpy(owner->path, path);
            owner->file = *file;
            owner->file.path = owner->path;
        }
        if (index + 1 < file->clusters) {
            if (!dt_fat(&ui.volume, cluster, &next))
                return 0;
            cluster = next;
        }
    }
    return 1;
}

static void owner_view(dt_u32 sector)
{
    OwnerLookup owner;
    DtResult result;
    char saved_status[160], line[100];
    size_t horizontal = 0;
    unsigned row;
    int complete;

    if (!ui.mounted || sector < ui.volume.start + ui.volume.data ||
        sector - ui.volume.start - ui.volume.data >= ui.volume.clusters * ui.volume.spc) {
        const char *lines[] = {
            "This sector is outside the selected volume's data clusters.",
            "Boot sectors, FATs and fixed root directories have no data-cluster owner."};

        modal(" NOT A DATA CLUSTER ", lines, 2);
        return;
    }
    memset(&owner, 0, sizeof(owner));
    owner.target = 2 + (sector - ui.volume.start - ui.volume.data) / ui.volume.spc;
    strcpy(saved_status, ui.status);
    strcpy(ui.status, "Locating the first catalog owner. Esc cancels the lookup.");
    ui.busy = 1;
    ui.volume.cache_sector = 0xffffffffUL;
    complete = dt_scan_files(&ui.volume, &result, owner_visit, &owner, NULL, NULL, progress, NULL);
    ui.busy = 0;
    strcpy(ui.status, saved_status);
    for (;;) {
        int k;

        draw();
        box(2, 4, 76, 19, " CLUSTER OWNER ");
        sprintf(line,
                "Cluster %lu  |  Selected disk LBA %lu",
                (unsigned long)owner.target,
                (unsigned long)sector);
        text(4, 5, BRIGHT, line);
        if (owner.path) {
            for (row = 0; row < 4; row++) {
                dt_display_utf8(owner.path, line, 71, horizontal + row * 70);
                text(4, 7 + row, CYAN, line);
            }
            sprintf(line,
                    "%s  |  First cluster %lu  |  %lu bytes",
                    owner.file.directory == 2 ? "FAT32 root directory"
                    : owner.file.directory    ? "Directory"
                                              : "Regular file",
                    (unsigned long)owner.file.start,
                    (unsigned long)owner.file.size);
            text(4, 12, WORK, line);
            if (owner.file.directory != 2) {
                sprintf(line,
                        "Directory entry: disk LBA %lu byte %u",
                        (unsigned long)(ui.volume.start + owner.file.sector),
                        owner.file.offset);
                text(4, 14, WORK, line);
            }
            text(4,
                 16,
                 MUTED,
                 "First owner in the scanned allocation prefixes; no sectors written.");
        } else {
            text(4, 8, AMBER, "No owner found in the scanned allocated prefixes.");
        }
        text(4,
             18,
             complete && !result.errors ? GREEN : AMBER,
             !complete       ? "Lookup INCOMPLETE; additional references may be unknown."
             : result.errors ? "Scan findings may affect ownership. Use F3, then F6 to review."
                             : "Catalog complete; no filesystem issues reported.");
        text(4,
             21,
             CYAN,
             owner.path && !owner.file.directory
                 ? "R: export file   Left/Right: scroll path   Home: start   Esc: return"
                 : "Left/Right: scroll path   Home: path start   Esc: return");
        flush();
        k = key();
        if (k == 27 || k == 13)
            break;
        if ((k == 'r' || k == 'R') && owner.path && !owner.file.directory) {
            free(owner.path);
            recover_files(1, owner.target);
            return;
        }
        if (k == NAV_LEFT)
            horizontal = horizontal >= 35 ? horizontal - 35 : 0;
        if (k == NAV_RIGHT && owner.path && horizontal < strlen(owner.path))
            horizontal += 35;
        if (k == NAV_HOME)
            horizontal = 0;
    }
    free(owner.path);
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
        text(3, 22, CYAN, "G: go to disk LBA  R: reread  O: cluster owner  Esc: return");
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
        } else if (k == 'o' || k == 'O') {
            owner_view(sector);
        } else if (k == 'g' || k == 'G') {
            char input[32];
            dt_u32 value;
            int base;

            if (!prompt(" GO TO ABSOLUTE DISK LBA ", input, sizeof(input)))
                continue;
            base = !strncmp(input, "0x", 2) || !strncmp(input, "0X", 2) ? 16 : 10;
            if (!dt_parse_u32(input, (unsigned)base, &value) || value >= ui.disk.sectors) {
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

static int confirm_bios_write(void)
{
    const char *lines[] = {"Use a clean DOS boot without disk caches or disk TSRs.",
                           "The source disk must have no mounted DOS partitions.",
                           "Keep the journal on a different local IDE disk.",
                           "Both disks must support ATA flush on a legacy IDE controller.",
                           "Enter runs the checks. Esc cancels without writing."};

    return ui.bios != 1 || modal(" BIOS REPAIR REQUIREMENTS ", lines, 5);
}

static void apply_reviewed_plan(DtPlan *plan, const char *journal)
{
    char err[160], reopen_err[160];
    int ok;

    if (!confirm_bios_write()) {
        strcpy(ui.status, "Repair cancelled; no writes made.");
        return;
    }
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

/* All repairs share the reviewed plan and backend write preflight. */
static void change_bad_marker(int mark_bad, dt_u32 selected)
{
    char input[128], journal[128], err[160];
    char first_line[80], second_line[80], third_line[80], owner_line[80];
    const char *lines[8];
    const char *confirm = mark_bad ? "MARK" : "UNMARK";
    DtPlan *plan;
    dt_u32 cluster, disk_sector;
    int ok, relocate = 0;
    dt_u32 allocation_value;

    if (ui.candidates) {
        strcpy(ui.status, "Discovered volumes are read-only; markers cannot be changed.");
        return;
    }
    disk_sector = selected;
    if (selected == 0xffffffffUL) {
        if (!prompt(ui.bios == 2 ? (mark_bad ? " MARK BAD: VOLUME SECTOR (LBA) "
                                             : " UNMARK BAD: VOLUME SECTOR (LBA) ")
                                 : (mark_bad ? " MARK BAD: ABSOLUTE DISK SECTOR (LBA) "
                                             : " UNMARK BAD: ABSOLUTE DISK SECTOR (LBA) "),
                    input,
                    sizeof(input)))
            return;
        if (!dt_parse_u32(input, 10, &disk_sector)) {
            strcpy(ui.status, "Enter a valid sector number inside this volume's data area.");
            return;
        }
    }
    if (disk_sector > 0xfffffffeUL || disk_sector < ui.volume.start + ui.volume.data ||
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
    if (!ok && relocate &&
        !strcmp(err, "Source cluster is unreadable; export recoverable file data first")) {
        const char *warning[] = {
            "Some source sectors remained unreadable after three attempts.",
            "Salvage can copy readable bytes and replace missing sectors with ZERO.",
            "This loses the missing bytes in the active file. Export needed data first.",
            "Directories cannot be salvaged this way. The original cluster is not written.",
            "Enter previews salvage; Esc leaves the volume unchanged."};

        if (modal(" UNREADABLE FILE DATA ", warning, 5)) {
            clear_findings();
            ui.busy = 1;
            ok = dt_salvage_plan(&ui.volume, cluster, plan, finding, NULL, progress, NULL, err);
            ui.busy = 0;
            ui.preview = ok ? 6 : 0;
            ui.scanned = ui.retested = 0;
            confirm = "SALVAGE";
        }
    }
    if (!ok) {
        strcpy(ui.status, err);
        dt_plan_free(plan);
        return;
    }
    sprintf(first_line,
            "%s cluster %lu: %u sectors will be journaled.",
            confirm,
            (unsigned long)cluster,
            plan->count);
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
    if (plan->kind == DT_REPAIR_RELOCATE_DIR) {
        lines[3] = "Directory data moves to a free cluster; the source is marked bad.";
        lines[4] = "Directory links and child parent pointers are updated together.";
    }
    if (plan->kind == DT_REPAIR_RELOCATE_ROOT) {
        lines[3] = "Root directory data moves; the source cluster is marked bad.";
        lines[4] = "Moving the root head updates primary and backup boot pointers.";
    }
    if (plan->kind == DT_REPAIR_SALVAGE) {
        unsigned sector, gaps = 0, index;

        strcpy(owner_line, "File identity unavailable; cancel and export before repair.");
        for (index = 0; index < ui.log_count; index++) {
            if (!strcmp(ui.log[index].code, "SALVAGE_FILE")) {
                const char *name = strrchr(ui.log[index].message, '/');

                strcpy(owner_line, "File: ");
                dt_display_utf8(name ? name + 1 : ui.log[index].message, owner_line + 6, 65, 0);
                break;
            }
        }
        if (index == ui.log_count) {
            strcpy(ui.status, "Cannot retain the salvage file identity; no sectors changed.");
            dt_plan_free(plan);
            return;
        }
        lines[2] = owner_line;

        for (sector = 0; sector < ui.volume.spc; sector++) {
            if (plan->gaps[sector / 8] & (1u << (sector % 8)))
                gaps++;
        }
        sprintf(second_line,
                "%u unreadable sectors (%lu bytes) will become ZERO in the file.",
                gaps,
                (unsigned long)gaps * 512);
        lines[3] = "Readable data moves; missing bytes are NOT recovered. Source marked bad.";
        lines[4] = "File name and size stay the same. Original source sectors are not written.";
    }
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
    char input[32], journal[128], err[160], source_line[80], target_line[80];
    const char *lines[6];
    dt_u32 source;
    int ok;

    if (!prompt(" BACKUP BOOT SECTOR (USUALLY 6) ", input, sizeof(input)))
        return;
    if (!dt_parse_u32(input, 10, &source) || source < 1 || source > 65534) {
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
    char input[32], journal[128], err[160];
    char summary[80], source_line[80];
    const char *lines[7];
    dt_u32 source;
    int ok;

    if (!prompt(" SOURCE FAT NUMBER TO KEEP (1, 2, ...) ", input, sizeof(input)))
        return;
    if (!dt_parse_u32(input, 10, &source) || source < 1 || source > ui.volume.copies) {
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
            (unsigned long)source,
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
    char input[32], journal[128], err[160], summary[80], authority[80];
    dt_u32 source = 0;
    int tree = mode == CHAIN_TREE || mode == CHAIN_SOURCE;
    const char *lines[9];
    int ok;

    if (mode == CHAIN_SOURCE) {
        if (!prompt(" SOURCE FAT NUMBER TO REPAIR AND KEEP ", input, sizeof(input)))
            return;
        if (!dt_parse_u32(input, 10, &source) || source < 1 || source > ui.volume.copies) {
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
        sprintf(authority,
                "FAT %lu is authority; its repaired copy replaces every FAT.",
                (unsigned long)source);
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

static void rebuild_fat_headers(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[5];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for header repair.");
        return;
    }
    ui.busy = 1;
    ok = dt_fat_header_plan(&ui.volume, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "FAT headers are valid; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary, "Rebuild reserved FAT entries in %u metadata sectors.", plan->count);
    lines[0] = summary;
    lines[1] = "The boot sector supplies the media descriptor.";
    lines[2] = "Clean-shutdown and I/O-error flags stay intact.";
    lines[3] = "Allocation and directory ownership have been checked.";
    lines[4] = "Type REBUILD to approve; the journal saves original bytes.";
    if (!modal(" REVIEW FAT HEADERS ", lines, 5) ||
        !prompt(" TYPE REBUILD TO REPAIR FAT HEADERS ", input, sizeof(input)) ||
        strcmp(input, "REBUILD") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "FAT header repair cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void clear_timestamps(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[6];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for timestamp repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_timestamp_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 5 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No invalid timestamps; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Clear %lu invalid timestamp fields in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Invalid fields become zero; the original date/time is unknown.";
    lines[2] = "Zero time displays as midnight. Valid fields stay intact.";
    lines[3] = "Names, file contents and allocation stay intact.";
    lines[4] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[5] = "Type CLEAR to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW INVALID TIMESTAMPS ", lines, 6) ||
        !prompt(" TYPE CLEAR TO RESET INVALID FIELDS ", input, sizeof(input)) ||
        strcmp(input, "CLEAR") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Timestamp repair cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void clear_invalid_lfn(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[6];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for long-name cleanup.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_lfn_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 7 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No invalid long names; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Delete %lu invalid LFN records in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Invalid long names are lost; their DOS aliases stay intact.";
    lines[2] = "Valid long names, file contents and allocation stay intact.";
    lines[3] = "Only invalid LFN slots are deleted; no names are guessed.";
    lines[4] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[5] = "Type CLEAR to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW INVALID LONG NAMES ", lines, 6) ||
        !prompt(" TYPE CLEAR TO DELETE INVALID LFN ", input, sizeof(input)) ||
        strcmp(input, "CLEAR") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Long-name cleanup cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_short_names(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[6];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for alias repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_alias_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 8 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No invalid DOS aliases; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Replace %lu invalid DOS aliases in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Invalid DOS aliases become unique R0000001.CHK-style names.";
    lines[2] = "Valid long names, file contents and allocation stay intact.";
    lines[3] = "Valid LFN checksums follow the new aliases; no text is guessed.";
    lines[4] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[5] = "Type RENAME to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW INVALID DOS ALIASES ", lines, 6) ||
        !prompt(" TYPE RENAME TO REPLACE INVALID ALIASES ", input, sizeof(input)) ||
        strcmp(input, "RENAME") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Alias repair cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_labels(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[7];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for label repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_label_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 9 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No extra or misplaced labels; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Remove %lu volume-label records in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Keep the first root label; remove extra and subdirectory labels.";
    lines[2] = "Labels with cluster pointers or file sizes are refused.";
    lines[3] = "Boot label, names, contents and allocation stay intact.";
    lines[4] = "Existing name conflicts need a separate name repair.";
    lines[5] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[6] = "Type LABELS to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW VOLUME LABELS ", lines, 7) ||
        !prompt(" TYPE LABELS TO REMOVE EXTRA LABEL RECORDS ", input, sizeof(input)) ||
        strcmp(input, "LABELS") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Label repair cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void clear_extra_dots(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[6];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for label repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_extra_dot_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 12 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No extra dot entries; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Remove %lu redundant dot records in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Remove root dots and extra dots beyond the leading pair.";
    lines[2] = "Require matching pointers, directory type and zero size.";
    lines[3] = "Keep the valid leading pair, names, contents and allocation.";
    lines[4] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[5] = "Type DOTS to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW EXTRA DOT ENTRIES ", lines, 6) ||
        !prompt(" TYPE DOTS TO REMOVE EXTRA DOT ENTRIES ", input, sizeof(input)) ||
        strcmp(input, "DOTS") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Dot cleanup cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_label_text(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[6];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for label repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_label_text_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 13 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No invalid label text; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Normalize %lu volume labels in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Replace invalid bytes and a leading space with underscores.";
    lines[2] = "Keep valid spaces/OEM bytes; selected OEM policy is journaled.";
    lines[3] = "Synchronize extended boot labels and validated FAT32 backup.";
    lines[4] = "Esc cancels; F6 lists byte changes; F7 exports the preview.";
    lines[5] = "Type TEXT to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW VOLUME LABEL TEXT ", lines, 6) ||
        !prompt(" TYPE TEXT TO REPAIR VOLUME LABEL TEXT ", input, sizeof(input)) ||
        strcmp(input, "TEXT") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Label-text repair cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_conflicts(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[6];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for name-conflict repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_conflict_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 10 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No conflicting or invalid names; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Rename %lu conflicting/invalid entries in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Keep first names; later conflicts receive R0000001.CHK names.";
    lines[2] = "Conflicting long names are replaced; other LFN text stays.";
    lines[3] = "File contents, dates, directory children and allocation stay.";
    lines[4] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[5] = "Type NAMES to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW NAME CONFLICTS ", lines, 6) ||
        !prompt(" TYPE NAMES TO RENAME CONFLICTING ENTRIES ", input, sizeof(input)) ||
        strcmp(input, "NAMES") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Name repair cancelled; F6 shows proposed changes.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

static void repair_attributes(void)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[7];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for attribute repair.");
        return;
    }
    clear_findings();
    ui.scanned = ui.retested = 0;
    ui.busy = 1;
    ok = dt_attribute_plan_ex(&ui.volume, plan, finding, NULL, progress, NULL, err);
    ui.busy = 0;
    ui.preview = ok ? 11 : 0;
    if (!ok || !plan->count) {
        strcpy(ui.status, ok ? "No reserved attribute bits; no writes made." : err);
        dt_plan_free(plan);
        return;
    }
    sprintf(summary,
            "Clear reserved bits in %lu entries in %u sectors.",
            (unsigned long)plan->cluster,
            plan->count);
    lines[0] = summary;
    lines[1] = "Clear only reserved attribute bits 40h and 80h.";
    lines[2] = "Keep defined flags, entry types, names and LFN text.";
    lines[3] = "File contents, dates, directory children and allocation stay.";
    lines[4] = "Other name, label or dot findings need separate repairs.";
    lines[5] = "Esc cancels; F6 lists changes and F7 exports the preview.";
    lines[6] = "Type FLAGS to approve. The journal preserves original bytes.";
    if (!modal(" REVIEW RESERVED ATTRIBUTES ", lines, 7) ||
        !prompt(" TYPE FLAGS TO CLEAR RESERVED ATTRIBUTE BITS ", input, sizeof(input)) ||
        strcmp(input, "FLAGS") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Attribute repair cancelled; F6 shows proposed changes.");
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

/* Data-preserving alternatives to truncation and reclaim. */
static void preserve_data(int lost)
{
    DtPlan *plan;
    char input[32], journal[128], err[160], summary[80];
    const char *lines[7];
    int ok;

    plan = dt_plan_create();
    if (!plan) {
        strcpy(ui.status, "Insufficient memory for repair plan.");
        return;
    }
    ui.busy = 1;
    strcpy(ui.status, lost ? "Checking ownership before adopting lost chains."
                           : "Checking ownership before copying cross-linked clusters.");
    ok = lost ? dt_lost_files_plan(&ui.volume, dt_dos_now(), plan, progress, NULL, err)
              : dt_crosslink_plan(&ui.volume, plan, progress, NULL, err);
    ui.busy = 0;
    if (!ok || !plan->count) {
        strcpy(ui.status,
               !ok    ? err
               : lost ? "No lost chains to convert; no writes made."
                      : "No cross-linked files; no writes made.");
        dt_plan_free(plan);
        return;
    }
    if (lost) {
        sprintf(summary,
                "Adopt lost chains as %lu root FILEnnnn.CHK files.",
                (unsigned long)plan->cluster);
        lines[1] = "Every lost cluster becomes readable through a file.";
        lines[2] = "Lengths are whole clusters; review and rename them later.";
        lines[3] = "Unused root slots are used before deleted entries.";
        lines[4] = "A full FAT32 root gains one zeroed cluster if needed.";
        lines[5] = "The journal restores every changed sector exactly.";
        lines[6] = "Type ADOPT in the next dialog to approve this repair.";
    } else {
        sprintf(summary,
                "Copy shared clusters for %lu cross-linked files.",
                (unsigned long)plan->cluster);
        lines[1] = "The first owner keeps the shared clusters unchanged.";
        lines[2] = "Each later file receives private copies of its tail.";
        lines[3] = "Copies use readable free clusters; names stay intact.";
        lines[4] = "Unreadable shared clusters are refused, never guessed.";
        lines[5] = "The journal restores every changed sector exactly.";
        lines[6] = "Type COPY in the next dialog to approve this repair.";
    }
    lines[0] = summary;
    if (!modal(lost ? " REVIEW LOST-CHAIN ADOPTION " : " REVIEW CROSS-LINK COPIES ", lines, 7) ||
        !prompt(lost ? " TYPE ADOPT TO CREATE .CHK FILES " : " TYPE COPY TO SEPARATE FILES ",
                input,
                sizeof(input)) ||
        strcmp(input, lost ? "ADOPT" : "COPY") ||
        !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
        strcpy(ui.status, "Repair cancelled; no writes made.");
        dt_plan_free(plan);
        return;
    }
    apply_reviewed_plan(plan, journal);
    dt_plan_free(plan);
}

typedef struct {
    char line[8][76];
    unsigned count, more;
} CmosLog;

static void cmos_line(void *ctx, const char *code, dt_u32 where, const char *message)
{
    CmosLog *log = (CmosLog *)ctx;

    (void)code;
    (void)where;
    if (log->count == 8) {
        log->more++;
        return;
    }
    sprintf(log->line[log->count++], "%.58s", message);
}

/* Read-only: CMOS registers and BIOS counts are compared, never written. */
static void cmos_check(void)
{
    unsigned char cmos[128];
    DtCmosHost host;
    CmosLog log;
    char err[160], head[80], more[40];
    const char *lines[10];
    unsigned issues, i, n = 0;

    memset(&log, 0, sizeof(log));
    if (!dt_cmos_read(cmos, &host, err)) {
        strcpy(ui.status, err);
        return;
    }
    issues = dt_cmos_check(cmos, &host, cmos_line, &log);
    sprintf(head, issues ? "%u CMOS setup problems found." : "CMOS setup is consistent.", issues);
    lines[n++] = head;
    for (i = 0; i < log.count && n < 9; i++)
        lines[n++] = log.line[i];
    if (log.more) {
        sprintf(more, "... and %u more", log.more);
        lines[n++] = more;
    }
    if (!issues)
        lines[n++] = "Battery, checksum, clock, drives and memory agree.";
    modal(" CMOS CHECK (READ ONLY) ", lines, n);
    strcpy(ui.status, issues ? "CMOS problems found; correct them in BIOS setup."
                             : "CMOS check passed; nothing was written.");
}

/* Read-only DoubleSpace/DriveSpace CVF check of a host file such as
   H:\DBLSPACE.000; findings reuse the CMOS dialog's line collector. */
static void cvf_check(void)
{
    DtCvfResult cvf;
    CmosLog log;
    char path[128], err[160], head[80], body[80], more[40];
    const char *lines[10];
    unsigned i, n = 0;
    int ok;

    if (!prompt(" CVF FILE (E.G. H:\\DBLSPACE.000) ", path, sizeof(path))) {
        strcpy(ui.status, "Compressed volume check cancelled.");
        return;
    }
    memset(&log, 0, sizeof(log));
    ui.busy = 1;
    ok = dt_cvf_check(path, &cvf, cmos_line, &log, progress, NULL, err);
    ui.busy = 0;
    if (!ok) {
        strcpy(ui.status, err);
        return;
    }
    sprintf(head,
            "%s %s: %lu issues.",
            cvf.drivespace ? "DriveSpace" : "DoubleSpace",
            cvf.complete ? "check complete" : "check PARTIAL",
            (unsigned long)cvf.errors);
    sprintf(body,
            "%lu clusters used; heap %lu/%lu sectors; %lu files.",
            (unsigned long)cvf.used,
            (unsigned long)cvf.heap_used,
            (unsigned long)cvf.heap_sectors,
            (unsigned long)cvf.files);
    lines[n++] = head;
    lines[n++] = body;
    for (i = 0; i < log.count && n < 9; i++)
        lines[n++] = log.line[i];
    if (log.more) {
        sprintf(more, "... and %u more", log.more);
        lines[n++] = more;
    }
    modal(" COMPRESSED VOLUME CHECK ", lines, n);
    strcpy(ui.status, cvf.errors ? "Compressed volume problems found; nothing was written."
                                 : "Compressed volume check finished; nothing was written.");
    if (cvf.errors) {
        DtCvfRepair fix;
        char input[16], journal[128], plan[80];

        if (!dt_cvf_repair(path, 0, NULL, dt_dos_now(), &fix, NULL, NULL, err)) {
            strcpy(ui.status, err);
            return;
        }
        if (!fix.sectors) {
            strcpy(ui.status, "No automatic CVF repair applies; findings remain.");
            return;
        }
        sprintf(plan,
                "%u repairs in %u sectors; %u problems stay unresolved.",
                fix.fixes,
                fix.sectors,
                fix.unresolved);
        n = 0;
        lines[n++] = plan;
        lines[n++] = "Only bookkeeping changes: MDFAT, BitFAT, FAT and entries.";
        lines[n++] = "Shared compressed data is copied byte-for-byte.";
        lines[n++] = "Lost chains become root FILEnnnn.CHK files.";
        lines[n++] = "Unmount the CVF and clear its R/S/H attributes first.";
        lines[n++] = "Type REPAIR, then a NEW journal file on another disk.";
        if (!modal(" REPAIR COMPRESSED VOLUME ", lines, n) ||
            !prompt(" TYPE REPAIR TO FIX THE COMPRESSED VOLUME ", input, sizeof(input)) ||
            strcmp(input, "REPAIR") ||
            !prompt(" NEW JOURNAL FILE ON A SEPARATE DISK ", journal, sizeof(journal))) {
            strcpy(ui.status, "Compressed volume repair cancelled; nothing was written.");
            return;
        }
        ui.busy = 1;
        ok = dt_cvf_repair(path, 1, journal, dt_dos_now(), &fix, NULL, NULL, err);
        ui.busy = 0;
        strcpy(ui.status, ok ? "Compressed volume repaired and verified. Keep the journal."
                             : err);
    }
}

typedef struct {
    char line[8][64];
    unsigned count, more;
} AutofixLog;

static void autofix_log(void *ctx, const char *step, int outcome, const DtPlan *plan,
                        const char *detail)
{
    AutofixLog *log = (AutofixLog *)ctx;
    char *line;

    (void)detail;
    if (outcome == DT_AUTOFIX_REFUSED)
        return;
    if (log->count == 8) {
        log->more++;
        return;
    }
    line = log->line[log->count++];
    if (outcome == DT_AUTOFIX_FAILED)
        sprintf(line, "FAILED  %.40s", step);
    else
        sprintf(line,
                "%s %.34s: %lu items",
                outcome == DT_AUTOFIX_APPLIED ? "Fixed  " : "Pending",
                step,
                (unsigned long)plan->cluster);
}

static void automatic_repair(int surface)
{
    DtAutofixOptions options;
    DtAutofix result;
    AutofixLog log;
    char input[16], directory[128], err[160], reopen_err[160], head[128], tail[128], extra[64];
    const char *lines[10];
    unsigned i, n;
    int ok;

    if (ui.candidates || ui.volume.boot_source) {
        strcpy(ui.status, "Automatic repair needs a normally mounted partition.");
        return;
    }
    memset(&options, 0, sizeof(options));
    memset(&log, 0, sizeof(log));
    options.surface = surface;
    options.stamp = dt_dos_now();
    ui.busy = 1;
    strcpy(ui.status, surface ? "Planning repairs and reading every sector; Esc cancels."
                              : "Planning every applicable repair; Esc cancels.");
    ok = dt_autofix(&ui.volume, &options, &result, autofix_log, &log, NULL, NULL, progress, NULL,
                    NULL, NULL, err);
    ui.busy = 0;
    if (!ok) {
        strcpy(ui.status, err);
        return;
    }
    if (!result.planned && !result.surface_failed) {
        strcpy(ui.status, result.initial.errors
                              ? "No automatic repair applies; F6 lists remaining findings."
                              : "No problems found; no writes made.");
        return;
    }
    sprintf(head,
            "%lu issues, %lu lost clusters, %lu unreadable sectors.",
            (unsigned long)result.initial.errors,
            (unsigned long)result.initial.lost_clusters,
            (unsigned long)result.surface_failed);
    n = 0;
    lines[n++] = head;
    for (i = 0; i < log.count && n < 7; i++)
        lines[n++] = log.line[i];
    if (log.more || i < log.count) {
        sprintf(extra, "... and %u more steps", log.more + log.count - i);
        lines[n++] = extra;
    }
    lines[n++] = "Steps are re-planned after each verified commit.";
    lines[n++] = "Type FIX, then an existing journal DIRECTORY elsewhere.";
    if (!modal(" REVIEW AUTOMATIC REPAIR ", lines, n) ||
        !prompt(" TYPE FIX TO REPAIR EVERYTHING ", input, sizeof(input)) || strcmp(input, "FIX") ||
        !prompt(" JOURNAL DIRECTORY ON A SEPARATE DISK ", directory, sizeof(directory)) ||
        !confirm_bios_write()) {
        strcpy(ui.status, "Automatic repair cancelled; no writes made.");
        return;
    }
    memset(&log, 0, sizeof(log));
    options.apply = 1;
    options.journal_dir = directory;
    release_disk_handle();
    ui.opened = reopen_target(directory, err);
    if (ui.opened) {
        ui.busy = 1;
        strcpy(ui.status, "Repairing: each step is journaled, written and verified.");
        ok = dt_autofix(&ui.volume, &options, &result, autofix_log, &log, NULL, NULL, progress,
                        NULL, write_progress, NULL, err);
        ui.busy = 0;
        if (!release_disk_handle()) {
            ok = 0;
            strcpy(err, "DOS flush/unlock failed; preserve journals and stop using this volume.");
        }
    } else
        ok = 0;
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
    if (!ui.mounted) {
        strcpy(ui.status, reopen_err);
        return;
    }
    if (!ok) {
        strcpy(ui.status, err);
        if (!result.applied)
            return;
    }
    sprintf(head, "Committed %u repairs in %u passes.", result.applied, result.passes);
    sprintf(tail,
            "Now: %lu issues, %lu lost, %lu bad clusters, %lu not retired.",
            (unsigned long)result.final.errors,
            (unsigned long)result.final.lost_clusters,
            (unsigned long)result.final.bad_clusters,
            (unsigned long)result.unresolved_bad);
    n = 0;
    lines[n++] = head;
    lines[n++] = tail;
    for (i = 0; i < log.count && n < 8; i++)
        lines[n++] = log.line[i];
    lines[n++] = "Journals MDFXnnnn.MDJ: restore newest first to undo.";
    modal(ok ? " AUTOMATIC REPAIR RESULTS " : " AUTOMATIC REPAIR STOPPED ", lines, n);
    if (ok)
        strcpy(ui.status,
               result.final.errors || result.unresolved_bad
                   ? "Automatic repair finished; F3 and F6 show what still needs attention."
                   : "Automatic repair verified: no remaining issues. Keep the journals.");
}

static void recover_files(int live, dt_u32 cluster)
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
    const char *deleted_lines[] = {"Export deleted-file candidates without changing the source.",
                                   "Choose an existing directory on a separate volume.",
                                   "All candidates are UNVERIFIED; old data may be overwritten.",
                                   "Cleared FAT links require a CONTIGUOUS GUESS.",
                                   "Live-owned clusters are excluded; missing data is reported.",
                                   "Raw deleted names/LFN slots are kept in RECOVERY.TXT."};
    const char *result_lines[5];
    char summary[100], gaps[100], location[63];
    int ok;

    if (live == 3) {
        deleted_lines[0] = "Find unowned directories with intact dot entries.";
        deleted_lines[3] = "Export surviving contents; directories are not reattached.";
        deleted_lines[5] = "Directory evidence and file names are kept in RECOVERY.TXT.";
    }
    if (cluster)
        live_lines[0] = "Export the regular file owning the selected cluster.";
    if (ui.bios == 1) {
        lines[1] = "Choose an existing directory on a different physical disk.";
        live_lines[1] = lines[1];
        deleted_lines[1] = lines[1];
    }
    if (!modal(live == 3   ? " FIND ORPHAN DIRECTORIES "
               : live >= 2 ? " RECOVER DELETED CANDIDATES "
               : live      ? " EXPORT LIVE FILES "
                           : " RECOVER LOST CHAINS ",
               live >= 2 ? deleted_lines
               : live    ? live_lines
                         : lines,
               sizeof(lines) / sizeof(lines[0])) ||
        !prompt(" RECOVERY DESTINATION DIRECTORY ", destination, sizeof(destination)))
        return;
    ui.busy = 1;
    strcpy(ui.status,
           live ? "Checking allocation before exporting files."
                : "Checking ownership before exporting lost chains.");
    ok = live == 3
             ? dt_recover_directories(&ui.volume, destination, &recovered, progress, NULL, err)
         : live >= 2 ? dt_recover_deleted(&ui.volume, destination, &recovered, progress, NULL, err)
         : live
             ? (cluster ? dt_export_cluster_file(
                              &ui.volume, cluster, destination, &recovered, progress, NULL, err)
                        : dt_export_files(&ui.volume, destination, &recovered, progress, NULL, err))
             : dt_recover_lost(&ui.volume, destination, &recovered, progress, NULL, err);
    ui.busy = 0;
    sprintf(summary,
            "%lu files; %lu/%lu clusters.",
            (unsigned long)recovered.files,
            (unsigned long)recovered.clusters,
            (unsigned long)recovered.lost);
    if (live >= 2)
        sprintf(summary,
                "%lu unverified candidates; %lu files; %lu clusters.",
                (unsigned long)recovered.candidates,
                (unsigned long)recovered.files,
                (unsigned long)recovered.clusters);
    sprintf(gaps,
            "%lu unreadable; %lu damaged; %lu scan findings.",
            (unsigned long)recovered.unreadable,
            (unsigned long)recovered.damaged,
            (unsigned long)recovered.issues);
    dt_display_utf8(recovered.directory, location, sizeof(location), 0);
    result_lines[0] = recovered.catalog_incomplete
                          ? "Directory catalog INCOMPLETE: files or owners may be missing."
                      : ok ? "Export finished. Source allocation is unchanged."
                           : "Recovery is incomplete; retained files may be partial.";
    if (recovered.catalog_incomplete)
        sprintf(gaps,
                "%lu unreadable payload sectors; %lu scan findings. See directory gaps.",
                (unsigned long)recovered.unreadable,
                (unsigned long)recovered.issues);
    result_lines[1] = summary;
    result_lines[2] = gaps;
    result_lines[3] = *recovered.directory ? location : "No recovery directory created.";
    result_lines[4] =
        *recovered.directory ? "See RECOVERY.TXT for cluster provenance and gaps." : "";
    modal(live == 3   ? " ORPHAN-DIRECTORY RESULTS "
          : live >= 2 ? " DELETED-CANDIDATE RESULTS "
          : live      ? " LIVE-FILE EXPORT RESULTS "
                      : " LOST-CHAIN RECOVERY RESULTS ",
          result_lines,
          5);
    strcpy(ui.status,
           ok ? (live == 3   ? "Unverified candidates exported; verify contents using the manifest."
                 : live == 2 ? "Deleted candidates exported; verify contents using the manifest."
                 : recovered.unreadable || recovered.damaged
                     ? "Recovery finished with recorded gaps or damage."
                 : recovered.issues ? "Recovery finished with recorded scan findings."
                                    : "Recovery finished; source allocation unchanged.")
              : err);
}

static void restore_journal(int torn)
{
    char journal[128], confirm[32], err[160], reopen_err[160];
    int ok = 0;

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
    if (!confirm_bios_write()) {
        strcpy(ui.status, "Repair cancelled; no writes made.");
        return;
    }
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

static int mbr_number(const char *title, dt_u32 *value)
{
    char input[32];

    if (!prompt(title, input, sizeof(input)))
        return 0;
    if (!dt_parse_u32(input, 10, value)) {
        strcpy(ui.status, "Enter a decimal sector or geometry value.");
        return 0;
    }
    return 1;
}

static void partition_transaction(int restore, int torn, int ebr)
{
    DtMbrPlan plan;
    DtEbrPlan logical;
    DtDisk target;
    dt_u32 slot, start, source, heads, sectors;
    char journal[128], confirm[32], path[128], err[160], title[80];
    char extent[96], geometry[96], hidden[96];
    const char *kind = ebr ? "EBR" : "MBR";
    const char *lines[6];
    int ok, opened;

    if (!restore) {
        if (!mbr_number(ebr ? " EBR: RECORD DISK LBA " : " MBR: EMPTY SLOT (1..4) ", &slot))
            return;
        sprintf(title, " %s: CANDIDATE START LBA ", kind);
        if (!mbr_number(title, &start))
            return;
        sprintf(title, " %s: BOOT SOURCE (0 = PRIMARY) ", kind);
        if (!mbr_number(title, &source))
            return;
        sprintf(title, " %s: BIOS GEOMETRY HEADS ", kind);
        if (!mbr_number(title, &heads))
            return;
        sprintf(title, " %s: BIOS SECTORS PER TRACK ", kind);
        if (!mbr_number(title, &sectors))
            return;
        if (ebr) {
            ok = dt_ebr_plan(&ui.disk, slot, start, source, heads, sectors, &logical, err);
            if (ok)
                plan = logical.entry;
        } else {
            ok = dt_mbr_plan(&ui.disk, slot, start, source, heads, sectors, &plan, err);
        }
        if (!ok) {
            strcpy(ui.status, err);
            return;
        }
        if (ebr)
            sprintf(extent,
                    "EBR %lu: FAT%u, LBA %lu, %lu sectors",
                    (unsigned long)logical.sector,
                    plan.bits,
                    (unsigned long)plan.start,
                    (unsigned long)plan.total);
        else
            sprintf(extent,
                    "Slot %u: FAT%u, LBA %lu, %lu sectors, inactive",
                    plan.slot,
                    plan.bits,
                    (unsigned long)plan.start,
                    (unsigned long)plan.total);
        sprintf(geometry,
                "CHS %u heads / %u sectors; boot source %u",
                plan.heads,
                plan.sectors,
                plan.boot_source);
        if (ebr)
            sprintf(hidden,
                    "BPB hidden %lu; EBR relative %lu (unchanged)",
                    (unsigned long)dt_dword(plan.volume_boot + 28),
                    (unsigned long)(plan.start - logical.sector));
        else {
            sprintf(hidden,
                    "BPB hidden sectors %lu%s",
                    (unsigned long)dt_dword(plan.volume_boot + 28),
                    dt_dword(plan.volume_boot + 28) == plan.start ? "" : " MISMATCH (unchanged)");
        }
        lines[0] = extent;
        lines[1] = geometry;
        lines[2] = hidden;
        lines[3] = ebr ? "Existing chain links, MBR and bootstrap are preserved."
                       : "Bootstrap and occupied entries are preserved.";
        lines[4] = "Inspect candidate files before reconstructing the table.";
        lines[5] = "This does not repair boot code or boot-sector geometry.";
        sprintf(title, " REVIEW %s RECONSTRUCTION ", kind);
        if (!modal(title, lines, 6))
            return;
    } else {
        sprintf(extent, "Restore only a MagicDisk %s reconstruction journal.", kind);
        lines[0] = extent;
        lines[1] = "Candidate identity and original insertion are revalidated.";
        lines[2] = torn ? "Allows only saved before/after byte mixtures."
                        : "Current record must match a saved before/after sector.";
        lines[3] = "Later unrelated changes are refused. Journal is retained.";
        sprintf(title, " REVIEW %s%s UNDO ", torn ? "INTERRUPTED " : "", kind);
        if (!modal(title, lines, 4))
            return;
    }
    sprintf(title, restore ? " %s JOURNAL TO RESTORE " : " NEW %s JOURNAL PATH ", kind);
    if (!prompt(title, journal, sizeof(journal)))
        return;
    if (restore && ebr) {
        if (!dt_ebr_review_journal(&ui.disk, journal, &logical, err)) {
            strcpy(ui.status, err);
            return;
        }
        sprintf(
            extent, "Undo logical partition at EBR disk LBA %lu", (unsigned long)logical.sector);
        lines[0] = extent;
        lines[1] = "Only this extended record is eligible for writing.";
        if (!modal(" REVIEW EBR JOURNAL TARGET ", lines, 2))
            return;
    }
    sprintf(title, restore ? " TYPE RESTORE TO UNDO %s " : " TYPE %s TO RECONSTRUCT TABLE ", kind);
    if (!prompt(title, confirm, sizeof(confirm)) || strcmp(confirm, restore ? "RESTORE" : kind))
        return;
    if (!confirm_bios_write())
        return;
    strcpy(path, ui.source);
    release_disk_handle();
    ui.opened = ui.mounted = 0;
    opened =
        ui.bios ? dt_bios_open(&target, ui.drive, err) : dt_image_open_write(&target, path, err);
    ok = opened;
    if (ok && ui.bios)
        ok = ebr ? dt_bios_enable_writes(&target, logical.sector, 1, journal, err)
                 : dt_bios_enable_mbr_write(&target, journal, err);
    if (ok) {
        if (ebr) {
            logical.entry.disk = &target;
            ok = restore ? dt_ebr_restore(&target, journal, torn, err)
                         : dt_ebr_apply(&logical, journal, err);
        } else {
            plan.disk = &target;
            ok = restore ? dt_mbr_restore(&target, journal, torn, err)
                         : dt_mbr_apply(&plan, journal, err);
        }
    }
    if (opened) {
        if (ui.bios)
            dt_bios_close(&target);
        else
            dt_image_close(&target);
    }
    open_disk(path, ui.bios, ui.drive);
    if (ok)
        sprintf(ui.status,
                "%s %s and verified. Journal retained.",
                kind,
                restore ? "restored" : "reconstructed");
    else
        strcpy(ui.status, err);
}

static int review_link(const DtEbrLinkPlan *plan, int restore)
{
    unsigned top = 0, i;
    char line[100];
    int k;

    for (;;) {
        draw();
        box(5, 3, 70, 19, restore ? " REVIEW EBR LINK UNDO " : " REVIEW EBR CHAIN LINK ");
        sprintf(line,
                "Source EBR %lu -> target EBR %lu; %u volumes",
                (unsigned long)plan->sector,
                (unsigned long)plan->target,
                plan->recovered);
        text(8, 5, BRIGHT, line);
        sprintf(line,
                "Container %lu + %lu sectors; CHS %u/%u",
                (unsigned long)plan->base,
                (unsigned long)plan->span,
                plan->heads,
                plan->sectors);
        text(8, 6, CYAN, line);
        text(8, 7, CYAN, "EBR disk LBA   Volume disk LBA       Sectors    Type");
        for (i = 0; i < 9 && top + i < plan->witness_count; i++) {
            const DtLinkWitness *witness = &plan->witness[top + i];

            sprintf(line,
                    "%12lu     %12lu  %12lu      %02X",
                    (unsigned long)witness->record_sector,
                    (unsigned long)witness->volume_start,
                    (unsigned long)dt_dword(witness->record + 458),
                    witness->record[450]);
            text(8, 8 + i, WORK, line);
        }
        sprintf(line,
                "Records %u-%u of %u; PgUp/PgDn or arrows scroll",
                top + 1,
                top + 9 < plan->witness_count ? top + 9 : plan->witness_count,
                plan->witness_count);
        text(8, 18, CYAN, line);
        text(8, 19, BRIGHT, "Only the source link changes. Inspect recovered files first.");
        text(8, 20, FOCUS, " Enter: accept review   Esc: cancel   Home/End: first/last ");
        flush();
        k = key();
        if (k == 13 || k == 27)
            return k == 13;
        if (k == NAV_HOME)
            top = 0;
        else if (k == NAV_END)
            top = plan->witness_count > 9 ? plan->witness_count - 9 : 0;
        else if ((k == NAV_DOWN || k == NAV_PGDN) && top + 9 < plan->witness_count) {
            top += k == NAV_PGDN ? 9 : 1;
            if (top + 9 > plan->witness_count)
                top = plan->witness_count - 9;
        } else if (k == NAV_UP || k == NAV_PGUP) {
            unsigned step = k == NAV_PGUP ? 9 : 1;

            top = top > step ? top - step : 0;
        }
    }
}

static void link_transaction(int restore, int torn)
{
    DtEbrLinkPlan plan;
    DtDisk target;
    dt_u32 from, to, heads, sectors;
    char journal[128], confirm[32], path[128], err[160];
    int ok, opened;

    memset(&plan, 0, sizeof(plan));
    if (restore) {
        if (torn) {
            const char *lines[] = {"Undo a link after an interrupted write.",
                                   "Only saved before/after byte mixtures are accepted.",
                                   "Every recovered record and boot sector is revalidated."};

            if (!modal(" INTERRUPTED EBR LINK UNDO ", lines, 3))
                return;
        }
        if (!prompt(" LINK JOURNAL TO RESTORE ", journal, sizeof(journal)))
            return;
        ok = dt_ebr_link_review_journal(&ui.disk, journal, &plan, err);
    } else {
        if (!mbr_number(" LINK: SOURCE EBR DISK LBA ", &from) ||
            !mbr_number(" LINK: TARGET EBR DISK LBA ", &to) ||
            !mbr_number(" LINK: BIOS GEOMETRY HEADS ", &heads) ||
            !mbr_number(" LINK: BIOS SECTORS PER TRACK ", &sectors))
            return;
        ok = dt_ebr_link_plan(&ui.disk, from, to, heads, sectors, &plan, err);
    }
    if (!ok) {
        strcpy(ui.status, err);
        goto done;
    }
    if (!review_link(&plan, restore))
        goto done;
    if (!restore && !prompt(" NEW LINK JOURNAL PATH ", journal, sizeof(journal)))
        goto done;
    if (!prompt(restore ? " TYPE RESTORE TO UNDO LINK " : " TYPE LINK TO RECONNECT CHAIN ",
                confirm,
                sizeof(confirm)) ||
        strcmp(confirm, restore ? "RESTORE" : "LINK"))
        goto done;
    if (!confirm_bios_write())
        goto done;
    strcpy(path, ui.source);
    release_disk_handle();
    ui.opened = ui.mounted = 0;
    opened =
        ui.bios ? dt_bios_open(&target, ui.drive, err) : dt_image_open_write(&target, path, err);
    ok = opened;
    if (ok && ui.bios)
        ok = dt_bios_enable_writes(&target, plan.sector, 1, journal, err);
    if (ok) {
        plan.disk = &target;
        ok = restore ? dt_ebr_link_restore(&target, journal, torn, err)
                     : dt_ebr_link_apply(&plan, journal, err);
    }
    if (opened) {
        if (ui.bios)
            dt_bios_close(&target);
        else
            dt_image_close(&target);
    }
    open_disk(path, ui.bios, ui.drive);
    strcpy(ui.status,
           ok ? (restore ? "EBR link restored and verified. Journal retained."
                         : "EBR link inserted and verified. Journal retained.")
              : err);
done:
    dt_ebr_link_clear(&plan);
}

static int review_container(const DtContainerPlan *plan, int restore)
{
    unsigned top = 0, i;
    char line[100];
    int k;

    for (;;) {
        draw();
        box(5, 3, 70, 19, restore ? " REVIEW CONTAINER UNDO " : " REVIEW EXTENDED CONTAINER ");
        sprintf(line,
                "MBR slot %u; type %02X; %u recovered volumes",
                plan->slot,
                plan->type,
                plan->witness_count);
        text(8, 5, BRIGHT, line);
        sprintf(line,
                "Container %lu + %lu sectors; CHS %u/%u",
                (unsigned long)plan->base,
                (unsigned long)plan->span,
                plan->heads,
                plan->sectors);
        text(8, 6, CYAN, line);
        text(8, 7, CYAN, "EBR disk LBA   Volume disk LBA       Sectors    Type");
        for (i = 0; i < 9 && top + i < plan->witness_count; i++) {
            const DtLinkWitness *witness = &plan->witness[top + i];

            sprintf(line,
                    "%12lu     %12lu  %12lu      %02X",
                    (unsigned long)witness->record_sector,
                    (unsigned long)witness->volume_start,
                    (unsigned long)dt_dword(witness->record + 458),
                    witness->record[450]);
            text(8, 8 + i, WORK, line);
        }
        sprintf(line,
                "Records %u-%u of %u; PgUp/PgDn or arrows scroll",
                top + 1,
                top + 9 < plan->witness_count ? top + 9 : plan->witness_count,
                plan->witness_count);
        text(8, 18, CYAN, line);
        text(8, 19, BRIGHT, "Only the MBR entry/signature changes. Inspect files first.");
        text(8, 20, FOCUS, " Enter: accept review   Esc: cancel   Home/End: first/last ");
        flush();
        k = key();
        if (k == 13 || k == 27)
            return k == 13;
        if (k == NAV_HOME)
            top = 0;
        else if (k == NAV_END)
            top = plan->witness_count > 9 ? plan->witness_count - 9 : 0;
        else if ((k == NAV_DOWN || k == NAV_PGDN) && top + 9 < plan->witness_count) {
            top += k == NAV_PGDN ? 9 : 1;
            if (top + 9 > plan->witness_count)
                top = plan->witness_count - 9;
        } else if (k == NAV_UP || k == NAV_PGUP) {
            unsigned step = k == NAV_PGUP ? 9 : 1;

            top = top > step ? top - step : 0;
        }
    }
}

static void container_transaction(int restore, int torn)
{
    DtContainerPlan plan;
    DtDisk target;
    dt_u32 slot, base, span, type, heads, sectors;
    char type_text[16];
    char journal[128], confirm[32], path[128], err[160];
    int ok, opened;

    memset(&plan, 0, sizeof(plan));
    if (restore) {
        if (torn) {
            const char *lines[] = {"Undo a container after an interrupted write.",
                                   "Only saved before/after byte mixtures are accepted.",
                                   "Every recovered record and boot sector is revalidated."};

            if (!modal(" INTERRUPTED CONTAINER UNDO ", lines, 3))
                return;
        }
        if (!prompt(" CONTAINER JOURNAL TO RESTORE ", journal, sizeof(journal)))
            return;
        ok = dt_container_review_journal(&ui.disk, journal, &plan, err);
    } else {
        if (!mbr_number(" CONTAINER: MBR SLOT (1-4) ", &slot) ||
            !mbr_number(" CONTAINER: FIRST EBR DISK LBA ", &base) ||
            !mbr_number(" CONTAINER: TOTAL SECTORS ", &span) ||
            !prompt(" CONTAINER TYPE: 05, 0F OR 85 ", type_text, sizeof(type_text)))
            return;
        if (!dt_parse_u32(type_text, 16, &type) || (type != 5 && type != 15 && type != 0x85)) {
            strcpy(ui.status, "Enter extended partition type 05, 0F or 85.");
            return;
        }
        if (!mbr_number(" CONTAINER: BIOS GEOMETRY HEADS ", &heads) ||
            !mbr_number(" CONTAINER: BIOS SECTORS PER TRACK ", &sectors))
            return;
        ok = dt_container_plan(&ui.disk, slot, base, span, type, heads, sectors, &plan, err);
    }
    if (!ok) {
        strcpy(ui.status, err);
        goto done;
    }
    if (!review_container(&plan, restore))
        goto done;
    if (!restore && !prompt(" NEW CONTAINER JOURNAL PATH ", journal, sizeof(journal)))
        goto done;
    if (!prompt(restore ? " TYPE RESTORE TO UNDO CONTAINER " : " TYPE CONTAINER TO RECONSTRUCT ",
                confirm,
                sizeof(confirm)) ||
        strcmp(confirm, restore ? "RESTORE" : "CONTAINER"))
        goto done;
    if (!confirm_bios_write())
        goto done;
    strcpy(path, ui.source);
    release_disk_handle();
    ui.opened = ui.mounted = 0;
    opened =
        ui.bios ? dt_bios_open(&target, ui.drive, err) : dt_image_open_write(&target, path, err);
    ok = opened;
    if (ok && ui.bios)
        ok = dt_bios_enable_mbr_write(&target, journal, err);
    if (ok) {
        plan.disk = &target;
        ok = restore ? dt_container_restore(&target, journal, torn, err)
                     : dt_container_apply(&plan, journal, err);
    }
    if (opened) {
        if (ui.bios)
            dt_bios_close(&target);
        else
            dt_image_close(&target);
    }
    open_disk(path, ui.bios, ui.drive);
    strcpy(ui.status,
           ok ? (restore ? "Container restored and verified. Journal retained."
                         : "Container inserted and verified. Journal retained.")
              : err);
done:
    dt_container_clear(&plan);
}

static int review_record(const DtEbrRecordPlan *plan, int restore)
{
    unsigned top = 0, i, count = plan->witness_count + 1;
    const DtMbrPlan *logical = &plan->logical.entry;
    char line[100];
    int k;

    for (;;) {
        draw();
        box(5, 3, 70, 19, restore ? " REVIEW WHOLE EBR UNDO " : " REVIEW WHOLE EBR RECORD ");
        sprintf(line,
                "EBR %lu -> volume %lu; next EBR %lu",
                (unsigned long)plan->logical.sector,
                (unsigned long)logical->start,
                (unsigned long)plan->next);
        text(8, 5, BRIGHT, line);
        sprintf(line,
                "FAT%u; %lu sectors; boot +%u; CHS %u/%u",
                logical->bits,
                (unsigned long)logical->total,
                logical->boot_source,
                logical->heads,
                logical->sectors);
        text(8, 6, CYAN, line);
        text(8, 7, CYAN, "EBR disk LBA   Volume disk LBA       Sectors    Type");
        for (i = 0; i < 9 && top + i < count; i++) {
            unsigned index = top + i;
            dt_u32 record, start, total;
            unsigned type;

            if (!index) {
                record = plan->logical.sector;
                start = logical->start;
                total = logical->total;
                type = plan->after[450];
            } else {
                const DtLinkWitness *witness = &plan->witness[index - 1];

                record = witness->record_sector;
                start = witness->volume_start;
                total = dt_dword(witness->record + 458);
                type = witness->record[450];
            }
            sprintf(line,
                    "%12lu     %12lu  %12lu      %02X",
                    (unsigned long)record,
                    (unsigned long)start,
                    (unsigned long)total,
                    type);
            text(8, 8 + i, WORK, line);
        }
        sprintf(line,
                "Records %u-%u of %u; PgUp/PgDn or arrows scroll",
                top + 1,
                top + 9 < count ? top + 9 : count,
                count);
        text(8, 18, CYAN, line);
        text(8, 19, BRIGHT, "Only this EBR table/signature changes. Inspect files first.");
        text(8, 20, FOCUS, " Enter: accept review   Esc: cancel   Home/End: first/last ");
        flush();
        k = key();
        if (k == 13 || k == 27)
            return k == 13;
        if (k == NAV_HOME)
            top = 0;
        else if (k == NAV_END)
            top = count > 9 ? count - 9 : 0;
        else if ((k == NAV_DOWN || k == NAV_PGDN) && top + 9 < count) {
            top += k == NAV_PGDN ? 9 : 1;
            if (top + 9 > count)
                top = count - 9;
        } else if (k == NAV_UP || k == NAV_PGUP) {
            unsigned step = k == NAV_PGUP ? 9 : 1;

            top = top > step ? top - step : 0;
        }
    }
}

static void record_transaction(int restore, int torn)
{
    DtEbrRecordPlan plan;
    DtDisk target;
    dt_u32 sector, start, next, source, heads, sectors;
    char journal[128], confirm[32], path[128], err[160];
    int ok, opened;

    memset(&plan, 0, sizeof(plan));
    if (restore) {
        if (torn) {
            const char *lines[] = {"Undo a whole EBR after an interrupted write.",
                                   "Only saved before/after byte mixtures are accepted.",
                                   "Every recovered record and boot sector is revalidated."};

            if (!modal(" INTERRUPTED WHOLE EBR UNDO ", lines, 3))
                return;
        }
        if (!prompt(" WHOLE EBR JOURNAL TO RESTORE ", journal, sizeof(journal)))
            return;
        ok = dt_ebr_record_review_journal(&ui.disk, journal, &plan, err);
    } else {
        if (!mbr_number(" RECORD: EBR DISK LBA ", &sector) ||
            !mbr_number(" RECORD: VOLUME DISK LBA ", &start) ||
            !mbr_number(" RECORD: NEXT EBR LBA (0 ENDS CHAIN) ", &next) ||
            !mbr_number(" RECORD: BOOT SOURCE (0 PRIMARY, 6 BACKUP) ", &source) ||
            !mbr_number(" RECORD: BIOS GEOMETRY HEADS ", &heads) ||
            !mbr_number(" RECORD: BIOS SECTORS PER TRACK ", &sectors))
            return;
        ok = dt_ebr_record_plan(&ui.disk, sector, start, source, next, heads, sectors, &plan, err);
    }
    if (!ok) {
        strcpy(ui.status, err);
        goto done;
    }
    if (!review_record(&plan, restore))
        goto done;
    if (!restore && !prompt(" NEW WHOLE EBR JOURNAL PATH ", journal, sizeof(journal)))
        goto done;
    if (!prompt(restore ? " TYPE RESTORE TO UNDO WHOLE EBR " : " TYPE RECORD TO REBUILD WHOLE EBR ",
                confirm,
                sizeof(confirm)) ||
        strcmp(confirm, restore ? "RESTORE" : "RECORD"))
        goto done;
    if (!confirm_bios_write())
        goto done;
    strcpy(path, ui.source);
    release_disk_handle();
    ui.opened = ui.mounted = 0;
    opened =
        ui.bios ? dt_bios_open(&target, ui.drive, err) : dt_image_open_write(&target, path, err);
    ok = opened;
    if (ok && ui.bios)
        ok = dt_bios_enable_writes(&target, plan.logical.sector, 1, journal, err);
    if (ok) {
        plan.logical.entry.disk = &target;
        ok = restore ? dt_ebr_record_restore(&target, journal, torn, err)
                     : dt_ebr_record_apply(&plan, journal, err);
    }
    if (opened) {
        if (ui.bios)
            dt_bios_close(&target);
        else
            dt_image_close(&target);
    }
    open_disk(path, ui.bios, ui.drive);
    strcpy(ui.status,
           ok ? (restore ? "EBR record restored and verified. Journal retained."
                         : "EBR record reconstructed and verified. Journal retained.")
              : err);
done:
    dt_ebr_record_clear(&plan);
}

/* Menu actions are shared with shortcuts; unavailable actions remain disabled. */
enum {
    OPEN_IMAGE = 1,
    OPEN_BIOS,
    OPEN_DOS,
    OEM_CODEPAGE,
    VOLUMES,
    SEARCH_VOLUMES,
    RECONSTRUCT_MBR,
    UNDO_MBR,
    UNDO_MBR_TORN,
    RECONSTRUCT_EBR,
    UNDO_EBR,
    UNDO_EBR_TORN,
    REBUILD_RECORD,
    UNDO_RECORD,
    UNDO_RECORD_TORN,
    REBUILD_CONTAINER,
    UNDO_CONTAINER,
    UNDO_CONTAINER_TORN,
    RECONNECT_EBR,
    UNDO_LINK,
    UNDO_LINK_TORN,
    QUIT,
    CHECK,
    SURFACE,
    SURFACE_BLOCK,
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
    RECOVER_DELETED,
    RECOVER_DIRECTORIES,
    RESTORE_TORN,
    RECLAIM_LOST,
    REPAIR_FILES,
    REPAIR_DIRECTORIES,
    REPAIR_TREE,
    REPAIR_TREE_SOURCE,
    REPAIR_ENTRIES,
    CLEAR_TIMESTAMPS,
    CLEAR_LFN,
    REPAIR_LABEL_TEXT,
    CLEAR_EXTRA_DOTS,
    REPAIR_ATTRIBUTES,
    REPAIR_CONFLICTS,
    REPAIR_LABELS,
    REPAIR_ALIASES,
    REBUILD_FAT_HEADERS,
    AUTOFIX,
    AUTOFIX_SURFACE,
    REPAIR_CROSSLINKS,
    CONVERT_LOST,
    CMOS_CHECK,
    CVF_CHECK,
    SECTOR_VIEW,
    OWNER_VIEW,
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
    {"Search lost volumes...", SEARCH_VOLUMES},
    {"Reconstruct MBR...", RECONSTRUCT_MBR},
    {"Undo MBR reconstruction...", UNDO_MBR},
    {"Undo interrupted MBR...", UNDO_MBR_TORN},
    {"Reconstruct EBR...", RECONSTRUCT_EBR},
    {"Undo EBR reconstruction...", UNDO_EBR},
    {"Undo interrupted EBR...", UNDO_EBR_TORN},
    {"Reconnect EBR chain...", RECONNECT_EBR},
    {"Undo EBR link...", UNDO_LINK},
    {"Undo interrupted link...", UNDO_LINK_TORN},
    {"Rebuild extended container...", REBUILD_CONTAINER},
    {"Undo extended container...", UNDO_CONTAINER},
    {"Undo interrupted container...", UNDO_CONTAINER_TORN},
    {"Rebuild whole EBR...", REBUILD_RECORD},
    {"Undo whole EBR...", UNDO_RECORD},
    {"Undo interrupted record...", UNDO_RECORD_TORN},
    {"OEM code page...", OEM_CODEPAGE},
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
    {"Rebuild FAT headers...", REBUILD_FAT_HEADERS},
    {"Rebuild FAT copies...", REBUILD_FAT},
    {"Rebuild backup boot...", REBUILD_BOOT_BACKUP},
    {"Rebuild FAT32 FSInfo...", REBUILD_FSINFO},
    {"Remove extra dot entries...", CLEAR_EXTRA_DOTS},
    {"Repair reserved attributes...", REPAIR_ATTRIBUTES},
    {"Repair name conflicts...", REPAIR_CONFLICTS},
    {"Repair volume label text...", REPAIR_LABEL_TEXT},
    {"Repair volume labels...", REPAIR_LABELS},
    {"Repair invalid DOS names...", REPAIR_ALIASES},
    {"Clear invalid long names...", CLEAR_LFN},
    {"Clear invalid timestamps...", CLEAR_TIMESTAMPS},
    {NULL, 0},
};
static const MenuItem view_menu[] = {
    {"Allocation map    F5", ALLOCATION},
    {"Zoom into block    +", ZOOM_IN},
    {"Whole volume       -", ZOOM_OUT},
    {"Findings          F6", FINDINGS},
    {"Sector viewer...  F8", SECTOR_VIEW},
    {"Retest map block   T", SURFACE_BLOCK},
    {"Cluster owner...   O", OWNER_VIEW},
    {NULL, 0},
};
static const MenuItem report_menu[] = {
    {"Save report...    F7", SAVE_REPORT},
    {"Export live files...", EXPORT_FILES},
    {"Recover interrupted write...", RESTORE_TORN},
    {"Recover deleted files...", RECOVER_DELETED},
    {"Find orphan directories...", RECOVER_DIRECTORIES},
    {NULL, 0},
};
static const MenuItem help_menu[] = {
    {"Keyboard / guide  F1", HELP},
    {"About MagicDisk...", ABOUT},
    {NULL, 0},
};
static const MenuItem fix_menu[] = {
    {"Automatic repair...      F9", AUTOFIX},
    {"Automatic + surface...", AUTOFIX_SURFACE},
    {"Copy cross-linked files...", REPAIR_CROSSLINKS},
    {"Convert lost chains...", CONVERT_LOST},
    {"CMOS check...", CMOS_CHECK},
    {"Check compressed volume...", CVF_CHECK},
    {NULL, 0},
};
static const MenuItem *menus[] = {disk_menu, diagnose_menu, view_menu, report_menu, help_menu,
                                  fix_menu};
static const int menu_x[] = {1, 9, 21, 29, 40, 48};
static const char *menu_labels[] = {"Disk", "Diagnose", "View", "Reports", "Help", "Fix"};

static int enabled(int action)
{
    if (action == RECONSTRUCT_MBR || action == UNDO_MBR || action == UNDO_MBR_TORN ||
        action == RECONSTRUCT_EBR || action == UNDO_EBR || action == UNDO_EBR_TORN ||
        action == RECONNECT_EBR || action == UNDO_LINK || action == UNDO_LINK_TORN ||
        action == REBUILD_CONTAINER || action == UNDO_CONTAINER || action == UNDO_CONTAINER_TORN ||
        action == REBUILD_RECORD || action == UNDO_RECORD || action == UNDO_RECORD_TORN)
        return ui.opened && ui.bios != 2;
    if (action == SEARCH_VOLUMES)
        return ui.opened && ui.bios != 2;
    if (ui.candidates &&
        (action == MARK_BAD || action == UNMARK_BAD || action == RESTORE_JOURNAL ||
         action == RESTORE_TORN || action == RECOVER_PRIMARY_BOOT || action == REBUILD_FSINFO ||
         action == REBUILD_BOOT_BACKUP || action == REBUILD_FAT || action == RECLAIM_LOST ||
         action == REPAIR_FILES || action == REPAIR_DIRECTORIES || action == REPAIR_TREE ||
         action == REPAIR_TREE_SOURCE || action == REPAIR_ENTRIES || action == CLEAR_TIMESTAMPS ||
         action == CLEAR_LFN || action == REPAIR_LABEL_TEXT || action == CLEAR_EXTRA_DOTS ||
         action == REPAIR_ATTRIBUTES || action == REPAIR_CONFLICTS || action == REPAIR_LABELS ||
         action == REPAIR_ALIASES || action == REBUILD_FAT_HEADERS || action == AUTOFIX ||
         action == AUTOFIX_SURFACE || action == REPAIR_CROSSLINKS || action == CONVERT_LOST))
        return 0;
    if (action == SURFACE_BLOCK)
        return ui.mounted && ui.map.cells;
    if (action == OWNER_VIEW)
        return ui.mounted;
    if (action == SECTOR_VIEW)
        return ui.opened && ui.disk.sectors;
    if (action == RECOVER_PRIMARY_BOOT)
        return ui.opened && ui.count;
    if (ui.mounted && ui.volume.boot_source &&
        (action == MARK_BAD || action == UNMARK_BAD || action == REBUILD_FSINFO ||
         action == REBUILD_BOOT_BACKUP || action == REBUILD_FAT || action == RECLAIM_LOST ||
         action == REPAIR_FILES || action == REPAIR_DIRECTORIES || action == REPAIR_TREE ||
         action == REPAIR_TREE_SOURCE || action == REPAIR_ENTRIES || action == CLEAR_TIMESTAMPS ||
         action == CLEAR_LFN || action == REPAIR_LABEL_TEXT || action == CLEAR_EXTRA_DOTS ||
         action == REPAIR_ATTRIBUTES || action == REPAIR_CONFLICTS || action == REPAIR_LABELS ||
         action == REPAIR_ALIASES || action == REBUILD_FAT_HEADERS || action == AUTOFIX ||
         action == AUTOFIX_SURFACE || action == REPAIR_CROSSLINKS || action == CONVERT_LOST))
        return 0;
    if (action == VOLUMES)
        return ui.opened && ui.count;
    if (action == RECOVER_DIRECTORIES || action == RESTORE_TORN || action == RECOVER_DELETED ||
        action == EXPORT_FILES || action == RECOVER_LOST || action == RECLAIM_LOST ||
        action == REPAIR_FILES || action == REPAIR_DIRECTORIES || action == REPAIR_TREE ||
        action == REPAIR_ENTRIES || action == CLEAR_TIMESTAMPS || action == CLEAR_LFN ||
        action == REPAIR_LABEL_TEXT || action == CLEAR_EXTRA_DOTS || action == REPAIR_ATTRIBUTES ||
        action == REPAIR_CONFLICTS || action == REPAIR_LABELS || action == REPAIR_ALIASES ||
        action == REBUILD_FAT_HEADERS || action == AUTOFIX || action == AUTOFIX_SURFACE ||
        action == REPAIR_CROSSLINKS || action == CONVERT_LOST)
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
    unsigned tab = 0, selected = 0, top = 0, i, count, visible;
    for (;;) {
        int k, x = menu_x[tab], width = 26;
        const MenuItem *items = menus[tab];
        for (count = 0; items[count].label; count++) {
            int needed = (int)strlen(items[count].label) + 4;

            if (needed > width)
                width = needed;
        }
        if (x + width > 79)
            x = 79 - width;
        visible = count < 19 ? count : 19;
        if (selected < top)
            top = selected;
        if (selected >= top + visible)
            top = selected - visible + 1;
        draw_workspace();
        fill(menu_x[tab], 1, (int)strlen(menu_labels[tab]) + 2, ' ', FOCUS);
        text(menu_x[tab] + 1, 1, FOCUS, menu_labels[tab]);
        box(x, 2, width, (int)visible + 2, NULL);
        for (i = 0; i < visible; i++) {
            unsigned item = top + i;
            unsigned attr =
                !enabled(items[item].action) ? MUTED : (item == selected ? FOCUS : WORK);

            fill(x + 1, 3 + i, width - 2, ' ', attr);
            text(x + 2, 3 + i, attr, items[item].label);
        }
        if (top)
            cell(x + width - 3, 2, '^', BRIGHT);
        if (top + visible < count)
            cell(x + width - 3, (int)visible + 3, 'v', BRIGHT);
        flush();
        k = key();
        if (k == 27 || k == KEY_F10)
            return 0;
        if (k == NAV_LEFT || k == NAV_RIGHT) {
            tab = (tab + (k == NAV_RIGHT ? 1 : 5)) % 6;
            selected = top = 0;
        }
        if (k == NAV_UP)
            selected = (selected + count - 1) % count;
        if (k == NAV_DOWN)
            selected = (selected + 1) % count;
        if (k == NAV_PGUP)
            selected = selected >= visible ? selected - visible : 0;
        if (k == NAV_PGDN)
            selected = selected + visible < count ? selected + visible : count - 1;
        if (k == NAV_HOME)
            selected = 0;
        if (k == NAV_END)
            selected = count - 1;
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
    static const char *help[] = {"F10 opens menus. Arrows, Home/End and Enter navigate.",
                                 "",
                                 "F2 Volumes  F3 Filesystem check  F4 Surface read",
                                 "F5 Allocation map  F6 Findings  F7 Save report  F8 Bytes",
                                 "",
                                 "Map: arrows select; Enter details; + zoom; - reset; T retest.",
                                 "Esc cancels an operation or closes a window.",
                                 "F9 Automatic repair: every fix journaled and verified.",
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
    case OEM_CODEPAGE:
        sprintf(path, "%u", dt_oem_codepage());
        if (prompt(" OEM CODE PAGE: 0 UNKNOWN, 437, 850, 852, 860, 866 ", path, sizeof(path))) {
            if (!dt_parse_u32(path, 10, &first) ||
                (first > 65535 || !dt_set_oem_codepage((unsigned)first))) {
                strcpy(ui.status, "Use 0 (unknown), 437, 850, 852, 860 or 866.");
            } else {
                clear_findings();
                ui.scanned = ui.retested = 0;
                strcpy(ui.status,
                       "OEM policy changed. F3 checks names using the selected code page.");
            }
        }
        break;
    case OWNER_VIEW:
        owner_view(selected_sector());
        break;
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
    case REBUILD_RECORD:
        record_transaction(0, 0);
        break;
    case UNDO_RECORD:
        record_transaction(1, 0);
        break;
    case UNDO_RECORD_TORN:
        record_transaction(1, 1);
        break;
    case REBUILD_CONTAINER:
        container_transaction(0, 0);
        break;
    case UNDO_CONTAINER:
        container_transaction(1, 0);
        break;
    case UNDO_CONTAINER_TORN:
        container_transaction(1, 1);
        break;
    case RECONNECT_EBR:
        link_transaction(0, 0);
        break;
    case UNDO_LINK:
        link_transaction(1, 0);
        break;
    case UNDO_LINK_TORN:
        link_transaction(1, 1);
        break;
    case RECONSTRUCT_EBR:
        partition_transaction(0, 0, 1);
        break;
    case UNDO_EBR:
        partition_transaction(1, 0, 1);
        break;
    case UNDO_EBR_TORN:
        partition_transaction(1, 1, 1);
        break;
    case RECONSTRUCT_MBR:
        partition_transaction(0, 0, 0);
        break;
    case UNDO_MBR:
        partition_transaction(1, 0, 0);
        break;
    case UNDO_MBR_TORN:
        partition_transaction(1, 1, 0);
        break;
    case SEARCH_VOLUMES:
        search_volumes();
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
    case SURFACE_BLOCK:
        first = 0;
        count = ui.volume.total;
        if (command == SURFACE_BLOCK) {
            dt_map_range(&ui.map, ui.cell, &first, &count);
            if (!ui.map.surface) {
                first = ui.volume.data + (first - 2) * ui.volume.spc;
                count *= ui.volume.spc;
            }
        }
        clear_findings();
        ui.scanned = ui.retested = 0;
        ui.surface_tested = ui.surface_failed = 0;
        ui.busy = 1;
        ui.cell = 0;
        ui.surface_first = first;
        ui.surface_count = count;
        strcpy(ui.status,
               command == SURFACE_BLOCK
                   ? "Retesting the selected map block. Esc cancels."
                   : "Reading every volume sector, including metadata. Esc cancels.");
        ok = dt_surface_ex(&ui.volume, first, count, &ui.map, progress, NULL, finding, NULL);
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
                   "F5 switches from surface results to the zoomable "
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
        strcpy(ui.status,
               ui.retest.complete
                   ? "Bad markers retained. F6 shows cluster results; F7 saves the retest report."
                   : "Retest incomplete. Bad markers retained. F6 shows why; F7 saves results.");
        break;
    case MARK_BAD:
        change_bad_marker(1, 0xffffffffUL);
        break;
    case UNMARK_BAD:
        change_bad_marker(0, 0xffffffffUL);
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
    case REBUILD_FAT_HEADERS:
        rebuild_fat_headers();
        break;
    case REPAIR_LABEL_TEXT:
        repair_label_text();
        break;
    case CLEAR_EXTRA_DOTS:
        clear_extra_dots();
        break;
    case REPAIR_ATTRIBUTES:
        repair_attributes();
        break;
    case REPAIR_CONFLICTS:
        repair_conflicts();
        break;
    case REPAIR_LABELS:
        repair_labels();
        break;
    case REPAIR_ALIASES:
        repair_short_names();
        break;
    case CLEAR_LFN:
        clear_invalid_lfn();
        break;
    case CLEAR_TIMESTAMPS:
        clear_timestamps();
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
    case AUTOFIX:
    case AUTOFIX_SURFACE:
        automatic_repair(command == AUTOFIX_SURFACE);
        break;
    case REPAIR_CROSSLINKS:
        preserve_data(0);
        break;
    case CONVERT_LOST:
        preserve_data(1);
        break;
    case CMOS_CHECK:
        cmos_check();
        break;
    case CVF_CHECK:
        cvf_check();
        break;
    case RECOVER_DIRECTORIES:
        recover_files(3, 0);
        break;
    case RECOVER_DELETED:
        recover_files(2, 0);
        break;
    case EXPORT_FILES:
        recover_files(1, 0);
        break;
    case RECOVER_LOST:
        recover_files(0, 0);
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

/* Route diagnosis to existing reviewed, journaled workflows. A preview may
   replace the findings array, so never retain its code pointer across action(). */
static int finding_repair(const char *code, int run)
{
    static const struct {
        const char *code;
        int command;
    } repairs[] = {
        {"FILE_SIZE", REPAIR_FILES},
        {"CHAIN_RANGE", REPAIR_TREE},
        {"CHAIN_SHARED", REPAIR_CROSSLINKS},
        {"LOST_CLUSTERS", CONVERT_LOST},
        {"CHAIN_INVALID", REPAIR_TREE},
        {"DIR_DOT_METADATA", REPAIR_ENTRIES},
        {"DIR_DOT_MISSING", REPAIR_ENTRIES},
        {"DIR_DOT_ORDER", REPAIR_ENTRIES},
        {"DIR_SIZE", REPAIR_ENTRIES},
        {"DIR_TIMESTAMP", CLEAR_TIMESTAMPS},
        {"DIR_DOT_ROOT", CLEAR_EXTRA_DOTS},
        {"DIR_DOT_POSITION", CLEAR_EXTRA_DOTS},
        {"DIR_ATTRIBUTE", REPAIR_ATTRIBUTES},
        {"DIR_DUPLICATE", REPAIR_CONFLICTS},
        {"DIR_LABEL_NAME", REPAIR_LABEL_TEXT},
        {"DIR_LABEL", REPAIR_LABELS},
        {"DIR_NAME", REPAIR_ALIASES},
        {"LFN_ORDER", CLEAR_LFN},
        {"LFN_ORPHAN", CLEAR_LFN},
        {"LFN_METADATA", CLEAR_LFN},
        {"LFN_CHECKSUM", CLEAR_LFN},
        {"LFN_NAME", CLEAR_LFN},
        {"FAT_HEADER", REBUILD_FAT_HEADERS},
        {"FAT_MIRROR", REPAIR_TREE_SOURCE},
        {"FSINFO_SIGNATURE", REBUILD_FSINFO},
        {"FSINFO_FREE", REBUILD_FSINFO},
        {"FSINFO_NEXT", REBUILD_FSINFO},
        {"FSINFO_RESERVED", REBUILD_FSINFO},
        {"BOOT_BACKUP", REBUILD_BOOT_BACKUP},
        {"BOOT_PRIMARY", RECOVER_PRIMARY_BOOT},
    };

    unsigned i;

    if (ui.preview)
        return 0;
    for (i = 0; i < sizeof(repairs) / sizeof(repairs[0]); i++) {
        if (!strcmp(code, repairs[i].code) && enabled(repairs[i].command)) {
            if (run)
                action(repairs[i].command);
            return 1;
        }
    }
    return 0;
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
        else if (k == KEY_F1 + 8)
            command = AUTOFIX;
        else if (k == 't' || k == 'T')
            command = SURFACE_BLOCK;
        else if (k == 'o' || k == 'O')
            command = OWNER_VIEW;
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
