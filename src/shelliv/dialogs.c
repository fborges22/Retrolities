/* ShellIV - dialog boxes. */
#include "shelliv.h"

/* ---- shared ------------------------------------------------------------------ */

static BOOL BrowseFile(HWND owner, char *path, int size, const char *filter, const char *title)
{
    OPENFILENAME ofn;
    char file[MAXP], dir[MAXP];

    lstrcpyn(file, path, MAXP);
    if (file[0] == '"') {
        memmove(file, file + 1, strlen(file));
        if (strchr(file, '"'))
            *strchr(file, '"') = '\0';
    } else if (strchr(file, ' ')) {
        *strchr(file, ' ') = '\0';
    }
    lstrcpyn(dir, file, MAXP);
    if (!PathParent(dir))
        dir[0] = '\0';
    lstrcpyn(file, PathName(file), MAXP);
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAXP;
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileName(&ofn))
        return FALSE;
    lstrcpyn(path, file, size);
    return TRUE;
}

static const char progFilter[] =
    "Programs\0*.EXE;*.COM;*.PIF;*.BAT\0All Files (*.*)\0*.*\0";
static const char iconFilter[] =
    "Icon Files\0*.EXE;*.ICO;*.DLL\0All Files (*.*)\0*.*\0";

/* Paints an icon centred on a dialog control. */
static void PaintIconAt(HWND hDlg, int id, HICON h)
{
    HWND ctl = GetDlgItem(hDlg, id);
    RECT r;
    POINT pt;
    HDC hdc;

    if (!ctl || !h)
        return;
    GetWindowRect(ctl, &r);
    pt.x = r.left + (r.right - r.left - 32) / 2;
    pt.y = r.top + (r.bottom - r.top - 32) / 2;
    ScreenToClient(hDlg, &pt);
    hdc = GetDC(hDlg);
    DrawIcon(hdc, pt.x, pt.y, h);
    ReleaseDC(hDlg, hdc);
}

/* ---- confirm (Yes / Yes to All / No / Cancel) ----------------------------- */

static const char *confText, *confTitle;
static BOOL confAll;

DLGRESULT CALLBACK __export ConfirmProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowText(hDlg, confTitle);
        SetDlgItemText(hDlg, IDC_PROMPT, confText);
        if (!confAll)
            ShowWindow(GetDlgItem(hDlg, IDC_YESALL), SW_HIDE);
        CenterWindow(hDlg);
        return TRUE;
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDYES:      EndDialog(hDlg, CONF_YES); return TRUE;
        case IDC_YESALL: EndDialog(hDlg, CONF_ALL); return TRUE;
        case IDNO:       EndDialog(hDlg, CONF_NO); return TRUE;
        case IDCANCEL:   EndDialog(hDlg, CONF_CANCEL); return TRUE;
        }
    }
    return FALSE;
}

int DlgConfirm(const char *title, const char *text, BOOL all)
{
    confTitle = title;
    confText = text;
    confAll = all;
    return RunDialog(IDD_CONFIRM, (FARPROC)ConfirmProc, 0);
}

/* ---- text input ------------------------------------------------------------ */

static const char *inTitle, *inPrompt;
static char *inBuf;
static int inSize;

DLGRESULT CALLBACK __export InputProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowText(hDlg, inTitle);
        SetDlgItemText(hDlg, IDC_PROMPT, inPrompt);
        SetDlgItemText(hDlg, IDC_EDIT, inBuf);
        SendDlgItemMessage(hDlg, IDC_EDIT, EM_LIMITTEXT, inSize - 1, 0);
        SendDlgItemMessage(hDlg, IDC_EDIT, EM_SETSEL, 0, MAKELONG(0, -1));
#ifdef RETRO_WIN32
        SendDlgItemMessage(hDlg, IDC_EDIT, EM_SETSEL, 0, -1);
