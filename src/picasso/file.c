/* BMP loading / saving and printing.  Bitmaps are converted one scan line
 * (or one small band) at a time, so no buffer ever exceeds 64 KB. */
#include "picasso.h"

static BYTE bmiBuf[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
#define BMI ((BITMAPINFO *)bmiBuf)
#define BIH ((BITMAPINFOHEADER *)bmiBuf)

static unsigned RowBytes(int w, int bpp)
{
    return (unsigned)(((long)w * bpp + 31) / 32 * 4);
}

BOOL LoadBMP(const char *path)
{
    BITMAPFILEHEADER bfh;
    DWORD size;
    HFILE f;
    HBITMAP hbm = NULL, old;
    HDC hdc, dc;
    BYTE *row = NULL;
    const char *err = "This file is not a valid bitmap.";
    BOOL core = FALSE, topDown = FALSE;
    int w, h, bpp, n, i;
    unsigned rowBytes;
    HCURSOR oldCur;

    f = _lopen(path, OF_READ);
    if (f == HFILE_ERROR) {
        ErrorBox("The file could not be opened.");
        return FALSE;
    }
    oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    memset(bmiBuf, 0, sizeof(bmiBuf));
    if (_lread(f, &bfh, sizeof(bfh)) != sizeof(bfh) || bfh.bfType != 0x4D42
        || _lread(f, &size, 4) != 4)
        goto fail;
    if (size == 12) {                   /* OS/2 1.x header */
        WORD c[4];

        if (_lread(f, c, 8) != 8)
            goto fail;
        BIH->biWidth = c[0];
        BIH->biHeight = c[1];
        BIH->biBitCount = c[3];
        core = TRUE;
    } else if (size >= 40) {
        if (_lread(f, bmiBuf + 4, 36) != 36)
            goto fail;
        if (size > 40)
            _llseek(f, size - 40, 1);
    } else {
        goto fail;
    }
    if (BIH->biHeight < 0) {
        topDown = TRUE;
        BIH->biHeight = -BIH->biHeight;
    }
    if (BIH->biWidth < 1 || BIH->biHeight < 1)
        goto fail;
    if (BIH->biWidth > MAXDIM || BIH->biHeight > MAXDIM) {
        err = "This picture is too large (the limit is 2000 x 2000 pixels).";
        goto fail;
    }
    bpp = BIH->biBitCount;
    if (BIH->biCompression != BI_RGB || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24)) {
        err = "This bitmap format (compressed or high color) is not supported.";
        goto fail;
    }
    w = (int)BIH->biWidth;
    h = (int)BIH->biHeight;
    if (bpp <= 8) {
        n = BIH->biClrUsed ? (int)BIH->biClrUsed : 1 << bpp;
        if (n < 1 || n > 256)
            goto fail;
        if (core) {
            for (i = 0; i < n; i++)
                if (_lread(f, &BMI->bmiColors[i], 3) != 3)
                    goto fail;
        } else if (_lread(f, BMI->bmiColors, n * 4) != (UINT)(n * 4)) {
            goto fail;
        }
    }
    BIH->biSize = sizeof(BITMAPINFOHEADER);
    BIH->biPlanes = 1;
    BIH->biSizeImage = 0;
    if (bfh.bfOffBits)
        _llseek(f, bfh.bfOffBits, 0);

    rowBytes = RowBytes(w, bpp);
    row = malloc(rowBytes);
    hbm = MakeBitmap(w, h);
    if (!row || !hbm) {
        err = "Not enough memory to open this picture.";
        goto fail;
    }
    dc = CreateCompatibleDC(hdcImg);
    old = SelectObject(dc, hbm);
    FillColor(dc, 0, 0, w, h, RGB(255, 255, 255));
    SelectObject(dc, old);
    DeleteDC(dc);
    hdc = GetDC(NULL);
    for (i = 0; i < h; i++) {
        if (_lread(f, row, rowBytes) != rowBytes)
            break;                      /* truncated file: keep what we have */
        SetDIBits(hdc, hbm, topDown ? h - 1 - i : i, 1, row, BMI, DIB_RGB_COLORS);
    }
    ReleaseDC(NULL, hdc);
    free(row);
    _lclose(f);

    SelFree();
    ClearUndo();
    SetImage(hbm, w, h);
    saveBpp = bpp;
    SetCursor(oldCur);
    return TRUE;

fail:
    if (hbm) DeleteObject(hbm);
    free(row);
    _lclose(f);
    SetCursor(oldCur);
    ErrorBox(err);
    return FALSE;
}

/* Prepares bmiBuf for fetching the picture at the given colour depth and
 * fills in its colour table. */
static void PrepareDIB(HDC hdc, HBITMAP hbm, int bpp, BYTE *row)
{
    memset(bmiBuf, 0, sizeof(bmiBuf));
    BIH->biSize = sizeof(BITMAPINFOHEADER);
    BIH->biWidth = imgW;
    BIH->biHeight = imgH;
    BIH->biPlanes = 1;
    BIH->biBitCount = bpp;
    GetDIBits(hdc, hbm, 0, 1, row, BMI, DIB_RGB_COLORS);
    BIH->biCompression = BI_RGB;
    BIH->biSizeImage = (DWORD)RowBytes(imgW, bpp) * imgH;
    BIH->biClrUsed = 0;
    BIH->biClrImportant = 0;
}

