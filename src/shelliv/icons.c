/* ShellIV - item icons: icons extracted from programs, the icons of the
 * programs associated with documents, and the builtin artwork. */
#include "shelliv.h"

#define NCACHE 96

typedef struct {
    char key[MAXP + 8];
    HICON h;            /* NULL: the file has no usable icon */
    DWORD used;
} ICONENT;

static ICONENT cache[NCACHE];
static DWORD clock_;

static ICONENT *Lookup(const char *key)
{
    int i;

    for (i = 0; i < NCACHE; i++)
        if (cache[i].key[0] && lstrcmpi(cache[i].key, key) == 0) {
            cache[i].used = ++clock_;
            return &cache[i];
        }
    return NULL;
}

static ICONENT *Slot(const char *key)
{
    int i, best = 0;

    for (i = 0; i < NCACHE; i++) {
        if (!cache[i].key[0]) {
            best = i;
            break;
        }
        if (cache[i].used < cache[best].used)
            best = i;
    }
    if (cache[best].h)
        DestroyIcon(cache[best].h);
    lstrcpyn(cache[best].key, key, sizeof(cache[best].key));
    cache[best].h = NULL;
    cache[best].used = ++clock_;
    return &cache[best];
}

void IconsFlush(void)
{
    int i;

    for (i = 0; i < NCACHE; i++) {
        if (cache[i].h)
            DestroyIcon(cache[i].h);
        cache[i].h = NULL;
        cache[i].key[0] = '\0';
    }
}

static BOOL IsProgramExt(const char *ext)
{
    return lstrcmpi(ext, ".EXE") == 0;
}

/* The icon of a program file (index idx), cached. */
HICON FileIcon(const char *file, int idx)
{
    char key[MAXP + 8];
    ICONENT *e;
    HICON h;

    if (!file[0])
        return NULL;
    sprintf(key, "%s,%d", file, idx);
    e = Lookup(key);
    if (e)
        return e->h;
    h = ExtractIcon(hInst, file, idx);
    if ((UINT)h <= 1)
        h = NULL;
    e = Slot(key);
    e->h = h;
    return h;
}

/* Number of icons in a file (for the icon picker). */
int FileIconCount(const char *file)
{
    HICON h = ExtractIcon(hInst, file, (UINT)-1);

    return (int)(UINT)h;
}

/* The icon of the program associated with a document, cached by extension. */
static HICON AssocIcon(const char *path)
{
    char key[16], exe[MAXP], dir[MAXP];
    const char *ext = PathExt(path);
    ICONENT *e;
    HICON h = NULL;

    if (!ext[0] || strlen(ext) > 10)
        return NULL;
    sprintf(key, "*%s", ext);
    e = Lookup(key);
    if (e)
        return e->h;
    lstrcpyn(dir, path, MAXP);
    PathParent(dir);
    exe[0] = '\0';
    if ((UINT)FindExecutable(path, dir, exe) > 32 && exe[0]) {
        h = ExtractIcon(hInst, exe, 0);
        if ((UINT)h <= 1)
            h = NULL;
    }
    e = Slot(key);
    e->h = h;
    return h;
}

/* ---- classification -------------------------------------------------------- */

typedef struct { const char *ext; const char *type; int bi; int glyph; } FTYPE;

