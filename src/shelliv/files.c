/* ShellIV - file system access for Win16 (DOS calls) and Win32. */
#include "shelliv.h"
#ifndef RETRO_WIN32
#include <dos.h>
#include <direct.h>
#include <i86.h>
#endif

/* ---- directory enumeration ---------------------------------------------- */

struct FINDSTATE {
#ifdef RETRO_WIN32
    HANDLE h;
    WIN32_FIND_DATA fd;
#else
    struct find_t ft;
#endif
};

#ifdef RETRO_WIN32
static void ToDos(const FILETIME *ft, WORD *date, WORD *time)
{
    FILETIME local;

    FileTimeToLocalFileTime(ft, &local);
    if (!FileTimeToDosDateTime(&local, date, time))
        *date = *time = 0;
}
#endif

static BOOL Fill(FINDSTATE *fs, FINDINFO *fi)
{
#ifdef RETRO_WIN32
    const char *n = fs->fd.cFileName;

    if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
        return FALSE;
    lstrcpyn(fi->name, n, MAXP);
    fi->size = fs->fd.nFileSizeLow;
    fi->attr = (BYTE)(fs->fd.dwFileAttributes & 0x3F);
    ToDos(&fs->fd.ftLastWriteTime, &fi->date, &fi->time);
#else
    const char *n = fs->ft.name;

    if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
        return FALSE;
    if (fs->ft.attrib & _A_VOLID)
        return FALSE;
    lstrcpyn(fi->name, n, MAXP);
    fi->size = fs->ft.size;
    fi->attr = (BYTE)(fs->ft.attrib & 0x3F);
    fi->date = fs->ft.wr_date;
    fi->time = fs->ft.wr_time;
#endif
    if (fi->attr & FA_DIR)
        fi->size = 0;
    return TRUE;
}

FINDSTATE *FsOpen(const char *dir, const char *pattern, FINDINFO *fi)
{
    FINDSTATE *fs = (FINDSTATE *)malloc(sizeof(FINDSTATE));
    char spec[MAXP];

    if (!fs)
        return NULL;
    PathJoin(spec, dir, pattern ? pattern : "*.*");
#ifdef RETRO_WIN32
    fs->h = FindFirstFile(spec, &fs->fd);
    if (fs->h == INVALID_HANDLE_VALUE) {
        free(fs);
        return NULL;
    }
#else
    if (_dos_findfirst(spec, _A_NORMAL | _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_SUBDIR | _A_ARCH,
                       &fs->ft) != 0) {
        free(fs);
        return NULL;
    }
#endif
    if (Fill(fs, fi))
        return fs;
    if (FsNext(fs, fi))
        return fs;
    FsClose(fs);
    return NULL;
}

BOOL FsNext(FINDSTATE *fs, FINDINFO *fi)
{
    for (;;) {
#ifdef RETRO_WIN32
        if (!FindNextFile(fs->h, &fs->fd))
            return FALSE;
#else
        if (_dos_findnext(&fs->ft) != 0)
            return FALSE;
#endif
        if (Fill(fs, fi))
            return TRUE;
    }
}

void FsClose(FINDSTATE *fs)
{
    if (!fs)
        return;
#ifdef RETRO_WIN32
    FindClose(fs->h);
#else
    _dos_findclose(&fs->ft);
#endif
    free(fs);
}

/* ---- drives -------------------------------------------------------------- */

#ifndef RETRO_WIN32
static BOOL IsCdrom(int drive)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.w.ax = 0x150B;                    /* MSCDEX: drive check */
    r.w.cx = (unsigned short)drive;
    int86(0x2F, &r, &r);
    return r.w.bx == 0xADAD && r.w.ax != 0;
}
#endif

int DriveKind(int drive)
{
#ifdef RETRO_WIN32
    char root[4];

    root[0] = (char)('A' + drive);
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    switch (GetDriveType(root)) {
    case DRIVE_REMOVABLE: return DK_FLOPPY;
    case DRIVE_FIXED:     return DK_HARD;
    case DRIVE_REMOTE:    return DK_NET;
    case DRIVE_CDROM:     return DK_CDROM;
    case DRIVE_RAMDISK:   return DK_RAM;
    }
    return DK_NONE;
#else
    switch (GetDriveType(drive)) {
    case DRIVE_REMOVABLE: return DK_FLOPPY;
    case DRIVE_FIXED:     return DK_HARD;
    case DRIVE_REMOTE:    return IsCdrom(drive) ? DK_CDROM : DK_NET;
    }
    return DK_NONE;
#endif
}

