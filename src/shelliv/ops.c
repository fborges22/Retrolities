/* ShellIV - file operations: copy, move, delete with progress, the
 * wastebasket (recoverable deletes), the file clipboard and dropping. */
#include <ctype.h>
#include "shelliv.h"
#ifndef RETRO_WIN32
#include <dos.h>
#endif

#define WASTEDIR    "SHELLIV.WST"
#define WASTEINDEX  "INDEX.INF"

HWND hProgress;
WASTEENT *waste;
int nWaste;

static BOOL busy, cancelled;
static PATHLIST clip;
static BOOL clipCut;
static DWORD totalBytes, doneBytes, fileBase;
static int replaceAll;          /* CONF_ALL: overwrite without asking */
static long barPos, barMax;
static HWND hBar;

/* ---- path lists ------------------------------------------------------------- */

void PlInit(PATHLIST *pl)
{
    pl->n = pl->cap = 0;
    pl->p = NULL;
}

void PlAdd(PATHLIST *pl, const char *path)
{
    if (pl->n == pl->cap) {
        int cap = pl->cap ? pl->cap * 2 : 16;
        char **n = (char **)realloc(pl->p, cap * sizeof(char *));

        if (!n)
            return;
        pl->p = n;
        pl->cap = cap;
    }
    pl->p[pl->n++] = StrDup(path);
}

void PlFree(PATHLIST *pl)
{
    int i;

    for (i = 0; i < pl->n; i++)
        free(pl->p[i]);
    if (pl->p)
        free(pl->p);
    PlInit(pl);
}

void SelectedPaths(PATHLIST *pl)
{
    char path[MAXP];
    int i;

    PlInit(pl);
    for (i = 0; i < nItems; i++) {
        ITEM *it = items[i];

        if (!it->sel)
            continue;
        if (it->kind == IK_FILE || it->kind == IK_DIR || it->kind == IK_WASTE) {
            ViewItemPath(it, path);
            if (path[0])
                PlAdd(pl, path);
        }
    }
}

BOOL OpsBusy(void)
{
    return busy;
}

/* ---- progress dialog ---------------------------------------------------------- */

LRESULT CALLBACK __export BarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r, f;
        int w;

        GetClientRect(hwnd, &r);
        FillR(hdc, &r, RGB(255, 255, 255));
        FrameR(hdc, &r, Mix(th.accent, RGB(128, 128, 128), 50));
        InflateRect(&r, -2, -2);
        w = barMax > 0 ? (int)((r.right - r.left) * (barPos / 64) / max(1L, barMax / 64)) : 0;
        w = min(w, r.right - r.left);
        SetRect(&f, r.left, r.top, r.left + w, r.bottom);
        Gradient(hdc, &f, Mix(th.gauge, RGB(255, 255, 255), 40), th.gauge, TRUE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

DLGRESULT CALLBACK __export ProgressProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        CenterWindow(hDlg);
        hBar = GetDlgItem(hDlg, IDC_BAR);
        return TRUE;
    case WM_COMMAND:
        if (CMD_ID(wParam, lParam) == IDCANCEL)
            cancelled = TRUE;
        return TRUE;
    }
    return FALSE;
}

static FARPROC progThunk;

static void ProgressOpen(const char *title)
{
    cancelled = FALSE;
    barPos = 0;
    progThunk = THUNK(ProgressProc);
    hProgress = CreateDialog(hInst, MAKEINTRESOURCE(IDD_PROGRESS), hFrame, (DLGPROC)progThunk);
    if (hProgress) {
        SetWindowText(hProgress, title);
        ShowWindow(hProgress, SW_SHOW);
        UpdateWindow(hProgress);
        EnableWindow(hFrame, FALSE);
    }
    SetBusy(TRUE);
}

static void ProgressClose(void)
{
    EnableWindow(hFrame, TRUE);
    if (hProgress)
        DestroyWindow(hProgress);
    hProgress = NULL;
    hBar = NULL;
    if (progThunk)
        FREETHUNK(progThunk);
    progThunk = NULL;
    SetBusy(FALSE);
    SetActiveWindow(hFrame);
}

static void ProgressText(const char *verb, const char *from, const char *to)
{
    char buf[MAXP + 40];

    if (!hProgress)
        return;
    sprintf(buf, "%s %.200s", verb, from);
    SetDlgItemText(hProgress, IDC_FROM, buf);
    if (to) {
        sprintf(buf, "to %.200s", to);
        SetDlgItemText(hProgress, IDC_TO, buf);
    } else {
        SetDlgItemText(hProgress, IDC_TO, "");
    }
}