static const FTYPE ftypes[] = {
    { ".EXE", "Application", BI_PROGRAM, G_PROGRAM },
    { ".COM", "MS-DOS Program", BI_DOSPROG, G_DOSPROG },
    { ".BAT", "MS-DOS Batch File", BI_DOSPROG, G_DOSPROG },
    { ".PIF", "Program Information File", BI_DOSPROG, G_DOSPROG },
    { ".TXT", "Text Document", BI_TEXT, G_TEXT },
    { ".INI", "Configuration Settings", BI_TEXT, G_TEXT },
    { ".LOG", "Log File", BI_TEXT, G_TEXT },
    { ".DOC", "Document", BI_TEXT, G_TEXT },
    { ".WRI", "Write Document", BI_TEXT, G_TEXT },
    { ".C", "C Source File", BI_TEXT, G_TEXT },
    { ".H", "C Header File", BI_TEXT, G_TEXT },
    { ".ASM", "Assembly Source", BI_TEXT, G_TEXT },
    { ".BMP", "Bitmap Image", BI_IMAGE, G_IMAGE },
    { ".PCX", "PCX Image", BI_IMAGE, G_IMAGE },
    { ".GIF", "GIF Image", BI_IMAGE, G_IMAGE },
    { ".JPG", "JPEG Image", BI_IMAGE, G_IMAGE },
    { ".PNG", "PNG Image", BI_IMAGE, G_IMAGE },
    { ".TIF", "TIFF Image", BI_IMAGE, G_IMAGE },
    { ".ICO", "Icon", BI_IMAGE, G_IMAGE },
    { ".WMF", "Windows Metafile", BI_IMAGE, G_IMAGE },
    { ".DLL", "Application Extension", BI_SYSFILE, G_SYSFILE },
    { ".SYS", "System File", BI_SYSFILE, G_SYSFILE },
    { ".DRV", "Device Driver", BI_SYSFILE, G_SYSFILE },
    { ".386", "Virtual Device Driver", BI_SYSFILE, G_SYSFILE },
    { ".VXD", "Virtual Device Driver", BI_SYSFILE, G_SYSFILE },
    { ".FON", "Font", BI_SYSFILE, G_SYSFILE },
    { ".FOT", "TrueType Font Resource", BI_SYSFILE, G_SYSFILE },
    { ".TTF", "TrueType Font", BI_SYSFILE, G_SYSFILE },
    { ".CPL", "Control Panel Applet", BI_SYSFILE, G_SYSFILE },
    { ".GRP", "Program Group", BI_GROUP, G_GROUP },
    { ".HLP", "Help File", BI_DOC, G_DOC },
    { ".ZIP", "ZIP Archive", BI_ARCHIVE, G_ARCHIVE },
    { ".ARJ", "ARJ Archive", BI_ARCHIVE, G_ARCHIVE },
    { ".LZH", "LHA Archive", BI_ARCHIVE, G_ARCHIVE },
    { ".CAB", "Cabinet", BI_ARCHIVE, G_ARCHIVE },
    { ".WAV", "Wave Sound", BI_MUSIC, G_MUSIC },
    { ".MID", "MIDI Sequence", BI_MUSIC, G_MUSIC },
    { ".RMI", "MIDI Sequence", BI_MUSIC, G_MUSIC },
    { ".VOC", "Creative Voice", BI_MUSIC, G_MUSIC },
    { NULL, NULL, 0, 0 }
};

static const FTYPE *FType(const char *name)
{
    const char *ext = PathExt(name);
    const FTYPE *t;

    if (!ext[0])
        return NULL;
    for (t = ftypes; t->ext; t++)
        if (lstrcmpi(t->ext, ext) == 0)
            return t;
    return NULL;
}

void ItemTypeName(const ITEM *it, char *out)
{
    const FTYPE *t;
    const char *ext;

    switch (it->kind) {
    case IK_DRIVE:
        switch (it->dk) {
        case DK_FLOPPY: strcpy(out, "Floppy Disk Drive"); return;
        case DK_CDROM:  strcpy(out, "CD-ROM Drive"); return;
        case DK_NET:    strcpy(out, "Network Drive"); return;
        case DK_RAM:    strcpy(out, "RAM Disk"); return;
        }
        strcpy(out, "Hard Disk");
        return;
    case IK_DIR:
        strcpy(out, "File Folder");
        return;
    case IK_PROG:
        strcpy(out, "Program Item");
        return;
    case IK_GROUP:
        strcpy(out, "Program Group");
        return;
    }
    if (it->kind == IK_WASTE && (it->attr & FA_DIR)) {
        strcpy(out, "File Folder");
        return;
    }
    t = FType(it->name);
    if (t) {
        strcpy(out, t->type);
        return;
    }
    ext = PathExt(it->name);
    if (ext[0]) {
        int i;

        sprintf(out, "%s File", ext + 1);
        for (i = 0; out[i] != ' '; i++)
            if (out[i] >= 'a' && out[i] <= 'z')
                out[i] = (char)(out[i] - 'a' + 'A');
    } else {
        strcpy(out, "File");
    }
}

int ItemBuiltin(const ITEM *it)
{
    const FTYPE *t;

    switch (it->kind) {
    case IK_DRIVE:
        switch (it->dk) {
        case DK_FLOPPY: return BI_FLOPPY;
        case DK_CDROM:  return BI_CDROM;
        case DK_NET:    return BI_NETDRIVE;
        case DK_RAM:    return BI_RAMDISK;
        }
        return BI_HARDDISK;
    case IK_DIR:
        return BI_FOLDER;
    case IK_PROG:
        return BI_PROGRAM;
    case IK_GROUP:
        return it->ref == -1 ? BI_RECENT : BI_GROUP;
    }
    if (it->attr & FA_DIR)
        return BI_FOLDER;
    t = FType(it->name);
    return t ? t->bi : BI_DOC;
}

