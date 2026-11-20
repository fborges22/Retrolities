/* Polaris - text user interface (80x25, off-screen buffer, custom glyphs). */
#ifndef TUI_H
#define TUI_H

#include "pl.h"
#include "glyphs.h"

#define SW 80
#define SH 25

/* colour roles: attribute indices whose DAC values each theme defines */
enum {
    C_DEEP = 0, C_BG, C_GREEN, C_PANEL, C_RED, C_VIOLET, C_AMBER, C_DIM,
    C_BORDER, C_SELECT, C_LGREEN, C_ICE, C_PINK, C_GLOW, C_GOLD, C_TEXT
};
#define A(fg, bg) ((u8)(((bg) << 4) | (fg)))

enum { TH_NIGHT = 0, TH_AURORA, TH_EMBER, TH_GLACIER, TH_CLASSIC, TH_COUNT };
extern const char *const theme_name[TH_COUNT];

extern u16 scr[SH * SW];
extern u8  tui_vga;               /* VGA present: custom font and palette */
extern u8  tui_fancy;             /* custom glyphs active */
extern u8  tui_mono;
extern void (*tui_idle)(void);    /* called while waiting for keys */

void tui_init(u8 theme, u8 nofont);
void tui_done(void);              /* back to a plain mode-3 screen */
void tui_theme(u8 theme);
void tui_flush(void);
void tui_cursor(u8 x, u8 y, u8 show);

void tui_put(u8 x, u8 y, u8 ch, u8 attr);
u8   tui_puts(u8 x, u8 y, const char *s, u8 attr);
void tui_putsn(u8 x, u8 y, const char *s, u8 attr, u8 width);
void tui_putr(u8 xr, u8 y, const char *s, u8 attr);   /* right-aligned at xr */
void tui_center(u8 x, u8 w, u8 y, const char *s, u8 attr);
void tui_fill(u8 x, u8 y, u8 w, u8 h, u8 ch, u8 attr);
void tui_recolor(u8 x, u8 y, u8 w, u8 h, u8 attr);
void tui_shadow(u8 x, u8 y, u8 w, u8 h);
void tui_card(u8 x, u8 y, u8 w, u8 h, const char *title, u8 border, u8 bg);
void tui_hline(u8 x, u8 y, u8 w, u8 attr);
void tui_bar(u8 x, u8 y, u8 w, u32 num, u32 den, u8 fg, u8 bg);
void tui_icon(u8 x, u8 y, u8 icon, u8 attr);
void tui_logo(u8 x, u8 y, u8 fg, u8 star, u8 bg);
u8   tui_key(u8 x, u8 y, const char *key, const char *label, u8 bg);
void tui_toggle(u8 x, u8 y, u8 on, u8 bg);
void tui_check(u8 x, u8 y, u8 on, u8 attr);
void tui_scrollbar(u8 x, u8 y, u8 h, u16 top, u16 visible, u16 total, u8 attr);
void tui_twinkle(u8 phase);       /* animate the C_GLOW colour */

void tui_push(void);
void tui_pop(void);

u16  tui_getkey(void);            /* waits, runs tui_idle */
int  tui_input(u8 x, u8 y, u8 w, char *buf, u8 max, u8 secret);
int  tui_msg(const char *title, const char *text, const char *buttons, u8 tone);
int  tui_choose(const char *title, const char *const *items, u8 n, u8 sel);
int  tui_ask(const char *title, const char *prompt, char *buf, u8 max, u8 secret);
void tui_progress(const char *title, const char *step, u32 num, u32 den);

/* message tones */
enum { MT_INFO = 0, MT_WARN, MT_ERROR, MT_OK };

#endif
