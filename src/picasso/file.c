/* Loading, saving (through the imgio codecs) and printing.  Pictures move
 * between the files and the display bitmap one scan line at a time, so no
 * buffer ever exceeds 64 KB. */
#include "picasso.h"

static BYTE bmiBuf[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
#define BMI ((BITMAPINFO *)bmiBuf)
#define BIH ((BITMAPINFOHEADER *)bmiBuf)

/* Describes scan lines exchanged with GDI: 24-bit, or - on palette displays -
 * 8-bit indices into Picasso's logical palette. */
static void SetupDIB(int w, int h, BOOL indices)
{
    WORD *t = (WORD *)BMI->bmiColors;
    int i;

    memset(BIH, 0, sizeof(BITMAPINFOHEADER));
    BIH->biSize = sizeof(BITMAPINFOHEADER);
    BIH->biWidth = w;
    BIH->biHeight = h;
    BIH->biPlanes = 1;
    BIH->biBitCount = indices ? 8 : 24;
    if (indices)
        for (i = 0; i < 256; i++)
            t[i] = i;
}

/* ---- file access for the codecs -------------------------------------------- */

typedef struct {
    HFILE f;
    HDC sdc;                    /* screen DC used for DIB conversions */
    BYTE *row, *full;           /* scan line buffers (padded) */
    /* loading */
    IMGINFO info;
    IMGQUANT *q;
    BOOL histogram;             /* first pass on palette displays: count colours */
    BOOL skipped;               /* ... which is not needed for this file */
    BOOL indices;               /* GDI receives palette indices */
    BOOL quant;                 /* RGB rows are reduced to the palette */
    BOOL palChanged;
    HBITMAP hbm, old;
    HDC dc;
} FILECTX;

static FILECTX ctx;

static unsigned io_read(IMGIO *io, void *buf, unsigned n)
{
    UINT r = _lread(ctx.f, buf, n);

    (void)io;
    return r == (UINT)HFILE_ERROR ? 0 : r;
}

static unsigned io_write(IMGIO *io, const void *buf, unsigned n)
{
    UINT r = _lwrite(ctx.f, buf, n);

    (void)io;
    return r == (UINT)HFILE_ERROR ? 0 : r;
}

static int io_seek(IMGIO *io, long pos)
{
    (void)io;
    return _llseek(ctx.f, pos, 0) != -1L;
}

/* ---- loading --------------------------------------------------------------- */

static int ld_begin(IMGIO *io, const IMGINFO *info)
{
    int n;

    (void)io;
    ctx.info = *info;
    if (ctx.histogram) {
        ctx.skipped = info->indexed;
        return !info->indexed;
    }
    ctx.indices = ctx.quant = FALSE;
    if (PalDevice()) {
        if (info->indexed) {
            PalSet(info->pal, info->ncolors);
        } else {
            n = imgq_build(ctx.q, 236, ctx.info.pal);
            PalSet(ctx.info.pal, n);
            ctx.quant = TRUE;
        }
        ctx.indices = ctx.palChanged = TRUE;
    }
    ctx.sdc = ScreenDC();       /* only now: it must hold the final palette */
    ctx.row = malloc((unsigned)info->width * 3 + 8);
    ctx.full = malloc((unsigned)info->width * 3 + 8);
    ctx.hbm = MakeBitmap(info->width, info->height);
    if (!ctx.row || !ctx.full || !ctx.hbm)
        return 0;
    ctx.dc = MemDC();
    ctx.old = SelectObject(ctx.dc, ctx.hbm);
    FillColor(ctx.dc, 0, 0, info->width, info->height, RGB(255, 255, 255));
    return 1;
}

static int ld_put(IMGIO *io, int y, int x0, int step, int n, const uint8_t *px)
{
    int w = ctx.info.width, h = ctx.info.height, size = ctx.indices ? 1 : 3, i;
    const BYTE *c;
    BYTE *src = ctx.row, *d;

    (void)io;
    if (ctx.histogram) {
        imgq_add(ctx.q, px, n);
        return 1;
    }
    if (n < 1 || y < 0 || y >= h || x0 < 0 || x0 + (long)(n - 1) * step >= w)
        return 1;
    if (ctx.quant) {
        imgq_map(ctx.q, px, n, ctx.row);
    } else if (ctx.info.indexed && !ctx.indices) {      /* palette -> B, G, R */
        for (i = 0, d = ctx.row; i < n; i++, d += 3) {
            c = ctx.info.pal[px[i]];
            d[0] = c[2];
            d[1] = c[1];
            d[2] = c[0];
        }
    } else {
        memcpy(ctx.row, px, (unsigned)n * size);
    }
    if (step != 1) {            /* interlaced: merge into the row drawn so far */
        SelectObject(ctx.dc, ctx.old);
        SetupDIB(w, h, ctx.indices);
        GetDIBits(ctx.sdc, ctx.hbm, h - 1 - y, 1, ctx.full, BMI,
                  ctx.indices ? DIB_PAL_COLORS : DIB_RGB_COLORS);
        SelectObject(ctx.dc, ctx.hbm);
        for (i = 0; i < n; i++)
            memcpy(ctx.full + (unsigned)(x0 + i * step) * size, ctx.row + (unsigned)i * size, size);
        src = ctx.full;
        x0 = 0;
        n = w;
    }
    SetupDIB(n, 1, ctx.indices);
    SetDIBitsToDevice(ctx.dc, x0, y, n, 1, 0, 0, 0, 1, src, BMI,
                      ctx.indices ? DIB_PAL_COLORS : DIB_RGB_COLORS);
    return 1;
}

/* Opens a picture file.  Returns FALSE (after reporting the problem) on
 * failure; *reset is then TRUE if the previous picture had to be given up. */
BOOL LoadImageFile(const char *path, BOOL *reset)
{
    IMGIO io;
    HCURSOR oldCur;
    BOOL ok;

    *reset = FALSE;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = _lopen(path, OF_READ);
    if (ctx.f == HFILE_ERROR) {
        ErrorBox("The file could not be opened.");
        return FALSE;
    }
    oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    memset(&io, 0, sizeof(io));
    io.read = io_read;
    io.seek = io_seek;
    io.begin = ld_begin;
    io.put = ld_put;

    ok = TRUE;
    if (PalDevice()) {
        /* A palette has to be chosen before the pixels can be stored: read
         * true-colour files once just to count their colours. */
        ctx.q = imgq_new();
        ctx.histogram = TRUE;
        ok = ctx.q && (img_load(&io) || ctx.skipped);
        ctx.histogram = FALSE;
        _llseek(ctx.f, 0, 0);
    }
    if (ok)
        ok = img_load(&io);

    if (ctx.dc) {
        SelectObject(ctx.dc, ctx.old);
        FreeDC(ctx.dc);
    }
    if (ctx.sdc)
        ReleaseScreenDC(ctx.sdc);
    free(ctx.row);
    free(ctx.full);
    if (ctx.q)
        imgq_free(ctx.q);
    _lclose(ctx.f);
    SetCursor(oldCur);

    if (!ok) {
        if (ctx.hbm)
            DeleteObject(ctx.hbm);
        *reset = ctx.palChanged;
        ErrorBox(io.error ? io.error : "The file could not be opened.");
        return FALSE;
    }
    SelFree();
    ClearUndo();
    SetImage(ctx.hbm, ctx.info.width, ctx.info.height);
    saveFmt = ctx.info.format;
    saveBpp = ctx.info.bpp;
    return TRUE;
}

/* ---- saving ---------------------------------------------------------------- */

static int sv_get(IMGIO *io, int y, uint8_t *px)
{
    (void)io;
    SetupDIB(imgW, imgH, ctx.indices);
    GetDIBits(ctx.sdc, hbmImg, imgH - 1 - y, 1, ctx.row, BMI,
              ctx.indices ? DIB_PAL_COLORS : DIB_RGB_COLORS);
    memcpy(px, ctx.row, (unsigned)imgW * (ctx.indices ? 1 : 3));
    return 1;
}

/* Saves the picture as format (IMG_xxx); bpp is the colour depth for BMP. */
BOOL SaveImageFile(const char *path, int format, int bpp)
{
    IMGIO io;
    HBITMAP stock;
    HCURSOR oldCur;
    BOOL ok;

    memset(&ctx, 0, sizeof(ctx));
    ctx.row = malloc((unsigned)imgW * 3 + 8);
    if (!ctx.row) {
        ErrorBox("Not enough memory to save the picture.");
        return FALSE;
    }
    ctx.f = _lcreat(path, 0);
    if (ctx.f == HFILE_ERROR) {
        free(ctx.row);
        ErrorBox("The file could not be created.");
        return FALSE;
    }
    oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    memset(&io, 0, sizeof(io));
    io.write = io_write;
    io.seek = io_seek;
    io.get = sv_get;

    ctx.info.width = imgW;
    ctx.info.height = imgH;
    if (PalDevice()) {          /* the picture is exactly its palette's colours */
        ctx.indices = TRUE;
        ctx.info.indexed = 1;
        ctx.info.ncolors = PalEntries(ctx.info.pal);
    }
    if (format == IMG_JPEG)
        bpp = GetProfileInt("Picasso", "JpegQuality", 90);

    /* GetDIBits needs the bitmap not to be selected into a DC. */
    ctx.sdc = ScreenDC();
    stock = CreateBitmap(1, 1, 1, 1, NULL);
    SelectObject(hdcImg, stock);
    ok = img_save(&io, &ctx.info, format, bpp);
    SelectObject(hdcImg, hbmImg);
    DeleteObject(stock);
    ReleaseScreenDC(ctx.sdc);
    if (_lclose(ctx.f) != 0)
        ok = FALSE;
    free(ctx.row);
    SetCursor(oldCur);
    if (!ok) {
        FILE_DELETE(path);
        ErrorBox(io.error ? io.error : "The picture could not be saved (is the disk full?).");
    }
    return ok;
}

/* ---- printing -------------------------------------------------------------- */

void PrintImage(void)
{
    PRINTDLG pd;
    DOCINFO di;
    HDC hdc, prn;
    HBITMAP stock;
    BYTE *buf;
    unsigned rowBytes = (unsigned)(((long)imgW * 24 + 31) / 32 * 4);
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

    band = (int)(60000u / rowBytes);
    if (band < 1) band = 1;
    if (band > imgH) band = imgH;
    buf = malloc(band * rowBytes);
    if (!buf) {
        DeleteDC(prn);
        ErrorBox("Not enough memory to print the picture.");
        return;
    }
    oldCur = SetCursor(LoadCursor(NULL, IDC_WAIT));
    hdc = ScreenDC();

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
    memset(&di, 0, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = "Picasso";
    if (StartDoc(prn, &di) > 0 && StartPage(prn) > 0) {
        for (s = 0; s < imgH; s += n) {     /* s counts scan lines from the bottom */
            n = imgH - s < band ? imgH - s : band;
            SetupDIB(imgW, imgH, FALSE);
            GetDIBits(hdc, hbmImg, s, n, buf, BMI, DIB_RGB_COLORS);
            SetupDIB(imgW, n, FALSE);
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
    ReleaseScreenDC(hdc);
    DeleteDC(prn);
    free(buf);
    SetCursor(oldCur);
}
