/* Frame window: toolbar, tool box, color box, status bar, menus, dialogs. */
#include "picasso.h"

#define TB_H        30      /* toolbar height */
#define TB_BTN_W    24
#define TB_BTN_H    22
#define TX_W        58      /* tool box width */
#define TX_BTN      25
#define CB_H        48      /* color box height */
#define ST_H        22      /* status bar height */
#define NCOLORS     28

#define IT_TB       1000    /* item codes: toolbar button, tool, option, color */
#define IT_TOOL     2000
#define IT_OPT      3000
#define IT_COLOR    4000
#define IT_IND      5000

#define TIP_TIMER   1

enum { E_RAISED, E_PRESSED, E_SUNKEN, E_THIN };

HINSTANCE hInst;
HWND hFrame, hCanvas;
COLORREF fgColor = RGB(0, 0, 0), bgColor = RGB(255, 255, 255);
int curTool = T_PENCIL, prevTool = T_PENCIL;
int optOpaque, eraserIdx = 2, brushIdx = 1, sprayIdx, lineIdx, fillIdx;
BOOL showGrid, dirty;
char fileName[260];
int saveFmt = IMG_BMP, saveBpp;
BOOL batchMode;
LOGFONT textFont;

static HWND hTip;
static BOOL showToolbar = TRUE, showTools = TRUE, showColors = TRUE, showStatus = TRUE;
static RECT rcToolbar, rcTools, rcColors, rcStatus, rcCanvas;
static HBITMAP hbmTb, hbmTbM, hbmTbD, hbmTl, hbmTlM;
static HBRUSH hbrDither;
static HFONT hFontUI;
static COLORREF cFace, cHi, cSh, cDk, cText;
static int palSel;
static COLORREF custColors[16];
static char statusText[160], tipText[48];
static int statX = -1, statY, statW = -1, statH;
static int hotItem, pressItem, tipTicks;
static BOOL pressIn;
static int defW, defH;

static COLORREF palette[NCOLORS] = {
    RGB(0, 0, 0), RGB(128, 128, 128), RGB(128, 0, 0), RGB(128, 128, 0),
    RGB(0, 128, 0), RGB(0, 128, 128), RGB(0, 0, 128), RGB(128, 0, 128),
    RGB(128, 128, 64), RGB(0, 64, 64), RGB(0, 128, 255), RGB(0, 64, 128),
    RGB(64, 0, 255), RGB(128, 64, 0),
    RGB(255, 255, 255), RGB(192, 192, 192), RGB(255, 0, 0), RGB(255, 255, 0),
    RGB(0, 255, 0), RGB(0, 255, 255), RGB(0, 0, 255), RGB(255, 0, 255),
    RGB(255, 255, 128), RGB(0, 255, 128), RGB(128, 255, 255), RGB(128, 128, 255),
    RGB(255, 0, 128), RGB(255, 128, 64)
};

/* cmd 0 = separator */
static const struct { int cmd, glyph; } tbBtns[] = {
    { IDM_NEW, 0 }, { IDM_OPEN, 1 }, { IDM_SAVE, 2 }, { 0, 0 },
    { IDM_PRINT, 3 }, { 0, 0 },
    { IDM_CUT, 4 }, { IDM_COPY, 5 }, { IDM_PASTE, 6 }, { 0, 0 },
    { IDM_UNDO, 7 }, { IDM_REDO, 8 }, { 0, 0 },
    { IDM_ZOOMOUT, 9 }, { IDM_ZOOMIN, 10 }, { IDM_GRID, 11 }, { 0, 0 },
    { IDM_ABOUT, 12 }
};
#define NTB (sizeof(tbBtns) / sizeof(tbBtns[0]))

static const int zoomLevels[5] = { 1, 2, 4, 6, 8 };
static const int zoomOpts[4] = { 1, 2, 6, 8 };

void ErrorBox(const char *text)
{
    if (batchMode)
        return;
    MessageBox(hFrame, text, "Picasso", MB_OK | MB_ICONEXCLAMATION);
}

/* ---- drawing helpers ------------------------------------------------------- */

static void LoadColors(void)
{
    cFace = GetSysColor(COLOR_BTNFACE);
    cHi = GetSysColor(COLOR_BTNHIGHLIGHT);
    cSh = GetSysColor(COLOR_BTNSHADOW);
    cDk = GetSysColor(COLOR_WINDOWFRAME);
    cText = GetSysColor(COLOR_BTNTEXT);
}