#endif
        CenterWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_EDIT));
        return FALSE;
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDOK:
            GetDlgItemText(hDlg, IDC_EDIT, inBuf, inSize);
            EndDialog(hDlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL DlgInput(const char *title, const char *prompt, char *buf, int size)
{
    inTitle = title;
    inPrompt = prompt;
    inBuf = buf;
    inSize = size;
    return RunDialog(IDD_INPUT, (FARPROC)InputProc, 0) == 1 && buf[0];
}

/* ---- icon picker ------------------------------------------------------------- */

static char icPath[MAXP];
static int icIdx;
static HICON icList[64];
static int icN;

static void IconsLoad(HWND hDlg)
{
    HWND lb = GetDlgItem(hDlg, IDC_ICONLIST);
    int i, n;

    for (i = 0; i < icN; i++)
        if (icList[i])
            DestroyIcon(icList[i]);
    icN = 0;
    SendMessage(lb, LB_RESETCONTENT, 0, 0);
    n = FileIconCount(icPath);
    for (i = 0; i < n && i < 64; i++) {
        HICON h = ExtractIcon(hInst, icPath, i);

        if ((UINT)h <= 1)
            break;
        icList[icN++] = h;
        SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)i);
    }
    if (icN == 0)
        SetDlgItemText(hDlg, IDC_INFO, "This file contains no icons.");
    else {
        char buf[40];

        sprintf(buf, "%d icon%s:", icN, icN == 1 ? "" : "s");
        SetDlgItemText(hDlg, IDC_INFO, buf);
    }
    SendMessage(lb, LB_SETCURSEL, icIdx < icN ? icIdx : 0, 0);
}

DLGRESULT CALLBACK __export IconsProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(hDlg, IDC_FILE, icPath);
        SendDlgItemMessage(hDlg, IDC_ICONLIST, LB_SETCOLUMNWIDTH, 42, 0);
        IconsLoad(hDlg);
        CenterWindow(hDlg);
        return TRUE;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT XFAR *mi = (MEASUREITEMSTRUCT XFAR *)LP2PTR(lParam);

        mi->itemWidth = 42;
        mi->itemHeight = 40;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT XFAR *di = (DRAWITEMSTRUCT XFAR *)LP2PTR(lParam);
        RECT r;
        int i = (int)di->itemID;

        r = di->rcItem;
        FillR(di->hDC, &r, (di->itemState & ODS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHT)
                                                         : GetSysColor(COLOR_WINDOW));
        if (i >= 0 && i < icN)
            DrawIcon(di->hDC, r.left + (r.right - r.left - 32) / 2, r.top + 4, icList[i]);
        if (di->itemState & ODS_FOCUS)
            DrawFocusRect(di->hDC, &r);
        return TRUE;
    }
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDC_BROWSE: {
            char p[MAXP];

            GetDlgItemText(hDlg, IDC_FILE, p, MAXP);
            if (BrowseFile(hDlg, p, MAXP, iconFilter, "Change Icon")) {
                SetDlgItemText(hDlg, IDC_FILE, p);
                lstrcpyn(icPath, p, MAXP);
                icIdx = 0;
                IconsLoad(hDlg);
            }
            return TRUE;
        }
        case IDC_FILE:
            if (CMD_NOTIFY(wParam, lParam) == EN_KILLFOCUS) {
                char p[MAXP];

                GetDlgItemText(hDlg, IDC_FILE, p, MAXP);
                if (lstrcmpi(p, icPath) != 0) {
                    lstrcpyn(icPath, p, MAXP);
                    icIdx = 0;
                    IconsLoad(hDlg);
                }
            }
            return TRUE;
        case IDC_ICONLIST:
            if (CMD_NOTIFY(wParam, lParam) != LBN_DBLCLK)
                return TRUE;
            /* fall through */
        case IDOK: {
            int s = (int)SendDlgItemMessage(hDlg, IDC_ICONLIST, LB_GETCURSEL, 0, 0);

            GetDlgItemText(hDlg, IDC_FILE, icPath, MAXP);
            icIdx = s < 0 ? 0 : s;
            EndDialog(hDlg, icN > 0);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    case WM_DESTROY: {
        int i;

        for (i = 0; i < icN; i++)
            if (icList[i])
                DestroyIcon(icList[i]);
        icN = 0;
        break;
    }
    }
    return FALSE;
}