static BOOL Tick(void)
{
    static DWORD last;

    if (GetTickCount() - last > 80) {
        last = GetTickCount();
        barPos = (long)doneBytes;
        barMax = (long)totalBytes;
        if (hBar)
            InvalidateRect(hBar, NULL, FALSE);
        if (!PumpMessages())
            cancelled = TRUE;
    }
    return !cancelled;
}

static BOOL CopyTick(DWORD done)
{
    doneBytes = fileBase + done;
    return Tick();
}

/* ---- recursive helpers ---------------------------------------------------- */

static DWORD TreeSize(const char *path, int depth)
{
    FINDINFO fi;
    FINDSTATE *fs;
    DWORD total = 0;
    char *sub;
    int a = GetAttr(path);

    if (a < 0)
        return 0;
    if (!(a & FA_DIR)) {
        WORD d, t;
        DWORD s;

        return GetStamp(path, &d, &t, &s) ? s : 0;
    }
    if (depth > 30)
        return 0;
    sub = (char *)malloc(MAXP);
    if (!sub)
        return 0;
    fs = FsOpen(path, "*.*", &fi);
    if (fs) {
        do {
            PathJoin(sub, path, fi.name);
            total += (fi.attr & FA_DIR) ? TreeSize(sub, depth + 1) : fi.size;
        } while (FsNext(fs, &fi));
        FsClose(fs);
    }
    free(sub);
    return total;
}

static BOOL DeleteTree(const char *path)
{
    FINDINFO fi;
    FINDSTATE *fs;
    char *sub;
    int a = GetAttr(path);
    BOOL ok = TRUE;

    if (a < 0)
        return TRUE;
    if (!(a & FA_DIR)) {
        if (a & (FA_RDONLY | FA_HIDDEN | FA_SYSTEM))
            SetAttr(path, 0);
        ProgressText("Deleting", path, NULL);
        if (!Tick())
            return FALSE;
        return DeleteOne(path);
    }
    sub = (char *)malloc(MAXP);
    if (!sub)
        return FALSE;
    for (;;) {
        /* restart the listing after deleting: some file systems lose the place */
        fs = FsOpen(path, "*.*", &fi);
        if (!fs)
            break;
        PathJoin(sub, path, fi.name);
        FsClose(fs);
        if (!DeleteTree(sub)) {
            ok = FALSE;
            break;
        }
    }
    free(sub);
    if (ok) {
        if (a & (FA_RDONLY | FA_HIDDEN | FA_SYSTEM))
            SetAttr(path, 0);
        ok = RemoveDir(path);
    }
    return ok;
}

static BOOL Overwrite(const char *dst)
{
    char buf[MAXP + 80];
    int r;

    if (replaceAll == CONF_ALL || !confirmOps)
        return TRUE;
    sprintf(buf, "\"%s\" already exists.\n\nDo you want to replace it?", dst);
    if (hProgress)
        EnableWindow(hProgress, FALSE);
    r = DlgConfirm("Confirm File Replace", buf, TRUE);
    if (hProgress)
        EnableWindow(hProgress, TRUE);
    if (r == CONF_CANCEL)
        cancelled = TRUE;
    if (r == CONF_ALL)
        replaceAll = CONF_ALL;
    return r == CONF_YES || r == CONF_ALL;
}

