/* ShellIV - program groups, recent programs, favorites and launching. */
#include "shelliv.h"

GROUP groups[MAXGROUPS];
int nGroups;
GROUP recent;
char *favs[MAXFAVS];
int nFavs;

/* ---- programs ----------------------------------------------------------- */

void ProgFree(PROG *p)
{
    StrFree(&p->name);
    StrFree(&p->cmd);
    StrFree(&p->dir);
    StrFree(&p->iconPath);
}

void ProgSet(PROG *p, const char *name, const char *cmd, const char *dir,
             const char *iconPath, int iconIdx)
{
    StrSet(&p->name, name);
    StrSet(&p->cmd, cmd);
    StrSet(&p->dir, dir);
    StrSet(&p->iconPath, iconPath);
    p->iconIdx = iconIdx;
}

static void ProgCopy(PROG *dst, const PROG *src)
{
    memset(dst, 0, sizeof(*dst));
    ProgSet(dst, src->name, src->cmd, src->dir, src->iconPath, src->iconIdx);
    dst->hotkey = src->hotkey;
}

/* The program file of a command line, without arguments or quotes. */
void ProgDisplayCmd(const PROG *p, char *file, int size)
{
    const char *s = p->cmd ? p->cmd : "";
    int n = 0;

    while (*s == ' ')
        s++;
    if (*s == '"') {
        for (s++; *s && *s != '"' && n < size - 1; s++)
            file[n++] = *s;
    } else {
        for (; *s && *s != ' ' && n < size - 1; s++)
            file[n++] = *s;
    }
    file[n] = '\0';
}

/* "C:\WINDOWS\NOTEPAD.EXE" -> "Notepad" */
static void NiceName(char *out, const char *path)
{
    const char *n = PathName(path);
    char *dot;
    BOOL upper = TRUE, allUpper = TRUE;
    int i;

    lstrcpyn(out, n, 64);
    dot = strrchr(out, '.');
    if (dot && dot != out)
        *dot = '\0';
    for (i = 0; out[i]; i++)
        if (out[i] >= 'a' && out[i] <= 'z')
            allUpper = FALSE;
    if (!allUpper)
        return;
    for (i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z' && !upper)
            out[i] = (char)(out[i] - 'A' + 'a');
        upper = out[i] == ' ' || out[i] == '_' || out[i] == '-';
        if (out[i] == '_')
            out[i] = ' ';
    }
}

void ProgFromFile(PROG *p, const char *path)
{
    char name[80], dir[MAXP], cmd[MAXP + 2];

    memset(p, 0, sizeof(*p));
    NiceName(name, path);
    lstrcpyn(dir, path, MAXP);
    PathParent(dir);
    if (strchr(path, ' ') && strlen(path) < MAXP)
        sprintf(cmd, "\"%s\"", path);
    else
        strcpy(cmd, path);
    ProgSet(p, name, cmd, dir, "", 0);
}

/* ---- groups -------------------------------------------------------------- */

GROUP *GroupPtr(int g)
{
    if (g == -1)
        return &recent;
    if (g < 0 || g >= nGroups)
        return NULL;
    return &groups[g];
}

int GroupAdd(const char *name)
{
    GROUP *gr;

    if (nGroups >= MAXGROUPS)
        return -1;
    gr = &groups[nGroups];
    memset(gr, 0, sizeof(*gr));
    gr->name = StrDup(name);
    return nGroups++;
}

static void GroupClear(GROUP *gr)
{
    int i;

    for (i = 0; i < gr->n; i++)
        ProgFree(&gr->items[i]);
    if (gr->items)
        free(gr->items);
    gr->items = NULL;
    gr->n = gr->cap = 0;
}

void GroupDelete(int g)
{
    if (g < 0 || g >= nGroups)
        return;
    GroupClear(&groups[g]);
    StrFree(&groups[g].name);
    memmove(&groups[g], &groups[g + 1], (nGroups - g - 1) * sizeof(GROUP));
    nGroups--;
}

void GroupRename(int g, const char *name)
{
    if (g >= 0 && g < nGroups)
        StrSet(&groups[g].name, name);
}

