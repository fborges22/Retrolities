/*
 * Arrow for DOS - text mode user interface
 *
 * 80x25 colour (or monochrome) text mode, direct video memory access.
 * Networking: mTCP + packet driver (see net_dos.cpp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <bios.h>
#include <i86.h>
#include "ip3core.h"
#include "ip3net.h"

extern volatile unsigned char net_break;

/* ---- colours ---------------------------------------------------- */
#define A_BAR      0x70
#define A_BARKEY   0x74
#define A_FRAME    0x1B
#define A_FTITLE   0x1E
#define A_LABEL    0x17
#define A_VALUE    0x1F
#define A_DIM      0x13
#define A_FOCUS    0x30
#define A_LED      0x0A
#define A_LEDDIM   0x02
#define A_PLABEL   0x07
#define A_PVALUE   0x0F
#define A_GRID     0x08

#define HIST_MAX   78
#define LOG_LINES  4
#define LOG_WIDTH  78

/* ---- screen primitives ----------------------------------------- */
static unsigned char far *vram;
static int mono;

static unsigned char mattr(unsigned char a)
{
    unsigned char bg = (unsigned char)(a >> 4) & 7;
    if (bg != 0 && bg != 1) return 0x70;
    return (a & 0x08) ? 0x0F : 0x07;
}

static void pc(int x, int y, unsigned char ch, unsigned char at)
{
    unsigned char far *p = vram + ((y * 80 + x) << 1);
    p[0] = ch;
    p[1] = mono ? mattr(at) : at;
}

static void fill(int x, int y, int w, int h, unsigned char ch, unsigned char at)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) pc(x + i, y + j, ch, at);
}

static int ps(int x, int y, const char *s, unsigned char at)
{
    while (*s && x < 80) pc(x++, y, (unsigned char)*s++, at);
    return x;
}

/* print padded/truncated to exactly w cells */
static void psw(int x, int y, const char *s, unsigned char at, int w)
{
    while (w-- > 0) pc(x++, y, (unsigned char)(*s ? *s++ : ' '), at);
}

static void box(int x, int y, int w, int h, const char *title)
{
    int i;
    pc(x, y, 201, A_FRAME); pc(x + w - 1, y, 187, A_FRAME);
    pc(x, y + h - 1, 200, A_FRAME); pc(x + w - 1, y + h - 1, 188, A_FRAME);
    for (i = 1; i < w - 1; i++) { pc(x + i, y, 205, A_FRAME); pc(x + i, y + h - 1, 205, A_FRAME); }
    for (i = 1; i < h - 1; i++) { pc(x, y + i, 186, A_FRAME); pc(x + w - 1, y + i, 186, A_FRAME); }
    if (title) {
        i = ps(x + 2, y, " ", A_FTITLE);
        i = ps(i, y, title, A_FTITLE);
        ps(i, y, " ", A_FTITLE);
    }
}

static void cursor(int on)
{
    union REGS r;
    r.h.ah = 1;
    r.w.cx = on ? (mono ? 0x0B0C : 0x0607) : 0x2000;
    int86(0x10, &r, &r);
}

static void gotoxy(int x, int y)
{
    union REGS r;
    r.h.ah = 2; r.h.bh = 0; r.h.dh = (unsigned char)y; r.h.dl = (unsigned char)x;
    int86(0x10, &r, &r);
}

static void video_init(void)
{
    union REGS r;
    r.h.ah = 0x0F;
    int86(0x10, &r, &r);
    if (r.h.al == 7) {
        mono = 1;
        vram = (unsigned char far *)MK_FP(0xB000, 0);
    } else {
        if (r.h.al != 3) { r.w.ax = 0x0003; int86(0x10, &r, &r); }
        vram = (unsigned char far *)MK_FP(0xB800, 0);
    }
    cursor(0);
}

static void video_done(void)
{
    fill(0, 0, 80, 25, ' ', 0x07);
    gotoxy(0, 0);
    cursor(1);
}

