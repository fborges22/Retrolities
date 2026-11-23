/* ShellIV - locations, history and filling the item view. */
#include "shelliv.h"

#define MAXHIST 40

LOCATION cur;
char searchPat[80], searchText[80];
BOOL searchSubdirs = TRUE;
BOOL showHidden;

static LOCATION *hist;
static int histN, histPos = -1;
static int generation;          /* changes on every navigation; stops searches */

void NavInit(void)
{
    hist = (LOCATION *)calloc(MAXHIST, sizeof(LOCATION));
}

void NavDone(void)
{
    if (hist)
        free(hist);
    hist = NULL;
}

BOOL LocIsFiles(void)
{
    return cur.kind == LOC_DIR || cur.kind == LOC_SEARCH;
}

void LocTitle(const LOCATION *loc, char *out, int size)
{
    switch (loc->kind) {
    case LOC_COMPUTER: lstrcpyn(out, "My Computer", size); break;
    case LOC_PROGRAMS: lstrcpyn(out, "Programs", size); break;
    case LOC_RECENT:   lstrcpyn(out, "Recent Programs", size); break;
    case LOC_WASTE:    lstrcpyn(out, "Wastebasket", size); break;
    case LOC_SEARCH:   lstrcpyn(out, "Search Results", size); break;
    case LOC_GROUP: {
        GROUP *g = GroupPtr(loc->group);

        lstrcpyn(out, g ? g->name : "Group", size);
        break;
    }
    default:
        lstrcpyn(out, loc->path, size);
    }
}

/* ---- filling the view --------------------------------------------------- */

static void Sub(char *out, int n, const char *what, DWORD bytes, BOOL withSize)
{
    char num[20], sz[24];

    FmtNum(num, (DWORD)n);
    if (withSize) {
        FmtSize(sz, bytes);
        sprintf(out, "%s %s%s  \xB7  %s", num, what, n == 1 ? "" : "s", sz);
    } else {
        sprintf(out, "%s %s%s", num, what, n == 1 ? "" : "s");
    }
}

static BOOL FillDir(const char *keep)
{
    FINDINFO fi;
    FINDSTATE *fs;
    DWORD total = 0, fk, tk;
    int n = 0, drive = cur.path[0] - 'A';
    char sub[200], title[MAXP], label[40], count[80];
    ITEM *it;

    if (!IsDir(cur.path) && !PathIsRoot(cur.path))
        return FALSE;
    ViewBegin();
    fs = FsOpen(cur.path, "*.*", &fi);
    if (fs) {
        do {
            if (!showHidden && (fi.attr & (FA_HIDDEN | FA_SYSTEM)))
                continue;
            it = ViewAdd((fi.attr & FA_DIR) ? IK_DIR : IK_FILE, fi.name, NULL);
            if (!it)
                break;
            it->size = fi.size;
            it->date = fi.date;
            it->time = fi.time;
            it->attr = fi.attr;
            total += fi.size;
            n++;
        } while (FsNext(fs, &fi));
        FsClose(fs);
    } else if (!PathIsRoot(cur.path) && !Exists(cur.path)) {
        return FALSE;
    }
    if (PathIsRoot(cur.path)) {
        if (DriveLabel(drive, label, sizeof(label)))
            sprintf(title, "%s (%c:)", label, cur.path[0]);
        else
            sprintf(title, "Drive %c:", cur.path[0]);
    } else {
        lstrcpyn(title, PathName(cur.path), MAXP);
    }
    Sub(count, n, "item", total, TRUE);
    sprintf(sub, "%.100s  \xB7  %s", cur.path, count);
    if (drive >= 0 && drive < 26 && DriveSpace(drive, &fk, &tk))
        ViewSetGauge(fk, tk);
    else
        ViewSetGauge(0, 0);
    if (PathIsRoot(cur.path)) {
        int k = DriveKind(drive);

        ViewSetBanner(k == DK_FLOPPY ? BI_FLOPPY : k == DK_CDROM ? BI_CDROM :
                      k == DK_NET ? BI_NETDRIVE : BI_HARDDISK, title, sub);
    } else {
        ViewSetBanner(BI_FOLDER, title, sub);
    }
    ViewEnd(keep);
    return TRUE;
}