static void Fill(HDC hdc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;

    SetRect(&r, x, y, x + w, y + h);
    SetBkColor(hdc, c);
    ExtTextOut(hdc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
}

static void FillR(HDC hdc, const RECT *r, COLORREF c)
{
    Fill(hdc, r->left, r->top, r->right - r->left, r->bottom - r->top, c);
}

static void FillTrue(HDC hdc, const RECT *r, COLORREF c)    /* may dither */
{
    HBRUSH br = CreateSolidBrush(c);

    FillRect(hdc, r, br);
    DeleteObject(br);
}

static void FillPal(HDC hdc, const RECT *r, COLORREF c)     /* as the picture shows it */
{
    FillTrue(hdc, r, PalColor(c));
}

static void FillDither(HDC hdc, const RECT *r)
{
    SetTextColor(hdc, cFace);
    SetBkColor(hdc, cHi);
    FillRect(hdc, r, hbrDither);
}

static void Frame(HDC hdc, const RECT *r, COLORREF tl, COLORREF br)
{
    int w = r->right - r->left, h = r->bottom - r->top;

    Fill(hdc, r->left, r->top, w - 1, 1, tl);
    Fill(hdc, r->left, r->top, 1, h - 1, tl);
    Fill(hdc, r->right - 1, r->top, 1, h, br);
    Fill(hdc, r->left, r->bottom - 1, w, 1, br);
}

static void Edge(HDC hdc, const RECT *r, int style)
{
    RECT q = *r;

    switch (style) {
    case E_RAISED:
        Frame(hdc, &q, cHi, cDk);
        InflateRect(&q, -1, -1);
        Frame(hdc, &q, cFace, cSh);
        break;
    case E_PRESSED:
        Frame(hdc, &q, cDk, cHi);
        InflateRect(&q, -1, -1);
        Frame(hdc, &q, cSh, cFace);
        break;
    case E_SUNKEN:
        Frame(hdc, &q, cSh, cHi);
        InflateRect(&q, -1, -1);
        Frame(hdc, &q, cDk, cFace);
        break;
    default:
        Frame(hdc, &q, cSh, cHi);
        break;
    }
}

/* Draws a 16x16 glyph from a strip; disabled glyphs are embossed. */
static void DrawGlyph(HDC hdc, int x, int y, HBITMAP strip, HBITMAP mask, HBITMAP dark,
                      int idx, BOOL disabled)
{
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP old;

    SetTextColor(hdc, RGB(0, 0, 0));
    SetBkColor(hdc, RGB(255, 255, 255));
    if (disabled && dark) {
        HBRUSH bh = CreateSolidBrush(cHi), bs = CreateSolidBrush(cSh), ob;

        old = SelectObject(dc, dark);
        ob = SelectObject(hdc, bh);
        BitBlt(hdc, x + 1, y + 1, 16, 16, dc, idx * 16, 0, ROP_PSDPxax);
        SelectObject(hdc, bs);
        BitBlt(hdc, x, y, 16, 16, dc, idx * 16, 0, ROP_PSDPxax);
        SelectObject(hdc, ob);
        DeleteObject(bh);
        DeleteObject(bs);
    } else {
        old = SelectObject(dc, strip);
        BitBlt(hdc, x, y, 16, 16, dc, idx * 16, 0, SRCINVERT);
        SelectObject(dc, mask);
        BitBlt(hdc, x, y, 16, 16, dc, idx * 16, 0, SRCAND);
        SelectObject(dc, strip);
        BitBlt(hdc, x, y, 16, 16, dc, idx * 16, 0, SRCINVERT);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
}

static void DrawButton(HDC hdc, const RECT *r, HBITMAP strip, HBITMAP mask, HBITMAP dark,
                       int glyph, BOOL pressed, BOOL checked, BOOL disabled)
{
    RECT q = *r;
    int down = (pressed || checked) ? 1 : 0;

    InflateRect(&q, -2, -2);
    if (checked && !pressed)
        FillDither(hdc, &q);
    else
        FillR(hdc, &q, cFace);
    Edge(hdc, r, down ? E_PRESSED : E_RAISED);
    DrawGlyph(hdc, r->left + (r->right - r->left - 16) / 2 + down,
              r->top + (r->bottom - r->top - 16) / 2 + down, strip, mask, dark, glyph, disabled);
}

/* ---- layout and hit testing ------------------------------------------------ */

static void Layout(void)
{
    RECT cl;
    int top = 0, bottom, left = 0;

    GetClientRect(hFrame, &cl);
    bottom = cl.bottom;
    SetRectEmpty(&rcToolbar);
    SetRectEmpty(&rcTools);
    SetRectEmpty(&rcColors);
    SetRectEmpty(&rcStatus);
    if (showToolbar) {
        SetRect(&rcToolbar, 0, 0, cl.right, TB_H);
        top = TB_H;
    }
    if (showStatus) {
        SetRect(&rcStatus, 0, bottom - ST_H, cl.right, bottom);
        bottom -= ST_H;
    }
    if (showColors) {
        SetRect(&rcColors, 0, bottom - CB_H, cl.right, bottom);
        bottom -= CB_H;
    }
    if (showTools) {
        SetRect(&rcTools, 0, top, TX_W, bottom);
        left = TX_W;
    }
    SetRect(&rcCanvas, left, top, cl.right, bottom);
    if (hCanvas)
        MoveWindow(hCanvas, rcCanvas.left + 2, rcCanvas.top + 2,
                   max(0, rcCanvas.right - rcCanvas.left - 4),
                   max(0, rcCanvas.bottom - rcCanvas.top - 4), TRUE);
}

static int OptCount(int tool)
{
    switch (tool) {
    case T_LASSO: case T_SELECT: case T_TEXT:   return 2;
    case T_ERASER: case T_ZOOM:                 return 4;
    case T_BRUSH:                               return 12;
    case T_SPRAY:                               return 3;
    case T_LINE: case T_CURVE:                  return 5;
    case T_RECT: case T_POLYGON: case T_ELLIPSE: case T_RRECT: return 3;
    }
    return 0;
}

static int OptGet(int tool)
{
    int i;

    switch (tool) {
    case T_LASSO: case T_SELECT: case T_TEXT:   return optOpaque;
    case T_ERASER:                              return eraserIdx;
    case T_BRUSH:                               return brushIdx;
    case T_SPRAY:                               return sprayIdx;
    case T_LINE: case T_CURVE:                  return lineIdx;
    case T_RECT: case T_POLYGON: case T_ELLIPSE: case T_RRECT: return fillIdx;
    case T_ZOOM:
        for (i = 0; i < 4; i++)
            if (zoomOpts[i] == zoom)
                return i;
    }
    return -1;
}

static void OptBox(RECT *r)
{
    SetRect(r, rcTools.left + 8, rcTools.top + 4 + 8 * TX_BTN + 6,
            rcTools.left + 50, rcTools.top + 4 + 8 * TX_BTN + 6 + 66);
}

static BOOL GetItemRect(int it, RECT *r)
{
    int i, x;

    if (it >= IT_IND) {
        SetRect(r, rcColors.left + 6, rcColors.top + 8, rcColors.left + 38, rcColors.top + 40);
    } else if (it >= IT_COLOR) {
        i = it - IT_COLOR;
        x = rcColors.left + 44 + (i % 14) * 16;
        SetRect(r, x, rcColors.top + 8 + (i / 14) * 16, x + 16, rcColors.top + 24 + (i / 14) * 16);
    } else if (it >= IT_OPT) {
        RECT b;
        int n = OptCount(curTool);

        i = it - IT_OPT;
        OptBox(&b);
        InflateRect(&b, -3, -3);
        if (n == 12)
            SetRect(r, b.left + (i % 3) * 12, b.top + (i / 3) * 15,
                    b.left + (i % 3) * 12 + 12, b.top + (i / 3) * 15 + 15);
        else
            SetRect(r, b.left, b.top + i * (60 / n), b.right, b.top + (i + 1) * (60 / n));
    } else if (it >= IT_TOOL) {
        i = it - IT_TOOL;
        x = rcTools.left + 4 + (i % 2) * TX_BTN;
        SetRect(r, x, rcTools.top + 4 + (i / 2) * TX_BTN, x + TX_BTN,
                rcTools.top + 4 + (i / 2) * TX_BTN + TX_BTN);
    } else if (it >= IT_TB) {
        x = 6;
        for (i = 0; i < it - IT_TB; i++)
            x += tbBtns[i].cmd ? TB_BTN_W : 8;
        SetRect(r, x, 4, x + TB_BTN_W, 4 + TB_BTN_H);
    } else {
        return FALSE;
    }
    return TRUE;
}

static int HitTest(POINT pt)
{
    RECT r;
    int i, n;

    if (showToolbar && PtInRect(&rcToolbar, pt)) {
        for (i = 0; i < NTB; i++)
            if (tbBtns[i].cmd && GetItemRect(IT_TB + i, &r) && PtInRect(&r, pt))
                return IT_TB + i;
    } else if (showTools && PtInRect(&rcTools, pt)) {
        for (i = 0; i < T_COUNT; i++)
            if (GetItemRect(IT_TOOL + i, &r) && PtInRect(&r, pt))
                return IT_TOOL + i;
        n = OptCount(curTool);
        for (i = 0; i < n; i++)
            if (GetItemRect(IT_OPT + i, &r) && PtInRect(&r, pt))
                return IT_OPT + i;
    } else if (showColors && PtInRect(&rcColors, pt)) {
        for (i = 0; i < NCOLORS; i++)
            if (GetItemRect(IT_COLOR + i, &r) && PtInRect(&r, pt))
                return IT_COLOR + i;
        if (GetItemRect(IT_IND, &r) && PtInRect(&r, pt))
            return IT_IND;
    }
    return 0;
}

static void InvalidateItem(int it)
{
    RECT r;

    if (GetItemRect(it, &r))
        InvalidateRect(hFrame, &r, FALSE);
}

static int ItemHint(int it)
{
    if (it >= IT_IND)   return IDS_INDHINT;
    if (it >= IT_COLOR) return IDS_COLORHINT;
    if (it >= IT_OPT)   return IDS_OPTHINT;
    if (it >= IT_TOOL)  return IDM_TOOL0 + it - IT_TOOL;
    if (it >= IT_TB)    return tbBtns[it - IT_TB].cmd;
    return 0;
}

/* ---- status bar ------------------------------------------------------------ */

static void StatusPane(int i, RECT *r)
{
    int right = rcStatus.right - 2;

    if (i == 0)
        SetRect(r, 2, rcStatus.top + 3, max(40, right - 244), rcStatus.bottom - 1);
    else
        SetRect(r, right - 240 + (i - 1) * 122, rcStatus.top + 3,
                right - 240 + (i - 1) * 122 + 118, rcStatus.bottom - 1);
}

static void InvalidatePane(int i)
{
    RECT r;

    if (!showStatus)
        return;
    StatusPane(i, &r);
    InvalidateRect(hFrame, &r, FALSE);
}

/* id 0 = the description of the current tool */
static void SetStatus(int id)
{
    char buf[160], *nl;

    buf[0] = '\0';
    if (id >= 0)
        LoadString(hInst, id ? id : IDM_TOOL0 + curTool, buf, sizeof(buf));
    nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    if (strcmp(buf, statusText) != 0) {
        strcpy(statusText, buf);
        InvalidatePane(0);
    }
}

void SetStatusPos(int x, int y)
{
    if (x < 0) x = -1;
    if (x == statX && (x < 0 || y == statY))
        return;
    statX = x;
    statY = y;
    InvalidatePane(1);
}

void SetStatusSize(int w, int h)
{
    if (w < 0) w = -1;
    if (w == statW && (w < 0 || h == statH))
        return;
    statW = w;
    statH = h;
    InvalidatePane(2);
}

static void PaintStatus(HDC hdc)
{
    char buf[40];
    RECT r, q;
    HFONT of = SelectObject(hdc, hFontUI);
    int i;

    SetTextColor(hdc, cText);
    for (i = 0; i < 3; i++) {
        StatusPane(i, &r);
        Edge(hdc, &r, E_THIN);
        q = r;
        InflateRect(&q, -1, -1);
        buf[0] = '\0';
        if (i == 1 && statX >= 0)
            wsprintf(buf, "%d, %d", statX, statY);
        if (i == 2 && statW >= 0)
            wsprintf(buf, "%d x %d", statW, statH);
        SetBkColor(hdc, cFace);
        ExtTextOut(hdc, q.left + (i ? 20 : 3), q.top + 1, ETO_OPAQUE | ETO_CLIPPED, &q,
                   i ? buf : statusText, lstrlen(i ? buf : statusText), NULL);
        if (i == 1) {               /* small cross-hair / size markers */
            Fill(hdc, q.left + 5, q.top + 7, 9, 1, cText);
            Fill(hdc, q.left + 9, q.top + 3, 1, 9, cText);
        } else if (i == 2) {
            SetRect(&q, q.left + 5, q.top + 4, q.left + 15, q.top + 12);
            Frame(hdc, &q, cText, cText);
        }
    }
    SelectObject(hdc, of);
}

/* ---- toolbar, tool box, color box ------------------------------------------ */

static BOOL IsEnabled(int cmd)
{
    switch (cmd) {
    case IDM_UNDO:      return nUndo > 0;
    case IDM_REDO:      return nRedo > 0;
    case IDM_CUT:
    case IDM_COPY:
    case IDM_CLEARSEL:  return sel.active;
    case IDM_PASTE:     return IsClipboardFormatAvailable(CF_BITMAP);
    case IDM_ZOOMIN:    return zoom < 8;
    case IDM_ZOOMOUT:   return zoom > 1;
    }
    return TRUE;
}

static void PaintToolbar(HDC hdc)
{
    RECT r;
    int i;

    Fill(hdc, 0, 0, rcToolbar.right, 1, cSh);
    Fill(hdc, 0, 1, rcToolbar.right, 1, cHi);
    for (i = 0; i < NTB; i++) {
        if (!tbBtns[i].cmd)
            continue;
        GetItemRect(IT_TB + i, &r);
        DrawButton(hdc, &r, hbmTb, hbmTbM, hbmTbD, tbBtns[i].glyph,
                   pressItem == IT_TB + i && pressIn,
                   tbBtns[i].cmd == IDM_GRID && showGrid, !IsEnabled(tbBtns[i].cmd));
    }
}

static void DrawOption(HDC hdc, int tool, int i, const RECT *r, BOOL selected)
{
    COLORREF ink = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : cText;
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    int s, k, n;
    RECT q;
    HPEN pen, op;
    HBRUSH br, ob;
    unsigned long seed = 12345;
    char buf[8];

    FillR(hdc, r, selected ? GetSysColor(COLOR_HIGHLIGHT) : cFace);
    switch (tool) {
    case T_LASSO: case T_SELECT: case T_TEXT:
        Fill(hdc, cx - 12, cy - 10, 13, 13, RGB(255, 0, 0));
        if (i == 0)
            Fill(hdc, cx - 5, cy - 4, 17, 14, RGB(255, 255, 255));
        br = CreateSolidBrush(RGB(0, 0, 255));
        ob = SelectObject(hdc, br);
        op = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - 2, cy - 2, cx + 10, cy + 9);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(br);
        break;

    case T_ERASER:
        s = eraserSize[i];
        Fill(hdc, cx - s / 2, cy - s / 2, s, s, ink);
        break;

    case T_ZOOM:
        wsprintf(buf, "%dx", zoomOpts[i]);
        SetTextColor(hdc, ink);
        SetBkMode(hdc, TRANSPARENT);
        q = *r;
        DrawText(hdc, buf, -1, &q, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetBkMode(hdc, OPAQUE);
        break;

    case T_BRUSH:
        s = brushSize[i / 3][i % 3];
        pen = CreatePen(PS_SOLID, 1, ink);
        br = CreateSolidBrush(ink);
        op = SelectObject(hdc, pen);
        ob = SelectObject(hdc, br);
        switch (i / 3) {
        case 0:
            if (s <= 2)
                Fill(hdc, cx - 1, cy - 1, s, s, ink);
            else
                Ellipse(hdc, cx - s / 2, cy - s / 2, cx - s / 2 + s, cy - s / 2 + s);
            break;
        case 1:
            Fill(hdc, cx - s / 2, cy - s / 2, s, s, ink);
            break;
        case 2:
            MoveTo(hdc, cx - s / 2, cy + s / 2);
            LineTo(hdc, cx + s / 2 + 1, cy - s / 2 - 1);
            break;
        default:
            MoveTo(hdc, cx - s / 2, cy - s / 2);
            LineTo(hdc, cx + s / 2 + 1, cy + s / 2 + 1);
            break;
        }
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pen);
        DeleteObject(br);
        break;

    case T_SPRAY:
        s = 3 + i * 2;
        n = 10 + i * 12;
        for (k = 0; k < n; ) {
            int dx, dy;

            seed = seed * 1103515245UL + 12345;
            dx = (int)((seed >> 16) % (2 * s + 1)) - s;
            seed = seed * 1103515245UL + 12345;
            dy = (int)((seed >> 16) % (2 * s + 1)) - s;
            if (dx * dx + dy * dy > s * s)
                continue;
            SetPixel(hdc, cx + dx, cy + dy, ink);
            k++;
        }
        break;

    case T_LINE: case T_CURVE:
        Fill(hdc, r->left + 4, cy - (i + 1) / 2, r->right - r->left - 8, i + 1, ink);
        break;

    default:
        SetRect(&q, r->left + 5, r->top + 5, r->right - 5, r->bottom - 5);
        if (i > 0)
            FillR(hdc, &q, RGB(128, 128, 128));
        if (i < 2)
            Frame(hdc, &q, ink, ink);
        break;
    }
}

static void PaintOptions(HDC hdc)
{
    RECT b, r;
    int i, n = OptCount(curTool), cur = OptGet(curTool);

    OptBox(&b);
    Edge(hdc, &b, E_SUNKEN);
    InflateRect(&b, -2, -2);
    FillR(hdc, &b, cFace);
    for (i = 0; i < n; i++) {
        GetItemRect(IT_OPT + i, &r);
        DrawOption(hdc, curTool, i, &r, i == cur);
    }
}

static void PaintTools(HDC hdc)
{
    RECT r;
    int i;

    for (i = 0; i < T_COUNT; i++) {
        GetItemRect(IT_TOOL + i, &r);
        DrawButton(hdc, &r, hbmTl, hbmTlM, NULL, i, FALSE, i == curTool, FALSE);
    }
    PaintOptions(hdc);
}

static void PaintIndicator(HDC hdc)
{
    RECT r, q;

    GetItemRect(IT_IND, &r);
    Edge(hdc, &r, E_SUNKEN);
    q = r;
    InflateRect(&q, -2, -2);
    FillDither(hdc, &q);
    SetRect(&q, r.left + 13, r.top + 13, r.left + 28, r.top + 28);
    Frame(hdc, &q, cHi, cSh);
    InflateRect(&q, -1, -1);
    Frame(hdc, &q, cFace, cFace);
    InflateRect(&q, -1, -1);
    FillPal(hdc, &q, bgColor);
    SetRect(&q, r.left + 5, r.top + 5, r.left + 20, r.top + 20);
    Frame(hdc, &q, cHi, cSh);
    InflateRect(&q, -1, -1);
    Frame(hdc, &q, cFace, cFace);
    InflateRect(&q, -1, -1);
    FillPal(hdc, &q, fgColor);
}

static void PaintColors(HDC hdc)
{
    RECT r;
    int i;

    PaintIndicator(hdc);
    for (i = 0; i < NCOLORS; i++) {
        GetItemRect(IT_COLOR + i, &r);
        r.right--;
        r.bottom--;
        Edge(hdc, &r, E_SUNKEN);
        InflateRect(&r, -2, -2);
        FillPal(hdc, &r, palette[i]);
    }
}

static void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT r;

    PalSelect(hdc);
    if (showToolbar && IntersectRect(&r, &ps.rcPaint, &rcToolbar))
        PaintToolbar(hdc);
    if (showTools && IntersectRect(&r, &ps.rcPaint, &rcTools))
        PaintTools(hdc);
    if (showColors && IntersectRect(&r, &ps.rcPaint, &rcColors))
        PaintColors(hdc);
    if (showStatus && IntersectRect(&r, &ps.rcPaint, &rcStatus))
        PaintStatus(hdc);
    Edge(hdc, &rcCanvas, E_SUNKEN);
    PalUnselect(hdc);
    EndPaint(hwnd, &ps);
}

