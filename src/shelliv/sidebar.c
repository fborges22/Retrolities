/* ShellIV - the navigation sidebar: program groups, favorites, a drive and
 * folder tree, the wastebasket and the running tasks, in collapsible
 * sections. */
#include "shelliv.h"

enum { N_HEAD, N_PROGRAMS, N_RECENT, N_GROUP, N_FAV, N_HINT, N_COMPUTER, N_DRIVE,
       N_DIR, N_WASTE, N_TASK };
enum { S_PROGRAMS, S_FAVORITES, S_COMPUTER, S_TASKS, S_COUNT };

typedef struct {
    BYTE type, depth, expanded, kids, dk;
    short ref;                  /* section, group, favorite, drive or task index */
    char *label, *path;
    HWND hwnd;                  /* tasks */
    DWORD freeKB, totalKB;      /* drives */
} NODE;

#define HEAD_H  28
#define MAXTASKS 40

HWND hSide;

static NODE *nodes;
static int nNodes, capNodes, selNode = -1, hotNode = -1, dropNode = -1, scrollY, menuNode = -1;
static int rowH = 22;
static BOOL collapsed[S_COUNT];
static BOOL hotTimer;
static HWND tasks[MAXTASKS];
static int nTasks;
static const char *const sectName[S_COUNT] = { "PROGRAMS", "FAVORITES", "COMPUTER", "RUNNING TASKS" };

/* ---- node list ------------------------------------------------------------- */

static void FreeNode(NODE *n)
{
    StrFree(&n->label);
    StrFree(&n->path);
}

static void ClearNodes(void)
{
    int i;

    for (i = 0; i < nNodes; i++)
        FreeNode(&nodes[i]);
    nNodes = 0;
}

static NODE *InsertNode(int at, int type, int depth, const char *label, const char *path)
{
    NODE *n;

    if (nNodes == capNodes) {
        int cap = capNodes ? capNodes * 2 : 64;
        NODE *p = (NODE *)realloc(nodes, cap * sizeof(NODE));

        if (!p)
            return NULL;
        nodes = p;
        capNodes = cap;
    }
    if (at < 0 || at > nNodes)
        at = nNodes;
    memmove(&nodes[at + 1], &nodes[at], (nNodes - at) * sizeof(NODE));
    nNodes++;
    n = &nodes[at];
    memset(n, 0, sizeof(*n));
    n->type = (BYTE)type;
    n->depth = (BYTE)depth;
    n->label = StrDup(label);
    n->path = path ? StrDup(path) : NULL;
    n->ref = -1;
    return n;
}

static int NodeH(int i)
{
    return nodes[i].type == N_HEAD ? HEAD_H : rowH;
}

static int NodeY(int i)
{
    int y = 6, k;

    for (k = 0; k < i; k++)
        y += NodeH(k);
    return y - scrollY;
}

static int TotalH(void)
{
    int y = 12, k;

    for (k = 0; k < nNodes; k++)
        y += NodeH(k);
    return y;
}

static int NodeAt(int y)
{
    int k, top = 6 - scrollY;

    for (k = 0; k < nNodes; k++) {
        if (y >= top && y < top + NodeH(k))
            return k;
        top += NodeH(k);
    }
    return -1;
}

static void UpdateScroll(void)
{
    RECT r;
    int maxs;

    GetClientRect(hSide, &r);
    maxs = max(0, TotalH() - r.bottom);
    if (scrollY > maxs)
        scrollY = maxs;
#ifdef RETRO_WIN32
    {
        SCROLLINFO si;

        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = TotalH() - 1;
        si.nPage = max(1, r.bottom);
        si.nPos = scrollY;
        SetScrollInfo(hSide, SB_VERT, &si, TRUE);
    }
#else
    SetScrollRange(hSide, SB_VERT, 0, maxs, FALSE);
    SetScrollPos(hSide, SB_VERT, scrollY, TRUE);
    if (!maxs)
        ShowScrollBar(hSide, SB_VERT, FALSE);
#endif
}

static void Changed(void)
{
    UpdateScroll();
    InvalidateRect(hSide, NULL, FALSE);
}