static BOOL CopyTree(const char *src, const char *dst, int depth)
{
    FINDINFO fi;
    FINDSTATE *fs;
    char *a, *b;
    BOOL ok = TRUE;
    int attr = GetAttr(src);

    if (attr < 0 || cancelled)
        return FALSE;
    if (!(attr & FA_DIR)) {
        int da = GetAttr(dst);

        if (da >= 0) {
            if (da & FA_DIR)
                return FALSE;
            if (!Overwrite(dst))
                return !cancelled;
            if (da & FA_RDONLY)
                SetAttr(dst, 0);
        }
        ProgressText("Copying", src, dst);
        fileBase = doneBytes;
        ok = CopyFileData(src, dst, CopyTick);
        if (!ok && !cancelled) {
            if (Msg(MB_OKCANCEL | MB_ICONSTOP, "Cannot copy \"%s\".\n\nThe disk may be full or "
                    "write-protected.", src) == IDCANCEL)
                cancelled = TRUE;
        }
        return ok;
    }
    if (depth > 30)
        return FALSE;
    if (!IsDir(dst) && !MakeDir(dst)) {
        Msg(MB_ICONSTOP, "Cannot create the folder \"%s\".", dst);
        return FALSE;
    }
    a = (char *)malloc(MAXP);
    b = (char *)malloc(MAXP);
    if (!a || !b) {
        if (a) free(a);
        if (b) free(b);
        return FALSE;
    }
    fs = FsOpen(src, "*.*", &fi);
    if (fs) {
        do {
            PathJoin(a, src, fi.name);
            PathJoin(b, dst, fi.name);
            if (!CopyTree(a, b, depth + 1))
                ok = FALSE;
        } while (!cancelled && FsNext(fs, &fi));
        FsClose(fs);
    }
    free(a);
    free(b);
    return ok && !cancelled;
}

/* Moves within one drive: renames, falling back to moving the contents
 * one by one (DOS cannot move directories to another parent). */
static BOOL MoveTree(const char *src, const char *dst, int depth)
{
    FINDINFO fi;
    FINDSTATE *fs;
    char *a, *b;
    BOOL ok = TRUE;
    int attr = GetAttr(src);

    if (attr < 0)
        return FALSE;
    ProgressText("Moving", src, dst);
    if (!Tick())
        return FALSE;
    if (!(attr & FA_DIR)) {
        int da = GetAttr(dst);

        if (da >= 0) {
            if ((da & FA_DIR) || !Overwrite(dst))
                return FALSE;
            SetAttr(dst, 0);
            DeleteOne(dst);
        }
        if (RenameOne(src, dst)) {
            WORD d, t;
            DWORD s;

            if (GetStamp(dst, &d, &t, &s))
                doneBytes += s;
            return TRUE;
        }
        return FALSE;
    }
    if (!Exists(dst) && RenameOne(src, dst))
        return TRUE;
    if (depth > 30 || (!IsDir(dst) && !MakeDir(dst)))
        return FALSE;
    a = (char *)malloc(MAXP);
    b = (char *)malloc(MAXP);
    if (!a || !b) {
        if (a) free(a);
        if (b) free(b);
        return FALSE;
    }
    for (;;) {
        fs = FsOpen(src, "*.*", &fi);
        if (!fs)
            break;
        FsClose(fs);
        PathJoin(a, src, fi.name);
        PathJoin(b, dst, fi.name);
        if (!MoveTree(a, b, depth + 1)) {
            ok = FALSE;
            break;
        }
    }
    free(a);
    free(b);
    if (ok) {
        SetAttr(dst, attr & ~FA_DIR);
        if (attr & FA_RDONLY)
            SetAttr(src, 0);
        RemoveDir(src);
    }
    return ok;
}

static void AfterOps(void)
{
    SideRefreshDrives();
    SideRebuild();
    NavRefresh();
}

/* ---- copy and move ---------------------------------------------------------- */

BOOL OpsCopyMove(PATHLIST *pl, const char *destDir, BOOL move)
{
    char dst[MAXP], dir[MAXP];
    int i, failed = 0;

    if (busy || !pl->n)
        return FALSE;
    if (!IsDir(destDir)) {
        Msg(MB_ICONEXCLAMATION, "The destination \"%s\" is not a folder.", destDir);
        return FALSE;
    }
    for (i = 0; i < pl->n; i++) {
        if (IsDir(pl->p[i]) && PathInside(destDir, pl->p[i])) {
            Msg(MB_ICONEXCLAMATION, "Cannot %s \"%s\" into itself.", move ? "move" : "copy",
                PathName(pl->p[i]));
            return FALSE;
        }
    }
    busy = TRUE;
    replaceAll = 0;
    ProgressOpen(move ? "Moving" : "Copying");
    totalBytes = doneBytes = 0;
    ProgressText("Preparing", destDir, NULL);
    for (i = 0; i < pl->n && !cancelled; i++) {
        totalBytes += TreeSize(pl->p[i], 0);
        if (!PumpMessages())
            cancelled = TRUE;
    }
    for (i = 0; i < pl->n && !cancelled; i++) {
        const char *src = pl->p[i];

        PathJoin(dst, destDir, PathName(src));
        lstrcpyn(dir, src, MAXP);
        PathParent(dir);
        if (lstrcmpi(src, dst) == 0) {
            if (move)
                continue;
            UniqueName(dst);
        }
        if (move && toupper(src[0]) == toupper(dst[0]) && src[1] == ':' && dst[1] == ':') {
            if (!MoveTree(src, dst, 0))
                failed++;
        } else {
            if (CopyTree(src, dst, 0)) {
                if (move && !cancelled && !DeleteTree(src))
                    failed++;
            } else {
                failed++;
            }
        }
    }
    ProgressClose();
    busy = FALSE;
    if (failed && !cancelled)
        Msg(MB_ICONEXCLAMATION, "%d item%s could not be %s.", failed, failed == 1 ? "" : "s",
            move ? "moved" : "copied");
    AfterOps();
    return !failed && !cancelled;
}

