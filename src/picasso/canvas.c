/* The canvas window: picture display, zooming, scrolling and the tools. */
#include "picasso.h"

#define MARGIN      4       /* gap between the window edge and the picture */
#define HANDLE      5       /* size of the picture resize handles */
#define MAXPOLY     100
#define MAXLASSO    600
#define SPRAY_TIMER 1

enum { D_NONE, D_DRAW, D_SHAPE, D_SELECT, D_MOVE, D_LASSO, D_RESIZE, D_POLY, D_CURVE };

int zoom = 1, scrollX, scrollY;
const int brushSize[4][3] = { { 7, 4, 2 }, { 8, 5, 2 }, { 8, 5, 3 }, { 8, 5, 3 } };
const int eraserSize[4] = { 4, 6, 8, 10 };
const int sprayRadius[3] = { 4, 7, 11 };

static int drag;
static BOOL dragRight, dragMoved;
static POINT ptStart, ptLast;
static COLORREF cFg, cBg;           /* colours of the operation in progress */
static RECT rcPrev;                 /* picture area touched by the last preview */

static POINT poly[MAXPOLY];
static int polyN;
static BOOL polyActive;

static POINT curve[4];
static int curveStage;              /* 0 idle, 1 / 2: waiting for that control point */

static BOOL textActive, caretOn;
static POINT textPos, textCaret;
static char textBuf[256];
static int textLen, textLineH = 16;

static POINT lasso[MAXLASSO];
static int lassoN;

static int resizeKind, resizeW, resizeH;
static RECT rcResize;
static POINT moveOff;

/* ---- coordinates ----------------------------------------------------------- */

static int ToImg(int c, int scroll)
{
    long v = (long)c - MARGIN;

    if (v < 0)
        v -= zoom - 1;
    return scroll + (int)(v / zoom);
}

static int ToClient(int i, int scroll)
{
    long v = (long)(i - scroll) * zoom + MARGIN;

    if (v < -16000) v = -16000;
    if (v > 16000) v = 16000;
    return (int)v;
}

#define CxToImg(x) ToImg(x, scrollX)
#define CyToImg(y) ToImg(y, scrollY)
#define ImgToCx(x) ToClient(x, scrollX)
#define ImgToCy(y) ToClient(y, scrollY)

void InvalidateImg(const RECT *rc, int inflate)
{
    RECT c;

    if (!hCanvas)
        return;
    if (!rc) {
        InvalidateRect(hCanvas, NULL, FALSE);
        return;
    }
    c.left = ImgToCx(rc->left - inflate) - 1;
    c.top = ImgToCy(rc->top - inflate) - 1;
    c.right = ImgToCx(rc->right + inflate) + 1;
    c.bottom = ImgToCy(rc->bottom + inflate) + 1;
    InvalidateRect(hCanvas, &c, FALSE);
}

static void UpdateCaret(void)
{
    if (caretOn) {
        DestroyCaret();
        caretOn = FALSE;
    }
    if (textActive && GetFocus() == hCanvas) {
        CreateCaret(hCanvas, NULL, zoom > 2 ? 2 : 1, textLineH * zoom);
        SetCaretPos(ImgToCx(textCaret.x), ImgToCy(textCaret.y));
        ShowCaret(hCanvas);
        caretOn = TRUE;
    }
}

static void UpdateScrollBars(void)
{
    static BOOL busy;
    RECT cl;
    int pass, maxX = 0, maxY = 0, pad = (2 * MARGIN + HANDLE + 3) / zoom + 1;

    if (busy || !hCanvas)
        return;
    busy = TRUE;
    for (pass = 0; pass < 3; pass++) {  /* bars appearing change the client area */
        GetClientRect(hCanvas, &cl);
        maxX = imgW + pad - cl.right / zoom;
        maxY = imgH + pad - cl.bottom / zoom;
        if (maxX < 0) maxX = 0;
        if (maxY < 0) maxY = 0;
        if (scrollX > maxX) scrollX = maxX;
        if (scrollY > maxY) scrollY = maxY;
        if (scrollX < 0) scrollX = 0;
        if (scrollY < 0) scrollY = 0;
        SetScrollRange(hCanvas, SB_HORZ, 0, maxX, FALSE);
        SetScrollRange(hCanvas, SB_VERT, 0, maxY, FALSE);
    }
    SetScrollPos(hCanvas, SB_HORZ, scrollX, TRUE);
    SetScrollPos(hCanvas, SB_VERT, scrollY, TRUE);
    busy = FALSE;
}

void CanvasImageChanged(void)
{
    SetRectEmpty(&rcPrev);
    UpdateScrollBars();
    InvalidateImg(NULL, 0);
    UpdateCaret();
}