int ProgAdd(int g, const PROG *p, int pos)
{
    GROUP *gr = GroupPtr(g);

    if (!gr)
        return -1;
    if (gr->n == gr->cap) {
        int cap = gr->cap ? gr->cap * 2 : 8;
        PROG *n = (PROG *)realloc(gr->items, cap * sizeof(PROG));

        if (!n)
            return -1;
        gr->items = n;
        gr->cap = cap;
    }
    if (pos < 0 || pos > gr->n)
        pos = gr->n;
    memmove(&gr->items[pos + 1], &gr->items[pos], (gr->n - pos) * sizeof(PROG));
    ProgCopy(&gr->items[pos], p);
    gr->n++;
    return pos;
}

void ProgDelete(int g, int i)
{
    GROUP *gr = GroupPtr(g);

    if (!gr || i < 0 || i >= gr->n)
        return;
    ProgFree(&gr->items[i]);
    memmove(&gr->items[i], &gr->items[i + 1], (gr->n - i - 1) * sizeof(PROG));
    gr->n--;
}

/* Moves item i of group g to position pos of group g2 (pos -1 = end). */
void ProgMove(int g, int i, int g2, int pos)
{
    GROUP *a = GroupPtr(g), *b = GroupPtr(g2);
    PROG tmp;

    if (!a || !b || i < 0 || i >= a->n)
        return;
    if (a == b) {
        if (pos < 0 || pos > a->n)
            pos = a->n;
        if (pos > i)
            pos--;
        if (pos == i)
            return;
        tmp = a->items[i];
        memmove(&a->items[i], &a->items[i + 1], (a->n - i - 1) * sizeof(PROG));
        memmove(&a->items[pos + 1], &a->items[pos], (a->n - 1 - pos) * sizeof(PROG));
        a->items[pos] = tmp;
        return;
    }
    if (ProgAdd(g2, &a->items[i], pos) >= 0)
        ProgDelete(g, i);
}

/* ---- persistence ----------------------------------------------------------- */

static void Field(const char **s, char *out, int size)
{
    int n = 0;

    while (**s && **s != '|') {
        if (n < size - 1)
            out[n++] = **s;
        (*s)++;
    }
    out[n] = '\0';
    if (**s == '|')
        (*s)++;
}

static void LoadProgs(const char *sec, int g)
{
    char key[16], line[600], name[80], cmd[MAXP], dir[MAXP], icon[MAXP], idx[8], hot[8];
    const char *s;
    int i, n = IniInt(sec, "Count", 0);
    PROG p;

    for (i = 1; i <= n; i++) {
        sprintf(key, "P%d", i);
        IniGet(sec, key, "", line, sizeof(line));
        if (!line[0])
            continue;
        s = line;
        Field(&s, name, sizeof(name));
        Field(&s, cmd, sizeof(cmd));
        Field(&s, dir, sizeof(dir));
        Field(&s, icon, sizeof(icon));
        Field(&s, idx, sizeof(idx));
        Field(&s, hot, sizeof(hot));
        memset(&p, 0, sizeof(p));
        ProgSet(&p, name, cmd, dir, icon, atoi(idx));
        p.hotkey = (WORD)atoi(hot);
        ProgAdd(g, &p, -1);
        ProgFree(&p);
    }
}

static void SaveProgs(const char *sec, GROUP *gr)
{
    char key[16], *line = (char *)malloc(1200);
    int i;

    if (!line)
        return;
    IniPut(sec, NULL, NULL);
    IniPutInt(sec, "Count", gr->n);
    for (i = 0; i < gr->n; i++) {
        PROG *p = &gr->items[i];

        sprintf(key, "P%d", i + 1);
        sprintf(line, "%s|%s|%s|%s|%d|%u", p->name ? p->name : "", p->cmd ? p->cmd : "",
                p->dir ? p->dir : "", p->iconPath ? p->iconPath : "", p->iconIdx, p->hotkey);
        IniPut(sec, key, line);
    }
    free(line);
}

