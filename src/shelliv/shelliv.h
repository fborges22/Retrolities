/* ShellIV - a program and file shell for Windows 3.x (also Windows 9x/NT).
 *
 * One window replaces both Program Manager and File Manager: a themed
 * navigation sidebar (programs, favorites, drives with a folder tree,
 * the wastebasket and running tasks), an item view with icon, tile and
 * detail layouts, a preview pane and a status bar.
 */
#ifndef SHELLIV_H
#define SHELLIV_H

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "retro.h"
#include "resource.h"

#define APPNAME     "ShellIV"
#define INIFILE     "SHELLIV.INI"
#define MAXP        260         /* longest path handled */

/* ---- Win16 / Win32 differences ---------------------------------------- */
#ifdef RETRO_WIN32
#define __export
#define CMD_ID(wp, lp)          LOWORD(wp)
#define CMD_NOTIFY(wp, lp)      HIWORD(wp)
#define CMD_HWND(wp, lp)        ((HWND)(lp))
#define SCROLL_CODE(wp)         LOWORD(wp)
#define SCROLL_POS(wp, lp)      ((int)(short)HIWORD(wp))
#define MENUSEL_ITEM(wp)        LOWORD(wp)
#define MENUSEL_FLAGS(wp, lp)   HIWORD(wp)
#define THUNK(f)                ((FARPROC)(f))
#define FREETHUNK(f)            ((void)0)
#define PATHCMP                 lstrcmpi
#define WINDOW_TASK(h)          GetWindowThreadProcessId(h, NULL)
typedef BOOL DLGRESULT;
#else
#define CMD_ID(wp, lp)          (wp)
#define CMD_NOTIFY(wp, lp)      HIWORD(lp)
#define CMD_HWND(wp, lp)        ((HWND)LOWORD(lp))
#define SCROLL_CODE(wp)         (wp)
#define SCROLL_POS(wp, lp)      ((int)(short)LOWORD(lp))
#define MENUSEL_ITEM(wp)        (wp)
#define MENUSEL_FLAGS(wp, lp)   LOWORD(lp)
#define THUNK(f)                MakeProcInstance((FARPROC)(f), hInst)
#define FREETHUNK(f)            FreeProcInstance(f)
#define PATHCMP                 lstrcmpi
#define WINDOW_TASK(h)          GetWindowTask(h)
typedef BOOL DLGRESULT;
#endif
/* The Windows 386 extender has no export records, nor lstrcpyn(). */
#ifdef RETRO_WIN386
#define __export
#define lstrcpyn                StrCopyN
#endif
char *StrCopyN(char *dst, const char *src, int n);

#ifdef RETRO_WIN386
HICON W386ExtractIcon(HINSTANCE inst, const char *file, UINT idx);
HINSTANCE W386FindExecutable(const char *file, const char *dir, char *result);
HINSTANCE W386ShellExecute(HWND hwnd, const char *op, const char *file, const char *args,
                           const char *dir, int show);
UINT W386DragQueryFile(HDROP hd, UINT i, char *buf, UINT size);
BOOL W386DragQueryPoint(HDROP hd, POINT *pt);
void W386DragFinish(HDROP hd);
void W386DragAcceptFiles(HWND hwnd, BOOL accept);
UINT W386WNetAddConnection(const char *path, const char *pass, const char *local);
UINT W386WNetCancelConnection(const char *name, BOOL force);
#ifndef SHELL16_C
#define DragFinish      W386DragFinish
#define DragAcceptFiles W386DragAcceptFiles
#define WNetAddConnection(a, b, c)  W386WNetAddConnection(a, b, c)
#define WNetCancelConnection(a, b)  W386WNetCancelConnection(a, b)
#define ExtractIcon     W386ExtractIcon
#define FindExecutable  W386FindExecutable
#define ShellExecute    W386ShellExecute
#define DragQueryFile   W386DragQueryFile
#define DragQueryPoint  W386DragQueryPoint
#endif
#endif

/* Pointers that arrive in lParam (WM_DRAWITEM, WM_GETMINMAXINFO, ...).
 * Under the Windows 386 extender they are 16:16 and need a 32-bit alias. */
