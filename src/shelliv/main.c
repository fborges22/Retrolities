/* ShellIV - the frame window: toolbar with address and search fields,
 * sidebar / view / preview panes with splitters, status bar, menus,
 * commands, drag and drop, settings and the shell start-up. */
#include <ctype.h>
#include "shelliv.h"
#ifndef RETRO_WIN32
#include <dos.h>
#endif

#define TB_H        40
#define ST_H        24
#define BTN_W       28
#define BTN_H       28
#define SPLIT_W     5
#define TIMER_CLOCK 1
#define WM_SELCHANGED (WM_USER + 10)

HINSTANCE hInst;
HWND hFrame, hAddr, hFind;
BOOL showPreview = TRUE, confirmDelete = TRUE, confirmOps = TRUE, minimizeOnUse, saveOnExit = TRUE;
BOOL isShell;
BOOL dragging;

static int sideW = 210, prevW = 230;
static int hotBtn = -1, pressBtn = -1, splitting;      /* 1 = sidebar, 2 = preview */
static char statusMsg[160];
static DWORD statusTime;
static char clockText[16];
static BOOL selPosted, hotTimer;
static HCURSOR curWait, curArrow, curSizeWE, curDrag[4];
static int busyCount;

/* ---- toolbar --------------------------------------------------------------- */

typedef struct { int cmd, glyph; const char *hint; } TBBTN;

static const TBBTN tb[] = {
    { IDM_BACK, G_BACK, "Back to the previous location (Alt+Left)" },
    { IDM_FORWARD, G_FORWARD, "Forward (Alt+Right)" },
    { IDM_UP, G_UP, "Up one level (Backspace)" },
    { 0, 0, NULL },
    { IDM_VICONS, G_VICONS, "View as large icons" },
    { IDM_VTILES, G_VTILES, "View as tiles with details" },
    { IDM_VDETAILS, G_VDETAILS, "View as a sortable list" },
    { 0, 0, NULL },
    { IDM_CUT, G_CUT, "Cut the selected files (Ctrl+X)" },
    { IDM_COPY, G_COPY, "Copy the selected files (Ctrl+C)" },
    { IDM_PASTE, G_PASTE, "Paste files here (Ctrl+V)" },
    { IDM_DELETE, G_DELETE, "Move the selection to the Wastebasket (Del)" },
    { IDM_PROPS, G_PROPS, "Properties (Alt+Enter)" },
    { 0, 0, NULL },
    { IDM_NEWFOLDER, G_NEWFOLDER, "New folder or program" },
    { IDM_RUN, G_RUN, "Run a program (Ctrl+R)" },
    { IDM_PREVIEW, G_PREVIEW, "Show or hide the preview pane (F3)" },
    { IDM_OPTIONS, G_THEME, "Themes and options" },
};
#define NTB (int)(sizeof(tb) / sizeof(tb[0]))

static BOOL BtnRect(int i, RECT *r)
{
    int x = 8, k;

    for (k = 0; k < i; k++)
        x += tb[k].cmd ? BTN_W + 1 : 9;
    if (!tb[i].cmd)
        return FALSE;
    SetRect(r, x, (TB_H - BTN_H) / 2, x + BTN_W, (TB_H - BTN_H) / 2 + BTN_H);
    return TRUE;
}

static int ButtonsRight(void)
{
    RECT r;

    BtnRect(NTB - 1, &r);
    return r.right + 12;
}

static BOOL BtnEnabled(int cmd)
{
    int n = ViewSelCount();

    switch (cmd) {
    case IDM_BACK:    return NavCanBack();
    case IDM_FORWARD: return NavCanForward();
    case IDM_UP:      return NavCanUp();
    case IDM_CUT:
    case IDM_COPY:    return n > 0 && LocIsFiles();
    case IDM_PASTE:   return OpsCanPaste();
    case IDM_DELETE:  return n > 0 && cur.kind != LOC_COMPUTER;
    case IDM_PROPS:   return n > 0 || cur.kind == LOC_DIR;
    case IDM_NEWFOLDER: return cur.kind == LOC_DIR || cur.kind == LOC_GROUP || cur.kind == LOC_PROGRAMS;
    }
    return TRUE;
}

static BOOL BtnChecked(int cmd)
{
    switch (cmd) {
    case IDM_VICONS:   return viewMode == VM_ICONS;
    case IDM_VTILES:   return viewMode == VM_TILES;
    case IDM_VDETAILS: return viewMode == VM_DETAILS;
    case IDM_PREVIEW:  return showPreview;
    }
    return FALSE;
}

static void FieldRect(HWND ed, RECT *r)
{
    GetWindowRect(ed, r);
    ScreenToClient(hFrame, (POINT *)&r->left);
    ScreenToClient(hFrame, (POINT *)&r->right);
    InflateRect(r, 6, 5);
}

static void PaintToolbar(HDC hdc, int w)
{
    RECT r;
    int i;

    SetRect(&r, 0, 0, w, TB_H);
    Gradient(hdc, &r, th.bar1, th.bar2, TRUE);
    HLine(hdc, 0, w, TB_H - 1, Mix(th.bar2, RGB(0, 0, 0), 22));
    for (i = 0; i < NTB; i++) {
        BOOL en, chk;

        if (!BtnRect(i, &r)) {
            BtnRect(i - 1, &r);
            VLine(hdc, r.right + 4, 9, TB_H - 9, Mix(th.bar2, RGB(0, 0, 0), 18));
            VLine(hdc, r.right + 5, 9, TB_H - 9, Mix(th.bar1, RGB(255, 255, 255), 50));
            continue;
        }
        en = BtnEnabled(tb[i].cmd);
        chk = BtnChecked(tb[i].cmd);
        if (en && (i == hotBtn || i == pressBtn || chk)) {
            COLORREF f = i == pressBtn && i == hotBtn ? th.sel : chk ? Mix(th.barHot, th.bar1, 30) : th.barHot;

            Pill(hdc, &r, f, chk || i == pressBtn ? th.selBorder : Mix(th.selBorder, th.barHot, 50));
        }
        if (en) {
            int d = (i == pressBtn && i == hotBtn) ? 1 : 0;

            DrawGlyph(hdc, r.left + 6 + d, r.top + 6 + d, tb[i].glyph);
        } else {
            /* disabled: an embossed silhouette */
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, 16, 16), old = SelectObject(mem, bmp);
            RECT q;
            int x, y;
            COLORREF bg;

            SetRect(&q, 0, 0, 16, 16);
            bg = Mix(th.bar1, th.bar2, 50);
            FillR(mem, &q, bg);
            DrawGlyph(mem, 0, 0, tb[i].glyph);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 16; x++) {
                    COLORREF c = GetPixel(mem, x, y);

                    if (c != bg) {
                        int lum = (GetRValue(c) * 3 + GetGValue(c) * 6 + GetBValue(c)) / 10;

                        SetPixel(hdc, r.left + 6 + x, r.top + 6 + y,
                                 lum < 160 ? Mix(th.bar2, th.barText, 30) : Mix(th.bar1, th.bar2, 40));
                    }
                }
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
    }
    /* address and search fields */
    if (hAddr) {
        FieldRect(hAddr, &r);
        r.left -= 22;
        Pill(hdc, &r, RGB(255, 255, 255), Mix(th.bar2, RGB(0, 0, 0), 25));
        DrawGlyph(hdc, r.left + 5, r.top + (r.bottom - r.top - 16) / 2,
                  cur.kind == LOC_DIR ? G_FOLDEROPEN : cur.kind == LOC_COMPUTER ? G_COMPUTER :
                  cur.kind == LOC_WASTE ? G_WASTE : cur.kind == LOC_SEARCH ? G_SEARCH :
                  cur.kind == LOC_RECENT ? G_RECENT : G_GROUP);
    }
    if (hFind) {
        FieldRect(hFind, &r);
        r.left -= 22;
        Pill(hdc, &r, RGB(255, 255, 255), Mix(th.bar2, RGB(0, 0, 0), 25));
        DrawGlyph(hdc, r.left + 5, r.top + (r.bottom - r.top - 16) / 2, G_SEARCH);
    }
}

static int BtnAt(POINT pt)
{
    RECT r;
    int i;

    if (pt.y >= TB_H)
        return -1;
    for (i = 0; i < NTB; i++)
        if (BtnRect(i, &r) && PtInRect(&r, pt))
            return i;
    return -1;
}

static void InvalidateToolbar(void)
{
    RECT r;

    GetClientRect(hFrame, &r);
    r.bottom = TB_H;
    InvalidateRect(hFrame, &r, FALSE);
}

/* ---- status bar ------------------------------------------------------------- */

static void StatusRect(RECT *r)
{
    GetClientRect(hFrame, r);
    r->top = r->bottom - ST_H;
}