/* ---- state changes --------------------------------------------------------- */

static void UpdateTitle(void)
{
    static char shown[300];
    char buf[300];
    const char *name = fileName[0] ? fileName : "untitled";
    const char *p;

    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            name = p + 1;
    wsprintf(buf, "%s%s - Picasso", (LPSTR)name, (LPSTR)(dirty ? " *" : ""));
    if (strcmp(buf, shown) != 0) {
        strcpy(shown, buf);
        SetWindowText(hFrame, buf);
    }
}

void UpdateUI(void)
{
    RECT r;

    if (!hFrame)
        return;
    if (showToolbar)
        InvalidateRect(hFrame, &rcToolbar, FALSE);
    if (showTools) {
        OptBox(&r);
        InvalidateRect(hFrame, &r, FALSE);
    }
    UpdateTitle();
}

void SetTool(int tool)
{
    OpsFinish();
    SelCommit();
    if (tool != curTool) {
        if (curTool != T_PICKER && curTool != T_ZOOM)
            prevTool = curTool;
        curTool = tool;
        if (showTools)
            InvalidateRect(hFrame, &rcTools, FALSE);
    }
    if (!hotItem)
        SetStatus(0);
    UpdateUI();
}

void SetFgColor(COLORREF c)
{
    fgColor = c;
    InvalidateItem(IT_IND);
    TextRefresh();
}

