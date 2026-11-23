/* ShellIV - strings, paths, formatting, profile and message helpers. */
#include <stdarg.h>
#include <ctype.h>
#include "shelliv.h"

char *StrCopyN(char *dst, const char *src, int n)
{
    int i;

    if (n <= 0)
        return dst;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

char *StrDup(const char *s)
{
    char *p;

    if (!s)
        s = "";
    p = (char *)malloc(strlen(s) + 1);
    if (p)
        strcpy(p, s);
    return p;
}

void StrFree(char **p)
{
    if (*p) {
        free(*p);
        *p = NULL;
    }
}

void StrSet(char **p, const char *s)
{
    char *n = StrDup(s);

    StrFree(p);
    *p = n;
}

/* ---- paths ----------------------------------------------------------- */

void PathJoin(char *out, const char *dir, const char *name)
{
    char tmp[MAXP];
    int n;

    lstrcpyn(tmp, dir, MAXP);
    n = strlen(tmp);
    if (n > 0 && tmp[n - 1] != '\\' && tmp[n - 1] != ':' && n < MAXP - 1)
        tmp[n++] = '\\';
    tmp[n] = '\0';
    lstrcpyn(tmp + n, name, MAXP - n);
    strcpy(out, tmp);
}

BOOL PathIsRoot(const char *path)
{
    return path[0] && path[1] == ':' && (path[2] == '\0' || (path[2] == '\\' && path[3] == '\0'));
}

BOOL PathParent(char *path)
{
    char *s;

    if (PathIsRoot(path))
        return FALSE;
    s = strrchr(path, '\\');
    if (!s)
        return FALSE;
    if (s == path + 2 && path[1] == ':')
        s[1] = '\0';                    /* "C:\DOS" -> "C:\" */
    else
        *s = '\0';
    return TRUE;
}

const char *PathName(const char *path)
{
    const char *s = strrchr(path, '\\');

    if (PathIsRoot(path))
        return path;
    return s ? s + 1 : path;
}

const char *PathExt(const char *path)
{
    const char *name = PathName(path), *dot = strrchr(name, '.');

    return dot ? dot : "";
}

void PathNormalize(char *path)
{
    int n;
    char *s;

    for (s = path; *s; s++)
        if (*s == '/')
            *s = '\\';
#ifndef RETRO_WIN32
    AnsiUpper(path);
#endif
    if (path[0] && path[1] == ':' && path[2] == '\0') {
        path[2] = '\\';
        path[3] = '\0';
    }
    n = strlen(path);
    while (n > 3 && path[n - 1] == '\\')
        path[--n] = '\0';
    if (path[1] == ':' && path[0] >= 'a' && path[0] <= 'z')
        path[0] = (char)(path[0] - 'a' + 'A');
}

BOOL PathInside(const char *path, const char *dir)
{
    int n = strlen(dir);
    char tmp[MAXP];

    if (n == 0 || n > (int)strlen(path))
        return FALSE;
    lstrcpyn(tmp, path, n + 1);
    if (lstrcmpi(tmp, dir) != 0)
        return FALSE;
    return path[n] == '\0' || path[n] == '\\' || dir[n - 1] == '\\';
}

static int Up(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

static BOOL Wild(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return TRUE;
            for (; *s; s++)
                if (Wild(p, s))
                    return TRUE;
            return FALSE;
        }
        if (!*s)
            return *p == '.' && p[1] == '*' && !p[2] ? TRUE : FALSE;
        if (*p != '?' && Up((unsigned char)*p) != Up((unsigned char)*s))
            return FALSE;
        p++;
        s++;
    }
    return *s == '\0';
}

/* Matches name against one or more patterns separated by ';', ',' or ' '.
 * A pattern without wildcards matches names that contain it. */
BOOL WildMatch(const char *pat, const char *name)
{
    char one[80];
    int n;

    while (*pat) {
        while (*pat == ';' || *pat == ',' || *pat == ' ')
            pat++;
        for (n = 0; *pat && *pat != ';' && *pat != ',' && *pat != ' ' && n < 70; pat++)
            one[n++] = *pat;
        one[n] = '\0';
        if (!n)
            continue;
        if (strcmp(one, "*.*") == 0)
            return TRUE;
        if (!strchr(one, '*') && !strchr(one, '?')) {
            memmove(one + 1, one, n + 1);
            one[0] = '*';
            strcat(one, "*");
        }
        if (Wild(one, name))
            return TRUE;
    }
    return FALSE;
}

/* ---- formatting -------------------------------------------------------- */

