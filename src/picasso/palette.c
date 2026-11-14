/* Palette management for 256-colour displays.
 *
 * On a palette display the picture is a device-dependent bitmap whose pixels
 * refer to the system palette, so Picasso owns one logical palette (hPal):
 * the colours of the picture, followed by the colours used for drawing while
 * there is room.  It is selected into every DC that touches the picture, and
 * all drawing colours are palette-relative.  On other displays hPal is NULL
 * and these functions do nothing special. */
#include "picasso.h"

#define PAL_MAX 236             /* 256 minus the 20 static system colours */

HPALETTE hPal;
static int palCount;
static BOOL palDevice;

BOOL PalDevice(void)
{
    return palDevice;
}

void PalRealize(void)
{
    HDC hdc;

    if (!hPal)
        return;
    if (hFrame) {
        hdc = GetDC(hFrame);
        SelectPalette(hdc, hPal, FALSE);
        RealizePalette(hdc);
        ReleaseDC(hFrame, hdc);
    }
    RealizePalette(hdcImg);
}

/* Replaces the palette with n colours (R, G, B); n = 0 gives black and white. */
void PalSet(const BYTE pal[][3], int n)
{
    static const BYTE bw[2][3] = { { 0, 0, 0 }, { 255, 255, 255 } };
    LOGPALETTE *lp;
    HPALETTE np;
    int i;

    if (!palDevice)
        return;
    if (n < 1 || !pal) {
        pal = bw;
        n = 2;
    }
    if (n > 256)
        n = 256;
    lp = malloc(sizeof(LOGPALETTE) + 255 * sizeof(PALETTEENTRY));
    if (!lp)
        return;
    lp->palVersion = 0x300;
    lp->palNumEntries = n;
    for (i = 0; i < n; i++) {
        lp->palPalEntry[i].peRed = pal[i][0];
        lp->palPalEntry[i].peGreen = pal[i][1];
        lp->palPalEntry[i].peBlue = pal[i][2];
        lp->palPalEntry[i].peFlags = 0;
    }
    np = CreatePalette(lp);
    free(lp);
    if (!np)
        return;
    SelectPalette(hdcImg, np, FALSE);
    if (hPal)
        DeleteObject(hPal);
    hPal = np;
    palCount = n;
    PalRealize();
}

void PalInit(void)
{
    HDC hdc = GetDC(NULL);

    palDevice = (GetDeviceCaps(hdc, RASTERCAPS) & RC_PALETTE) != 0;
    ReleaseDC(NULL, hdc);
    PalSet(NULL, 0);
}

void PalDone(void)
{
    if (hPal) {
        SelectPalette(hdcImg, GetStockObject(DEFAULT_PALETTE), FALSE);
        DeleteObject(hPal);
        hPal = NULL;
    }
}

/* Copies the palette (R, G, B); returns the number of colours. */
int PalEntries(BYTE pal[][3])
{
    PALETTEENTRY pe;
    int i;

    for (i = 0; i < palCount; i++) {
        GetPaletteEntries(hPal, i, 1, &pe);
        pal[i][0] = pe.peRed;
        pal[i][1] = pe.peGreen;
        pal[i][2] = pe.peBlue;
    }
    return palCount;
}

HPALETTE PalCopy(void)
{
    LOGPALETTE *lp;
    HPALETTE copy;

    if (!hPal)
        return NULL;
    lp = malloc(sizeof(LOGPALETTE) + 255 * sizeof(PALETTEENTRY));
    if (!lp)
        return NULL;
    lp->palVersion = 0x300;
    lp->palNumEntries = palCount;
    GetPaletteEntries(hPal, 0, palCount, lp->palPalEntry);
    copy = CreatePalette(lp);
    free(lp);
    return copy;
}

void PalSelect(HDC hdc)
{
    if (hPal) {
        SelectPalette(hdc, hPal, FALSE);
        RealizePalette(hdc);
    }
}

void PalUnselect(HDC hdc)
{
    if (hPal)
        SelectPalette(hdc, GetStockObject(DEFAULT_PALETTE), FALSE);
}

/* The colour as GDI should be given it to match the picture's palette. */
COLORREF PalColor(COLORREF c)
{
    c &= 0x00FFFFFFL;
    return hPal ? c | 0x02000000L : c;
}

/* The solid colour actually used when drawing with c.  On palette displays
 * the colour is added to the palette while there is room for it. */
COLORREF Near(COLORREF c)
{
    PALETTEENTRY pe;
    UINT i;

    c &= 0x00FFFFFFL;
    if (!hPal)
        return GetNearestColor(hdcImg, c);
    i = GetNearestPaletteIndex(hPal, c);
    GetPaletteEntries(hPal, i, 1, &pe);
    if (RGB(pe.peRed, pe.peGreen, pe.peBlue) != c && palCount < PAL_MAX
        && ResizePalette(hPal, palCount + 1)) {
        pe.peRed = GetRValue(c);
        pe.peGreen = GetGValue(c);
        pe.peBlue = GetBValue(c);
        pe.peFlags = 0;
        SetPaletteEntries(hPal, palCount, 1, &pe);
        palCount++;
        PalRealize();
    }
    return PALETTERGB(pe.peRed, pe.peGreen, pe.peBlue);
}

/* A memory DC ready to draw on pieces of the picture. */
HDC MemDC(void)
{
    HDC dc = CreateCompatibleDC(hdcImg);

    if (dc)
        PalSelect(dc);
    return dc;
}

void FreeDC(HDC dc)
{
    if (dc) {
        PalUnselect(dc);
        DeleteDC(dc);
    }
}

/* A screen DC for converting between the picture and DIBs. */
HDC ScreenDC(void)
{
    HDC hdc = GetDC(hFrame);

    PalSelect(hdc);
    return hdc;
}

void ReleaseScreenDC(HDC hdc)
{
    PalUnselect(hdc);
    ReleaseDC(hFrame, hdc);
}
