/* ShellIV - colour themes, fonts and drawing primitives. */
#include "shelliv.h"

static const THEME themes[] = {
    { "Midnight",
      RGB(24, 52, 96), RGB(36, 128, 150), RGB(255, 255, 255),
      RGB(27, 36, 54), RGB(214, 222, 235), RGB(110, 168, 220), RGB(59, 110, 165),
      RGB(255, 255, 255), RGB(42, 54, 80),
      RGB(255, 255, 255), RGB(20, 24, 32), RGB(110, 118, 130), RGB(244, 247, 251),
      RGB(204, 224, 245), RGB(59, 110, 165), RGB(10, 20, 40),
      RGB(246, 248, 251), RGB(220, 227, 237), RGB(30, 36, 48), RGB(196, 218, 244),
      RGB(59, 130, 200), RGB(210, 60, 50), RGB(59, 110, 165) },
    { "Sunrise",
      RGB(170, 40, 60), RGB(240, 150, 40), RGB(255, 255, 255),
      RGB(253, 246, 236), RGB(74, 56, 40), RGB(200, 90, 40), RGB(232, 120, 50),
      RGB(255, 255, 255), RGB(246, 228, 206),
      RGB(255, 255, 255), RGB(40, 30, 20), RGB(140, 120, 100), RGB(253, 249, 243),
      RGB(253, 226, 196), RGB(232, 120, 50), RGB(60, 30, 10),
      RGB(255, 251, 245), RGB(244, 232, 214), RGB(70, 50, 30), RGB(252, 220, 180),
      RGB(232, 120, 50), RGB(190, 30, 30), RGB(232, 120, 50) },
    { "Forest",
      RGB(28, 80, 48), RGB(120, 170, 70), RGB(255, 255, 255),
      RGB(30, 48, 38), RGB(214, 230, 214), RGB(150, 200, 120), RGB(70, 130, 80),
      RGB(255, 255, 255), RGB(44, 66, 54),
      RGB(252, 253, 250), RGB(24, 34, 24), RGB(110, 124, 110), RGB(243, 248, 240),
      RGB(214, 236, 204), RGB(70, 130, 80), RGB(20, 40, 20),
      RGB(248, 251, 245), RGB(226, 236, 220), RGB(30, 48, 30), RGB(205, 230, 195),
      RGB(80, 150, 80), RGB(200, 60, 40), RGB(70, 130, 80) },
    { "Graphite",
      RGB(44, 44, 48), RGB(110, 110, 120), RGB(255, 255, 255),
      RGB(38, 38, 42), RGB(220, 220, 224), RGB(240, 170, 60), RGB(240, 170, 60),
      RGB(30, 30, 30), RGB(58, 58, 64),
      RGB(250, 250, 250), RGB(30, 30, 30), RGB(120, 120, 124), RGB(242, 242, 244),
      RGB(252, 230, 190), RGB(220, 150, 40), RGB(30, 30, 30),
      RGB(245, 245, 246), RGB(220, 220, 224), RGB(40, 40, 44), RGB(252, 226, 180),
      RGB(230, 160, 50), RGB(200, 50, 50), RGB(220, 150, 40) },
    { "Amethyst",
      RGB(60, 30, 110), RGB(180, 80, 160), RGB(255, 255, 255),
      RGB(246, 242, 250), RGB(50, 36, 70), RGB(120, 70, 170), RGB(120, 70, 170),
      RGB(255, 255, 255), RGB(232, 224, 244),
      RGB(255, 255, 255), RGB(30, 20, 40), RGB(120, 110, 135), RGB(249, 246, 252),
      RGB(232, 220, 246), RGB(120, 70, 170), RGB(40, 20, 60),
      RGB(252, 250, 254), RGB(232, 226, 242), RGB(50, 36, 70), RGB(224, 210, 244),
      RGB(140, 80, 190), RGB(200, 50, 60), RGB(120, 70, 170) },
    { "Classic",
      RGB(0, 0, 128), RGB(0, 128, 128), RGB(255, 255, 255),
      RGB(255, 255, 255), RGB(0, 0, 0), RGB(0, 0, 128), RGB(0, 0, 128),
      RGB(255, 255, 255), RGB(224, 224, 224),
      RGB(255, 255, 255), RGB(0, 0, 0), RGB(128, 128, 128), RGB(255, 255, 255),
      RGB(0, 0, 128), RGB(0, 0, 128), RGB(255, 255, 255),
      RGB(192, 192, 192), RGB(192, 192, 192), RGB(0, 0, 0), RGB(224, 224, 224),
      RGB(0, 0, 128), RGB(128, 0, 0), RGB(0, 0, 128) },
};