BOOL DlgPickIcon(char *path, int *idx)
{
    lstrcpyn(icPath, path, MAXP);
    icIdx = *idx;
    if (RunDialog(IDD_ICONS, (FARPROC)IconsProc, 0) != 1)
        return FALSE;
    lstrcpyn(path, icPath, MAXP);
    *idx = icIdx;
    return TRUE;
}

/* ---- program item properties ------------------------------------------------ */

static PROG *pgProg;
static BOOL pgNew;
static char pgIcon[MAXP];
static int pgIdx;
static HICON pgShown;

static void ProgIconUpdate(HWND hDlg)
{
    char cmd[MAXP];
    PROG tmp;

    pgShown = NULL;
    if (pgIcon[0]) {
        pgShown = FileIcon(pgIcon, pgIdx);
    } else {
        GetDlgItemText(hDlg, IDC_CMD, cmd, MAXP);
        memset(&tmp, 0, sizeof(tmp));
        tmp.cmd = cmd;
        ProgDisplayCmd(&tmp, cmd, MAXP);
        if (lstrcmpi(PathExt(cmd), ".EXE") == 0)
            pgShown = FileIcon(cmd, 0);
    }
    if (!pgShown)
        pgShown = BuiltinIcon(BI_PROGRAM);
    InvalidateRect(hDlg, NULL, TRUE);
}