static void EnsureVisible(int i)
{
    RECT r;
    int y;

    if (i < 0)
        return;
    GetClientRect(hSide, &r);
    y = NodeY(i);
    if (y < 0)
        scrollY += y - 4;
    else if (y + NodeH(i) > r.bottom)
        scrollY += y + NodeH(i) - r.bottom + 4;
    if (scrollY < 0)
        scrollY = 0;
    Changed();
}

/* ---- folder tree ------------------------------------------------------------ */

static int CmpStr(const void *a, const void *b)
{
    return lstrcmpi(*(char *const *)a, *(char *const *)b);
}

static void Collapse(int i)
{
    int j = i + 1;

    while (j < nNodes && nodes[j].type != N_HEAD && nodes[j].depth > nodes[i].depth)
        j++;
    {
        int k;

        for (k = i + 1; k < j; k++)
            FreeNode(&nodes[k]);
        memmove(&nodes[i + 1], &nodes[j], (nNodes - j) * sizeof(NODE));
        nNodes -= j - i - 1;
        if (selNode > i && selNode < j)
            selNode = i;
        else if (selNode >= j)
            selNode -= j - i - 1;
    }
    nodes[i].expanded = 0;
}

static void Expand(int i)
{
    FINDINFO fi;
    FINDSTATE *fs;
    char **names = NULL, path[MAXP], parent[MAXP];
    int n = 0, cap = 0, k;
    HCURSOR old;

    if (nodes[i].expanded || !nodes[i].path)
        return;
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    lstrcpyn(parent, nodes[i].path, MAXP);
    fs = FsOpen(parent, "*.*", &fi);
    if (fs) {
        do {
            if (!(fi.attr & FA_DIR))
                continue;
            if (!showHidden && (fi.attr & (FA_HIDDEN | FA_SYSTEM)))
                continue;
            if (n == cap) {
                char **p;

                cap = cap ? cap * 2 : 32;
                p = (char **)realloc(names, cap * sizeof(char *));
                if (!p)
                    break;
                names = p;
            }
            names[n++] = StrDup(fi.name);
        } while (FsNext(fs, &fi));
        FsClose(fs);
    }
    if (n > 1)
        qsort(names, n, sizeof(char *), CmpStr);
    for (k = 0; k < n; k++) {
        NODE *nd;

        PathJoin(path, parent, names[k]);
        nd = InsertNode(i + 1 + k, N_DIR, nodes[i].depth + 1, names[k], path);
        if (nd)
            nd->kids = 1;
        free(names[k]);
        if (selNode > i)
            selNode++;
    }
    if (names)
        free(names);
    nodes[i].expanded = 1;
    nodes[i].kids = (BYTE)(n > 0);
    SetCursor(old);
}

static void Toggle(int i)
{
    if (nodes[i].expanded)
        Collapse(i);
    else
        Expand(i);
    Changed();
}

/* ---- building ---------------------------------------------------------------- */

static void Drives(int at, int depth)
{
    int d;
    char label[40], name[60], root[4];

    for (d = 0; d < 26; d++) {
        int k = DriveKind(d);
        NODE *n;

        if (k == DK_NONE)
            continue;
        sprintf(root, "%c:\\", 'A' + d);
        label[0] = '\0';
        if (k == DK_HARD || k == DK_RAM)
            DriveLabel(d, label, sizeof(label));
        if (label[0])
            sprintf(name, "%c:  %s", 'A' + d, label);
        else
            sprintf(name, "%c:", 'A' + d);
        n = InsertNode(at++, N_DRIVE, depth, name, root);
        if (!n)
            return;
        n->ref = (short)d;
        n->kids = 1;
        n->dk = (BYTE)k;
        if (k == DK_HARD || k == DK_RAM || k == DK_NET)
            DriveSpace(d, &n->freeKB, &n->totalKB);
    }
}

static BOOL IsTaskWnd(HWND h)
{
    return h != hFrame && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetWindowTextLength(h) > 0;
}

static int ListTasks(HWND *out, int max)
{
    HWND h = GetWindow(GetDesktopWindow(), GW_CHILD);
    int n = 0;

    for (; h && n < max; h = GetWindow(h, GW_HWNDNEXT))
        if (IsTaskWnd(h))
            out[n++] = h;
    return n;
}