/* Zooms keeping the picture point (cx, cy) in the middle of the window. */
void SetZoom(int z, int cx, int cy)
{
    RECT cl;

    if (z == zoom)
        return;
    zoom = z;
    GetClientRect(hCanvas, &cl);
    scrollX = cx - cl.right / zoom / 2;
    scrollY = cy - cl.bottom / zoom / 2;
    UpdateScrollBars();
    GetClientRect(hCanvas, &cl);        /* the bars may have changed it */
    scrollX = cx - cl.right / zoom / 2;
    scrollY = cy - cl.bottom / zoom / 2;
    UpdateScrollBars();
    InvalidateImg(NULL, 0);
    UpdateCaret();
    UpdateUI();
}

void ZoomCenter(int z)
{
    RECT cl;

    GetClientRect(hCanvas, &cl);
    SetZoom(z, scrollX + cl.right / zoom / 2, scrollY + cl.bottom / zoom / 2);
}

/* ---- previews -------------------------------------------------------------- */

static void PreviewRestore(void)
{
    if (!IsRectEmpty(&rcPrev))
        BaseBlt(&rcPrev);
}

static void PreviewDone(const RECT *bounds)
{
    RECT u;

    UnionRect(&u, &rcPrev, bounds);
    InvalidateImg(&u, 0);
    rcPrev = *bounds;
}

static void Bounds(RECT *r, const POINT *pts, int n, int inflate)
{
    int i;

    SetRect(r, pts[0].x, pts[0].y, pts[0].x, pts[0].y);
    for (i = 1; i < n; i++) {
        if (pts[i].x < r->left) r->left = pts[i].x;
        if (pts[i].x > r->right) r->right = pts[i].x;
        if (pts[i].y < r->top) r->top = pts[i].y;
        if (pts[i].y > r->bottom) r->bottom = pts[i].y;
    }
    r->right++;
    r->bottom++;
    InflateRect(r, inflate, inflate);
}

static HPEN LinePen(BOOL inside)
{
    int w = lineIdx + 1;

    return CreatePen(inside && w > 1 ? PS_INSIDEFRAME : PS_SOLID, w, cFg);
}

/* Selects the pen and brush for outlined / filled shapes. */
static void ShapeBegin(HPEN *pen, HBRUSH *br, HPEN *op, HBRUSH *ob)
{
    if (fillIdx == 2) {
        *pen = CreatePen(PS_SOLID, 1, cFg);
        *br = CreateSolidBrush(cFg);
    } else {
        *pen = LinePen(TRUE);
        *br = fillIdx == 1 ? CreateSolidBrush(cBg) : NULL;
    }
    *op = SelectObject(hdcImg, *pen);
    *ob = SelectObject(hdcImg, *br ? *br : GetStockObject(NULL_BRUSH));
}

static void ShapeEnd(HPEN pen, HBRUSH br, HPEN op, HBRUSH ob)
{
    SelectObject(hdcImg, op);
    SelectObject(hdcImg, ob);
    DeleteObject(pen);
    if (br)
        DeleteObject(br);
}

static void DrawShape(int tool, POINT a, POINT b)
{
    HPEN pen, op;
    HBRUSH br, ob;
    int t;

    if (tool == T_LINE) {
        pen = LinePen(FALSE);
        op = SelectObject(hdcImg, pen);
        MoveTo(hdcImg, a.x, a.y);
        LineTo(hdcImg, b.x, b.y);
        if (lineIdx == 0)
            SetPixel(hdcImg, b.x, b.y, cFg);
        SelectObject(hdcImg, op);
        DeleteObject(pen);
        return;
    }
    if (a.x > b.x) { t = a.x; a.x = b.x; b.x = t; }
    if (a.y > b.y) { t = a.y; a.y = b.y; b.y = t; }
    ShapeBegin(&pen, &br, &op, &ob);
    if (tool == T_RECT)
        Rectangle(hdcImg, a.x, a.y, b.x + 1, b.y + 1);
    else if (tool == T_ELLIPSE)
        Ellipse(hdcImg, a.x, a.y, b.x + 1, b.y + 1);
    else
        RoundRect(hdcImg, a.x, a.y, b.x + 1, b.y + 1, 17, 17);
    ShapeEnd(pen, br, op, ob);
}

/* Shift: lines snap to 45 degree steps, shapes become square. */
static void Constrain(int tool, POINT a, POINT *b)
{
    int dx = b->x - a.x, dy = b->y - a.y;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, m;

    if (tool == T_LINE) {
        if (ax > 2 * ay) { b->y = a.y; return; }
        if (ay > 2 * ax) { b->x = a.x; return; }
    }
    m = ax < ay ? ax : ay;
    b->x = a.x + (dx < 0 ? -m : m);
    b->y = a.y + (dy < 0 ? -m : m);
}

