/* Polaris - text user interface.
 *
 * Drawing goes to an off-screen copy of the text page and is copied to video
 * memory by tui_flush(), so screens never flicker while they are rebuilt.  On
 * a VGA the attribute palette is made an identity map, blinking is turned
 * off (16 background colours), the DAC is loaded with the selected theme,
 * characters are switched to 8 dots wide and Polaris' glyphs are loaded.
 * Other adapters get CP437 stand-ins and the standard colours.
 */
#define GLYPHS_IMPL
#include "tui.h"

u16 scr[SH * SW];
u8  tui_vga, tui_fancy, tui_mono;
void (*tui_idle)(void);

static u8  xlat[256];
static u16 front[SH * SW];        /* what video memory holds right now */
static u8  front_valid;
static u8  dac_pending, dac_rgb[3];
static u16 status_port = 0x3DA;
/* screens saved under modal dialogs, kept outside the 64 KB data group:
 * stage 2 runs at 2000:0100, so 3000:0000 is free conventional memory */
#ifdef STAGE2
#define SAVED(n) ((u16 FAR *)MKFP(0x3000 + (n) * 0x100, 0))
#else
static u16 __far saved_buf[2][SH * SW];
#define SAVED(n) ((u16 FAR *)saved_buf[n])
#endif
static u8  depth;
static u8  cur_theme;
static u16 vseg = 0xB800;

const char *const theme_name[TH_COUNT] = {
    "Polaris Night", "Aurora", "Ember", "Glacier", "Classic VGA"
};

/* 16 colours x RGB (6-bit) per theme, indexed by the C_* roles */
static const u8 themes[TH_COUNT][48] = {
    {   2, 4,10,   5, 9,20,  12,46,32,   9,14,28,  54,16,20,  34,20,50,  58,42,12,  38,44,54,
       18,26,42,  12,30,54,  30,58,42,  40,56,63,  63,34,40,  50,46,63,  63,54,26,  60,62,63 },
    {   1, 6, 7,   3,11,13,  10,44,28,   6,17,19,  54,18,20,  38,22,52,  58,46,16,  36,48,46,
       12,30,32,   8,36,34,  30,63,46,  34,63,54,  63,36,46,  40,63,50,  63,58,30,  58,63,62 },
    {   6, 2, 1,  13, 6, 4,  20,44,22,  20,10, 6,  58,14,10,  44,18,30,  63,40,10,  50,40,34,
       36,20,12,  46,20, 8,  40,58,30,  63,48,26,  63,30,26,  63,42,22,  63,56,28,  63,60,56 },
    {  44,50,58,  54,58,62,  10,38,20,  62,63,63,  48,10,12,  32,16,44,  50,32, 4,  22,28,38,
       44,48,56,  40,52,63,  14,44,26,   8,24,48,  56,20,30,  20,36,60,  52,34, 0,   4, 8,16 },
    {   0, 0, 0,   0, 0,42,   0,42, 0,   0,42,42,  42, 0, 0,  42, 0,42,  42,21, 0,  42,42,42,
       21,21,21,  21,21,63,  21,63,21,  21,63,63,  63,21,21,  63,21,63,  63,63,21,  63,63,63 },
};

static u16 dseg(void)
{
    return FPSEG((void FAR *)scr);
}

/* ES = DS for the table calls; bp is the ES:BP offset (font loading) */
static void int10(u16 ax, u16 bx, u16 cx, u16 dx, u16 bp)
{
    rmregs r;
    r.a.w = ax; r.b.w = bx; r.c.w = cx; r.d.w = dx;
    r.es = r.ds = dseg();
    r.si.w = 0; r.di.w = 0; r.bp = bp;
    pl_intr(0x10, &r);
}

void tui_theme(u8 theme)
{
    u8 i;
    if (theme >= TH_COUNT) theme = TH_NIGHT;
    cur_theme = theme;
    if (!tui_vga) return;
    outb(0x3C8, 0);
    for (i = 0; i < 48; i++) outb(0x3C9, themes[theme][i]);
}