/* ---- wastebasket -------------------------------------------------------------- */

static void WasteDir(int drive, char *out)
{
    sprintf(out, "%c:\\%s", 'A' + drive, WASTEDIR);
}

BOOL WasteUsable(const char *path)
{
    if (path[1] != ':')
        return FALSE;
    return DriveKind((path[0] & 0xDF) - 'A') == DK_HARD;
}

void WasteFree(void)
{
    int i;

    for (i = 0; i < nWaste; i++) {
        StrFree(&waste[i].stored);
        StrFree(&waste[i].origin);
    }
    if (waste)
        free(waste);
    waste = NULL;
    nWaste = 0;
}

static void WasteAppend(const char *stored, const char *origin, WORD date, WORD time,
                        DWORD size, int attr)
{
    WASTEENT *n = (WASTEENT *)realloc(waste, (nWaste + 1) * sizeof(WASTEENT));

    if (!n)
        return;
    waste = n;
    waste[nWaste].stored = StrDup(stored);
    waste[nWaste].origin = StrDup(origin);
    waste[nWaste].date = date;
    waste[nWaste].time = time;
    waste[nWaste].size = size;
    waste[nWaste].attr = (BYTE)attr;
    nWaste++;
}

void WasteLoad(void)
{
    int d;
    char dir[MAXP], index[MAXP], line[2 * MAXP + 60], stored[MAXP];
    FILE *f;

    WasteFree();
    for (d = 2; d < 26; d++) {
        if (DriveKind(d) != DK_HARD)
            continue;
        WasteDir(d, dir);
        PathJoin(index, dir, WASTEINDEX);
        f = fopen(index, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            char *p[6], *s = line;
            int k;

            for (k = 0; k < 6; k++) {
                p[k] = s;
                s = strchr(s, '|');
                if (!s)
                    break;
                *s++ = '\0';
            }
            if (k < 5)
                continue;
            PathJoin(stored, dir, p[0]);
            if (!Exists(stored))
                continue;
            WasteAppend(stored, p[1], (WORD)strtoul(p[2], NULL, 10), (WORD)strtoul(p[3], NULL, 10),
                        strtoul(p[4], NULL, 10), atoi(p[5]));
        }
        fclose(f);
    }
}

/* Rewrites the index files of all drives from the waste[] table. */
static void WasteSave(void)
{
    int d, i;
    char dir[MAXP], index[MAXP];
    FILE *f;

    for (d = 2; d < 26; d++) {
        BOOL any = FALSE;

        if (DriveKind(d) != DK_HARD)
            continue;
        WasteDir(d, dir);
        if (!IsDir(dir))
            continue;
        PathJoin(index, dir, WASTEINDEX);
        f = fopen(index, "w");
        if (!f)
            continue;
        for (i = 0; i < nWaste; i++) {
            if ((waste[i].stored[0] & 0xDF) - 'A' != d)
                continue;
            fprintf(f, "%s|%s|%u|%u|%lu|%d\n", PathName(waste[i].stored), waste[i].origin,
                    waste[i].date, waste[i].time, waste[i].size, waste[i].attr);
            any = TRUE;
        }
        fclose(f);
        if (!any) {
            DeleteOne(index);
            SetAttr(dir, 0);
            RemoveDir(dir);
        }
    }
}