#ifdef RETRO_WIN386
#define XFAR                    __far
#define LP2PTR(lp)              MK_FP32((void *)(lp))
#else
#define XFAR
#define LP2PTR(lp)              ((void *)(lp))
#endif
/* Windows 3.x has no DT_END_ELLIPSIS: TextIn() shortens such text itself. */
#ifndef DT_END_ELLIPSIS
#define DT_END_ELLIPSIS         0x8000
#define NEED_ELLIPSIS           1
#endif
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL           0x020A
#endif
#define MOUSE_X(lp)             ((int)(short)LOWORD(lp))
#define MOUSE_Y(lp)             ((int)(short)HIWORD(lp))
#ifndef max
#define max(a, b)               ((a) > (b) ? (a) : (b))
#define min(a, b)               ((a) < (b) ? (a) : (b))
#endif

/* ---- builtin glyphs (16x16 strip, res/glyphs.bmp) ---------------------- */
enum {
    G_FOLDER, G_FOLDEROPEN, G_FLOPPY, G_HARDDISK, G_CDROM, G_NETDRIVE,
    G_COMPUTER, G_GROUP, G_RECENT, G_STAR, G_TASK, G_DOC, G_PROGRAM,
    G_SEARCH, G_TEXT, G_IMAGE, G_WASTE, G_RAMDISK, G_DOSPROG, G_SYSFILE,
    G_ARCHIVE, G_MUSIC,
    /* toolbar glyphs */
    G_BACK, G_FORWARD, G_UP, G_VICONS, G_VTILES, G_VDETAILS, G_CUT,
    G_COPY, G_PASTE, G_DELETE, G_PROPS, G_NEWFOLDER, G_RUN, G_PREVIEW,
    G_REFRESH, G_THEME,
    G_COUNT
};

/* ---- builtin large icons (resource ids IDI_xxx) ------------------------ */
enum {
    BI_FOLDER, BI_FLOPPY, BI_HARDDISK, BI_CDROM, BI_NETDRIVE, BI_COMPUTER,
    BI_GROUP, BI_RECENT, BI_STAR, BI_DOC, BI_PROGRAM, BI_DOSPROG, BI_TEXT,
    BI_IMAGE, BI_WASTE, BI_WASTEFULL, BI_SEARCH, BI_SYSFILE, BI_RAMDISK,
    BI_ARCHIVE, BI_MUSIC, BI_APP,
    BI_COUNT
};

/* ---- themes (theme.c) -------------------------------------------------- */
typedef struct {
    const char *name;
    COLORREF band1, band2, bandText;        /* location banner gradient */
    COLORREF side, sideText, sideHead, sideSel, sideSelText, sideHot;
    COLORREF view, viewText, dimText, stripe;
    COLORREF sel, selBorder, selText;
    COLORREF bar1, bar2, barText, barHot;   /* toolbar and status bar */
    COLORREF gauge, gaugeFull, accent;
} THEME;

extern THEME th;
extern int themeIdx, nThemes;
extern HFONT fontUI, fontBold, fontTitle, fontSmall, fontHead;
extern int lineH;                           /* height of fontUI */

void ThemeInit(void);
void ThemeDone(void);
void ThemeSet(int idx);
const char *ThemeName(int idx);
COLORREF Solid(COLORREF c);
COLORREF PC(COLORREF c);                  /* colour for GDI: palette index on 8-bit displays */
extern HPALETTE hPal;                     /* theme palette on palette devices, else NULL */
void PalSelect(HDC hdc);
BOOL PalRealize(HWND hwnd);               /* TRUE if colours changed */
void FillR(HDC hdc, const RECT *r, COLORREF c);
void FillXY(HDC hdc, int x, int y, int w, int h, COLORREF c);
void Gradient(HDC hdc, const RECT *r, COLORREF a, COLORREF b, BOOL vertical);
void FrameR(HDC hdc, const RECT *r, COLORREF c);
void Bevel(HDC hdc, const RECT *r, COLORREF hi, COLORREF lo);
void HLine(HDC hdc, int x1, int x2, int y, COLORREF c);
void VLine(HDC hdc, int x, int y1, int y2, COLORREF c);
void Pill(HDC hdc, const RECT *r, COLORREF fill, COLORREF border);
void Triangle(HDC hdc, int x, int y, BOOL down, COLORREF c);
void DrawGauge(HDC hdc, const RECT *r, DWORD freeKB, DWORD totalKB, COLORREF back);
void DrawGlyph(HDC hdc, int x, int y, int glyph);
void TextAt(HDC hdc, int x, int y, const char *s, COLORREF c);
void TextIn(HDC hdc, const RECT *r, const char *s, COLORREF c, UINT fmt);
int TextW(HDC hdc, const char *s);
COLORREF Mix(COLORREF a, COLORREF b, int pct);
HICON BuiltinIcon(int bi);