static void CurvePreview(void)
{
    POINT pts[25];
    RECT b;
    HPEN pen, op;
    int i;

    for (i = 0; i <= 24; i++) {
        long t = i * 32L / 24, u = 32 - t;
        long k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t, k3 = t * t * t;

        pts[i].x = (int)((k0 * curve[0].x + k1 * curve[1].x + k2 * curve[2].x
                          + k3 * curve[3].x + 16384) >> 15);
        pts[i].y = (int)((k0 * curve[0].y + k1 * curve[1].y + k2 * curve[2].y
                          + k3 * curve[3].y + 16384) >> 15);
    }
    pts[24] = curve[3];
    PreviewRestore();
    pen = LinePen(FALSE);
    op = SelectObject(hdcImg, pen);
    Polyline(hdcImg, pts, 25);
    if (lineIdx == 0)
        SetPixel(hdcImg, pts[24].x, pts[24].y, cFg);
    SelectObject(hdcImg, op);
    DeleteObject(pen);
    Bounds(&b, pts, 25, lineIdx + 2);
    PreviewDone(&b);
}

static void PolyPreview(POINT cur)
{
    RECT b;
    HPEN pen, op;

    PreviewRestore();
    pen = LinePen(FALSE);
    op = SelectObject(hdcImg, pen);
    poly[polyN] = cur;
    Polyline(hdcImg, poly, polyN + 1);
    if (lineIdx == 0)
        SetPixel(hdcImg, cur.x, cur.y, cFg);
    SelectObject(hdcImg, op);
    DeleteObject(pen);
    Bounds(&b, poly, polyN + 1, lineIdx + 2);
    PreviewDone(&b);
}

static void PolyFinish(void)
{
    RECT b;
    HPEN pen, op;
    HBRUSH br, ob;

    if (!polyActive)
        return;
    polyActive = FALSE;
    if (polyN < 2) {
        UndoDiscard();
        return;
    }
    PreviewRestore();
    ShapeBegin(&pen, &br, &op, &ob);
    Polygon(hdcImg, poly, polyN);
    ShapeEnd(pen, br, op, ob);
    Bounds(&b, poly, polyN, lineIdx + 2);
    PreviewDone(&b);
    SetRectEmpty(&rcPrev);
}

/* ---- text ------------------------------------------------------------------ */

static void TextRender(void)
{
    HFONT font, of;
    TEXTMETRIC tm;
    RECT b;
    int i, start = 0, line = 0, maxw = 0, w;

    PreviewRestore();
    font = CreateFontIndirect(&textFont);
    of = SelectObject(hdcImg, font);
    GetTextMetrics(hdcImg, &tm);
    textLineH = tm.tmHeight;
    SetTextColor(hdcImg, Near(fgColor));
    SetBkColor(hdcImg, Near(bgColor));
    SetBkMode(hdcImg, optOpaque == 0 ? OPAQUE : TRANSPARENT);
    for (i = 0; i <= textLen; i++) {
        if (i < textLen && textBuf[i] != '\n')
            continue;
        TextOut(hdcImg, textPos.x, textPos.y + line * textLineH, textBuf + start, i - start);
        w = LOWORD(GetTextExtent(hdcImg, textBuf + start, i - start));
        if (w > maxw)
            maxw = w;
        if (i == textLen) {
            textCaret.x = textPos.x + w;
            textCaret.y = textPos.y + line * textLineH;
        }
        start = i + 1;
        line++;
    }
    SetBkMode(hdcImg, OPAQUE);
    SelectObject(hdcImg, of);
    DeleteObject(font);
    SetRect(&b, textPos.x - 1, textPos.y, textPos.x + maxw + tm.tmMaxCharWidth,
            textPos.y + line * textLineH);
    PreviewDone(&b);
    UpdateCaret();
}

void TextRefresh(void)
{
    if (textActive)
        TextRender();
}

static void TextFinish(void)
{
    if (!textActive)
        return;
    textActive = FALSE;
    if (textLen == 0)
        UndoDiscard();
    SetRectEmpty(&rcPrev);
    UpdateCaret();
}

/* ---- operations in progress ------------------------------------------------ */

/* Completes a pending polygon, curve or text entry. */
void OpsFinish(void)
{
    PolyFinish();
    curveStage = 0;
    TextFinish();
    SetRectEmpty(&rcPrev);
}

/* Abandons a pending polygon, curve or text entry; FALSE if there was none. */
BOOL OpsCancel(void)
{
    if (!polyActive && !curveStage && !textActive)
        return FALSE;
    polyActive = FALSE;
    curveStage = 0;
    textActive = FALSE;
    SetRectEmpty(&rcPrev);
    UndoDiscard();
    UpdateCaret();
    UpdateUI();
    return TRUE;
}

/* ---- free-hand drawing ----------------------------------------------------- */

static void Stamp(int x, int y)
{
    int kind, s, h;

    if (curTool == T_ERASER) {
        s = eraserSize[eraserIdx];
        PatBlt(hdcImg, x - s / 2, y - s / 2, s, s, PATCOPY);
        return;
    }
    kind = brushIdx / 3;
    s = brushSize[kind][brushIdx % 3];
    h = s / 2;
    if (kind == 1) {
        PatBlt(hdcImg, x - h, y - h, s, s, PATCOPY);
    } else if (kind == 2) {
        MoveTo(hdcImg, x - h, y + h);
        LineTo(hdcImg, x + h + 1, y - h - 1);
    } else {
        MoveTo(hdcImg, x - h, y - h);
        LineTo(hdcImg, x + h + 1, y + h + 1);
    }
}

