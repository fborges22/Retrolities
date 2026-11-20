/* Polaris - screens shared by the boot menu and the installer: backdrop
 * with the animated night sky, disk maps, device explorer, system
 * information and the setup editor. */
#include "ui.h"
#include "config.h"

int (*ui_boot)(struct pl_entry *e);        /* set by stage 2 */
void (*ui_tick)(void);                     /* extra per-tick work */
const char *ui_where = "";                 /* "Hard disk 80h" ... */

/* ======================================================================
 * backdrop, header and animation
 * ==================================================================== */

#define NSTARS 22
static u8 star_x[NSTARS], star_y[NSTARS], star_f[NSTARS];
static u32 last_tick;
static u8 phase;
static u16 rnd = 0x2B1F;

static u8 random8(void)
{
    rnd = (u16)(rnd * 25173u + 13849u);
    return (u8)(rnd >> 8);
}

static void make_stars(void)
{
    u8 i = 0;
    while (i < NSTARS) {
        u8 x = (u8)(random8() % 80), y = (u8)(random8() % 4);
        if (y >= 1 && y <= 2 && x < 64) continue;    /* logo and caption */
        if (y >= 1 && y <= 2 && x >= 62) continue;   /* clock */
        star_x[i] = x; star_y[i] = y; star_f[i] = (u8)(random8() % 3);
        i++;
    }
}

static u8 bcd(u8 v) { return (u8)((v >> 4) * 10 + (v & 15)); }

static void draw_clock(void)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    rmregs r;
    char t[20];
    r.a.w = 0x0200;
    pl_intr(0x1A, &r);
    if (!(r.flags & CF_FLAG)) {
        t[0] = (char)('0' + (r.c.b.h >> 4)); t[1] = (char)('0' + (r.c.b.h & 15)); t[2] = ':';
        t[3] = (char)('0' + (r.c.b.l >> 4)); t[4] = (char)('0' + (r.c.b.l & 15)); t[5] = ':';
        t[6] = (char)('0' + (r.d.b.h >> 4)); t[7] = (char)('0' + (r.d.b.h & 15)); t[8] = 0;
        tui_putr(77, 1, t, A(C_TEXT, C_DEEP));
    }
    r.a.w = 0x0400;
    pl_intr(0x1A, &r);
    if (!(r.flags & CF_FLAG)) {
        u8 m = bcd(r.d.b.h);
        if (m < 1 || m > 12) m = 1;
        t[0] = (char)('0' + (r.d.b.l >> 4)); t[1] = (char)('0' + (r.d.b.l & 15)); t[2] = ' ';
        t[3] = mon[(m - 1) * 3]; t[4] = mon[(m - 1) * 3 + 1]; t[5] = mon[(m - 1) * 3 + 2];
        t[6] = ' ';
        t[7] = (char)('0' + (r.c.b.h >> 4)); t[8] = (char)('0' + (r.c.b.h & 15));
        t[9] = (char)('0' + (r.c.b.l >> 4)); t[10] = (char)('0' + (r.c.b.l & 15)); t[11] = 0;
        tui_putr(77, 2, t, A(C_DIM, C_DEEP));
    }
}

void draw_header_dynamic(void)
{
    u8 i;
    static const u8 frames[3] = { G_STAR1, G_STAR2, G_STAR3 };
    for (i = 0; i < NSTARS; i++) {
        u8 f = star_f[i];
        tui_put(star_x[i], star_y[i], frames[f], A(f == 2 ? C_GLOW : C_DIM, C_DEEP));
    }
    draw_clock();
}

void draw_backdrop(const char *subtitle)
{
    u8 x;
    if (!star_x[0] && !star_y[0]) make_stars();
    tui_fill(0, 0, SW, 4, ' ', A(C_TEXT, C_DEEP));
    for (x = 0; x < SW; x++) tui_put(x, 4, G_SHADE2, A(C_DEEP, C_BG));
    tui_fill(0, 5, SW, 19, ' ', A(C_TEXT, C_BG));
    tui_logo(3, 1, C_ICE, C_GLOW, C_DEEP);
    x = tui_puts(31, 1, "BOOT MANAGER", A(C_DIM, C_DEEP));
    tui_puts(x + 1, 1, PL_VERSION_STR, A(C_BORDER, C_DEEP));
    tui_puts(31, 2, subtitle, A(C_GOLD, C_DEEP));
    tui_fill(0, 24, SW, 1, ' ', A(C_DIM, C_DEEP));
    draw_header_dynamic();
}

/* keys: pairs of strings, NULL terminated */
void draw_footer(const char *const *keys)
{
    u8 x = 1;
    tui_fill(0, 24, SW, 1, ' ', A(C_DIM, C_DEEP));
    while (keys[0] && x < 76) {
        x = tui_key(x, 24, keys[0], keys[1], C_DEEP);
        keys += 2;
    }
}

void ui_idle(void)
{
    u32 t = pl_ticks();
    if (t == last_tick) return;
    last_tick = t;
    phase++;
    tui_twinkle(phase);
    if (!(phase & 3)) {
        u8 i = (u8)(random8() % NSTARS);
        star_f[i] = (u8)(random8() % 3);
    }
    draw_header_dynamic();
    if (ui_tick) ui_tick();
    tui_flush();
}