void SetBgColor(COLORREF c)
{
    bgColor = c;
    InvalidateItem(IT_IND);
    TextRefresh();
    if (sel.floating && optOpaque) {
        SelCompose(NULL);
        InvalidateImg(NULL, 0);
    }
}

static void SetOption(int i)
{
    switch (curTool) {
    case T_LASSO: case T_SELECT: case T_TEXT:
        optOpaque = i;
        TextRefresh();
        if (sel.floating) {
            SelCompose(NULL);
            InvalidateImg(NULL, 0);
        }
        break;
    case T_ERASER:  eraserIdx = i; break;
    case T_BRUSH:   brushIdx = i; break;
    case T_SPRAY:   sprayIdx = i; break;
    case T_LINE: case T_CURVE: lineIdx = i; break;
    case T_ZOOM:    ZoomCenter(zoomOpts[i]); break;
    default:        fillIdx = i; break;
    }
    UpdateUI();
}

static void EditColors(void)
{
    CHOOSECOLOR cc;

    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hFrame;
    cc.rgbResult = palette[palSel];
    cc.lpCustColors = custColors;
    cc.Flags = CC_RGBINIT;
    if (ChooseColor(&cc)) {
        palette[palSel] = cc.rgbResult;
        InvalidateItem(IT_COLOR + palSel);
        SetFgColor(cc.rgbResult);
    }
}