/* Rebuilds the sections; open folders in the tree are kept. */
void SideRebuild(void)
{
    NODE *old = nodes;
    int nOld = nNodes, i, g, sel = -1, s;
    char selPath[MAXP];
    int selType = -1, selRef = -1;

    selPath[0] = '\0';
    if (selNode >= 0 && selNode < nNodes) {
        selType = nodes[selNode].type;
        selRef = nodes[selNode].ref;
        if (nodes[selNode].path)
            lstrcpyn(selPath, nodes[selNode].path, MAXP);
    }
    nodes = NULL;
    nNodes = capNodes = 0;
    for (s = 0; s < S_COUNT; s++) {
        NODE *h = InsertNode(-1, N_HEAD, 0, sectName[s], NULL);

        if (!h)
            break;
        h->ref = (short)s;
        if (collapsed[s])
            continue;
        switch (s) {
        case S_PROGRAMS:
            InsertNode(-1, N_PROGRAMS, 0, "All Groups", NULL);
            InsertNode(-1, N_RECENT, 0, "Recent Programs", NULL);
            for (g = 0; g < nGroups; g++) {
                NODE *n = InsertNode(-1, N_GROUP, 0, groups[g].name, NULL);

                if (n)
                    n->ref = (short)g;
            }
            break;
        case S_FAVORITES:
            for (i = 0; i < nFavs; i++) {
                NODE *n = InsertNode(-1, N_FAV, 0, PathIsRoot(favs[i]) ? favs[i] : PathName(favs[i]),
                                     favs[i]);

                if (n)
                    n->ref = (short)i;
            }
            if (!nFavs)
                InsertNode(-1, N_HINT, 0, "Ctrl+D adds a folder", NULL);
            break;
        case S_COMPUTER: {
            int at, k;

            InsertNode(-1, N_COMPUTER, 0, "My Computer", NULL);
            at = nNodes;
            /* reuse the old drive subtree when there is one */
            for (k = 0; k < nOld && old[k].type != N_DRIVE; k++)
                ;
            if (k < nOld) {
                for (; k < nOld && (old[k].type == N_DRIVE || old[k].type == N_DIR); k++) {
                    NODE *n = InsertNode(-1, old[k].type, old[k].depth, old[k].label, old[k].path);

                    if (n) {
                        n->ref = old[k].ref;
                        n->kids = old[k].kids;
                        n->expanded = old[k].expanded;
                        n->dk = old[k].dk;
                        n->freeKB = old[k].freeKB;
                        n->totalKB = old[k].totalKB;
                    }
                }
            } else {
                Drives(at, 1);
            }
            InsertNode(-1, N_WASTE, 0, "Wastebasket", NULL);
            break;
        }
        case S_TASKS:
            nTasks = ListTasks(tasks, MAXTASKS);
            for (i = 0; i < nTasks; i++) {
                char title[80];
                NODE *n;

                GetWindowText(tasks[i], title, sizeof(title));
                n = InsertNode(-1, N_TASK, 0, title, NULL);
                if (n) {
                    n->hwnd = tasks[i];
                    n->ref = (short)i;
                }
            }
            if (!nTasks)
                InsertNode(-1, N_HINT, 0, "No other programs", NULL);
            break;
        }
    }
    for (i = 0; i < nOld; i++)
        FreeNode(&old[i]);
    if (old)
        free(old);
    /* restore the selection */
    for (i = 0; i < nNodes && selType >= 0; i++) {
        if (nodes[i].type != selType)
            continue;
        if (nodes[i].path && selPath[0] ? lstrcmpi(nodes[i].path, selPath) == 0 : nodes[i].ref == selRef) {
            sel = i;
            break;
        }
    }
    selNode = sel;
    hotNode = dropNode = -1;
    if (hSide)
        Changed();
}