/* ---- model ------------------------------------------------------ */
enum { F_MODE, F_HOST, F_PORT, F_TIME, F_STREAMS, F_BLOCK, F_DIR, F_COUNT };
enum { T_TOGGLE, T_TEXT, T_NUM };

typedef struct {
    const char *label;
    int         type;
    char        text[64];
    int         maxlen;
    int         toggle;
    const char *opt[2];
} field_t;

static field_t fld[F_COUNT] = {
    { "Mode",      T_TOGGLE, "",      0, 0, { "Client", "Server" } },
    { "Host",      T_TEXT,   "",     63, 0, { 0, 0 } },
    { "Port",      T_NUM,    "5201",  5, 0, { 0, 0 } },
    { "Time (s)",  T_NUM,    "10",    4, 0, { 0, 0 } },
    { "Streams",   T_NUM,    "1",     1, 0, { 0, 0 } },
    { "Block",     T_NUM,    "8192",  5, 0, { 0, 0 } },
    { "Direction", T_TOGGLE, "",      0, 0, { "Send", "Receive" } }
};

static int    focus = F_HOST;
static int    batch;
static int    st_code = IP3_ST_IDLE;
static char   st_text[80] = "Ready";
static double cur_bps, peak_bps, tot_bytes, elapsed;
static double hist[HIST_MAX];
static int    nhist;
static int    run_secs;
static char   logbuf[LOG_LINES][LOG_WIDTH + 1];
static int    d_top = 1, d_setup = 1, d_meter = 1, d_hist = 1, d_log = 1;

static double nice_scale(double v)
{
    double s = 10000.0;
    static const double m[3] = { 2.0, 2.5, 2.0 };
    int i = 0;
    while (s < v) { s *= m[i]; i = (i + 1) % 3; }
    return s;
}

/* ---- drawing ---------------------------------------------------- */
static void draw_static(void)
{
    static const char *keys[] = { "F2", " Start  ", "F3", " Stop  ", "\x18\x19", " Field  ",
                                  "\x1b\x1a", " Change  ", "F10", " Quit", 0 };
    int i, x;
    fill(0, 0, 80, 25, ' ', A_LABEL);
    box(0, 1, 28, 11, "Setup");
    box(28, 1, 52, 11, "Throughput");
    fill(29, 2, 50, 9, ' ', A_PLABEL);
    box(0, 12, 80, 6, "History");
    fill(1, 13, 78, 4, ' ', A_PLABEL);
    box(0, 18, 80, 6, "Report");
    fill(0, 24, 80, 1, ' ', A_BAR);
    for (i = 0, x = 1; keys[i]; i++) x = ps(x, 24, keys[i], (unsigned char)((i & 1) ? A_BAR : A_BARKEY));
    ps(80 - 21, 24, "Arrow \xb3 TCP \xb3 mTCP", A_BAR);
}

static void draw_top(void)
{
    static const char *names[] = { "IDLE", "RESOLVING", "CONNECTING", "LISTENING", "NEGOTIATING",
                                   "RUNNING", "FINISHING", "DONE", "ERROR" };
    static const unsigned char cols[] = { 0x70, 0x60, 0x60, 0x3F, 0x60, 0x2F, 0x60, 0x2F, 0x4F };
    char b[40];
    int x;
    fill(0, 0, 80, 1, ' ', A_BAR);
    x = ps(1, 0, "\xf0 ", A_BARKEY);
    x = ps(x, 0, "Arrow", A_BARKEY);
    ps(x, 0, " for DOS  v" IP3_VERSION, A_BAR);
    sprintf(b, " %s ", names[st_code]);
    x = 79 - (int)strlen(b);
    ps(x, 0, b, cols[st_code]);
    sprintf(b, "IP %s ", net_local_ip());
    ps(x - (int)strlen(b), 0, b, A_BAR);
    d_top = 0;
}