void Status(const char *text)
{
    RECT r;

    lstrcpyn(statusMsg, text, sizeof(statusMsg));
    statusTime = GetTickCount();
    StatusRect(&r);
    InvalidateRect(hFrame, &r, FALSE);
}

static void SelSummary(char *out)
{
    int n = ViewSelCount(), i;
    DWORD bytes = 0;
    char a[30], b[30];

    for (i = 0; i < nItems; i++)
        if (items[i]->sel)
            bytes += items[i]->size;
    FmtNum(a, (DWORD)nItems);
    if (n == 0) {
        sprintf(out, "%s item%s", a, nItems == 1 ? "" : "s");
    } else if (LocIsFiles() || cur.kind == LOC_WASTE) {
        FmtSize(b, bytes);
        sprintf(out, "%d of %s selected  \xB7  %s", n, a, b);
    } else {
        sprintf(out, "%d of %s selected", n, a);
    }
}

static void PaintStatus(HDC hdc, int w, int h)
{
    RECT r, q;
    char text[200], a[30], b[30];
    int x;

    SetRect(&r, 0, h - ST_H, w, h);
    Gradient(hdc, &r, th.bar2, th.bar1, TRUE);
    HLine(hdc, 0, w, r.top, Mix(th.bar2, RGB(0, 0, 0), 22));
    SelectObject(hdc, fontUI);
    /* clock */
    x = w - 70;
    SetRect(&q, x, r.top + 1, w - 10, r.bottom);
    TextIn(hdc, &q, clockText, th.barText, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    VLine(hdc, x - 4, r.top + 5, r.bottom - 5, Mix(th.bar2, RGB(0, 0, 0), 20));
    /* memory and resources */
#ifdef RETRO_WIN32
    {
        MEMORYSTATUS ms;

        ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        FmtKB(a, ms.dwAvailPhys / 1024);
        sprintf(text, "%s free  \xB7  %lu%% load", a, ms.dwMemoryLoad);
    }
#else
    FmtKB(a, GetFreeSpace(0) / 1024);
    sprintf(text, "%s free  \xB7  %u%% resources", a, GetFreeSystemResources(GFSR_SYSTEMRESOURCES));
#endif
    x -= 190;
    SetRect(&q, x + 6, r.top + 1, x + 182, r.bottom);
    if (w > 640) {
        TextIn(hdc, &q, text, th.barText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        VLine(hdc, x, r.top + 5, r.bottom - 5, Mix(th.bar2, RGB(0, 0, 0), 20));
    } else {
        x += 190;
    }
    /* current drive */
    if (cur.kind == LOC_DIR && cur.path[1] == ':' && w > 520) {
        DWORD f, t;

        if (DriveSpace(cur.path[0] - 'A', &f, &t) && t) {
            x -= 210;
            FmtKB(a, f);
            FmtKB(b, t);
            SetRect(&q, x + 8, r.top + 8, x + 70, r.bottom - 7);
            DrawGauge(hdc, &q, f, t, RGB(255, 255, 255));
            sprintf(text, "%c:  %s free", cur.path[0], a);
            SetRect(&q, x + 76, r.top + 1, x + 206, r.bottom);
            TextIn(hdc, &q, text, th.barText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            VLine(hdc, x, r.top + 5, r.bottom - 5, Mix(th.bar2, RGB(0, 0, 0), 20));
        }
    }
    /* message or selection summary */
    if (statusMsg[0] && GetTickCount() - statusTime < 6000)
        lstrcpyn(text, statusMsg, sizeof(text));
    else
        SelSummary(text);
    SetRect(&q, 10, r.top + 1, x - 8, r.bottom);
    TextIn(hdc, &q, text, th.barText, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

/* ---- layout --------------------------------------------------------------- */

void Layout(void)
{
    RECT cl;
    int top = TB_H, bottom, left, right, findW = 170, addrL;

    if (!hFrame)
        return;
    GetClientRect(hFrame, &cl);
    bottom = cl.bottom - ST_H;
    sideW = max(120, min(sideW, cl.right / 2));
    prevW = max(150, min(prevW, cl.right / 3));
    left = sideW;
    right = cl.right;
    if (showPreview && cl.right > 560)
        right -= prevW;
    if (hSide)
        MoveWindow(hSide, 0, top, sideW - 1, max(0, bottom - top), TRUE);
    if (hView)
        MoveWindow(hView, left + (SPLIT_W - 1), top, max(0, right - left - (SPLIT_W - 1)), max(0, bottom - top), TRUE);
    if (hPreview) {
        if (showPreview && cl.right > 560) {
            MoveWindow(hPreview, right, top, cl.right - right, max(0, bottom - top), TRUE);
            ShowWindow(hPreview, SW_SHOWNA);
        } else {
            ShowWindow(hPreview, SW_HIDE);
        }
    }
    addrL = ButtonsRight() + 24;
    if (cl.right - addrL - findW - 40 < 120)
        findW = max(80, cl.right - addrL - 160);
    if (hAddr)
        MoveWindow(hAddr, addrL, (TB_H - lineH) / 2, max(40, cl.right - addrL - findW - 44),
                   lineH + 2, TRUE);
    if (hFind)
        MoveWindow(hFind, cl.right - findW - 8, (TB_H - lineH) / 2, findW, lineH + 2, TRUE);
    InvalidateRect(hFrame, NULL, FALSE);
}

static void PaintFrame(HWND hwnd, HDC hdc, const RECT *rc)
{
    RECT cl, r;

    GetClientRect(hwnd, &cl);
    if (rc->top < TB_H)
        PaintToolbar(hdc, cl.right);
    if (rc->bottom > cl.bottom - ST_H)
        PaintStatus(hdc, cl.right, cl.bottom);
    /* splitters */
    SetRect(&r, sideW - 1, TB_H, sideW + SPLIT_W - 1, cl.bottom - ST_H);
    FillR(hdc, &r, th.bar2);
    VLine(hdc, sideW + SPLIT_W - 2, TB_H, cl.bottom - ST_H, Mix(th.bar2, RGB(0, 0, 0), 18));
}

static int SplitAt(int x, int y)
{
    RECT cl;

    GetClientRect(hFrame, &cl);
    if (y < TB_H || y > cl.bottom - ST_H)
        return 0;
    if (x >= sideW - 3 && x <= sideW + SPLIT_W + 1)
        return 1;
    if (showPreview && hPreview && IsWindowVisible(hPreview)) {
        RECT p;

        GetWindowRect(hPreview, &p);
        ScreenToClient(hFrame, (POINT *)&p.left);
        if (x >= p.left - 4 && x <= p.left + 2)
            return 2;
    }
    return 0;
}

/* ---- UI state ---------------------------------------------------------------- */

void UpdateTitle(void)
{
    char t[MAXP + 20], loc[MAXP];

    LocTitle(&cur, loc, MAXP);
    if (cur.kind == LOC_GROUP) {
        sprintf(t, "Programs\\%s", loc);
        SetWindowText(hAddr, t);
    } else {
        SetWindowText(hAddr, loc);
    }
    sprintf(t, "%s - %s", APPNAME, loc);
    SetWindowText(hFrame, t);
}

void UpdateUI(void)
{
    InvalidateToolbar();
    if (hFrame) {
        RECT r;

        StatusRect(&r);
        InvalidateRect(hFrame, &r, FALSE);
    }
}

void SelectionChanged(void)
{
    if (!selPosted && hFrame) {
        selPosted = TRUE;
        PostMessage(hFrame, WM_SELCHANGED, 0, 0);
    }
}

void SetBusy(BOOL busy)
{
    busyCount += busy ? 1 : -1;
    if (busyCount < 0)
        busyCount = 0;
    SetCursor(busyCount ? curWait : curArrow);
}

void FocusView(void)
{
    if (hView)
        SetFocus(hView);
}

/* ---- drag and drop ------------------------------------------------------------ */

enum { DS_NONE, DS_FILES, DS_PROGS };
static int dragSrc, dragGroup;
static PATHLIST dragList;
static int *dragIdx, nDragIdx;

void DragBegin(void)
{
    int i;

    dragSrc = DS_NONE;
    PlFree(&dragList);
    if (cur.kind == LOC_GROUP) {
        if (dragIdx)
            free(dragIdx);
        dragIdx = (int *)malloc(nItems * sizeof(int) + 1);
        nDragIdx = 0;
        for (i = 0; i < nItems && dragIdx; i++)
            if (items[i]->sel && items[i]->kind == IK_PROG)
                dragIdx[nDragIdx++] = items[i]->ref;
        if (nDragIdx) {
            dragSrc = DS_PROGS;
            dragGroup = cur.group;
        }
    } else if (LocIsFiles()) {
        SelectedPaths(&dragList);
        if (dragList.n)
            dragSrc = DS_FILES;
    } else if (cur.kind == LOC_RECENT) {
        dragSrc = DS_PROGS;
        dragGroup = -1;
        dragIdx = (int *)malloc(nItems * sizeof(int) + 1);
        nDragIdx = 0;
        for (i = 0; i < nItems && dragIdx; i++)
            if (items[i]->sel)
                dragIdx[nDragIdx++] = items[i]->ref;
    }
    if (dragSrc == DS_NONE)
        return;
    dragging = TRUE;
    SetCursor(curDrag[0]);
}

/* What would happen on a drop here: 0 nothing, 1 move, 2 copy, 3 link. */
static int DropAction(POINT screen, BOOL copy, int *t, char *path, int *group, int *pos)
{
    HWND h = WindowFromPoint(screen);
    POINT pt = screen;

    *t = DT_NONE;
    *group = -1;
    *pos = -1;
    path[0] = '\0';
    if (h == hView) {
        ScreenToClient(hView, &pt);
        *t = ViewDropTarget(pt, path, group, pos);
    } else {
        ViewSetDropHilite(-1);
    }
    if (h == hSide) {
        ScreenToClient(hSide, &pt);
        *t = SideDropTarget(pt, path, group);
    } else {
        SideSetDropHilite(-1);
    }
    if (dragSrc == DS_FILES) {
        if (*t == DT_GROUP)
            return 3;
        if (*t == DT_WASTE)
            return 1;
        if (*t == DT_DIR) {
            char parent[MAXP];

            lstrcpyn(parent, dragList.p[0], MAXP);
            PathParent(parent);
            if (lstrcmpi(parent, path) == 0 && !copy)
                return 0;
            if (copy || toupper(path[0]) != toupper(dragList.p[0][0]))
                return 2;
            return 1;
        }
        return 0;
    }
    if (dragSrc == DS_PROGS && *t == DT_GROUP) {
        if (*group == dragGroup && *pos < 0 && h == hView)
            return 0;
        return copy || dragGroup < 0 ? 2 : 1;
    }
    return 0;
}

void DragMove(POINT screen, BOOL copy)
{
    int t, g, pos, a;
    char path[MAXP];

    if (!dragging)
        return;
    a = DropAction(screen, copy, &t, path, &g, &pos);
    SetCursor(a == 0 ? curDrag[2] : a == 1 ? curDrag[0] : a == 2 ? curDrag[1] : curDrag[3]);
}

static int CmpIntDesc(const void *a, const void *b)
{
    return *(const int *)b - *(const int *)a;
}

void DragEnd(POINT screen, BOOL copy, BOOL cancel)
{
    int t, g, pos, a, i;
    char path[MAXP];

    if (!dragging)
        return;
    dragging = FALSE;
    a = cancel ? 0 : DropAction(screen, copy, &t, path, &g, &pos);
    ViewSetDropHilite(-1);
    SideSetDropHilite(-1);
    SetCursor(curArrow);
    if (a == 0)
        return;
    if (dragSrc == DS_FILES) {
        if (t == DT_GROUP)
            OpsDrop(&dragList, NULL, g, TRUE, FALSE);
        else if (t == DT_WASTE)
            OpsDelete(&dragList, FALSE);
        else
            OpsCopyMove(&dragList, path, a == 1);
        PlFree(&dragList);
        return;
    }
    if (dragSrc == DS_PROGS && dragIdx) {
        GROUP *src = GroupPtr(dragGroup);

        if (!src)
            return;
        if (a == 2) {
            for (i = 0; i < nDragIdx; i++)
                if (dragIdx[i] < src->n)
                    ProgAdd(g, &src->items[dragIdx[i]], pos < 0 ? -1 : pos + i);
        } else {
            qsort(dragIdx, nDragIdx, sizeof(int), CmpIntDesc);
            for (i = 0; i < nDragIdx; i++) {
                int p = pos;

                if (g == dragGroup && pos >= 0 && dragIdx[i] < pos)
                    p = pos--;
                ProgMove(dragGroup, dragIdx[i], g, p);
            }
        }
        GroupsSave();
        SideRebuild();
        NavRefresh();
    }
}

/* ---- commands ----------------------------------------------------------------- */

static int TargetGroup(void)
{
    ITEM *it = ViewFocusItem();

    if (cur.kind == LOC_GROUP)
        return cur.group;
    if (cur.kind == LOC_PROGRAMS && it && it->kind == IK_GROUP)
        return it->ref;
    return -1;
}

static void DeleteSelection(BOOL permanent)
{
    PATHLIST pl;
    int i, n = ViewSelCount();
    char buf[160];

    if (!n)
        return;
    if (cur.kind == LOC_GROUP || cur.kind == LOC_RECENT) {
        GROUP *gr = GroupPtr(cur.kind == LOC_RECENT ? -1 : cur.group);

        if (!gr)
            return;
        if (n == 1 && ViewFocusItem())
            sprintf(buf, "Remove \"%s\" from %s?\n\nThe program itself is not deleted.",
                    ViewFocusItem()->name, cur.kind == LOC_RECENT ? "the recent list" : gr->name);
        else
            sprintf(buf, "Remove these %d programs?\n\nThe programs themselves are not deleted.", n);
        if (cur.kind != LOC_RECENT && MessageBox(hFrame, buf, "Remove Programs", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        for (i = nItems - 1; i >= 0; i--)
            if (items[i]->sel)
                ProgDelete(cur.kind == LOC_RECENT ? -1 : cur.group, items[i]->ref);
        GroupsSave();
        NavRefresh();
        return;
    }
    if (cur.kind == LOC_PROGRAMS) {
        ITEM *it = ViewFocusItem();

        if (it && it->ref >= 0) {
            cur.group = it->ref;
            Command(IDM_DELGROUP);
        }
        return;
    }
    SelectedPaths(&pl);
    OpsDelete(&pl, permanent || cur.kind == LOC_WASTE);
    PlFree(&pl);
}

static void OpenSelection(void)
{
    int i, n = 0;
    ITEM *it = ViewFocusItem();

    if (ViewSelCount() <= 1) {
        if (it)
            NavOpenItem(it);
        return;
    }
    /* several items: start the programs and documents */
    for (i = 0; i < nItems && n < 12; i++)
        if (items[i]->sel && (items[i]->kind == IK_FILE || items[i]->kind == IK_PROG)) {
            NavOpenItem(items[i]);
            n++;
        }
}

static void AddToGroup(int g)
{
    PATHLIST pl;

    SelectedPaths(&pl);
    OpsDrop(&pl, NULL, g, TRUE, FALSE);
    PlFree(&pl);
}

/* The drive of the selection or the current folder ("A"), or 0. */
static char CurrentDrive(void)
{
    ITEM *it = ViewFocusItem();

    if (it && it->kind == IK_DRIVE)
        return (char)('A' + it->ref);
    if ((cur.kind == LOC_DIR || cur.kind == LOC_SEARCH) && cur.path[1] == ':')
        return (char)(cur.path[0] & 0xDF);
    return 0;
}

/* Runs an MS-DOS command in a window and refreshes when it is started. */
static void DosCommand(const char *cmd)
{
    char line[MAXP];

#ifdef RETRO_WIN32
    sprintf(line, "%s /C %s", (GetVersion() & 0x80000000UL) ? "COMMAND.COM" : "CMD.EXE", cmd);
#else
    sprintf(line, "COMMAND.COM /C %s", cmd);
#endif
    Launch(line, NULL, SW_SHOWNORMAL);
}

/* File Manager's Disk menu. */
static void DiskCommand(int id)
{
    char drive[8], label[40], buf[MAXP], local[8];
    char d = CurrentDrive();
    UINT r;

    switch (id) {
    case IDM_COPYDISK:
        strcpy(drive, "A:");
        if (DlgInput("Copy Disk", "Copy the disk in drive (DISKCOPY copies it onto another disk "
                     "in the same drive):", drive, sizeof(drive))) {
            sprintf(buf, "DISKCOPY %.2s %.2s", drive, drive);
            DosCommand(buf);
        }
        break;
    case IDM_FORMATDISK:
        strcpy(drive, "A:");
        if (DlgInput("Format Disk", "Format the floppy disk in drive (everything on it is erased):",
                     drive, sizeof(drive))) {
            if (DriveKind((drive[0] & 0xDF) - 'A') != DK_FLOPPY) {
                Msg(MB_ICONSTOP, "Only floppy disks can be formatted from ShellIV.");
                break;
            }
            sprintf(buf, "FORMAT %.2s", drive);
            DosCommand(buf);
        }
        break;
    case IDM_LABELDISK:
        if (!d) {
            Msg(MB_ICONINFORMATION, "Open a drive or a folder on it first.");
            break;
        }
        DriveLabel(d - 'A', label, sizeof(label));
        sprintf(buf, "Volume label of drive %c: (up to 11 characters):", d);
        if (!DlgInput("Label Disk", buf, label, 12) && label[0])
            break;
#ifdef RETRO_WIN32
        {
            char root[4];

            sprintf(root, "%c:\\", d);
            if (!SetVolumeLabel(root, label))
                Msg(MB_ICONSTOP, "The label of drive %c: could not be changed.", d);
        }
#else
        AnsiUpper(label);
        sprintf(buf, "LABEL %c:%s", d, label);
        DosCommand(buf);
#endif
        SetTimer(hFrame, 6, 4000, NULL);    /* show the new label */
        break;
    case IDM_NETCONNECT:
        buf[0] = '\0';
        if (!DlgInput("Connect Network Drive", "Network path (for example \\\\SERVER\\SHARE):",
                      buf, MAXP))
            break;
        strcpy(local, "F:");
        for (local[0] = 'F'; local[0] <= 'Z' && DriveKind(local[0] - 'A') != DK_NONE; local[0]++)
            ;
        if (!DlgInput("Connect Network Drive", "Drive letter:", local, sizeof(local)))
            break;
        local[0] = (char)(local[0] & 0xDF);
        local[1] = ':';
        local[2] = '\0';
        r = (UINT)WNetAddConnection(buf, NULL, local);
        if (r)
            Msg(MB_ICONSTOP, "Cannot connect %s to %s (network error %u).", local, buf, r);
        else
            SetTimer(hFrame, 6, 500, NULL);
        break;
    case IDM_NETDISC:
        if (!d || DriveKind(d - 'A') != DK_NET) {
            Msg(MB_ICONINFORMATION, "Select a network drive in My Computer first.");
            break;
        }
        sprintf(local, "%c:", d);
        sprintf(buf, "Disconnect network drive %s?", local);
        if (MessageBox(hFrame, buf, "Disconnect Network Drive", MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
        r = (UINT)WNetCancelConnection(local, FALSE);
        if (r)
            Msg(MB_ICONSTOP, "Cannot disconnect %s (network error %u).", local, r);
        else
            SetTimer(hFrame, 6, 500, NULL);
        break;
    }
}

static void SelectPattern(void)
{
    static char pat[80] = "*.*";

    if (!DlgInput("Select Files", "Select the files matching (for example *.TXT; *.INI):", pat, sizeof(pat)))
        return;
    if (!ViewSelectPattern(pat))
        MessageBeep(0);
}

static void Shortcuts(void)
{
    MessageBox(hFrame,
               "Enter\tOpen the selection\n"
               "Backspace\tUp one level\n"
               "Alt+Left / Right\tBack and forward\n"
               "F2\tRename\n"
               "Del / Shift+Del\tTo the Wastebasket / delete\n"
               "Alt+Enter\tProperties\n"
               "Ctrl+X, C, V\tCut, copy and paste files\n"
               "Ctrl+A\tSelect all\n"
               "Ctrl+F\tFind files\n"
               "Ctrl+L\tType an address\n"
               "Ctrl+D\tAdd to Favorites\n"
               "Ctrl+R\tRun\n"
               "Ctrl+T\tRunning tasks (Enter switches)\n"
               "Ctrl+1, 2, 3\tIcons, tiles, details\n"
               "F3\tPreview pane\n"
               "F5\tRefresh\n"
               "F6\tNext pane\n"
               "Type letters\tJump to a name\n\n"
               "Drag with the right mouse button or hold Ctrl to copy.",
               "ShellIV Keyboard Shortcuts", MB_OK | MB_ICONINFORMATION);
}

void SaveSettings(void)
{
    WINDOWPLACEMENT wp;
    char buf[80];

    wp.length = sizeof(wp);
    if (GetWindowPlacement(hFrame, &wp)) {
        sprintf(buf, "%d,%d,%d,%d,%d", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                wp.rcNormalPosition.right, wp.rcNormalPosition.bottom, wp.showCmd == SW_SHOWMAXIMIZED);
        IniPut("Settings", "Window", buf);
    }
    IniPutInt("Settings", "Theme", themeIdx);
    IniPutInt("Settings", "View", viewMode);
    IniPutInt("Settings", "Sidebar", sideW);
    IniPutInt("Settings", "PreviewWidth", prevW);
    IniPutInt("Settings", "Preview", showPreview);
    IniPutInt("Settings", "ConfirmDelete", confirmDelete);
    IniPutInt("Settings", "ConfirmReplace", confirmOps);
    IniPutInt("Settings", "MinimizeOnUse", minimizeOnUse);
    IniPutInt("Settings", "SaveOnExit", saveOnExit);
    IniPutInt("Settings", "ShowHidden", showHidden);
    IniPutInt("Settings", "SortColumn", sortCol);
    IniPutInt("Settings", "SortDescending", sortDesc);
    IniPutInt("Settings", "Location", cur.kind);
    IniPutInt("Settings", "Group", cur.group);
    IniPut("Settings", "Path", cur.kind == LOC_DIR ? cur.path : "");
    SideSaveState();
}

static void ExitWindowsNow(void)
{
#ifdef RETRO_WIN32
    ExitWindowsEx(EWX_LOGOFF, 0);
#else
    ExitWindows(0, 0);
#endif
}

void Command(int id)
{
    PATHLIST pl;
    char buf[MAXP];
    int g, i;

    if (OpsBusy())
        return;
    if (id >= IDM_THEME0 && id < IDM_THEME0 + nThemes) {
        ThemeSet(id - IDM_THEME0);
        IniPutInt("Settings", "Theme", themeIdx);
        InvalidateRect(hFrame, NULL, FALSE);
        InvalidateRect(hSide, NULL, FALSE);
        InvalidateRect(hView, NULL, FALSE);
        InvalidateRect(hPreview, NULL, FALSE);
        {
            char t[60];

            sprintf(t, "Theme: %s", ThemeName(themeIdx));
            Status(t);
        }
        return;
    }
    if (id >= IDM_ADDGRP0 && id < IDM_ADDGRP0 + MAXGROUPS) {
        AddToGroup(id - IDM_ADDGRP0);
        return;
    }
    switch (id) {
    case IDM_OPEN:       OpenSelection(); break;
    case IDM_RUN:        DlgRun(NULL); break;
    case IDM_NEWFOLDER:
        if (cur.kind == LOC_GROUP)
            Command(IDM_NEWPROG);
        else if (cur.kind == LOC_PROGRAMS)
            Command(IDM_NEWGROUP);
        else
            OpsNewFolder();
        break;
    case IDM_NEWGROUP:
        buf[0] = '\0';
        if (DlgInput("New Group", "Name of the new program group:", buf, 64)) {
            g = GroupAdd(buf);
            if (g >= 0) {
                GroupsSave();
                SideRebuild();
                NavKind(LOC_GROUP, g);
            }
        }
        break;
    case IDM_NEWPROG: {
        PROG p;

        g = TargetGroup();
        if (g < 0) {
            Msg(MB_ICONINFORMATION, "Open the program group that should hold the new program first.");
            break;
        }
        memset(&p, 0, sizeof(p));
        if (DlgProgram(&p, TRUE)) {
            i = ProgAdd(g, &p, -1);
            GroupsSave();
            SideRebuild();
            if (cur.kind == LOC_GROUP)
                NavRefresh();
            if (i >= 0)
                ViewSelectName(p.name);
        }
        ProgFree(&p);
        break;
    }
    case IDM_RENAME:     ViewRename(); break;
    case IDM_DELETE:     DeleteSelection(FALSE); break;
    case IDM_DELPERM:    DeleteSelection(TRUE); break;
    case IDM_PROPS:      DlgProperties(); break;
    case IDM_PROPS + 10000:
        for (i = 0; i < nItems; i++)
            items[i]->sel = 0;
        DlgProperties();
        break;
    case IDM_COPYTO:
    case IDM_MOVETO:
        SelectedPaths(&pl);
        if (pl.n) {
            lstrcpyn(buf, cur.kind == LOC_DIR ? cur.path : "C:\\", MAXP);
            if (DlgInput(id == IDM_COPYTO ? "Copy To" : "Move To",
                         "Destination folder:", buf, MAXP)) {
                PathNormalize(buf);
                OpsCopyMove(&pl, buf, id == IDM_MOVETO);
            }
        }
        PlFree(&pl);
        break;
    case IDM_EXIT:       PostMessage(hFrame, WM_CLOSE, 0, 0); break;
    case IDM_EXITWIN:
        if (MessageBox(hFrame, "This will end your Windows session.", "Exit Windows",
                       MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
            SaveSettings();
            GroupsSave();
            ExitWindowsNow();
        }
        break;
    case IDM_DOSPROMPT:
#ifdef RETRO_WIN32
        if (GetVersion() & 0x80000000UL)
            Launch("COMMAND.COM", cur.kind == LOC_DIR ? cur.path : NULL, SW_SHOWNORMAL);
        else
            Launch("CMD.EXE", cur.kind == LOC_DIR ? cur.path : NULL, SW_SHOWNORMAL);
#else
        {
            char pif[MAXP], win[MAXP];

            GetWindowsDirectory(win, MAXP);
            PathJoin(pif, win, "DOSPRMPT.PIF");
            Launch(Exists(pif) ? pif : "COMMAND.COM", cur.kind == LOC_DIR ? cur.path : NULL,
                   SW_SHOWNORMAL);
        }
#endif
        break;
    case IDM_PRINT:
        for (i = 0; i < nItems; i++)
            if (items[i]->sel && items[i]->kind == IK_FILE) {
                char dir[MAXP];
                UINT r;

                ViewItemPath(items[i], buf);
                lstrcpyn(dir, buf, MAXP);
                PathParent(dir);
                r = (UINT)ShellExecute(hFrame, "print", buf, NULL, dir, SW_SHOWMINIMIZED);
                if (r <= 32) {
                    Msg(MB_ICONEXCLAMATION, "\"%s\" cannot be printed: no program is "
                        "associated with it for printing.", items[i]->name);
                    break;
                }
            }
        break;
    case IDM_ASSOCIATE: {
        ITEM *it = ViewFocusItem();
        char ext[16], prompt[120];
        const char *e;

        if (!it || it->kind != IK_FILE || !*(e = PathExt(it->name))) {
            Msg(MB_ICONINFORMATION, "Select a file with an extension first.");
            break;
        }
        lstrcpyn(ext, e + 1, sizeof(ext));
        AnsiLower(ext);
        GetProfileString("Extensions", ext, "", buf, MAXP);
        if (strchr(buf, ' '))
            *strchr(buf, ' ') = '\0';
        sprintf(prompt, "Open .%s files with this program (for example NOTEPAD.EXE):", ext);
        if (DlgInput("Associate", prompt, buf, MAXP - 20)) {
            sprintf(buf + strlen(buf), " ^.%s", ext);
            WriteProfileString("Extensions", ext, buf);
            IconsFlush();
            NavRefresh();
            sprintf(prompt, ".%s files now open with %s", ext, buf);
            *strstr(prompt, " ^.") = '\0';
            Status(prompt);
        }
        break;
    }
    case IDM_COPYDISK:
    case IDM_FORMATDISK:
    case IDM_LABELDISK:
    case IDM_NETCONNECT:
    case IDM_NETDISC:
        DiskCommand(id);
        break;
    case IDM_CUT:        OpsClipboard(TRUE); break;
    case IDM_COPY:       OpsClipboard(FALSE); break;
    case IDM_PASTE:      OpsPaste(); break;
    case IDM_SELALL:     ViewSelectAll(); break;
    case IDM_INVERTSEL:
        for (i = 0; i < nItems; i++)
            items[i]->sel = (BYTE)!items[i]->sel;
        ViewInvalidate();
        SelectionChanged();
        break;
    case IDM_SELPAT:     SelectPattern(); break;
    case IDM_VICONS:     ViewSetMode(VM_ICONS); UpdateUI(); break;
    case IDM_VTILES:     ViewSetMode(VM_TILES); UpdateUI(); break;
    case IDM_VDETAILS:   ViewSetMode(VM_DETAILS); UpdateUI(); break;
    case IDM_SORTNAME:   ViewSort(0); break;
    case IDM_SORTSIZE:   ViewSort(1); break;
    case IDM_SORTTYPE:   ViewSort(2); break;
    case IDM_SORTDATE:   ViewSort(3); break;
    case IDM_PREVIEW:
        showPreview = !showPreview;
        Layout();
        if (showPreview)
            PreviewUpdate();
        UpdateUI();
        break;
    case IDM_HIDDEN:
        showHidden = !showHidden;
        SideRebuild();
        NavRefresh();
        break;
    case IDM_REFRESH:
        IconsFlush();
        SideRefreshDrives();
        SideRebuild();
        NavRefresh();
        break;
    case IDM_BACK:       NavBack(); break;
    case IDM_FORWARD:    NavForward(); break;
    case IDM_UP:         NavUp(); break;
    case IDM_COMPUTER:   NavKind(LOC_COMPUTER, 0); break;
    case IDM_PROGRAMS:   NavKind(LOC_PROGRAMS, 0); break;
    case IDM_RECENT:     NavKind(LOC_RECENT, -1); break;
    case IDM_WASTE:      NavKind(LOC_WASTE, 0); break;
    case IDM_GOTO:
        SetFocus(hAddr);
        SendMessage(hAddr, EM_SETSEL, 0, MAKELONG(0, -1));
#ifdef RETRO_WIN32
        SendMessage(hAddr, EM_SETSEL, 0, -1);
#endif
        break;
    case IDM_ADDFAV: {
        ITEM *it = ViewFocusItem();

        if (it && it->kind == IK_DIR && ViewSelCount() == 1)
            ViewItemPath(it, buf);
        else if (cur.kind == LOC_DIR)
            lstrcpyn(buf, cur.path, MAXP);
        else
            break;
        FavAdd(buf);
        GroupsSave();
        SideRebuild();
        SideSync();
        sprintf(buf + strlen(buf), " added to Favorites.");
        Status(buf);
        break;
    }
    case IDM_FIND:       DlgFind(); break;
    case IDM_RENGROUP:
        g = TargetGroup();
        if (g >= 0) {
            lstrcpyn(buf, groups[g].name, 64);
            if (DlgInput("Rename Group", "New name of the group:", buf, 64)) {
                GroupRename(g, buf);
                GroupsSave();
                SideRebuild();
                NavRefresh();
                UpdateTitle();
            }
        }
        break;
    case IDM_DELGROUP:
        g = cur.kind == LOC_PROGRAMS || cur.kind == LOC_GROUP ? cur.group : -1;
        if (cur.kind == LOC_PROGRAMS && ViewFocusItem() && ViewFocusItem()->kind == IK_GROUP)
            g = ViewFocusItem()->ref;
        if (g >= 0 && g < nGroups) {
            sprintf(buf, "Delete the group \"%s\" and its %d program item%s?\n\n"
                    "The programs themselves are not deleted.", groups[g].name, groups[g].n,
                    groups[g].n == 1 ? "" : "s");
            if (MessageBox(hFrame, buf, "Delete Group", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                GroupDelete(g);
                GroupsSave();
                SideRebuild();
                NavKind(LOC_PROGRAMS, 0);
            }
        }
        break;
    case IDM_IMPORT:
        if (ImportProgman(FALSE)) {
            GroupsSave();
            SideRebuild();
            NavKind(LOC_PROGRAMS, 0);
        }
        break;
    case IDM_OPTIONS:    DlgOptions(); break;
    case IDM_EMPTYWASTE: WasteEmpty(); break;
    case IDM_RESTORE:
        if (cur.kind == LOC_WASTE) {
            SelectedPaths(&pl);
            if (pl.n)
                WasteRestore(&pl);
            PlFree(&pl);
        }
        break;
    case IDM_ABOUT:      DlgAbout(); break;
    case IDM_KEYS:       Shortcuts(); break;
    case IDM_FOCUSSIDE:  SideFocusNode(); break;
    case IDM_TASKS:      SideFocusTasks(); break;
    case IDM_NEXTPANE: {
        HWND f = GetFocus();

        if (f == hView)
            SideFocusNode();
        else if (f == hSide)
            Command(IDM_GOTO);
        else
            FocusView();
        break;
    }
    }
}

/* ---- menus ------------------------------------------------------------------- */

static void InitMenu(HMENU m)
{
    int n = ViewSelCount(), i;
    ITEM *it = ViewFocusItem();
    BOOL files = LocIsFiles() && n > 0;

#define EN(id, c) EnableMenuItem(m, id, MF_BYCOMMAND | ((c) ? MF_ENABLED : MF_GRAYED))
#define CK(id, c) CheckMenuItem(m, id, MF_BYCOMMAND | ((c) ? MF_CHECKED : MF_UNCHECKED))
    EN(IDM_OPEN, it != NULL);
    EN(IDM_PRINT, it && it->kind == IK_FILE);
    EN(IDM_ASSOCIATE, it && it->kind == IK_FILE);
    EN(IDM_NEWFOLDER, cur.kind == LOC_DIR);
    EN(IDM_NEWPROG, TargetGroup() >= 0);
    EN(IDM_RENAME, it && (it->kind == IK_FILE || it->kind == IK_DIR ||
                          (it->kind == IK_PROG && cur.kind == LOC_GROUP) ||
                          (it->kind == IK_GROUP && it->ref >= 0)));
    EN(IDM_DELETE, n > 0 && cur.kind != LOC_COMPUTER);
    EN(IDM_DELPERM, files || (cur.kind == LOC_WASTE && n > 0));
    EN(IDM_PROPS, n > 0 || cur.kind == LOC_DIR);
    EN(IDM_COPYTO, files);
    EN(IDM_MOVETO, files);
    EN(IDM_CUT, files);
    EN(IDM_COPY, files);
    EN(IDM_PASTE, OpsCanPaste());
    EN(IDM_ADDTOGROUP, files);
    EN(IDM_SELALL, nItems > 0);
    EN(IDM_INVERTSEL, nItems > 0);
    EN(IDM_SELPAT, nItems > 0);
    EN(IDM_BACK, NavCanBack());
    EN(IDM_FORWARD, NavCanForward());
    EN(IDM_UP, NavCanUp());
    EN(IDM_ADDFAV, cur.kind == LOC_DIR);
    EN(IDM_RENGROUP, TargetGroup() >= 0);
    EN(IDM_DELGROUP, TargetGroup() >= 0);
    EN(IDM_RESTORE, cur.kind == LOC_WASTE && n > 0);
    EN(IDM_SORTNAME, cur.kind == LOC_DIR || cur.kind == LOC_SEARCH || cur.kind == LOC_WASTE);
    EN(IDM_SORTSIZE, cur.kind == LOC_DIR || cur.kind == LOC_SEARCH || cur.kind == LOC_WASTE);
    EN(IDM_SORTTYPE, cur.kind == LOC_DIR || cur.kind == LOC_SEARCH || cur.kind == LOC_WASTE);
    EN(IDM_SORTDATE, cur.kind == LOC_DIR || cur.kind == LOC_SEARCH || cur.kind == LOC_WASTE);
    CK(IDM_VICONS, viewMode == VM_ICONS);
    CK(IDM_VTILES, viewMode == VM_TILES);
    CK(IDM_VDETAILS, viewMode == VM_DETAILS);
    CK(IDM_SORTNAME, sortCol == 0);
    CK(IDM_SORTSIZE, sortCol == 1);
    CK(IDM_SORTTYPE, sortCol == 2);
    CK(IDM_SORTDATE, sortCol == 3);
    CK(IDM_PREVIEW, showPreview);
    CK(IDM_HIDDEN, showHidden);
    for (i = 0; i < nThemes; i++)
        CK(IDM_THEME0 + i, i == themeIdx);
#ifdef RETRO_WIN32
    EN(IDM_EXITWIN, TRUE);
#endif
#undef EN
#undef CK
}

static HMENU GroupsSubmenu(void)
{
    HMENU sub = CreatePopupMenu();
    int g;

    for (g = 0; g < nGroups; g++)
        AppendMenu(sub, MF_STRING, IDM_ADDGRP0 + g, groups[g].name);
    return sub;
}

void ContextMenu(HWND from, POINT pt, BOOL onItem)
{
    HMENU m = CreatePopupMenu(), sub;
    ITEM *it = ViewFocusItem();

    (void)from;
    if (onItem && it) {
        switch (it->kind) {
        case IK_FILE:
        case IK_DIR:
            AppendMenu(m, MF_STRING, IDM_OPEN, it->kind == IK_DIR ? "&Open" : "&Open / Run");
            if (it->kind == IK_FILE) {
                AppendMenu(m, MF_STRING, IDM_PRINT, "&Print");
                AppendMenu(m, MF_STRING, IDM_ASSOCIATE, "&Associate...");
            }
            if (it->kind == IK_FILE && nGroups)
                AppendMenu(m, MF_POPUP, (UINT)GroupsSubmenu(), "Add to &Group");
            if (it->kind == IK_DIR)
                AppendMenu(m, MF_STRING, IDM_ADDFAV, "Add to &Favorites");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_CUT, "Cu&t");
            AppendMenu(m, MF_STRING, IDM_COPY, "&Copy");
            AppendMenu(m, MF_STRING, IDM_COPYTO, "Cop&y To...");
            AppendMenu(m, MF_STRING, IDM_MOVETO, "Mo&ve To...");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_DELETE, "&Delete");
            AppendMenu(m, MF_STRING, IDM_RENAME, "Rena&me");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_PROPS, "P&roperties");
            break;
        case IK_PROG:
            AppendMenu(m, MF_STRING, IDM_OPEN, "&Run");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, cur.kind == LOC_GROUP ? MF_STRING : MF_GRAYED, IDM_RENAME, "Rena&me");
            AppendMenu(m, MF_STRING, IDM_DELETE, cur.kind == LOC_RECENT ? "Remove from &List" : "&Remove");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, cur.kind == LOC_GROUP ? MF_STRING : MF_GRAYED, IDM_PROPS, "P&roperties");
            break;
        case IK_GROUP:
            AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
            if (it->ref >= 0) {
                AppendMenu(m, MF_STRING, IDM_RENAME, "Rena&me");
                AppendMenu(m, MF_STRING, IDM_DELETE, "&Delete Group");
            }
            break;
        case IK_DRIVE:
            AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
            AppendMenu(m, MF_STRING, IDM_FIND, "&Find Files...");
            break;
        case IK_WASTE:
            AppendMenu(m, MF_STRING, IDM_RESTORE, "&Restore");
            AppendMenu(m, MF_STRING, IDM_DELETE, "&Delete Permanently");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_PROPS, "P&roperties");
            break;
        }
    } else {
        sub = CreatePopupMenu();
        AppendMenu(sub, MF_STRING | (viewMode == VM_ICONS ? MF_CHECKED : 0), IDM_VICONS, "&Icons");
        AppendMenu(sub, MF_STRING | (viewMode == VM_TILES ? MF_CHECKED : 0), IDM_VTILES, "&Tiles");
        AppendMenu(sub, MF_STRING | (viewMode == VM_DETAILS ? MF_CHECKED : 0), IDM_VDETAILS, "&Details");
        AppendMenu(m, MF_POPUP, (UINT)sub, "&View");
        if (LocIsFiles() || cur.kind == LOC_WASTE) {
            sub = CreatePopupMenu();
            AppendMenu(sub, MF_STRING, IDM_SORTNAME, "by &Name");
            AppendMenu(sub, MF_STRING, IDM_SORTTYPE, "by &Type");
            AppendMenu(sub, MF_STRING, IDM_SORTSIZE, "by &Size");
            AppendMenu(sub, MF_STRING, IDM_SORTDATE, "by &Date");
            AppendMenu(m, MF_POPUP, (UINT)sub, "&Sort");
        }
        AppendMenu(m, MF_STRING, IDM_REFRESH, "R&efresh");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        switch (cur.kind) {
        case LOC_DIR:
            AppendMenu(m, OpsCanPaste() ? MF_STRING : MF_GRAYED, IDM_PASTE, "&Paste");
            AppendMenu(m, MF_STRING, IDM_NEWFOLDER, "New &Folder");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_FIND, "F&ind Files Here...");
            AppendMenu(m, MF_STRING, IDM_DOSPROMPT, "MS-DOS Pro&mpt Here");
            AppendMenu(m, MF_STRING, IDM_ADDFAV, "Add to F&avorites");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_PROPS, "P&roperties");
            break;
        case LOC_GROUP:
            AppendMenu(m, MF_STRING, IDM_NEWPROG, "New &Program...");
            AppendMenu(m, OpsCanPaste() ? MF_STRING : MF_GRAYED, IDM_PASTE, "Paste as Pro&grams");
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_RENGROUP, "Rena&me Group...");
            AppendMenu(m, MF_STRING, IDM_DELGROUP, "&Delete Group");
            break;
        case LOC_PROGRAMS:
            AppendMenu(m, MF_STRING, IDM_NEWGROUP, "New &Group...");
            AppendMenu(m, MF_STRING, IDM_IMPORT, "&Import Program Manager Groups");
            break;
        case LOC_WASTE:
            AppendMenu(m, nWaste ? MF_STRING : MF_GRAYED, IDM_EMPTYWASTE, "&Empty Wastebasket");
            break;
        default:
            AppendMenu(m, MF_STRING, IDM_FIND, "F&ind Files...");
        }
    }
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hFrame, NULL);
    DestroyMenu(m);
}

/* ---- address and search fields ------------------------------------------------ */

static void AddressGo(void)
{
    char text[MAXP], path[MAXP];
    int g;
    const char *s;

    GetWindowText(hAddr, text, MAXP);
    s = text;
    while (*s == ' ')
        s++;
    if (!*s)
        return;
    if (!lstrcmpi(s, "My Computer")) { NavKind(LOC_COMPUTER, 0); FocusView(); return; }
    if (!lstrcmpi(s, "Programs")) { NavKind(LOC_PROGRAMS, 0); FocusView(); return; }
    if (!lstrcmpi(s, "Recent Programs")) { NavKind(LOC_RECENT, -1); FocusView(); return; }
    if (!lstrcmpi(s, "Wastebasket")) { NavKind(LOC_WASTE, 0); FocusView(); return; }
    {
        BOOL prefixed = !strnicmp(s, "Programs\\", 9);
        const char *name = prefixed ? s + 9 : s;

        for (g = 0; g < nGroups; g++)
            if (!lstrcmpi(name, groups[g].name)) {
                NavKind(LOC_GROUP, g);
                FocusView();
                return;
            }
        if (prefixed) {
            Msg(MB_ICONINFORMATION, "There is no program group named \"%s\".", name);
            return;
        }
    }
    lstrcpyn(path, s, MAXP);
    PathNormalize(path);
    if (path[1] == ':' && (IsDir(path) || PathIsRoot(path))) {
        NavDir(path);
        FocusView();
        return;
    }
    if (cur.kind == LOC_DIR && !strchr(path, ':') && !strchr(path, '\\')) {
        char rel[MAXP];

        PathJoin(rel, cur.path, s);
        if (IsDir(rel)) {
            NavDir(rel);
            FocusView();
            return;
        }
    }
    /* not a place: run it */
    if (Launch(s, cur.kind == LOC_DIR ? cur.path : NULL, SW_SHOWNORMAL))
        UpdateTitle();
}

static void FindGo(void)
{
    char text[80];

    GetWindowText(hFind, text, sizeof(text));
    if (!text[0])
        return;
    NavSearch(cur.kind == LOC_DIR || cur.kind == LOC_SEARCH ? cur.path : "C:\\", text, "", TRUE);
    FocusView();
}

/* Keys for the edit fields: Enter, Escape and Tab. */
static BOOL FieldKey(MSG *msg)
{
    if (msg->message != WM_KEYDOWN && msg->message != WM_CHAR)
        return FALSE;
    if (msg->hwnd == hRename) {
        if (msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN || msg->wParam == VK_ESCAPE)) {
            ViewEndRename(msg->wParam == VK_RETURN);
            return TRUE;
        }
        return msg->message == WM_CHAR && (msg->wParam == '\r' || msg->wParam == 27);
    }
    if (msg->hwnd != hAddr && msg->hwnd != hFind)
        return FALSE;
    if (msg->message == WM_CHAR)
        return msg->wParam == '\r' || msg->wParam == 27 || msg->wParam == '\t';
    switch (msg->wParam) {
    case VK_RETURN:
        if (msg->hwnd == hAddr)
            AddressGo();
        else
            FindGo();
        return TRUE;
    case VK_ESCAPE:
        UpdateTitle();
        FocusView();
        return TRUE;
    case VK_TAB:
        if (msg->hwnd == hAddr && GetKeyState(VK_SHIFT) >= 0)
            SetFocus(hFind);
        else
            FocusView();
        return TRUE;
    }
    return FALSE;
}

