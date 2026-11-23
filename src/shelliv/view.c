/* ShellIV - the item view: a location banner over icons, tiles or
 * sortable details, with selection, marquee, type-ahead, in-place
 * renaming and drag and drop. */
#include "shelliv.h"

#define BANNER_H    60
#define HEADER_H    21
#define CELL_W      92
#define CELL_H      80
#define TILE_W      216
#define TILE_H      54
#define MARGIN      10
#define TIMER_HOT   2

HWND hView, hRename;
ITEM **items;
int nItems, viewMode = VM_ICONS, sortCol, sortDesc;

static int capItems, focus = -1, anchor = -1, hot = -1, dropHi = -1, scrollY, contentH;
static int bannerIcon = BI_FOLDER;
static char bannerTitle[MAXP], bannerSub[200];
static DWORD gaugeFree, gaugeTotal;
static BOOL tracking, marquee, pendingSingle, hotTimer;
static POINT downPt, markA, markB;     /* marquee corners in content coordinates */
static char typeBuf[40];
static DWORD typeTime;
static int renameIdx = -1;
static int rowH = 20;

/* ---- columns ---------------------------------------------------------------- */

enum { C_NAME, C_SIZE, C_TYPE, C_DATE, C_ATTR, C_SUB, C_FREE, C_TOTAL, C_DIR };

typedef struct { const char *title; int key, width; BOOL right; } COLUMN;

static const COLUMN colFiles[] = {
    { "Name", C_NAME, 0, FALSE }, { "Size", C_SIZE, 84, TRUE }, { "Type", C_TYPE, 150, FALSE },
    { "Modified", C_DATE, 128, FALSE }, { "Attr", C_ATTR, 46, FALSE }, { NULL, 0, 0, 0 }
};
static const COLUMN colSearch[] = {
    { "Name", C_NAME, 0, FALSE }, { "In Folder", C_SUB, 190, FALSE }, { "Size", C_SIZE, 80, TRUE },
    { "Modified", C_DATE, 128, FALSE }, { NULL, 0, 0, 0 }
};
static const COLUMN colWaste[] = {
    { "Name", C_NAME, 0, FALSE }, { "Original Location", C_SUB, 200, FALSE },
    { "Deleted", C_DATE, 128, FALSE }, { "Size", C_SIZE, 80, TRUE }, { NULL, 0, 0, 0 }
};
static const COLUMN colProgs[] = {
    { "Name", C_NAME, 0, FALSE }, { "Command Line", C_SUB, 260, FALSE },
    { "Working Directory", C_DIR, 170, FALSE }, { NULL, 0, 0, 0 }
};
static const COLUMN colDrives[] = {
    { "Name", C_NAME, 0, FALSE }, { "Type", C_TYPE, 130, FALSE }, { "Free Space", C_FREE, 96, TRUE },
    { "Total Size", C_TOTAL, 96, TRUE }, { "", -1, 130, FALSE }, { NULL, 0, 0, 0 }
};
static const COLUMN colGroups[] = {
    { "Name", C_NAME, 0, FALSE }, { "Programs", C_SIZE, 90, TRUE }, { NULL, 0, 0, 0 }
};

static const COLUMN *Columns(void)
{
    switch (cur.kind) {
    case LOC_SEARCH:   return colSearch;
    case LOC_WASTE:    return colWaste;
    case LOC_GROUP:
    case LOC_RECENT:   return colProgs;
    case LOC_COMPUTER: return colDrives;
    case LOC_PROGRAMS: return colGroups;
    }
    return colFiles;
}

static int ColWidth(const COLUMN *c, int i, int cw)
{
    int w = 0, j;

    if (c[i].width)
        return c[i].width;
    for (j = 0; c[j].title; j++)
        w += c[j].width;
    return max(170, cw - w - 4);
}

/* ---- geometry -------------------------------------------------------------- */

static int ClientW(void)
{
    RECT r;

    GetClientRect(hView, &r);
    return r.right;
}

static int ClientH(void)
{
    RECT r;

    GetClientRect(hView, &r);
    return r.bottom;
}

static int Top(void)
{
    return BANNER_H + (viewMode == VM_DETAILS ? HEADER_H : 0);
}

static int Cols(void)
{
    int w = ClientW() - 2 * MARGIN;

    if (viewMode == VM_ICONS)
        return max(1, w / CELL_W);
    if (viewMode == VM_TILES)
        return max(1, w / TILE_W);
    return 1;
}

/* Rectangle of item i in window coordinates. */
static void ItemRect(int i, RECT *r)
{
    int cols = Cols(), top = Top() - scrollY;

    if (viewMode == VM_ICONS) {
        int x = MARGIN + (i % cols) * CELL_W, y = top + MARGIN + (i / cols) * CELL_H;

        SetRect(r, x, y, x + CELL_W, y + CELL_H);
    } else if (viewMode == VM_TILES) {
        int x = MARGIN + (i % cols) * TILE_W, y = top + MARGIN + (i / cols) * TILE_H;

        SetRect(r, x, y, x + TILE_W - 6, y + TILE_H - 4);
    } else {
        SetRect(r, 0, top + i * rowH, ClientW(), top + (i + 1) * rowH);
    }
}

/* The part of an item that reacts to the mouse. */
static void HitRect(int i, RECT *r)
{
    ItemRect(i, r);
    if (viewMode == VM_ICONS)
        InflateRect(r, -8, -2);
    else if (viewMode == VM_DETAILS) {
        const COLUMN *c = Columns();

        r->right = ColWidth(c, 0, ClientW());
    }
}

static int ItemAt(int x, int y)
{
    RECT r;
    POINT pt;
    int i, first, last, cols = Cols();

    if (y < Top())
        return -1;
    pt.x = x;
    pt.y = y;
    if (viewMode == VM_DETAILS) {
        i = (y - Top() + scrollY) / rowH;
        if (i >= 0 && i < nItems) {
            HitRect(i, &r);
            if (PtInRect(&r, pt))
                return i;
        }
        return -1;
    }
    first = max(0, ((y - Top() + scrollY - MARGIN) / (viewMode == VM_ICONS ? CELL_H : TILE_H) - 1) * cols);
    last = min(nItems, first + 3 * cols);
    for (i = first; i < last; i++) {
        HitRect(i, &r);
        if (PtInRect(&r, pt))
            return i;
    }
    return -1;
}

