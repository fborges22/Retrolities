/*
 * Arrow - Windows GUI front end
 *
 * One source for Windows 3.x (Win16), Windows 9x (Win32) and Windows
 * NT 4.0/2000 (Win32) and x64 Windows (both built with IP3_WINNT).
 * The first two use only
 * Windows 3.0-era APIs and Winsock 1.1; the 3-D look is drawn by hand so
 * that it looks the same on Windows 3.1 and Windows 95.  The NT flavour
 * adds Winsock 2, per-user registry settings and the Windows 2000 UI font.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "resource.h"
#include "ip3core.h"
#include "ip3net.h"

#if defined(__NT__) || defined(_WIN32)
#define EXPORT
#define CMD_ID(w, l)   LOWORD(w)
#define APP_TITLE      "Arrow"
#ifdef IP3_WINNT
#ifdef _WIN64
#define APP_FLAVOUR    "for x64 Windows"
#else
#define APP_FLAVOUR    "for Windows NT/2000"
#endif
#define NET_NAME       "Winsock 2.2"
#else
#define APP_FLAVOUR    "for Windows 9x"
#endif
#else
#define EXPORT         __export
#define CMD_ID(w, l)   (w)
#define APP_TITLE      "Arrow"
#define APP_FLAVOUR    "for Windows 3.x"
#endif
#ifndef NET_NAME
#define NET_NAME       "Winsock 1.1"
#endif

#define CW        624
#define CH        440
#define HEAD_H    48
#define HIST_MAX  60
#define PI        3.14159265358979

#define C_FACE    RGB(192, 192, 192)
#define C_HI      RGB(255, 255, 255)
#define C_SHADOW  RGB(128, 128, 128)
#define C_DARK    RGB(0, 0, 0)
#define C_PANEL   RGB(8, 16, 28)
#define C_GREEN   RGB(64, 255, 128)
#define C_AMBER   RGB(255, 208, 0)
#define C_RED     RGB(255, 64, 48)

static const RECT rcSetup = { 8, 56, 206, 304 };
static const RECT rcGauge = { 214, 56, 414, 216 };
static const RECT rcStats = { 422, 56, 616, 216 };
static const RECT rcChart = { 214, 224, 616, 304 };
static const RECT rcLog   = { 8, 312, 616, 408 };
static const RECT rcBar   = { 0, 414, CW, CH };
static const RECT rcHead  = { 0, 0, CW, HEAD_H };

static HINSTANCE hInst;
static HWND      hMain, hLog;
static HBITMAP   hbmBack;
static HBRUSH    hbrFace;
static HFONT     hfUI, hfBold, hfTitle, hfBig, hfMono, hfSmall;
#ifdef IP3_WINNT
static char      szKey[] = "Software\\Arrow\\Settings";
#else
static char      szIni[] = "ARROW.INI";
static char      szSec[] = "Settings";
#endif

static int    st_code = IP3_ST_IDLE;
static char   st_text[100] = "Ready";
static double cur_bps, disp_bps, peak_bps, tot_bytes, elapsed;
static double hist[HIST_MAX];
static int    nhist, run_secs, busy;

/* ---- small helpers --------------------------------------------- */
static double nice_scale(double v)
{
    static const double m[3] = { 2.0, 2.5, 2.0 };
    double s = 10000.0;
    int i = 0;
    while (s < v) { s *= m[i]; i = (i + 1) % 3; }
    return s;
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x0; r.top = y0; r.right = x1; r.bottom = y1;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void edge(HDC dc, int x0, int y0, int x1, int y1, COLORREF tl, COLORREF br)
{
    fill(dc, x0, y0, x1 - 1, y0 + 1, tl);
    fill(dc, x0, y0, x0 + 1, y1 - 1, tl);
    fill(dc, x0, y1 - 1, x1, y1, br);
    fill(dc, x1 - 1, y0, x1, y1, br);
}

/* Windows 95 style sunken client edge around r (drawn outside of r) */
static void sunken(HDC dc, const RECT *r)
{
    edge(dc, r->left - 2, r->top - 2, r->right + 2, r->bottom + 2, C_SHADOW, C_HI);
    edge(dc, r->left - 1, r->top - 1, r->right + 1, r->bottom + 1, C_DARK, C_FACE);
}

static void text(HDC dc, int x, int y, const char *s, HFONT f, COLORREF c, UINT align)
{
    HFONT old = SelectObject(dc, f);
    SetTextAlign(dc, align);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    TextOut(dc, x, y, s, lstrlen(s));
    SelectObject(dc, old);
}

static HFONT mkfont(int h, int bold, const char *face, int fixed)
{
    return CreateFont(-h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                      (BYTE)((fixed ? FIXED_PITCH : VARIABLE_PITCH) | (fixed ? FF_MODERN : FF_SWISS)), face);
}

static void fmt_time(double s, char *o)
{
    long t = (long)(s + 0.5);
    sprintf(o, "%02ld:%02ld", t / 60, t % 60);
}

/* ---- painting -------------------------------------------------- */
static void paint_header(HDC dc)
{
    static const char *names[] = { "IDLE", "RESOLVING", "CONNECTING", "LISTENING", "NEGOTIATING",
                                   "RUNNING", "FINISHING", "DONE", "ERROR" };
    COLORREF led;
    HBRUSH b, ob;
    HPEN op;
    int i, x;

    for (i = 0; i < 39; i++)            /* navy to steel-blue sweep */
        fill(dc, i * 16, 0, i * 16 + 16, HEAD_H - 2,
             RGB(0, (BYTE)(8 + i * 2), (BYTE)(88 + i * 3)));
    fill(dc, 0, HEAD_H - 2, CW, HEAD_H - 1, RGB(0, 0, 48));
    fill(dc, 0, HEAD_H - 1, CW, HEAD_H, C_HI);

    for (i = 0; i < 5; i++)             /* signal-bars logo */
        fill(dc, 12 + i * 7, 34 - (i + 1) * 5, 17 + i * 7, 36,
             i < 3 ? C_GREEN : i < 4 ? C_AMBER : C_RED);

    text(dc, 55, 7, APP_TITLE, hfTitle, RGB(0, 0, 40), TA_LEFT | TA_TOP);
    text(dc, 54, 6, APP_TITLE, hfTitle, C_HI, TA_LEFT | TA_TOP);
    text(dc, 56, 30, "Network throughput test  \267  iperf3 protocol  \267  " APP_FLAVOUR,
         hfSmall, RGB(176, 216, 255), TA_LEFT | TA_TOP);

    switch (st_code) {
    case IP3_ST_RUNNING: case IP3_ST_DONE:  led = RGB(0, 255, 96);  break;
    case IP3_ST_ERROR:                      led = RGB(255, 48, 32); break;
    case IP3_ST_IDLE:                       led = RGB(144, 144, 144); break;
    case IP3_ST_LISTENING:                  led = RGB(64, 208, 255); break;
    default:                                led = RGB(255, 208, 0); break;
    }
    x = CW - 20;
    b = CreateSolidBrush(led);
    ob = SelectObject(dc, b);
    op = SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, x - 6, 17, x + 7, 30);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    fill(dc, x - 2, 20, x + 1, 22, C_HI);
    text(dc, x - 14, 17, names[st_code], hfBold, C_HI, TA_RIGHT | TA_TOP);
}