static void FillComputer(const char *keep)
{
    int d, n = 0;
    char label[40], name[80], sub[200], tmp[40];
    ITEM *it;

    ViewBegin();
    for (d = 0; d < 26; d++) {
        int k = DriveKind(d);

        if (k == DK_NONE)
            continue;
        label[0] = '\0';
        if (k == DK_HARD || k == DK_RAM)
            DriveLabel(d, label, sizeof(label));
        if (label[0])
            sprintf(name, "%s (%c:)", label, 'A' + d);
        else
            sprintf(name, "%s (%c:)", k == DK_FLOPPY ? "Floppy" : k == DK_CDROM ? "CD-ROM" :
                    k == DK_NET ? "Network" : k == DK_RAM ? "RAM Disk" : "Hard Disk", 'A' + d);
        it = ViewAdd(IK_DRIVE, name, NULL);
        if (!it)
            break;
        it->ref = (short)d;
        it->dk = (BYTE)k;
        if (k != DK_FLOPPY && k != DK_CDROM)
            DriveSpace(d, &it->size, &it->total);
        n++;
    }
    Sub(sub, n, "drive", 0, FALSE);
#ifdef RETRO_WIN32
    {
        char host[40];

        sprintf(sub + strlen(sub), "  \xB7  %s", retro_host(host, sizeof(host)));
    }
#else
    {
        DWORD v = GetVersion();
        DWORD flags = GetWinFlags();

        sprintf(sub + strlen(sub), "  \xB7  Windows %u.%02u %s  \xB7  ",
                LOBYTE(LOWORD(v)), HIBYTE(LOWORD(v)),
                (flags & WF_ENHANCED) ? "386 Enhanced" : "Standard");
        FmtKB(tmp, GetFreeSpace(0) / 1024);
        sprintf(sub + strlen(sub), "%s free memory  \xB7  %u%% resources", tmp,
                GetFreeSystemResources(GFSR_SYSTEMRESOURCES));
    }
#endif
    (void)tmp;
    ViewSetGauge(0, 0);
    ViewSetBanner(BI_COMPUTER, "My Computer", sub);
    ViewEnd(keep);
}

static void FillPrograms(const char *keep)
{
    int g, total = 0;
    char sub[120], a[40];
    ITEM *it;

    ViewBegin();
    for (g = 0; g < nGroups; g++) {
        it = ViewAdd(IK_GROUP, groups[g].name, NULL);
        if (!it)
            break;
        it->ref = (short)g;
        it->size = groups[g].n;
        total += groups[g].n;
    }
    it = ViewAdd(IK_GROUP, "Recent Programs", NULL);
    if (it) {
        it->ref = -1;
        it->size = recent.n;
    }
    Sub(sub, nGroups, "group", 0, FALSE);
    Sub(a, total, "program", 0, FALSE);
    sprintf(sub + strlen(sub), "  \xB7  %s", a);
    ViewSetGauge(0, 0);
    ViewSetBanner(BI_GROUP, "Programs", sub);
    ViewEnd(keep);
}

static void FillGroup(const char *keep)
{
    GROUP *gr = GroupPtr(cur.kind == LOC_RECENT ? -1 : cur.group);
    char sub[160];
    int i;
    ITEM *it;

    ViewBegin();
    if (gr)
        for (i = 0; i < gr->n; i++) {
            it = ViewAdd(IK_PROG, gr->items[i].name, gr->items[i].cmd);
            if (!it)
                break;
            it->ref = (short)i;
        }
    Sub(sub, gr ? gr->n : 0, "program", 0, FALSE);
    if (cur.kind == LOC_RECENT)
        strcat(sub, "  \xB7  the programs you started last");
    else if (!lstrcmpi(gr ? gr->name : "", "StartUp"))
        strcat(sub, "  \xB7  started when ShellIV starts as the Windows shell");
    else
        strcat(sub, "  \xB7  drop files here to add programs");
    ViewSetGauge(0, 0);
    ViewSetBanner(cur.kind == LOC_RECENT ? BI_RECENT : BI_GROUP,
                  cur.kind == LOC_RECENT ? "Recent Programs" : gr ? gr->name : "Group", sub);
    ViewEnd(keep);
}