DLGRESULT CALLBACK __export ProgramProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[MAXP];

    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowText(hDlg, pgNew ? "New Program" : "Program Properties");
        SetDlgItemText(hDlg, IDC_NAME, pgProg->name ? pgProg->name : "");
        SetDlgItemText(hDlg, IDC_CMD, pgProg->cmd ? pgProg->cmd : "");
        SetDlgItemText(hDlg, IDC_DIR, pgProg->dir ? pgProg->dir : "");
        buf[0] = '\0';
        if (LOBYTE(pgProg->hotkey))
            HotkeyText(pgProg->hotkey, buf);
        SetDlgItemText(hDlg, IDC_HOTKEY, buf);     /* empty: no shortcut key */
        lstrcpyn(pgIcon, pgProg->iconPath ? pgProg->iconPath : "", MAXP);
        pgIdx = pgProg->iconIdx;
        ProgIconUpdate(hDlg);
        CenterWindow(hDlg);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;

        BeginPaint(hDlg, &ps);
        EndPaint(hDlg, &ps);
        PaintIconAt(hDlg, IDC_ICONBOX, pgShown);
        return TRUE;
    }
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDC_CMD:
            if (CMD_NOTIFY(wParam, lParam) == EN_KILLFOCUS && !pgIcon[0])
                ProgIconUpdate(hDlg);
            return TRUE;
        case IDC_BROWSE:
            GetDlgItemText(hDlg, IDC_CMD, buf, MAXP);
            if (BrowseFile(hDlg, buf, MAXP, progFilter, "Browse")) {
                PROG tmp;

                SetDlgItemText(hDlg, IDC_CMD, buf);
                ProgFromFile(&tmp, buf);
                if (GetWindowTextLength(GetDlgItem(hDlg, IDC_NAME)) == 0)
                    SetDlgItemText(hDlg, IDC_NAME, tmp.name);
                if (GetWindowTextLength(GetDlgItem(hDlg, IDC_DIR)) == 0)
                    SetDlgItemText(hDlg, IDC_DIR, tmp.dir);
                ProgFree(&tmp);
                ProgIconUpdate(hDlg);
            }
            return TRUE;
        case IDC_CHICON: {
            PROG tmp;

            if (!pgIcon[0]) {
                GetDlgItemText(hDlg, IDC_CMD, buf, MAXP);
                memset(&tmp, 0, sizeof(tmp));
                tmp.cmd = buf;
                ProgDisplayCmd(&tmp, pgIcon, MAXP);
                if (lstrcmpi(PathExt(pgIcon), ".EXE") != 0) {
                    GetModuleFileName(hInst, pgIcon, MAXP);
                    pgIdx = 0;
                }
            }
            if (!DlgPickIcon(pgIcon, &pgIdx) && !pgProg->iconPath)
                pgIcon[0] = '\0';
            ProgIconUpdate(hDlg);
            return TRUE;
        }
        case IDOK: {
            char name[80], cmd[MAXP], dir[MAXP];

            GetDlgItemText(hDlg, IDC_NAME, name, sizeof(name));
            GetDlgItemText(hDlg, IDC_CMD, cmd, MAXP);
            GetDlgItemText(hDlg, IDC_DIR, dir, MAXP);
            if (!cmd[0]) {
                MessageBox(hDlg, "Enter the command line of the program.", APPNAME, MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(hDlg, IDC_CMD));
                return TRUE;
            }
            if (!name[0]) {
                PROG tmp;

                memset(&tmp, 0, sizeof(tmp));
                tmp.cmd = cmd;
                ProgDisplayCmd(&tmp, buf, MAXP);
                ProgFromFile(&tmp, buf);
                lstrcpyn(name, tmp.name, sizeof(name));
                ProgFree(&tmp);
            }
            {
                WORD hk;
                char hot[40];

                GetDlgItemText(hDlg, IDC_HOTKEY, hot, sizeof(hot));
                if (!HotkeyParse(hot, &hk)) {
                    MessageBox(hDlg, "Type a shortcut key such as Ctrl+Alt+N, Ctrl+Shift+F5 "
                               "or just N, or None.", APPNAME, MB_ICONEXCLAMATION);
                    SetFocus(GetDlgItem(hDlg, IDC_HOTKEY));
                    return TRUE;
                }
                pgProg->hotkey = hk;
            }
            ProgSet(pgProg, name, cmd, dir, pgIcon, pgIdx);
            EndDialog(hDlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL DlgProgram(PROG *p, BOOL isNew)
{
    pgProg = p;
    pgNew = isNew;
    return RunDialog(IDD_PROGRAM, (FARPROC)ProgramProc, 0) == 1;
}

/* ---- run ------------------------------------------------------------------- */

static char runInit[MAXP];

DLGRESULT CALLBACK __export RunProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[MAXP], key[8];
    int i;

    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        for (i = 1; i <= 12; i++) {
            sprintf(key, "R%d", i);
            IniGet("Run", key, "", buf, MAXP);
            if (buf[0])
                SendDlgItemMessage(hDlg, IDC_EDIT, CB_ADDSTRING, 0, (LPARAM)(LPSTR)buf);
        }
        SetDlgItemText(hDlg, IDC_EDIT, runInit);
        if (cur.kind == LOC_DIR) {
            sprintf(buf, "Starts in %s", cur.path);
            SetDlgItemText(hDlg, IDC_INFO, buf);
        }
        CenterWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_EDIT));
        return FALSE;
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDC_BROWSE:
            GetDlgItemText(hDlg, IDC_EDIT, buf, MAXP);
            if (BrowseFile(hDlg, buf, MAXP, progFilter, "Browse"))
                SetDlgItemText(hDlg, IDC_EDIT, buf);
            return TRUE;
        case IDOK: {
            BOOL minimized = IsDlgButtonChecked(hDlg, IDC_MINRUN);
            char old[MAXP];
            int k = 2;

            GetDlgItemText(hDlg, IDC_EDIT, buf, MAXP);
            if (!buf[0])
                return TRUE;
            if (!Launch(buf, cur.kind == LOC_DIR ? cur.path : NULL,
                        minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL))
                return TRUE;
            /* most recent command first, no duplicates */
            for (i = 1; i <= 12 && k <= 12; i++) {
                sprintf(key, "R%d", i);
                IniGet("Run", key, "", old, MAXP);
                if (old[0] && lstrcmpi(old, buf) != 0) {
                    char *saved = StrDup(old);

                    sprintf(key, "T%d", k++);
                    IniPut("Run", key, saved);
                    free(saved);
                }
            }
            for (i = 2; i <= 12; i++) {
                sprintf(key, "T%d", i);
                IniGet("Run", key, "", old, MAXP);
                IniPut("Run", key, NULL);
                sprintf(key, "R%d", i);
                IniPut("Run", key, old[0] ? old : NULL);
            }
            IniPut("Run", "R1", buf);
            EndDialog(hDlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

void DlgRun(const char *initial)
{
    lstrcpyn(runInit, initial ? initial : "", MAXP);
    RunDialog(IDD_RUN, (FARPROC)RunProc, 0);
}

/* ---- find ------------------------------------------------------------------ */

static char fdDir[MAXP], fdPat[80], fdText[80];
static BOOL fdSub;

DLGRESULT CALLBACK __export FindProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemText(hDlg, IDC_PATTERN, fdPat);
        SetDlgItemText(hDlg, IDC_TEXT, fdText);
        SetDlgItemText(hDlg, IDC_DIR, fdDir);
        CheckDlgButton(hDlg, IDC_SUBDIRS, fdSub);
        CenterWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_PATTERN));
        SendDlgItemMessage(hDlg, IDC_PATTERN, EM_SETSEL, 0, MAKELONG(0, -1));
