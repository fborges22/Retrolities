/* ShellIV - SHELL.DLL calls for the Windows 386 extender.
 *
 * The extender's import thunks for SHELL.DLL do not marshal the arguments
 * (USER, GDI and KERNEL calls are fine), so these wrappers call the 16-bit
 * functions through _Call16(), which converts flat pointers to 16:16. */
#define SHELL16_C
#include "shelliv.h"

#ifdef RETRO_WIN386

static FARPROC Proc(const char *name)
{
    static HINSTANCE shell;

    if (!shell)
        shell = GetModuleHandle("SHELL");
    if (!shell)
        shell = LoadLibrary("SHELL.DLL");
    if ((UINT)shell < 32)
        return NULL;
    return GetProcAddress(shell, name);
}

HICON W386ExtractIcon(HINSTANCE inst, const char *file, UINT idx)
{
    FARPROC p = Proc("ExtractIcon");

    if (!p || !file)
        return NULL;
    return (HICON)LOWORD(_Call16(p, "wpw", inst, file, idx));
}

HINSTANCE W386FindExecutable(const char *file, const char *dir, char *result)
{
    FARPROC p = Proc("FindExecutable");

    result[0] = '\0';
    if (!p || !file)
        return (HINSTANCE)2;
    if (dir && dir[0])
        return (HINSTANCE)LOWORD(_Call16(p, "ppp", file, dir, result));
    return (HINSTANCE)LOWORD(_Call16(p, "pdp", file, 0L, result));
}

HINSTANCE W386ShellExecute(HWND hwnd, const char *op, const char *file, const char *args,
                           const char *dir, int show)
{
    FARPROC p = Proc("ShellExecute");
    char fmt[8];

    if (!p || !file)
        return (HINSTANCE)2;
    fmt[0] = 'w';
    fmt[1] = (char)(op ? 'p' : 'd');
    fmt[2] = 'p';
    fmt[3] = (char)(args ? 'p' : 'd');
    fmt[4] = (char)(dir ? 'p' : 'd');
    fmt[5] = 'w';
    fmt[6] = '\0';
    return (HINSTANCE)LOWORD(_Call16(p, fmt, hwnd, op ? (DWORD)op : 0L, file,
                                     args ? (DWORD)args : 0L, dir ? (DWORD)dir : 0L, show));
}

UINT W386DragQueryFile(HDROP hd, UINT i, char *buf, UINT size)
{
    FARPROC p = Proc("DragQueryFile");

    if (!p)
        return 0;
    if (!buf)
        return LOWORD(_Call16(p, "wwdw", hd, i, 0L, 0));
    return LOWORD(_Call16(p, "wwpw", hd, i, buf, size));
}

BOOL W386DragQueryPoint(HDROP hd, POINT *pt)
{
    FARPROC p = Proc("DragQueryPoint");

    if (!p)
        return FALSE;
    return (BOOL)LOWORD(_Call16(p, "wp", hd, pt));
}

void W386DragFinish(HDROP hd)
{
    FARPROC p = Proc("DragFinish");

    if (p)
        _Call16(p, "w", hd);
}

void W386DragAcceptFiles(HWND hwnd, BOOL accept)
{
    FARPROC p = Proc("DragAcceptFiles");

    if (p)
        _Call16(p, "ww", hwnd, accept);
}
/* network drives: USER.EXE's WNet functions */
static FARPROC UserProc(const char *name)
{
    HINSTANCE user = GetModuleHandle("USER");

    return user ? GetProcAddress(user, name) : NULL;
}

UINT W386WNetAddConnection(const char *path, const char *pass, const char *local)
{
    FARPROC p = UserProc("WNETADDCONNECTION");

    if (!p)
        return 1;           /* WN_NOT_SUPPORTED */
    if (pass && *pass)
        return LOWORD(_Call16(p, "ppp", path, pass, local));
    return LOWORD(_Call16(p, "pdp", path, 0L, local));
}

UINT W386WNetCancelConnection(const char *name, BOOL force)
{
    FARPROC p = UserProc("WNETCANCELCONNECTION");

    if (!p)
        return 1;
    return LOWORD(_Call16(p, "pw", name, force));
}
#else
typedef int shell16_empty;      /* nothing to do on other platforms */
#endif