/* Reads the drive list again (labels, new or removed network drives). */
void SideRebuildDrives(void)
{
    int i = 0, j;

    while (i < nNodes) {
        if (nodes[i].type == N_DRIVE || nodes[i].type == N_DIR) {
            FreeNode(&nodes[i]);
            memmove(&nodes[i], &nodes[i + 1], (nNodes - i - 1) * sizeof(NODE));
            nNodes--;
        } else {
            i++;
        }
    }
    for (j = 0; j < nNodes; j++)
        if (nodes[j].type == N_COMPUTER) {
            Drives(j + 1, 1);
            break;
        }
    selNode = -1;
    SideSync();
}

void SideRefreshDrives(void)
{
    int i;

    for (i = 0; i < nNodes; i++)
        if (nodes[i].type == N_DRIVE && nodes[i].totalKB)
            DriveSpace(nodes[i].ref, &nodes[i].freeKB, &nodes[i].totalKB);
    if (hSide)
        InvalidateRect(hSide, NULL, FALSE);
}

BOOL SideTasksChanged(void)
{
    HWND now[MAXTASKS];
    int n = ListTasks(now, MAXTASKS), i;
    BOOL changed = n != nTasks;

    if (collapsed[S_TASKS])
        return FALSE;
    for (i = 0; !changed && i < n; i++)
        changed = now[i] != tasks[i];
    if (!changed) {
        /* titles change too ("Notepad - FOO.TXT") */
        for (i = 0; i < nNodes; i++)
            if (nodes[i].type == N_TASK) {
                char title[80];

                GetWindowText(nodes[i].hwnd, title, sizeof(title));
                if (strcmp(title, nodes[i].label) != 0) {
                    changed = TRUE;
                    break;
                }
            }
    }
    if (changed)
        SideRebuild();
    return changed;
}

/* ---- selection and navigation --------------------------------------------- */