static void FillWaste(const char *keep)
{
    int i;
    DWORD total = 0;
    char sub[120];
    ITEM *it;

    WasteLoad();
    ViewBegin();
    for (i = 0; i < nWaste; i++) {
        char dir[MAXP];

        lstrcpyn(dir, waste[i].origin, MAXP);
        PathParent(dir);
        it = ViewAdd(IK_WASTE, PathName(waste[i].origin), dir);
        if (!it)
            break;
        it->ref = (short)i;
        it->size = waste[i].size;
        it->date = waste[i].date;
        it->time = waste[i].time;
        it->attr = waste[i].attr;
        total += waste[i].size;
    }
    Sub(sub, nWaste, "item", total, TRUE);
    strcat(sub, "  \xB7  deleted files can be restored");
    ViewSetGauge(0, 0);
    ViewSetBanner(nWaste ? BI_WASTEFULL : BI_WASTE, "Wastebasket", sub);
    ViewEnd(keep);
}

/* ---- search ---------------------------------------------------------------- */

static BOOL FileContains(const char *path, const char *text)
{
    HFILE f;
    char *buf;
    int n = strlen(text), have = 0, got, i;
    BOOL found = FALSE;
    static char up[256];
    static BOOL init;

    if (!n)
        return TRUE;
    if (!init) {
        for (i = 0; i < 256; i++)
            up[i] = (char)((i >= 'a' && i <= 'z') ? i - 32 : i);
        init = TRUE;
    }
    buf = (char *)malloc(8192 + 80);
    if (!buf)
        return FALSE;
    f = _lopen(path, OF_READ | OF_SHARE_DENY_NONE);
    if (f == HFILE_ERROR) {
        free(buf);
        return FALSE;
    }
    for (;;) {
        got = (int)_lread(f, buf + have, 8192);
        if (got <= 0 || got == (int)HFILE_ERROR)
            break;
        have += got;
        for (i = 0; i + n <= have; i++) {
            int k;

            for (k = 0; k < n; k++)
                if (up[(BYTE)buf[i + k]] != up[(BYTE)text[k]])
                    break;
            if (k == n) {
                found = TRUE;
                break;
            }
        }
        if (found)
            break;
        /* keep the tail for matches across buffers */
        if (have > n - 1) {
            memmove(buf, buf + have - (n - 1), n - 1);
            have = n - 1;
        }
    }
    _lclose(f);
    free(buf);
    return found;
}

