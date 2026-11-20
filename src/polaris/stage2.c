/* Polaris - stage 2: the boot menu.
 *
 * Loaded by stage 1 at 1000:0100 with DL = boot drive.  Reads the
 * configuration, discovers disks/partitions/ATAPI drives and shows the menu.
 */
#include "polaris.h"
#include "disk.h"
#include "atapi.h"
#include "config.h"
#include "tui.h"
#include "ui.h"
#include "boot.h"

extern u8 boot_drive;
extern int (*ui_boot)(struct pl_entry *e);
extern void (*ui_tick)(void);
extern const char *ui_where;

u8 cfg_drive;
static struct pl_info info;
static u8 booting = PL_NO_ENTRY;
static char where[24];

int cfg_store(void)
{
    u8 i;
    for (i = 0; i < PL_CONFIG_SECTS; i++)
        if (disk_write(cfg_drive, info.config_lba + i, (u8 FAR *)&cfg + i * 512)) return -1;
    return 0;
}

void boot_before_jump(void)
{
    u8 save = 0;
    if ((cfg.flags & CF_REMEMBER) && booting != PL_NO_ENTRY && cfg.last != booting) {
        cfg.last = booting;
        save = 1;
    }
    if (save) {
        cfg_seal(&cfg);
        cfg_store();
    }
}

/* ======================================================================
 * the menu
 * ==================================================================== */

static u8 sel;
static u8 top;
static s32 countdown;          /* ticks left, < 0 = stopped */
static u32 cd_total;
static u32 cd_last;
#define VIS 15

static u8 selectable(u8 i)
{
    return i < cfg.count && cfg.entry[i].kind != PK_SEPARATOR;
}

static void draw_countdown(void)
{
    char t[64], n[8];
    tui_fill(0, 23, SW, 1, ' ', A(C_TEXT, C_BG));
    if (countdown < 0 || !selectable(sel)) {
        tui_puts(3, 23, "Timer stopped \xF9 choose an entry and press Enter", A(C_BORDER, C_BG));
        return;
    }
    pl_strncpy(t, "Starting ", sizeof t);
    pl_strcat(t, cfg.entry[sel].name);
    pl_strcat(t, " in ");
    pl_utoa(n, (u32)(countdown * 10 + 181) / 182);
    pl_strcat(t, n);
    pl_strcat(t, " s");
    tui_putsn(3, 23, t, A(C_TEXT, C_BG), 44);
    tui_bar(48, 23, 29, (u32)countdown, cd_total, C_ICE, C_BG);
}

static void chip(u8 *x, u8 y, const char *s, u8 col)
{
    tui_put(*x, y, G_KEYL, A(col, C_PANEL));
    *x = tui_puts(*x + 1, y, s, A(C_DEEP, col));
    tui_put(*x, y, G_KEYR, A(col, C_PANEL));
    *x += 2;
}

static void draw_details(struct pl_entry *e)
{
    char t[40];
    u8 x = 56, y = 8, i;
    if (e->kind == PK_SEPARATOR) return;
    tui_icon(x, y, e->icon, A(C_ICE, C_PANEL));
    tui_puts(x + 3, y, kind_name(e->kind), A(C_DIM, C_PANEL));
    /* name, wrapped over two lines */
    pl_strncpy(t, e->name, sizeof t);
    if (pl_strlen(t) > 21) {
        u8 cut = 21;
        while (cut > 8 && t[cut] != ' ') cut--;
        if (t[cut] == ' ') {
            t[cut] = 0;
            tui_putsn(x, y + 3, t + cut + 1, A(C_ICE, C_PANEL), 21);
        }
    }
    tui_putsn(x, y + 2, t, A(C_ICE, C_PANEL), 21);

    tui_puts(x, y + 5, "Location", A(C_BORDER, C_PANEL));
    entry_location(e, t);
    tui_putsn(x, y + 6, t, A(C_TEXT, C_PANEL), 21);
    tui_puts(x, y + 7, "Type", A(C_BORDER, C_PANEL));
    entry_detail(e, t);
    tui_putsn(x, y + 8, t, A(C_TEXT, C_PANEL), 21);

    x = 56;
    if (e->flags & PF_ACTIVE) chip(&x, y + 10, "active", C_GREEN);
    if (e->flags & PF_SWAP) chip(&x, y + 10, "as 80h", C_SELECT);
    if (e->flags & PF_PASSWORD) chip(&x, y + 10, "locked", C_PINK);
    x = 56;
    if (e->hidemask) {
        t[0] = 0;
        pl_strcat(t, "hides");
        for (i = 0; i < 4; i++)
            if (e->hidemask & (1 << i)) { u8 n = (u8)pl_strlen(t); t[n] = ' '; t[n + 1] = 'P';
                                          t[n + 2] = (char)('1' + i); t[n + 3] = 0; }
        chip(&x, y + 11, t, C_AMBER);
    }
    if (e->kind == PK_PART || e->kind == PK_DISK) {
        struct bdisk *d = disk_find(e->drive);
        draw_diskmap(56, y + 12, 20, e->drive, e->kind == PK_PART ? e->lba : 0xFFFFFFFFUL, C_PANEL);
        if (d) {
            pl_strncpy(t, "Disk ", sizeof t);
            hexbyte(t + 5, e->drive);
            pl_strcat(t, " \xF9 ");
            pl_size(t + pl_strlen(t), d->sectors);
            tui_puts(56, y + 13, t, A(C_DIM, C_PANEL));
        } else tui_puts(56, y + 13, "Disk not present", A(C_PINK, C_PANEL));
    }
}

