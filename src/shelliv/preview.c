/* ShellIV - the preview pane: a large icon, the details of the selection
 * and a look inside it: text, bitmaps, program descriptions, a hex dump,
 * or a pie chart of a drive. */
#include <math.h>
#include "shelliv.h"

HWND hPreview;

enum { PV_NONE, PV_TEXT, PV_HEX, PV_BITMAP, PV_ICONS, PV_PIE };

static int kind;
static char *text;              /* PV_TEXT / PV_HEX: text to show */
static BYTE *dib;               /* PV_BITMAP: header, colours and bits */
static long dibSize;
static char exeDesc[120];       /* program description (NE module) */
static char path[MAXP];
static ITEM sel;                /* copy of the selected item */
static char selName[MAXP], selSub[MAXP];
static int nSel;
static DWORD selBytes;
static int nIcons;

static void Clear(void)
{
    if (text)
        free(text);
    if (dib)
        free(dib);
    text = NULL;
    dib = NULL;
    kind = PV_NONE;
    exeDesc[0] = '\0';
    nIcons = 0;
}

static BOOL ReadHead(const char *file, BYTE *buf, int size, long at, int *got)
{
    HFILE f = _lopen(file, OF_READ | OF_SHARE_DENY_NONE);

    *got = 0;
    if (f == HFILE_ERROR)
        return FALSE;
    if (at)
        _llseek(f, at, 0);
    *got = (int)_lread(f, buf, size);
    _lclose(f);
    if (*got < 0)
        *got = 0;
    return TRUE;
}

#define LE16(b, o) ((WORD)((b)[o] | ((WORD)(b)[(o) + 1] << 8)))
#define LE32(b, o) ((DWORD)LE16(b, o) | ((DWORD)LE16(b, (o) + 2) << 16))

/* "Windows application" plus the module description of NE files. */
static void ExeInfo(const char *file)
{
    BYTE h[64], ne[64], d[130];
    int got;
    DWORD off;

    if (!ReadHead(file, h, 64, 0, &got) || got < 64 || h[0] != 'M' || h[1] != 'Z') {
        strcpy(exeDesc, "MS-DOS program");
        return;
    }
    off = LE32(h, 0x3C);
    if (LE16(h, 0x18) < 0x40 || !ReadHead(file, ne, 64, (long)off, &got) || got < 64) {
        strcpy(exeDesc, "MS-DOS program");
        return;
    }
    if (ne[0] == 'P' && ne[1] == 'E') {
        strcpy(exeDesc, "32-bit Windows program (Win32)");
        return;
    }
    if (ne[0] != 'N' || ne[1] != 'E') {
        strcpy(exeDesc, ne[0] == 'L' ? "OS/2 or VxD module" : "MS-DOS program");
        return;
    }
    /* the first entry of the non-resident name table is the description */
    if (ReadHead(file, d, sizeof(d) - 1, (long)LE32(ne, 0x2C), &got) && got > 1) {
        int n = min((int)d[0], got - 1);

        memcpy(exeDesc, d + 1, n);
        exeDesc[n] = '\0';
    }
    if (!exeDesc[0])
        strcpy(exeDesc, (ne[0x36] & 2) ? "Windows program" : "OS/2 program");
}

static BOOL IsTextual(const BYTE *b, int n)
{
    int i, odd = 0;

    for (i = 0; i < n; i++) {
        if (b[i] == 0)
            return FALSE;
        if (b[i] < 9 || (b[i] > 13 && b[i] < 32 && b[i] != 26))
            odd++;
    }
    return odd * 20 <= n;
}

static void LoadText(const char *file)
{
    BYTE *b = (BYTE *)malloc(3001);
    int got, i, n = 0;

    if (!b)
        return;
    ReadHead(file, b, 3000, 0, &got);
    text = (char *)malloc(got + 1);
    if (!text) {
        free(b);
        return;
    }
    for (i = 0; i < got; i++) {
        if (b[i] == 26)
            break;
        if (b[i] != '\r')
            text[n++] = (char)(b[i] == '\n' ? '\n' : b[i] < 32 && b[i] != '\t' ? ' ' : b[i]);
    }
    text[n] = '\0';
    free(b);
    kind = PV_TEXT;
}

static void LoadHex(const char *file)
{
    BYTE b[128];
    int got, i, j;
    char *s;

    ReadHead(file, b, 128, 0, &got);
    text = (char *)malloc(16 * 80);
    if (!text)
        return;
    s = text;
    for (i = 0; i < got; i += 8) {
        for (j = 0; j < 8; j++)
            s += i + j < got ? sprintf(s, "%02X ", b[i + j]) : sprintf(s, "   ");
        *s++ = ' ';
        for (j = 0; j < 8 && i + j < got; j++)
            *s++ = (char)(b[i + j] >= 32 && b[i + j] < 127 ? b[i + j] : '.');
        *s++ = '\n';
    }
    *s = '\0';
    kind = PV_HEX;
}