static void Stroke(POINT a, POINT b)
{
    HPEN pen, op;
    HBRUSH br, ob;
    RECT r;
    POINT pts[2];
    int size = 1;

    if (curTool == T_PENCIL) {
        pen = CreatePen(PS_SOLID, 1, cFg);
        op = SelectObject(hdcImg, pen);
        MoveTo(hdcImg, a.x, a.y);
        LineTo(hdcImg, b.x, b.y);
        SetPixel(hdcImg, b.x, b.y, cFg);
        SelectObject(hdcImg, op);
        DeleteObject(pen);
    } else if (curTool == T_BRUSH && brushIdx < 3) {    /* round brushes */
        size = brushSize[0][brushIdx];
        pen = CreatePen(PS_SOLID, size, cFg);
        br = CreateSolidBrush(cFg);
        op = SelectObject(hdcImg, pen);
        ob = SelectObject(hdcImg, br);
        if (a.x == b.x && a.y == b.y) {
            SelectObject(hdcImg, GetStockObject(NULL_PEN));
            Ellipse(hdcImg, a.x - size / 2, a.y - size / 2,
                    a.x - size / 2 + size + 1, a.y - size / 2 + size + 1);
        } else {
            MoveTo(hdcImg, a.x, a.y);
            LineTo(hdcImg, b.x, b.y);
        }
        SelectObject(hdcImg, op);
        SelectObject(hdcImg, ob);
        DeleteObject(pen);
        DeleteObject(br);
    } else {                                            /* stamped shapes */
        int dx = b.x - a.x, dy = b.y - a.y;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1, err = ax - ay, e2;
        int x = a.x, y = a.y;

        size = curTool == T_ERASER ? eraserSize[eraserIdx] : 9;
        pen = CreatePen(PS_SOLID, 1, cFg);
        br = CreateSolidBrush(cFg);
        op = SelectObject(hdcImg, pen);
        ob = SelectObject(hdcImg, br);
        for (;;) {
            Stamp(x, y);
            if (x == b.x && y == b.y)
                break;
            e2 = 2 * err;
            if (e2 > -ay) { err -= ay; x += sx; }
            if (e2 < ax) { err += ax; y += sy; }
        }
        SelectObject(hdcImg, op);
        SelectObject(hdcImg, ob);
        DeleteObject(pen);
        DeleteObject(br);
    }
    pts[0] = a;
    pts[1] = b;
    Bounds(&r, pts, 2, size + 1);
    InvalidateImg(&r, 0);
}

static void Spray(void)
{
    int r = sprayRadius[sprayIdx], n = 6 + 5 * sprayIdx, dx, dy;
    RECT rc;

    while (n > 0) {
        dx = rand() % (2 * r + 1) - r;
        dy = rand() % (2 * r + 1) - r;
        if (dx * dx + dy * dy > r * r)
            continue;
        SetPixel(hdcImg, ptLast.x + dx, ptLast.y + dy, cFg);
        n--;
    }
    SetRect(&rc, ptLast.x - r, ptLast.y - r, ptLast.x + r + 1, ptLast.y + r + 1);
    InvalidateImg(&rc, 0);
}

/* ---- picture resize handles ------------------------------------------------ */

static BOOL HandleRect(int kind, RECT *r)
{
    long right = (long)(imgW - scrollX) * zoom + MARGIN;
    long bottom = (long)(imgH - scrollY) * zoom + MARGIN;
    long x = kind == 2 ? (MARGIN - (long)scrollX * zoom + right) / 2 - HANDLE / 2 : right;
    long y = kind == 1 ? (MARGIN - (long)scrollY * zoom + bottom) / 2 - HANDLE / 2 : bottom;

    if (x < -100 || x > 16000 || y < -100 || y > 16000)
        return FALSE;
    SetRect(r, (int)x, (int)y, (int)x + HANDLE, (int)y + HANDLE);
    return TRUE;
}

/* 1 = right edge, 2 = bottom edge, 3 = corner */
static int HandleHit(int cx, int cy)
{
    RECT r;
    POINT pt;
    int k;

    pt.x = cx;
    pt.y = cy;
    for (k = 3; k >= 1; k--)
        if (HandleRect(k, &r)) {
            InflateRect(&r, 2, 2);
            if (PtInRect(&r, pt))
                return k;
        }
    return 0;
}

static void ResizeFrame(HWND hwnd)
{
    HDC hdc;

    if (IsRectEmpty(&rcResize))
        return;
    hdc = GetDC(hwnd);
    DrawFocusRect(hdc, &rcResize);
    ReleaseDC(hwnd, hdc);
}