/* ======================================================================
 * disk map: proportional bar of the partitions of one disk
 * ==================================================================== */

void draw_diskmap(u8 x, u8 y, u8 w, u8 drive, u32 lba, u8 bg)
{
    static const u8 cols[4] = { C_SELECT, C_VIOLET, C_GREEN, C_AMBER };
    struct bdisk *d = disk_find(drive);
    u8 i, c = 0;
    u32 total;
    if (!d || !(drive & 0x80) || !d->sectors) return;
    total = d->sectors;
    for (i = 0; i < w; i++) tui_put(x + i, y, G_TRACK, A(C_BORDER, bg));
    for (i = 0; i < nparts; i++) {
        struct part *p = &parts[i];
        u8 a, b, k, col;
        if (disks[p->disk].drive != drive || ptype_extended(p->type)) continue;
        a = (u8)(p->lba * w / total);
        b = (u8)((p->lba + p->sectors) * w / total);
        if (b <= a) b = a + 1;
        if (b > w) b = w;
        col = p->lba == lba ? C_GOLD : cols[c++ & 3];
        for (k = a; k < b; k++)
            tui_put(x + k, y, k + 1 == b && b - a > 1 ? G_BAR7 : G_BAR8, A(col, bg));
    }
}

/* ======================================================================
 * device explorer / picker
 * ==================================================================== */

static void device_icon(struct devitem *it, u8 *icon, u8 *col)
{
    *col = C_ICE;
    switch (it->type) {
    case DV_DISK: *icon = PI_DISK; *col = C_DIM; break;
    case DV_PART: *icon = parts[it->idx].icon; *col = C_TEXT; break;
    case DV_FLOPPY: *icon = PI_FLOPPY; break;
    case DV_ATAPI: *icon = atapi_icon(&atapi[it->idx]); *col = C_GOLD; break;
    default: {
        static const u8 ic[] = { 0, 0, 0, 0, 0, 0, PI_NET, PI_REBOOT, PI_POWER, PI_STAR };
        *icon = ic[it->idx % 10];
        *col = C_VIOLET;
    }
    }
}

static void device_details(struct devitem *it, u8 x, u8 y)
{
    char t[40], h[12];
    u8 a = A(C_DIM, C_PANEL), v = A(C_TEXT, C_PANEL);
    tui_fill(x, y, 26, 12, ' ', v);
    switch (it->type) {
    case DV_DISK: {
        struct bdisk *d = &disks[it->idx];
        tui_puts(x, y, "BIOS hard disk", A(C_ICE, C_PANEL));
        tui_puts(x, y + 2, "Drive", a);    hexbyte(h, d->drive); tui_puts(x + 10, y + 2, h, v);
        tui_puts(x, y + 3, "Capacity", a); tui_puts(x + 10, y + 3, pl_size(t, d->sectors), v);
        tui_puts(x, y + 4, "Access", a);   tui_puts(x + 10, y + 4, d->edd ? "EDD / LBA" : "CHS", v);
        tui_puts(x, y + 5, "Geometry", a);
        pl_utoa(t, d->cyls); pl_strcat(t, "/"); pl_utoa(h, d->heads); pl_strcat(t, h);
        pl_strcat(t, "/"); pl_utoa(h, d->spt); pl_strcat(t, h);
        tui_puts(x + 10, y + 5, t, v);
        draw_diskmap(x, y + 7, 24, d->drive, 0xFFFFFFFFUL, C_PANEL);
        break;
    }
    case DV_PART: {
        struct part *p = &parts[it->idx];
        tui_puts(x, y, p->os[0] ? p->os : p->fs, A(C_ICE, C_PANEL));
        tui_puts(x, y + 2, "Type", a);
        hexbyte(h, p->type); pl_strncpy(t, h, 5); pl_strcat(t, " ");
        pl_strcat(t, ptype_name(p->type));
        tui_puts(x + 10, y + 2, t, v);
        tui_puts(x, y + 3, "Size", a);   tui_puts(x + 10, y + 3, pl_size(t, p->sectors), v);
        tui_puts(x, y + 4, "Start", a);  tui_puts(x + 10, y + 4, pl_utoa(t, p->lba), v);
        tui_puts(x, y + 5, "Label", a);  tui_puts(x + 10, y + 5, p->label[0] ? p->label : "-", v);
        tui_puts(x, y + 6, "Boot", a);
        tui_puts(x + 10, y + 6, p->bootable ? (p->active ? "Yes, active" : "Yes") : "No signature",
                 A(p->bootable ? C_LGREEN : C_PINK, C_PANEL));
        draw_diskmap(x, y + 8, 24, disks[p->disk].drive, p->lba, C_PANEL);
        break;
    }
    case DV_ATAPI: {
        struct atadev *d = &atapi[it->idx];
        tui_puts(x, y, atapi_kind(d), A(C_ICE, C_PANEL));
        tui_putsn(x, y + 2, d->model, v, 26);
        tui_puts(x, y + 4, "Port", a);
        pl_hex(t, d->io, 3); pl_strcat(t, d->slave ? "h slave" : "h master");
        tui_puts(x + 10, y + 4, t, v);
        tui_puts(x, y + 5, "Bus", a);  tui_puts(x + 10, y + 5, d->pci ? "PCI native" : "Legacy IDE", v);
        tui_puts(x, y + 6, "Media", a);
        if (d->media) {
            pl_size(t, d->blocks * (d->blksize / 512));
            tui_puts(x + 10, y + 6, t, A(C_LGREEN, C_PANEL));
        } else tui_puts(x + 10, y + 6, "Empty", A(C_AMBER, C_PANEL));
        tui_puts(x, y + 8, "Booted by Polaris' own", A(C_DIM, C_PANEL));
        tui_puts(x, y + 9, "ATAPI driver; no BIOS", A(C_DIM, C_PANEL));
        tui_puts(x, y + 10, "support needed.", A(C_DIM, C_PANEL));
        break;
    }
    case DV_FLOPPY:
        tui_puts(x, y, "Floppy drive", A(C_ICE, C_PANEL));
        tui_puts(x, y + 2, it->idx ? "Drive B: (01h)" : "Drive A: (00h)", v);
        break;
    default:
        tui_puts(x, y, kind_name(it->idx), A(C_ICE, C_PANEL));
        break;
    }
}