/* Flicker free painting: calls paint(hdc, rcPaint) on an off-screen strip
 * buffer that uses window coordinates. */
typedef void (*PAINTFN)(HWND hwnd, HDC hdc, const RECT *rc);
void BufferedPaint(HWND hwnd, PAINTFN paint);

/* ---- helpers (util.c) -------------------------------------------------- */
extern HINSTANCE hInst;
char *StrDup(const char *s);
void StrFree(char **p);
void StrSet(char **p, const char *s);
void PathJoin(char *out, const char *dir, const char *name);
BOOL PathParent(char *path);              /* FALSE at a drive root */
const char *PathName(const char *path);   /* last component */
const char *PathExt(const char *path);    /* ".EXT" or "" */
void PathNormalize(char *path);
BOOL PathIsRoot(const char *path);
BOOL PathInside(const char *path, const char *dir);  /* dir is path or ancestor */
BOOL WildMatch(const char *pat, const char *name);
void FmtNum(char *out, DWORD n);          /* 1,234,567 */
void FmtSize(char *out, DWORD bytes);     /* 12.3 KB */
void FmtKB(char *out, DWORD kb);          /* 1.2 GB */
void FmtDate(char *out, WORD date, WORD time);
void FmtAttr(char *out, int attr);
int IniInt(const char *sec, const char *key, int def);
void IniGet(const char *sec, const char *key, const char *def, char *out, int size);
void IniPut(const char *sec, const char *key, const char *val);
void IniPutInt(const char *sec, const char *key, int val);
int Msg(UINT flags, const char *fmt, ...);
BOOL PumpMessages(void);                  /* FALSE if Esc was pressed */
int RunDialog(int id, FARPROC proc, LPARAM lp);
void CenterWindow(HWND hwnd);

/* ---- file system (files.c) -------------------------------------------- */
#define FA_RDONLY   0x01
#define FA_HIDDEN   0x02
#define FA_SYSTEM   0x04
#define FA_DIR      0x10
#define FA_ARCH     0x20

enum { DK_NONE, DK_FLOPPY, DK_HARD, DK_CDROM, DK_NET, DK_RAM };

typedef struct {
    char name[MAXP];
    DWORD size;
    WORD date, time;
    BYTE attr;
} FINDINFO;

typedef struct FINDSTATE FINDSTATE;
FINDSTATE *FsOpen(const char *dir, const char *pattern, FINDINFO *fi);
BOOL FsNext(FINDSTATE *fs, FINDINFO *fi);
void FsClose(FINDSTATE *fs);

int DriveKind(int drive);                 /* drive 0 = A: */
BOOL DriveSpace(int drive, DWORD *freeKB, DWORD *totalKB);
BOOL DriveLabel(int drive, char *out, int size);
BOOL IsDir(const char *path);
BOOL Exists(const char *path);
int GetAttr(const char *path);            /* -1 on error */
BOOL SetAttr(const char *path, int attr);
BOOL GetStamp(const char *path, WORD *date, WORD *time, DWORD *size);
BOOL MakeDir(const char *path);
BOOL RemoveDir(const char *path);
BOOL DeleteOne(const char *path);
BOOL RenameOne(const char *from, const char *to);
BOOL CopyFileData(const char *from, const char *to, BOOL (*progress)(DWORD done));
void UniqueName(char *path);              /* adds " (2)" / ~2 until unused */

/* ---- program groups (groups.c) ----------------------------------------- */
typedef struct {
    char *name, *cmd, *dir, *iconPath;
    int iconIdx;
    WORD hotkey;
} PROG;

typedef struct {
    char *name;
    int n, cap;
    PROG *items;
} GROUP;

#define MAXGROUPS   64
#define MAXRECENT   16
#define MAXFAVS     24

extern GROUP groups[MAXGROUPS];
extern int nGroups;
extern GROUP recent;
extern char *favs[MAXFAVS];
extern int nFavs;