void NavSearch(const char *dir, const char *pattern, const char *text, BOOL subdirs)
{
    LOCATION loc;
    char **stack = NULL, sub[200], path[MAXP];
    int nStack = 0, capStack = 0, found = 0, gen, scanned = 0;
    DWORD lastPaint = GetTickCount();
    BOOL cancelled = FALSE;

    memset(&loc, 0, sizeof(loc));
    loc.kind = LOC_SEARCH;
    lstrcpyn(loc.path, dir, MAXP);
    lstrcpyn(searchPat, pattern[0] ? pattern : "*", sizeof(searchPat));
    lstrcpyn(searchText, text, sizeof(searchText));
    searchSubdirs = subdirs;
    Navigate(&loc, TRUE);           /* shows an empty result list */
    gen = generation;
    SetBusy(TRUE);
    capStack = 64;
    stack = (char **)malloc(capStack * sizeof(char *));
    if (stack)
        stack[nStack++] = StrDup(dir);
    ViewBegin();
    while (nStack > 0) {
        char *d = stack[--nStack];
        FINDINFO fi;
        FINDSTATE *fs = FsOpen(d, "*.*", &fi);

        if (fs) {
            do {
                if (!showHidden && (fi.attr & (FA_HIDDEN | FA_SYSTEM)))
                    continue;
                if ((fi.attr & FA_DIR) && subdirs) {
                    if (nStack == capStack) {
                        char **p = (char **)realloc(stack, capStack * 2 * sizeof(char *));

                        if (!p)
                            continue;
                        stack = p;
                        capStack *= 2;
                    }
                    PathJoin(path, d, fi.name);
                    stack[nStack++] = StrDup(path);
                }
                if (!WildMatch(searchPat, fi.name))
                    continue;
                if (searchText[0]) {
                    if (fi.attr & FA_DIR)
                        continue;
                    PathJoin(path, d, fi.name);
                    if (!FileContains(path, searchText))
                        continue;
                }
                {
                    ITEM *it = ViewAdd((fi.attr & FA_DIR) ? IK_DIR : IK_FILE, fi.name, d);

                    if (it) {
                        it->size = fi.size;
                        it->date = fi.date;
                        it->time = fi.time;
                        it->attr = fi.attr;
                        found++;
                    }
                }
            } while (FsNext(fs, &fi));
            FsClose(fs);
        }
        scanned++;
        if (GetTickCount() - lastPaint > 250) {
            lastPaint = GetTickCount();
            sprintf(sub, "Searching %.90s ...  %d found  \xB7  press Esc to stop", d, found);
            ViewSetBanner(BI_SEARCH, "Searching...", sub);
            ViewInvalidate();
            if (!PumpMessages())
                cancelled = TRUE;
            if (generation != gen)
                cancelled = TRUE;
        }
        free(d);
        if (cancelled)
            break;
    }
    while (nStack > 0)
        free(stack[--nStack]);
    if (stack)
        free(stack);
    SetBusy(FALSE);
    if (generation != gen)
        return;                     /* the user went elsewhere */
    sprintf(sub, "%d found for \"%s\"%s%s%s in %.80s%s", found, searchPat,
            searchText[0] ? " containing \"" : "", searchText, searchText[0] ? "\"" : "",
            dir, cancelled ? "  \xB7  stopped" : "");
    ViewSetGauge(0, 0);
    ViewSetBanner(BI_SEARCH, "Search Results", sub);
    ViewEnd(NULL);
    (void)scanned;
}

/* ---- navigation ------------------------------------------------------------ */

static BOOL Fill(const char *keep)
{
    switch (cur.kind) {
    case LOC_DIR:      return FillDir(keep);
    case LOC_COMPUTER: FillComputer(keep); break;
    case LOC_PROGRAMS: FillPrograms(keep); break;
    case LOC_GROUP:
    case LOC_RECENT:   FillGroup(keep); break;
    case LOC_WASTE:    FillWaste(keep); break;
    case LOC_SEARCH:
        ViewBegin();
        ViewSetGauge(0, 0);
        ViewSetBanner(BI_SEARCH, "Search Results", "");
        ViewEnd(keep);
        break;
    }
    return TRUE;
}

static BOOL SameLoc(const LOCATION *a, const LOCATION *b)
{
    if (a->kind != b->kind)
        return FALSE;
    if (a->kind == LOC_GROUP)
        return a->group == b->group;
    if (a->kind == LOC_DIR || a->kind == LOC_SEARCH)
        return lstrcmpi(a->path, b->path) == 0;
    return TRUE;
}

void Navigate(const LOCATION *loc, BOOL history)
{
    LOCATION old = cur;
    char keep[MAXP];

    keep[0] = '\0';
    generation++;
    cur = *loc;
    if (cur.kind == LOC_DIR)
        PathNormalize(cur.path);
    /* coming back up: select the folder we came from */
    if (cur.kind == LOC_DIR && old.kind == LOC_DIR && PathInside(old.path, cur.path)
        && lstrcmpi(old.path, cur.path) != 0) {
        const char *rest = old.path + strlen(cur.path);
        char *bs;

        while (*rest == '\\')
            rest++;
        lstrcpyn(keep, rest, MAXP);
        bs = strchr(keep, '\\');
        if (bs)
            *bs = '\0';
    }
    SetBusy(TRUE);
    if (!Fill(keep[0] ? keep : NULL)) {
        SetBusy(FALSE);
        Msg(MB_ICONEXCLAMATION, "Cannot open \"%s\".\n\nThe folder does not exist or the drive is not ready.",
            cur.path);
        cur = old;
        Fill(NULL);
        return;
    }
    SetBusy(FALSE);
    if (history && hist && (histPos < 0 || !SameLoc(&hist[histPos], &cur))) {
        if (histPos == MAXHIST - 1) {
            memmove(&hist[0], &hist[1], (MAXHIST - 1) * sizeof(LOCATION));
            histPos--;
        }
        hist[++histPos] = cur;
        histN = histPos + 1;
    }
    SideSync();
    UpdateTitle();
    UpdateUI();
}