/* mode 0: explorer, 1: pick an entry target */
int devices_run(u8 pick, struct pl_entry *out)
{
    static const char *const keys_ex[] = { "Enter", "Boot now", "Ins", "Add to menu", "R", "Rescan",
                                           "Esc", "Back", 0, 0 };
    static const char *const keys_pk[] = { "Enter", "Choose", "Esc", "Cancel", 0, 0 };
    u8 n = devlist_build(pick), sel = 0, top = 0, i, vis = 15;
    char lb[60];
    if (natapi) {
        tui_progress("Devices", "Checking removable media...", 1, 2);
        for (i = 0; i < natapi; i++) atapi_capacity(&atapi[i]);
        tui_progress(0, 0, 0, 0);
    }
    for (;;) {
        draw_backdrop(pick ? "Choose what to boot" : "Device explorer");
        tui_card(2, 6, 48, 17, pick ? "Bootable devices" : "Devices", C_BORDER, C_PANEL);
        tui_card(52, 6, 26, 17, "Details", C_BORDER, C_PANEL);
        tui_shadow(2, 6, 48, 17);
        tui_shadow(52, 6, 26, 17);
        if (sel >= n) sel = n ? n - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        for (i = 0; i < vis && top + i < n; i++) {
            struct devitem *it = &devitems[top + i];
            u8 y = 7 + i, on = top + i == sel, icon, col;
            u8 bg = on ? C_SELECT : C_PANEL;
            tui_fill(3, y, 46, 1, ' ', A(C_TEXT, bg));
            if (on) tui_put(3, y, G_ACCENT, A(C_ICE, bg));
            device_icon(it, &icon, &col);
            tui_icon(it->type == DV_PART ? 7 : 5, y, icon, A(col, bg));
            devitem_label(it, lb, it->type == DV_PART ? 38 : 40);
            if (it->type == DV_PART) {
                u8 last = top + i + 1 >= n || devitems[top + i + 1].type != DV_PART;
                tui_puts(10, y, lb, A(C_TEXT, bg));
                tui_put(5, y, last ? 0xC0 : 0xC3, A(C_BORDER, bg));
                tui_put(6, y, 0xC4, A(C_BORDER, bg));
            } else
                tui_puts(8, y, lb, A(it->type == DV_DISK ? C_ICE : C_TEXT, bg));
        }
        tui_scrollbar(48, 7, vis, top, vis, n, A(C_BORDER, C_PANEL));
        if (!n) tui_puts(5, 8, "No devices found.", A(C_DIM, C_PANEL));
        else device_details(&devitems[sel], 54, 8);
        draw_footer(pick ? keys_pk : (ui_boot ? keys_ex : keys_ex + 2));
        tui_flush();
        {
            u16 k = tui_getkey();
            u8 c = (u8)pl_upper(KEYCHAR(k));
            if (k == K_ESC) return 0;
            if (k == K_UP && sel) sel--;
            else if (k == K_DOWN && sel + 1 < n) sel++;
            else if (k == K_PGUP) sel = sel > vis ? sel - vis : 0;
            else if (k == K_PGDN) sel = (u8)(sel + vis < n ? sel + vis : n - 1);
            else if (k == K_HOME) sel = 0;
            else if (k == K_END) sel = n - 1;
            else if (c == 'R' && !pick) {
                tui_progress("Scanning", "Looking for disks, partitions and ATAPI drives...", 1, 2);
                disk_scan(); atapi_scan(1);
                tui_progress(0, 0, 0, 0);
                n = devlist_build(0);
            } else if (n && k == K_ENTER) {
                struct pl_entry e;
                devitem_entry(&devitems[sel], &e);
                if (pick) { *out = e; return 1; }
                if (ui_boot && devitems[sel].type != DV_DISK) ui_boot(&e);
                else if (ui_boot) ui_boot(&e);
            } else if (n && (k == K_INS || c == 'A') && !pick) {
                struct pl_entry e;
                devitem_entry(&devitems[sel], &e);
                if (cfg_insert(&cfg, cfg.count, &e) < 0)
                    tui_msg("Menu full", "The menu already has the maximum number of entries.", "OK", MT_WARN);
                else if (tui_msg("Added", "The entry was added to the end of the menu.\nSave the configuration now?",
                                 "Save|Later", MT_OK) == 0) {
                    cfg_seal(&cfg);
                    if (cfg_store()) tui_msg("Error", "The configuration could not be written.", "OK", MT_ERROR);
                }
            }
        }
    }
}