BOOL SaveBMP(const char *path, int bpp)
{
    BITMAPFILEHEADER bfh;
    unsigned rowBytes = RowBytes(imgW, bpp);
    unsigned tableBytes = bpp <= 8 ? (4u << bpp) : 0;
    BYTE *row = malloc(rowBytes);
    HBITMAP stock;
    HFILE f;
    HDC hdc;
    BOOL ok;
    int i;
    HCURSOR oldCur;

    if (!row) {
        ErrorBox("Not enough memory to save the picture.");
        return FALSE;
    }
    f = _lcreat(path, 0);
    if (f == HFILE_ERROR) {
        free(row);
        ErrorBox("The file could not be created.");
        return FALSE;
    }
    oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    hdc = GetDC(NULL);
    /* GetDIBits needs the bitmap not to be selected into a DC. */
    stock = CreateBitmap(1, 1, 1, 1, NULL);
    SelectObject(hdcImg, stock);
    PrepareDIB(hdc, hbmImg, bpp, row);

    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(BITMAPINFOHEADER) + tableBytes;
    bfh.bfSize = bfh.bfOffBits + BIH->biSizeImage;
    bfh.bfReserved1 = bfh.bfReserved2 = 0;
    ok = _lwrite(f, (LPCSTR)&bfh, sizeof(bfh)) == sizeof(bfh)
      && _lwrite(f, (LPCSTR)bmiBuf, sizeof(BITMAPINFOHEADER) + tableBytes)
            == sizeof(BITMAPINFOHEADER) + tableBytes;
    for (i = 0; ok && i < imgH; i++) {
        GetDIBits(hdc, hbmImg, i, 1, row, BMI, DIB_RGB_COLORS);
        ok = _lwrite(f, (LPCSTR)row, rowBytes) == rowBytes;
    }
    SelectObject(hdcImg, hbmImg);
    DeleteObject(stock);
    ReleaseDC(NULL, hdc);
    _lclose(f);
    free(row);
    SetCursor(oldCur);
    if (!ok)
        ErrorBox("The picture could not be saved (is the disk full?).");
    return ok;
}

void PrintImage(void)
{
    PRINTDLG pd;
    DOCINFO di;
    HDC hdc, prn;
    HBITMAP stock;
    BYTE *buf;
    unsigned rowBytes = RowBytes(imgW, 8);
    int band, s, n, pw, ph, dw, dh, dx, y0, y1;
    HCURSOR oldCur;

    memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = hFrame;
    pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIES;
    if (!PrintDlg(&pd))
        return;                 /* cancelled, or no printer (already reported) */
    prn = pd.hDC;
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    if (!prn)
        return;

    band = (int)(30000u / rowBytes);
    if (band < 1) band = 1;
    if (band > imgH) band = imgH;
    buf = malloc(band * rowBytes);
    if (!buf) {
        DeleteDC(prn);
        ErrorBox("Not enough memory to print the picture.");
        return;
    }
    oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    hdc = GetDC(NULL);

    /* Print at screen size, shrunk if needed to fit the page, centred. */
    pw = GetDeviceCaps(prn, HORZRES);
    ph = GetDeviceCaps(prn, VERTRES);
    dw = MulDiv(imgW, GetDeviceCaps(prn, LOGPIXELSX), GetDeviceCaps(hdc, LOGPIXELSX));
    dh = MulDiv(imgH, GetDeviceCaps(prn, LOGPIXELSY), GetDeviceCaps(hdc, LOGPIXELSY));
    if (dw > pw) { dh = MulDiv(dh, pw, dw); dw = pw; }
    if (dh > ph) { dw = MulDiv(dw, ph, dh); dh = ph; }
    dx = (pw - dw) / 2;

    stock = CreateBitmap(1, 1, 1, 1, NULL);
    SelectObject(hdcImg, stock);
    PrepareDIB(hdc, hbmImg, 8, buf);

    memset(&di, 0, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = "Picasso";
    if (StartDoc(prn, &di) > 0 && StartPage(prn) > 0) {
        for (s = 0; s < imgH; s += n) {     /* s counts scan lines from the bottom */
            n = imgH - s < band ? imgH - s : band;
            BIH->biHeight = imgH;
            GetDIBits(hdc, hbmImg, s, n, buf, BMI, DIB_RGB_COLORS);
            BIH->biHeight = n;
            BIH->biSizeImage = 0;
            y0 = MulDiv(imgH - (s + n), dh, imgH);
            y1 = MulDiv(imgH - s, dh, imgH);
            StretchDIBits(prn, dx, y0, dw, y1 - y0, 0, 0, imgW, n,
                          buf, BMI, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPage(prn);
        EndDoc(prn);
    } else {
        ErrorBox("The picture could not be printed.");
    }
    SelectObject(hdcImg, hbmImg);
    DeleteObject(stock);
    ReleaseDC(NULL, hdc);
    DeleteDC(prn);
    free(buf);
    SetCursor(oldCur);
}
