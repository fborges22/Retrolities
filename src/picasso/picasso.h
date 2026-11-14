/* Picasso - a Windows 95 style paint program for Windows 3.x. */
#ifndef PICASSO_H
#define PICASSO_H

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"

#define MAXDIM      2000        /* largest picture width / height */
#define MAXUNDO     3

#define ROP_PSDPxax 0x00B8074AL /* source 0 -> pattern, source 1 -> dest */
#define ROP_DSPDxax 0x00E20746L /* source 1 -> pattern, source 0 -> dest */

enum {
    T_LASSO, T_SELECT, T_ERASER, T_FILL, T_PICKER, T_ZOOM, T_PENCIL, T_BRUSH,
    T_SPRAY, T_TEXT, T_LINE, T_CURVE, T_RECT, T_POLYGON, T_ELLIPSE, T_RRECT,
    T_COUNT
};

enum { X_FLIPH, X_FLIPV, X_ROT90, X_ROT180, X_ROT270, X_INVERT };

typedef struct { HBITMAP hbm; int w, h; } SNAP;

/* The selection: a rectangle (optionally masked to a free-form shape) that
 * becomes "floating" once it is moved, pasted or transformed.  While it
 * floats, the picture holds base + selection and hbmBase the picture alone. */
typedef struct {
    BOOL active, floating;
    RECT rc;
    HBITMAP hbm, hbmMask, hbmBase;
} SEL;

/* ---- main.c ---- */
extern HINSTANCE hInst;
extern HWND hFrame, hCanvas;
extern COLORREF fgColor, bgColor;
extern int curTool, prevTool;
extern int optOpaque, eraserIdx, brushIdx, sprayIdx, lineIdx, fillIdx;
extern BOOL showGrid, dirty;
extern char fileName[260];
extern int saveBpp;
extern LOGFONT textFont;

void SetTool(int tool);
void SetFgColor(COLORREF c);
void SetBgColor(COLORREF c);
void SetStatusPos(int x, int y);        /* x < 0 clears */
void SetStatusSize(int w, int h);       /* w < 0 clears */
void UpdateUI(void);
void ErrorBox(const char *text);

/* ---- image.c ---- */
extern HDC hdcImg;
extern HBITMAP hbmImg;
extern int imgW, imgH;
extern int nUndo, nRedo;
extern SEL sel;

BOOL ImageInit(void);
void ImageDone(void);
HBITMAP MakeBitmap(int w, int h);
HBITMAP GrabBitmap(HDC src, int x, int y, int w, int h);
void FillColor(HDC hdc, int x, int y, int w, int h, COLORREF c);
COLORREF Near(COLORREF c);
BOOL NewImage(int w, int h);
void SetImage(HBITMAP hbm, int w, int h);
BOOL ResizeImage(int w, int h);
void ClearUndo(void);
BOOL PushUndo(void);
void UndoDiscard(void);
void BaseBlt(const RECT *rc);
void DoUndo(void);
void DoRedo(void);
void SelFree(void);
void SelCommit(void);
BOOL SelLift(BOOL copy);
void SelStamp(void);
void SelCompose(const RECT *old);
void SelDelete(void);
void DoCopy(void);
void DoPaste(void);
void Transform(int kind);
void ClearImage(void);

/* ---- file.c ---- */
BOOL LoadBMP(const char *path);
BOOL SaveBMP(const char *path, int bpp);
void PrintImage(void);

/* ---- canvas.c ---- */
extern int zoom, scrollX, scrollY;
extern const int brushSize[4][3];
extern const int eraserSize[4];
extern const int sprayRadius[3];

BOOL CanvasRegister(void);
void InvalidateImg(const RECT *rc, int inflate);
void CanvasImageChanged(void);
void SetZoom(int z, int cx, int cy);
void ZoomCenter(int z);
void OpsFinish(void);
BOOL OpsCancel(void);
void TextRefresh(void);

#endif