static void paint_setup(HDC dc)
{
    static const char *lab[] = { "Mode", "Server host", "Port", "Duration (s)", "Streams", "Block (bytes)" };
    int i;
    edge(dc, rcSetup.left, rcSetup.top + 6, rcSetup.right, rcSetup.bottom, C_SHADOW, C_HI);
    edge(dc, rcSetup.left + 1, rcSetup.top + 7, rcSetup.right - 1, rcSetup.bottom - 1, C_HI, C_SHADOW);
    fill(dc, rcSetup.left + 8, rcSetup.top, rcSetup.left + 72, rcSetup.top + 14, C_FACE);
    text(dc, rcSetup.left + 12, rcSetup.top, "Test setup", hfBold, RGB(0, 0, 128), TA_LEFT | TA_TOP);
    for (i = 0; i < 6; i++)
        text(dc, 18, 80 + i * 26, lab[i], hfUI, C_DARK, TA_LEFT | TA_TOP);
#if !defined(__NT__) && !defined(_WIN32)
    for (i = IDC_HOST; i <= IDC_BLOCK; i++) {       /* hand-drawn edit borders */
        RECT r;
        GetWindowRect(GetDlgItem(hMain, i), &r);
        ScreenToClient(hMain, (LPPOINT)&r);
        ScreenToClient(hMain, ((LPPOINT)&r) + 1);
        sunken(dc, &r);
    }
#endif
}

static void dial_pt(double f, double r, int cx, int cy, POINT *p)
{
    double a = (225.0 - 270.0 * f) * PI / 180.0;
    p->x = cx + (int)floor(r * cos(a) + 0.5);
    p->y = cy - (int)floor(r * sin(a) + 0.5);
}