static BOOL ToWaste(const char *path)
{
    char dir[MAXP], stored[MAXP], name[20];
    const char *ext = PathExt(path);
    long n;
    WORD d, t;
    DWORD size;
    int attr = GetAttr(path);

    WasteDir((path[0] & 0xDF) - 'A', dir);
    if (!IsDir(dir)) {
        if (!MakeDir(dir))
            return FALSE;
        SetAttr(dir, FA_HIDDEN);
    }
    n = IniInt("Waste", "Next", 1);
    do {
        sprintf(name, "W%07ld%.4s", n++, (attr & FA_DIR) ? "" : ext);
        PathJoin(stored, dir, name);
    } while (Exists(stored));
    IniPutInt("Waste", "Next", (int)(n % 9999999L));
    size = TreeSize(path, 0);
    if (!MoveTree(path, stored, 0))
        return FALSE;
    {
        /* deletion time: now */
#ifdef RETRO_WIN32
        SYSTEMTIME st;
        FILETIME ft;

        GetLocalTime(&st);
        SystemTimeToFileTime(&st, &ft);
        FileTimeToDosDateTime(&ft, &d, &t);
#else
        struct dosdate_t dd;
        struct dostime_t dt;

        _dos_getdate(&dd);
        _dos_gettime(&dt);
        d = (WORD)(((dd.year - 1980) << 9) | (dd.month << 5) | dd.day);
        t = (WORD)((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));
#endif
    }
    WasteAppend(stored, path, d, t, size, attr < 0 ? 0 : attr);
    return TRUE;
}

BOOL OpsDelete(PATHLIST *pl, BOOL permanent)
{
    char buf[MAXP + 120];
    int i, failed = 0, r;
    BOOL inWaste = cur.kind == LOC_WASTE, useWaste;

    if (busy || !pl->n)
        return FALSE;
    useWaste = !permanent && !inWaste;
    for (i = 0; i < pl->n && useWaste; i++)
        if (!WasteUsable(pl->p[i]))
            useWaste = FALSE;
    if (confirmDelete || !useWaste) {
        if (pl->n == 1)
            sprintf(buf, useWaste ? "Move \"%s\" to the Wastebasket?" :
                    "Permanently delete \"%s\"?\n\nThis cannot be undone.", PathName(pl->p[0]));
        else
            sprintf(buf, useWaste ? "Move these %d items to the Wastebasket?" :
                    "Permanently delete these %d items?\n\nThis cannot be undone.", pl->n);
        if (inWaste && pl->n == 1)
            sprintf(buf, "Permanently delete this item from the Wastebasket?\n\nIt cannot be restored.");
        r = MessageBox(hFrame, buf, useWaste ? "Move to Wastebasket" : "Confirm Delete",
                       MB_YESNO | (useWaste ? MB_ICONQUESTION : MB_ICONEXCLAMATION));
        if (r != IDYES)
            return FALSE;
    }
    busy = TRUE;
    if (useWaste || inWaste)
        WasteLoad();
    ProgressOpen(useWaste ? "Moving to Wastebasket" : "Deleting");
    totalBytes = doneBytes = 0;
    for (i = 0; i < pl->n && !cancelled; i++) {
        BOOL ok;

        if (useWaste) {
            ok = ToWaste(pl->p[i]);
        } else {
            ok = DeleteTree(pl->p[i]);
            if (ok && inWaste) {
                int k;

                for (k = 0; k < nWaste; k++)
                    if (lstrcmpi(waste[k].stored, pl->p[i]) == 0) {
                        StrFree(&waste[k].stored);
                        StrFree(&waste[k].origin);
                        memmove(&waste[k], &waste[k + 1], (nWaste - k - 1) * sizeof(WASTEENT));
                        nWaste--;
                        break;
                    }
            }
        }
        if (!ok)
            failed++;
    }
    if (useWaste || inWaste)
        WasteSave();
    ProgressClose();
    busy = FALSE;
    if (failed && !cancelled)
        Msg(MB_ICONEXCLAMATION, "%d item%s could not be deleted.\n\nFiles in use or on a "
            "write-protected disk cannot be deleted.", failed, failed == 1 ? " " : "s");
    AfterOps();
    return !failed;
}