void NavDir(const char *path)
{
    LOCATION loc;

    memset(&loc, 0, sizeof(loc));
    loc.kind = LOC_DIR;
    lstrcpyn(loc.path, path, MAXP);
    Navigate(&loc, TRUE);
}

void NavKind(int kind, int group)
{
    LOCATION loc;

    memset(&loc, 0, sizeof(loc));
    loc.kind = kind;
    loc.group = group;
    Navigate(&loc, TRUE);
}

void NavRefresh(void)
{
    char keep[MAXP];
    ITEM *it = ViewFocusItem();

    keep[0] = '\0';
    if (it)
        lstrcpyn(keep, it->name, MAXP);
    if (cur.kind == LOC_SEARCH) {
        NavSearch(cur.path, searchPat, searchText, searchSubdirs);
        return;
    }
    if (!Fill(keep)) {
        /* the folder went away: go to the nearest existing parent */
        while (PathParent(cur.path) && !IsDir(cur.path))
            ;
        Fill(NULL);
    }
    SideSync();
    UpdateUI();
}

BOOL NavCanBack(void)
{
    return histPos > 0;
}

BOOL NavCanForward(void)
{
    return histPos >= 0 && histPos < histN - 1;
}

BOOL NavCanUp(void)
{
    return (cur.kind == LOC_DIR) || cur.kind == LOC_GROUP || cur.kind == LOC_RECENT
           || cur.kind == LOC_SEARCH || cur.kind == LOC_WASTE;
}

void NavBack(void)
{
    if (!NavCanBack())
        return;
    histPos--;
    Navigate(&hist[histPos], FALSE);
}

void NavForward(void)
{
    if (!NavCanForward())
        return;
    histPos++;
    Navigate(&hist[histPos], FALSE);
}

void NavUp(void)
{
    char path[MAXP];

    switch (cur.kind) {
    case LOC_DIR:
        lstrcpyn(path, cur.path, MAXP);
        if (PathParent(path))
            NavDir(path);
        else
            NavKind(LOC_COMPUTER, 0);
        break;
    case LOC_GROUP:
    case LOC_RECENT:
        NavKind(LOC_PROGRAMS, 0);
        break;
    case LOC_SEARCH:
        NavDir(cur.path);
        break;
    case LOC_WASTE:
        NavKind(LOC_COMPUTER, 0);
        break;
    }
}

void NavOpenItem(ITEM *it)
{
    char path[MAXP];
    PROG p;
    GROUP *gr;

    switch (it->kind) {
    case IK_DRIVE:
        if (it->dk == DK_FLOPPY || it->dk == DK_CDROM) {
            DWORD f, t;

            if (!DriveSpace(it->ref, &f, &t)) {
                Msg(MB_ICONEXCLAMATION, "Drive %c: is not ready.\n\nInsert a disk and try again.",
                    'A' + it->ref);
                return;
            }
        }
        /* fall through */
    case IK_DIR:
        ViewItemPath(it, path);
        NavDir(path);
        break;
    case IK_GROUP:
        if (it->ref < 0)
            NavKind(LOC_RECENT, -1);
        else
            NavKind(LOC_GROUP, it->ref);
        break;
    case IK_PROG:
        gr = GroupPtr(cur.kind == LOC_RECENT ? -1 : cur.group);
        if (gr && it->ref >= 0 && it->ref < gr->n) {
            LaunchProg(&gr->items[it->ref]);
            if (cur.kind == LOC_RECENT)
                NavRefresh();
        }
        break;
    case IK_FILE:
        ViewItemPath(it, path);
        if (lstrcmpi(PathExt(path), ".GRP") == 0) {
            Msg(MB_ICONINFORMATION, "Use Programs > Import Program Manager Groups to bring "
                "Program Manager groups into ShellIV.");
            return;
        }
        ProgFromFile(&p, path);
        LaunchProg(&p);
        ProgFree(&p);
        break;
    case IK_WASTE:
        Command(IDM_PROPS);
        break;
    }
}