static BOOL FindProgram(const char *file, char *out)
{
    char dir[MAXP];

    GetWindowsDirectory(dir, MAXP);
    PathJoin(out, dir, file);
    if (Exists(out))
        return TRUE;
    GetSystemDirectory(dir, MAXP);
    PathJoin(out, dir, file);
    if (Exists(out))
        return TRUE;
    GetModuleFileName(hInst, dir, MAXP);
    PathParent(dir);
    PathJoin(out, dir, file);
    return Exists(out);
}

static void AddDefaults(int g, const char *const *list)
{
    char path[MAXP];
    PROG p;

    for (; *list; list += 2) {
        if (!FindProgram(list[1], path))
            continue;
        ProgFromFile(&p, path);
        StrSet(&p.name, list[0]);
        ProgAdd(g, &p, -1);
        ProgFree(&p);
    }
}

static const char *const defMain[] = {
    "Control Panel", "CONTROL.EXE", "Print Manager", "PRINTMAN.EXE",
    "Clipboard Viewer", "CLIPBRD.EXE", "MS-DOS Prompt", "DOSPRMPT.PIF",
    "Windows Setup", "SETUP.EXE", "PIF Editor", "PIFEDIT.EXE",
    "Registry Editor", "REGEDIT.EXE", "File Manager", "WINFILE.EXE",
    "Program Manager", "PROGMAN.EXE", "Read Me", "README.WRI", NULL
};
static const char *const defAcc[] = {
    "Write", "WRITE.EXE", "Wordpad", "WORDPAD.EXE", "Paintbrush", "PBRUSH.EXE",
    "Notepad", "NOTEPAD.EXE", "Terminal", "TERMINAL.EXE", "Recorder", "RECORDER.EXE",
    "Cardfile", "CARDFILE.EXE", "Calendar", "CALENDAR.EXE", "Calculator", "CALC.EXE",
    "Clock", "CLOCK.EXE", "Character Map", "CHARMAP.EXE", "Media Player", "MPLAYER.EXE",
    "Sound Recorder", "SOUNDREC.EXE", "Object Packager", "PACKAGER.EXE",
    "Help", "WINHELP.EXE", NULL
};
static const char *const defGames[] = {
    "Solitaire", "SOL.EXE", "Minesweeper", "WINMINE.EXE", "Hearts", "MSHEARTS.EXE",
    "Reversi", "REVERSI.EXE", "FreeCell", "FREECELL.EXE", NULL
};
static const char *const defRetro[] = {
    "Picasso", "PICASSO.EXE", "Arrow", "ARROW16.EXE", "Arrow 32", "ARROW32.EXE",
    "Hello", "HELLOWIN.EXE", NULL
};

static void Defaults(void)
{
    int g;

    if ((g = GroupAdd("Main")) >= 0)
        AddDefaults(g, defMain);
    if ((g = GroupAdd("Accessories")) >= 0)
        AddDefaults(g, defAcc);
    if ((g = GroupAdd("Games")) >= 0)
        AddDefaults(g, defGames);
    if ((g = GroupAdd("Retrolities")) >= 0)
        AddDefaults(g, defRetro);
    for (g = nGroups - 1; g >= 0; g--)
        if (groups[g].n == 0)
            GroupDelete(g);
    GroupAdd("StartUp");
}

void GroupsLoad(void)
{
    char key[16], name[80], sec[16], path[MAXP];
    int i, n = IniInt("Groups", "Count", -1);

    if (n < 0) {
        if (ImportProgman(TRUE) == 0)
            Defaults();
        else if (GroupPtr(0) && !GroupPtr(1))
            Defaults();
        GroupsSave();
    } else {
        for (i = 1; i <= n; i++) {
            int g;

            sprintf(key, "G%d", i);
            IniGet("Groups", key, "", name, sizeof(name));
            if (!name[0] || (g = GroupAdd(name)) < 0)
                continue;
            sprintf(sec, "Group%d", i);
            LoadProgs(sec, g);
        }
    }
    LoadProgs("Recent", -1);
    n = IniInt("Favorites", "Count", 0);
    for (i = 1; i <= n && nFavs < MAXFAVS; i++) {
        sprintf(key, "F%d", i);
        IniGet("Favorites", key, "", path, sizeof(path));
        if (path[0])
            favs[nFavs++] = StrDup(path);
    }
}