static void LoadBitmap_(const char *file)
{
    HFILE f;
    long size;

    f = _lopen(file, OF_READ | OF_SHARE_DENY_NONE);
    if (f == HFILE_ERROR)
        return;
    size = _llseek(f, 0, 2);
    _llseek(f, 0, 0);
#ifdef RETRO_WIN16
    if (size > 60000L)
        size = 0;               /* too large for one 16-bit segment */
#endif
    if (size > 54 && size < 4L * 1024 * 1024) {
        dib = (BYTE *)malloc((size_t)size);
        if (dib && (long)_lread(f, dib, (UINT)size) == size && dib[0] == 'B' && dib[1] == 'M'
            && LE32(dib, 14) == 40 && LE16(dib, 28) <= 24 && LE32(dib, 30) == 0) {
            dibSize = size;
            kind = PV_BITMAP;
        } else if (dib) {
            free(dib);
            dib = NULL;
        }
    }
    _lclose(f);
}

void PreviewUpdate(void)
{
    ITEM *it = ViewFocusItem();
    int i;

    Clear();
    nSel = ViewSelCount();
    selBytes = 0;
    for (i = 0; i < nItems; i++)
        if (items[i]->sel)
            selBytes += items[i]->size;
    if (nSel == 1 && !it)
        for (i = 0; i < nItems; i++)
            if (items[i]->sel)
                it = items[i];
    if (it && nSel == 1) {
        sel = *it;
        lstrcpyn(selName, it->name, MAXP);
        lstrcpyn(selSub, it->sub ? it->sub : "", MAXP);
        sel.name = selName;
        sel.sub = selSub;
        ViewItemPath(it, path);
        if (it->kind == IK_DRIVE) {
            if (it->total)
                kind = PV_PIE;
        } else if (it->kind == IK_FILE && hPreview && IsWindowVisible(hPreview)) {
            const char *ext = PathExt(path);
            BYTE head[512];
            int got;

            if (!lstrcmpi(ext, ".EXE") || !lstrcmpi(ext, ".DLL") || !lstrcmpi(ext, ".DRV")
                || !lstrcmpi(ext, ".CPL") || !lstrcmpi(ext, ".COM")) {
                ExeInfo(path);
                if (!lstrcmpi(ext, ".COM"))
                    strcpy(exeDesc, "MS-DOS program");
                nIcons = lstrcmpi(ext, ".EXE") == 0 || lstrcmpi(ext, ".DLL") == 0 ? FileIconCount(path) : 0;
                kind = nIcons > 0 ? PV_ICONS : PV_NONE;
                if (kind == PV_NONE)
                    LoadHex(path);
            } else if (!lstrcmpi(ext, ".BMP")) {
                LoadBitmap_(path);
            } else if (!lstrcmpi(ext, ".ICO")) {
                nIcons = 1;
                kind = PV_ICONS;
            } else if (it->size && ReadHead(path, head, sizeof(head), 0, &got)) {
                if (IsTextual(head, got))
                    LoadText(path);
                else
                    LoadHex(path);
            }
        }
    }
    if (hPreview)
        InvalidateRect(hPreview, NULL, FALSE);
}

void PreviewDone(void)
{
    Clear();
}

/* ---- painting ------------------------------------------------------------ */