static void paint_gauge(HDC dc)
{
    int cx = (rcGauge.left + rcGauge.right) / 2, cy = rcGauge.top + 82, i;
    double scale = nice_scale(peak_bps > cur_bps ? peak_bps : cur_bps), f, div;
    char num[24], *unit, lab[16];
    POINT p, q, n[3];
    HPEN pen, old;
    HBRUSH b, ob;

    sunken(dc, &rcGauge);
    fill(dc, rcGauge.left, rcGauge.top, rcGauge.right, rcGauge.bottom, C_PANEL);

    /* coloured arc */
    for (i = 0; i < 54; i++) {
        pen = CreatePen(PS_SOLID, 4, i < 36 ? RGB(0, 176, 80) : i < 47 ? RGB(224, 176, 0) : RGB(224, 40, 32));
        old = SelectObject(dc, pen);
        dial_pt(i / 54.0, 70.0, cx, cy, &p);
        dial_pt((i + 1) / 54.0, 70.0, cx, cy, &q);
        MoveToEx(dc, p.x, p.y, NULL);
        LineTo(dc, q.x, q.y);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    /* ticks and labels */
    div = scale >= 1e9 ? 1e9 : scale >= 1e6 ? 1e6 : 1e3;
    pen = CreatePen(PS_SOLID, 1, RGB(200, 208, 216));
    old = SelectObject(dc, pen);
    for (i = 0; i <= 20; i++) {
        dial_pt(i / 20.0, 65.0, cx, cy, &p);
        dial_pt(i / 20.0, (i % 4) ? 61.0 : 56.0, cx, cy, &q);
        MoveToEx(dc, p.x, p.y, NULL);
        LineTo(dc, q.x, q.y);
        if (i % 4 == 0) {
            dial_pt(i / 20.0, 45.0, cx, cy, &q);
            sprintf(lab, "%g", scale * i / 20.0 / div);
            text(dc, q.x, q.y - 6, lab, hfSmall, RGB(160, 176, 192), TA_CENTER | TA_TOP);
        }
    }
    SelectObject(dc, old);
    DeleteObject(pen);

    /* needle */
    f = disp_bps / scale;
    if (f < 0) f = 0;
    if (f > 1.02) f = 1.02;
    dial_pt(f, 60.0, cx, cy, &n[0]);
    dial_pt(f + 0.5, 4.0, cx, cy, &p);       /* opposite side of the hub */
    {
        double a = (225.0 - 270.0 * f) * PI / 180.0;
        n[1].x = cx + (int)floor(4.0 * cos(a + PI / 2) + 0.5);
        n[1].y = cy - (int)floor(4.0 * sin(a + PI / 2) + 0.5);
        n[2].x = cx + (int)floor(4.0 * cos(a - PI / 2) + 0.5);
        n[2].y = cy - (int)floor(4.0 * sin(a - PI / 2) + 0.5);
    }
    b = CreateSolidBrush(C_RED);
    pen = CreatePen(PS_SOLID, 1, RGB(255, 160, 144));
    ob = SelectObject(dc, b);
    old = SelectObject(dc, pen);
    Polygon(dc, n, 3);
    SelectObject(dc, old);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(b);
    b = CreateSolidBrush(RGB(176, 184, 192));
    ob = SelectObject(dc, b);
    old = SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, cx - 7, cy - 7, cx + 8, cy + 8);
    SelectObject(dc, old);
    SelectObject(dc, ob);
    DeleteObject(b);

    /* digital readout */
    ip3_fmt_rate(cur_bps, num);
    unit = strchr(num, ' ');
    *unit++ = 0;
    text(dc, cx, cy + 25, num, hfBig, C_GREEN, TA_CENTER | TA_TOP);
    text(dc, cx, cy + 53, unit, hfSmall, RGB(0, 176, 96), TA_CENTER | TA_TOP);
    text(dc, rcGauge.left + 6, rcGauge.top + 4, "BITRATE", hfSmall, RGB(96, 112, 128), TA_LEFT | TA_TOP);
}