/* ---- painting -------------------------------------------------------------- */

static void FillIf(HDC hdc, int l, int t, int r, int b, HBRUSH br)
{
    RECT rc;

    if (r > l && b > t) {
        SetRect(&rc, l, t, r, b);
        FillRect(hdc, &rc, br);
    }
}

static void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_BTNSHADOW));
    RECT cl, ir, r;
    int sx0, sx1, sy0, sy1, i, k;

    PalSelect(hdc);
    GetClientRect(hwnd, &cl);
    ir.left = ImgToCx(0);
    ir.top = ImgToCy(0);
    ir.right = ImgToCx(imgW);
    ir.bottom = ImgToCy(imgH);
    FillIf(hdc, cl.left, cl.top, cl.right, ir.top, br);
    FillIf(hdc, cl.left, ir.bottom, cl.right, cl.bottom, br);
    FillIf(hdc, cl.left, ir.top, ir.left, ir.bottom, br);
    FillIf(hdc, ir.right, ir.top, cl.right, ir.bottom, br);
    DeleteObject(br);

    sx0 = CxToImg(ps.rcPaint.left);
    sy0 = CyToImg(ps.rcPaint.top);
    sx1 = CxToImg(ps.rcPaint.right - 1) + 1;
    sy1 = CyToImg(ps.rcPaint.bottom - 1) + 1;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > imgW) sx1 = imgW;
    if (sy1 > imgH) sy1 = imgH;
    if (sx1 > sx0 && sy1 > sy0) {
        int dx = ImgToCx(sx0), dy = ImgToCy(sy0);
        int dw = (sx1 - sx0) * zoom, dh = (sy1 - sy0) * zoom;

        if (zoom == 1) {
            BitBlt(hdc, dx, dy, dw, dh, hdcImg, sx0, sy0, SRCCOPY);
        } else {
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchBlt(hdc, dx, dy, dw, dh, hdcImg, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
        }
        if (showGrid && zoom >= 4) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
            HPEN op = SelectObject(hdc, pen);

            for (i = sx0; i < sx1; i++) {
                MoveTo(hdc, dx + (i - sx0) * zoom, dy);
                LineTo(hdc, dx + (i - sx0) * zoom, dy + dh);
            }
            for (i = sy0; i < sy1; i++) {
                MoveTo(hdc, dx, dy + (i - sy0) * zoom);
                LineTo(hdc, dx + dw, dy + (i - sy0) * zoom);
            }
            SelectObject(hdc, op);
            DeleteObject(pen);
        }
    }

    br = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
    for (k = 1; k <= 3; k++)
        if (HandleRect(k, &r))
            FillRect(hdc, &r, br);
    DeleteObject(br);

    if (sel.active) {
        SetRect(&r, ImgToCx(sel.rc.left), ImgToCy(sel.rc.top),
                ImgToCx(sel.rc.right), ImgToCy(sel.rc.bottom));
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        DrawFocusRect(hdc, &r);
    }
    PalUnselect(hdc);
    EndPaint(hwnd, &ps);
}

/* ---- mouse ----------------------------------------------------------------- */

static void ClampToImage(POINT *p)
{
    if (p->x < 0) p->x = 0;
    if (p->y < 0) p->y = 0;
    if (p->x > imgW - 1) p->x = imgW - 1;
    if (p->y > imgH - 1) p->y = imgH - 1;
}

static void SelFromDrag(POINT cur)
{
    RECT nr, u, old = sel.rc;

    ClampToImage(&cur);
    nr.left = min(ptStart.x, cur.x);
    nr.top = min(ptStart.y, cur.y);
    nr.right = max(ptStart.x, cur.x) + 1;
    nr.bottom = max(ptStart.y, cur.y) + 1;
    if (sel.active)
        UnionRect(&u, &old, &nr);
    else
        u = nr;
    sel.rc = nr;
    sel.active = TRUE;
    InvalidateImg(&u, 1);
    SetStatusSize(nr.right - nr.left, nr.bottom - nr.top);
}

static void LassoAdd(HWND hwnd, POINT p)
{
    HDC hdc;

    ClampToImage(&p);
    if (lassoN >= MAXLASSO || (lassoN > 0 && lasso[lassoN - 1].x == p.x && lasso[lassoN - 1].y == p.y))
        return;
    if (lassoN > 0) {
        hdc = GetDC(hwnd);
        SetROP2(hdc, R2_NOT);
        MoveTo(hdc, ImgToCx(lasso[lassoN - 1].x) + zoom / 2, ImgToCy(lasso[lassoN - 1].y) + zoom / 2);
        LineTo(hdc, ImgToCx(p.x) + zoom / 2, ImgToCy(p.y) + zoom / 2);
        ReleaseDC(hwnd, hdc);
    }
    lasso[lassoN++] = p;
}