void FmtNum(char *out, DWORD n)
{
    char tmp[16];
    int len, i, j = 0;

    sprintf(tmp, "%lu", n);
    len = strlen(tmp);
    for (i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0)
            out[j++] = ',';
        out[j++] = tmp[i];
    }
    out[j] = '\0';
}

void FmtKB(char *out, DWORD kb)
{
    if (kb < 1024UL)
        sprintf(out, "%lu KB", kb);
    else if (kb < 10UL * 1024)
        sprintf(out, "%lu.%lu MB", kb / 1024, (kb % 1024) * 10 / 1024);
    else if (kb < 1024UL * 1024)
        sprintf(out, "%lu MB", kb / 1024);
    else
        sprintf(out, "%lu.%lu GB", kb / (1024UL * 1024), (kb % (1024UL * 1024)) / 104858UL);
}

void FmtSize(char *out, DWORD bytes)
{
    if (bytes < 1024UL) {
        sprintf(out, "%lu bytes", bytes);
        if (bytes == 1)
            strcpy(out, "1 byte");
    } else if (bytes < 10UL * 1024) {
        sprintf(out, "%lu.%lu KB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    } else {
        FmtKB(out, (bytes + 1023) / 1024);
    }
}

void FmtDate(char *out, WORD date, WORD time)
{
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int m = (date >> 5) & 15;

    if (!date) {
        out[0] = '\0';
        return;
    }
    if (m < 1 || m > 12)
        m = 1;
    sprintf(out, "%02d-%s-%04d %02d:%02d", date & 31, mon[m - 1], (date >> 9) + 1980,
            time >> 11, (time >> 5) & 63);
}

void FmtAttr(char *out, int attr)
{
    out[0] = (char)(attr & FA_RDONLY ? 'R' : '-');
    out[1] = (char)(attr & FA_HIDDEN ? 'H' : '-');
    out[2] = (char)(attr & FA_SYSTEM ? 'S' : '-');
    out[3] = (char)(attr & FA_ARCH ? 'A' : '-');
    out[4] = '\0';
}

/* ---- profile --------------------------------------------------------- */

int IniInt(const char *sec, const char *key, int def)
{
#ifdef RETRO_WIN32
    return (int)GetPrivateProfileInt(sec, key, def, INIFILE);
#else
    return (int)(short)GetPrivateProfileInt(sec, key, def, INIFILE);   /* 16-bit UINT */
#endif
}

void IniGet(const char *sec, const char *key, const char *def, char *out, int size)
{
    GetPrivateProfileString(sec, key, def, out, size, INIFILE);
}

void IniPut(const char *sec, const char *key, const char *val)
{
    WritePrivateProfileString(sec, key, val, INIFILE);
}

void IniPutInt(const char *sec, const char *key, int val)
{
    char buf[16];

    sprintf(buf, "%d", val);
    IniPut(sec, key, buf);
}

/* ---- messages and dialogs ---------------------------------------------- */

int Msg(UINT flags, const char *fmt, ...)
{
    char buf[600];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    return MessageBox(hFrame ? hFrame : NULL, buf, APPNAME, flags);
}

extern HWND hProgress;

BOOL PumpMessages(void)
{
    MSG msg;
    BOOL ok = TRUE;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            return FALSE;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            ok = FALSE;
            continue;
        }
        if (hProgress && IsDialogMessage(hProgress, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return ok;
}

int RunDialog(int id, FARPROC proc, LPARAM lp)
{
    FARPROC thunk = THUNK(proc);
    HWND owner = GetActiveWindow();
    int r;

    /* a dialog opened from another dialog belongs to that dialog, so
     * closing it does not re-enable the main window underneath */
    if (!owner || (owner != hFrame && WINDOW_TASK(owner) != WINDOW_TASK(hFrame)))
        owner = hFrame;
    r = DialogBoxParam(hInst, MAKEINTRESOURCE(id), owner, (DLGPROC)thunk, lp);

    FREETHUNK(thunk);
    return r;
}

void CenterWindow(HWND hwnd)
{
    RECT r, p;
    HWND parent = GetParent(hwnd);

    GetWindowRect(hwnd, &r);
    if (parent && IsWindowVisible(parent) && !IsIconic(parent))
        GetWindowRect(parent, &p);
    else
        SetRect(&p, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    SetWindowPos(hwnd, NULL,
                 max(0, p.left + (p.right - p.left - (r.right - r.left)) / 2),
                 max(0, p.top + (p.bottom - p.top - (r.bottom - r.top)) / 2),
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}