int ItemGlyph(const ITEM *it)
{
    static const int map[BI_COUNT] = {
        G_FOLDER, G_FLOPPY, G_HARDDISK, G_CDROM, G_NETDRIVE, G_COMPUTER,
        G_GROUP, G_RECENT, G_STAR, G_DOC, G_PROGRAM, G_DOSPROG, G_TEXT,
        G_IMAGE, G_WASTE, G_WASTE, G_SEARCH, G_SYSFILE, G_RAMDISK,
        G_ARCHIVE, G_MUSIC, G_PROGRAM
    };

    return map[ItemBuiltin(it)];
}

/* The program icon of an item, if it has one: programs, executables
 * and documents with an association.  *doc is set for documents (the
 * icon is drawn on a page). */
HICON ItemIcon(const ITEM *it, BOOL *doc)
{
    char path[MAXP];
    const FTYPE *t;

    *doc = FALSE;
    if (it->kind == IK_PROG) {
        GROUP *gr = GroupPtr(cur.kind == LOC_RECENT ? -1 : cur.group);
        PROG *p;
        HICON h;

        if (!gr || it->ref < 0 || it->ref >= gr->n)
            return NULL;
        p = &gr->items[it->ref];
        if (p->iconPath && p->iconPath[0])
            return FileIcon(p->iconPath, p->iconIdx);
        ProgDisplayCmd(p, path, MAXP);
        if (IsProgramExt(PathExt(path))) {
            /* a bare file name: let the shell find it on the path */
            if (!strchr(path, '\\')) {
                char full[MAXP];

                if ((UINT)FindExecutable(path, p->dir ? p->dir : "", full) > 32)
                    lstrcpyn(path, full, MAXP);
            }
            return FileIcon(path, p->iconIdx);
        }
        if (path[0]) {
            h = AssocIcon(path);
            *doc = h != NULL;
            return h;
        }
        return NULL;
    }
    if (it->kind != IK_FILE)
        return NULL;
    t = FType(it->name);
    ViewItemPath(it, path);
    if (t && t->bi == BI_SYSFILE)
        return NULL;
    if (IsProgramExt(PathExt(it->name)))
        return FileIcon(path, 0);
    if (t && (t->bi == BI_DOSPROG || t->bi == BI_GROUP))
        return NULL;
    *doc = TRUE;
    return AssocIcon(path);
}

/* Draws a 32x32 icon at half size on a known background colour. */
void DrawIconSmall(HDC hdc, int x, int y, HICON h, COLORREF bg)
{
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, 32, 32), old;
    RECT r;

    if (!mem || !bmp) {
        if (mem)
            DeleteDC(mem);
        return;
    }
    old = SelectObject(mem, bmp);
    SetRect(&r, 0, 0, 32, 32);
    FillR(mem, &r, bg);
    DrawIcon(mem, 0, 0, h);
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchBlt(hdc, x, y, 16, 16, mem, 0, 0, 32, 32, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* Large (32x32) icon of an item. */
void DrawItemIcon(HDC hdc, int x, int y, const ITEM *it)
{
    BOOL doc;
    HICON h = ItemIcon(it, &doc);

    if (h && !doc) {
        DrawIcon(hdc, x, y, h);
        return;
    }
    if (h) {
        DrawIcon(hdc, x, y, BuiltinIcon(BI_DOC));
        DrawIconSmall(hdc, x + 9, y + 10, h, RGB(255, 255, 255));
        return;
    }
    if (it->kind == IK_WASTE && (it->attr & FA_DIR))
        DrawIcon(hdc, x, y, BuiltinIcon(BI_FOLDER));
    else
        DrawIcon(hdc, x, y, BuiltinIcon(ItemBuiltin(it)));
}

/* Small (16x16) icon of an item. */
void DrawItemSmall(HDC hdc, int x, int y, const ITEM *it, COLORREF bg)
{
    BOOL doc;
    HICON h = NULL;

    if (it->kind == IK_PROG || (it->kind == IK_FILE && IsProgramExt(PathExt(it->name))))
        h = ItemIcon(it, &doc);
    if (h)
        DrawIconSmall(hdc, x, y, h, bg);
    else
        DrawGlyph(hdc, x, y, ItemGlyph(it));
}