BOOL WasteRestore(PATHLIST *pl)
{
    int i, k, failed = 0;
    char dst[MAXP], dir[MAXP];

    if (busy)
        return FALSE;
    busy = TRUE;
    WasteLoad();
    ProgressOpen("Restoring");
    for (i = 0; i < pl->n && !cancelled; i++) {
        for (k = 0; k < nWaste; k++)
            if (lstrcmpi(waste[k].stored, pl->p[i]) == 0)
                break;
        if (k == nWaste)
            continue;
        lstrcpyn(dst, waste[k].origin, MAXP);
        lstrcpyn(dir, dst, MAXP);
        PathParent(dir);
        if (!IsDir(dir)) {
            /* recreate the missing folders */
            char part[MAXP];
            int n;

            for (n = 3; dir[n - 1]; n++)
                if (dir[n] == '\\' || dir[n] == '\0') {
                    lstrcpyn(part, dir, n + 1);
                    if (!IsDir(part))
                        MakeDir(part);
                }
        }
        UniqueName(dst);
        if (MoveTree(waste[k].stored, dst, 0)) {
            if (waste[k].attr & ~FA_DIR)
                SetAttr(dst, waste[k].attr & ~FA_DIR);
            StrFree(&waste[k].stored);
            StrFree(&waste[k].origin);
            memmove(&waste[k], &waste[k + 1], (nWaste - k - 1) * sizeof(WASTEENT));
            nWaste--;
        } else {
            failed++;
        }
    }
    WasteSave();
    ProgressClose();
    busy = FALSE;
    if (failed)
        Msg(MB_ICONEXCLAMATION, "%d item%s could not be restored.", failed, failed == 1 ? "" : "s");
    AfterOps();
    return !failed;
}

BOOL WasteEmpty(void)
{
    int i;

    WasteLoad();
    if (!nWaste)
        return TRUE;
    if (MessageBox(hFrame, "Permanently delete everything in the Wastebasket?\n\n"
                   "The files cannot be restored afterwards.", "Empty Wastebasket",
                   MB_YESNO | MB_ICONEXCLAMATION) != IDYES)
        return FALSE;
    busy = TRUE;
    ProgressOpen("Emptying Wastebasket");
    for (i = 0; i < nWaste && !cancelled; i++)
        DeleteTree(waste[i].stored);
    WasteLoad();
    WasteSave();
    ProgressClose();
    busy = FALSE;
    AfterOps();
    return TRUE;
}

/* ---- clipboard, new folder, rename ---------------------------------------- */

void OpsClipboard(BOOL cut)
{
    char buf[80];

    PlFree(&clip);
    SelectedPaths(&clip);
    clipCut = cut;
    if (clip.n) {
        sprintf(buf, "%d item%s %s. Use Paste in the destination folder.", clip.n,
                clip.n == 1 ? "" : "s", cut ? "cut" : "copied");
        Status(buf);
    }
    UpdateUI();
}

BOOL OpsCanPaste(void)
{
    return clip.n > 0 && (cur.kind == LOC_DIR || cur.kind == LOC_GROUP);
}

void OpsPaste(void)
{
    if (!OpsCanPaste())
        return;
    if (cur.kind == LOC_GROUP) {
        OpsDrop(&clip, NULL, cur.group, TRUE, FALSE);
        return;
    }
    if (OpsCopyMove(&clip, cur.path, clipCut) && clipCut)
        PlFree(&clip);
}

void OpsNewFolder(void)
{
    char path[MAXP];

    if (cur.kind != LOC_DIR)
        return;
#ifdef RETRO_WIN32
    PathJoin(path, cur.path, "New Folder");
#else
    PathJoin(path, cur.path, "NEWFOLDR");
#endif
    UniqueName(path);
    if (!MakeDir(path)) {
        Msg(MB_ICONSTOP, "Cannot create a folder in \"%s\".", cur.path);
        return;
    }
    SideRebuild();
    NavRefresh();
    ViewSelectName(PathName(path));
    ViewRename();
}

#ifndef RETRO_WIN32
/* DOS names: up to 8 characters, a dot and 3 more, no reserved characters. */
static BOOL ValidDosName(char *name)
{
    char *dot = strchr(name, '.');
    const char *bad = "\"*+,/:;<=>?[\\]| ";
    int i;

    AnsiUpper(name);
    for (i = 0; name[i]; i++)
        if (strchr(bad, name[i]) || (BYTE)name[i] < 32)
            return FALSE;
    if (dot) {
        if (dot == name || dot - name > 8 || strlen(dot + 1) > 3 || strchr(dot + 1, '.'))
            return FALSE;
    } else if (strlen(name) > 8) {
        return FALSE;
    }
    return TRUE;
}
#endif

