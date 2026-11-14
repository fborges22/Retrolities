/* Picture bitmap, undo stack, selection, clipboard and transformations. */
#include "picasso.h"

HDC hdcImg;
HBITMAP hbmImg;
int imgW, imgH;
int nUndo, nRedo;
SEL sel;

static HBITMAP hbmStock;
static SNAP undoStk[MAXUNDO], redoStk[MAXUNDO];

BOOL ImageInit(void)
{
    HBITMAP one;

    hdcImg = CreateCompatibleDC(NULL);
    if (!hdcImg)
        return FALSE;
    one = CreateBitmap(1, 1, 1, 1, NULL);
    hbmStock = SelectObject(hdcImg, one);
    SelectObject(hdcImg, hbmStock);
    DeleteObject(one);
    return TRUE;
}

void ImageDone(void)
{
    SelFree();
    ClearUndo();
    if (hdcImg) {
        SelectObject(hdcImg, hbmStock);
        if (hbmImg)
            DeleteObject(hbmImg);
        DeleteDC(hdcImg);
    }
}

HBITMAP MakeBitmap(int w, int h)
{
    HDC hdc = GetDC(NULL);
    HBITMAP hbm = CreateCompatibleBitmap(hdc, w, h);

    ReleaseDC(NULL, hdc);
    return hbm;
}

HBITMAP GrabBitmap(HDC src, int x, int y, int w, int h)
{
    HBITMAP hbm = MakeBitmap(w, h), old;
    HDC dc;

    if (!hbm)
        return NULL;
    dc = CreateCompatibleDC(hdcImg);
    old = SelectObject(dc, hbm);
    BitBlt(dc, 0, 0, w, h, src, x, y, SRCCOPY);
    SelectObject(dc, old);
    DeleteDC(dc);
    return hbm;
}

/* Drawing always uses solid colours, so flood fills behave predictably. */
COLORREF Near(COLORREF c)
{
    return GetNearestColor(hdcImg, c);
}

void FillColor(HDC hdc, int x, int y, int w, int h, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(GetNearestColor(hdc, c));
    HBRUSH old = SelectObject(hdc, br);

    PatBlt(hdc, x, y, w, h, PATCOPY);
    SelectObject(hdc, old);
    DeleteObject(br);
}

/* Makes hbm the picture, deleting the previous one. */
void SetImage(HBITMAP hbm, int w, int h)
{
    HBITMAP old = hbmImg;

    SelectObject(hdcImg, hbm);
    hbmImg = hbm;
    imgW = w;
    imgH = h;
    if (old)
        DeleteObject(old);
    CanvasImageChanged();
}

BOOL NewImage(int w, int h)
{
    HBITMAP hbm = MakeBitmap(w, h);

    if (!hbm) {
        ErrorBox("Not enough memory for a picture of this size.");
        return FALSE;
    }
    SelFree();
    ClearUndo();
    SetImage(hbm, w, h);
    FillColor(hdcImg, 0, 0, w, h, RGB(255, 255, 255));
    return TRUE;
}