void GroupsLoad(void);
void GroupsSave(void);
void GroupsDone(void);
int GroupAdd(const char *name);
void GroupDelete(int g);
void GroupRename(int g, const char *name);
GROUP *GroupPtr(int g);                   /* -1 = recent */
int ProgAdd(int g, const PROG *p, int pos);
void ProgDelete(int g, int i);
void ProgMove(int g, int i, int g2, int pos);
void ProgSet(PROG *p, const char *name, const char *cmd, const char *dir,
             const char *iconPath, int iconIdx);
void ProgFree(PROG *p);
void ProgFromFile(PROG *p, const char *path);
int ImportProgman(BOOL quiet);
BOOL Launch(const char *cmd, const char *dir, int show);
void LaunchProg(const PROG *p);
void RecentAdd(const PROG *p);
void FavAdd(const char *path);
void FavRemove(int i);
void ProgDisplayCmd(const PROG *p, char *file, int size);

/* Shortcut keys of program items, as in Program Manager: the low byte is
 * the virtual key, the high byte HK_SHIFT / HK_CTRL / HK_ALT. */
#define HK_SHIFT    0x01
#define HK_CTRL     0x02
#define HK_ALT      0x04
void HotkeyText(WORD hk, char *out);
int HotkeyParse(const char *text, WORD *hk);      /* FALSE if invalid */
BOOL HotkeyRun(WORD hk);                          /* starts the item that has it */
void HotkeysRegister(void);                       /* system-wide keys (Win32) */

/* ---- items in the view (view.c) ---------------------------------------- */
enum { IK_DRIVE, IK_DIR, IK_FILE, IK_PROG, IK_WASTE, IK_GROUP };

typedef struct {
    char *name;         /* display name */
    char *sub;          /* program: command line; search: folder; waste: origin */
    DWORD size;         /* bytes; drive: free KB */
    DWORD total;        /* drive: total KB */
    WORD date, time;
    BYTE attr, kind, sel, dk;
    short ref;          /* drive number, program index, waste entry */
} ITEM;

enum { VM_ICONS, VM_TILES, VM_DETAILS };

extern HWND hView;
extern ITEM **items;
extern int nItems, viewMode, sortCol, sortDesc;

BOOL ViewRegister(void);
void ViewBegin(void);
ITEM *ViewAdd(int kind, const char *name, const char *sub);
void ViewEnd(const char *keepFocus);
void ViewSetBanner(int bi, const char *title, const char *sub);
void ViewSetMode(int mode);
void ViewSort(int col);
int ViewSelCount(void);
int ViewFocus(void);
ITEM *ViewFocusItem(void);
void ViewSelectName(const char *name);
void ViewSelectAll(void);
int ViewSelectPattern(const char *pat);
void ViewRename(void);
void ViewItemPath(const ITEM *it, char *out);
void ViewInvalidate(void);
void ViewFree(void);
int ViewDropTarget(POINT pt, char *path, int *group, int *pos);
void ViewSetDropHilite(int i);
void ViewEndRename(BOOL commit);
void ViewSetGauge(DWORD freeKB, DWORD totalKB);
void ViewEnsureVisible(int i);
extern HWND hRename;
enum { DT_NONE, DT_DIR, DT_GROUP, DT_WASTE };

/* ---- icons (icons.c) ---------------------------------------------------- */
void IconsFlush(void);
HICON FileIcon(const char *file, int idx);
int FileIconCount(const char *file);
void ItemTypeName(const ITEM *it, char *out);
int ItemBuiltin(const ITEM *it);
int ItemGlyph(const ITEM *it);
HICON ItemIcon(const ITEM *it, BOOL *doc);
void DrawIconSmall(HDC hdc, int x, int y, HICON h, COLORREF bg);
void DrawItemIcon(HDC hdc, int x, int y, const ITEM *it);
void DrawItemSmall(HDC hdc, int x, int y, const ITEM *it, COLORREF bg);

/* ---- navigation (nav.c) ------------------------------------------------- */
enum { LOC_COMPUTER, LOC_DIR, LOC_GROUP, LOC_RECENT, LOC_SEARCH, LOC_WASTE, LOC_PROGRAMS };

typedef struct {
    int kind;
    int group;
    char path[MAXP];
} LOCATION;