/* ---- tool tips ------------------------------------------------------------- */

static void HideTip(void)
{
    if (IsWindowVisible(hTip))
        ShowWindow(hTip, SW_HIDE);
}

static void ShowTip(void)
{
    char buf[160], *nl;
    POINT pt;
    HDC hdc;
    HFONT of;
    DWORD ext;
    int w, h, sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    buf[0] = '\0';
    LoadString(hInst, ItemHint(hotItem), buf, sizeof(buf));
    nl = strchr(buf, '\n');
    if (!nl)
        return;
    lstrcpyn(tipText, nl + 1, sizeof(tipText));
    hdc = GetDC(hTip);
    of = SelectObject(hdc, hFontUI);
    ext = GetTextExtent(hdc, tipText, lstrlen(tipText));
    SelectObject(hdc, of);
    ReleaseDC(hTip, hdc);
    w = LOWORD(ext) + 8;
    h = HIWORD(ext) + 6;
    GetCursorPos(&pt);
    pt.y += 20;
    if (pt.x + w > sw) pt.x = sw - w;
    if (pt.y + h > sh) pt.y -= 40 + h;
    SetWindowPos(hTip, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hTip, NULL, TRUE);
}

LRESULT CALLBACK __export TipProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HFONT of = SelectObject(hdc, hFontUI);
        RECT r;

        GetClientRect(hwnd, &r);
        FillTrue(hdc, &r, RGB(255, 255, 225));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));
        TextOut(hdc, 3, 2, tipText, lstrlen(tipText));
        SelectObject(hdc, of);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void Hover(int it)
{
    if (it == hotItem)
        return;
    hotItem = it;
    HideTip();
    tipTicks = 0;
    SetStatus(ItemHint(it));
    if (it)
        SetTimer(hFrame, TIP_TIMER, 350, NULL);
    else
        KillTimer(hFrame, TIP_TIMER);
}

static void OnTipTimer(void)
{
    POINT pt, cl;

    GetCursorPos(&pt);
    cl = pt;
    ScreenToClient(hFrame, &cl);
    if (WindowFromPoint(pt) != hFrame || HitTest(cl) != hotItem) {
        if (!pressItem)
            Hover(0);
        return;
    }
    if (++tipTicks == 2 && !pressItem && hotItem < IT_OPT)
        ShowTip();
}

/* ---- dialogs --------------------------------------------------------------- */

/* Gives dialogs the gray Windows 95 look; TRUE if the message was handled
 * (*res is then the dialog procedure's return value). */
static BOOL DlgLook(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam, BOOL *res)
{
    static HBRUSH br;
    static COLORREF brColor;
    RECT r;
    int type;

    if (msg != WM_CTLCOLOR && msg != WM_ERASEBKGND)
        return FALSE;
    if (!br || brColor != cFace) {
        if (br)
            DeleteObject(br);
        br = CreateSolidBrush(cFace);
        brColor = cFace;
    }
    if (msg == WM_ERASEBKGND) {
        GetClientRect(hDlg, &r);
        FillRect((HDC)wParam, &r, br);
        *res = TRUE;
        return TRUE;
    }
    type = HIWORD(lParam);
    if (type == CTLCOLOR_EDIT || type == CTLCOLOR_LISTBOX || type == CTLCOLOR_SCROLLBAR)
        return FALSE;
    SetBkColor((HDC)wParam, cFace);
    SetTextColor((HDC)wParam, cText);
    *res = (BOOL)(UINT)br;
    return TRUE;
}

BOOL CALLBACK __export AboutProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[48];
    BOOL res;

    if (DlgLook(hDlg, msg, wParam, lParam, &res))
        return res;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessage(hDlg, IDC_APPICON, STM_SETICON,
                           (WPARAM)LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP)), 0);
        wsprintf(buf, "%lu KB free", GetFreeSpace(0) / 1024);
        SetDlgItemText(hDlg, IDC_MEMORY, buf);
        wsprintf(buf, "%u%% free", GetFreeSystemResources(GFSR_SYSTEMRESOURCES));
        SetDlgItemText(hDlg, IDC_RESOURCES, buf);
        return TRUE;
    case WM_COMMAND:
        if (wParam == IDOK || wParam == IDCANCEL)
            EndDialog(hDlg, TRUE);
        return TRUE;
    }
    return FALSE;
}

BOOL CALLBACK __export AttrProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    BOOL res, ok1, ok2;
    UINT w, h;

    if (DlgLook(hDlg, msg, wParam, lParam, &res))
        return res;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemInt(hDlg, IDC_WIDTH, imgW, FALSE);
        SetDlgItemInt(hDlg, IDC_HEIGHT, imgH, FALSE);
        SendDlgItemMessage(hDlg, IDC_WIDTH, EM_SETSEL, 0, MAKELPARAM(0, -1));
        return TRUE;
    case WM_COMMAND:
        switch (wParam) {
        case IDC_DEFAULT:
            SetDlgItemInt(hDlg, IDC_WIDTH, defW, FALSE);
            SetDlgItemInt(hDlg, IDC_HEIGHT, defH, FALSE);
            return TRUE;
        case IDOK:
            w = GetDlgItemInt(hDlg, IDC_WIDTH, &ok1, FALSE);
            h = GetDlgItemInt(hDlg, IDC_HEIGHT, &ok2, FALSE);
            if (!ok1 || !ok2 || w < 1 || h < 1 || w > MAXDIM || h > MAXDIM) {
                MessageBox(hDlg, "Please enter a width and height between 1 and 16000 pixels.",
                           "Attributes", MB_OK | MB_ICONEXCLAMATION);
                return TRUE;
            }
            if (((int)w != imgW || (int)h != imgH) && PushUndo())
                ResizeImage((int)w, (int)h);
            EndDialog(hDlg, TRUE);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, FALSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void RunDialog(int id, DLGPROC proc)
{
    FARPROC thunk = MakeProcInstance((FARPROC)proc, hInst);

    DialogBox(hInst, MAKEINTRESOURCE(id), hFrame, (DLGPROC)thunk);
    FreeProcInstance(thunk);
}

/* ---- files ----------------------------------------------------------------- */

static int DefaultBpp(void)
{
    HDC hdc = GetDC(NULL);
    int bits = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);

    ReleaseDC(NULL, hdc);
    return bits <= 1 ? 1 : bits <= 4 ? 4 : bits <= 8 ? 8 : 24;
}