static void UpdateScroll(void)
{
    int rows, page = ClientH() - Top(), maxs;

    if (viewMode == VM_DETAILS)
        contentH = nItems * rowH + 4;
    else {
        rows = (nItems + Cols() - 1) / Cols();
        contentH = rows * (viewMode == VM_ICONS ? CELL_H : TILE_H) + 2 * MARGIN;
    }
    maxs = max(0, contentH - page);
    if (scrollY > maxs)
        scrollY = maxs;
    if (scrollY < 0)
        scrollY = 0;
#ifdef RETRO_WIN32
    {
        SCROLLINFO si;

        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = contentH - 1;
        si.nPage = max(1, page);
        si.nPos = scrollY;
        SetScrollInfo(hView, SB_VERT, &si, TRUE);
    }
#else
    SetScrollRange(hView, SB_VERT, 0, maxs, FALSE);
    SetScrollPos(hView, SB_VERT, scrollY, TRUE);
    if (maxs == 0)
        ShowScrollBar(hView, SB_VERT, FALSE);
#endif
}

static void ScrollTo(int y)
{
    int maxs = max(0, contentH - (ClientH() - Top()));

    y = max(0, min(y, maxs));
    if (y == scrollY)
        return;
    scrollY = y;
    UpdateScroll();
    InvalidateRect(hView, NULL, FALSE);
    if (hRename)
        ViewEndRename(TRUE);
}

void ViewEnsureVisible(int i)
{
    RECT r;
    int top = Top(), h = ClientH();

    if (i < 0 || i >= nItems)
        return;
    ItemRect(i, &r);
    if (r.top < top)
        ScrollTo(scrollY - (top - r.top) - (viewMode == VM_DETAILS ? 0 : MARGIN));
    else if (r.bottom > h)
        ScrollTo(scrollY + (r.bottom - h) + (viewMode == VM_DETAILS ? 0 : MARGIN));
}

static void InvalidateItem(int i)
{
    RECT r;

    if (i < 0 || i >= nItems)
        return;
    ItemRect(i, &r);
    InflateRect(&r, 2, 2);
    InvalidateRect(hView, &r, FALSE);
}

void ViewInvalidate(void)
{
    if (hView) {
        UpdateScroll();
        InvalidateRect(hView, NULL, FALSE);
    }
}

/* ---- item list ------------------------------------------------------------- */

static void FreeItem(ITEM *it)
{
    StrFree(&it->name);
    StrFree(&it->sub);
    free(it);
}

void ViewFree(void)
{
    int i;

    if (hRename)
        ViewEndRename(FALSE);
    for (i = 0; i < nItems; i++)
        FreeItem(items[i]);
    nItems = 0;
    focus = anchor = hot = dropHi = -1;
}

void ViewBegin(void)
{
    ViewFree();
}

ITEM *ViewAdd(int kind, const char *name, const char *sub)
{
    ITEM *it;

    if (nItems == capItems) {
        int cap = capItems ? capItems * 2 : 256;
        ITEM **n = (ITEM **)realloc(items, cap * sizeof(ITEM *));

        if (!n)
            return NULL;
        items = n;
        capItems = cap;
    }
    it = (ITEM *)calloc(1, sizeof(ITEM));
    if (!it)
        return NULL;
    it->kind = (BYTE)kind;
    it->name = StrDup(name);
    it->sub = sub ? StrDup(sub) : NULL;
    it->ref = -1;
    items[nItems++] = it;
    return it;
}

static int Compare(const ITEM *a, const ITEM *b)
{
    int r = 0, da, db;
    char ta[60], tb[60];

    /* folders first */
    da = a->kind == IK_DIR || a->kind == IK_DRIVE || (a->kind == IK_WASTE && (a->attr & FA_DIR));
    db = b->kind == IK_DIR || b->kind == IK_DRIVE || (b->kind == IK_WASTE && (b->attr & FA_DIR));
    if (da != db)
        return db - da;
    switch (sortCol) {
    case C_SIZE:
        r = a->size < b->size ? -1 : a->size > b->size;
        break;
    case C_TYPE:
        ItemTypeName(a, ta);
        ItemTypeName(b, tb);
        r = lstrcmpi(ta, tb);
        break;
    case C_DATE:
        r = a->date != b->date ? (a->date < b->date ? -1 : 1)
                               : (a->time < b->time ? -1 : a->time > b->time);
        break;
    case C_SUB:
        r = lstrcmpi(a->sub ? a->sub : "", b->sub ? b->sub : "");
        break;
    }
    if (r == 0)
        r = lstrcmpi(a->name, b->name);
    return sortDesc ? -r : r;
}

static int QCompare(const void *a, const void *b)
{
    return Compare(*(ITEM *const *)a, *(ITEM *const *)b);
}

static BOOL Sortable(void)
{
    return cur.kind == LOC_DIR || cur.kind == LOC_SEARCH || cur.kind == LOC_WASTE;
}

void ViewEnd(const char *keepFocus)
{
    if (Sortable() && nItems > 1)
        qsort(items, nItems, sizeof(ITEM *), QCompare);
    focus = anchor = -1;
    if (keepFocus && keepFocus[0])
        ViewSelectName(keepFocus);
    if (focus < 0 && nItems)
        focus = anchor = 0;
    if (!keepFocus)
        scrollY = 0;
    ViewInvalidate();
    if (focus >= 0)
        ViewEnsureVisible(focus);
    SelectionChanged();
}