/* ======================================================================
 * system information
 * ==================================================================== */

static void cpu_name(char *b)
{
    u8 cls = pl_cpu_class();
    b[0] = 0;
    if (cls < 5) {
        pl_strcat(b, cls == 3 ? "Intel 386 class" : "Intel 486 class");
        return;
    }
    {
        u32 r[4], max;
        u8 i, n = 0;
        pl_cpuid(0x80000000UL, r);
        max = r[0];
        if (max >= 0x80000004UL) {
            for (i = 0; i < 3; i++) {
                pl_cpuid(0x80000002UL + i, r);
                pl_memcpy(b + i * 16, r, 16);
            }
            b[48] = 0;
            while (b[n] == ' ') n++;
            if (n) pl_memcpy(b, b + n, 49 - n);
            return;
        }
        pl_cpuid(0, r);
        pl_memcpy(b, &r[1], 4); pl_memcpy(b + 4, &r[3], 4); pl_memcpy(b + 8, &r[2], 4);
        b[12] = 0;
        pl_cpuid(1, r);
        pl_strcat(b, " family ");
        pl_utoa(b + pl_strlen(b), (r[0] >> 8) & 15);
        pl_strcat(b, " model ");
        pl_utoa(b + pl_strlen(b), (r[0] >> 4) & 15);
    }
}

static void row(u8 y, const char *k, const char *v, u8 col)
{
    tui_puts(6, y, k, A(C_DIM, C_PANEL));
    tui_putsn(24, y, v, A(col, C_PANEL), 50);
}

void sysinfo_run(void)
{
    static const char *const keys[] = { "Esc", "Back", 0, 0 };
    char t[64], h[16];
    rmregs r;
    u16 conv, ext = 0;
    u32 big = 0;

    draw_backdrop("System information");
    tui_card(3, 6, 74, 17, "This computer", C_BORDER, C_PANEL);
    tui_shadow(3, 6, 74, 17);

    cpu_name(t);
    tui_icon(6, 8, PI_GEAR, A(C_GOLD, C_PANEL));
    tui_puts(9, 8, "Processor", A(C_ICE, C_PANEL));
    tui_putsn(24, 8, t, A(C_TEXT, C_PANEL), 50);

    pl_intr(0x12, &r);
    conv = r.a.w;
    r.a.w = 0xE801; r.c.w = 0; r.d.w = 0;
    pl_intr(0x15, &r);
    if (!(r.flags & CF_FLAG)) {
        u16 lo = r.a.w ? r.a.w : r.c.w, hi = r.b.w ? r.b.w : r.d.w;
        big = (u32)lo + (u32)hi * 64;
    } else {
        r.a.w = 0x8800;
        pl_intr(0x15, &r);
        if (!(r.flags & CF_FLAG)) ext = r.a.w;
        big = ext;
    }
    pl_utoa(t, conv); pl_strcat(t, " KB conventional, ");
    pl_strcat(t, pl_size(h, (big + 1024) * 2)); pl_strcat(t, " total");
    row(10, "Memory", t, C_TEXT);

    t[0] = 0;
    pl_strcat(t, tui_vga ? "VGA" : tui_mono ? "Monochrome" : "EGA/CGA");
    pl_strcat(t, tui_fancy ? ", Polaris glyphs, 8-dot text" : ", standard font");
    row(11, "Display", t, C_TEXT);

    {
        u8 FAR *bd = MKFP(0xF000, 0xFFF5);
        u8 i;
        for (i = 0; i < 8; i++) t[i] = (char)bd[i];
        t[8] = 0;
        row(12, "BIOS date", t, C_TEXT);
    }
    r.a.w = 0x5300; r.b.w = 0;
    pl_intr(0x15, &r);
    if (!(r.flags & CF_FLAG) && r.b.w == 0x504D) {
        pl_strncpy(t, "APM ", 8);
        h[0] = (char)('0' + r.a.b.h); h[1] = '.'; h[2] = (char)('0' + (r.a.b.l >> 4));
        h[3] = (char)('0' + (r.a.b.l & 15)); h[4] = 0;
        pl_strcat(t, h);
    } else pl_strncpy(t, "No APM BIOS", 16);
    pl_strcat(t, pci_present ? ", PCI bus" : ", no PCI");
    row(13, "Platform", t, C_TEXT);

    pl_utoa(t, ndisks); pl_strcat(t, " hard disk(s), ");
    pl_utoa(h, nparts); pl_strcat(t, h); pl_strcat(t, " partition(s), ");
    pl_utoa(h, nfloppies); pl_strcat(t, h); pl_strcat(t, " floppy, ");
    pl_utoa(h, natapi); pl_strcat(t, h); pl_strcat(t, " ATAPI");
    row(14, "Storage", t, C_TEXT);

    tui_icon(6, 16, PI_STAR, A(C_GLOW, C_PANEL));
    tui_puts(9, 16, "Polaris", A(C_ICE, C_PANEL));
    pl_strncpy(t, "Version " PL_VERSION_STR " \xF9 running from ", 64);
    pl_strcat(t, ui_where);
    row(16, "", t, C_TEXT);
    row(17, "", "Resident INT 13h: drive swap, El Torito no-emu,", C_DIM);
    row(18, "", "floppy and hard disk emulation, own ATAPI driver,", C_DIM);
    row(19, "", "partition hiding, BPB repair, passwords, themes.", C_DIM);
    row(21, "", "MIT License \xF9 part of Retrolities", C_BORDER);
    draw_footer(keys);
    tui_flush();
    while (tui_getkey() != K_ESC) ;
}