static void draw_setup(void)
{
    int i, running = ip3_active();
    int server = fld[F_MODE].toggle;
    char v[20];
    for (i = 0; i < F_COUNT; i++) {
        field_t *f = &fld[i];
        int y = 2 + i, na;
        unsigned char at;
        na = server && (i == F_HOST || i == F_TIME || i == F_STREAMS || i == F_BLOCK || i == F_DIR);
        psw(2, y, f->label, (unsigned char)(na ? A_DIM : A_LABEL), 10);
        if (f->type == T_TOGGLE) {
            sprintf(v, "\x11 %-8s \x10", f->opt[f->toggle]);
        } else {
            int n = (int)strlen(f->text);
            strcpy(v, n > 13 ? f->text + n - 13 : f->text);
        }
        at = (unsigned char)(na ? A_DIM : A_VALUE);
        if (i == focus && !running) at = A_FOCUS;
        psw(12, y, na ? "\xc4" : v, at, 14);
    }
    fill(1, 9, 26, 2, ' ', A_LABEL);
    if (running && run_secs > 0) {              /* time progress */
        int n = (int)(elapsed / run_secs * 24.0 + 0.5);
        for (i = 0; i < 24; i++) pc(2 + i, 9, (unsigned char)(i < n ? 254 : 250), (unsigned char)(i < n ? 0x1B : A_DIM));
    }
    if (running) psw(2, 10, "  \xfe STOP  (F3 / Esc)   ", 0x4F, 24);
    else         psw(2, 10, "  \x10 START (F2 / Enter) ", 0x2F, 24);
    if (!running && fld[focus].type != T_TOGGLE) {
        int n = (int)strlen(fld[focus].text);
        gotoxy(12 + (n > 13 ? 13 : n), 2 + focus);
        cursor(1);
    } else cursor(0);
    d_setup = 0;
}

/* seven segment style digits, 4x5 "pixels" drawn with half blocks */
static void big_digit(int x, int y, int d, unsigned char at)
{
    static const unsigned char seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    unsigned char s = seg[d], px[6][4];
    int r, c;
#define S(b) ((s >> (b)) & 1)
    for (c = 0; c < 4; c++) {
        int l = (c == 0), rt = (c == 3);
        px[0][c] = (unsigned char)(S(0) || (l && S(5)) || (rt && S(1)));
        px[1][c] = (unsigned char)((l && S(5)) || (rt && S(1)));
        px[2][c] = (unsigned char)(S(6) || (l && (S(5) || S(4))) || (rt && (S(1) || S(2))));
        px[3][c] = (unsigned char)((l && S(4)) || (rt && S(2)));
        px[4][c] = (unsigned char)(S(3) || (l && S(4)) || (rt && S(2)));
        px[5][c] = 0;
    }
#undef S
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++) {
            int t = px[r * 2][c], b = px[r * 2 + 1][c];
            pc(x + c, y + r, (unsigned char)(t && b ? 219 : t ? 223 : b ? 220 : ' '), at);
        }
}

static void fmt_time(double s, char *o)
{
    long t = (long)(s + 0.5);
    sprintf(o, "%02ld:%02ld", t / 60, t % 60);
}