void ViewSetBanner(int bi, const char *title, const char *sub)
{
    RECT r;

    bannerIcon = bi;
    lstrcpyn(bannerTitle, title, sizeof(bannerTitle));
    lstrcpyn(bannerSub, sub, sizeof(bannerSub));
    if (hView) {
        SetRect(&r, 0, 0, ClientW(), BANNER_H);
        InvalidateRect(hView, &r, FALSE);
    }
}

void ViewSetGauge(DWORD freeKB, DWORD totalKB)
{
    gaugeFree = freeKB;
    gaugeTotal = totalKB;
}

void ViewSetMode(int mode)
{
    int f = focus;

    if (hRename)
        ViewEndRename(TRUE);
    viewMode = mode;
    scrollY = 0;
    ViewInvalidate();
    ViewEnsureVisible(f);
}

void ViewSort(int col)
{
    char name[MAXP];

    if (!Sortable())
        return;
    if (sortCol == col)
        sortDesc = !sortDesc;
    else {
        sortCol = col;
        sortDesc = col == C_DATE || col == C_SIZE;
    }
    name[0] = '\0';
    if (focus >= 0)
        lstrcpyn(name, items[focus]->name, MAXP);
    if (nItems > 1)
        qsort(items, nItems, sizeof(ITEM *), QCompare);
    focus = -1;
    if (name[0]) {
        int i;

        for (i = 0; i < nItems; i++)
            if (strcmp(items[i]->name, name) == 0)
                focus = anchor = i;
    }
    ViewInvalidate();
    ViewEnsureVisible(focus);
}

/* ---- selection ------------------------------------------------------------- */

int ViewSelCount(void)
{
    int i, n = 0;

    for (i = 0; i < nItems; i++)
        if (items[i]->sel)
            n++;
    return n;
}

int ViewFocus(void)
{
    return focus;
}

ITEM *ViewFocusItem(void)
{
    if (focus >= 0 && focus < nItems && items[focus]->sel)
        return items[focus];
    return NULL;
}

static void SelectNone(void)
{
    int i;

    for (i = 0; i < nItems; i++)
        if (items[i]->sel) {
            items[i]->sel = 0;
            InvalidateItem(i);
        }
}

static void SetFocusItem(int i, BOOL select, BOOL extend)
{
    int a, b, j;

    if (i < 0 || i >= nItems)
        return;
    InvalidateItem(focus);
    if (extend && anchor >= 0) {
        a = min(anchor, i);
        b = max(anchor, i);
        for (j = 0; j < nItems; j++) {
            BYTE s = (BYTE)(j >= a && j <= b);

            if (items[j]->sel != s) {
                items[j]->sel = s;
                InvalidateItem(j);
            }
        }
    } else if (select) {
        SelectNone();
        items[i]->sel = 1;
        anchor = i;
    }
    focus = i;
    InvalidateItem(i);
    ViewEnsureVisible(i);
    SelectionChanged();
}

void ViewSelectName(const char *name)
{
    int i;

    for (i = 0; i < nItems; i++)
        if (lstrcmpi(items[i]->name, name) == 0) {
            SetFocusItem(i, TRUE, FALSE);
            return;
        }
}

/* Selects the items matching pat; the first one gets the focus. */
int ViewSelectPattern(const char *pat)
{
    int i, n = 0, first = -1;

    for (i = 0; i < nItems; i++) {
        items[i]->sel = (BYTE)(WildMatch(pat, items[i]->name) ? 1 : 0);
        if (items[i]->sel && first < 0)
            first = i;
        n += items[i]->sel;
    }
    if (first >= 0) {
        focus = anchor = first;
        ViewEnsureVisible(first);
    }
    InvalidateRect(hView, NULL, FALSE);
    SelectionChanged();
    return n;
}

void ViewSelectAll(void)
{
    int i;

    for (i = 0; i < nItems; i++)
        items[i]->sel = 1;
    InvalidateRect(hView, NULL, FALSE);
    SelectionChanged();
}

void ViewItemPath(const ITEM *it, char *out)
{
    out[0] = '\0';
    switch (it->kind) {
    case IK_DRIVE:
        sprintf(out, "%c:\\", 'A' + it->ref);
        break;
    case IK_WASTE:
        if (it->ref >= 0 && it->ref < nWaste)
            lstrcpyn(out, waste[it->ref].stored, MAXP);
        break;
    case IK_DIR:
    case IK_FILE:
        if (cur.kind == LOC_SEARCH && it->sub)
            PathJoin(out, it->sub, it->name);
        else
            PathJoin(out, cur.path, it->name);
        break;
    }
}

/* ---- drag and drop targets ---------------------------------------------- */

void ViewSetDropHilite(int i)
{
    if (i == dropHi)
        return;
    InvalidateItem(dropHi);
    dropHi = i;
    InvalidateItem(dropHi);
    if (hView)
        UpdateWindow(hView);
}

int ViewDropTarget(POINT pt, char *path, int *group, int *pos)
{
    int i = ItemAt(pt.x, pt.y);
    ITEM *it = i >= 0 ? items[i] : NULL;

    *pos = -1;
    if (it && !it->sel) {
        if (it->kind == IK_DIR || it->kind == IK_DRIVE) {
            ViewItemPath(it, path);
            ViewSetDropHilite(i);
            return DT_DIR;
        }
        if (it->kind == IK_GROUP && it->ref >= 0) {
            *group = it->ref;
            ViewSetDropHilite(i);
            return DT_GROUP;
        }
    }
    ViewSetDropHilite(-1);
    if (cur.kind == LOC_DIR) {
        strcpy(path, cur.path);
        return DT_DIR;
    }
    if (cur.kind == LOC_GROUP) {
        *group = cur.group;
        if (it && it->kind == IK_PROG)
            *pos = it->ref;
        return DT_GROUP;
    }
    return DT_NONE;
}

/* ---- in-place rename ------------------------------------------------------- */