/* ======================================================================
 * setup editor
 * ==================================================================== */

static const char *const icon_names[PI_COUNT] = {
    "Disk", "DOS", "Windows 9x", "Windows NT", "OS/2", "Linux", "BSD", "CD-ROM",
    "Floppy", "ZIP", "Network", "Power", "Restart", "Setup", "Star", "Lock"
};

enum { F_NAME, F_HOTKEY, F_ICON, F_TARGET, F_ACTIVE, F_HIDE, F_UNHIDE, F_SWAP, F_FIXBPB,
       F_NOCHECK, F_EMU, F_PASSWORD, F_DEFAULT, F_COUNT };

static u8 field_used(struct pl_entry *e, u8 f)
{
    u8 part = e->kind == PK_PART, atapi = e->kind == PK_CDROM || e->kind == PK_ATAPIDISK;
    u8 boot = part || atapi || e->kind == PK_DISK || e->kind == PK_FLOPPY;
    switch (f) {
    case F_ACTIVE: case F_HIDE: case F_UNHIDE: case F_FIXBPB: return part;
    case F_SWAP: return part || e->kind == PK_DISK || e->kind == PK_FLOPPY;
    case F_NOCHECK: return part || e->kind == PK_DISK || e->kind == PK_FLOPPY;
    case F_EMU: return atapi;
    case F_TARGET: return boot;
    case F_PASSWORD: return e->kind != PK_SEPARATOR;
    case F_HOTKEY: return e->kind != PK_SEPARATOR;
    }
    return 1;
}

static void draw_entry_form(struct pl_entry *e, u8 sel, u8 isdef)
{
    static const char *const labels[F_COUNT] = {
        "Name", "Hot key", "Icon", "Boots", "Make partition active", "Hide primaries",
        "Unhide the partition", "Present disk as 80h", "Repair BPB in memory",
        "Boot without 55AAh", "ATAPI emulation", "Password", "Default entry"
    };
    static const char *const emu[] = { "Automatic", "None (2048-byte)", "Floppy (drive 00h)",
                                       "Hard disk (drive 80h)" };
    u8 f, y = 7, x = 6;
    char t[48];
    tui_card(3, 6, 74, 17, "Edit entry", C_ICE, C_PANEL);
    tui_shadow(3, 6, 74, 17);
    for (f = 0; f < F_COUNT; f++, y++) {
        u8 on = f == sel, used = field_used(e, f);
        u8 bg = on ? C_SELECT : C_PANEL;
        u8 la = A(used ? (on ? C_TEXT : C_DIM) : C_BORDER, bg), va = A(used ? C_TEXT : C_BORDER, bg);
        tui_fill(4, y, 72, 1, ' ', A(C_TEXT, bg));
        if (on) tui_put(4, y, G_ACCENT, A(C_GOLD, bg));
        tui_puts(x, y, labels[f], la);
        x = 30;
        switch (f) {
        case F_NAME: tui_puts(x, y, e->name, va); break;
        case F_HOTKEY:
            t[0] = e->hotkey ? (char)e->hotkey : '-'; t[1] = 0;
            tui_puts(x, y, t, A(C_GOLD, bg));
            tui_puts(x + 3, y, "(press a letter or digit, Del clears)", A(C_BORDER, bg));
            break;
        case F_ICON:
            tui_put(x, y, 0x11, A(C_DIM, bg));
            tui_icon(x + 2, y, e->icon, A(C_ICE, bg));
            tui_puts(x + 5, y, icon_names[e->icon % PI_COUNT], va);
            tui_put(x + 17, y, 0x10, A(C_DIM, bg));
            break;
        case F_TARGET:
            if (used) {
                entry_location(e, t);
                x = tui_puts(x, y, t, va);
                tui_puts(x + 2, y, kind_name(e->kind), A(C_DIM, bg));
            } else tui_puts(x, y, kind_name(e->kind), va);
            x = 30;
            break;
        case F_ACTIVE: tui_toggle(x, y, (e->flags & PF_ACTIVE) != 0, bg); break;
        case F_HIDE: {
            u8 i;
            for (i = 0; i < 4; i++) {
                tui_check(x + i * 5, y, (e->hidemask >> i) & 1, A(used ? C_ICE : C_BORDER, bg));
                t[0] = 'P'; t[1] = (char)('1' + i); t[2] = 0;
                tui_puts(x + i * 5 + 1, y, t, va);
            }
            tui_puts(x + 21, y, "(1-4 toggle)", A(C_BORDER, bg));
            break;
        }
        case F_UNHIDE: tui_toggle(x, y, (e->flags & PF_UNHIDE) != 0, bg); break;
        case F_SWAP:
            tui_toggle(x, y, (e->flags & PF_SWAP) != 0, bg);
            tui_puts(x + 3, y, "second, SCSI or USB disk as first", A(C_BORDER, bg));
            break;
        case F_FIXBPB: tui_toggle(x, y, (e->flags & PF_FIXBPB) != 0, bg); break;
        case F_NOCHECK: tui_toggle(x, y, (e->flags & PF_NOCHECK) != 0, bg); break;
        case F_EMU: tui_puts(x, y, emu[e->emu & 3], va); break;
        case F_PASSWORD:
            if (e->flags & PF_PASSWORD) {
                tui_icon(x, y, PI_LOCK, A(C_GOLD, bg));
                tui_puts(x + 3, y, "Protected (Enter changes, Del removes)", va);
            } else tui_puts(x, y, "None (Enter sets one)", va);
            break;
        case F_DEFAULT: tui_toggle(x, y, isdef, bg); break;
        }
        x = 6;
    }
}