void tui_twinkle(u8 phase)
{
    const u8 *g = &themes[cur_theme][C_GLOW * 3];
    u8 t = phase & 31, i;
    if (!tui_vga) return;
    if (t > 15) t = 31 - t;          /* triangle 0..15 */
    for (i = 0; i < 3; i++)          /* applied by tui_flush() in the retrace */
        dac_rgb[i] = (u8)(g[i] / 3 + (u16)(63 - g[i] / 3) * t / 15);
    dac_pending = 1;
}

/* load one glyph through the BIOS (AX=1100h keeps the mode untouched) */
static void load_glyph(u8 code, const u8 *bits)
{
    int10(0x1100, 0x1000, 1, code, (u16)bits);
}

static void eight_dot(void)
{
    u8 v;
    cli();
    outb(0x3C4, 0); outb(0x3C5, 1);                   /* synchronous reset */
    outb(0x3C4, 1); v = inb(0x3C5); outb(0x3C5, v | 1);  /* 8-dot chars */
    v = inb(0x3CC); outb(0x3C2, v & ~0x0C);           /* 25 MHz dot clock */
    outb(0x3C4, 0); outb(0x3C5, 3);
    inb(0x3DA); outb(0x3C0, 0x13 | 0x20); outb(0x3C0, 0);  /* no panning */
    sti();
}

void tui_init(u8 theme, u8 nofont)
{
    rmregs r;
    u16 i;
    static const u8 identity[17] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0};

    for (i = 0; i < 256; i++) xlat[i] = (u8)i;
    r.a.w = 0x0F00;
    pl_intr(0x10, &r);
    tui_mono = (r.a.b.l == 7);
    front_valid = 0;
    if (tui_mono) { vseg = 0xB000; status_port = 0x3BA; }
    else if (r.a.b.l != 3) int10(0x0003, 0, 0, 0, 0);

    r.a.w = 0x1A00;
    pl_intr(0x10, &r);
    tui_vga = (r.a.b.l == 0x1A && (r.b.b.l == 7 || r.b.b.l == 8)) && !tui_mono;

    if (tui_vga) {
        int10(0x1002, 0, 0, (u16)identity, 0); /* attribute regs = identity */
        int10(0x1003, 0, 0, 0, 0);             /* intensity, not blinking */
        tui_theme(theme);
        if (!nofont) {
            for (i = 0; i < GLYPH_COUNT; i++)
                load_glyph(glyph_data[i][0], &glyph_data[i][1]);
            eight_dot();
            tui_fancy = 1;
        }
    } else {
        int10(0x1003, 0, 0, 0, 0);             /* EGA: still try */
    }
    if (!tui_fancy)
        for (i = 0; i < GLYPH_COUNT; i++)
            xlat[glyph_fallback[i][0]] = glyph_fallback[i][1];
    cur_theme = theme;
    tui_cursor(0, 0, 0);
}

void tui_done(void)
{
    front_valid = 0;
    int10(tui_mono ? 0x0007 : 0x0003, 0, 0, 0, 0);
}

void tui_cursor(u8 x, u8 y, u8 show)
{
    int10(0x0200, 0, 0, ((u16)y << 8) | x, 0);
    int10(0x0100, 0, show ? 0x0D0E : 0x2000, 0, 0);
}

/* Wait for the start of a vertical retrace so video memory and the DAC are
 * updated while the beam is off screen (no tearing, no CGA snow, no DAC
 * sparkles).  Bounded, so an adapter without the status bit cannot hang us. */
static void wait_vretrace(void)
{
    u16 n;
    for (n = 0; n < 0xC000 && (inb(status_port) & 8); n++) ;   /* leave a retrace */
    for (n = 0; n < 0xC000 && !(inb(status_port) & 8); n++) ;  /* start of the next */
}