static void paint_stats(HDC dc)
{
    static const char *lab[] = { "Current", "Average", "Peak", "Transferred", "Elapsed" };
    char v[5][40], t2[12];
    int i, y, n, x;
    RECT pr;

    sunken(dc, &rcStats);
    fill(dc, rcStats.left, rcStats.top, rcStats.right, rcStats.bottom, C_HI);
    ip3_fmt_rate(cur_bps, v[0]);
    ip3_fmt_rate(elapsed > 0 ? tot_bytes * 8.0 / elapsed : 0.0, v[1]);
    ip3_fmt_rate(peak_bps, v[2]);
    ip3_fmt_bytes(tot_bytes, v[3]);
    fmt_time(elapsed, v[4]);
    if (run_secs > 0) { fmt_time((double)run_secs, t2); strcat(v[4], " / "); strcat(v[4], t2); }
    for (i = 0; i < 5; i++) {
        y = rcStats.top + 8 + i * 25;
        text(dc, rcStats.left + 10, y, lab[i], hfUI, RGB(64, 64, 64), TA_LEFT | TA_TOP);
        text(dc, rcStats.right - 10, y, v[i], hfBold, i == 0 ? RGB(0, 96, 0) : RGB(0, 0, 128), TA_RIGHT | TA_TOP);
        if (i < 4) fill(dc, rcStats.left + 8, y + 18, rcStats.right - 8, y + 19, RGB(224, 224, 224));
    }
    /* segmented progress bar, Windows 3.1 setup style */
    pr.left = rcStats.left + 10; pr.right = rcStats.right - 10;
    pr.top = rcStats.bottom - 24; pr.bottom = rcStats.bottom - 10;
    edge(dc, pr.left - 1, pr.top - 1, pr.right + 1, pr.bottom + 1, C_SHADOW, C_FACE);
    fill(dc, pr.left, pr.top, pr.right, pr.bottom, RGB(240, 240, 240));
    n = 0;
    if (run_secs > 0) n = (int)(elapsed / run_secs * 17.0 + 0.5);
    else if (st_code == IP3_ST_RUNNING) n = -1;
    if (st_code == IP3_ST_DONE) n = 17;
    if (n > 17) n = 17;
    for (i = 0; i < 17; i++) {
        x = pr.left + 2 + i * 10;
        if (n < 0 ? (i % 6) == ((int)elapsed % 6) : i < n)
            fill(dc, x, pr.top + 2, x + 8, pr.bottom - 2, RGB(0, 0, 160));
    }
}

static void paint_chart(HDC dc)
{
    static POINT pts[HIST_MAX + 2];
    int w = rcChart.right - rcChart.left - 8, h = rcChart.bottom - rcChart.top - 22;
    int x0 = rcChart.left + 4, y1 = rcChart.bottom - 5, i, n;
    double mx = 0, scale;
    char r[24], lab[48];
    HPEN pen, old;
    HBRUSH b, ob;

    sunken(dc, &rcChart);
    fill(dc, rcChart.left, rcChart.top, rcChart.right, rcChart.bottom, C_PANEL);
    for (i = 0; i < nhist; i++) if (hist[i] > mx) mx = hist[i];
    scale = nice_scale(mx);

    pen = CreatePen(PS_DOT, 1, RGB(0, 80, 48));
    old = SelectObject(dc, pen);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i <= 4; i++) {
        MoveToEx(dc, x0, y1 - h * i / 4, NULL);
        LineTo(dc, x0 + w, y1 - h * i / 4);
    }
    for (i = 1; i < 6; i++) {
        MoveToEx(dc, x0 + w * i / 6, y1 - h, NULL);
        LineTo(dc, x0 + w * i / 6, y1);
    }
    SelectObject(dc, old);
    DeleteObject(pen);

    if (nhist > 0) {
        n = 0;
        pts[n].x = x0; pts[n].y = y1; n++;
        for (i = 0; i < nhist; i++) {
            pts[n].x = x0 + (int)((long)w * i / (HIST_MAX - 1));
            pts[n].y = y1 - (int)(hist[i] / scale * h + 0.5);
            n++;
        }
        pts[n].x = pts[n - 1].x; pts[n].y = y1; n++;
        b = CreateSolidBrush(RGB(0, 96, 56));
        ob = SelectObject(dc, b);
        old = SelectObject(dc, GetStockObject(NULL_PEN));
        if (nhist > 1) Polygon(dc, pts, n);
        SelectObject(dc, old);
        SelectObject(dc, ob);
        DeleteObject(b);
        pen = CreatePen(PS_SOLID, 2, C_GREEN);
        old = SelectObject(dc, pen);
        if (nhist > 1) Polyline(dc, pts + 1, n - 2);
        SelectObject(dc, old);
        DeleteObject(pen);
        fill(dc, pts[n - 2].x - 2, pts[n - 2].y - 2, pts[n - 2].x + 3, pts[n - 2].y + 3, C_HI);
    }
    ip3_fmt_rate(scale, r);
    sprintf(lab, "HISTORY  \267  full scale %s", r);
    text(dc, rcChart.left + 6, rcChart.top + 3, lab, hfSmall, RGB(0, 176, 96), TA_LEFT | TA_TOP);
    text(dc, rcChart.right - 6, rcChart.top + 3, "1 sample / second", hfSmall, RGB(96, 112, 128), TA_RIGHT | TA_TOP);
}