static void draw_menu(void)
{
    static const char *const keys[] = { "\x18\x19", "Select", "Enter", "Boot", "F2", "Setup",
                                        "F3", "Devices", "F4", "System", "F10", "Power", 0, 0 };
    u8 i;
    char t[8];
    draw_backdrop(cfg.title);
    tui_card(2, 6, 50, 17, "Boot menu", C_BORDER, C_PANEL);
    tui_card(54, 6, 24, 17, "Details", C_BORDER, C_PANEL);
    tui_shadow(2, 6, 50, 17);
    tui_shadow(54, 6, 24, 17);
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    for (i = 0; i < VIS && top + i < cfg.count; i++) {
        struct pl_entry *e = &cfg.entry[top + i];
        u8 y = 7 + i, on = top + i == sel, bg = on ? C_SELECT : C_PANEL;
        if (e->kind == PK_SEPARATOR) {
            u8 x = tui_puts(5, y, e->name, A(C_VIOLET, C_PANEL));
            tui_hline(x + 1, y, (u8)(48 - x), A(C_BORDER, C_PANEL));
            continue;
        }
        tui_fill(3, y, 48, 1, ' ', A(C_TEXT, bg));
        if (on) tui_put(3, y, G_ACCENT, A(C_ICE, bg));
        tui_icon(5, y, e->icon, A(on ? C_TEXT : C_ICE, bg));
        tui_putsn(8, y, e->name, A(on ? C_TEXT : C_TEXT, bg), 30);
        if (top + i == cfg.deflt) tui_put(39, y, G_STAR3, A(C_GOLD, bg));
        if (e->flags & PF_PASSWORD) tui_icon(41, y, PI_LOCK, A(C_PINK, bg));
        if (e->hotkey) {
            t[0] = (char)e->hotkey; t[1] = 0;
            tui_put(45, y, G_KEYL, A(on ? C_ICE : C_BORDER, bg));
            tui_puts(46, y, t, A(on ? C_DEEP : C_GOLD, on ? C_ICE : C_BORDER));
            tui_put(47, y, G_KEYR, A(on ? C_ICE : C_BORDER, bg));
        }
    }
    tui_scrollbar(50, 7, VIS, top, VIS, cfg.count, A(C_BORDER, C_PANEL));
    if (!cfg.count) {
        tui_puts(6, 9, "No boot entries yet.", A(C_TEXT, C_PANEL));
        tui_puts(6, 11, "Press F2 for Setup, or F3 to browse", A(C_DIM, C_PANEL));
        tui_puts(6, 12, "and boot any device directly.", A(C_DIM, C_PANEL));
    } else if (selectable(sel)) draw_details(&cfg.entry[sel]);
    draw_countdown();
    draw_footer(keys);
}

static void menu_tick(void)
{
    u32 now = pl_ticks();
    if (countdown >= 0) {
        countdown -= (s32)(now - cd_last);
        if (countdown < 0) countdown = 0;
        draw_countdown();
    }
    cd_last = now;
}

static void stop_timer(void)
{
    if (countdown >= 0) countdown = -1;
}

static int start(u8 i)
{
    if (!selectable(i)) return -1;
    booting = i;
    stop_timer();
    boot_entry(&cfg.entry[i]);        /* returns only on failure */
    booting = PL_NO_ENTRY;
    return -1;
}

static int boot_now(struct pl_entry *e)
{
    stop_timer();
    return boot_entry(e);
}

static void power_menu(void)
{
    static const char *const items[] = { "Restart the computer", "Power off",
                                         "Next BIOS boot device (INT 18h)", "Back to the menu" };
    struct pl_entry e;
    int r = tui_choose("Power", items, 4, 0);
    if (r == 0) boot_reboot();
    else if (r == 1) boot_poweroff();
    else if (r == 2) { entry_simple(&e, PK_BIOSNEXT, 0); boot_entry(&e); }
}

static void setup(void)
{
    if (cfg.setup_pw) {
        char pw[24];
        pw[0] = 0;
        if (!tui_ask("Setup", "Enter the setup password:", pw, 20, 1)) return;
        if (pl_hash(pw) != cfg.setup_pw) {
            tui_msg("Access denied", "Wrong password.", "OK", MT_ERROR);
            return;
        }
    }
    ui_tick = 0;
    setup_run(0);
    ui_tick = menu_tick;
    if (sel >= cfg.count) sel = 0;
}