static u16 cell(u16 i)
{
    u16 c = scr[i];
    u8 a = (u8)(c >> 8);
    if (tui_fancy) return c;
    if (tui_mono) {
        u8 bg = a >> 4;
        a = (bg == C_SELECT || bg == C_ICE || bg == C_TEXT) ? 0x70 :
            ((a & 15) == C_TEXT || (a & 15) == C_ICE || (a & 15) == C_GOLD) ? 0x0F : 0x07;
    }
    return ((u16)a << 8) | xlat[(u8)c];
}

/* Present the back buffer: only cells that differ from what is already on
 * screen are written, inside the vertical retrace. */
void tui_flush(void)
{
    u16 FAR *v = MKFP(vseg, 0);
    u16 i, first = 0xFFFF;
    for (i = 0; i < SH * SW; i++)
        if (!front_valid || cell(i) != front[i]) { first = i; break; }
    if (first == 0xFFFF && !dac_pending) return;
    wait_vretrace();
    if (dac_pending && tui_vga) {
        outb(0x3C8, C_GLOW);
        outb(0x3C9, dac_rgb[0]); outb(0x3C9, dac_rgb[1]); outb(0x3C9, dac_rgb[2]);
    }
    dac_pending = 0;
    if (first == 0xFFFF) return;
    for (i = first; i < SH * SW; i++) {
        u16 c = cell(i);
        if (!front_valid || c != front[i]) {
            v[i] = c;
            front[i] = c;
        }
    }
    front_valid = 1;
}

void tui_put(u8 x, u8 y, u8 ch, u8 attr)
{
    if (x < SW && y < SH) scr[y * SW + x] = ((u16)attr << 8) | ch;
}

u8 tui_puts(u8 x, u8 y, const char *s, u8 attr)
{
    while (*s && x < SW) tui_put(x++, y, (u8)*s++, attr);
    return x;
}

void tui_putsn(u8 x, u8 y, const char *s, u8 attr, u8 width)
{
    while (width--) {
        tui_put(x++, y, (u8)(*s ? *s++ : ' '), attr);
    }
}

void tui_putr(u8 xr, u8 y, const char *s, u8 attr)
{
    u16 n = pl_strlen(s);
    tui_puts((u8)(xr + 1 - n), y, s, attr);
}

void tui_center(u8 x, u8 w, u8 y, const char *s, u8 attr)
{
    u16 n = pl_strlen(s);
    if (n > w) n = w;
    tui_puts((u8)(x + (w - n) / 2), y, s, attr);
}

void tui_fill(u8 x, u8 y, u8 w, u8 h, u8 ch, u8 attr)
{
    u8 i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) tui_put(x + i, y + j, ch, attr);
}

void tui_recolor(u8 x, u8 y, u8 w, u8 h, u8 attr)
{
    u8 i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < SW && y + j < SH) {
                u16 *c = &scr[(y + j) * SW + x + i];
                *c = (*c & 0xFF) | ((u16)attr << 8);
            }
}

void tui_shadow(u8 x, u8 y, u8 w, u8 h)
{
    tui_recolor(x + w, y + 1, 2, h, A(C_BORDER, C_DEEP));
    tui_recolor(x + 2, y + h, w, 1, A(C_BORDER, C_DEEP));
}

void tui_hline(u8 x, u8 y, u8 w, u8 attr)
{
    while (w--) tui_put(x++, y, 0xC4, attr);
}

void tui_card(u8 x, u8 y, u8 w, u8 h, const char *title, u8 border, u8 bg)
{
    u8 a = A(border, bg), j;
    tui_fill(x, y, w, h, ' ', A(C_TEXT, bg));
    tui_put(x, y, G_RTL, a);
    tui_put(x + w - 1, y, G_RTR, a);
    tui_put(x, y + h - 1, G_RBL, a);
    tui_put(x + w - 1, y + h - 1, G_RBR, a);
    tui_hline(x + 1, y, w - 2, a);
    tui_hline(x + 1, y + h - 1, w - 2, a);
    for (j = 1; j < h - 1; j++) {
        tui_put(x, y + j, 0xB3, a);
        tui_put(x + w - 1, y + j, 0xB3, a);
    }
    if (title && *title) {
        u8 e;
        tui_put(x + 2, y, ' ', a);
        e = tui_puts(x + 3, y, title, A(C_ICE, bg));
        tui_put(e, y, ' ', a);
    }
}