void GroupsSave(void)
{
    char key[16], sec[16];
    int i, old = IniInt("Groups", "Count", 0);

    IniPut("Groups", NULL, NULL);
    IniPutInt("Groups", "Count", nGroups);
    for (i = 0; i < nGroups; i++) {
        sprintf(key, "G%d", i + 1);
        IniPut("Groups", key, groups[i].name);
        sprintf(sec, "Group%d", i + 1);
        SaveProgs(sec, &groups[i]);
    }
    for (i = nGroups + 1; i <= old; i++) {
        sprintf(sec, "Group%d", i);
        IniPut(sec, NULL, NULL);
    }
    SaveProgs("Recent", &recent);
    HotkeysRegister();
    IniPut("Favorites", NULL, NULL);
    IniPutInt("Favorites", "Count", nFavs);
    for (i = 0; i < nFavs; i++) {
        sprintf(key, "F%d", i + 1);
        IniPut("Favorites", key, favs[i]);
    }
}

void GroupsDone(void)
{
    int i;

    for (i = 0; i < nGroups; i++) {
        GroupClear(&groups[i]);
        StrFree(&groups[i].name);
    }
    nGroups = 0;
    GroupClear(&recent);
    for (i = 0; i < nFavs; i++)
        StrFree(&favs[i]);
    nFavs = 0;
}

/* ---- favorites ----------------------------------------------------------- */

void FavAdd(const char *path)
{
    int i;

    for (i = 0; i < nFavs; i++)
        if (lstrcmpi(favs[i], path) == 0)
            return;
    if (nFavs < MAXFAVS)
        favs[nFavs++] = StrDup(path);
}

void FavRemove(int i)
{
    if (i < 0 || i >= nFavs)
        return;
    StrFree(&favs[i]);
    memmove(&favs[i], &favs[i + 1], (nFavs - i - 1) * sizeof(char *));
    nFavs--;
}

/* ---- Program Manager group import ----------------------------------------- */

#define W(b, o) ((WORD)((BYTE)(b)[o] | ((WORD)(BYTE)(b)[(o) + 1] << 8)))

static void GrpString(const char *buf, WORD size, WORD off, char *out, int max)
{
    int n = 0;

    while (off < size && buf[off] && n < max - 1)
        out[n++] = buf[off++];
    out[n] = '\0';
}

static BOOL ImportGrp(const char *file)
{
    HFILE f;
    char *buf, name[80], cmd[MAXP], icon[MAXP], dir[MAXP];
    WORD size, cItems, ext, i, ptr, hot;
    int g;
    PROG p;

    f = _lopen(file, OF_READ);
    if (f == HFILE_ERROR)
        return FALSE;
    buf = (char *)malloc(65000U);
    if (!buf) {
        _lclose(f);
        return FALSE;
    }
    size = (WORD)_lread(f, buf, 65000U);
    _lclose(f);
    if (size == (WORD)HFILE_ERROR || size < 36 || memcmp(buf, "PMCC", 4) != 0) {
        free(buf);
        return FALSE;
    }
    ext = W(buf, 6);
    GrpString(buf, size, W(buf, 22), name, sizeof(name));
    cItems = W(buf, 32);
    g = GroupAdd(name[0] ? name : PathName(file));
    if (g < 0) {
        free(buf);
        return FALSE;
    }
    for (i = 0; i < cItems && 34 + 2 * i + 1 < size; i++) {
        ptr = W(buf, 34 + 2 * i);
        if (!ptr || ptr + 24 > size)
            continue;
        GrpString(buf, size, W(buf, ptr + 18), name, sizeof(name));
        GrpString(buf, size, W(buf, ptr + 20), cmd, sizeof(cmd));
        GrpString(buf, size, W(buf, ptr + 22), icon, sizeof(icon));
        dir[0] = '\0';
        hot = 0;
        if (ext && ext < size) {
            WORD t = ext;

            while (t + 6 <= size) {
                WORD type = W(buf, t), num = W(buf, t + 2), cb = W(buf, t + 4);

                if (num == i && type == 0x8101)
                    GrpString(buf, size, (WORD)(t + 6), dir, sizeof(dir));
                if (num == i && type == 0x8102 && t + 8 <= size)
                    hot = W(buf, t + 6);
                if (!cb || type == 0xFFFF)
                    break;
                t += cb;
            }
        }
        memset(&p, 0, sizeof(p));
        ProgSet(&p, name, cmd, dir, icon, W(buf, ptr + 4));
        p.hotkey = hot;
        ProgAdd(g, &p, -1);
        ProgFree(&p);
    }
    free(buf);
    return TRUE;
}