/* Crops or extends the picture (new area gets the background colour). */
BOOL ResizeImage(int w, int h)
{
    HBITMAP hbm = MakeBitmap(w, h), old;
    HDC dc;

    if (!hbm) {
        ErrorBox("Not enough memory for a picture of this size.");
        return FALSE;
    }
    dc = CreateCompatibleDC(hdcImg);
    old = SelectObject(dc, hbm);
    FillColor(dc, 0, 0, w, h, bgColor);
    BitBlt(dc, 0, 0, imgW, imgH, hdcImg, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteDC(dc);
    SetImage(hbm, w, h);
    return TRUE;
}

/* ---- undo ------------------------------------------------------------------ */

static void FreeStack(SNAP *stk, int *n)
{
    while (*n > 0)
        DeleteObject(stk[--*n].hbm);
}

void ClearUndo(void)
{
    FreeStack(undoStk, &nUndo);
    FreeStack(redoStk, &nRedo);
}

static void DropOldest(void)
{
    DeleteObject(undoStk[0].hbm);
    memmove(&undoStk[0], &undoStk[1], (MAXUNDO - 1) * sizeof(SNAP));
    nUndo--;
}

/* Saves the picture before it is modified.  Fails only when out of memory. */
BOOL PushUndo(void)
{
    HBITMAP hbm;

    FreeStack(redoStk, &nRedo);
    if (nUndo == MAXUNDO)
        DropOldest();
    hbm = GrabBitmap(hdcImg, 0, 0, imgW, imgH);
    while (!hbm && nUndo > 0) {
        DropOldest();
        hbm = GrabBitmap(hdcImg, 0, 0, imgW, imgH);
    }
    if (!hbm) {
        MessageBeep(0);
        ErrorBox("Not enough memory to complete this operation.");
        return FALSE;
    }
    undoStk[nUndo].hbm = hbm;
    undoStk[nUndo].w = imgW;
    undoStk[nUndo].h = imgH;
    nUndo++;
    dirty = TRUE;
    return TRUE;
}

static void SwapImage(SNAP *s)
{
    SNAP cur;

    cur.hbm = hbmImg;
    cur.w = imgW;
    cur.h = imgH;
    SelectObject(hdcImg, s->hbm);
    hbmImg = s->hbm;
    imgW = s->w;
    imgH = s->h;
    *s = cur;
    CanvasImageChanged();
}

/* Abandons the operation in progress: back to the last snapshot. */
void UndoDiscard(void)
{
    SNAP s;

    if (nUndo == 0)
        return;
    s = undoStk[--nUndo];
    SwapImage(&s);
    DeleteObject(s.hbm);
}

/* Copies (part of) the last snapshot back; used to redraw previews. */
void BaseBlt(const RECT *rc)
{
    HDC dc;
    HBITMAP old;

    if (nUndo == 0)
        return;
    dc = CreateCompatibleDC(hdcImg);
    old = SelectObject(dc, undoStk[nUndo - 1].hbm);
    if (rc)
        BitBlt(hdcImg, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
               dc, rc->left, rc->top, SRCCOPY);
    else
        BitBlt(hdcImg, 0, 0, imgW, imgH, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteDC(dc);
}

void DoUndo(void)
{
    SNAP s;

    if (OpsCancel())
        return;
    if (sel.floating) {
        SelFree();
        UndoDiscard();
        return;
    }
    SelFree();
    if (nUndo == 0)
        return;
    s = undoStk[--nUndo];
    SwapImage(&s);
    redoStk[nRedo++] = s;
    dirty = TRUE;
}

void DoRedo(void)
{
    SNAP s;

    OpsFinish();
    SelCommit();
    if (nRedo == 0)
        return;
    s = redoStk[--nRedo];
    SwapImage(&s);
    undoStk[nUndo++] = s;
    dirty = TRUE;
}

/* ---- selection ------------------------------------------------------------- */

void SelFree(void)
{
    BOOL was = sel.active;

    if (sel.hbm) DeleteObject(sel.hbm);
    if (sel.hbmMask) DeleteObject(sel.hbmMask);
    if (sel.hbmBase) DeleteObject(sel.hbmBase);
    memset(&sel, 0, sizeof(sel));
    if (was) {
        InvalidateImg(NULL, 0);
        SetStatusSize(-1, 0);
    }
}

void SelCommit(void)
{
    SelFree();          /* a floating selection is already part of the picture */
}

/* Fills the selected shape of dc at (x, y) with the background colour. */
static void FillSelShape(HDC dc, int x, int y)
{
    int w = sel.rc.right - sel.rc.left, h = sel.rc.bottom - sel.rc.top;

    if (sel.hbmMask) {
        HDC dm = CreateCompatibleDC(hdcImg);
        HBITMAP om = SelectObject(dm, sel.hbmMask);
        HBRUSH br = CreateSolidBrush(GetNearestColor(dc, bgColor));
        HBRUSH ob = SelectObject(dc, br);

        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        BitBlt(dc, x, y, w, h, dm, 0, 0, ROP_PSDPxax);
        SelectObject(dc, ob);
        DeleteObject(br);
        SelectObject(dm, om);
        DeleteDC(dm);
    } else {
        FillColor(dc, x, y, w, h, bgColor);
    }
}

/* Draws the selection bitmap, honouring its shape mask and the
 * opaque / transparent option (background colour = transparent). */
static void DrawSel(HDC hdc, int x, int y)
{
    int w = sel.rc.right - sel.rc.left, h = sel.rc.bottom - sel.rc.top;
    HDC ds = CreateCompatibleDC(hdcImg);
    HBITMAP os = SelectObject(ds, sel.hbm);

    if (!sel.hbmMask && optOpaque == 0) {
        BitBlt(hdc, x, y, w, h, ds, 0, 0, SRCCOPY);
    } else {
        HDC dm = CreateCompatibleDC(hdcImg);
        HBITMAP mask = CreateBitmap(w, h, 1, 1, NULL);
        HBITMAP om = SelectObject(dm, mask);

        if (optOpaque != 0) {
            SetBkColor(ds, Near(bgColor));
            BitBlt(dm, 0, 0, w, h, ds, 0, 0, SRCCOPY);  /* background -> 1 */
        } else {
            PatBlt(dm, 0, 0, w, h, BLACKNESS);
        }
        if (sel.hbmMask) {
            HDC d2 = CreateCompatibleDC(hdcImg);
            HBITMAP o2 = SelectObject(d2, sel.hbmMask);

            BitBlt(dm, 0, 0, w, h, d2, 0, 0, SRCPAINT);
            SelectObject(d2, o2);
            DeleteDC(d2);
        }
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, RGB(255, 255, 255));
        BitBlt(hdc, x, y, w, h, ds, 0, 0, SRCINVERT);
        BitBlt(hdc, x, y, w, h, dm, 0, 0, SRCAND);
        BitBlt(hdc, x, y, w, h, ds, 0, 0, SRCINVERT);
        SelectObject(dm, om);
        DeleteObject(mask);
        DeleteDC(dm);
    }
    SelectObject(ds, os);
    DeleteDC(ds);
}

/* Rebuilds the picture from the base and the floating selection. */
void SelCompose(const RECT *old)
{
    HDC dc;
    HBITMAP ob;

    if (!sel.floating)
        return;
    dc = CreateCompatibleDC(hdcImg);
    ob = SelectObject(dc, sel.hbmBase);
    if (old)
        BitBlt(hdcImg, old->left, old->top, old->right - old->left, old->bottom - old->top,
               dc, old->left, old->top, SRCCOPY);
    else
        BitBlt(hdcImg, 0, 0, imgW, imgH, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteDC(dc);
    DrawSel(hdcImg, sel.rc.left, sel.rc.top);
}

/* Detaches the selected area from the picture so it can be moved. */
BOOL SelLift(BOOL copy)
{
    int w = sel.rc.right - sel.rc.left, h = sel.rc.bottom - sel.rc.top;

    if (!sel.active)
        return FALSE;
    if (sel.floating)
        return TRUE;
    if (!PushUndo())
        return FALSE;
    sel.hbm = GrabBitmap(hdcImg, sel.rc.left, sel.rc.top, w, h);
    sel.hbmBase = GrabBitmap(hdcImg, 0, 0, imgW, imgH);
    if (!sel.hbm || !sel.hbmBase) {
        SelFree();
        UndoDiscard();
        ErrorBox("Not enough memory to complete this operation.");
        return FALSE;
    }
    if (!copy) {
        HDC dc = CreateCompatibleDC(hdcImg);
        HBITMAP ob = SelectObject(dc, sel.hbmBase);

        FillSelShape(dc, sel.rc.left, sel.rc.top);
        SelectObject(dc, ob);
        DeleteDC(dc);
    }
    sel.floating = TRUE;
    SelCompose(NULL);
    return TRUE;
}

/* Leaves a copy of the floating selection where it is (Ctrl+drag). */
void SelStamp(void)
{
    HBITMAP hbm;

    if (!sel.floating)
        return;
    hbm = GrabBitmap(hdcImg, 0, 0, imgW, imgH);
    if (hbm) {
        DeleteObject(sel.hbmBase);
        sel.hbmBase = hbm;
    }
}

void SelDelete(void)
{
    if (!sel.active)
        return;
    if (sel.floating) {
        HDC dc = CreateCompatibleDC(hdcImg);
        HBITMAP ob = SelectObject(dc, sel.hbmBase);

        BitBlt(hdcImg, 0, 0, imgW, imgH, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteDC(dc);
    } else {
        if (!PushUndo())
            return;
        FillSelShape(hdcImg, sel.rc.left, sel.rc.top);
    }
    SelFree();
    InvalidateImg(NULL, 0);
}

/* ---- clipboard ------------------------------------------------------------- */

void DoCopy(void)
{
    int w, h;
    HBITMAP hbm;

    if (!sel.active)
        return;
    w = sel.rc.right - sel.rc.left;
    h = sel.rc.bottom - sel.rc.top;
    if (sel.floating) {
        HDC ds = CreateCompatibleDC(hdcImg);
        HBITMAP os = SelectObject(ds, sel.hbm);

        hbm = GrabBitmap(ds, 0, 0, w, h);
        SelectObject(ds, os);
        DeleteDC(ds);
    } else {
        hbm = GrabBitmap(hdcImg, sel.rc.left, sel.rc.top, w, h);
    }
    if (!hbm) {
        ErrorBox("Not enough memory to copy the selection.");
        return;
    }
    if (sel.hbmMask) {          /* outside the free-form shape: background */
        HDC dc = CreateCompatibleDC(hdcImg), dm = CreateCompatibleDC(hdcImg);
        HBITMAP oc = SelectObject(dc, hbm), om = SelectObject(dm, sel.hbmMask);
        HBRUSH br = CreateSolidBrush(Near(bgColor)), ob = SelectObject(dc, br);

        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        BitBlt(dc, 0, 0, w, h, dm, 0, 0, ROP_DSPDxax);
        SelectObject(dc, ob);
        DeleteObject(br);
        SelectObject(dc, oc);
        SelectObject(dm, om);
        DeleteDC(dc);
        DeleteDC(dm);
    }
    if (OpenClipboard(hFrame)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hbm);
        CloseClipboard();
    } else {
        DeleteObject(hbm);
    }
}

void DoPaste(void)
{
    HBITMAP clip, mine = NULL;
    BITMAP bm;
    int w = 0, h = 0;

    if (!OpenClipboard(hFrame))
        return;
    clip = GetClipboardData(CF_BITMAP);
    if (clip && GetObject(clip, sizeof(bm), &bm)) {
        HDC dc = CreateCompatibleDC(hdcImg);
        HBITMAP old = SelectObject(dc, clip);

        w = bm.bmWidth > MAXDIM ? MAXDIM : bm.bmWidth;
        h = bm.bmHeight > MAXDIM ? MAXDIM : bm.bmHeight;
        mine = GrabBitmap(dc, 0, 0, w, h);
        SelectObject(dc, old);
        DeleteDC(dc);
    }
    CloseClipboard();
    if (!mine)
        return;

    SetTool(T_SELECT);          /* commits whatever was in progress */
    if (!PushUndo()) {
        DeleteObject(mine);
        return;
    }
    if (w > imgW || h > imgH)
        ResizeImage(w > imgW ? w : imgW, h > imgH ? h : imgH);
    sel.hbm = mine;
    sel.hbmBase = GrabBitmap(hdcImg, 0, 0, imgW, imgH);
    if (!sel.hbmBase) {
        SelFree();
        UndoDiscard();
        return;
    }
    sel.rc.left = scrollX + w > imgW ? imgW - w : scrollX;
    sel.rc.top = scrollY + h > imgH ? imgH - h : scrollY;
    sel.rc.right = sel.rc.left + w;
    sel.rc.bottom = sel.rc.top + h;
    sel.active = sel.floating = TRUE;
    SelCompose(NULL);
    InvalidateImg(NULL, 0);
}

/* ---- transformations ------------------------------------------------------- */

/* Inverts with a white brush rather than DSTINVERT, which would also flip
 * the unused byte of 32-bit pixels and confuse later flood fills. */
static void InvertBits(HDC dc, int w, int h)
{
    HBRUSH ob = SelectObject(dc, GetStockObject(WHITE_BRUSH));

    PatBlt(dc, 0, 0, w, h, PATINVERT);
    SelectObject(dc, ob);
}

static HBITMAP BlankLike(int w, int h, BOOL mono)
{
    return mono ? CreateBitmap(w, h, 1, 1, NULL) : MakeBitmap(w, h);
}

static HBITMAP FlipBitmap(HBITMAP src, int w, int h, BOOL horiz, BOOL mono)
{
    HBITMAP dst = BlankLike(w, h, mono), os, od;
    HDC ds, dd;
    int i;

    if (!dst)
        return NULL;
    ds = CreateCompatibleDC(hdcImg);
    dd = CreateCompatibleDC(hdcImg);
    os = SelectObject(ds, src);
    od = SelectObject(dd, dst);
    if (horiz)
        for (i = 0; i < w; i++)
            BitBlt(dd, w - 1 - i, 0, 1, h, ds, i, 0, SRCCOPY);
    else
        for (i = 0; i < h; i++)
            BitBlt(dd, 0, h - 1 - i, w, 1, ds, 0, i, SRCCOPY);
    SelectObject(ds, os);
    SelectObject(dd, od);
    DeleteDC(ds);
    DeleteDC(dd);
    return dst;
}

/* Rotates by a quarter turn: every source row is fetched as a DIB scan line
 * and written back as a one pixel wide column. */
static HBITMAP RotateBitmap(HBITMAP src, int w, int h, BOOL clockwise, BOOL mono)
{
    HBITMAP dst = BlankLike(h, w, mono), od;
    BITMAPINFOHEADER bi;
    BYTE *row, *col;
    HDC hdc, dd;
    int x, y, i;
    unsigned rowBytes = (unsigned)(((long)w * 24 + 31) / 32 * 4);

    row = malloc(rowBytes);
    col = malloc((unsigned)w * 4);
    if (!dst || !row || !col) {
        if (dst) DeleteObject(dst);
        free(row);
        free(col);
        return NULL;
    }
    hdc = GetDC(NULL);
    dd = CreateCompatibleDC(hdcImg);
    od = SelectObject(dd, dst);
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    for (y = 0; y < h; y++) {
        bi.biWidth = w;
        bi.biHeight = h;
        GetDIBits(hdc, src, h - 1 - y, 1, row, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
        for (i = 0; i < w; i++) {       /* scan line i = destination row w-1-i */
            x = clockwise ? w - 1 - i : i;
            col[i * 4] = row[x * 3];
            col[i * 4 + 1] = row[x * 3 + 1];
            col[i * 4 + 2] = row[x * 3 + 2];
        }
        bi.biWidth = 1;
        bi.biHeight = w;
        bi.biSizeImage = 0;
        SetDIBitsToDevice(dd, clockwise ? h - 1 - y : y, 0, 1, w, 0, 0, 0, w,
                          col, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    }
    SelectObject(dd, od);
    DeleteDC(dd);
    ReleaseDC(NULL, hdc);
    free(row);
    free(col);
    return dst;
}

/* Returns a transformed copy of a bitmap that is not selected into any DC. */
static HBITMAP TransformBitmap(HBITMAP src, int w, int h, int kind, BOOL mono)
{
    HBITMAP tmp, out;

    switch (kind) {
    case X_FLIPH:  return FlipBitmap(src, w, h, TRUE, mono);
    case X_FLIPV:  return FlipBitmap(src, w, h, FALSE, mono);
    case X_ROT90:  return RotateBitmap(src, w, h, TRUE, mono);
    case X_ROT270: return RotateBitmap(src, w, h, FALSE, mono);
    case X_ROT180:
        tmp = FlipBitmap(src, w, h, TRUE, mono);
        if (!tmp)
            return NULL;
        out = FlipBitmap(tmp, w, h, FALSE, mono);
        DeleteObject(tmp);
        return out;
    }
    return NULL;
}

void Transform(int kind)
{
    HCURSOR oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    BOOL turn = (kind == X_ROT90 || kind == X_ROT270);
    BOOL ok = TRUE;

    if (sel.active) {
        int w = sel.rc.right - sel.rc.left, h = sel.rc.bottom - sel.rc.top;

        if (SelLift(FALSE)) {
            if (kind == X_INVERT) {
                HDC dc = CreateCompatibleDC(hdcImg);
                HBITMAP ob = SelectObject(dc, sel.hbm);

                InvertBits(dc, w, h);
                SelectObject(dc, ob);
                DeleteDC(dc);
            } else {
                HBITMAP nb = TransformBitmap(sel.hbm, w, h, kind, FALSE);
                HBITMAP nm = sel.hbmMask ? TransformBitmap(sel.hbmMask, w, h, kind, TRUE) : NULL;

                if (!nb || (sel.hbmMask && !nm)) {
                    if (nb) DeleteObject(nb);
                    if (nm) DeleteObject(nm);
                    ok = FALSE;
                } else {
                    DeleteObject(sel.hbm);
                    sel.hbm = nb;
                    if (nm) {
                        DeleteObject(sel.hbmMask);
                        sel.hbmMask = nm;
                    }
                    if (turn) {
                        sel.rc.right = sel.rc.left + h;
                        sel.rc.bottom = sel.rc.top + w;
                    }
                }
            }
            SelCompose(NULL);
        }
    } else if (PushUndo()) {
        if (kind == X_INVERT) {
            InvertBits(hdcImg, imgW, imgH);
        } else {
            HBITMAP nb;

            SelectObject(hdcImg, hbmStock);
            nb = TransformBitmap(hbmImg, imgW, imgH, kind, FALSE);
            SelectObject(hdcImg, hbmImg);
            if (nb) {
                if (turn)
                    SetImage(nb, imgH, imgW);
                else
                    SetImage(nb, imgW, imgH);
            } else {
                UndoDiscard();
                ok = FALSE;
            }
        }
    }
    InvalidateImg(NULL, 0);
    SetCursor(oldCur);
    if (!ok)
        ErrorBox("Not enough memory to complete this operation.");
}

void ClearImage(void)
{
    if (sel.active) {
        SelDelete();
        return;
    }
    if (!PushUndo())
        return;
    FillColor(hdcImg, 0, 0, imgW, imgH, bgColor);
    InvalidateImg(NULL, 0);
}