static void draw_meter(void)
{
    char r[24], b[60], t1[12], t2[12], *unit;
    double scale = nice_scale(peak_bps > cur_bps ? peak_bps : cur_bps), avg;
    int x, i, n;

    /* big readout */
    fill(29, 2, 50, 9, ' ', A_PLABEL);
    ps(31, 2, "CURRENT BITRATE", A_GRID);
    ip3_fmt_rate(cur_bps, r);
    unit = strchr(r, ' ');
    *unit++ = 0;
    for (x = 31, i = 0; r[i]; i++) {
        if (r[i] == '.') { pc(x, 5, 220, A_LED); x += 2; }
        else { big_digit(x, 3, r[i] - '0', A_LED); x += 5; }
    }
    ps(x + 1, 5, unit, A_LED);

    /* gauge */
    n = (int)(cur_bps / scale * 48.0 + 0.5);
    if (n > 48) n = 48;
    for (i = 0; i < 48; i++) {
        unsigned char at = (unsigned char)(i < 34 ? 0x0A : i < 43 ? 0x0E : 0x0C);
        if (i < n) pc(30 + i, 7, 219, at); else pc(30 + i, 7, 176, A_GRID);
    }
    ip3_fmt_rate(scale, r);
    ps(30, 8, "0", A_GRID);
    ps(78 - (int)strlen(r), 8, r, A_GRID);

    /* statistics */
    avg = elapsed > 0 ? tot_bytes * 8.0 / elapsed : 0.0;
    ip3_fmt_rate(avg, r);
    x = ps(30, 9, "Average ", A_PLABEL); psw(x, 9, r, A_PVALUE, 16);
    ip3_fmt_rate(peak_bps, r);
    x = ps(55, 9, "Peak ", A_PLABEL); psw(x, 9, r, A_PVALUE, 16);
    ip3_fmt_bytes(tot_bytes, r);
    x = ps(30, 10, "Data    ", A_PLABEL); psw(x, 10, r, A_PVALUE, 14);
    fmt_time(elapsed, t1);
    if (run_secs > 0) { fmt_time((double)run_secs, t2); sprintf(b, "%s / %s", t1, t2); }
    else strcpy(b, t1);
    x = ps(55, 10, "Time ", A_PLABEL); ps(x, 10, b, A_PVALUE);
    d_meter = 0;
}

static void draw_hist(void)
{
    double mx = 0, scale;
    char r[24], t[60];
    int i, row, x;
    for (i = 0; i < nhist; i++) if (hist[i] > mx) mx = hist[i];
    scale = nice_scale(mx);
    for (i = 30; i < 78; i++) pc(i, 12, 205, A_FRAME);
    ip3_fmt_rate(scale, r);
    sprintf(t, " full scale %s ", r);
    ps(78 - (int)strlen(t), 12, t, A_FTITLE);
    for (x = 0; x < HIST_MAX; x++) {
        int idx = nhist - HIST_MAX + x, lvl = 0;
        if (idx >= 0) {
            lvl = (int)(hist[idx] / scale * 8.0 + 0.5);
            if (lvl == 0 && hist[idx] > 0) lvl = 1;
            if (lvl > 8) lvl = 8;
        }
        for (row = 0; row < 4; row++) {
            int k = 3 - row;
            unsigned char at = (unsigned char)(idx == nhist - 1 ? 0x0E : (x & 1) ? 0x0A : 0x02);
            if (lvl >= 2 * k + 2)      pc(1 + x, 13 + row, 219, at);
            else if (lvl == 2 * k + 1) pc(1 + x, 13 + row, 220, at);
            else                       pc(1 + x, 13 + row, 250, A_GRID);
        }
    }
    d_hist = 0;
}

static void draw_log(void)
{
    int i;
    for (i = 10; i < 78; i++) pc(i, 18, 205, A_FRAME);
    ps(10, 18, " \xfe ", (unsigned char)(st_code == IP3_ST_ERROR ? 0x1C : st_code == IP3_ST_RUNNING ? 0x1A : 0x1B));
    psw(13, 18, st_text, A_VALUE, (int)strlen(st_text) > 62 ? 62 : (int)strlen(st_text));
    ps(13 + ((int)strlen(st_text) > 62 ? 62 : (int)strlen(st_text)), 18, " ", A_VALUE);
    for (i = 0; i < LOG_LINES; i++) {
        const char *l = logbuf[i];
        unsigned char at = A_LABEL;
        if (!strncmp(l, "error", 5)) at = 0x1C;
        else if (strstr(l, "sender") || strstr(l, "receiver")) at = 0x1E;
        else if (l[0] != '[') at = 0x1B;
        psw(1, 19 + i, l, at, LOG_WIDTH);
    }
    d_log = 0;
}

static void redraw(void)
{
    if (d_top)   draw_top();
    if (d_meter) draw_meter();
    if (d_hist)  draw_hist();
    if (d_log)   draw_log();
    if (d_setup) draw_setup();
}