/* edit one entry in place; returns 1 if accepted */
int setup_entry(struct pl_entry *e)
{
    static const char *const keys[] = { "\x18\x19", "Field", "Space", "Change", "Enter", "Edit",
                                        "F10", "Done", "Esc", "Cancel", 0, 0 };
    struct pl_entry w = *e;
    u8 sel = 0, isdef = (u8)(cfg.deflt == (u8)(e - cfg.entry));
    for (;;) {
        u16 k;
        u8 c;
        draw_backdrop("Setup \xF9 entry");
        draw_entry_form(&w, sel, isdef);
        draw_footer(keys);
        tui_flush();
        k = tui_getkey();
        c = KEYCHAR(k);
        if (k == K_ESC) return 0;
        if (k == K_F10) {
            *e = w;
            if (isdef) cfg.deflt = (u8)(e - cfg.entry);
            return 1;
        }
        if (k == K_UP) { do sel = sel ? sel - 1 : F_COUNT - 1; while (!field_used(&w, sel)); continue; }
        if (k == K_DOWN || k == K_TAB) { do sel = (u8)((sel + 1) % F_COUNT); while (!field_used(&w, sel)); continue; }
        switch (sel) {
        case F_NAME:
            if (k == K_ENTER) {
                char b[28];
                pl_strncpy(b, w.name, sizeof b);
                if (tui_ask("Entry name", "Name shown in the boot menu:", b, 26, 0))
                    pl_strncpy(w.name, b, sizeof w.name);
            }
            break;
        case F_HOTKEY:
            if (k == K_DEL || k == K_BS) w.hotkey = 0;
            else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                w.hotkey = (u8)pl_upper(c);
            break;
        case F_ICON:
            if (k == K_RIGHT || k == K_SPACE || k == K_ENTER) w.icon = (u8)((w.icon + 1) % PI_COUNT);
            if (k == K_LEFT) w.icon = (u8)((w.icon + PI_COUNT - 1) % PI_COUNT);
            break;
        case F_TARGET:
            if (k == K_ENTER || k == K_SPACE) {
                struct pl_entry t;
                if (devices_run(1, &t)) {
                    pl_memcpy(t.name, w.name, sizeof t.name);
                    t.hotkey = w.hotkey;
                    t.pw = w.pw;
                    t.flags |= w.flags & PF_PASSWORD;
                    w = t;
                }
            }
            break;
        case F_HIDE:
            if (c >= '1' && c <= '4') w.hidemask ^= (u8)(1 << (c - '1'));
            break;
        case F_EMU:
            if (k == K_RIGHT || k == K_SPACE || k == K_ENTER) w.emu = (u8)((w.emu + 1) & 3);
            if (k == K_LEFT) w.emu = (u8)((w.emu + 3) & 3);
            break;
        case F_PASSWORD:
            if (k == K_DEL) { w.flags &= ~PF_PASSWORD; w.pw = 0; }
            else if (k == K_ENTER) {
                char b[24];
                b[0] = 0;
                if (tui_ask("Entry password", "New password for this entry:", b, 20, 1) && b[0]) {
                    w.pw = pl_hash(b);
                    w.flags |= PF_PASSWORD;
                }
            }
            break;
        case F_DEFAULT:
            if (k == K_SPACE || k == K_ENTER || k == K_LEFT || k == K_RIGHT) isdef = !isdef;
            break;
        default:
            if (k == K_SPACE || k == K_ENTER || k == K_LEFT || k == K_RIGHT) {
                static const u8 bit[F_COUNT] = { 0, 0, 0, 0, PF_ACTIVE, 0, PF_UNHIDE, PF_SWAP,
                                                 PF_FIXBPB, PF_NOCHECK, 0, 0, 0 };
                w.flags ^= bit[sel];
                if (sel == F_SWAP && !w.vdrive) w.vdrive = 0x80;
            }
        }
    }
}