THEME th;
int themeIdx;
int nThemes = sizeof(themes) / sizeof(themes[0]);
HFONT fontUI, fontBold, fontTitle, fontSmall, fontHead;
int lineH = 13;

static BOOL lowColor;           /* 16 or 256 colours: use solid colours */
static BOOL palDevice;          /* 256 colours: ShellIV brings its own palette */
HPALETTE hPal;
static HBITMAP hbmGlyphs, hbmGlyphsM;
static HICON icons[BI_COUNT];

static void BuildPalette(void);

static HFONT MakeFont(HDC hdc, int pt, int weight, const char *face)
{
    LOGFONT lf;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    lf.lfWeight = weight;
    lf.lfCharSet = ANSI_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    lstrcpy(lf.lfFaceName, face);
    return CreateFontIndirect(&lf);
}

void ThemeInit(void)
{
    HDC hdc = GetDC(NULL);
    TEXTMETRIC tm;
    HFONT old;

    lowColor = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES) <= 8;
    palDevice = lowColor && (GetDeviceCaps(hdc, RASTERCAPS) & RC_PALETTE) != 0;
    fontUI = MakeFont(hdc, 8, FW_NORMAL, "MS Sans Serif");
    fontBold = MakeFont(hdc, 8, FW_BOLD, "MS Sans Serif");
    fontSmall = MakeFont(hdc, 7, FW_NORMAL, "Small Fonts");
    fontHead = MakeFont(hdc, 7, FW_BOLD, "Arial");
    fontTitle = MakeFont(hdc, 15, FW_BOLD, "Arial");
    old = SelectObject(hdc, fontUI);
    GetTextMetrics(hdc, &tm);
    lineH = tm.tmHeight;
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
    hbmGlyphs = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_GLYPHS));
    hbmGlyphsM = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_GLYPHSM));
    ThemeSet(IniInt("Settings", "Theme", 0));
}

void ThemeDone(void)
{
    int i;

    DeleteObject(fontUI);
    DeleteObject(fontBold);
    DeleteObject(fontSmall);
    DeleteObject(fontHead);
    DeleteObject(fontTitle);
    DeleteObject(hbmGlyphs);
    DeleteObject(hbmGlyphsM);
    if (hPal)
        DeleteObject(hPal);
    for (i = 0; i < BI_COUNT; i++)
        if (icons[i])
            DestroyIcon(icons[i]);
}

/* ---- palette for 256-colour displays -------------------------------------- */

#define GRAD_STEPS_LOW 32

static LOGPALETTE *lp;

static void AddColor(COLORREF c)
{
    int i;

    if (lp->palNumEntries >= 236)
        return;
    for (i = 0; i < lp->palNumEntries; i++)
        if (RGB(lp->palPalEntry[i].peRed, lp->palPalEntry[i].peGreen, lp->palPalEntry[i].peBlue) == (c & 0xFFFFFFL))
            return;
    lp->palPalEntry[lp->palNumEntries].peRed = GetRValue(c);
    lp->palPalEntry[lp->palNumEntries].peGreen = GetGValue(c);
    lp->palPalEntry[lp->palNumEntries].peBlue = GetBValue(c);
    lp->palPalEntry[lp->palNumEntries].peFlags = 0;
    lp->palNumEntries++;
}

static void AddRamp(COLORREF a, COLORREF b, int n)
{
    int i;

    for (i = 0; i < n; i++)
        AddColor(Mix(a, b, n > 1 ? i * 100 / (n - 1) : 0));
}