static void LabelRect(int i, RECT *r)
{
    RECT ir;

    ItemRect(i, &ir);
    if (viewMode == VM_ICONS)
        SetRect(r, ir.left + 2, ir.top + 42, ir.right - 2, ir.top + 42 + lineH + 6);
    else if (viewMode == VM_TILES)
        SetRect(r, ir.left + 46, ir.top + 6, ir.right - 4, ir.top + 8 + lineH + 4);
    else
        SetRect(r, ir.left + 24, ir.top + 1, ColWidth(Columns(), 0, ClientW()) - 4, ir.bottom - 1);
}

void ViewRename(void)
{
    RECT r;
    ITEM *it = ViewFocusItem();

    if (!it || hRename)
        return;
    if (it->kind != IK_FILE && it->kind != IK_DIR && it->kind != IK_PROG && it->kind != IK_GROUP)
        return;
    if (cur.kind == LOC_RECENT || (it->kind == IK_GROUP && it->ref < 0))
        return;
    ViewEnsureVisible(focus);
    LabelRect(focus, &r);
    renameIdx = focus;
    hRename = CreateWindow("EDIT", it->name, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                           r.left, r.top, r.right - r.left, r.bottom - r.top,
                           hView, (HMENU)1, hInst, NULL);
    if (!hRename)
        return;
    SendMessage(hRename, WM_SETFONT, (WPARAM)fontUI, 0);
    SendMessage(hRename, EM_LIMITTEXT, MAXP - 20, 0);
    {
        /* select the name without its extension */
        const char *dot = strrchr(it->name, '.');
        int n = (dot && it->kind == IK_FILE) ? (int)(dot - it->name) : -1;

        SendMessage(hRename, EM_SETSEL, 0, n < 0 ? MAKELONG(0, -1) : MAKELONG(0, n));
#ifdef RETRO_WIN32
        SendMessage(hRename, EM_SETSEL, 0, n);
#endif
    }
    ShowWindow(hRename, SW_SHOW);
    SetFocus(hRename);
}

void ViewEndRename(BOOL commit)
{
    char name[MAXP];
    HWND h = hRename;
    int i = renameIdx;

    if (!h)
        return;
    hRename = NULL;
    renameIdx = -1;
    GetWindowText(h, name, MAXP);
    DestroyWindow(h);
    if (GetFocus() == NULL || GetFocus() == h)
        SetFocus(hView);
    if (commit && i >= 0 && i < nItems && name[0] && strcmp(name, items[i]->name) != 0)
        OpsRename(items[i], name);
}

/* ---- painting ------------------------------------------------------------- */