void tui_bar(u8 x, u8 y, u8 w, u32 num, u32 den, u8 fg, u8 bg)
{
    u32 eighths;
    u8 i;
    if (!den) den = 1;
    if (num > den) num = den;
    eighths = num * (u32)w * 8 / den;
    for (i = 0; i < w; i++) {
        if (eighths >= 8) {
            tui_put(x + i, y, G_BAR8, A(fg, bg));
            eighths -= 8;
        } else if (eighths) {
            tui_put(x + i, y, (u8)(G_BAR1 + eighths - 1), A(fg, bg));
            eighths = 0;
        } else {
            tui_put(x + i, y, G_TRACK, A(C_BORDER, bg));
        }
    }
}

void tui_icon(u8 x, u8 y, u8 icon, u8 attr)
{
    if (icon >= 16) icon = 0;
    tui_put(x, y, icon_glyph[icon][0], attr);
    tui_put(x + 1, y, icon_glyph[icon][1], attr);
}

void tui_logo(u8 x, u8 y, u8 fg, u8 star, u8 bg)
{
    u8 r, c;
    if (!tui_fancy) {
        tui_puts(x, y, "\x0F  P O L A R I S", A(fg, bg));
        return;
    }
    for (r = 0; r < 2; r++)
        for (c = 0; c < LOGO_W; c++)
            tui_put(x + c, y + r, logo_map[r][c], A(c < 3 ? star : fg, bg));
}

u8 tui_key(u8 x, u8 y, const char *key, const char *label, u8 bg)
{
    tui_put(x, y, G_KEYL, A(C_BORDER, bg));
    x = tui_puts(x + 1, y, key, A(C_TEXT, C_BORDER));
    tui_put(x, y, G_KEYR, A(C_BORDER, bg));
    x = tui_puts(x + 2, y, label, A(C_DIM, bg));
    return x + 2;
}

void tui_toggle(u8 x, u8 y, u8 on, u8 bg)
{
    if (!tui_fancy) {
        tui_puts(x, y, on ? "On " : "Off", A(on ? C_LGREEN : C_DIM, bg));
        return;
    }
    tui_put(x, y, on ? G_TOGON1 : G_TOGOFF1, A(on ? C_LGREEN : C_DIM, bg));
    tui_put(x + 1, y, on ? G_TOGON2 : G_TOGOFF2, A(on ? C_LGREEN : C_DIM, bg));
}

void tui_check(u8 x, u8 y, u8 on, u8 attr)
{
    tui_put(x, y, on ? G_BOXON : G_BOXOFF, attr);
}

void tui_scrollbar(u8 x, u8 y, u8 h, u16 top, u16 visible, u16 total, u8 attr)
{
    u8 i, t0, t1;
    if (total <= visible) return;
    t0 = (u8)((u32)top * h / total);
    t1 = (u8)(((u32)(top + visible) * h + total - 1) / total);
    for (i = 0; i < h; i++)
        tui_put(x, y + i, (i >= t0 && i < t1) ? G_SCRTHUMB : G_SCRTRACK, attr);
}

void tui_push(void)
{
    if (depth < 2) pl_fmemcpy(SAVED(depth), (void FAR *)scr, sizeof scr);
    depth++;
}

void tui_pop(void)
{
    if (!depth) return;
    depth--;
    if (depth < 2) pl_fmemcpy((void FAR *)scr, SAVED(depth), sizeof scr);
    tui_flush();
}

u16 tui_getkey(void)
{
    for (;;) {
        if (pl_key_peek()) return pl_key_get();
        if (tui_idle) tui_idle();
        cpu_halt();
    }
}