/* Every colour ShellIV paints with, so fills are solid and gradients smooth. */
static void BuildPalette(void)
{
    static const int pcts[] = { 10, 15, 18, 20, 22, 25, 30, 35, 40, 45, 50, 55, 60, 70, 75 };
    COLORREF base[24];
    int i, j, n = 0;

    lp = (LOGPALETTE *)malloc(sizeof(LOGPALETTE) + 256 * sizeof(PALETTEENTRY));
    if (!lp)
        return;
    lp->palVersion = 0x300;
    lp->palNumEntries = 0;
    AddRamp(th.band1, th.band2, GRAD_STEPS_LOW);
    AddRamp(th.bar1, th.bar2, 16);
    AddRamp(Mix(th.gauge, RGB(255, 255, 255), 35), th.gauge, 8);
    AddRamp(Mix(th.gaugeFull, RGB(255, 255, 255), 35), th.gaugeFull, 8);
    AddRamp(Mix(th.gauge, RGB(255, 255, 255), 40), th.gauge, 8);
    base[n++] = th.band1; base[n++] = th.band2; base[n++] = th.bandText; base[n++] = th.side;
    base[n++] = th.sideText; base[n++] = th.sideHead; base[n++] = th.sideSel; base[n++] = th.sideSelText;
    base[n++] = th.sideHot; base[n++] = th.view; base[n++] = th.viewText; base[n++] = th.dimText;
    base[n++] = th.stripe; base[n++] = th.sel; base[n++] = th.selBorder; base[n++] = th.selText;
    base[n++] = th.bar1; base[n++] = th.bar2; base[n++] = th.barText; base[n++] = th.barHot;
    base[n++] = th.gauge; base[n++] = th.gaugeFull; base[n++] = th.accent;
    for (i = 0; i < n; i++)
        AddColor(base[i]);
    /* the blends used for hover, focus, borders and the preview pane */
    for (j = 0; j < (int)(sizeof(pcts) / sizeof(pcts[0])); j++) {
        AddColor(Mix(th.sel, th.view, pcts[j]));
        AddColor(Mix(th.view, th.bar2, pcts[j]));
        AddColor(Mix(th.bar2, RGB(0, 0, 0), pcts[j]));
        AddColor(Mix(th.side, RGB(0, 0, 0), pcts[j]));
        AddColor(Mix(th.band1, RGB(255, 255, 255), pcts[j]));
        AddColor(Mix(th.band1, RGB(0, 0, 0), pcts[j]));
        AddColor(Mix(th.sideText, th.side, pcts[j]));
        AddColor(Mix(th.sideHead, th.side, pcts[j]));
        AddColor(Mix(th.selBorder, th.view, pcts[j]));
        AddColor(Mix(th.view, th.viewText, pcts[j]));
    }
    AddColor(Mix(th.bandText, th.band2, 22));
    AddColor(Mix(th.gauge, RGB(255, 255, 255), 75));
    AddColor(Mix(th.gauge, RGB(0, 0, 0), 35));
    AddColor(Mix(Mix(th.gauge, RGB(255, 255, 255), 75), RGB(0, 0, 0), 25));
    AddColor(Mix(th.barHot, th.bar1, 30));
    AddColor(Mix(th.selBorder, th.barHot, 50));
    AddColor(Mix(th.bar1, RGB(255, 255, 255), 50));
    AddColor(Mix(th.selText, th.sel, 25));
    if (hPal)
        DeleteObject(hPal);
    hPal = CreatePalette(lp);
    free(lp);
    lp = NULL;
    if (hFrame)
        PalRealize(hFrame);
}

COLORREF PC(COLORREF c)
{
    if (palDevice)
        return (c & 0x00FFFFFFL) | 0x02000000L;    /* PALETTERGB */
    if (lowColor)
        return Solid(c);
    return c;
}

void PalSelect(HDC hdc)
{
    if (hPal) {
        SelectPalette(hdc, hPal, FALSE);
        RealizePalette(hdc);
    }
}

BOOL PalRealize(HWND hwnd)
{
    HDC hdc;
    HPALETTE old;
    UINT n;

    if (!hPal)
        return FALSE;
    hdc = GetDC(hwnd);
    old = SelectPalette(hdc, hPal, FALSE);
    n = RealizePalette(hdc);
    SelectPalette(hdc, old, FALSE);
    ReleaseDC(hwnd, hdc);
    return n > 0;
}

const char *ThemeName(int idx)
{
    return themes[idx].name;
}

void ThemeSet(int idx)
{
    if (idx < 0 || idx >= nThemes)
        idx = 0;
    themeIdx = idx;
    th = themes[idx];
    if (palDevice) {
        BuildPalette();
        return;                 /* exact colours through the palette */
    }
    if (lowColor) {
        /* Text backgrounds must be pure colours on palette devices. */
        th.side = Solid(th.side);
        th.view = Solid(th.view);
        th.sel = Solid(th.sel);
        th.sideSel = Solid(th.sideSel);
        th.sideHot = Solid(th.sideHot);
        th.stripe = Solid(th.stripe);
        th.bar1 = Solid(th.bar1);
        th.bar2 = Solid(th.bar2);
        th.barHot = Solid(th.barHot);
    }
}