BOOL DriveSpace(int drive, DWORD *freeKB, DWORD *totalKB)
{
#ifdef RETRO_WIN32
    char root[4];
    DWORD spc, bps, nfree, ntotal;

    root[0] = (char)('A' + drive);
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    *freeKB = *totalKB = 0;
    if (!GetDiskFreeSpace(root, &spc, &bps, &nfree, &ntotal))
        return FALSE;
    spc *= bps;                         /* bytes per cluster */
    if (spc >= 1024) {
        *freeKB = nfree * (spc / 1024);
        *totalKB = ntotal * (spc / 1024);
    } else {
        *freeKB = nfree / (1024 / max(1UL, spc));
        *totalKB = ntotal / (1024 / max(1UL, spc));
    }
    return TRUE;
#else
    struct diskfree_t df;
    DWORD bpc;

    *freeKB = *totalKB = 0;
    if (_dos_getdiskfree(drive + 1, &df) != 0)
        return FALSE;
    bpc = (DWORD)df.sectors_per_cluster * df.bytes_per_sector;
    if (bpc >= 1024) {
        *freeKB = (DWORD)df.avail_clusters * (bpc / 1024);
        *totalKB = (DWORD)df.total_clusters * (bpc / 1024);
    } else if (bpc) {
        *freeKB = (DWORD)df.avail_clusters / (1024 / bpc);
        *totalKB = (DWORD)df.total_clusters / (1024 / bpc);
    }
    return TRUE;
#endif
}

BOOL DriveLabel(int drive, char *out, int size)
{
    char root[8];

    sprintf(root, "%c:\\", 'A' + drive);
    out[0] = '\0';
#ifdef RETRO_WIN32
    {
        DWORD serial, maxlen, flags;

        if (!GetVolumeInformation(root, out, size, &serial, &maxlen, &flags, NULL, 0))
            return FALSE;
    }
#else
    {
        struct find_t ft;
        char *dot;

        strcat(root, "*.*");
        if (_dos_findfirst(root, _A_VOLID, &ft) != 0)
            return FALSE;
        _dos_findclose(&ft);
        dot = strchr(ft.name, '.');
        if (dot)
            memmove(dot, dot + 1, strlen(dot));
        lstrcpyn(out, ft.name, size);
    }
#endif
    return out[0] != '\0';
}

/* ---- files ---------------------------------------------------------------- */

int GetAttr(const char *path)
{
#ifdef RETRO_WIN32
    DWORD a = GetFileAttributes(path);

    return a == 0xFFFFFFFFUL ? -1 : (int)(a & 0x3F);
#else
    unsigned a;

    if (PathIsRoot(path))
        return FA_DIR;
    return _dos_getfileattr(path, &a) == 0 ? (int)(a & 0x3F) : -1;
#endif
}

BOOL SetAttr(const char *path, int attr)
{
    attr &= FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_ARCH;
#ifdef RETRO_WIN32
    return SetFileAttributes(path, attr ? (DWORD)attr : FILE_ATTRIBUTE_NORMAL);
#else
    return _dos_setfileattr(path, attr) == 0;
#endif
}

BOOL IsDir(const char *path)
{
    int a = GetAttr(path);

    return a >= 0 && (a & FA_DIR);
}

BOOL Exists(const char *path)
{
    return GetAttr(path) >= 0;
}

BOOL GetStamp(const char *path, WORD *date, WORD *time, DWORD *size)
{
    FINDINFO fi;
    FINDSTATE *fs;
    char dir[MAXP];

    lstrcpyn(dir, path, MAXP);
    if (!PathParent(dir))
        return FALSE;
    fs = FsOpen(dir, PathName(path), &fi);
    if (!fs)
        return FALSE;
    FsClose(fs);
    *date = fi.date;
    *time = fi.time;
    *size = fi.size;
    return TRUE;
}