static void LassoFinish(void)
{
    RECT b;
    HRGN rgn;
    HDC dc;
    HBITMAP mask, old;
    int i, w, h;

    InvalidateImg(NULL, 0);
    if (lassoN < 3)
        return;
    Bounds(&b, lasso, lassoN, 0);
    w = b.right - b.left;
    h = b.bottom - b.top;
    if (w < 2 || h < 2)
        return;
    for (i = 0; i < lassoN; i++) {
        lasso[i].x -= b.left;
        lasso[i].y -= b.top;
    }
    rgn = CreatePolygonRgn(lasso, lassoN, WINDING);
    mask = CreateBitmap(w, h, 1, 1, NULL);
    if (!rgn || !mask) {
        if (rgn) DeleteObject(rgn);
        if (mask) DeleteObject(mask);
        return;
    }
    dc = MemDC();
    old = SelectObject(dc, mask);
    PatBlt(dc, 0, 0, w, h, WHITENESS);          /* 1 = outside the shape */
    FillRgn(dc, rgn, GetStockObject(BLACK_BRUSH));
    FrameRgn(dc, rgn, GetStockObject(BLACK_BRUSH), 1, 1);
    SelectObject(dc, old);
    FreeDC(dc);
    DeleteObject(rgn);
    sel.rc = b;
    sel.hbmMask = mask;
    sel.active = TRUE;
    SetStatusSize(w, h);
}

static void OnDown(HWND hwnd, int cx, int cy, BOOL right, WPARAM keys)
{
    POINT p;
    BOOL inside;
    int k;

    if (drag)
        return;
    if (GetFocus() != hwnd)
        SetFocus(hwnd);
    p.x = CxToImg(cx);
    p.y = CyToImg(cy);
    inside = p.x >= 0 && p.y >= 0 && p.x < imgW && p.y < imgH;
    dragRight = right;
    dragMoved = FALSE;

    k = right ? 0 : HandleHit(cx, cy);
    if (k) {
        OpsFinish();
        SelCommit();
        drag = D_RESIZE;
        resizeKind = k;
        resizeW = imgW;
        resizeH = imgH;
        SetRectEmpty(&rcResize);
        SetCapture(hwnd);
        return;
    }
    if (!polyActive && !curveStage) {
        cFg = Near(right ? bgColor : fgColor);
        cBg = Near(right ? fgColor : bgColor);
    }

    switch (curTool) {
    case T_SELECT:
    case T_LASSO:
        if (sel.active && PtInRect(&sel.rc, p)) {
            if (right)
                return;
            if (sel.floating && (keys & MK_CONTROL))
                SelStamp();
            else if (!SelLift((keys & MK_CONTROL) != 0))
                return;
            moveOff.x = p.x - sel.rc.left;
            moveOff.y = p.y - sel.rc.top;
            drag = D_MOVE;
        } else {
            SelCommit();
            ClampToImage(&p);
            if (curTool == T_SELECT) {
                ptStart = p;
                drag = D_SELECT;
            } else {
                lassoN = 0;
                LassoAdd(hwnd, p);
                drag = D_LASSO;
            }
        }
        break;

    case T_ERASER:
    case T_PENCIL:
    case T_BRUSH:
    case T_SPRAY:
        if (!PushUndo())
            return;
        if (curTool == T_ERASER)
            cFg = Near(bgColor);
        ptLast = p;
        drag = D_DRAW;
        if (curTool == T_SPRAY) {
            Spray();
            SetTimer(hwnd, SPRAY_TIMER, 30, NULL);
        } else {
            Stroke(p, p);
        }
        break;

    case T_FILL:
        if (inside && !SAMECOLOR(GetPixel(hdcImg, p.x, p.y), cFg) && PushUndo()) {
            HBRUSH br = CreateSolidBrush(cFg), ob = SelectObject(hdcImg, br);

            ExtFloodFill(hdcImg, p.x, p.y, PalColor(GetPixel(hdcImg, p.x, p.y)), FLOODFILLSURFACE);
            SelectObject(hdcImg, ob);
            DeleteObject(br);
            InvalidateImg(NULL, 0);
        }
        break;

    case T_PICKER:
        if (inside) {
            if (right)
                SetBgColor(GetPixel(hdcImg, p.x, p.y));
            else
                SetFgColor(GetPixel(hdcImg, p.x, p.y));
            SetTool(prevTool);
        }
        break;

    case T_ZOOM:
        ClampToImage(&p);
        if (right || zoom > 1)
            SetZoom(1, p.x, p.y);
        else
            SetZoom(4, p.x, p.y);
        break;

    case T_TEXT:
        if (right)
            return;
        TextFinish();
        if (!inside || !PushUndo())
            break;
        textActive = TRUE;
        textPos = p;
        textLen = 0;
        SetRectEmpty(&rcPrev);
        TextRender();
        break;

    case T_LINE:
    case T_RECT:
    case T_ELLIPSE:
    case T_RRECT:
        if (!PushUndo())
            return;
        ptStart = p;
        SetRectEmpty(&rcPrev);
        drag = D_SHAPE;
        break;

    case T_POLYGON:
        if (!polyActive) {
            if (!PushUndo())
                return;
            polyActive = TRUE;
            polyN = 1;
            poly[0] = p;
            SetRectEmpty(&rcPrev);
        }
        drag = D_POLY;
        PolyPreview(p);
        break;

    case T_CURVE:
        if (curveStage == 0) {
            if (!PushUndo())
                return;
            curve[0] = curve[1] = curve[2] = curve[3] = p;
            SetRectEmpty(&rcPrev);
        } else if (curveStage == 1) {
            curve[1] = curve[2] = p;
        } else {
            curve[2] = p;
        }
        drag = D_CURVE;
        CurvePreview();
        break;
    }
    if (drag)
        SetCapture(hwnd);
    UpdateUI();
}