/* ---- engine callbacks ------------------------------------------ */
void ui_log(const char *line)
{
    int i;
    if (batch) { puts(line); return; }
    for (i = 0; i < LOG_LINES - 1; i++) strcpy(logbuf[i], logbuf[i + 1]);
    strncpy(logbuf[LOG_LINES - 1], line, LOG_WIDTH);
    logbuf[LOG_LINES - 1][LOG_WIDTH] = 0;
    d_log = 1;
}

void ui_status(int state, const char *text)
{
    if (state == IP3_ST_RUNNING) {
        cur_bps = peak_bps = tot_bytes = elapsed = 0;
        nhist = 0;
        run_secs = ip3_duration();
        d_hist = 1;
    }
    st_code = state;
    strncpy(st_text, text, sizeof(st_text) - 1);
    d_top = d_log = d_setup = d_meter = 1;
    if (!batch) redraw();
}

void ui_interval(double t0, double t1, double bytes, double bps)
{
    (void)t0;
    cur_bps = bps;
    if (bps > peak_bps) peak_bps = bps;
    tot_bytes += bytes;
    elapsed = t1;
    if (nhist == HIST_MAX) { memmove(hist, hist + 1, sizeof(double) * (HIST_MAX - 1)); nhist--; }
    hist[nhist++] = bps;
    d_meter = d_hist = d_setup = 1;
}

void ui_summary(const ip3_summary *s)
{
    cur_bps = s->recv_bps;
    tot_bytes = s->we_send ? s->sent_bytes : s->recv_bytes;
    elapsed = s->secs;
    d_meter = 1;
}

/* ---- control ---------------------------------------------------- */
static void build_cfg(ip3_cfg *c)
{
    ip3_defaults(c);
    strcpy(c->host, fld[F_HOST].text);
    c->server = fld[F_MODE].toggle;
    c->reverse = fld[F_DIR].toggle;
    c->port = (unsigned)atol(fld[F_PORT].text);
    c->duration = atoi(fld[F_TIME].text);
    c->parallel = atoi(fld[F_STREAMS].text);
    c->blksize = (unsigned long)atol(fld[F_BLOCK].text);
    if (c->parallel < 1) c->parallel = 1;
    if (c->parallel > IP3_MAX_STREAMS) c->parallel = IP3_MAX_STREAMS;
    if (c->blksize < 64) c->blksize = 64;
    if (c->blksize > IP3_BLK_MAX) c->blksize = IP3_BLK_MAX;
    if (c->duration < 1) c->duration = 1;
    sprintf(fld[F_STREAMS].text, "%d", c->parallel);
    sprintf(fld[F_BLOCK].text, "%lu", c->blksize);
    sprintf(fld[F_TIME].text, "%d", c->duration);
}

static void start_test(void)
{
    ip3_cfg c;
    build_cfg(&c);
    if (!c.server && c.host[0] == 0) {
        ui_status(IP3_ST_ERROR, "Enter the test server host name or address first");
        focus = F_HOST;
        return;
    }
    ip3_start(&c);
    d_setup = 1;
}

static int handle_key(unsigned k)
{
    unsigned char a = (unsigned char)k, sc = (unsigned char)(k >> 8);
    field_t *f = &fld[focus];
    int n;

    if (a == 0 || a == 0xE0) {
        if (sc == 0x44 || sc == 0x2D) return 0;                 /* F10, Alt-X */
        if (sc == 0x3D) { ip3_stop(); d_setup = 1; return 1; }  /* F3 */
        if (ip3_active()) return 1;
        switch (sc) {
        case 0x3C: start_test(); break;                         /* F2 */
        case 0x48: focus = (focus + F_COUNT - 1) % F_COUNT; break;
        case 0x50: focus = (focus + 1) % F_COUNT; break;
        case 0x4B: case 0x4D:
            if (f->type == T_TOGGLE) f->toggle ^= 1;
            break;
        case 0x0F: focus = (focus + F_COUNT - 1) % F_COUNT; break;  /* Shift-Tab */
        }
        d_setup = 1;
        return 1;
    }
    if (a == 27) {
        if (ip3_active()) { ip3_stop(); d_setup = 1; return 1; }
        return 0;
    }
    if (ip3_active()) return 1;
    if (a == 13) { start_test(); d_setup = 1; return 1; }
    if (a == 9)  { focus = (focus + 1) % F_COUNT; d_setup = 1; return 1; }
    if (f->type == T_TOGGLE) {
        if (a == ' ') f->toggle ^= 1;
    } else {
        n = (int)strlen(f->text);
        if (a == 8) { if (n) f->text[n - 1] = 0; }
        else if (n < f->maxlen && ((f->type == T_NUM && isdigit(a)) ||
                 (f->type == T_TEXT && a > 32 && a < 127))) {
            f->text[n] = (char)a; f->text[n + 1] = 0;
        }
    }
    d_setup = 1;
    return 1;
}