static void paint_bar(HDC dc)
{
    RECT r;
    char b[64];
    fill(dc, 0, rcBar.top - 2, CW, rcBar.top - 1, C_SHADOW);
    fill(dc, 0, rcBar.top - 1, CW, rcBar.top, C_HI);
    r.left = 4; r.top = rcBar.top + 4; r.right = CW - 198; r.bottom = CH - 4;
    edge(dc, r.left, r.top, r.right, r.bottom, C_SHADOW, C_HI);
    text(dc, r.left + 6, r.top + 3, st_text, hfUI, st_code == IP3_ST_ERROR ? RGB(160, 0, 0) : C_DARK, TA_LEFT | TA_TOP);
    r.left = CW - 194; r.right = CW - 4;
    edge(dc, r.left, r.top, r.right, r.bottom, C_SHADOW, C_HI);
    sprintf(b, "TCP  \267  " NET_NAME "  \267  v" IP3_VERSION);
    text(dc, r.left + 6, r.top + 3, b, hfUI, RGB(64, 64, 64), TA_LEFT | TA_TOP);
}

static int hits(const RECT *a, const RECT *clip)
{
    RECT t, g = *a;
    InflateRect(&g, 3, 3);
    return IntersectRect(&t, &g, clip);
}

static void paint_all(HDC dc, const RECT *clip)
{
    HBRUSH ob;
    HRGN rgn = CreateRectRgnIndirect(clip);
    SelectClipRgn(dc, rgn);
    DeleteObject(rgn);
    ob = SelectObject(dc, hbrFace);
    FillRect(dc, clip, hbrFace);
    SelectObject(dc, ob);
    if (hits(&rcHead, clip))  paint_header(dc);
    if (hits(&rcSetup, clip)) paint_setup(dc);
    if (hits(&rcGauge, clip)) paint_gauge(dc);
    if (hits(&rcStats, clip)) paint_stats(dc);
    if (hits(&rcChart, clip)) paint_chart(dc);
    if (hits(&rcBar, clip))   paint_bar(dc);
#if !defined(__NT__) && !defined(_WIN32)
    if (hits(&rcLog, clip))   sunken(dc, &rcLog);
#endif
    SelectClipRgn(dc, NULL);
}

static void inval(const RECT *r)
{
    RECT g = *r;
    InflateRect(&g, 2, 2);
    InvalidateRect(hMain, &g, FALSE);
}

/* ---- engine callbacks ------------------------------------------ */
void ui_log(const char *line)
{
    int n;
    if (!hLog) return;
    n = (int)SendMessage(hLog, LB_ADDSTRING, 0, (LPARAM)(LPCSTR)line);
    if (n > 400) { SendMessage(hLog, LB_DELETESTRING, 0, 0); n--; }
    SendMessage(hLog, LB_SETTOPINDEX, (WPARAM)(n > 4 ? n - 4 : 0), 0);
}

static void sync_controls(void)
{
    int run = ip3_active() || busy;
    int server = IsDlgButtonChecked(hMain, IDC_SERVER);
    EnableWindow(GetDlgItem(hMain, IDC_CLIENT), !run);
    EnableWindow(GetDlgItem(hMain, IDC_SERVER), !run);
    EnableWindow(GetDlgItem(hMain, IDC_PORT), !run);
    EnableWindow(GetDlgItem(hMain, IDC_HOST), !run && !server);
    EnableWindow(GetDlgItem(hMain, IDC_TIME), !run && !server);
    EnableWindow(GetDlgItem(hMain, IDC_STREAMS), !run && !server);
    EnableWindow(GetDlgItem(hMain, IDC_BLOCK), !run && !server);
    EnableWindow(GetDlgItem(hMain, IDC_REVERSE), !run && !server);
    EnableWindow(GetDlgItem(hMain, IDC_START), !run);
    EnableWindow(GetDlgItem(hMain, IDC_STOP), run);
    EnableMenuItem(GetMenu(hMain), IDM_START, run ? MF_GRAYED : MF_ENABLED);
    EnableMenuItem(GetMenu(hMain), IDM_STOP, run ? MF_ENABLED : MF_GRAYED);
}

void ui_status(int state, const char *t)
{
    if (state == IP3_ST_RUNNING) {
        cur_bps = peak_bps = tot_bytes = elapsed = 0;
        nhist = 0;
        run_secs = ip3_duration();
        inval(&rcChart);
    }
    st_code = state;
    lstrcpyn(st_text, t, sizeof(st_text));
    if (!hMain) return;
    sync_controls();
    inval(&rcHead); inval(&rcBar); inval(&rcStats); inval(&rcGauge);
    UpdateWindow(hMain);
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
    inval(&rcGauge); inval(&rcStats); inval(&rcChart);
}

void ui_summary(const ip3_summary *s)
{
    cur_bps = s->recv_bps;
    tot_bytes = s->we_send ? s->sent_bytes : s->recv_bytes;
    elapsed = s->secs;
    inval(&rcGauge); inval(&rcStats);
}