static void OnMove(HWND hwnd, int cx, int cy, WPARAM keys)
{
    POINT p, pts[2];
    RECT b, old;

    p.x = CxToImg(cx);
    p.y = CyToImg(cy);
    if (p.x >= 0 && p.y >= 0 && p.x < imgW && p.y < imgH)
        SetStatusPos(p.x, p.y);
    else if (!drag)
        SetStatusPos(-1, 0);
    if (!drag)
        return;
    dragMoved = TRUE;

    switch (drag) {
    case D_DRAW:
        if (curTool != T_SPRAY)
            Stroke(ptLast, p);
        ptLast = p;
        break;

    case D_SHAPE:
        if (keys & MK_SHIFT)
            Constrain(curTool, ptStart, &p);
        PreviewRestore();
        DrawShape(curTool, ptStart, p);
        pts[0] = ptStart;
        pts[1] = p;
        Bounds(&b, pts, 2, lineIdx + 2);
        PreviewDone(&b);
        SetStatusSize(abs(p.x - ptStart.x) + 1, abs(p.y - ptStart.y) + 1);
        break;

    case D_SELECT:
        SelFromDrag(p);
        break;

    case D_MOVE:
        old = sel.rc;
        OffsetRect(&sel.rc, p.x - moveOff.x - sel.rc.left, p.y - moveOff.y - sel.rc.top);
        SelCompose(&old);
        UnionRect(&b, &old, &sel.rc);
        InvalidateImg(&b, 1);
        break;

    case D_LASSO:
        LassoAdd(hwnd, p);
        break;

    case D_POLY:
        PolyPreview(p);
        break;

    case D_CURVE:
        if (curveStage == 0)
            curve[3] = curve[2] = p, curve[1] = curve[0];
        else if (curveStage == 1)
            curve[1] = curve[2] = p;
        else
            curve[2] = p;
        CurvePreview();
        break;

    case D_RESIZE:
        ResizeFrame(hwnd);
        if (resizeKind != 2) resizeW = p.x < 1 ? 1 : p.x > MAXDIM ? MAXDIM : p.x;
        if (resizeKind != 1) resizeH = p.y < 1 ? 1 : p.y > MAXDIM ? MAXDIM : p.y;
        SetRect(&rcResize, ImgToCx(0), ImgToCy(0), ImgToCx(resizeW), ImgToCy(resizeH));
        ResizeFrame(hwnd);
        SetStatusSize(resizeW, resizeH);
        break;
    }
}

static void OnUp(HWND hwnd, int cx, int cy, BOOL right)
{
    POINT p;
    int d = drag;

    if (!drag || right != dragRight)
        return;
    p.x = CxToImg(cx);
    p.y = CyToImg(cy);
    drag = D_NONE;
    ReleaseCapture();

    switch (d) {
    case D_DRAW:
        KillTimer(hwnd, SPRAY_TIMER);
        break;

    case D_SHAPE:
        if (!dragMoved)
            UndoDiscard();
        SetRectEmpty(&rcPrev);
        SetStatusSize(-1, 0);
        break;

    case D_SELECT:
        if (!dragMoved || (sel.rc.right - sel.rc.left < 2 && sel.rc.bottom - sel.rc.top < 2))
            SelFree();
        break;

    case D_LASSO:
        LassoFinish();
        break;

    case D_POLY:
        if (p.x != poly[polyN - 1].x || p.y != poly[polyN - 1].y)
            poly[polyN++] = p;
        if (polyN >= MAXPOLY - 1
            || (polyN > 3 && abs(p.x - poly[0].x) <= 3 && abs(p.y - poly[0].y) <= 3))
            PolyFinish();
        break;

    case D_CURVE:
        if (curveStage == 0 && curve[0].x == curve[3].x && curve[0].y == curve[3].y) {
            UndoDiscard();
            SetRectEmpty(&rcPrev);
        } else if (++curveStage > 2) {
            curveStage = 0;
            SetRectEmpty(&rcPrev);
        }
        break;

    case D_RESIZE:
        ResizeFrame(hwnd);
        SetRectEmpty(&rcResize);
        SetStatusSize(-1, 0);
        if ((resizeW != imgW || resizeH != imgH) && PushUndo())
            ResizeImage(resizeW, resizeH);
        break;
    }
    UpdateUI();
}