BOOL MakeDir(const char *path)
{
#ifdef RETRO_WIN32
    return CreateDirectory(path, NULL);
#else
    return mkdir(path) == 0;
#endif
}

BOOL RemoveDir(const char *path)
{
#ifdef RETRO_WIN32
    return RemoveDirectory(path);
#else
    return rmdir(path) == 0;
#endif
}

BOOL DeleteOne(const char *path)
{
#ifdef RETRO_WIN32
    return DeleteFile(path);
#else
    return remove(path) == 0;
#endif
}

BOOL RenameOne(const char *from, const char *to)
{
#ifdef RETRO_WIN32
    return MoveFile(from, to);
#else
    return rename(from, to) == 0;
#endif
}

#define COPYBUF 16384

BOOL CopyFileData(const char *from, const char *to, BOOL (*progress)(DWORD done))
{
    char *buf = (char *)malloc(COPYBUF);
    DWORD done = 0;
    BOOL ok = FALSE;
#ifdef RETRO_WIN32
    HANDLE in, out;
    DWORD n, w;
    FILETIME c, a, m;

    if (!buf)
        return FALSE;
    in = CreateFile(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) {
        free(buf);
        return FALSE;
    }
    out = CreateFile(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        for (;;) {
            if (!ReadFile(in, buf, COPYBUF, &n, NULL))
                break;
            if (n == 0) {
                ok = TRUE;
                break;
            }
            if (!WriteFile(out, buf, n, &w, NULL) || w != n)
                break;
            done += n;
            if (progress && !progress(done))
                break;
        }
        if (ok && GetFileTime(in, &c, &a, &m))
            SetFileTime(out, &c, &a, &m);
        CloseHandle(out);
        if (!ok)
            DeleteFile(to);
    }
    CloseHandle(in);
#else
    HFILE in, out;
    UINT n;
    unsigned d, t;

    if (!buf)
        return FALSE;
    in = _lopen(from, OF_READ | OF_SHARE_DENY_WRITE);
    if (in == HFILE_ERROR)
        in = _lopen(from, OF_READ);
    if (in == HFILE_ERROR) {
        free(buf);
        return FALSE;
    }
    out = _lcreat(to, 0);
    if (out != HFILE_ERROR) {
        for (;;) {
            n = _lread(in, buf, COPYBUF);
            if (n == (UINT)HFILE_ERROR)
                break;
            if (n == 0) {
                ok = TRUE;
                break;
            }
            if (_lwrite(out, buf, n) != n)
                break;
            done += n;
            if (progress && !progress(done))
                break;
        }
        if (ok && _dos_getftime(in, &d, &t) == 0)
            _dos_setftime(out, d, t);
        _lclose(out);
        if (!ok)
            remove(to);
    }
    _lclose(in);
#endif
    free(buf);
    if (ok) {
        int a = GetAttr(from);

        if (a > 0)
            SetAttr(to, a);
    }
    return ok;
}

/* Makes path unique: "NAME.TXT" -> "NAME~2.TXT" (Win16) or "Name (2).txt". */
void UniqueName(char *path)
{
    char base[MAXP], ext[20], dir[MAXP], name[MAXP], *dot;
    int n;

    if (!Exists(path))
        return;
    lstrcpyn(dir, path, MAXP);
    PathParent(dir);
    lstrcpyn(base, PathName(path), MAXP);
    dot = strrchr(base, '.');
    ext[0] = '\0';
    if (dot && dot != base) {
        lstrcpyn(ext, dot, sizeof(ext));
        *dot = '\0';
    }
    for (n = 2; n < 1000; n++) {
#ifdef RETRO_WIN32
        sprintf(name, "%s (%d)%s", base, n, ext);
#else
        char num[8];
        int keep;

        sprintf(num, "~%d", n);
        keep = 8 - strlen(num);
        if ((int)strlen(base) < keep)
            keep = strlen(base);
        sprintf(name, "%.*s%s%s", keep, base, num, ext);
#endif
        PathJoin(path, dir, name);
        if (!Exists(path))
            return;
    }
}