/* Puts the colour box colours into the palette of a 256-colour display
 * (while there is room), so they can be drawn with exactly. */
static void AddColorBox(void)
{
    int i;

    if (!hPal)
        return;
    for (i = 0; i < NCOLORS; i++)
        Near(palette[i]);
    if (hFrame)
        InvalidateRect(hFrame, &rcColors, FALSE);
}

/* A blank, unnamed picture. */
static BOOL NewPicture(void)
{
    PalSet(NULL, 0);
    if (!NewImage(defW, defH))
        return FALSE;
    AddColorBox();
    fileName[0] = '\0';
    dirty = FALSE;
    saveFmt = IMG_BMP;
    saveBpp = DefaultBpp();
    scrollX = scrollY = 0;
    CanvasImageChanged();
    return TRUE;
}

static BOOL HasExtension(const char *path)
{
    const char *p;
    BOOL dot = FALSE;

    for (p = path; *p; p++) {
        if (*p == '.')
            dot = TRUE;
        else if (*p == '\\' || *p == '/' || *p == ':')
            dot = FALSE;
    }
    return dot;
}

static BOOL DoSaveAs(void)
{
    static const int depth[4] = { 1, 4, 8, 24 };
    static const int formats[7] = { IMG_BMP, IMG_BMP, IMG_BMP, IMG_BMP, IMG_PNG, IMG_JPEG, IMG_TIFF };
    static const char *exts[7] = { ".bmp", ".bmp", ".bmp", ".bmp", ".png", ".jpg", ".tif" };
    OPENFILENAME ofn;
    OFSTRUCT of;
    char path[260], msg[320];
    int i, idx, fmt, bpp;

    strcpy(path, fileName);
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hFrame;
    ofn.lpstrFilter = "Monochrome Bitmap (*.bmp)\0*.bmp\0"
                      "16 Color Bitmap (*.bmp)\0*.bmp\0"
                      "256 Color Bitmap (*.bmp)\0*.bmp\0"
                      "24-bit Bitmap (*.bmp)\0*.bmp\0"
                      "PNG (*.png)\0*.png\0"
                      "JPEG (*.jpg)\0*.jpg\0"
                      "TIFF (*.tif)\0*.tif\0";
    ofn.nFilterIndex = 4;
    for (i = 0; i < 7; i++)
        if (formats[i] == saveFmt && (saveFmt != IMG_BMP || depth[i] == saveBpp))
            ofn.nFilterIndex = i + 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path) - 5;
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileName(&ofn))
        return FALSE;
    idx = (ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= 7) ? (int)ofn.nFilterIndex - 1 : 3;

    /* The extension decides the format; without one, the chosen type does. */
    fmt = img_format_from_name(path);
    if (fmt == IMG_NONE) {
        fmt = formats[idx];
        if (!HasExtension(path)) {
            strcat(path, exts[idx]);
            if (OpenFile(path, &of, OF_EXIST) != HFILE_ERROR) {
                wsprintf(msg, "%s already exists.\nDo you want to replace it?", (LPSTR)path);
                if (MessageBox(hFrame, msg, "Save As", MB_YESNO | MB_ICONEXCLAMATION) != IDYES)
                    return FALSE;
            }
        }
    }
    bpp = (fmt == IMG_BMP && formats[idx] == IMG_BMP) ? depth[idx] : saveBpp;
    if (!SaveImageFile(path, fmt, bpp))
        return FALSE;
    saveFmt = fmt;
    saveBpp = bpp;
    strcpy(fileName, path);
    dirty = FALSE;
    return TRUE;
}

static BOOL DoSave(void)
{
    OpsFinish();
    SelCommit();
    if (!fileName[0])
        return DoSaveAs();
    if (!SaveImageFile(fileName, saveFmt, saveBpp))
        return FALSE;
    dirty = FALSE;
    return TRUE;
}

/* Offers to save a modified picture; FALSE means "cancel what you were doing". */
static BOOL QuerySave(void)
{
    char buf[340];
    int r;

    OpsFinish();
    SelCommit();
    if (!dirty)
        return TRUE;
    wsprintf(buf, "Save changes to %s?", (LPSTR)(fileName[0] ? fileName : "untitled"));
    r = MessageBox(hFrame, buf, "Picasso", MB_YESNOCANCEL | MB_ICONEXCLAMATION);
    if (r == IDCANCEL)
        return FALSE;
    return r == IDNO || DoSave();
}

static void OpenPath(const char *path)
{
    BOOL reset;

    if (LoadImageFile(path, &reset)) {
        AddColorBox();
        strcpy(fileName, path);
        dirty = FALSE;
        scrollX = scrollY = 0;
        CanvasImageChanged();
    } else if (reset) {
        NewPicture();           /* the palette no longer suits the old picture */
    }
    UpdateUI();
}