#ifdef RETRO_WIN32
        SendDlgItemMessage(hDlg, IDC_PATTERN, EM_SETSEL, 0, -1);
#endif
        return FALSE;
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDOK:
            GetDlgItemText(hDlg, IDC_PATTERN, fdPat, sizeof(fdPat));
            GetDlgItemText(hDlg, IDC_TEXT, fdText, sizeof(fdText));
            GetDlgItemText(hDlg, IDC_DIR, fdDir, sizeof(fdDir));
            fdSub = IsDlgButtonChecked(hDlg, IDC_SUBDIRS);
            PathNormalize(fdDir);
            if (!IsDir(fdDir)) {
                MessageBox(hDlg, "The folder to search in does not exist.", APPNAME, MB_ICONEXCLAMATION);
                return TRUE;
            }
            EndDialog(hDlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL DlgFind(void)
{
    if (cur.kind == LOC_DIR || cur.kind == LOC_SEARCH)
        lstrcpyn(fdDir, cur.path, MAXP);
    else if (!fdDir[0])
        strcpy(fdDir, "C:\\");
    if (!fdPat[0]) {
        strcpy(fdPat, "*.*");
        fdSub = TRUE;
    }
    if (RunDialog(IDD_FIND, (FARPROC)FindProc, 0) != 1)
        return FALSE;
    NavSearch(fdDir, fdPat, fdText, fdSub);
    return TRUE;
}

/* ---- properties ------------------------------------------------------------- */

static PATHLIST prList;
static int prAttrAnd, prAttrOr;     /* attributes common to / present in the selection */
static HICON prIcon;
static ITEM *prItem;

static void Tally(const char *path, DWORD *bytes, long *files, long *dirs, int depth)
{
    FINDINFO fi;
    FINDSTATE *fs;
    char *sub;

    if (depth > 30)
        return;
    sub = (char *)malloc(MAXP);
    if (!sub)
        return;
    fs = FsOpen(path, "*.*", &fi);
    if (fs) {
        do {
            if (fi.attr & FA_DIR) {
                (*dirs)++;
                PathJoin(sub, path, fi.name);
                Tally(sub, bytes, files, dirs, depth + 1);
            } else {
                (*files)++;
                *bytes += fi.size;
            }
        } while (FsNext(fs, &fi));
        FsClose(fs);
    }
    free(sub);
    if ((*files & 63) == 0)
        PumpMessages();
}

DLGRESULT CALLBACK __export PropsProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static const int ids[4] = { IDC_RDONLY, IDC_HIDDEN, IDC_SYSTEM, IDC_ARCHIVE };
    static const int bits[4] = { FA_RDONLY, FA_HIDDEN, FA_SYSTEM, FA_ARCH };
    char buf[MAXP + 60], a[40], b[40];
    int i;

    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        DWORD bytes = 0;
        long files = 0, dirs = 0;
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));

        prAttrAnd = 0x3F;
        prAttrOr = 0;
        for (i = 0; i < prList.n; i++) {
            int at = GetAttr(prList.p[i]);

            if (at < 0)
                continue;
            prAttrAnd &= at;
            prAttrOr |= at;
            if (at & FA_DIR) {
                dirs++;
                Tally(prList.p[i], &bytes, &files, &dirs, 0);
            } else {
                WORD d, t;
                DWORD s;

                files++;
                if (GetStamp(prList.p[i], &d, &t, &s))
                    bytes += s;
            }
        }
        SetCursor(old);
        if (prList.n == 1) {
            SetDlgItemText(hDlg, IDC_NAME, PathName(prList.p[0]));
            lstrcpyn(buf, prList.p[0], MAXP);
            PathParent(buf);
            SetDlgItemText(hDlg, IDC_WHERE, buf);
            if (prItem) {
                ItemTypeName(prItem, a);
                SetDlgItemText(hDlg, IDC_TYPE, a);
                FmtDate(a, prItem->date, prItem->time);
                SetDlgItemText(hDlg, IDC_DATE, a);
            }
        } else {
            sprintf(buf, "%d items", prList.n);
            SetDlgItemText(hDlg, IDC_NAME, buf);
            SetDlgItemText(hDlg, IDC_TYPE, "Multiple types");
            lstrcpyn(buf, prList.p[0], MAXP);
            PathParent(buf);
            SetDlgItemText(hDlg, IDC_WHERE, buf);
        }
        FmtSize(a, bytes);
        FmtNum(b, bytes);
        if (bytes >= 1024)
            sprintf(buf, "%s (%s bytes)", a, b);
        else
            strcpy(buf, a);
        SetDlgItemText(hDlg, IDC_FSIZE, buf);
        if (dirs) {
            sprintf(buf, "%ld file%s, %ld folder%s", files, files == 1 ? "" : "s",
                    dirs, dirs == 1 ? "" : "s");
            SetDlgItemText(hDlg, IDC_CONTENTS, buf);
        } else {
            ShowWindow(GetDlgItem(hDlg, IDC_CONTLBL), SW_HIDE);
            ShowWindow(GetDlgItem(hDlg, IDC_CONTENTS), SW_HIDE);
        }
        for (i = 0; i < 4; i++)
            CheckDlgButton(hDlg, ids[i], (prAttrAnd & bits[i]) ? 1 : (prAttrOr & bits[i]) ? 2 : 0);
        prIcon = prItem ? NULL : BuiltinIcon(BI_DOC);
        if (prItem) {
            BOOL doc;

            prIcon = ItemIcon(prItem, &doc);
            if (!prIcon || doc)
                prIcon = BuiltinIcon(ItemBuiltin(prItem));
        }
        CenterWindow(hDlg);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;

        BeginPaint(hDlg, &ps);
        EndPaint(hDlg, &ps);
        PaintIconAt(hDlg, IDC_ICONBOX, prIcon);
        return TRUE;
    }
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDOK: {
            int failed = 0, k;

            for (k = 0; k < prList.n; k++) {
                int at = GetAttr(prList.p[k]), nat;

                if (at < 0)
                    continue;
                nat = at;
                for (i = 0; i < 4; i++) {
                    UINT st = IsDlgButtonChecked(hDlg, ids[i]);

                    if (st == 1)
                        nat |= bits[i];
                    else if (st == 0)
                        nat &= ~bits[i];
                }
                if (nat != at && !SetAttr(prList.p[k], nat & ~FA_DIR))
                    failed++;
            }
            if (failed)
                MessageBox(hDlg, "Some attributes could not be changed.", APPNAME, MB_ICONEXCLAMATION);
            EndDialog(hDlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

/* Properties of the selected files and folders, or of a program item. */
void DlgProperties(void)
{
    ITEM *it = ViewFocusItem();

    if (it && it->kind == IK_PROG && cur.kind == LOC_GROUP) {
        GROUP *gr = GroupPtr(cur.group);

        if (gr && it->ref >= 0 && it->ref < gr->n && DlgProgram(&gr->items[it->ref], FALSE)) {
            GroupsSave();
            IconsFlush();
            NavRefresh();
        }
        return;
    }
    if (it && it->kind == IK_GROUP && it->ref >= 0) {
        Command(IDM_RENGROUP);
        return;
    }
    SelectedPaths(&prList);
    if (!prList.n && cur.kind == LOC_DIR)
        PlAdd(&prList, cur.path);
    if (!prList.n) {
        PlFree(&prList);
        return;
    }
    prItem = prList.n == 1 && it && ViewSelCount() == 1 ? it : NULL;
    if (RunDialog(IDD_PROPS, (FARPROC)PropsProc, 0) == 1)
        NavRefresh();
    PlFree(&prList);
}

/* ---- options --------------------------------------------------------------- */

static int optTheme;

static void ThemePreview(HWND hDlg)
{
    HWND ctl = GetDlgItem(hDlg, IDC_THEMEPREV);
    RECT r, q;
    HDC hdc;
    int save = themeIdx, w, h;

    if (!ctl)
        return;
    ThemeSet(optTheme);
    GetWindowRect(ctl, &r);
    ScreenToClient(hDlg, (POINT *)&r.left);
    ScreenToClient(hDlg, (POINT *)&r.right);
    w = r.right - r.left;
    h = r.bottom - r.top;
    hdc = GetDC(hDlg);
    SetRect(&q, r.left, r.top, r.left + w / 3, r.bottom);
    FillR(hdc, &q, th.side);
    SetRect(&q, r.left + 4, r.top + 8, r.left + w / 3 - 4, r.top + 20);
    Pill(hdc, &q, th.sideSel, th.sideSel);
    SetRect(&q, r.left + 4, r.top + 24, r.left + w / 3 - 10, r.top + 28);
    FillR(hdc, &q, th.sideText);
    SetRect(&q, r.left + w / 3, r.top, r.right, r.top + h / 3);
    Gradient(hdc, &q, th.band1, th.band2, FALSE);
    SetRect(&q, r.left + w / 3, r.top + h / 3, r.right, r.bottom);
    FillR(hdc, &q, th.view);
    SetRect(&q, r.left + w / 3 + 8, r.top + h / 3 + 6, r.left + w / 3 + 40, r.bottom - 6);
    Pill(hdc, &q, th.sel, th.selBorder);
    FrameR(hdc, &r, RGB(128, 128, 128));
    ReleaseDC(hDlg, hdc);
    ThemeSet(save);
}

static BOOL ShellIsUs(void)
{
    char shell[MAXP], me[MAXP];

    GetPrivateProfileString("boot", "shell", "", shell, MAXP, "SYSTEM.INI");
    GetModuleFileName(hInst, me, MAXP);
    return lstrcmpi(PathName(shell), PathName(me)) == 0;
}

DLGRESULT CALLBACK __export OptionsProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    int i;

    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        optTheme = themeIdx;
        for (i = 0; i < nThemes; i++)
            SendDlgItemMessage(hDlg, IDC_THEMES, LB_ADDSTRING, 0, (LPARAM)(LPSTR)ThemeName(i));
        SendDlgItemMessage(hDlg, IDC_THEMES, LB_SETCURSEL, themeIdx, 0);
        CheckDlgButton(hDlg, IDC_CONFDEL, confirmDelete);
        CheckDlgButton(hDlg, IDC_CONFOPS, confirmOps);
        CheckDlgButton(hDlg, IDC_MINIMIZE, minimizeOnUse);
        CheckDlgButton(hDlg, IDC_SAVEEXIT, saveOnExit);
        CheckDlgButton(hDlg, IDC_SHOWHID, showHidden);
        CheckDlgButton(hDlg, IDC_SHELL, ShellIsUs());
#ifdef RETRO_WIN32
        EnableWindow(GetDlgItem(hDlg, IDC_SHELL), FALSE);
#endif
        CenterWindow(hDlg);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;

        BeginPaint(hDlg, &ps);
        EndPaint(hDlg, &ps);
        ThemePreview(hDlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (CMD_ID(wParam, lParam)) {
        case IDC_THEMES:
            if (CMD_NOTIFY(wParam, lParam) == LBN_SELCHANGE) {
                optTheme = (int)SendDlgItemMessage(hDlg, IDC_THEMES, LB_GETCURSEL, 0, 0);
                ThemePreview(hDlg);
            }
            return TRUE;
        case IDOK: {
            BOOL hid = IsDlgButtonChecked(hDlg, IDC_SHOWHID), shell = IsDlgButtonChecked(hDlg, IDC_SHELL);

            confirmDelete = IsDlgButtonChecked(hDlg, IDC_CONFDEL);
            confirmOps = IsDlgButtonChecked(hDlg, IDC_CONFOPS);
            minimizeOnUse = IsDlgButtonChecked(hDlg, IDC_MINIMIZE);
            saveOnExit = IsDlgButtonChecked(hDlg, IDC_SAVEEXIT);
            if (shell != ShellIsUs()) {
                char me[MAXP];

                GetModuleFileName(hInst, me, MAXP);
                if (shell) {
                    char old[MAXP];

                    GetPrivateProfileString("boot", "shell", "progman.exe", old, MAXP, "SYSTEM.INI");
                    IniPut("Settings", "PreviousShell", old);
                    WritePrivateProfileString("boot", "shell", me, "SYSTEM.INI");
                    MessageBox(hDlg, "ShellIV will start as the Windows shell the next time "
                               "Windows starts.\n\nPrograms in the StartUp group are run "
                               "automatically.", APPNAME, MB_ICONINFORMATION);
                } else {
                    char old[MAXP];

                    IniGet("Settings", "PreviousShell", "progman.exe", old, MAXP);
                    WritePrivateProfileString("boot", "shell", old, "SYSTEM.INI");
                }
            }
            if (optTheme != themeIdx)
                Command(IDM_THEME0 + optTheme);
            if (hid != showHidden) {
                showHidden = hid;
                SideRebuild();
                NavRefresh();
            }
            SaveSettings();         /* kept even if Windows stops before ShellIV closes */
            EndDialog(hDlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

void DlgOptions(void)
{
    RunDialog(IDD_OPTIONS, (FARPROC)OptionsProc, 0);
}

/* ---- about ----------------------------------------------------------------- */

DLGRESULT CALLBACK __export AboutProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    char buf[200], a[40];

    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        sprintf(buf, "Version 1.0 for %s (%d-bit)", retro_target(), RETRO_BITS);
        SetDlgItemText(hDlg, IDC_INFO, buf);
        retro_host(a, sizeof(a));
#ifdef RETRO_WIN32
        {
            MEMORYSTATUS ms;

            ms.dwLength = sizeof(ms);
            GlobalMemoryStatus(&ms);
            sprintf(buf, "Running on %s", a);
            SetDlgItemText(hDlg, IDC_SYSINFO, buf);
            FmtKB(a, ms.dwAvailPhys / 1024);
            sprintf(buf, "Free memory: %s", a);
            SetDlgItemText(hDlg, IDC_MEMORY, buf);
            sprintf(buf, "Memory load: %lu%%", ms.dwMemoryLoad);
            SetDlgItemText(hDlg, IDC_RESOURCES, buf);
        }
#else
        {
            DWORD f = GetWinFlags();

            sprintf(buf, "Running on %s, %s mode, %s CPU", a,
                    (f & WF_ENHANCED) ? "386 Enhanced" : (f & WF_STANDARD) ? "Standard" : "Real",
                    (f & WF_CPU486) ? "486" : (f & WF_CPU386) ? "386" : (f & WF_CPU286) ? "286" : "8086");
            SetDlgItemText(hDlg, IDC_SYSINFO, buf);
            FmtKB(a, GetFreeSpace(0) / 1024);
            sprintf(buf, "Free memory: %s", a);
            SetDlgItemText(hDlg, IDC_MEMORY, buf);
            sprintf(buf, "System resources: %u%% free", GetFreeSystemResources(GFSR_SYSTEMRESOURCES));
            SetDlgItemText(hDlg, IDC_RESOURCES, buf);
        }
#endif
        CenterWindow(hDlg);
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;

        BeginPaint(hDlg, &ps);
        EndPaint(hDlg, &ps);
        PaintIconAt(hDlg, IDC_ICONBOX, LoadIcon(hInst, MAKEINTRESOURCE(IDI_APP)));
        return TRUE;
    }
    case WM_COMMAND:
        if (CMD_ID(wParam, lParam) == IDOK || CMD_ID(wParam, lParam) == IDCANCEL)
            EndDialog(hDlg, 1);
        return TRUE;
    }
    return FALSE;
}

void DlgAbout(void)
{
    RunDialog(IDD_ABOUT, (FARPROC)AboutProc, 0);
}