int tui_input(u8 x, u8 y, u8 w, char *buf, u8 max, u8 secret)
{
    u8 len = (u8)pl_strlen(buf), pos = len, i;
    for (;;) {
        u8 first = pos >= w ? pos - w + 1 : 0;
        for (i = 0; i < w; i++) {
            u8 k = first + i;
            u8 ch = k < len ? (secret ? '*' : (u8)buf[k]) : ' ';
            tui_put(x + i, y, ch, A(C_TEXT, C_DEEP));
        }
        tui_flush();
        tui_cursor(x + pos - first, y, 1);
        {
            u16 k = tui_getkey();
            u8 c = KEYCHAR(k);
            if (k == K_ENTER) { tui_cursor(0, 0, 0); return 1; }
            if (k == K_ESC) { tui_cursor(0, 0, 0); return 0; }
            if (k == K_LEFT && pos) pos--;
            else if (k == K_RIGHT && pos < len) pos++;
            else if (k == K_HOME) pos = 0;
            else if (k == K_END) pos = len;
            else if (k == K_BS && pos) {
                pl_memcpy(buf + pos - 1, buf + pos, len - pos + 1);
                pos--, len--;
            } else if (k == K_DEL && pos < len) {
                pl_memcpy(buf + pos, buf + pos + 1, len - pos);
                len--;
            } else if (c >= 32 && c < 127 && len < max) {
                for (i = len + 1; i > pos; i--) buf[i] = buf[i - 1];
                buf[pos++] = (char)c;
                len++;
            }
        }
    }
}

static u8 tone_color(u8 tone)
{
    static const u8 c[4] = { C_ICE, C_AMBER, C_PINK, C_LGREEN };
    return c[tone & 3];
}

/* modal box helper: draws a centred card, returns its geometry */
static void modal(u8 w, u8 h, const char *title, u8 tone, u8 *ox, u8 *oy)
{
    u8 x = (u8)((SW - w) / 2), y = (u8)((SH - h) / 2);
    tui_card(x, y, w, h, title, tone_color(tone), C_PANEL);
    tui_fill(x, y + 1, 1, h - 2, G_ACCENT, A(tone_color(tone), C_PANEL));
    tui_shadow(x, y, w, h);
    *ox = x; *oy = y;
}

int tui_msg(const char *title, const char *text, const char *buttons, u8 tone)
{
    u8 lines = 1, wmax = (u8)(pl_strlen(title) + 6), cur = 0, n, nb = 1, bw = 0;
    const char *p;
    u8 x, y, w, h, i, sel = 0;
    char lb[80];

    for (p = text; *p; p++) {
        if (*p == '\n') { lines++; cur = 0; }
        else if (++cur + 6 > wmax) wmax = cur + 6;
    }
    for (p = buttons; *p; p++) {
        if (*p == '|') nb++;
        bw++;
    }
    bw += nb * 4;
    if (bw + 6 > wmax) wmax = bw + 6;
    if (wmax > 76) wmax = 76;
    w = wmax; h = (u8)(lines + 6);
    tui_push();
    modal(w, h, title, tone, &x, &y);
    p = text;
    for (i = 0; i < lines; i++) {
        n = 0;
        while (*p && *p != '\n' && n < 78) lb[n++] = *p++;
        lb[n] = 0;
        if (*p) p++;
        tui_puts(x + 3, y + 2 + i, lb, A(C_TEXT, C_PANEL));
    }
    for (;;) {
        u8 bx = (u8)(x + w - 2 - bw), b = 0;
        const char *q = buttons;
        while (*q) {
            n = 0;
            while (*q && *q != '|') lb[n++] = *q++;
            lb[n] = 0;
            if (*q) q++;
            {
                u8 a = b == sel ? A(C_TEXT, C_SELECT) : A(C_DIM, C_BORDER);
                tui_put(bx, y + h - 2, ' ', a);
                bx = tui_puts(bx + 1, y + h - 2, lb, a);
                tui_put(bx, y + h - 2, ' ', a);
                bx += 3;
            }
            b++;
        }
        tui_flush();
        {
            u16 k = tui_getkey();
            u8 c = (u8)pl_upper(KEYCHAR(k));
            if (k == K_ESC) { tui_pop(); return -1; }
            if (k == K_ENTER || k == K_SPACE) { tui_pop(); return sel; }
            if ((k == K_RIGHT || k == K_TAB) && sel + 1 < nb) sel++;
            else if ((k == K_LEFT || k == K_STAB) && sel) sel--;
            else if (c) {
                q = buttons; b = 0;
                while (*q) {
                    if (pl_upper(*q) == c) { tui_pop(); return b; }
                    while (*q && *q != '|') q++;
                    if (*q) q++;
                    b++;
                }
            }
        }
    }
}