static void DoOpen(void)
{
    OPENFILENAME ofn;
    char path[260];

    if (!QuerySave())
        return;
    path[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hFrame;
    ofn.lpstrFilter = "All Picture Files\0*.bmp;*.dib;*.rle;*.png;*.jpg;*.tif\0"
                      "Bitmap Files (*.bmp;*.dib;*.rle)\0*.bmp;*.dib;*.rle\0"
                      "PNG (*.png)\0*.png\0"
                      "JPEG (*.jpg)\0*.jpg;*.jpe\0"
                      "TIFF (*.tif)\0*.tif\0"
                      "All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileName(&ofn))
        OpenPath(path);
}

static void DoNew(void)
{
    if (QuerySave())
        NewPicture();
}

static void DoFont(void)
{
    CHOOSEFONT cf;
    LOGFONT lf = textFont;

    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hFrame;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
    if (ChooseFont(&cf)) {
        textFont = lf;
        TextRefresh();
    }
}

/* ---- commands -------------------------------------------------------------- */

static void ToggleView(BOOL *flag)
{
    *flag = !*flag;
    Layout();
    InvalidateRect(hFrame, NULL, TRUE);
}

static void StepZoom(int dir)
{
    int i;

    for (i = 0; i < 5; i++)
        if (zoomLevels[i] == zoom)
            break;
    i += dir;
    if (i >= 0 && i < 5)
        ZoomCenter(zoomLevels[i]);
}

static void OnCommand(int id)
{
    RECT r;

    if (!IsEnabled(id))
        return;
    if (id >= IDM_TOOL0 && id < IDM_TOOL0 + T_COUNT) {
        SetTool(id - IDM_TOOL0);
        return;
    }
    /* Undo cancels a pending text / polygon / curve; view commands leave it alone. */
    if (id != IDM_UNDO && !(id >= IDM_VTOOLBAR && id <= IDM_ZOOMOUT)
        && id != IDM_OPAQUE && id != IDM_FONT && id != IDM_ABOUT)
        OpsFinish();

    switch (id) {
    case IDM_NEW:       DoNew(); break;
    case IDM_OPEN:      DoOpen(); break;
    case IDM_SAVE:      DoSave(); break;
    case IDM_SAVEAS:    SelCommit(); DoSaveAs(); break;
    case IDM_PRINT:     SelCommit(); PrintImage(); break;
    case IDM_EXIT:      PostMessage(hFrame, WM_CLOSE, 0, 0); break;

    case IDM_UNDO:      DoUndo(); break;
    case IDM_REDO:      DoRedo(); break;
    case IDM_CUT:       DoCopy(); SelDelete(); break;
    case IDM_COPY:      DoCopy(); break;
    case IDM_PASTE:     DoPaste(); break;
    case IDM_CLEARSEL:  SelDelete(); break;
    case IDM_SELALL:
        SetTool(T_SELECT);
        SetRect(&sel.rc, 0, 0, imgW, imgH);
        sel.active = TRUE;
        InvalidateImg(NULL, 0);
        SetStatusSize(imgW, imgH);
        break;

    case IDM_VTOOLBAR:  ToggleView(&showToolbar); break;
    case IDM_VTOOLBOX:  ToggleView(&showTools); break;
    case IDM_VCOLORBOX: ToggleView(&showColors); break;
    case IDM_VSTATUS:   ToggleView(&showStatus); break;
    case IDM_ZOOM1:     ZoomCenter(1); break;
    case IDM_ZOOM2:     ZoomCenter(2); break;
    case IDM_ZOOM4:     ZoomCenter(4); break;
    case IDM_ZOOM6:     ZoomCenter(6); break;
    case IDM_ZOOM8:     ZoomCenter(8); break;
    case IDM_ZOOMIN:    StepZoom(1); break;
    case IDM_ZOOMOUT:   StepZoom(-1); break;
    case IDM_GRID:
        showGrid = !showGrid;
        if (showGrid && zoom < 4)
            ZoomCenter(4);
        InvalidateImg(NULL, 0);
        break;

    case IDM_FLIPH:     Transform(X_FLIPH); break;
    case IDM_FLIPV:     Transform(X_FLIPV); break;
    case IDM_ROT90:     Transform(X_ROT90); break;
    case IDM_ROT180:    Transform(X_ROT180); break;
    case IDM_ROT270:    Transform(X_ROT270); break;
    case IDM_INVERT:    Transform(X_INVERT); break;
    case IDM_ATTR:      SelCommit(); RunDialog(IDD_ATTR, AttrProc); break;
    case IDM_CLEARIMG:  ClearImage(); break;

    case IDM_EDITCOLORS: EditColors(); break;
    case IDM_SWAPCOLORS:
        {
            COLORREF c = fgColor;

            fgColor = bgColor;
            SetBgColor(c);
        }
        break;
    case IDM_FONT:      DoFont(); break;
    case IDM_OPAQUE:
        optOpaque = !optOpaque;
        TextRefresh();
        if (sel.floating) {
            SelCompose(NULL);
            InvalidateImg(NULL, 0);
        }
        if (showTools) {
            OptBox(&r);
            InvalidateRect(hFrame, &r, FALSE);
        }
        break;

    case IDM_ABOUT:     RunDialog(IDD_ABOUT, AboutProc); break;
    }
    UpdateUI();
}

static void OnInitMenu(HMENU menu)
{
    static const int gray[] = { IDM_UNDO, IDM_REDO, IDM_CUT, IDM_COPY, IDM_PASTE,
                                IDM_CLEARSEL, IDM_ZOOMIN, IDM_ZOOMOUT };
    int i;

    for (i = 0; i < sizeof(gray) / sizeof(gray[0]); i++)
        EnableMenuItem(menu, gray[i], MF_BYCOMMAND | (IsEnabled(gray[i]) ? MF_ENABLED : MF_GRAYED));
#define CHECK(id, on) CheckMenuItem(menu, id, MF_BYCOMMAND | ((on) ? MF_CHECKED : MF_UNCHECKED))
    CHECK(IDM_VTOOLBAR, showToolbar);
    CHECK(IDM_VTOOLBOX, showTools);
    CHECK(IDM_VCOLORBOX, showColors);
    CHECK(IDM_VSTATUS, showStatus);
    CHECK(IDM_GRID, showGrid);
    CHECK(IDM_OPAQUE, optOpaque == 0);
    for (i = 0; i < 5; i++)
        CHECK(IDM_ZOOM1 + i, zoomLevels[i] == zoom);
#undef CHECK
}

/* ---- frame window ---------------------------------------------------------- */

static void OnMouseDown(HWND hwnd, POINT pt, BOOL right, BOOL dbl)
{
    int it = HitTest(pt), i;

    HideTip();
    tipTicks = 100;
    if (pressItem)
        return;
    if (it >= IT_IND) {
        if (!right)
            OnCommand(IDM_SWAPCOLORS);
    } else if (it >= IT_COLOR) {
        i = it - IT_COLOR;
        if (right) {
            SetBgColor(palette[i]);
        } else {
            InvalidateItem(IT_COLOR + palSel);
            palSel = i;
            SetFgColor(palette[i]);
            if (dbl)
                EditColors();
        }
    } else if (it >= IT_OPT) {
        if (!right)
            SetOption(it - IT_OPT);
    } else if (it >= IT_TOOL) {
        if (!right)
            SetTool(it - IT_TOOL);
    } else if (it >= IT_TB && !right && IsEnabled(tbBtns[it - IT_TB].cmd)) {
        pressItem = it;
        pressIn = TRUE;
        SetCapture(hwnd);
        InvalidateItem(it);
    }
}

LRESULT CALLBACK __export FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    POINT pt;
    int it;

    switch (msg) {
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            Layout();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO FAR *)lParam)->ptMinTrackSize.x = 320;
        ((MINMAXINFO FAR *)lParam)->ptMinTrackSize.y = 240;
        return 0;

    case WM_SETFOCUS:
        if (hCanvas)
            SetFocus(hCanvas);
        return 0;

    case WM_ACTIVATE:
        if (wParam != WA_INACTIVE)
            UpdateUI();
        else
            HideTip();
        break;

    case WM_QUERYNEWPALETTE:            /* about to become the active window */
    case WM_PALETTECHANGED:             /* another program changed the colours */
        if (!hPal || (msg == WM_PALETTECHANGED && ((HWND)wParam == hwnd || (HWND)wParam == hCanvas)))
            break;
        {
            HDC hdc = GetDC(hwnd);
            UINT changed;

            SelectPalette(hdc, hPal, FALSE);
            changed = RealizePalette(hdc);
            ReleaseDC(hwnd, hdc);
            if (changed) {
                InvalidateRect(hCanvas, NULL, FALSE);
                InvalidateRect(hwnd, &rcColors, FALSE);
            }
            return changed != 0;
        }

    case WM_SYSCOLORCHANGE:
        LoadColors();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_MOUSEMOVE:
        pt.x = (int)LOWORD(lParam);
        pt.y = (int)HIWORD(lParam);
        it = HitTest(pt);
        if (pressItem) {
            if ((it == pressItem) != pressIn) {
                pressIn = !pressIn;
                InvalidateItem(pressItem);
            }
        } else {
            Hover(it);
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        pt.x = (int)LOWORD(lParam);
        pt.y = (int)HIWORD(lParam);
        OnMouseDown(hwnd, pt, msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK,
                    msg == WM_LBUTTONDBLCLK);
        return 0;

    case WM_LBUTTONUP:
        if (pressItem) {
            it = pressItem;
            pressItem = 0;
            ReleaseCapture();
            InvalidateItem(it);
            if (pressIn)
                OnCommand(tbBtns[it - IT_TB].cmd);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIP_TIMER)
            OnTipTimer();
        return 0;

    case WM_COMMAND:
        OnCommand((int)wParam);
        return 0;

    case WM_INITMENUPOPUP:
        OnInitMenu(GetMenu(hwnd));
        return 0;

    case WM_MENUSELECT:
        if (LOWORD(lParam) == 0xFFFF && HIWORD(lParam) == 0)
            SetStatus(0);
        else if (LOWORD(lParam) & (MF_POPUP | MF_SEPARATOR | MF_SYSMENU))
            SetStatus(-1);
        else
            SetStatus((int)wParam);
        return 0;

    case WM_DROPFILES:
        {
            char path[260];

            path[0] = '\0';
            DragQueryFile((HDROP)wParam, 0, path, sizeof(path));
            DragFinish((HDROP)wParam);
            if (path[0] && QuerySave())
                OpenPath(path);
        }
        return 0;

    case WM_QUERYENDSESSION:
        return QuerySave();

    case WM_CLOSE:
        if (QuerySave())
            DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIP_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static BOOL RegisterClasses(void)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = MAKEINTRESOURCE(IDR_MENU);
    wc.lpszClassName = "PicassoFrame";
    if (!RegisterClass(&wc))
        return FALSE;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_SAVEBITS;
    wc.lpfnWndProc = TipProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "PicassoTip";
    if (!RegisterClass(&wc))
        return FALSE;
    return CanvasRegister();
}