/* ---- settings -------------------------------------------------- */
#ifdef IP3_WINNT
/* per-user registry key: the Windows directory is not writable for
   ordinary users on NT, and every profile keeps its own settings */
static void cfg_set(const char *key, const char *val)
{
    HKEY k;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, szKey, 0, NULL, REG_OPTION_NON_VOLATILE,
                       KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueEx(k, key, 0, REG_SZ, (const BYTE *)val, lstrlen(val) + 1);
    RegCloseKey(k);
}

static void cfg_get(const char *key, const char *def, char *out, int len)
{
    HKEY k;
    DWORD type, n = (DWORD)len - 1;
    lstrcpyn(out, def, len);
    if (RegOpenKeyEx(HKEY_CURRENT_USER, szKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueEx(k, key, NULL, &type, (BYTE *)out, &n) == ERROR_SUCCESS && type == REG_SZ)
        out[n] = 0;
    else
        lstrcpyn(out, def, len);
    RegCloseKey(k);
}
#else
static void cfg_set(const char *key, const char *val)
{
    WritePrivateProfileString(szSec, key, val, szIni);
}

static void cfg_get(const char *key, const char *def, char *out, int len)
{
    GetPrivateProfileString(szSec, key, def, out, len, szIni);
}
#endif

static int cfg_flag(const char *key)
{
    char b[8];
    cfg_get(key, "0", b, sizeof(b));
    return atoi(b) != 0;
}

/* ---- commands -------------------------------------------------- */
static void save_settings(void)
{
    static const int ids[] = { IDC_HOST, IDC_PORT, IDC_TIME, IDC_STREAMS, IDC_BLOCK };
    static const char *keys[] = { "Host", "Port", "Time", "Streams", "Block" };
    char b[80];
    int i;
    for (i = 0; i < 5; i++) {
        GetDlgItemText(hMain, ids[i], b, sizeof(b));
        cfg_set(keys[i], b);
    }
    cfg_set("Server", IsDlgButtonChecked(hMain, IDC_SERVER) ? "1" : "0");
    cfg_set("Reverse", IsDlgButtonChecked(hMain, IDC_REVERSE) ? "1" : "0");
}

static void load_settings(void)
{
    static const int ids[] = { IDC_HOST, IDC_PORT, IDC_TIME, IDC_STREAMS, IDC_BLOCK };
    static const char *keys[] = { "Host", "Port", "Time", "Streams", "Block" };
    static const char *defs[] = { "", "5201", "10", "1", "" };
    char b[80], d[16];
    int i, server;
    for (i = 0; i < 5; i++) {
        lstrcpy(d, defs[i]);
        if (i == 4) wsprintf(d, "%lu", (unsigned long)IP3_BLK_MAX);
        cfg_get(keys[i], d, b, sizeof(b));
        SetDlgItemText(hMain, ids[i], b);
    }
    server = cfg_flag("Server");
    CheckRadioButton(hMain, IDC_CLIENT, IDC_SERVER, server ? IDC_SERVER : IDC_CLIENT);
    CheckDlgButton(hMain, IDC_REVERSE, cfg_flag("Reverse"));
}

static void cmd_start(void)
{
    ip3_cfg c;
    char b[32];
    if (ip3_active() || busy) return;
    ip3_defaults(&c);
    GetDlgItemText(hMain, IDC_HOST, c.host, sizeof(c.host));
    c.server = IsDlgButtonChecked(hMain, IDC_SERVER) ? 1 : 0;
    c.reverse = IsDlgButtonChecked(hMain, IDC_REVERSE) ? 1 : 0;
    c.port = GetDlgItemInt(hMain, IDC_PORT, NULL, FALSE);
    c.duration = (int)GetDlgItemInt(hMain, IDC_TIME, NULL, FALSE);
    c.parallel = (int)GetDlgItemInt(hMain, IDC_STREAMS, NULL, FALSE);
    GetDlgItemText(hMain, IDC_BLOCK, b, sizeof(b));
    c.blksize = (unsigned long)atol(b);
    if (c.port == 0) c.port = IP3_DEFAULT_PORT;
    if (c.duration < 1) c.duration = 1;
    if (c.duration > 3600) c.duration = 3600;
    if (c.parallel < 1) c.parallel = 1;
    if (c.parallel > IP3_MAX_STREAMS) c.parallel = IP3_MAX_STREAMS;
    if (c.blksize < 64) c.blksize = 64;
    if (c.blksize > IP3_BLK_MAX) c.blksize = IP3_BLK_MAX;
    SetDlgItemInt(hMain, IDC_PORT, c.port, FALSE);
    SetDlgItemInt(hMain, IDC_TIME, c.duration, FALSE);
    SetDlgItemInt(hMain, IDC_STREAMS, c.parallel, FALSE);
    wsprintf(b, "%lu", c.blksize);
    SetDlgItemText(hMain, IDC_BLOCK, b);
    if (!c.server && c.host[0] == 0) {
        MessageBox(hMain, "Please enter the name or IP address of the test server.",
                   APP_TITLE, MB_OK | MB_ICONINFORMATION);
        SetFocus(GetDlgItem(hMain, IDC_HOST));
        return;
    }
    save_settings();
    busy = 1;                       /* name resolution may pump messages */
    sync_controls();
    ip3_start(&c);
    busy = 0;
    sync_controls();
}

static void cmd_stop(void)
{
    if (busy) return;
    ip3_stop();
    sync_controls();
}

static void cmd_copy(void)
{
    int n = (int)SendMessage(hLog, LB_GETCOUNT, 0, 0), i;
    long size = 1;
    HGLOBAL h;
    LPSTR p;
    for (i = 0; i < n; i++) size += SendMessage(hLog, LB_GETTEXTLEN, i, 0) + 2;
    if (size > 60000L) return;
    h = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!h) return;
    p = GlobalLock(h);
    for (i = 0; i < n; i++) {
        p += (int)SendMessage(hLog, LB_GETTEXT, i, (LPARAM)p);
        *p++ = '\r'; *p++ = '\n';
    }
    *p = 0;
    GlobalUnlock(h);
    if (OpenClipboard(hMain)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, h);
        CloseClipboard();
    } else GlobalFree(h);
}