static void Select(int i, BOOL go)
{
    NODE *n;

    if (i < 0 || i >= nNodes)
        return;
    n = &nodes[i];
    if (n->type == N_HEAD || n->type == N_HINT)
        return;
    selNode = i;
    EnsureVisible(i);
    InvalidateRect(hSide, NULL, FALSE);
    if (!go)
        return;
    switch (n->type) {
    case N_PROGRAMS: NavKind(LOC_PROGRAMS, 0); break;
    case N_RECENT:   NavKind(LOC_RECENT, -1); break;
    case N_GROUP:    NavKind(LOC_GROUP, n->ref); break;
    case N_COMPUTER: NavKind(LOC_COMPUTER, 0); break;
    case N_WASTE:    NavKind(LOC_WASTE, 0); break;
    case N_FAV:
    case N_DRIVE:
    case N_DIR:
        if (n->type == N_DRIVE && !n->totalKB && n->dk != DK_FLOPPY)
            DriveSpace(n->ref, &n->freeKB, &n->totalKB);
        NavDir(n->path);
        break;
    case N_TASK:
        if (IsWindow(n->hwnd)) {
            /* as the Task List does: SC_RESTORE also wakes up MS-DOS
             * sessions, which ShowWindow() leaves as icons */
            if (IsIconic(n->hwnd))
                PostMessage(n->hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
#ifdef RETRO_WIN32
            SetForegroundWindow(n->hwnd);
#else
            BringWindowToTop(n->hwnd);
            SetActiveWindow(n->hwnd);
#endif
        }
        break;
    }
}

/* Selects the node for the current location, opening the folder tree. */
void SideSync(void)
{
    int i, found = -1;

    for (i = 0; i < nNodes && found < 0; i++) {
        NODE *n = &nodes[i];

        switch (cur.kind) {
        case LOC_PROGRAMS: if (n->type == N_PROGRAMS) found = i; break;
        case LOC_RECENT:   if (n->type == N_RECENT) found = i; break;
        case LOC_GROUP:    if (n->type == N_GROUP && n->ref == cur.group) found = i; break;
        case LOC_COMPUTER: if (n->type == N_COMPUTER) found = i; break;
        case LOC_WASTE:    if (n->type == N_WASTE) found = i; break;
        case LOC_DIR:
            if ((n->type == N_DRIVE || n->type == N_DIR) && n->path && PathInside(cur.path, n->path)) {
                if (lstrcmpi(n->path, cur.path) == 0)
                    found = i;
                else if (!n->expanded)
                    Expand(i);
            }
            break;
        }
    }
    if (found < 0 && cur.kind == LOC_DIR)
        for (i = 0; i < nNodes; i++)
            if (nodes[i].type == N_FAV && lstrcmpi(nodes[i].path, cur.path) == 0) {
                found = i;
                break;
            }
    selNode = found;
    Changed();
    if (found >= 0)
        EnsureVisible(found);
}

/* Keyboard task switching: selects the first running program. */
void SideFocusTasks(void)
{
    int i;

    if (collapsed[S_TASKS]) {
        collapsed[S_TASKS] = FALSE;
        SideRebuild();
    } else {
        SideTasksChanged();
    }
    SetFocus(hSide);
    for (i = 0; i < nNodes; i++)
        if (nodes[i].type == N_TASK) {
            Select(i, FALSE);
            return;
        }
    MessageBeep(0);
}

void SideFocusNode(void)
{
    SetFocus(hSide);
    if (selNode < 0) {
        int i;

        for (i = 0; i < nNodes; i++)
            if (nodes[i].type != N_HEAD && nodes[i].type != N_HINT) {
                Select(i, FALSE);
                break;
            }
    }
}

/* ---- drag and drop ----------------------------------------------------------- */

void SideSetDropHilite(int node)
{
    if (node == dropNode)
        return;
    dropNode = node;
    InvalidateRect(hSide, NULL, FALSE);
    UpdateWindow(hSide);
}

int SideDropTarget(POINT pt, char *path, int *group)
{
    int i = NodeAt(pt.y);
    NODE *n;

    if (i < 0) {
        SideSetDropHilite(-1);
        return DT_NONE;
    }
    n = &nodes[i];
    switch (n->type) {
    case N_GROUP:
        *group = n->ref;
        SideSetDropHilite(i);
        return DT_GROUP;
    case N_FAV:
    case N_DRIVE:
    case N_DIR:
        lstrcpyn(path, n->path, MAXP);
        SideSetDropHilite(i);
        return DT_DIR;
    case N_WASTE:
        SideSetDropHilite(i);
        return DT_WASTE;
    }
    SideSetDropHilite(-1);
    return DT_NONE;
}

/* ---- painting ---------------------------------------------------------------- */

static int Glyph(const NODE *n)
{
    switch (n->type) {
    case N_PROGRAMS: return G_GROUP;
    case N_RECENT:   return G_RECENT;
    case N_GROUP:    return G_GROUP;
    case N_FAV:      return G_STAR;
    case N_COMPUTER: return G_COMPUTER;
    case N_WASTE:    return G_WASTE;
    case N_TASK:     return G_TASK;
    case N_DIR:      return n->expanded ? G_FOLDEROPEN : G_FOLDER;
    case N_DRIVE:
        switch (n->dk) {
        case DK_FLOPPY: return G_FLOPPY;
        case DK_CDROM:  return G_CDROM;
        case DK_NET:    return G_NETDRIVE;
        case DK_RAM:    return G_RAMDISK;
        }
        return G_HARDDISK;
    }
    return G_FOLDER;
}

static void PaintNode(HDC hdc, int i, int y, int w)
{
    NODE *n = &nodes[i];
    RECT r;
    int x;
    BOOL sel = i == selNode, hot = i == hotNode || i == dropNode;
    COLORREF bg = th.side, fg = th.sideText;

    if (n->type == N_HEAD) {
        SelectObject(hdc, fontHead);
        SetRect(&r, 14, y + 10, w - 24, y + HEAD_H - 2);
        TextIn(hdc, &r, n->label, th.sideHead, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        Triangle(hdc, w - 20, y + 13, !collapsed[n->ref], Mix(th.sideHead, th.side, 30));
        HLine(hdc, 14, w - 10, y + HEAD_H - 2, Mix(th.sideHead, th.side, 75));
        return;
    }
    SetRect(&r, 6, y + 1, w - 6, y + rowH - 1);
    if (sel) {
        Pill(hdc, &r, th.sideSel, th.sideSel);
        bg = th.sideSel;
        fg = th.sideSelText;
        if (GetFocus() == hSide) {
            RECT f = r;

            InflateRect(&f, -2, -2);
            SetTextColor(hdc, PC(fg));
            DrawFocusRect(hdc, &f);
        }
    } else if (hot) {
        Pill(hdc, &r, th.sideHot, i == dropNode ? th.sideSel : th.sideHot);
        bg = th.sideHot;
    }
    x = 12 + n->depth * 14;
    if ((n->type == N_DRIVE || n->type == N_DIR) && n->kids)
        Triangle(hdc, x, y + (rowH - 7) / 2, n->expanded, sel ? fg : Mix(th.sideText, th.side, 45));
    x += 12;
    if (n->type == N_TASK) {
        #ifdef RETRO_WIN32
        HICON h = (HICON)GetClassLong(n->hwnd, GCL_HICON);
#else
        HICON h = (HICON)GetClassWord(n->hwnd, GCW_HICON);
#endif

        if (h)
            DrawIconSmall(hdc, x, y + (rowH - 16) / 2, h, bg);
        else
            DrawGlyph(hdc, x, y + (rowH - 16) / 2, G_TASK);
    } else if (n->type != N_HINT) {
        DrawGlyph(hdc, x, y + (rowH - 16) / 2, Glyph(n));
    }
    x += 22;
    SelectObject(hdc, n->type == N_HINT ? fontSmall : fontUI);
    SetRect(&r, x, y, w - 10, y + rowH);
    if (n->type == N_DRIVE && n->totalKB && w - x > 110) {
        RECT g;

        r.right = w - 56;
        SetRect(&g, w - 52, y + rowH / 2 - 3, w - 14, y + rowH / 2 + 3);
        DrawGauge(hdc, &g, n->freeKB, n->totalKB, Mix(bg, RGB(255, 255, 255), 10));
    }
    TextIn(hdc, &r, n->label, n->type == N_HINT ? Mix(th.sideText, th.side, 45) : fg,
           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void Paint(HWND hwnd, HDC hdc, const RECT *rc)
{
    RECT cl;
    int i, y = 6 - scrollY;

    GetClientRect(hwnd, &cl);
    FillR(hdc, rc, th.side);
    VLine(hdc, cl.right - 1, 0, cl.bottom, Mix(th.side, RGB(0, 0, 0), 25));
    for (i = 0; i < nNodes; i++) {
        int h = NodeH(i);

        if (y + h >= rc->top && y <= rc->bottom)
            PaintNode(hdc, i, y, cl.right - 1);
        y += h;
    }
}

/* ---- context menu ----------------------------------------------------------- */

static void NodeMenu(int i, POINT pt)
{
    HMENU m = CreatePopupMenu();
    NODE *n = &nodes[i];

    menuNode = i;
    switch (n->type) {
    case N_GROUP:
        AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
        AppendMenu(m, MF_STRING, IDM_NEWPROG, "New &Program...");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        AppendMenu(m, MF_STRING, IDM_RENGROUP, "&Rename Group...");
        AppendMenu(m, MF_STRING, IDM_DELGROUP, "&Delete Group");
        break;
    case N_PROGRAMS:
    case N_RECENT:
        AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
        AppendMenu(m, MF_STRING, IDM_NEWGROUP, "New &Group...");
        AppendMenu(m, MF_STRING, IDM_IMPORT, "&Import Program Manager Groups");
        break;
    case N_FAV:
        AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
        AppendMenu(m, MF_STRING, IDM_REMFAV, "&Remove from Favorites");
        break;
    case N_DRIVE:
    case N_DIR:
        AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
        AppendMenu(m, MF_STRING, IDM_ADDFAV, "Add to &Favorites");
        AppendMenu(m, MF_STRING, IDM_FIND, "F&ind Files Here...");
        AppendMenu(m, MF_STRING, IDM_DOSPROMPT, "MS-DOS &Prompt Here");
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        AppendMenu(m, MF_STRING, IDM_PROPS, "P&roperties");
        break;
    case N_WASTE:
        AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
        AppendMenu(m, nWaste ? MF_STRING : MF_GRAYED, IDM_EMPTYWASTE, "&Empty Wastebasket");
        break;
    case N_TASK:
        AppendMenu(m, MF_STRING, IDM_SWITCHTASK, "&Switch To");
        AppendMenu(m, MF_STRING, IDM_CLOSETASK, "&Close");
        break;
    case N_COMPUTER:
        AppendMenu(m, MF_STRING, IDM_OPEN, "&Open");
        AppendMenu(m, MF_STRING, IDM_REFRESH, "&Refresh Drives");
        break;
    default:
        DestroyMenu(m);
        return;
    }
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hSide, NULL);
    DestroyMenu(m);
}

static void NodeCommand(int id)
{
    NODE *n;
    int i = menuNode;

    if (i < 0 || i >= nNodes)
        return;
    n = &nodes[i];
    switch (id) {
    case IDM_OPEN:
    case IDM_SWITCHTASK:
        Select(i, TRUE);
        break;
    case IDM_CLOSETASK:
        if (IsWindow(n->hwnd))
            PostMessage(n->hwnd, WM_CLOSE, 0, 0);
        break;
    case IDM_REMFAV:
        FavRemove(n->ref);
        GroupsSave();
        SideRebuild();
        break;
    case IDM_REFRESH:
        SideRefreshDrives();
        if (cur.kind == LOC_COMPUTER)
            NavRefresh();
        break;
    case IDM_RENGROUP:
    case IDM_DELGROUP:
    case IDM_NEWPROG:
        if (cur.kind != LOC_GROUP || cur.group != n->ref)
            Select(i, TRUE);
        Command(id);
        break;
    case IDM_ADDFAV:
    case IDM_FIND:
    case IDM_DOSPROMPT:
    case IDM_PROPS:
        Select(i, TRUE);
        Command(id == IDM_PROPS ? IDM_PROPS + 10000 : id);
        break;
    default:
        Command(id);
    }
}

/* ---- window ---------------------------------------------------------------- */

static void ScrollTo(int y)
{
    RECT r;

    GetClientRect(hSide, &r);
    y = max(0, min(y, TotalH() - r.bottom));
    if (y != scrollY) {
        scrollY = y;
        Changed();
    }
}

static int Next(int i, int dir)
{
    for (i += dir; i >= 0 && i < nNodes; i += dir)
        if (nodes[i].type != N_HEAD && nodes[i].type != N_HINT)
            return i;
    return -1;
}

LRESULT CALLBACK __export SideProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    POINT pt;
    int i;

    switch (msg) {
    case WM_CREATE:
        hSide = hwnd;
        rowH = max(22, lineH + 9);
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
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_VSCROLL:
        switch (SCROLL_CODE(wParam)) {
        case SB_LINEUP:   ScrollTo(scrollY - rowH); break;
        case SB_LINEDOWN: ScrollTo(scrollY + rowH); break;
        case SB_PAGEUP:   ScrollTo(scrollY - 200); break;
        case SB_PAGEDOWN: ScrollTo(scrollY + 200); break;
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

    case WM_MOUSEWHEEL:
        ScrollTo(scrollY - (short)HIWORD(wParam) / 120 * 3 * rowH);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(hwnd);
        i = NodeAt(MOUSE_Y(lParam));
        if (i < 0)
            return 0;
        if (nodes[i].type == N_HEAD) {
            collapsed[nodes[i].ref] = !collapsed[nodes[i].ref];
            SideRebuild();
            if (!collapsed[S_COMPUTER] || cur.kind != LOC_DIR)
                SideSync();
            return 0;
        }
        if ((nodes[i].type == N_DRIVE || nodes[i].type == N_DIR) && nodes[i].kids) {
            int ax = 12 + nodes[i].depth * 14;

            if (MOUSE_X(lParam) >= ax - 4 && MOUSE_X(lParam) < ax + 11) {
                Toggle(i);
                return 0;
            }
        }
        if (msg == WM_LBUTTONDBLCLK && (nodes[i].type == N_DRIVE || nodes[i].type == N_DIR)) {
            Toggle(i);
            return 0;
        }
        Select(i, TRUE);
        return 0;

    case WM_RBUTTONUP:
        i = NodeAt(MOUSE_Y(lParam));
        if (i >= 0) {
            pt.x = MOUSE_X(lParam);
            pt.y = MOUSE_Y(lParam);
            ClientToScreen(hwnd, &pt);
            NodeMenu(i, pt);
        }
        return 0;

    case WM_COMMAND:
        NodeCommand(CMD_ID(wParam, lParam));
        return 0;

    case WM_MOUSEMOVE:
        i = NodeAt(MOUSE_Y(lParam));
        if (i >= 0 && (nodes[i].type == N_HEAD || nodes[i].type == N_HINT))
            i = -1;
        if (i != hotNode) {
            hotNode = i;
            InvalidateRect(hwnd, NULL, FALSE);
            if (!hotTimer) {
                SetTimer(hwnd, 1, 150, NULL);
                hotTimer = TRUE;
            }
        }
        return 0;

    case WM_TIMER:
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) != hwnd) {
            KillTimer(hwnd, 1);
            hotTimer = FALSE;
            if (hotNode >= 0) {
                hotNode = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_SYSKEYDOWN:
        /* F10 is a system key: Shift+F10 opens the context menu */
        if (wParam != VK_F10 || GetKeyState(VK_SHIFT) >= 0)
            break;
        /* fall through */
    case WM_KEYDOWN:
        i = selNode;
        switch (wParam) {
        case VK_UP:
        case VK_DOWN:
            i = Next(i, wParam == VK_UP ? -1 : 1);
            if (i >= 0)
                Select(i, TRUE);
            return 0;
        case VK_HOME:
            Select(Next(-1, 1), TRUE);
            return 0;
        case VK_END:
            Select(Next(nNodes, -1), FALSE);    /* the last node: often a task */
            return 0;
        case 0x5D:                              /* VK_APPS: the context menu */
        case VK_F10:
            if (wParam == VK_F10 && GetKeyState(VK_SHIFT) >= 0)
                break;
            if (i >= 0) {
                pt.x = 40;
                pt.y = NodeY(i) + rowH;
                ClientToScreen(hwnd, &pt);
                NodeMenu(i, pt);
            }
            return 0;
        case VK_RIGHT:
            if (i >= 0 && (nodes[i].type == N_DRIVE || nodes[i].type == N_DIR) && !nodes[i].expanded)
                Toggle(i);
            return 0;
        case VK_LEFT:
            if (i >= 0 && nodes[i].expanded) {
                Toggle(i);
            } else if (i >= 0 && nodes[i].type == N_DIR) {
                int d = nodes[i].depth;

                while (--i >= 0 && nodes[i].depth >= d)
                    ;
                if (i >= 0)
                    Select(i, TRUE);
            }
            return 0;
        case VK_RETURN:
            if (i >= 0)
                Select(i, TRUE);
            if (i < 0 || nodes[i].type != N_TASK)   /* keep the switched-to task active */
                FocusView();
            return 0;
        case VK_DELETE:
            if (i >= 0 && nodes[i].type == N_GROUP)
                Command(IDM_DELGROUP);
            else if (i >= 0 && nodes[i].type == N_FAV) {
                menuNode = i;
                NodeCommand(IDM_REMFAV);
            }
            return 0;
        }
        break;

    case WM_DROPFILES:
        OpsDropFiles(hwnd, (HDROP)wParam);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        ClearNodes();
        if (nodes)
            free(nodes);
        nodes = NULL;
        capNodes = 0;
        hSide = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL SideRegister(void)
{
    WNDCLASS wc;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = (WNDPROC)SideProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ShellIVSide";
    return RegisterClass(&wc) != 0;
}

void SideLoadState(void)
{
    int s;
    char key[16];

    for (s = 0; s < S_COUNT; s++) {
        sprintf(key, "Collapsed%d", s);
        collapsed[s] = IniInt("Sidebar", key, 0) != 0;
    }
}

void SideSaveState(void)
{
    int s;
    char key[16];

    for (s = 0; s < S_COUNT; s++) {
        sprintf(key, "Collapsed%d", s);
        IniPutInt("Sidebar", key, collapsed[s]);
    }
}

void SideDone(void)
{
    if (hSide)
        DestroyWindow(hSide);
}