void setup_options(void)
{
    static const char *const keys[] = { "\x18\x19", "Option", "\x1B\x1A", "Change", "Enter", "Edit",
                                        "Esc", "Done", 0, 0 };
    static const char *const labels[] = {
        "Menu timeout", "Colour theme", "Default = last booted", "Quiet boot (hide menu)",
        "Animated night sky", "Polaris glyphs (VGA)", "Menu title", "Setup password"
    };
    u8 sel = 0, i;
    char t[40];
    for (;;) {
        u16 k;
        draw_backdrop("Setup \xF9 options");
        tui_card(8, 7, 64, 14, "Options", C_ICE, C_PANEL);
        tui_shadow(8, 7, 64, 14);
        for (i = 0; i < 8; i++) {
            u8 y = 9 + i, on = i == sel, bg = on ? C_SELECT : C_PANEL, x = 40;
            tui_fill(9, y, 62, 1, ' ', A(C_TEXT, bg));
            if (on) tui_put(9, y, G_ACCENT, A(C_GOLD, bg));
            tui_puts(11, y, labels[i], A(on ? C_TEXT : C_DIM, bg));
            switch (i) {
            case 0:
                if (cfg.timeout == PL_TIMEOUT_NEVER) pl_strncpy(t, "Wait for a key", 20);
                else { pl_utoa(t, cfg.timeout); pl_strcat(t, " seconds"); }
                tui_put(x, y, 0x11, A(C_DIM, bg));
                tui_puts(x + 2, y, t, A(C_TEXT, bg));
                tui_put(x + 18, y, 0x10, A(C_DIM, bg));
                break;
            case 1:
                tui_put(x, y, 0x11, A(C_DIM, bg));
                tui_puts(x + 2, y, theme_name[cfg.theme % TH_COUNT], A(C_TEXT, bg));
                tui_put(x + 18, y, 0x10, A(C_DIM, bg));
                {
                    u8 c;
                    for (c = 0; c < 8; c++) tui_put(x + 21 + c, y, G_BAR8, A(c + 8, bg));
                }
                break;
            case 2: tui_toggle(x, y, (cfg.flags & CF_REMEMBER) != 0, bg); break;
            case 3: tui_toggle(x, y, (cfg.flags & CF_QUIET) != 0, bg); break;
            case 4: tui_toggle(x, y, !(cfg.flags & CF_NOSTARS), bg); break;
            case 5: tui_toggle(x, y, !(cfg.flags & CF_NOFONT), bg); break;
            case 6: tui_putsn(x, y, cfg.title, A(C_TEXT, bg), 30); break;
            case 7: tui_puts(x, y, cfg.setup_pw ? "Set (Del removes)" : "None", A(C_TEXT, bg)); break;
            }
        }
        draw_footer(keys);
        tui_flush();
        k = tui_getkey();
        if (k == K_ESC || k == K_F10) return;
        if (k == K_UP) sel = sel ? sel - 1 : 7;
        else if (k == K_DOWN || k == K_TAB) sel = (u8)((sel + 1) % 8);
        else if (sel == 0 && (k == K_LEFT || k == K_RIGHT)) {
            static const u8 steps[] = { 0, 1, 3, 5, 10, 15, 20, 30, 60, 99, PL_TIMEOUT_NEVER };
            u8 j = 0;
            while (j < sizeof steps - 1 && steps[j] < cfg.timeout) j++;
            if (k == K_RIGHT && j < sizeof steps - 1) j++;
            if (k == K_LEFT && j) j--;
            cfg.timeout = steps[j];
        } else if (sel == 1 && (k == K_LEFT || k == K_RIGHT || k == K_ENTER || k == K_SPACE)) {
            cfg.theme = (u8)((cfg.theme + (k == K_LEFT ? TH_COUNT - 1 : 1)) % TH_COUNT);
            tui_theme(cfg.theme);
        } else if (sel >= 2 && sel <= 5 && (k == K_SPACE || k == K_ENTER || k == K_LEFT || k == K_RIGHT)) {
            static const u8 bits[] = { CF_REMEMBER, CF_QUIET, CF_NOSTARS, CF_NOFONT };
            cfg.flags ^= bits[sel - 2];
        } else if (sel == 6 && k == K_ENTER) {
            char b[32];
            pl_strncpy(b, cfg.title, sizeof b);
            if (tui_ask("Menu title", "Text shown above the boot menu:", b, 31, 0))
                pl_strncpy(cfg.title, b, sizeof cfg.title);
        } else if (sel == 7 && k == K_DEL) cfg.setup_pw = 0;
        else if (sel == 7 && k == K_ENTER) {
            char b[24];
            b[0] = 0;
            if (tui_ask("Setup password", "Password required to enter Setup:", b, 20, 1) && b[0])
                cfg.setup_pw = pl_hash(b);
        }
    }
}