#ifdef _WIN64
INT_PTR
#else
BOOL
#endif
CALLBACK EXPORT AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (msg == WM_INITDIALOG) return TRUE;
    if (msg == WM_COMMAND && (CMD_ID(wp, lp) == IDOK || CMD_ID(wp, lp) == IDCANCEL)) {
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

static HWND ctl(const char *cls, const char *txt, DWORD style, int x, int y, int w, int h, int id, int edgeStyle)
{
    HWND c;
#if defined(__NT__) || defined(_WIN32)
    c = CreateWindowEx(edgeStyle ? WS_EX_CLIENTEDGE : 0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                       x, y, w, h, hMain, (HMENU)(WPARAM)id, hInst, NULL);
#else
    (void)edgeStyle;
    c = CreateWindow(cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hMain, (HMENU)id, hInst, NULL);
#endif
    SendMessage(c, WM_SETFONT, (WPARAM)hfUI, 0);
    return c;
}

static void create_controls(void)
{
    int i;
    ctl("BUTTON", "&Client", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 88, 78, 56, 18, IDC_CLIENT, 0);
    ctl("BUTTON", "Ser&ver", BS_AUTORADIOBUTTON, 146, 78, 58, 18, IDC_SERVER, 0);
    ctl("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, 92, 104, 104, 20, IDC_HOST, 1);
    for (i = 0; i < 4; i++)
        ctl("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, 92, 130 + i * 26, 60, 20, IDC_PORT + i, 1);
    ctl("BUTTON", "&Reverse (server sends)", BS_AUTOCHECKBOX | WS_TABSTOP, 18, 236, 180, 18, IDC_REVERSE, 0);
    SendMessage(ctl("BUTTON", "&Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 18, 266, 88, 28, IDC_START, 0),
                WM_SETFONT, (WPARAM)hfBold, 0);
    SendMessage(ctl("BUTTON", "S&top", BS_PUSHBUTTON | WS_TABSTOP, 110, 266, 88, 28, IDC_STOP, 0),
                WM_SETFONT, (WPARAM)hfBold, 0);
    hLog = ctl("LISTBOX", "", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
               rcLog.left, rcLog.top, rcLog.right - rcLog.left, rcLog.bottom - rcLog.top, IDC_LOG, 1);
    SendMessage(hLog, WM_SETFONT, (WPARAM)hfMono, 0);
    SendDlgItemMessage(hMain, IDC_HOST, EM_LIMITTEXT, 63, 0);
    SendDlgItemMessage(hMain, IDC_PORT, EM_LIMITTEXT, 5, 0);
    SendDlgItemMessage(hMain, IDC_TIME, EM_LIMITTEXT, 4, 0);
    SendDlgItemMessage(hMain, IDC_STREAMS, EM_LIMITTEXT, 1, 0);
    SendDlgItemMessage(hMain, IDC_BLOCK, EM_LIMITTEXT, 7, 0);
}

LRESULT CALLBACK EXPORT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(hwnd);
        hMain = hwnd;
        hbmBack = CreateCompatibleBitmap(dc, CW, CH);
        ReleaseDC(hwnd, dc);
        create_controls();
        load_settings();
        sync_controls();
        SetTimer(hwnd, 1, 70, NULL);
        return 0; }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP old = SelectObject(mem, hbmBack);
        paint_all(mem, &ps.rcPaint);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0; }

    case WM_TIMER: {                        /* ease the needle towards the reading */
        double d = cur_bps - disp_bps, sc = nice_scale(peak_bps > cur_bps ? peak_bps : cur_bps);
        if (fabs(d) > sc / 400.0) { disp_bps += d * 0.35; inval(&rcGauge); }
        else if (d != 0) { disp_bps = cur_bps; inval(&rcGauge); }
        return 0; }

#if defined(__NT__) || defined(_WIN32)
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, C_FACE);
        return (LRESULT)hbrFace;
#else
    case WM_CTLCOLOR:
        if (HIWORD(lp) == CTLCOLOR_BTN || HIWORD(lp) == CTLCOLOR_STATIC) {
            SetBkColor((HDC)wp, C_FACE);
            return (LRESULT)(UINT)hbrFace;
        }
        break;
#endif

    case DM_GETDEFID:
        return MAKELONG(IDC_START, DC_HASDEFID);

    case WM_COMMAND:
        switch (CMD_ID(wp, lp)) {
        case IDOK: case IDC_START: case IDM_START: cmd_start(); break;
        case IDCANCEL: case IDC_STOP: case IDM_STOP: cmd_stop(); break;
        case IDC_CLIENT: case IDC_SERVER: sync_controls(); break;
        case IDM_CLEAR: SendMessage(hLog, LB_RESETCONTENT, 0, 0); break;
        case IDM_COPY:  cmd_copy(); break;
        case IDM_EXIT:  PostMessage(hwnd, WM_CLOSE, 0, 0); break;
        case IDM_ABOUT: DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUT), hwnd, (DLGPROC)AboutProc); break;
        }
        return 0;

    case WM_CLOSE:
        if (busy) return 0;
        if (ip3_active()) ip3_stop();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (hbmBack) DeleteObject(hbmBack);
        hLog = 0;
        hMain = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    static char cls[] = "ArrowMain";
    WNDCLASS wc;
    MSG msg;
    RECT r;
    char err[120];
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    DWORD t0;
    int quit = 0, sr;
    const char *face = "MS Sans Serif";

    (void)cmd;
    hInst = inst;
    hbrFace = CreateSolidBrush(C_FACE);