void OpsRename(ITEM *it, const char *newName)
{
    char from[MAXP], to[MAXP], name[MAXP];
    GROUP *gr;

    lstrcpyn(name, newName, MAXP);
    if (it->kind == IK_PROG) {
        gr = GroupPtr(cur.group);
        if (gr && it->ref >= 0 && it->ref < gr->n) {
            StrSet(&gr->items[it->ref].name, name);
            GroupsSave();
            NavRefresh();
            ViewSelectName(name);
        }
        return;
    }
    if (it->kind == IK_GROUP) {
        GroupRename(it->ref, name);
        GroupsSave();
        SideRebuild();
        NavRefresh();
        return;
    }
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
        Msg(MB_ICONEXCLAMATION, "A file name cannot contain \\ / or :");
        return;
    }
#ifndef RETRO_WIN32
    if (!ValidDosName(name)) {
        Msg(MB_ICONEXCLAMATION, "\"%s\" is not a valid MS-DOS file name.\n\n"
            "Use up to 8 characters, optionally followed by a period and 3 more.", newName);
        return;
    }
#endif
    ViewItemPath(it, from);
    lstrcpyn(to, from, MAXP);
    PathParent(to);
    PathJoin(to, to, name);
    if (lstrcmpi(from, to) != 0 && Exists(to)) {
        Msg(MB_ICONEXCLAMATION, "\"%s\" already exists.", name);
        return;
    }
    if (!RenameOne(from, to)) {
        Msg(MB_ICONSTOP, "Cannot rename \"%s\".\n\nIt may be in use or on a write-protected disk.",
            it->name);
        return;
    }
    if (it->kind == IK_DIR)
        SideRebuild();
    NavRefresh();
    ViewSelectName(name);
}

/* ---- dropping ----------------------------------------------------------------- */

/* Files dropped onto a folder (copy or move) or onto a group (programs). */
void OpsDrop(PATHLIST *pl, const char *destDir, int group, BOOL copy, BOOL ask)
{
    int i, added = 0;

    (void)ask;
    if (!pl->n)
        return;
    if (group >= 0) {
        for (i = 0; i < pl->n; i++) {
            PROG p;

            if (IsDir(pl->p[i]))
                continue;
            ProgFromFile(&p, pl->p[i]);
            if (ProgAdd(group, &p, -1) >= 0)
                added++;
            ProgFree(&p);
        }
        GroupsSave();
        SideRebuild();
        if (cur.kind == LOC_GROUP && cur.group == group)
            NavRefresh();
        if (!added)
            Msg(MB_ICONINFORMATION, "Only files can be added to a group, not folders.");
        else {
            char buf[80];

            sprintf(buf, "%d program%s added to %s.", added, added == 1 ? "" : "s", groups[group].name);
            Status(buf);
        }
        return;
    }
    if (!destDir)
        return;
    /* like File Manager: move on the same drive, copy to another one */
    if (!copy) {
        for (i = 0; i < pl->n; i++)
            if (toupper(pl->p[i][0]) != toupper(destDir[0])) {
                copy = TRUE;
                break;
            }
    }
    OpsCopyMove(pl, destDir, !copy);
}

void OpsDropFiles(HWND hwnd, HDROP hd)
{
    PATHLIST pl;
    char path[MAXP], dir[MAXP];
    UINT n, i;
    int group = -1, t = DT_NONE;
    POINT pt;

    PlInit(&pl);
    n = DragQueryFile(hd, (UINT)-1, NULL, 0);
    for (i = 0; i < n; i++) {
        DragQueryFile(hd, i, path, MAXP);
        PlAdd(&pl, path);
    }
    DragQueryPoint(hd, &pt);
    DragFinish(hd);
    dir[0] = '\0';
    if (hwnd == hSide) {
        t = SideDropTarget(pt, dir, &group);
        SideSetDropHilite(-1);
    } else if (hwnd == hView) {
        int pos;

        t = ViewDropTarget(pt, dir, &group, &pos);
        ViewSetDropHilite(-1);
    }
    if (t == DT_DIR)
        OpsDrop(&pl, dir, -1, TRUE, FALSE);
    else if (t == DT_GROUP)
        OpsDrop(&pl, NULL, group, TRUE, FALSE);
    else if (t == DT_WASTE)
        OpsDelete(&pl, FALSE);
    else
        Msg(MB_ICONINFORMATION, "Drop files onto a folder, a drive or a program group.");
    PlFree(&pl);
}