static void usage(void)
{
    puts("Arrow for DOS v" IP3_VERSION " - network throughput test\n"
         "\n"
         "Usage: ARROWD [options]            (no options: interactive screen)\n"
         "  -c <host>   run as client and connect to <host>\n"
         "  -s          run as server\n"
         "  -p <port>   port (default 5201)\n"
         "  -t <secs>   test duration (default 10)\n"
         "  -P <n>      parallel streams, 1-4\n"
         "  -l <bytes>  block length (default 8192)\n"
         "  -R          reverse: the server sends, this machine receives\n"
         "  -b          batch mode: plain text output, no full screen UI\n"
         "\n"
         "Requires a packet driver and an mTCP configuration (SET MTCPCFG=...).");
}

int main(int argc, char **argv)
{
    char err[100];
    int i, autostart = 0, rc = 0, steps;
    unsigned long last = 0, now;

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if (a[0] != '-' && a[0] != '/') { usage(); return 1; }
        switch (a[1]) {
        case 'c': strncpy(fld[F_HOST].text, v, 63); fld[F_MODE].toggle = 0; autostart = 1; i++; break;
        case 's': fld[F_MODE].toggle = 1; autostart = 1; break;
        case 'p': strncpy(fld[F_PORT].text, v, 5); i++; break;
        case 't': strncpy(fld[F_TIME].text, v, 4); i++; break;
        case 'P': strncpy(fld[F_STREAMS].text, v, 1); i++; break;
        case 'l': strncpy(fld[F_BLOCK].text, v, 5); i++; break;
        case 'R': fld[F_DIR].toggle = 1; break;
        case 'b': batch = 1; break;
        default:  usage(); return 1;
        }
    }
    if (batch && !autostart) { usage(); return 1; }

    if (net_init(err, sizeof(err)) != 0) {
        printf("\nARROWD: %s\n", err);
        return 2;
    }

    if (batch) {
        ip3_cfg c;
        setbuf(stdout, NULL);
        build_cfg(&c);
        if (ip3_start(&c) == 0) {
            while (ip3_active()) {
                ip3_step();
                if (net_break || (_bios_keybrd(_KEYBRD_READY) && (_bios_keybrd(_KEYBRD_READ) & 0xFF) == 27))
                    ip3_stop();
            }
        }
        rc = (st_code == IP3_ST_DONE || c.server) ? 0 : 1;
        net_shutdown();
        return rc;
    }

    video_init();
    draw_static();
    ui_log("Welcome to Arrow. Fill in the setup panel and press F2 to start.");
    redraw();
    if (autostart) start_test();

    for (;;) {
        if (ip3_active()) {
            for (steps = 0; steps < 16 && ip3_step() == 1; steps++) ;
        } else {
            net_poll();
        }
        if (net_break) break;
        if (_bios_keybrd(_KEYBRD_READY)) {
            if (!handle_key(_bios_keybrd(_KEYBRD_READ))) break;
        }
        now = net_ms();
        if (now != last) { last = now; redraw(); }
    }

    if (ip3_active()) ip3_stop();
    net_shutdown();
    video_done();
    return 0;
}