static void DrawBig(HDC hdc, int x, int y, const ITEM *it, COLORREF bg)
{
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, 32, 32), old;
    RECT r;

    if (!mem || !bmp) {
        if (mem)
            DeleteDC(mem);
        DrawItemIcon(hdc, x + 16, y + 16, it);
        return;
    }
    old = SelectObject(mem, bmp);
    SetRect(&r, 0, 0, 32, 32);
    FillR(mem, &r, bg);
    DrawItemIcon(mem, 0, 0, it);
    StretchBlt(hdc, x, y, 64, 64, mem, 0, 0, 32, 32, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static int Row(HDC hdc, int y, int w, const char *label, const char *value)
{
    RECT r;
    int h;

    if (!value || !value[0])
        return y;
    SelectObject(hdc, fontSmall);
    SetRect(&r, 14, y, w - 12, y + 12);
    TextIn(hdc, &r, label, th.dimText, DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, fontUI);
    SetRect(&r, 14, y + 11, w - 12, y + 11 + 3 * lineH);
    h = DrawText(hdc, value, -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SetRect(&r, 14, y + 11, w - 12, y + 11 + min(h, 3 * lineH));
    TextIn(hdc, &r, value, th.viewText, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    return y + 15 + min(h, 3 * lineH);
}

static void Pie3D(HDC hdc, int x, int y, int w, int h, DWORD freeKB, DWORD totalKB)
{
    int depth = 10, d, cx = x + w / 2, cy = y + h / 2;
    long ang;
    int ex, ey;
    HPEN pen, op;
    HBRUSH b1, b2, ob;
    double a;
    COLORREF used = th.gauge, free_ = Mix(th.gauge, RGB(255, 255, 255), 75);

    if (totalKB / 32 * 10 - freeKB / 32 * 10 >= totalKB / 32 * 9)
        used = th.gaugeFull;
    ang = totalKB ? (long)((totalKB - freeKB) / 16 * 3600 / max(1UL, totalKB / 16)) : 0;
    a = ang * 3.14159265 / 1800.0;
    /* the end of the "used" wedge, counter-clockwise from 3 o'clock */
    ex = cx + (int)(1000 * cos(a));
    ey = cy - (int)(1000 * sin(a));
    pen = CreatePen(PS_SOLID, 1, PC(Mix(th.viewText, th.view, 40)));
    op = SelectObject(hdc, pen);
    for (d = depth; d >= 0; d--) {
        COLORREF cu = d ? Mix(used, RGB(0, 0, 0), 35) : used;
        COLORREF cf = d ? Mix(free_, RGB(0, 0, 0), 25) : free_;

        b1 = CreateSolidBrush(PC(cu));
        b2 = CreateSolidBrush(PC(cf));
        if (d && d != depth)
            SelectObject(hdc, GetStockObject(NULL_PEN));
        else
            SelectObject(hdc, pen);
        ob = SelectObject(hdc, b2);
        if (ang <= 0)
            Ellipse(hdc, x, y + d, x + w, y + h + d);
        else if (ang < 3600) {
            Pie(hdc, x, y + d, x + w, y + h + d, ex, ey + d, cx + 1000, cy + d);
            SelectObject(hdc, b1);
            Pie(hdc, x, y + d, x + w, y + h + d, cx + 1000, cy + d, ex, ey + d);
        } else {
            SelectObject(hdc, b1);
            Ellipse(hdc, x, y + d, x + w, y + h + d);
        }
        SelectObject(hdc, ob);
        DeleteObject(b1);
        DeleteObject(b2);
    }
    SelectObject(hdc, op);
    DeleteObject(pen);
}

static void PaintBitmap(HDC hdc, const RECT *area)
{
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)(dib + 14);
    long w = bi->biWidth, h = bi->biHeight, stride;
    int bpp = bi->biBitCount, ncol, dw, dh, x, y;
    BYTE *bits;
    long row0, rows, per;
    BOOL topDown = h < 0;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } hdr;

    if (topDown)
        h = -h;
    if (w <= 0 || h <= 0)
        return;
    ncol = bi->biClrUsed ? (int)bi->biClrUsed : bpp <= 8 ? 1 << bpp : 0;
    stride = ((w * bpp + 31) / 32) * 4;
    bits = dib + LE32(dib, 10);
    if (bits + stride * h > dib + dibSize)
        return;
    dw = area->right - area->left;
    dh = area->bottom - area->top;
    if (w * dh > h * dw)
        dh = (int)(h * dw / w);
    else
        dw = (int)(w * dh / h);
    if (w <= dw && h <= dh) {
        dw = (int)w;
        dh = (int)h;
    }
    x = area->left + (area->right - area->left - dw) / 2;
    y = area->top + (area->bottom - area->top - dh) / 2;
    memcpy(&hdr.h, bi, sizeof(BITMAPINFOHEADER));
    memcpy(hdr.c, dib + 14 + 40, ncol * sizeof(RGBQUAD));
    SetStretchBltMode(hdc, COLORONCOLOR);
    /* draw in bands of under 60 KB: 16-bit callers cannot pass more */
    per = max(1L, 60000L / stride);
    for (row0 = 0; row0 < h; row0 += per) {
        long y0, y1;

        rows = min(per, h - row0);
        hdr.h.biHeight = topDown ? -rows : rows;
        hdr.h.biSizeImage = 0;
        /* destination rows for source rows [row0, row0 + rows) in top-down order */
        if (topDown) {
            y0 = row0;
        } else {
            y0 = h - row0 - rows;
        }
        y1 = y0 + rows;
        {
            int dy0 = (int)(y0 * dh / h), dy1 = (int)(y1 * dh / h);

            if (dy1 > dy0)
                StretchDIBits(hdc, x, y + dy0, dw, dy1 - dy0, 0, 0, (int)w, (int)rows,
                              bits + row0 * stride, (BITMAPINFO *)&hdr, DIB_RGB_COLORS, SRCCOPY);
        }
    }
    FrameR(hdc, area, Mix(th.view, th.viewText, 15));
}

static void Paint(HWND hwnd, HDC hdc, const RECT *rc)
{
    RECT cl, r;
    int w, y;
    char a[40], b[40], c[MAXP];
    COLORREF bg = Mix(th.view, th.bar2, 35);

    GetClientRect(hwnd, &cl);
    w = cl.right;
    FillR(hdc, rc, bg);
    VLine(hdc, 0, 0, cl.bottom, Mix(th.bar2, RGB(0, 0, 0), 15));
    SelectObject(hdc, fontUI);
    if (nSel != 1) {
        SetRect(&r, 14, 30, w - 12, cl.bottom);
        if (nSel > 1) {
            FmtSize(a, selBytes);
            sprintf(c, "%d items selected\n\n%s", nSel, a);
            SelectObject(hdc, fontTitle);
            SetRect(&r, 14, 24, w - 12, 60);
            sprintf(b, "%d", nSel);
            TextIn(hdc, &r, b, th.accent, DT_LEFT | DT_SINGLELINE);
            SelectObject(hdc, fontUI);
            SetRect(&r, 14, 62, w - 12, 120);
            sprintf(c, "items selected\n%s in total", a);
            TextIn(hdc, &r, c, th.viewText, DT_LEFT | DT_WORDBREAK);
        } else {
            SelectObject(hdc, fontBold);
            TextIn(hdc, &r, "Nothing selected", th.viewText, DT_LEFT | DT_SINGLELINE);
            SelectObject(hdc, fontUI);
            SetRect(&r, 14, 52, w - 12, cl.bottom);
            TextIn(hdc, &r,
                   "Select an item to see its details here.\n\n"
                   "Tips\n"
                   "\x95 Enter opens, Backspace goes up\n"
                   "\x95 F2 renames, Del moves to the Wastebasket\n"
                   "\x95 Ctrl+C / Ctrl+X / Ctrl+V copy and move files\n"
                   "\x95 Drag files onto a group to add programs\n"
                   "\x95 Hold Ctrl while dragging to copy\n"
                   "\x95 Ctrl+F finds files, also by contents\n"
                   "\x95 Ctrl+D adds the folder to Favorites\n"
                   "\x95 Type a path or program in the address bar",
                   th.dimText, DT_LEFT | DT_WORDBREAK);
        }
        return;
    }
    DrawBig(hdc, (w - 64) / 2, 16, &sel, bg);
    SelectObject(hdc, fontBold);
    SetRect(&r, 10, 88, w - 10, 88 + 2 * lineH + 2);
    TextIn(hdc, &r, sel.name, th.viewText, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, fontUI);
    ItemTypeName(&sel, a);
    SetRect(&r, 10, 90 + 2 * lineH, w - 10, 90 + 3 * lineH + 2);
    TextIn(hdc, &r, exeDesc[0] ? exeDesc : a, th.dimText, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y = 98 + 3 * lineH;
    HLine(hdc, 14, w - 12, y, Mix(bg, th.viewText, 15));
    y += 8;
    switch (sel.kind) {
    case IK_DRIVE:
        if (sel.total) {
            FmtKB(a, sel.total - sel.size);
            y = Row(hdc, y, w, "USED", a);
            FmtKB(a, sel.size);
            y = Row(hdc, y, w, "FREE", a);
            FmtKB(a, sel.total);
            y = Row(hdc, y, w, "CAPACITY", a);
        }
        break;
    case IK_PROG: {
        GROUP *gr = GroupPtr(cur.kind == LOC_RECENT ? -1 : cur.group);

        y = Row(hdc, y, w, "COMMAND LINE", sel.sub);
        if (gr && sel.ref >= 0 && sel.ref < gr->n) {
            y = Row(hdc, y, w, "WORKING DIRECTORY", gr->items[sel.ref].dir);
            if (LOBYTE(gr->items[sel.ref].hotkey)) {
                HotkeyText(gr->items[sel.ref].hotkey, a);
                y = Row(hdc, y, w, "SHORTCUT KEY", a);
            }
            if (gr->items[sel.ref].iconPath && gr->items[sel.ref].iconPath[0])
                y = Row(hdc, y, w, "ICON", gr->items[sel.ref].iconPath);
        }
        break;
    }
    case IK_GROUP:
        sprintf(a, "%lu", sel.size);
        y = Row(hdc, y, w, "PROGRAMS", a);
        break;
    default:
        if (sel.kind == IK_FILE || (sel.kind == IK_WASTE && !(sel.attr & FA_DIR))) {
            FmtSize(a, sel.size);
            FmtNum(b, sel.size);
            if (sel.size >= 1024)
                sprintf(a + strlen(a), " (%s bytes)", b);
            y = Row(hdc, y, w, "SIZE", a);
        }
        FmtDate(a, sel.date, sel.time);
        y = Row(hdc, y, w, sel.kind == IK_WASTE ? "DELETED" : "MODIFIED", a);
        if (sel.kind == IK_WASTE)
            y = Row(hdc, y, w, "ORIGINAL LOCATION", sel.sub);
        else if (cur.kind == LOC_SEARCH)
            y = Row(hdc, y, w, "FOLDER", sel.sub);
        if (sel.attr & (FA_RDONLY | FA_HIDDEN | FA_SYSTEM)) {
            c[0] = '\0';
            if (sel.attr & FA_RDONLY) strcat(c, "Read-only  ");
            if (sel.attr & FA_HIDDEN) strcat(c, "Hidden  ");
            if (sel.attr & FA_SYSTEM) strcat(c, "System");
            y = Row(hdc, y, w, "ATTRIBUTES", c);
        }
    }
    y += 6;
    if (y >= cl.bottom - 20)
        return;
    SetRect(&r, 12, y, w - 10, cl.bottom - 10);
    switch (kind) {
    case PV_PIE: {
        int pw = min(w - 40, 150), ph = pw / 2;

        Pie3D(hdc, (w - pw) / 2, y + 4, pw, ph, sel.size, sel.total);
        y += ph + 22;
        FillXY(hdc, 20, y, 10, 10, th.gauge);
        TextAt(hdc, 36, y - 2, "Used space", th.viewText);
        FillXY(hdc, 20, y + 16, 10, 10, Mix(th.gauge, RGB(255, 255, 255), 75));
        TextAt(hdc, 36, y + 14, "Free space", th.viewText);
        break;
    }
    case PV_TEXT:
    case PV_HEX: {
        HFONT f = (HFONT)GetStockObject(kind == PV_HEX ? ANSI_FIXED_FONT : ANSI_VAR_FONT);

        FillR(hdc, &r, th.view);
        FrameR(hdc, &r, Mix(th.view, th.viewText, 15));
        InflateRect(&r, -6, -4);
        SelectObject(hdc, kind == PV_HEX ? f : fontSmall);
        if (kind == PV_HEX)
            SelectObject(hdc, f);
        TextIn(hdc, &r, text, kind == PV_HEX ? th.dimText : th.viewText,
               DT_LEFT | DT_EXPANDTABS | (kind == PV_TEXT ? DT_WORDBREAK : 0));
        SelectObject(hdc, fontUI);
        break;
    }
    case PV_BITMAP:
        if (r.bottom - r.top > 30) {
            r.bottom = min(r.bottom, r.top + (r.right - r.left) * 3 / 4 + 20);
            PaintBitmap(hdc, &r);
        }
        break;
    case PV_ICONS: {
        int i, x = 14, n = min(nIcons, 24);

        SelectObject(hdc, fontSmall);
        sprintf(a, nIcons == 1 ? "1 ICON" : "%d ICONS", nIcons);
        TextIn(hdc, &r, a, th.dimText, DT_LEFT | DT_SINGLELINE);
        SelectObject(hdc, fontUI);
        y += 16;
        for (i = 0; i < n; i++) {
            HICON h = FileIcon(path, i);

            if (x + 36 > w) {
                x = 14;
                y += 38;
            }
            if (y + 34 > cl.bottom)
                break;
            if (h)
                DrawIcon(hdc, x, y, h);
            x += 38;
        }
        break;
    }
    }
}

LRESULT CALLBACK __export PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        hPreview = hwnd;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
        BufferedPaint(hwnd, Paint);
        return 0;
    case WM_SHOWWINDOW:
        if (wParam)
            PostMessage(hwnd, WM_USER, 0, 0);
        break;
    case WM_USER:
        PreviewUpdate();
        return 0;
    case WM_DESTROY:
        Clear();
        hPreview = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL PreviewRegister(void)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = (WNDPROC)PreviewProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ShellIVPreview";
    return RegisterClass(&wc) != 0;
}