static void quiet_wait(void)
{
    /* minimal splash; any key opens the menu */
    while (countdown > 0) {
        u16 k;
        draw_backdrop(cfg.title);
        tui_center(0, SW, 12, "Press any key for the boot menu", A(C_DIM, C_BG));
        tui_bar(25, 14, 30, (u32)countdown, cd_total, C_ICE, C_BG);
        tui_flush();
        if ((k = pl_key_peek()) != 0) {
            pl_key_get();
            stop_timer();
            return;
        }
        ui_idle();
        menu_tick();
        cpu_halt();
    }
}

static void menu_run(void)
{
    sel = cfg.deflt;
    if ((cfg.flags & CF_REMEMBER) && cfg.last < cfg.count) sel = cfg.last;
    while (sel < cfg.count && !selectable(sel)) sel++;
    if (sel >= cfg.count) sel = 0;
    cd_last = pl_ticks();
    if (cfg.timeout == PL_TIMEOUT_NEVER || !cfg.count || !selectable(sel)) countdown = -1;
    else countdown = (s32)cfg.timeout * 182 / 10;
    cd_total = countdown > 0 ? (u32)countdown : 1;
    if (cfg.flags & CF_NOSTARS) tui_idle = 0;
    if (countdown == 0) { start(sel); countdown = -1; }
    if (countdown > 0 && (cfg.flags & CF_QUIET)) {
        quiet_wait();
        if (countdown == 0) { start(sel); countdown = -1; }
    }
    ui_tick = menu_tick;
    for (;;) {
        u16 k;
        u8 c, i;
        draw_menu();
        tui_flush();
        while (!(k = pl_key_peek())) {
            if (countdown == 0) break;
            if (tui_idle) tui_idle(); else menu_tick();
            cpu_halt();
        }
        if (!k) { start(sel); continue; }
        k = pl_key_get();
        stop_timer();
        c = (u8)pl_upper(KEYCHAR(k));
        if (k == K_UP) { i = sel; while (i && !selectable(--i)) ; if (selectable(i)) sel = i; }
        else if (k == K_DOWN) { i = sel; while (i + 1 < cfg.count && !selectable(++i)) ; if (selectable(i)) sel = i; }
        else if (k == K_HOME) { sel = 0; while (sel + 1 < cfg.count && !selectable(sel)) sel++; }
        else if (k == K_END && cfg.count) { sel = cfg.count - 1; while (sel && !selectable(sel)) sel--; }
        else if (k == K_PGUP) sel = sel > VIS ? sel - VIS : 0;
        else if (k == K_PGDN) sel = (u8)(sel + VIS < cfg.count ? sel + VIS : (cfg.count ? cfg.count - 1 : 0));
        else if (k == K_ENTER) start(sel);
        else if (k == K_F2) setup();
        else if (k == K_F3) devices_run(0, 0);
        else if (k == K_F4) sysinfo_run();
        else if (k == K_F10) power_menu();
        else if (c > ' ') {
            for (i = 0; i < cfg.count; i++)
                if (cfg.entry[i].hotkey == c && selectable(i)) { sel = i; draw_menu(); tui_flush(); start(i); break; }
        }
    }
}

void main(void)
{
    u8 i, fresh = 0;
    pl_fmemcpy((void FAR *)&info, MKFP(0, 0x7C00 + PL_INFO_OFS), sizeof info);
    if (pl_memcmp(info.magic, "PLRS", 4)) {
        info.config_lba = PL_LBA_CONFIG;
        info.media = boot_drive & 0x80 ? 0 : 1;
    }
    cfg_drive = boot_drive;
    for (i = 0; i < PL_CONFIG_SECTS; i++)
        if (disk_read(boot_drive, info.config_lba + i, (u8 FAR *)&cfg + i * 512)) break;
    if (i < PL_CONFIG_SECTS || !cfg_valid(&cfg)) {
        cfg_defaults(&cfg);
        fresh = 1;
    }
    pl_strncpy(where, boot_drive & 0x80 ? "hard disk " : "floppy ", sizeof where);
    hexbyte(where + pl_strlen(where), boot_drive);
    ui_where = where;
    ui_boot = boot_now;

    tui_init(cfg.theme, (cfg.flags & CF_NOFONT) != 0);
    tui_idle = ui_idle;
    draw_backdrop("Starting");
    tui_flush();
    if (cfg.flags & CF_SOUND) { rmregs r; r.a.w = 0x0E07; r.b.w = 0; pl_intr(0x10, &r); }
    disk_scan();
    atapi_scan(0);
    if (fresh || !cfg.count) {
        cfg_autodetect(&cfg);
        cfg_seal(&cfg);
    }
    if (cfg.oneshot < cfg.count) {
        u8 one = cfg.oneshot;
        cfg.oneshot = PL_NO_ENTRY;
        cfg_seal(&cfg);
        cfg_store();
        start(one);
    }
    menu_run();
}