int tui_choose(const char *title, const char *const *items, u8 n, u8 sel)
{
    u8 w = (u8)(pl_strlen(title) + 8), i, x, y, h, vis, top = 0;
    for (i = 0; i < n; i++)
        if (pl_strlen(items[i]) + 8 > w) w = (u8)(pl_strlen(items[i]) + 8);
    vis = n > 15 ? 15 : n;
    h = vis + 4;
    if (sel >= n) sel = 0;
    tui_push();
    modal(w, h, title, MT_INFO, &x, &y);
    for (;;) {
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        for (i = 0; i < vis; i++) {
            u8 k = top + i;
            u8 a = k == sel ? A(C_TEXT, C_SELECT) : A(C_TEXT, C_PANEL);
            tui_fill(x + 2, y + 2 + i, w - 4, 1, ' ', a);
            if (k == sel) tui_put(x + 2, y + 2 + i, G_CHEVRON, A(C_GOLD, C_SELECT));
            tui_puts(x + 4, y + 2 + i, items[k], a);
        }
        tui_scrollbar(x + w - 2, y + 2, vis, top, vis, n, A(C_BORDER, C_PANEL));
        tui_flush();
        {
            u16 k = tui_getkey();
            if (k == K_ESC) { tui_pop(); return -1; }
            if (k == K_ENTER) { tui_pop(); return sel; }
            if (k == K_UP) sel = sel ? sel - 1 : n - 1;
            else if (k == K_DOWN) sel = (u8)(sel + 1 < n ? sel + 1 : 0);
            else if (k == K_HOME) sel = 0;
            else if (k == K_END) sel = n - 1;
            else if (k == K_PGUP) sel = sel > vis ? sel - vis : 0;
            else if (k == K_PGDN) sel = (u8)(sel + vis < n ? sel + vis : n - 1);
        }
    }
}

int tui_ask(const char *title, const char *prompt, char *buf, u8 max, u8 secret)
{
    u8 w = 48, x, y, r;
    if (pl_strlen(prompt) + 6 > w) w = (u8)(pl_strlen(prompt) + 6);
    tui_push();
    modal(w, 7, title, secret ? MT_WARN : MT_INFO, &x, &y);
    tui_puts(x + 3, y + 2, prompt, A(C_TEXT, C_PANEL));
    tui_put(x + 3, y + 4, G_CHEVRON, A(C_GOLD, C_PANEL));
    r = (u8)tui_input(x + 5, y + 4, w - 8, buf, max, secret);
    tui_pop();
    return r;
}

void tui_progress(const char *title, const char *step, u32 num, u32 den)
{
    static u8 open;
    u8 x = 14, y = 9, w = 52;
    if (!title) { if (open) tui_pop(); open = 0; return; }
    if (!open) { tui_push(); open = 1; }
    else { tui_pop(); tui_push(); }
    tui_card(x, y, w, 7, title, C_ICE, C_PANEL);
    tui_shadow(x, y, w, 7);
    tui_putsn(x + 3, y + 2, step, A(C_TEXT, C_PANEL), w - 6);
    tui_bar(x + 3, y + 4, w - 12, num, den, C_ICE, C_PANEL);
    {
        char pc[8];
        u32 v = den ? num * 100 / den : 0;
        pl_utoa(pc, v);
        pl_strcat(pc, "%");
        tui_putr(x + w - 4, y + 4, pc, A(C_GOLD, C_PANEL));
    }
    tui_flush();
}