static void OnSetCursor(HWND hwnd)
{
    POINT pt, p;
    LPCSTR id = IDC_ARROW;
    HINSTANCE inst = NULL;
    int k;

    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    p.x = CxToImg(pt.x);
    p.y = CyToImg(pt.y);
    k = HandleHit(pt.x, pt.y);
    if (k) {
        id = k == 1 ? IDC_SIZEWE : k == 2 ? IDC_SIZENS : IDC_SIZENWSE;
    } else if (p.x >= 0 && p.y >= 0 && p.x < imgW && p.y < imgH) {
        static const int custom[T_COUNT] = {
            0, 0, IDC_ERASER, IDC_FILL, IDC_PICKER, IDC_ZOOM, IDC_PENCIL, IDC_BRUSH,
            IDC_SPRAY, 0, 0, 0, 0, 0, 0, 0
        };

        if (custom[curTool]) {
            inst = hInst;
            id = MAKEINTRESOURCE(custom[curTool]);
        } else if (curTool == T_TEXT) {
            id = IDC_IBEAM;
        } else if ((curTool == T_SELECT || curTool == T_LASSO) && sel.active && PtInRect(&sel.rc, p)) {
            id = IDC_SIZE;
        } else {
            id = IDC_CROSS;
        }
    }
    SetCursor(LoadCursor(inst, id));
}

static void OnScroll(HWND hwnd, int bar, WPARAM code, int thumb)
{
    RECT cl;
    int *pos = bar == SB_HORZ ? &scrollX : &scrollY;
    int page, line = 16 / zoom, old = *pos;

    GetClientRect(hwnd, &cl);
    page = (bar == SB_HORZ ? cl.right : cl.bottom) / zoom - line;
    if (line < 1) line = 1;
    if (page < 1) page = 1;
    switch (code) {
    case SB_LINEUP:        *pos -= line; break;
    case SB_LINEDOWN:      *pos += line; break;
    case SB_PAGEUP:        *pos -= page; break;
    case SB_PAGEDOWN:      *pos += page; break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:    *pos = thumb; break;
    case SB_TOP:           *pos = 0; break;
    case SB_BOTTOM:        *pos = MAXDIM; break;
    }
    UpdateScrollBars();
    if (*pos != old) {
        InvalidateImg(NULL, 0);
        UpdateCaret();
    }
}

LRESULT CALLBACK __export CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        UpdateScrollBars();
        return 0;

    case WM_HSCROLL:
        OnScroll(hwnd, SB_HORZ, wParam, LOWORD(lParam));
        return 0;

    case WM_VSCROLL:
        OnScroll(hwnd, SB_VERT, wParam, LOWORD(lParam));
        return 0;

    case WM_LBUTTONDBLCLK:
        if (curTool == T_POLYGON && polyActive && !drag) {
            PolyFinish();
            UpdateUI();
            return 0;
        }
        /* fall through */
    case WM_LBUTTONDOWN:
        OnDown(hwnd, (int)LOWORD(lParam), (int)HIWORD(lParam), FALSE, wParam);
        return 0;

    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        OnDown(hwnd, (int)LOWORD(lParam), (int)HIWORD(lParam), TRUE, wParam);
        return 0;

    case WM_MOUSEMOVE:
        OnMove(hwnd, (int)LOWORD(lParam), (int)HIWORD(lParam), wParam);
        return 0;

    case WM_LBUTTONUP:
        OnUp(hwnd, (int)LOWORD(lParam), (int)HIWORD(lParam), FALSE);
        return 0;

    case WM_RBUTTONUP:
        OnUp(hwnd, (int)LOWORD(lParam), (int)HIWORD(lParam), TRUE);
        return 0;

    case WM_TIMER:
        if (drag == D_DRAW && curTool == T_SPRAY)
            Spray();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && !drag) {
            OnSetCursor(hwnd);
            return TRUE;
        }
        break;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        UpdateCaret();
        if (msg == WM_KILLFOCUS && caretOn) {
            DestroyCaret();
            caretOn = FALSE;
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && !drag) {
            if (!OpsCancel())
                SelCommit();
            UpdateUI();
        }
        return 0;

    case WM_CHAR:
        if (!textActive)
            return 0;
        if (wParam == 8) {
            if (textLen > 0)
                textLen--;
        } else if (wParam == 13) {
            if (textLen < sizeof(textBuf) - 1)
                textBuf[textLen++] = '\n';
        } else if (wParam >= 32 && textLen < sizeof(textBuf) - 1) {
            textBuf[textLen++] = (char)wParam;
        } else {
            return 0;
        }
        TextRender();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL CanvasRegister(void)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = CanvasProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "PicassoCanvas";
    return RegisterClass(&wc);
}