/* ---- the frame window ------------------------------------------------------- */

static void SetClock(void)
{
#ifdef RETRO_WIN32
    SYSTEMTIME st;

    GetLocalTime(&st);
    sprintf(clockText, "%02d:%02d", st.wHour, st.wMinute);
#else
    struct dostime_t t;

    _dos_gettime(&t);
    sprintf(clockText, "%02d:%02d", t.hour, t.minute);
#endif
}

LRESULT CALLBACK __export FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    POINT pt;
    int i;

    switch (msg) {
    case WM_CREATE:
        hFrame = hwnd;
        SetTimer(hwnd, TIMER_CLOCK, 2000, NULL);
        SetClock();
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO XFAR *mm = (MINMAXINFO XFAR *)LP2PTR(lParam);

        mm->ptMinTrackSize.x = 480;
        mm->ptMinTrackSize.y = 320;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        BufferedPaint(hwnd, PaintFrame);
        return 0;

    case WM_SETFOCUS:
        FocusView();
        return 0;

    case WM_SETCURSOR:
        if (busyCount) {
            SetCursor(curWait);
            return TRUE;
        }
        if (dragging)
            return TRUE;
        if ((HWND)wParam == hwnd && LOWORD(lParam) == HTCLIENT) {
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (SplitAt(pt.x, pt.y)) {
                SetCursor(curSizeWE);
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN:
        pt.x = MOUSE_X(lParam);
        pt.y = MOUSE_Y(lParam);
        splitting = SplitAt(pt.x, pt.y);
        if (splitting) {
            SetCapture(hwnd);
            return 0;
        }
        i = BtnAt(pt);
        if (i >= 0 && BtnEnabled(tb[i].cmd)) {
            pressBtn = i;
            SetCapture(hwnd);
            InvalidateToolbar();
        }
        return 0;

    case WM_MOUSEMOVE:
        pt.x = MOUSE_X(lParam);
        pt.y = MOUSE_Y(lParam);
        if (splitting) {
            RECT cl;

            GetClientRect(hwnd, &cl);
            if (splitting == 1)
                sideW = max(120, min(pt.x, cl.right / 2));
            else
                prevW = max(150, min(cl.right - pt.x, cl.right / 3));
            Layout();
            UpdateWindow(hwnd);
            return 0;
        }
        i = BtnAt(pt);
        if (i != hotBtn) {
            hotBtn = i;
            InvalidateToolbar();
            if (i >= 0 && tb[i].hint)
                Status(tb[i].hint);
            if (!hotTimer) {
                SetTimer(hwnd, 3, 150, NULL);
                hotTimer = TRUE;
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (splitting) {
            splitting = 0;
            ReleaseCapture();
            return 0;
        }
        if (pressBtn >= 0) {
            int b = pressBtn;

            pressBtn = -1;
            ReleaseCapture();
            InvalidateToolbar();
            pt.x = MOUSE_X(lParam);
            pt.y = MOUSE_Y(lParam);
            if (BtnAt(pt) == b)
                Command(tb[b].cmd);
        }
        return 0;

    case WM_RBUTTONUP:
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_CLOCK) {
            char old[16];
            static int ticks;
            RECT r;

            strcpy(old, clockText);
            SetClock();
            StatusRect(&r);
            if (strcmp(old, clockText) != 0 || (++ticks % 3) == 0)
                InvalidateRect(hwnd, &r, FALSE);
            if (statusMsg[0] && GetTickCount() - statusTime > 6000) {
                statusMsg[0] = '\0';
                InvalidateRect(hwnd, &r, FALSE);
            }
            SideTasksChanged();
        } else if (wParam == 6) {          /* drives changed: label, network */
            KillTimer(hwnd, 6);
            SideRebuildDrives();
            if (cur.kind == LOC_COMPUTER || cur.kind == LOC_DIR)
                NavRefresh();
        } else if (wParam == 4 || wParam == 5) {
            KillTimer(hwnd, wParam);
            SideTasksChanged();
        } else if (wParam == 3) {
            GetCursorPos(&pt);
            if (WindowFromPoint(pt) != hwnd) {
                KillTimer(hwnd, 3);
                hotTimer = FALSE;
                if (hotBtn >= 0) {
                    hotBtn = -1;
                    InvalidateToolbar();
                }
            }
        }
        return 0;

#ifdef RETRO_WIN32
    case WM_HOTKEY: {
        int id = (int)wParam, g, i;

        for (g = 0; g < nGroups; g++)
            for (i = 0; i < groups[g].n; i++)
                if (LOBYTE(groups[g].items[i].hotkey) && --id == 0) {
                    LaunchProg(&groups[g].items[i]);
                    return 0;
                }
        return 0;
    }
#endif

    case WM_QUERYNEWPALETTE:
        if (PalRealize(hwnd)) {
            InvalidateRect(hwnd, NULL, FALSE);
            InvalidateRect(hSide, NULL, FALSE);
            InvalidateRect(hView, NULL, FALSE);
            InvalidateRect(hPreview, NULL, FALSE);
        }
        return TRUE;

    case WM_PALETTECHANGED:
        if ((HWND)wParam != hwnd && !IsChild(hwnd, (HWND)wParam) && PalRealize(hwnd)) {
            InvalidateRect(hwnd, NULL, FALSE);
            InvalidateRect(hSide, NULL, FALSE);
            InvalidateRect(hView, NULL, FALSE);
            InvalidateRect(hPreview, NULL, FALSE);
        }
        return 0;

    case WM_SELCHANGED:
        selPosted = FALSE;
        PreviewUpdate();
        UpdateUI();
        return 0;

    case WM_INITMENUPOPUP:
        InitMenu((HMENU)wParam);
        return 0;

    case WM_MENUSELECT: {
        UINT id = MENUSEL_ITEM(wParam);
        char hint[160];

        if (id && !(MENUSEL_FLAGS(wParam, lParam) & MF_POPUP)
            && LoadString(hInst, id, hint, sizeof(hint)) > 0)
            Status(hint);
        return 0;
    }

    case WM_COMMAND:
        if (CMD_HWND(wParam, lParam) && CMD_HWND(wParam, lParam) == hAddr)
            return 0;
        Command(CMD_ID(wParam, lParam));
        return 0;

    case WM_ACTIVATEAPP:
        if (wParam && hView)
            SideTasksChanged();
        return 0;

    case WM_QUERYENDSESSION:
        if (saveOnExit)
            SaveSettings();
        GroupsSave();
        return TRUE;

    case WM_CLOSE:
        if (isShell) {
            if (MessageBox(hwnd, "Closing ShellIV ends your Windows session.", "Exit Windows",
                           MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                return 0;
            if (saveOnExit)
                SaveSettings();
            GroupsSave();
            ExitWindowsNow();
            return 0;
        }
        if (saveOnExit)
            SaveSettings();
        GroupsSave();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_CLOCK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ---- start-up --------------------------------------------------------------------- */

extern LRESULT CALLBACK __export BarProc(HWND, UINT, WPARAM, LPARAM);

static BOOL RegisterClasses(void)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = (WNDPROC)FrameProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP));
    wc.hCursor = NULL;
    wc.lpszMenuName = MAKEINTRESOURCE(IDR_MENU);
    wc.lpszClassName = "ShellIVFrame";
    if (!RegisterClass(&wc))
        return FALSE;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = (WNDPROC)BarProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ShellIVBar";
    if (!RegisterClass(&wc))
        return FALSE;
    return ViewRegister() && SideRegister() && PreviewRegister();
}

static void LoadSettings(void)
{
    viewMode = IniInt("Settings", "View", VM_ICONS);
    if (viewMode < 0 || viewMode > VM_DETAILS)
        viewMode = VM_ICONS;
    sideW = IniInt("Settings", "Sidebar", 210);
    prevW = IniInt("Settings", "PreviewWidth", 230);
    showPreview = IniInt("Settings", "Preview", 1) != 0;
    confirmDelete = IniInt("Settings", "ConfirmDelete", 1) != 0;
    confirmOps = IniInt("Settings", "ConfirmReplace", 1) != 0;
    minimizeOnUse = IniInt("Settings", "MinimizeOnUse", 0) != 0;
    saveOnExit = IniInt("Settings", "SaveOnExit", 1) != 0;
    showHidden = IniInt("Settings", "ShowHidden", 0) != 0;
    sortCol = IniInt("Settings", "SortColumn", 0);
    sortDesc = IniInt("Settings", "SortDescending", 0);
}

static void RunStartup(void)
{
    int g, i;

    for (g = 0; g < nGroups; g++)
        if (!lstrcmpi(groups[g].name, "StartUp"))
            for (i = 0; i < groups[g].n; i++)
                Launch(groups[g].items[i].cmd, groups[g].items[i].dir, SW_SHOWMINNOACTIVE);
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    MSG msg;
    HACCEL accel;
    char buf[MAXP], me[MAXP];
    int x, y, w, h, maxed = 0, sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    LOCATION start;

    hInst = inst;
#ifndef RETRO_WIN32
    if (prev) {
        HWND other = FindWindow("ShellIVFrame", NULL);

        if (other) {
            if (IsIconic(other))
                ShowWindow(other, SW_RESTORE);
            BringWindowToTop(other);
        }
        return 0;
    }
#else
    (void)prev;
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    curWait = LoadCursor(NULL, IDC_WAIT);
    curArrow = LoadCursor(NULL, IDC_ARROW);
    curSizeWE = LoadCursor(NULL, IDC_SIZEWE);
    curDrag[0] = LoadCursor(inst, MAKEINTRESOURCE(IDC_DRAGMOVE));
    curDrag[1] = LoadCursor(inst, MAKEINTRESOURCE(IDC_DRAGCOPY));
    curDrag[2] = LoadCursor(inst, MAKEINTRESOURCE(IDC_DRAGNO));
    curDrag[3] = LoadCursor(inst, MAKEINTRESOURCE(IDC_DRAGLINK));
    if (!RegisterClasses())
        return 1;
    ThemeInit();
    NavInit();
    LoadSettings();
    SideLoadState();
    GroupsLoad();

    GetPrivateProfileString("boot", "shell", "", buf, MAXP, "SYSTEM.INI");
    GetModuleFileName(inst, me, MAXP);
    isShell = buf[0] && lstrcmpi(PathName(buf), PathName(me)) == 0;

    w = sw * 7 / 8;
    h = sh * 7 / 8;
    x = (sw - w) / 2;
    y = (sh - h) / 2;
    IniGet("Settings", "Window", "", buf, sizeof(buf));
    if (buf[0]) {
        int l, t, r, b;

        if (sscanf(buf, "%d,%d,%d,%d,%d", &l, &t, &r, &b, &maxed) == 5 && r - l >= 480 && b - t >= 320
            && l < sw - 40 && t < sh - 40) {
            x = l;
            y = t;
            w = r - l;
            h = b - t;
        }
    }
    hFrame = CreateWindow("ShellIVFrame", APPNAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          x, y, w, h, NULL, NULL, inst, NULL);
    if (!hFrame)
        return 1;
    CreateWindow("ShellIVSide", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
                 0, 0, 0, 0, hFrame, (HMENU)10, inst, NULL);
    CreateWindow("ShellIVView", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                 0, 0, 0, 0, hFrame, (HMENU)11, inst, NULL);
    CreateWindow("ShellIVPreview", NULL, WS_CHILD | WS_CLIPSIBLINGS,
                 0, 0, 0, 0, hFrame, (HMENU)12, inst, NULL);
    hAddr = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                         0, 0, 0, 0, hFrame, (HMENU)13, inst, NULL);
    hFind = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                         0, 0, 0, 0, hFrame, (HMENU)14, inst, NULL);
    SendMessage(hAddr, WM_SETFONT, (WPARAM)fontUI, 0);
    SendMessage(hFind, WM_SETFONT, (WPARAM)fontUI, 0);
    SendMessage(hFind, EM_LIMITTEXT, 70, 0);
    DragAcceptFiles(hSide, TRUE);
    HotkeysRegister();
    SideRebuild();
    Layout();

    /* the start location: a folder on the command line or the last one */
    memset(&start, 0, sizeof(start));
    while (*cmdline == ' ' || *cmdline == '"')
        cmdline++;
    lstrcpyn(buf, cmdline, MAXP);
    x = strlen(buf);
    while (x > 0 && (buf[x - 1] == ' ' || buf[x - 1] == '"'))
        buf[--x] = '\0';
    if (buf[0] && IsDir(buf)) {
        start.kind = LOC_DIR;
        lstrcpyn(start.path, buf, MAXP);
    } else {
        start.kind = IniInt("Settings", "Location", LOC_PROGRAMS);
        start.group = IniInt("Settings", "Group", 0);
        IniGet("Settings", "Path", "", start.path, MAXP);
        if (start.kind < LOC_COMPUTER || start.kind > LOC_PROGRAMS || start.kind == LOC_SEARCH
            || (start.kind == LOC_DIR && !IsDir(start.path))
            || (start.kind == LOC_GROUP && start.group >= nGroups))
            start.kind = LOC_PROGRAMS;
    }
    Navigate(&start, TRUE);
    ShowWindow(hFrame, maxed && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hFrame);
    FocusView();
    if (isShell && !prev)
        RunStartup();

    accel = LoadAccelerators(inst, MAKEINTRESOURCE(IDR_ACCEL));
    while (GetMessage(&msg, NULL, 0, 0)) {
        HWND f;
        char cls[16];

        if (FieldKey(&msg))
            continue;
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)
            && (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)) {
            WORD hk = (WORD)(LOBYTE(msg.wParam)
                             | ((GetKeyState(VK_CONTROL) < 0 ? HK_CTRL : 0)
                                | (GetKeyState(VK_MENU) < 0 ? HK_ALT : 0)
                                | (GetKeyState(VK_SHIFT) < 0 ? HK_SHIFT : 0)) << 8);

            if ((HIBYTE(hk) & (HK_CTRL | HK_ALT)) == (HK_CTRL | HK_ALT) || msg.wParam >= VK_F1)
                if (msg.wParam >= '0' && HotkeyRun(hk))
                    continue;
        }
        f = GetFocus();
        cls[0] = '\0';
        if (f)
            GetClassName(f, cls, sizeof(cls));
        /* edit fields keep their own keys; function keys still work */
        if (lstrcmpi(cls, "EDIT") == 0 && msg.message == WM_KEYDOWN
            && !(msg.wParam >= VK_F1 && msg.wParam <= VK_F12)
            && !(GetKeyState(VK_CONTROL) < 0 && (msg.wParam == 'F' || msg.wParam == 'L' || msg.wParam == 'R'))) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        if (!TranslateAccelerator(hFrame, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    PreviewDone();
    WasteFree();
    GroupsDone();
    IconsFlush();
    NavDone();
    ThemeDone();
    return (int)msg.wParam;
}