/* Imports the groups listed in PROGMAN.INI; returns how many were added. */
int ImportProgman(BOOL quiet)
{
    char *keys = (char *)malloc(2048), *k, file[MAXP], win[MAXP], path[MAXP];
    int count = 0, before = nGroups, n;

    if (!keys)
        return 0;
    memset(keys, 0, 2048);
    n = GetPrivateProfileString("Groups", NULL, "", keys, 2046, "PROGMAN.INI");
    if (n <= 0 || n >= 2046)
        keys[0] = '\0';
    keys[n > 0 && n < 2046 ? n : 0] = '\0';
    GetWindowsDirectory(win, MAXP);
    for (k = keys; *k && k < keys + n; k += strlen(k) + 1) {
        GetPrivateProfileString("Groups", k, "", file, MAXP, "PROGMAN.INI");
        if (!file[0])
            continue;
        if (!strchr(file, '\\'))
            PathJoin(path, win, file);
        else
            strcpy(path, file);
        if (ImportGrp(path))
            count++;
    }
    free(keys);
    if (!quiet) {
        if (count)
            Msg(MB_ICONINFORMATION, "Imported %d program group%s from Program Manager.",
                count, count == 1 ? "" : "s");
        else
            Msg(MB_ICONINFORMATION, "No Program Manager groups were found in PROGMAN.INI.");
    }
    (void)before;
    return count;
}

/* ---- launching ----------------------------------------------------------- */

BOOL Launch(const char *cmd, const char *dir, int show)
{
    char file[MAXP];
    const char *args, *s = cmd;
    int n = 0;
    UINT r;

    while (*s == ' ')
        s++;
    if (*s == '"') {
        for (s++; *s && *s != '"' && n < MAXP - 1; s++)
            file[n++] = *s;
        if (*s == '"')
            s++;
    } else {
        for (; *s && *s != ' ' && n < MAXP - 1; s++)
            file[n++] = *s;
    }
    file[n] = '\0';
    args = s;
    while (*args == ' ')
        args++;
    if (!file[0])
        return FALSE;
    r = (UINT)ShellExecute(hFrame, NULL, file, *args ? args : NULL,
                           dir && *dir ? dir : NULL, show);
    if (r > 32) {
        if (hFrame) {
            SetTimer(hFrame, 4, 700, NULL);     /* show the new task soon */
            SetTimer(hFrame, 5, 2500, NULL);
        }
        if (minimizeOnUse && hFrame)
            ShowWindow(hFrame, SW_MINIMIZE);
        return TRUE;
    }
    switch (r) {
    case 0: case 8:
        Msg(MB_ICONSTOP, "There is not enough memory to start %s.", file);
        break;
    case 2: case 3:
        Msg(MB_ICONSTOP, "Cannot find \"%s\".\n\nCheck the path and file name.", file);
        break;
    case 31:
        Msg(MB_ICONEXCLAMATION, "No application is associated with \"%s\".\n\n"
            "Associate one in the [Extensions] section of WIN.INI.", PathName(file));
        break;
    default:
        Msg(MB_ICONSTOP, "Cannot run \"%s\" (error %u).", file, r);
    }
    return FALSE;
}

void RecentAdd(const PROG *p)
{
    int i;

    for (i = 0; i < recent.n; i++)
        if (lstrcmpi(recent.items[i].cmd, p->cmd) == 0) {
            ProgDelete(-1, i);
            break;
        }
    ProgAdd(-1, p, 0);
    while (recent.n > MAXRECENT)
        ProgDelete(-1, recent.n - 1);
}