static void draw_setup_list(u8 sel, u8 top, u8 vis)
{
    u8 i;
    char t[40];
    tui_card(2, 6, 76, 17, "Menu entries", C_BORDER, C_PANEL);
    tui_shadow(2, 6, 76, 17);
    tui_puts(6, 7, "Name", A(C_BORDER, C_PANEL));
    tui_puts(36, 7, "Boots", A(C_BORDER, C_PANEL));
    tui_puts(58, 7, "Options", A(C_BORDER, C_PANEL));
    for (i = 0; i < vis && top + i < cfg.count; i++) {
        struct pl_entry *e = &cfg.entry[top + i];
        u8 y = 8 + i, on = top + i == sel, bg = on ? C_SELECT : C_PANEL, x;
        tui_fill(3, y, 74, 1, ' ', A(C_TEXT, bg));
        if (on) tui_put(3, y, G_ACCENT, A(C_ICE, bg));
        if (e->kind == PK_SEPARATOR) {
            tui_puts(6, y, e->name, A(C_VIOLET, bg));
            tui_hline((u8)(7 + pl_strlen(e->name)), y, 20, A(C_BORDER, bg));
            continue;
        }
        tui_icon(5, y, e->icon, A(C_ICE, bg));
        tui_putsn(8, y, e->name, A(C_TEXT, bg), 27);
        entry_location(e, t);
        tui_putsn(36, y, t, A(C_DIM, bg), 21);
        x = 58;
        if (top + i == cfg.deflt) { tui_put(x, y, G_STAR3, A(C_GOLD, bg)); x += 2; }
        if (e->hotkey) { t[0] = (char)e->hotkey; t[1] = 0; tui_puts(x, y, t, A(C_GOLD, bg)); x += 2; }
        if (e->flags & PF_PASSWORD) { tui_icon(x, y, PI_LOCK, A(C_PINK, bg)); x += 3; }
        if (e->flags & PF_SWAP) { tui_puts(x, y, "swap", A(C_LGREEN, bg)); x += 5; }
        if (e->hidemask) { tui_puts(x, y, "hide", A(C_AMBER, bg)); x += 5; }
        if (e->flags & PF_ACTIVE) tui_puts(x, y, "act", A(C_DIM, bg));
    }
    if (!cfg.count) tui_puts(6, 9, "The menu is empty. Press Ins to add an entry or F9 to detect.",
                             A(C_DIM, C_PANEL));
    tui_scrollbar(76, 8, vis, top, vis, cfg.count, A(C_BORDER, C_PANEL));
}

int setup_run(u8 installer)
{
    static const char *const keys[] = { "Enter", "Edit", "Ins", "Add", "Del", "Remove",
                                        "+/-", "Move", "F5", "Options", "F9", "Detect",
                                        "F10", "Save", "Esc", "Quit", 0, 0 };
    static struct pl_config backup;
    u8 sel = 0, top = 0, vis = 14, dirty = 0;
    backup = cfg;
    (void)installer;
    for (;;) {
        u16 k;
        u8 c;
        if (cfg.count && sel >= cfg.count) sel = cfg.count - 1;
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        draw_backdrop(dirty ? "Setup \xF9 unsaved changes" : "Setup");
        draw_setup_list(sel, top, vis);
        draw_footer(keys);
        tui_flush();
        k = tui_getkey();
        c = KEYCHAR(k);
        if (k == K_UP && sel) sel--;
        else if (k == K_DOWN && sel + 1 < cfg.count) sel++;
        else if (k == K_HOME) sel = 0;
        else if (k == K_END && cfg.count) sel = cfg.count - 1;
        else if (k == K_ENTER && cfg.count) dirty |= (u8)setup_entry(&cfg.entry[sel]);
        else if (k == K_INS) {
            struct pl_entry e;
            if (cfg.count >= PL_MAX_ENTRIES)
                tui_msg("Menu full", "Remove an entry first.", "OK", MT_WARN);
            else if (devices_run(1, &e)) {
                u8 at = cfg.count ? sel + 1 : 0;
                cfg_insert(&cfg, at, &e);
                sel = at;
                setup_entry(&cfg.entry[sel]);
                dirty = 1;
            }
        } else if (k == K_DEL && cfg.count) {
            if (tui_msg("Remove entry", "Remove this entry from the menu?", "Remove|Keep", MT_WARN) == 0) {
                cfg_delete(&cfg, sel);
                dirty = 1;
            }
        } else if ((c == '-' || c == '+') && cfg.count) {
            s8 d = c == '-' ? -1 : 1;
            if ((d < 0 && sel) || (d > 0 && sel + 1 < cfg.count)) {
                cfg_move(&cfg, sel, d);
                sel = (u8)(sel + d);
                dirty = 1;
            }
        } else if (k == K_F5) { setup_options(); dirty = 1; }
        else if (k == K_F9) {
            if (tui_msg("Detect", "Replace the menu with everything Polaris can find?",
                        "Replace|Cancel", MT_WARN) == 0) {
                tui_progress("Scanning", "Looking for disks, partitions and ATAPI drives...", 1, 2);
                disk_scan(); atapi_scan(0);
                cfg_autodetect(&cfg);
                tui_progress(0, 0, 0, 0);
                dirty = 1;
            }
        } else if (k == K_F10) {
            cfg_seal(&cfg);
            if (cfg_store()) {
                tui_msg("Not saved", "The configuration could not be written to the disk.", "OK", MT_ERROR);
            } else {
                tui_msg("Saved", "The configuration was saved.", "OK", MT_OK);
                return 1;
            }
        } else if (k == K_ESC) {
            if (!dirty) return 0;
            {
                int r = tui_msg("Unsaved changes", "Save the changes before leaving Setup?",
                                "Save|Discard|Stay", MT_WARN);
                if (r == 0) {
                    cfg_seal(&cfg);
                    if (!cfg_store()) return 1;
                    tui_msg("Not saved", "The configuration could not be written.", "OK", MT_ERROR);
                } else if (r == 1) {
                    cfg = backup;
                    tui_theme(cfg.theme);
                    return 0;
                }
            }
        }
    }
}