#ifdef IP3_WINNT
    if (LOBYTE(LOWORD(GetVersion())) >= 5) face = "Tahoma";     /* Windows 2000 UI font */
#endif
    hfUI    = mkfont(11, 0, face, 0);
    hfBold  = mkfont(11, 1, face, 0);
    hfSmall = mkfont(10, 0, face, 0);
    hfTitle = mkfont(22, 1, "Arial", 0);
    hfBig   = mkfont(24, 1, "Arial", 0);
    hfMono  = mkfont(12, 0, "Courier New", 1);

    if (!prev) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APP));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = hbrFace;
        wc.lpszMenuName = MAKEINTRESOURCE(IDM_MAIN);
        wc.lpszClassName = cls;
        if (!RegisterClass(&wc)) return 0;
    }
    r.left = 0; r.top = 0; r.right = CW; r.bottom = CH;
    AdjustWindowRect(&r, style, TRUE);
    if (!CreateWindow(cls, APP_TITLE " " APP_FLAVOUR, style, CW_USEDEFAULT, CW_USEDEFAULT,
                      r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL))
        return 0;
    ShowWindow(hMain, show);
    UpdateWindow(hMain);

    if (net_init(err, sizeof(err)) != 0) {
        MessageBox(hMain, err, APP_TITLE, MB_OK | MB_ICONSTOP);
        DestroyWindow(hMain);
        return 1;
    }
    ui_log("Welcome to Arrow. Enter a server and press Start, or switch to Server mode.");

    while (!quit) {
        if (ip3_active()) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit = 1; break; }
                if (!IsDialogMessage(hMain, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
            }
            if (quit) break;
            t0 = GetTickCount();
            do {
                sr = ip3_step();
                if (sr != 1) { if (sr == 2) net_idle(); break; }
            } while (GetTickCount() - t0 < 30);
            if (sr == 0) sync_controls();
        } else {
            if (!GetMessage(&msg, NULL, 0, 0)) break;
            if (!IsDialogMessage(hMain, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        }
    }

    net_shutdown();
    DeleteObject(hbrFace);
    DeleteObject(hfUI); DeleteObject(hfBold); DeleteObject(hfSmall);
    DeleteObject(hfTitle); DeleteObject(hfBig); DeleteObject(hfMono);
    return 0;
}