static void PaintBanner(HDC hdc, int cw)
{
    RECT r;
    int x = 62;
    BOOL gauge = gaugeTotal && cw > 460;
    int right = gauge ? cw - 190 : cw - 14;

    SetRect(&r, 0, 0, cw, BANNER_H);
    Gradient(hdc, &r, th.band1, th.band2, FALSE);
    HLine(hdc, 0, cw, BANNER_H - 1, Mix(th.band1, RGB(0, 0, 0), 30));
    /* a soft halo behind the icon */
    SetRect(&r, 12, 8, 56, 52);
    Pill(hdc, &r, Mix(th.band1, RGB(255, 255, 255), 18), Mix(th.band1, RGB(255, 255, 255), 30));
    DrawIcon(hdc, 18, 14, BuiltinIcon(bannerIcon));
    SelectObject(hdc, fontTitle);
    SetRect(&r, x, 7, right, 34);
    TextIn(hdc, &r, bannerTitle, Mix(th.band1, RGB(0, 0, 0), 40), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    OffsetRect(&r, -1, -1);
    TextIn(hdc, &r, bannerTitle, th.bandText, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, fontUI);
    SetRect(&r, x, 35, right, 52);
    TextIn(hdc, &r, bannerSub, Mix(th.bandText, th.band2, 22), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (gauge) {
        char a[24], b[24], txt[64];

        SetRect(&r, cw - 176, 20, cw - 16, 29);
        DrawGauge(hdc, &r, gaugeFree, gaugeTotal, Mix(th.band2, RGB(255, 255, 255), 70));
        FmtKB(a, gaugeFree);
        FmtKB(b, gaugeTotal);
        sprintf(txt, "%s free of %s", a, b);
        SelectObject(hdc, fontSmall);
        SetRect(&r, cw - 176, 32, cw - 16, 46);
        TextIn(hdc, &r, txt, th.bandText, DT_RIGHT | DT_SINGLELINE);
        SelectObject(hdc, fontUI);
    }
}

static void PaintHeader(HDC hdc, int cw)
{
    const COLUMN *c = Columns();
    RECT r;
    int x = 0, i, w;

    SetRect(&r, 0, BANNER_H, cw, BANNER_H + HEADER_H);
    Gradient(hdc, &r, th.bar1, th.bar2, TRUE);
    HLine(hdc, 0, cw, r.bottom - 1, Mix(th.bar2, RGB(0, 0, 0), 20));
    SelectObject(hdc, fontBold);
    for (i = 0; c[i].title; i++) {
        w = ColWidth(c, i, cw);
        SetRect(&r, x + 8, BANNER_H, x + w - 8, BANNER_H + HEADER_H - 1);
        TextIn(hdc, &r, c[i].title, th.barText,
               (c[i].right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (Sortable() && c[i].key == sortCol) {
            int ax = c[i].right ? x + 6 : x + 8 + TextW(hdc, c[i].title) + 6;
            int ay = BANNER_H + 7;
            int k;

            for (k = 0; k < 4; k++) {
                if (sortDesc)
                    HLine(hdc, ax + k, ax + 7 - k, ay + k, th.accent);
                else
                    HLine(hdc, ax + k, ax + 7 - k, ay + 3 - k, th.accent);
            }
        }
        x += w;
        VLine(hdc, x - 1, BANNER_H + 4, BANNER_H + HEADER_H - 4, Mix(th.bar2, RGB(0, 0, 0), 20));
    }
    SelectObject(hdc, fontUI);
}

static void ItemLine2(const ITEM *it, char *out)
{
    char a[30], b[30];

    out[0] = '\0';
    switch (it->kind) {
    case IK_DRIVE:
        if (it->total) {
            FmtKB(a, it->size);
            FmtKB(b, it->total);
            sprintf(out, "%s free of %s", a, b);
        } else {
            ItemTypeName(it, out);
        }
        break;
    case IK_DIR:
        if (cur.kind == LOC_SEARCH && it->sub)
            lstrcpyn(out, it->sub, 80);
        else
            strcpy(out, "File Folder");
        break;
    case IK_PROG:
        lstrcpyn(out, it->sub ? it->sub : "", 80);
        break;
    case IK_GROUP:
        sprintf(out, "%lu program%s", it->size, it->size == 1 ? "" : "s");
        break;
    default:
        ItemTypeName(it, a);
        FmtSize(b, it->size);
        if (it->attr & FA_DIR)
            strcpy(out, a);
        else
            sprintf(out, "%s, %s", a, b);
    }
}

static void PaintIconItem(HDC hdc, int i, const RECT *cell)
{
    ITEM *it = items[i];
    RECT lr, ir;
    BOOL sel = it->sel, hi = i == dropHi, isHot = i == hot && !sel;
    int ix = cell->left + (CELL_W - 32) / 2, iy = cell->top + 5;
    UINT fmt = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;

    SetRect(&ir, ix - 6, iy - 3, ix + 38, iy + 37);
    if (sel || hi || isHot)
        Pill(hdc, &ir, hi ? th.sel : isHot ? Mix(th.sel, th.view, 55) : Mix(th.sel, th.view, 35),
             hi ? th.selBorder : isHot ? Mix(th.sel, th.view, 30) : Mix(th.selBorder, th.view, 50));
    DrawItemIcon(hdc, ix, iy, it);
    SelectObject(hdc, fontUI);
    SetRect(&lr, cell->left + 4, cell->top + 44, cell->right - 4, cell->top + 44 + 2 * lineH);
    if (!(sel && i == focus)) {
        /* two lines at most */
        RECT calc = lr;

        DrawText(hdc, it->name, -1, &calc, fmt | DT_CALCRECT);
        if (calc.bottom > lr.bottom)
            fmt |= DT_END_ELLIPSIS;
    } else {
        RECT calc = lr;

        DrawText(hdc, it->name, -1, &calc, fmt | DT_CALCRECT);
        lr.bottom = max(lr.bottom, calc.bottom);
    }
    {
        RECT calc = lr, bg;
        int w;

        DrawText(hdc, it->name, -1, &calc, fmt | DT_CALCRECT);
        w = min(calc.right - calc.left, lr.right - lr.left);
        SetRect(&bg, lr.left + (lr.right - lr.left - w) / 2 - 3, lr.top - 1,
                lr.left + (lr.right - lr.left + w) / 2 + 3, min(lr.bottom, calc.bottom) + 1);
        if (sel)
            Pill(hdc, &bg, th.selBorder, th.selBorder);
        if (sel && i == focus && GetFocus() == hView) {
            InflateRect(&bg, -1, -1);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawFocusRect(hdc, &bg);
        }
        TextIn(hdc, &lr, it->name, sel ? RGB(255, 255, 255) : th.viewText, fmt);
        if (!sel && i == focus && GetFocus() == hView)
            DrawFocusRect(hdc, &bg);
    }
}

static void PaintTileItem(HDC hdc, int i, const RECT *cell)
{
    ITEM *it = items[i];
    RECT r = *cell, t;
    BOOL sel = it->sel, hi = i == dropHi, isHot = i == hot && !sel;
    char line[120];

    if (sel || hi)
        Pill(hdc, &r, th.sel, th.selBorder);
    else if (isHot)
        Pill(hdc, &r, Mix(th.sel, th.view, 55), Mix(th.sel, th.view, 30));
    if (i == focus && GetFocus() == hView) {
        RECT f = r;

        InflateRect(&f, -2, -2);
        SetTextColor(hdc, PC(th.viewText));
        DrawFocusRect(hdc, &f);
    }
    DrawItemIcon(hdc, r.left + 8, r.top + (r.bottom - r.top - 32) / 2, it);
    SelectObject(hdc, fontBold);
    SetRect(&t, r.left + 48, r.top + 6, r.right - 6, r.top + 6 + lineH + 2);
    TextIn(hdc, &t, it->name, sel ? th.selText : th.viewText, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, fontUI);
    ItemLine2(it, line);
    if (it->kind == IK_DRIVE && it->total) {
        SetRect(&t, r.left + 48, r.top + 9 + lineH, r.right - 14, r.top + 16 + lineH);
        DrawGauge(hdc, &t, it->size, it->total, th.view);
        SetRect(&t, r.left + 48, r.top + 18 + lineH, r.right - 6, r.bottom - 2);
    } else {
        SetRect(&t, r.left + 48, r.top + 8 + lineH, r.right - 6, r.top + 8 + 2 * lineH);
    }
    TextIn(hdc, &t, line, sel ? Mix(th.selText, th.sel, 25) : th.dimText,
           DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (it->kind == IK_FILE || it->kind == IK_WASTE) {
        if (it->date && t.bottom + lineH <= r.bottom + 2) {
            FmtDate(line, it->date, it->time);
            OffsetRect(&t, 0, lineH);
            TextIn(hdc, &t, line, sel ? Mix(th.selText, th.sel, 25) : th.dimText,
                   DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

static void CellText(const ITEM *it, int key, char *out)
{
    out[0] = '\0';
    switch (key) {
    case C_NAME:
        strcpy(out, it->name);
        break;
    case C_SIZE:
        if (it->kind == IK_GROUP)
            sprintf(out, "%lu", it->size);
        else if (it->kind == IK_FILE || (it->kind == IK_WASTE && !(it->attr & FA_DIR)))
            FmtSize(out, it->size);
        break;
    case C_TYPE:
        ItemTypeName(it, out);
        break;
    case C_DATE:
        FmtDate(out, it->date, it->time);
        break;
    case C_ATTR:
        FmtAttr(out, it->attr);
        break;
    case C_SUB:
        lstrcpyn(out, it->sub ? it->sub : "", MAXP);
        break;
    case C_DIR:
        if (it->kind == IK_PROG) {
            GROUP *gr = GroupPtr(cur.kind == LOC_RECENT ? -1 : cur.group);

            if (gr && it->ref >= 0 && it->ref < gr->n && gr->items[it->ref].dir)
                lstrcpyn(out, gr->items[it->ref].dir, MAXP);
        }
        break;
    case C_FREE:
        if (it->total)
            FmtKB(out, it->size);
        break;
    case C_TOTAL:
        if (it->total)
            FmtKB(out, it->total);
        break;
    }
}

static void PaintDetailRow(HDC hdc, int i, const RECT *row, int cw)
{
    ITEM *it = items[i];
    const COLUMN *c = Columns();
    RECT r = *row, t;
    BOOL sel = it->sel || i == dropHi;
    COLORREF bg = sel ? th.sel : (i == hot ? Mix(th.sel, th.view, 60) : (i & 1) ? th.stripe : th.view);
    COLORREF fg = sel ? th.selText : th.viewText;
    char text[MAXP];
    int x = 0, k, w;

    FillR(hdc, &r, bg);
    if (sel) {
        HLine(hdc, r.left, r.right, r.top, Mix(th.selBorder, th.sel, 50));
        HLine(hdc, r.left, r.right, r.bottom - 1, Mix(th.selBorder, th.sel, 50));
    }
    if (i == focus && GetFocus() == hView) {
        RECT f = r;

        InflateRect(&f, -1, 0);
        SetTextColor(hdc, PC(fg));
        SetBkColor(hdc, PC(bg));
        DrawFocusRect(hdc, &f);
    }
    for (k = 0; c[k].title; k++) {
        w = ColWidth(c, k, cw);
        if (k == 0) {
            DrawItemSmall(hdc, x + 5, r.top + (rowH - 16) / 2, it, bg);
            SetRect(&t, x + 26, r.top, x + w - 6, r.bottom);
        } else {
            SetRect(&t, x + 8, r.top, x + w - 8, r.bottom);
        }
        if (c[k].key == -1) {
            if (it->total) {
                RECT g;

                SetRect(&g, t.left, r.top + rowH / 2 - 4, t.right, r.top + rowH / 2 + 4);
                DrawGauge(hdc, &g, it->size, it->total, th.view);
            }
        } else {
            CellText(it, c[k].key, text);
            SelectObject(hdc, k == 0 && (it->kind == IK_DIR || it->kind == IK_DRIVE || it->kind == IK_GROUP)
                              ? fontBold : fontUI);
            TextIn(hdc, &t, text, k == 0 ? fg : (sel ? fg : th.dimText),
                   (c[k].right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        x += w;
    }
    SelectObject(hdc, fontUI);
}

static void PaintEmpty(HDC hdc, int cw, int ch)
{
    const char *msg;
    RECT r;

    switch (cur.kind) {
    case LOC_GROUP:  msg = "This group is empty.\nDrag programs here from a folder, or choose New Program."; break;
    case LOC_RECENT: msg = "Programs you start appear here."; break;
    case LOC_SEARCH: msg = "No matching files were found."; break;
    case LOC_WASTE:  msg = "The wastebasket is empty."; break;
    default:         msg = "This folder is empty."; break;
    }
    SetRect(&r, 20, Top() + 30, cw - 20, ch);
    SelectObject(hdc, fontUI);
    TextIn(hdc, &r, msg, th.dimText, DT_CENTER | DT_WORDBREAK);
}

static void Paint(HWND hwnd, HDC hdc, const RECT *rc)
{
    RECT r, cell, cl;
    int i, first, last, cw;

    (void)hwnd;
    GetClientRect(hView, &cl);
    cw = cl.right;
    SelectObject(hdc, fontUI);
    if (rc->top < BANNER_H)
        PaintBanner(hdc, cw);
    SetRect(&r, 0, Top(), cw, cl.bottom);
    if (rc->bottom > BANNER_H && viewMode == VM_DETAILS)
        PaintHeader(hdc, cw);
    if (rc->bottom <= Top())
        return;
    /* clip items to the area below the banner / header */
    IntersectClipRect(hdc, 0, Top(), cw, cl.bottom);
    FillR(hdc, &r, th.view);
    if (!nItems) {
        PaintEmpty(hdc, cw, cl.bottom);
        return;
    }
    if (viewMode == VM_DETAILS) {
        first = max(0, (rc->top - Top() + scrollY) / rowH);
        last = min(nItems, (rc->bottom - Top() + scrollY) / rowH + 1);
        for (i = first; i < last; i++) {
            ItemRect(i, &cell);
            PaintDetailRow(hdc, i, &cell, cw);
        }
    } else {
        int ch = viewMode == VM_ICONS ? CELL_H : TILE_H, cols = Cols();

        first = max(0, ((rc->top - Top() + scrollY - MARGIN) / ch - 1) * cols);
        last = min(nItems, ((rc->bottom - Top() + scrollY - MARGIN) / ch + 2) * cols);
        for (i = first; i < last; i++) {
            ItemRect(i, &cell);
            if (cell.bottom + 30 < rc->top || cell.top > rc->bottom)
                continue;
            if (viewMode == VM_ICONS)
                PaintIconItem(hdc, i, &cell);
            else
                PaintTileItem(hdc, i, &cell);
        }
    }
    if (marquee) {
        RECT m;

        SetRect(&m, min(markA.x, markB.x), min(markA.y, markB.y) - scrollY,
                max(markA.x, markB.x) + 1, max(markA.y, markB.y) - scrollY + 1);
        FrameR(hdc, &m, th.selBorder);
        InflateRect(&m, -1, -1);
        FrameR(hdc, &m, Mix(th.sel, th.view, 30));
    }
}

/* ---- mouse and keyboard ------------------------------------------------------ */

static void MarqueeSelect(BOOL add)
{
    RECT m, r, x;
    int i;
    BOOL changed = FALSE;

    SetRect(&m, min(markA.x, markB.x), min(markA.y, markB.y) - scrollY,
            max(markA.x, markB.x) + 1, max(markA.y, markB.y) - scrollY + 1);
    for (i = 0; i < nItems; i++) {
        BYTE s;

        HitRect(i, &r);
        s = (BYTE)IntersectRect(&x, &r, &m);
        if (add)
            s = (BYTE)(s || (items[i]->sel & 2));
        if ((items[i]->sel & 1) != s) {
            items[i]->sel = (BYTE)((items[i]->sel & 2) | s);
            changed = TRUE;
            if (s)
                focus = i;
        }
    }
    if (changed)
        SelectionChanged();
}

static void SetHot(int i)
{
    if (i == hot)
        return;
    InvalidateItem(hot);
    hot = i;
    InvalidateItem(hot);
    if (hot >= 0 && !hotTimer) {
        SetTimer(hView, TIMER_HOT, 150, NULL);
        hotTimer = TRUE;
    }
}

static void Move(int key, BOOL shift, BOOL ctrl)
{
    int cols = Cols(), page, i = focus < 0 ? 0 : focus;

    if (!nItems)
        return;
    page = max(1, (ClientH() - Top()) / (viewMode == VM_DETAILS ? rowH :
                                          viewMode == VM_ICONS ? CELL_H : TILE_H)) * cols;
    switch (key) {
    case VK_LEFT:  i = viewMode == VM_DETAILS ? i : i - 1; break;
    case VK_RIGHT: i = viewMode == VM_DETAILS ? i : i + 1; break;
    case VK_UP:    i -= cols; break;
    case VK_DOWN:  i += cols; break;
    case VK_PRIOR: i -= page; break;
    case VK_NEXT:  i += page; break;
    case VK_HOME:  i = 0; break;
    case VK_END:   i = nItems - 1; break;
    }
    i = max(0, min(nItems - 1, i));
    if (ctrl && !shift) {
        InvalidateItem(focus);
        focus = i;
        InvalidateItem(focus);
        ViewEnsureVisible(i);
        return;
    }
    SetFocusItem(i, TRUE, shift);
}

static void TypeAhead(char ch)
{
    DWORD now = GetTickCount();
    int n, i, start;

    if (now - typeTime > 1500)
        typeBuf[0] = '\0';
    typeTime = now;
    n = strlen(typeBuf);
    if (n < (int)sizeof(typeBuf) - 1) {
        typeBuf[n] = ch;
        typeBuf[n + 1] = '\0';
    }
    n = strlen(typeBuf);
    start = focus < 0 ? 0 : (n == 1 ? focus + 1 : focus);
    for (i = 0; i < nItems; i++) {
        ITEM *it = items[(start + i) % nItems];

        if (strnicmp(it->name, typeBuf, n) == 0) {
            SetFocusItem((start + i) % nItems, TRUE, FALSE);
            return;
        }
    }
    MessageBeep(0);
}

static int HeaderHit(int x)
{
    const COLUMN *c = Columns();
    int i, cw = ClientW(), pos = 0;

    for (i = 0; c[i].title; i++) {
        pos += ColWidth(c, i, cw);
        if (x < pos)
            return c[i].key;
    }
    return -1;
}

LRESULT CALLBACK __export ViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    POINT pt;
    int i;

    switch (msg) {
    case WM_CREATE:
        hView = hwnd;
        rowH = max(20, lineH + 7);
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_SIZE:
        UpdateScroll();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        BufferedPaint(hwnd, Paint);
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateItem(focus);
        return 0;

    case WM_VSCROLL: {
        int step = viewMode == VM_DETAILS ? rowH : viewMode == VM_ICONS ? CELL_H / 2 : TILE_H / 2;
        int page = ClientH() - Top() - step;

        switch (SCROLL_CODE(wParam)) {
        case SB_LINEUP:        ScrollTo(scrollY - step); break;
        case SB_LINEDOWN:      ScrollTo(scrollY + step); break;
        case SB_PAGEUP:        ScrollTo(scrollY - page); break;
        case SB_PAGEDOWN:      ScrollTo(scrollY + page); break;
        case SB_TOP:           ScrollTo(0); break;
        case SB_BOTTOM:        ScrollTo(contentH); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
#ifdef RETRO_WIN32
            {
                SCROLLINFO si;

                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hwnd, SB_VERT, &si);
                ScrollTo(si.nTrackPos);
            }
#else
            ScrollTo(SCROLL_POS(wParam, lParam));
#endif
            break;
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
        ScrollTo(scrollY - (short)HIWORD(wParam) / 120 * 3 * (viewMode == VM_DETAILS ? rowH : 30));
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_HOT) {
            GetCursorPos(&pt);
            if ((WindowFromPoint(pt) != hwnd || hot < 0) && !marquee) {
                SetHot(-1);
                KillTimer(hwnd, TIMER_HOT);
                hotTimer = FALSE;
            }
            if (marquee || tracking) {
                /* auto-scroll while selecting near the edges */
                ScreenToClient(hwnd, &pt);
                if (marquee && (pt.y < Top() || pt.y > ClientH())) {
                    ScrollTo(scrollY + (pt.y < Top() ? -20 : 20));
                    markB.x = pt.x;
                    markB.y = pt.y + scrollY;
                    MarqueeSelect(GetKeyState(VK_CONTROL) < 0);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        BOOL ctrl = (wParam & MK_CONTROL) != 0, shift = (wParam & MK_SHIFT) != 0;
        int x = MOUSE_X(lParam), y = MOUSE_Y(lParam);

        if (hRename)
            ViewEndRename(TRUE);
        SetFocus(hwnd);
        if (y >= BANNER_H && y < Top()) {
            if (msg == WM_LBUTTONDOWN)
                ViewSort(HeaderHit(x));
            return 0;
        }
        i = ItemAt(x, y);
        pendingSingle = FALSE;
        if (i >= 0) {
            if (msg == WM_RBUTTONDOWN) {
                if (!items[i]->sel)
                    SetFocusItem(i, TRUE, FALSE);
                return 0;
            }
            if (ctrl) {
                items[i]->sel ^= 1;
                InvalidateItem(focus);
                focus = anchor = i;
                InvalidateItem(i);
                SelectionChanged();
            } else if (shift) {
                SetFocusItem(i, FALSE, TRUE);
            } else if (!items[i]->sel) {
                SetFocusItem(i, TRUE, FALSE);
            } else {
                pendingSingle = TRUE;
                InvalidateItem(focus);
                focus = i;
                InvalidateItem(i);
                SelectionChanged();
            }
            tracking = TRUE;
            downPt.x = x;
            downPt.y = y;
            SetCapture(hwnd);
        } else {
            if (!ctrl) {
                SelectNone();
                SelectionChanged();
            }
            if (msg == WM_RBUTTONDOWN)
                return 0;
            for (i = 0; i < nItems; i++)
                items[i]->sel = (BYTE)(items[i]->sel ? 3 : 0);
            marquee = TRUE;
            markA.x = markB.x = x;
            markA.y = markB.y = y + scrollY;
            SetCapture(hwnd);
            if (!hotTimer) {
                SetTimer(hwnd, TIMER_HOT, 100, NULL);
                hotTimer = TRUE;
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = MOUSE_X(lParam), y = MOUSE_Y(lParam);

        if (dragging) {
            pt.x = x;
            pt.y = y;
            ClientToScreen(hwnd, &pt);
            DragMove(pt, (wParam & MK_CONTROL) != 0);
            return 0;
        }
        if (tracking && (wParam & MK_LBUTTON)) {
            if (abs(x - downPt.x) > 4 || abs(y - downPt.y) > 4) {
                tracking = FALSE;
                pendingSingle = FALSE;
                DragBegin();
            }
            return 0;
        }
        if (marquee) {
            markB.x = x;
            markB.y = y + scrollY;
            MarqueeSelect((wParam & MK_CONTROL) != 0);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        SetHot(ItemAt(x, y));
        return 0;
    }

    case WM_LBUTTONUP:
        pt.x = MOUSE_X(lParam);
        pt.y = MOUSE_Y(lParam);
        if (dragging) {
            ClientToScreen(hwnd, &pt);
            ReleaseCapture();
            DragEnd(pt, (wParam & MK_CONTROL) != 0, FALSE);
            return 0;
        }
        if (tracking) {
            tracking = FALSE;
            ReleaseCapture();
            if (pendingSingle && focus >= 0)
                SetFocusItem(focus, TRUE, FALSE);
        }
        if (marquee) {
            marquee = FALSE;
            ReleaseCapture();
            for (i = 0; i < nItems; i++)
                items[i]->sel &= 1;
            InvalidateRect(hwnd, NULL, FALSE);
            SelectionChanged();
        }
        return 0;

    case WM_RBUTTONUP:
        pt.x = MOUSE_X(lParam);
        pt.y = MOUSE_Y(lParam);
        i = ItemAt(pt.x, pt.y);
        ClientToScreen(hwnd, &pt);
        ContextMenu(hwnd, pt, i >= 0);
        return 0;

    case WM_LBUTTONDBLCLK:
        i = ItemAt(MOUSE_X(lParam), MOUSE_Y(lParam));
        if (i >= 0) {
            SetFocusItem(i, TRUE, FALSE);
            NavOpenItem(items[i]);
        } else if (MOUSE_Y(lParam) >= Top()) {
            Command(IDM_UP);
        }
        return 0;

    case WM_SYSKEYDOWN:
        /* F10 is a system key: Shift+F10 opens the context menu */
        if (wParam != VK_F10 || GetKeyState(VK_SHIFT) >= 0)
            break;
        /* fall through */
    case WM_KEYDOWN: {
        BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;

        switch (wParam) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
            Move((int)wParam, shift, ctrl);
            return 0;
        case VK_SPACE:
            if (ctrl && focus >= 0) {
                items[focus]->sel ^= 1;
                InvalidateItem(focus);
                SelectionChanged();
            }
            return 0;
        case VK_RETURN:
            if (focus >= 0 && items[focus]->sel)
                Command(GetKeyState(VK_MENU) < 0 ? IDM_PROPS : IDM_OPEN);
            return 0;
        case 0x5D:                              /* VK_APPS: the context menu */
        case VK_F10:
            if (wParam == VK_F10 && !shift)
                break;
            {
                RECT r;

                if (focus >= 0 && items[focus]->sel) {
                    ItemRect(focus, &r);
                    pt.x = r.left + 20;
                    pt.y = r.top + 20;
                } else {
                    pt.x = 40;
                    pt.y = Top() + 10;
                }
                ClientToScreen(hwnd, &pt);
                ContextMenu(hwnd, pt, focus >= 0 && items[focus]->sel);
            }
            return 0;
        case VK_ESCAPE:
            if (dragging) {
                GetCursorPos(&pt);
                ReleaseCapture();
                DragEnd(pt, FALSE, TRUE);
            }
            return 0;
        }
        break;
    }

    case WM_CHAR:
        if ((BYTE)wParam > ' ' && !(GetKeyState(VK_CONTROL) < 0))
            TypeAhead((char)wParam);
        return 0;

    case WM_COMMAND:
        if (CMD_HWND(wParam, lParam) == hRename && CMD_NOTIFY(wParam, lParam) == EN_KILLFOCUS)
            ViewEndRename(TRUE);
        return 0;

    case WM_DROPFILES:
        OpsDropFiles(hwnd, (HDROP)wParam);
        return 0;

    case WM_DESTROY:
        ViewFree();
        if (items)
            free(items);
        items = NULL;
        capItems = 0;
        hView = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL ViewRegister(void)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = (WNDPROC)ViewProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ShellIVView";
    return RegisterClass(&wc) != 0;
}