static void LoadArt(void)
{
    static const WORD bits[8] = { 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA };
    HBITMAP pat = CreateBitmap(8, 8, 1, 1, bits);
    HDC hdc = GetDC(NULL);

    hbrDither = CreatePatternBrush(pat);
    DeleteObject(pat);
    hbmTb = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_TOOLBAR));
    hbmTbM = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_TOOLBARM));
    hbmTbD = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_TOOLBARD));
    hbmTl = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_TOOLS));
    hbmTlM = LoadBitmap(hInst, MAKEINTRESOURCE(IDB_TOOLSM));
    hFontUI = CreateFont(-MulDiv(8, GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL,
                         0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS, "MS Sans Serif");
    ReleaseDC(NULL, hdc);

    memset(&textFont, 0, sizeof(textFont));
    textFont.lfHeight = -19;
    textFont.lfWeight = FW_NORMAL;
    textFont.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    strcpy(textFont.lfFaceName, "Arial");
}

static void FreeArt(void)
{
    DeleteObject(hbrDither);
    DeleteObject(hbmTb);
    DeleteObject(hbmTbM);
    DeleteObject(hbmTbD);
    DeleteObject(hbmTl);
    DeleteObject(hbmTlM);
    DeleteObject(hFontUI);
}

/* "/convert input output": converts a picture file without showing a window
 * (the output format follows the extension).  Returns 0 on success. */
static int Batch(LPSTR args)
{
    char in[260], out[260];
    BOOL reset;
    int n, fmt, rc = 0;

    batchMode = TRUE;
    while (*args == ' ')
        args++;
    for (n = 0; *args && *args != ' ' && n < sizeof(in) - 1; n++)
        in[n] = *args++;
    in[n] = '\0';
    while (*args == ' ')
        args++;
    for (n = 0; *args && *args != ' ' && n < sizeof(out) - 1; n++)
        out[n] = *args++;
    out[n] = '\0';
    fmt = img_format_from_name(out);
    if (!in[0] || fmt == IMG_NONE || !NewImage(8, 8))
        rc = 1;
    else if (!LoadImageFile(in, &reset))
        rc = 2;
    else if (!SaveImageFile(out, fmt, saveBpp))
        rc = 3;
    ImageDone();
    return rc;
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    HACCEL accel;
    MSG msg;
    char path[260];
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), n;

    hInst = inst;
    if (!prev && !RegisterClasses())
        return 1;
    LoadColors();
    LoadArt();
    for (n = 0; n < 16; n++)
        custColors[n] = RGB(255, 255, 255);
    defW = sw >= 800 ? 512 : 400;
    defH = sw >= 800 ? 320 : 240;
    if (!ImageInit())
        return 1;
    PalInit();
    while (*cmdline == ' ')
        cmdline++;
    lstrcpyn(path, cmdline, 10);
    if (lstrcmpi(path, "/convert ") == 0)
        return Batch(cmdline + 9);
    if (!NewPicture())
        return 1;

    hFrame = CreateWindow("PicassoFrame", "Picasso", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          sw / 16, sh / 16, sw - sw / 8, sh - sh / 8,
                          NULL, NULL, inst, NULL);
    if (!hFrame)
        return 1;
    hCanvas = CreateWindow("PicassoCanvas", NULL, WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL,
                           0, 0, 0, 0, hFrame, NULL, inst, NULL);
    hTip = CreateWindow("PicassoTip", NULL, WS_POPUP | WS_BORDER, 0, 0, 10, 10,
                        hFrame, NULL, inst, NULL);
    Layout();
    CanvasImageChanged();
    PalRealize();
    SetStatus(0);
    UpdateTitle();
    DragAcceptFiles(hFrame, TRUE);
    ShowWindow(hFrame, show);
    UpdateWindow(hFrame);

    /* a file name on the command line, optionally quoted */
    while (*cmdline == ' ' || *cmdline == '"')
        cmdline++;
    lstrcpyn(path, cmdline, sizeof(path));
    n = lstrlen(path);
    while (n > 0 && (path[n - 1] == ' ' || path[n - 1] == '"'))
        path[--n] = '\0';
    if (path[0])
        OpenPath(path);

    accel = LoadAccelerators(inst, MAKEINTRESOURCE(IDR_ACCEL));
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!TranslateAccelerator(hFrame, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    ImageDone();
    FreeArt();
    return (int)msg.wParam;
}