COLORREF Solid(COLORREF c)
{
    HDC hdc;

    if (!lowColor)
        return c;
    hdc = GetDC(NULL);
    c = GetNearestColor(hdc, c);
    ReleaseDC(NULL, hdc);
    return c;
}

COLORREF Mix(COLORREF a, COLORREF b, int pct)
{
    return RGB((GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100,
               (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100,
               (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100);
}

/* ---- primitives ------------------------------------------------------- */

void FillR(HDC hdc, const RECT *r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(PC(c));

    FillRect(hdc, r, br);
    DeleteObject(br);
}

void FillXY(HDC hdc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;

    SetRect(&r, x, y, x + w, y + h);
    FillR(hdc, &r, c);
}

void Gradient(HDC hdc, const RECT *r, COLORREF a, COLORREF b, BOOL vertical)
{
    int len = vertical ? r->bottom - r->top : r->right - r->left;
    int steps = min(len, palDevice ? GRAD_STEPS_LOW : 48), i;
    RECT q;

    if (len <= 0)
        return;
    if (steps < 1)
        steps = 1;
    for (i = 0; i < steps; i++) {
        int p0 = (int)((long)len * i / steps), p1 = (int)((long)len * (i + 1) / steps);

        if (vertical)
            SetRect(&q, r->left, r->top + p0, r->right, r->top + p1);
        else
            SetRect(&q, r->left + p0, r->top, r->left + p1, r->bottom);
        FillR(hdc, &q, Mix(a, b, steps > 1 ? i * 100 / (steps - 1) : 0));
    }
}

void HLine(HDC hdc, int x1, int x2, int y, COLORREF c)
{
    FillXY(hdc, x1, y, x2 - x1, 1, c);
}

void VLine(HDC hdc, int x, int y1, int y2, COLORREF c)
{
    FillXY(hdc, x, y1, 1, y2 - y1, c);
}

void FrameR(HDC hdc, const RECT *r, COLORREF c)
{
    HLine(hdc, r->left, r->right, r->top, c);
    HLine(hdc, r->left, r->right, r->bottom - 1, c);
    VLine(hdc, r->left, r->top, r->bottom, c);
    VLine(hdc, r->right - 1, r->top, r->bottom, c);
}

void Bevel(HDC hdc, const RECT *r, COLORREF hi, COLORREF lo)
{
    HLine(hdc, r->left, r->right - 1, r->top, hi);
    VLine(hdc, r->left, r->top, r->bottom - 1, hi);
    HLine(hdc, r->left, r->right, r->bottom - 1, lo);
    VLine(hdc, r->right - 1, r->top, r->bottom, lo);
}

void Pill(HDC hdc, const RECT *r, COLORREF fill, COLORREF border)
{
    HBRUSH br = CreateSolidBrush(PC(fill)), ob;
    HPEN pen = CreatePen(PS_SOLID, 1, PC(border)), op;

    ob = SelectObject(hdc, br);
    op = SelectObject(hdc, pen);
    RoundRect(hdc, r->left, r->top, r->right, r->bottom, 7, 7);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void Triangle(HDC hdc, int x, int y, BOOL down, COLORREF c)
{
    int i;

    /* a small 7x7 arrow drawn with lines: right or down */
    for (i = 0; i < 4; i++) {
        if (down)
            HLine(hdc, x + i, x + 7 - i, y + 1 + i, c);
        else
            VLine(hdc, x + 1 + i, y + i, y + 7 - i, c);
    }
}

void DrawGauge(HDC hdc, const RECT *r, DWORD freeKB, DWORD totalKB, COLORREF back)
{
    RECT q = *r, f;
    int w;
    DWORD used;
    COLORREF c;

    FillR(hdc, &q, back);
    FrameR(hdc, &q, Mix(th.dimText, back, 40));
    if (!totalKB)
        return;
    used = totalKB > freeKB ? totalKB - freeKB : 0;
    InflateRect(&q, -1, -1);
    w = (int)((q.right - q.left) * (used / 16) / max(1UL, totalKB / 16));
    if (w > q.right - q.left)
        w = q.right - q.left;
    c = (used / 16) * 10 >= (totalKB / 16) * 9 ? th.gaugeFull : th.gauge;
    SetRect(&f, q.left, q.top, q.left + w, q.bottom);
    Gradient(hdc, &f, Mix(c, RGB(255, 255, 255), 35), c, TRUE);
}

void DrawGlyph(HDC hdc, int x, int y, int glyph)
{
    HDC dc;
    HBITMAP old;
    COLORREF ot, ob;

    if (!hbmGlyphs || !hbmGlyphsM)
        return;
    dc = CreateCompatibleDC(hdc);
    ot = SetTextColor(hdc, RGB(0, 0, 0));
    ob = SetBkColor(hdc, RGB(255, 255, 255));
    old = SelectObject(dc, hbmGlyphs);
    BitBlt(hdc, x, y, 16, 16, dc, glyph * 16, 0, SRCINVERT);
    SelectObject(dc, hbmGlyphsM);
    BitBlt(hdc, x, y, 16, 16, dc, glyph * 16, 0, SRCAND);
    SelectObject(dc, hbmGlyphs);
    BitBlt(hdc, x, y, 16, 16, dc, glyph * 16, 0, SRCINVERT);
    SelectObject(dc, old);
    DeleteDC(dc);
    SetTextColor(hdc, ot);
    SetBkColor(hdc, ob);
}

void TextAt(HDC hdc, int x, int y, const char *s, COLORREF c)
{
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, PC(c));
    TextOut(hdc, x, y, s, strlen(s));
}

void TextIn(HDC hdc, const RECT *r, const char *s, COLORREF c, UINT fmt)
{
    RECT q = *r;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, PC(c));
#ifdef NEED_ELLIPSIS
    if (fmt & DT_END_ELLIPSIS) {
        int w = r->right - r->left, n = strlen(s);

        fmt &= ~DT_END_ELLIPSIS;
        if (n < 400 && TextW(hdc, s) > w) {
            char buf[404];

            if (!(fmt & DT_SINGLELINE)) {
                /* several lines: clip to the rectangle */
                DrawText(hdc, s, -1, &q, fmt | DT_NOPREFIX);
                return;
            }
            strcpy(buf, s);
            while (n > 0) {
                strcpy(buf + n, "...");
                if (TextW(hdc, buf) <= w)
                    break;
                n--;
            }
            DrawText(hdc, buf, -1, &q, fmt | DT_NOPREFIX);
            return;
        }
    }
#endif
    DrawText(hdc, s, -1, &q, fmt | DT_NOPREFIX);
}

int TextW(HDC hdc, const char *s)
{
    SIZE sz;

    GetTextExtentPoint(hdc, s, strlen(s), &sz);
    return sz.cx;
}

HICON BuiltinIcon(int bi)
{
    if (bi < 0 || bi >= BI_COUNT)
        bi = BI_DOC;
    if (!icons[bi])
        icons[bi] = LoadIcon(hInst, MAKEINTRESOURCE(IDI_BASE + bi));
    return icons[bi];
}

/* ---- buffered painting ------------------------------------------------- */

#define STRIP_H 96

void BufferedPaint(HWND hwnd, PAINTFN paint)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps), mem;
    HBITMAP bmp, old;
    RECT rc = ps.rcPaint, band;
    int w = rc.right - rc.left, y, h;

    if (w <= 0 || rc.bottom <= rc.top) {
        EndPaint(hwnd, &ps);
        return;
    }
    h = min(STRIP_H, rc.bottom - rc.top);
    PalSelect(hdc);
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, w, h);
    if (!mem || !bmp) {
        if (mem)
            DeleteDC(mem);
        paint(hwnd, hdc, &rc);          /* not enough memory: paint directly */
        EndPaint(hwnd, &ps);
        return;
    }
    old = SelectObject(mem, bmp);
    if (hPal)
        SelectPalette(mem, hPal, FALSE);
    for (y = rc.top; y < rc.bottom; y += h) {
        int bh = min(h, rc.bottom - y);
        HRGN rgn;

        SetRect(&band, rc.left, y, rc.right, y + bh);
        SetViewportOrgEx(mem, -rc.left, -y, NULL);
        rgn = CreateRectRgn(0, 0, w, bh);
        SelectClipRgn(mem, rgn);
        DeleteObject(rgn);
        paint(hwnd, mem, &band);
        SetViewportOrgEx(mem, 0, 0, NULL);
        SelectClipRgn(mem, NULL);
        BitBlt(hdc, rc.left, y, w, bh, mem, 0, 0, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}