extern LOCATION cur;
extern char searchPat[80], searchText[80];
extern BOOL searchSubdirs;
extern BOOL showHidden;

void Navigate(const LOCATION *loc, BOOL history);
void NavDir(const char *path);
void NavKind(int kind, int group);
void NavRefresh(void);
void NavBack(void);
void NavForward(void);
void NavUp(void);
BOOL NavCanBack(void);
BOOL NavCanForward(void);
BOOL NavCanUp(void);
void NavOpenItem(ITEM *it);
void NavSearch(const char *dir, const char *pattern, const char *text, BOOL subdirs);
void NavInit(void);
void NavDone(void);
void LocTitle(const LOCATION *loc, char *out, int size);
BOOL LocIsFiles(void);                   /* current location holds files */

/* ---- sidebar (sidebar.c) ------------------------------------------------- */
extern HWND hSide;
BOOL SideRegister(void);
void SideRebuild(void);
void SideSync(void);                     /* select the node for cur */
void SideRefreshDrives(void);
void SideRebuildDrives(void);
BOOL SideTasksChanged(void);
void SideDone(void);
int SideDropTarget(POINT pt, char *path, int *group);
void SideFocusNode(void);
void SideFocusTasks(void);
void SideSetDropHilite(int node);
void SideLoadState(void);
void SideSaveState(void);

/* ---- preview pane (preview.c) ------------------------------------------- */
extern HWND hPreview;
BOOL PreviewRegister(void);
void PreviewUpdate(void);
void PreviewDone(void);

/* ---- file operations (ops.c) ------------------------------------------- */
typedef struct {
    int n, cap;
    char **p;
} PATHLIST;

void PlInit(PATHLIST *pl);
void PlAdd(PATHLIST *pl, const char *path);
void PlFree(PATHLIST *pl);
void SelectedPaths(PATHLIST *pl);
BOOL OpsCopyMove(PATHLIST *pl, const char *destDir, BOOL move);
BOOL OpsDelete(PATHLIST *pl, BOOL permanent);
void OpsNewFolder(void);
void OpsClipboard(BOOL cut);
void OpsPaste(void);
BOOL OpsCanPaste(void);
void OpsDrop(PATHLIST *pl, const char *destDir, int group, BOOL copy, BOOL ask);
void OpsDropFiles(HWND hwnd, HDROP hd);
void OpsRename(ITEM *it, const char *newName);
BOOL OpsBusy(void);

/* wastebasket */
typedef struct {
    char *stored;       /* full path inside the waste folder */
    char *origin;       /* original full path */
    WORD date, time;    /* deletion time */
    DWORD size;
    BYTE attr;
} WASTEENT;
extern WASTEENT *waste;
extern int nWaste;
void WasteLoad(void);
void WasteFree(void);
BOOL WasteRestore(PATHLIST *pl);         /* list of stored paths */
BOOL WasteEmpty(void);
BOOL WasteUsable(const char *path);

/* ---- dialogs (dialogs.c) ------------------------------------------------ */
BOOL DlgProgram(PROG *p, BOOL isNew);
BOOL DlgInput(const char *title, const char *prompt, char *buf, int size);
void DlgRun(const char *initial);
BOOL DlgFind(void);
void DlgProperties(void);
void DlgAbout(void);
BOOL DlgPickIcon(char *path, int *idx);
void DlgOptions(void);
int DlgConfirm(const char *title, const char *text, BOOL all);
enum { CONF_NO, CONF_YES, CONF_ALL, CONF_CANCEL };

/* ---- frame (main.c) ---------------------------------------------------- */
extern HWND hFrame, hAddr, hFind;
extern BOOL showPreview, confirmDelete, confirmOps, minimizeOnUse, saveOnExit;
extern BOOL isShell;

void Status(const char *text);
void UpdateUI(void);
void UpdateTitle(void);
void SetBusy(BOOL busy);
void FocusView(void);
void Layout(void);
void SelectionChanged(void);
void ContextMenu(HWND from, POINT screen, BOOL onItem);
void Command(int id);
void SaveSettings(void);

/* drag and drop between the view and the sidebar */
extern BOOL dragging;
void DragBegin(void);
void DragMove(POINT screen, BOOL copy);
void DragEnd(POINT screen, BOOL copy, BOOL cancel);

#endif