void LaunchProg(const PROG *p)
{
    PROG copy;

    ProgCopy(&copy, p);                 /* p may live in the recent list */
    if (Launch(copy.cmd, copy.dir, SW_SHOWNORMAL))
        RecentAdd(&copy);
    ProgFree(&copy);
}

/* ---- shortcut keys ------------------------------------------------------ */

static void KeyName(int vk, char *out)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        sprintf(out, "%c", vk);
    else if (vk >= VK_F1 && vk <= VK_F12)
        sprintf(out, "F%d", vk - VK_F1 + 1);
    else
        sprintf(out, "#%d", vk);
}

void HotkeyText(WORD hk, char *out)
{
    char k[8];

    out[0] = '\0';
    if (!LOBYTE(hk)) {
        strcpy(out, "None");
        return;
    }
    if (HIBYTE(hk) & HK_CTRL)
        strcat(out, "Ctrl+");
    if (HIBYTE(hk) & HK_ALT)
        strcat(out, "Alt+");
    if (HIBYTE(hk) & HK_SHIFT)
        strcat(out, "Shift+");
    KeyName(LOBYTE(hk), k);
    strcat(out, k);
}

/* "Ctrl+Alt+N", "Ctrl+Shift+F5", or just "N" (Ctrl+Alt is added, as in
 * Program Manager).  Empty or "None" removes the key. */
int HotkeyParse(const char *text, WORD *hk)
{
    char t[40], *s, *part;
    int mods = 0, vk = 0;

    lstrcpyn(t, text, sizeof(t));
    AnsiUpper(t);
    for (s = t; *s == ' '; s++)
        ;
    if (!*s || !strcmp(s, "NONE")) {
        *hk = 0;
        return TRUE;
    }
    for (part = strtok(s, "+ "); part; part = strtok(NULL, "+ ")) {
        if (!strcmp(part, "CTRL") || !strcmp(part, "CONTROL"))
            mods |= HK_CTRL;
        else if (!strcmp(part, "ALT"))
            mods |= HK_ALT;
        else if (!strcmp(part, "SHIFT"))
            mods |= HK_SHIFT;
        else if (strlen(part) == 1 && ((*part >= 'A' && *part <= 'Z') || (*part >= '0' && *part <= '9')))
            vk = *part;
        else if (part[0] == 'F' && atoi(part + 1) >= 1 && atoi(part + 1) <= 12)
            vk = VK_F1 + atoi(part + 1) - 1;
        else
            return FALSE;
    }
    if (!vk)
        return FALSE;
    if (vk < VK_F1 && !(mods & (HK_CTRL | HK_ALT)))
        mods |= HK_CTRL | HK_ALT;
    *hk = (WORD)(vk | (mods << 8));
    return TRUE;
}

BOOL HotkeyRun(WORD hk)
{
    int g, i;

    if (!LOBYTE(hk))
        return FALSE;
    for (g = 0; g < nGroups; g++)
        for (i = 0; i < groups[g].n; i++)
            if (groups[g].items[i].hotkey == hk) {
                LaunchProg(&groups[g].items[i]);
                return TRUE;
            }
    return FALSE;
}

/* On Windows 9x/NT the keys work in every program, as they did with
 * Program Manager; on Windows 3.x they work while ShellIV is active. */
void HotkeysRegister(void)
{
#ifdef RETRO_WIN32
    static int registered;
    int g, i, id = 1;

    if (!hFrame)
        return;
    while (registered > 0)
        UnregisterHotKey(hFrame, registered--);
    for (g = 0; g < nGroups; g++)
        for (i = 0; i < groups[g].n; i++) {
            WORD hk = groups[g].items[i].hotkey;
            UINT mods = 0;

            if (!LOBYTE(hk))
                continue;
            if (HIBYTE(hk) & HK_CTRL)  mods |= MOD_CONTROL;
            if (HIBYTE(hk) & HK_ALT)   mods |= MOD_ALT;
            if (HIBYTE(hk) & HK_SHIFT) mods |= MOD_SHIFT;
            if (RegisterHotKey(hFrame, id, mods, LOBYTE(hk)))
                registered = id++;
        }
#endif
}
