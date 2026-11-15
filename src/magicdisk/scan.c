#include "disk.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    dt_u32 start, size, parent, sector;
    unsigned offset;
    int dir;
    char *path;
} Item;

typedef struct {
    DtVolume *v;
    DtResult *r;
    DtReport report;
    void *ctx;
    DtProgress progress;
    void *progress_ctx;
    int cancelled;
    const char *path;
    unsigned char *used;
    DtFileVisit visit;
    void *visit_ctx;
    Item *queue;
    size_t count, cap, head;
} Scan;

typedef struct {
    char *key;
    dt_u32 location;
    unsigned offset;
} DirectoryName;

typedef struct {
    dt_u32 start, parent, location, lfn_location;
    unsigned position, dots, remaining, name_entries, checksum;
    int root, ended, lfn_active, lfn_invalid;
    const char *path;
    unsigned short name[260];
    DirectoryName *names;
    size_t names_count, names_capacity;
} Directory;

static void issue(Scan *s, const char *code, dt_u32 c, const char *msg)
{
    s->r->errors++;
    if (s->report) {
        char *detail = NULL;
        if (s->path && *s->path) {
            size_t size = strlen(s->path) + strlen(msg) + 3;
            detail = (char *)malloc(size);
            if (detail)
                sprintf(detail, "%s: %s", s->path, msg);
        }
        s->report(s->ctx, code, c, detail ? detail : msg);
        free(detail);
    }
}

static int progress(Scan *s, const char *phase, dt_u32 done, dt_u32 total)
{
    if (s->progress && !s->progress(s->progress_ctx, phase, done, total)) {
        s->cancelled = 1;
        return 0;
    }
    return 1;
}

static int used(Scan *s, dt_u32 c)
{
    return (s->used[c / 8] >> (c % 8)) & 1;
}

static void mark(Scan *s, dt_u32 c)
{
    s->used[c / 8] |= (unsigned char)(1U << (c % 8));
}

static dt_u32 bad(DtVolume *v)
{
    return v->bits == 12 ? 0xff7 : (v->bits == 16 ? 0xfff7 : 0xffffff7);
}

static int enqueue(Scan *s, dt_u32 c, dt_u32 size, int dir, dt_u32 parent, const char *parent_path,
                   const char *name, dt_u32 sector, unsigned offset)
{
    size_t prefix = strlen(parent_path), length = strlen(name);
    char *path;

    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 128;
        Item *p;
        if (cap < s->cap || cap > ((size_t)-1) / sizeof(Item))
            return 0;
        p = (Item *)realloc(s->queue, cap * sizeof(Item));
        if (!p)
            return 0;
        s->queue = p;
        s->cap = cap;
    }
    if (prefix > (size_t)-1 - length - 2)
        return 0;
    path = (char *)malloc(prefix + length + 2);
    if (!path)
        return 0;
    strcpy(path, parent_path);
    if (prefix)
        strcat(path, "/");
    strcat(path, name);
    s->queue[s->count].path = path;
    s->queue[s->count].start = c;
    s->queue[s->count].size = size;
    s->queue[s->count].parent = parent;
    s->queue[s->count].sector = sector;
    s->queue[s->count].offset = offset;
    s->queue[s->count++].dir = dir;
    return 1;
}

static dt_u32 entry_cluster(const DtVolume *v, const unsigned char *entry)
{
    dt_u32 cluster = dt_word(entry + 26);

    if (v->bits == 32)
        cluster |= (dt_u32)dt_word(entry + 20) << 16;
    return cluster;
}

static void orphan_name(Scan *s, Directory *d)
{
    if (d->lfn_active)
        issue(s,
              "LFN_ORPHAN",
              d->lfn_location,
              "Long-name entries have no following short-name entry (disk LBA)");
    d->lfn_active = 0;
}

static void long_entry(Scan *s, Directory *d, const unsigned char *e, dt_u32 location)
{
    static const unsigned offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned ordinal = e[0] & 31, i;

    if ((e[0] & 0xa0) || !ordinal || ordinal > 20) {
        orphan_name(s, d);
        issue(s, "LFN_ORDER", location, "Invalid long-name ordinal (disk LBA)");
        return;
    }
    if (e[0] & 0x40) {
        orphan_name(s, d);
        d->lfn_active = 1;
        d->lfn_invalid = 0;
        d->remaining = d->name_entries = ordinal;
        d->checksum = e[13];
        d->lfn_location = location;
    }
    if (!d->lfn_active || ordinal != d->remaining) {
        issue(s, "LFN_ORDER", location, "Long-name sequence is missing or out of order (disk LBA)");
        d->lfn_active = 0;
        return;
    }
    if (e[11] != 15 || e[12] || dt_word(e + 26)) {
        d->lfn_invalid = 1;
        issue(
            s, "LFN_METADATA", location, "Invalid long-name attribute, type or cluster (disk LBA)");
    }
    if (e[13] != d->checksum) {
        d->lfn_invalid = 1;
        issue(s, "LFN_CHECKSUM", location, "Long-name entries disagree on checksum (disk LBA)");
    }
    for (i = 0; i < 13; i++)
        d->name[(ordinal - 1) * 13 + i] = (unsigned short)dt_word(e + offsets[i]);
    d->remaining--;
}

static int finish_name(Scan *s, Directory *d, const unsigned char *entry)
{
    unsigned i, length, units = d->name_entries * 13, checksum = 0;
    int invalid = d->lfn_invalid;

    if (!d->lfn_active)
        return 0;
    if (d->remaining) {
        issue(s, "LFN_ORDER", d->lfn_location, "Incomplete long-name sequence (disk LBA)");
        d->lfn_active = 0;
        return 0;
    }
    for (i = 0; i < 11; i++)
        checksum = (((checksum >> 1) | ((checksum & 1) << 7)) + entry[i]) & 255;
    if (checksum != d->checksum) {
        invalid = 1;
        issue(s,
              "LFN_CHECKSUM",
              d->lfn_location,
              "Long name does not match its short-name checksum (disk LBA)");
    }
    for (length = 0; length < units && d->name[length]; length++) {
        unsigned ch = d->name[length];

        if (ch >= 0xd800 && ch <= 0xdbff &&
            (length + 1 == units || d->name[length + 1] < 0xdc00 || d->name[length + 1] > 0xdfff))
            invalid = 1;
        if (ch >= 0xdc00 && ch <= 0xdfff &&
            (!length || d->name[length - 1] < 0xd800 || d->name[length - 1] > 0xdbff))
            invalid = 1;

        if (ch == 0xffff || ch < 32 || (ch < 128 && strchr("\"*/:<>?\\|", (int)ch)))
            invalid = 1;
    }
    if ((length == 1 && d->name[0] == '.') ||
        (length == 2 && d->name[0] == '.' && d->name[1] == '.'))
        invalid = 1;
    if (!length || length > 255 || (length + 12) / 13 != d->name_entries)
        invalid = 1;
    for (i = length + 1; i < units; i++)
        if (d->name[i] != 0xffff)
            invalid = 1;
    if (invalid)
        issue(s,
              "LFN_NAME",
              d->lfn_location,
              "Invalid long-name length, characters or terminator padding (disk LBA)");
    d->lfn_active = 0;
    return !invalid;
}

static void file_name(const Directory *d, const unsigned char *entry, int long_name, char *out)
{
    unsigned i, n = 0;

    if (long_name) {
        for (i = 0; i < d->name_entries * 13 && d->name[i]; i++) {
            dt_u32 ch = d->name[i];
            if (ch >= 0xd800 && ch <= 0xdbff) {
                ch = 0x10000UL + ((ch - 0xd800) << 10) + (d->name[++i] - 0xdc00);
                out[n++] = (char)(0xf0 | (ch >> 18));
                out[n++] = (char)(0x80 | ((ch >> 12) & 63));
                out[n++] = (char)(0x80 | ((ch >> 6) & 63));
                out[n++] = (char)(0x80 | (ch & 63));
            } else if (ch < 128) {
                out[n++] = (char)ch;
            } else if (ch < 2048) {
                out[n++] = (char)(0xc0 | (ch >> 6));
                out[n++] = (char)(0x80 | (ch & 63));
            } else {
                out[n++] = (char)(0xe0 | (ch >> 12));
                out[n++] = (char)(0x80 | ((ch >> 6) & 63));
                out[n++] = (char)(0x80 | (ch & 63));
            }
        }
    } else {
        /* Escaped OEM bytes keep aliases unambiguous without assuming the
           volume's original DOS code page, which FAT does not record. */
        unsigned end = 8, extension = 11;
        while (end && entry[end - 1] == ' ')
            end--;
        while (extension > 8 && entry[extension - 1] == ' ')
            extension--;
        for (i = 0; i < 11; i++) {
            unsigned ch = entry[i];
            if ((i < 8 && i >= end) || (i >= 8 && i >= extension))
                continue;
            if (i == 8)
                out[n++] = '.';
            if (!i && ch == 5)
                ch = 0xe5;
            if ((entry[12] & (i < 8 ? 8 : 16)) && ch >= 'A' && ch <= 'Z')
                ch += 'a' - 'A';
            if (ch >= 32 && ch < 127) {
                out[n++] = (char)ch;
            } else {
                sprintf(out + n, "\\x%02X", ch);
                n += 4;
            }
        }
    }
    out[n] = 0;
}

static int valid_date(unsigned value)
{
    static const unsigned char days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned day = value & 31, month = (value >> 5) & 15;
    unsigned year = 1980 + (value >> 9), limit;

    /* Zero denotes an unavailable stamp on legacy media, not a guessed date. */
    if (!value)
        return 1;
    if (!month || month > 12 || !day)
        return 0;
    limit = days[month - 1];
    if (month == 2 && !(year % 4) && (year % 100 || !(year % 400)))
        limit++;
    return day <= limit;
}

static void timestamps(Scan *s, const unsigned char *entry, dt_u32 location, unsigned offset,
                       const char *name)
{
    static const unsigned fields[] = {13, 14, 16, 18, 22, 24};
    static const char *labels[] = {"Creation fraction",
                                   "Creation time",
                                   "Creation date",
                                   "Access date",
                                   "Write time",
                                   "Write date"};
    unsigned i;

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        unsigned field = fields[i];
        unsigned value = field == 13 ? entry[field] : dt_word(entry + field);
        int valid;

        if (field == 13)
            valid = value <= 199;
        else if (field == 14 || field == 22)
            valid = (value & 31) <= 29 && ((value >> 5) & 63) <= 59 && (value >> 11) <= 23;
        else
            valid = valid_date(value);
        if (!valid) {
            char message[960];

            sprintf(message,
                    "%s: invalid %s value 0x%04X at sector byte %u; timestamp unchanged",
                    name,
                    labels[i],
                    value,
                    offset + field);
            issue(s, "DIR_TIMESTAMP", location, message);
        }
    }
}

static int dot_entry(Scan *s, Directory *d, const unsigned char *e, dt_u32 location,
                     unsigned offset)
{
    int dot = !memcmp(e, ".          ", 11);
    int parent = !memcmp(e, "..         ", 11);
    dt_u32 expected;

    if (!dot && !parent)
        return 0;
    timestamps(s, e, location, offset, parent ? ".." : ".");
    orphan_name(s, d);
    expected = parent ? d->parent : d->start;
    if (d->root)
        issue(s, "DIR_DOT_ROOT", location, "Root directory contains a dot entry (disk LBA)");
    else if (d->position != (unsigned)parent)
        issue(s,
              d->position < 2 ? "DIR_DOT_ORDER" : "DIR_DOT_POSITION",
              location,
              "Dot entry is not in its required position (disk LBA)");
    else
        d->dots |= parent ? 2 : 1;
    if (!(e[11] & 16) || (e[11] & 0xc8) || dt_dword(e + 28) || entry_cluster(s->v, e) != expected)
        issue(s,
              "DIR_DOT_METADATA",
              location,
              "Dot entry has an invalid parent/self cluster, size or attribute (disk LBA)");
    return 1;
}

static dt_u32 name_hash(const char *name)
{
    dt_u32 hash = 2166136261UL;

    while (*name) {
        hash ^= (unsigned char)*name++;
        hash *= 16777619UL;
    }
    return hash;
}

static void free_names(Directory *d)
{
    size_t i;

    for (i = 0; i < d->names_capacity; i++)
        free(d->names[i].key);
    free(d->names);
    d->names = NULL;
    d->names_count = d->names_capacity = 0;
}

static int remember_name(Scan *s, Directory *d, const char *name, dt_u32 location, unsigned offset)
{
    char key[768];
    size_t i, slot;

    for (i = 0; name[i]; i++) {
        unsigned ch = (unsigned char)name[i];

        key[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    }
    key[i] = 0;
    if (d->names_count >= d->names_capacity / 2) {
        size_t capacity = d->names_capacity ? d->names_capacity * 2 : 64;
        DirectoryName *names;

        if (capacity < d->names_capacity || capacity > (size_t)-1 / sizeof(*names))
            return 0;
        names = (DirectoryName *)calloc(capacity, sizeof(*names));
        if (!names)
            return 0;
        for (i = 0; i < d->names_capacity; i++) {
            if (!d->names[i].key)
                continue;
            slot = name_hash(d->names[i].key) & (capacity - 1);
            while (names[slot].key)
                slot = (slot + 1) & (capacity - 1);
            names[slot] = d->names[i];
        }
        free(d->names);
        d->names = names;
        d->names_capacity = capacity;
    }
    slot = name_hash(key) & (d->names_capacity - 1);
    while (d->names[slot].key) {
        DirectoryName *first = &d->names[slot];

        if (!strcmp(first->key, key)) {
            if (first->location != location || first->offset != offset) {
                char message[880];

                sprintf(message,
                        "Name conflicts with entry at disk LBA %lu byte %u: %s",
                        (unsigned long)first->location,
                        first->offset,
                        name);
                issue(s, "DIR_DUPLICATE", location, message);
            }
            return 1;
        }
        slot = (slot + 1) & (d->names_capacity - 1);
    }
    d->names[slot].key = (char *)malloc(strlen(key) + 1);
    if (!d->names[slot].key)
        return 0;
    strcpy(d->names[slot].key, key);
    d->names[slot].location = location;
    d->names[slot].offset = offset;
    d->names_count++;
    return 1;
}

static void directory_end(Scan *s, Directory *d)
{
    free_names(d);
    orphan_name(s, d);
    /* A chain with no readable unique prefix has no inspected dot slots.
       Its chain finding describes the problem; missing dots are unproven. */
    if (!d->root && d->location && d->dots != 3)
        issue(s,
              "DIR_DOT_MISSING",
              d->location,
              "Directory is missing its leading dot or dotdot entry (disk LBA)");
}

static int directory_sector(Scan *s, dt_u32 sector, Directory *d)
{
    unsigned char b[512];
    unsigned i;
    dt_u32 location = s->v->start + sector;

    if (!progress(s, "Directories", (dt_u32)s->head, (dt_u32)s->count))
        return 0;
    if (!dt_read(s->v, sector, b))
        return 0;
    if (d->ended)
        return 1;
    if (!d->location)
        d->location = location;
    for (i = 0; i < 512; i += 32) {
        unsigned char *e = b + i;
        dt_u32 c;
        int dir;
        char name[768];

        if (!e[0]) {
            d->ended = 1;
            break;
        }
        if (e[0] == 0xe5) {
            orphan_name(s, d);
        } else if ((e[11] & 63) == 15) {
            long_entry(s, d, e, location);
        } else if (!dot_entry(s, d, e, location, i)) {
            c = entry_cluster(s->v, e);
            dir = (e[11] & 16) != 0;
            if ((e[11] & 0xc0) || ((e[11] & 8) && dir))
                issue(
                    s, "DIR_ATTRIBUTE", location, "Invalid directory entry attributes (disk LBA)");
            if (e[11] & 8) {
                orphan_name(s, d);
                if (!d->root || c || dt_dword(e + 28))
                    issue(s,
                          "DIR_LABEL",
                          location,
                          "Invalid volume label location or metadata (disk LBA)");
            } else {
                char alias[768];
                int long_name = finish_name(s, d, e);

                file_name(d, e, long_name, name);
                timestamps(s, e, location, i, name);
                file_name(d, e, 0, alias);
                if (!remember_name(s, d, alias, location, i) ||
                    (long_name && !remember_name(s, d, name, location, i)))
                    return 0;
                if (e[0] == '.')
                    issue(s, "DIR_NAME", location, "Malformed dot entry or short name (disk LBA)");
                if (dir) {
                    s->r->dirs++;
                    if (dt_dword(e + 28))
                        issue(s,
                              "DIR_SIZE",
                              location,
                              "Subdirectory has a nonzero file size (disk LBA)");
                } else {
                    s->r->files++;
                }
                if (!enqueue(s,
                             c,
                             dt_dword(e + 28),
                             dir,
                             d->root ? 0 : d->start,
                             d->path,
                             name,
                             sector,
                             i))
                    return 0;
            }
        }
        if (d->position < 2)
            d->position++;
    }
    return 1;
}

/* A shared global ownership bit can mean a loop in this chain or a different
   owner. Revisit only this chain's known prefix to distinguish those cases. */
static int own_cluster(Scan *s, dt_u32 start, dt_u32 count, dt_u32 target, int *own)
{
    dt_u32 i, cluster = start;

    *own = 0;
    for (i = 0; i < count; i++) {
        if (!(i & 31) && !progress(s, "Classify chain loop", i, count))
            return 0;
        if (cluster == target) {
            *own = 1;
            return 1;
        }
        if (!dt_fat(s->v, cluster, &cluster))
            return 0;
    }
    return 1;
}

static int walk(Scan *s, Item item)
{
    DtVolume *v = s->v;
    dt_u32 c = item.start, next, n = 0, expected;
    unsigned j;
    Directory directory;
    DtFile file;

    memset(&file, 0, sizeof(file));
    file.sector = item.sector;
    file.offset = item.offset;
    file.start = item.start;
    file.parent = item.parent;
    file.size = item.size;
    file.directory = item.dir;
    file.path = item.path;
    memset(&directory, 0, sizeof(directory));
    directory.path = item.path;
    directory.start = item.start;
    directory.parent = item.parent;
    directory.root = item.dir == 2;
    expected = item.size / (v->spc * 512UL) + (item.size % (v->spc * 512UL) != 0);
    if (!c && !item.dir && !item.size)
        return !s->visit || s->visit(s->visit_ctx, &file);
    for (;;) {
        if ((n & 31) == 0 && !progress(s, "File chains", (dt_u32)s->head, (dt_u32)s->count))
            goto failed;
        if (c < 2 || c >= v->clusters + 2) {
            file.ending = DT_CHAIN_RANGE;
            issue(s, "CHAIN_RANGE", c, "Chain points outside data area");
            break;
        }
        if (used(s, c)) {
            int own;

            if (!own_cluster(s, item.start, file.clusters, c, &own))
                goto failed;
            file.ending = own ? DT_CHAIN_LOOP : DT_CHAIN_SHARED;
            issue(s,
                  "CHAIN_SHARED",
                  c,
                  own ? "Loop within this cluster chain" : "Cluster chain crosses another owner");
            break;
        }
        mark(s, c);
        ++n;
        if (!dt_fat(v, c, &next))
            goto failed;
        if (!next || next == bad(v) || (next >= bad(v) - 7 && next < bad(v))) {
            file.ending = DT_CHAIN_INVALID;
            issue(s, "CHAIN_INVALID", c, "File uses a free, bad or reserved cluster");
            break;
        }
        file.clusters++;
        file.last = c;
        if (item.dir)
            for (j = 0; j < v->spc; j++)
                if (!directory_sector(s, v->data + (c - 2) * v->spc + j, &directory))
                    goto failed;
        if (next >= bad(v) + 1)
            break;
        c = next;
    }
    if (item.dir)
        directory_end(s, &directory);
    if (!item.dir && n != expected)
        issue(s, "FILE_SIZE", item.start, "File size and allocated chain length disagree");
    return !s->visit || s->visit(s->visit_ctx, &file);
failed:
    free_names(&directory);
    return 0;
}

static int check_fsinfo(Scan *s, dt_u32 sector, int check_counts)
{
    unsigned char bytes[512];
    dt_u32 value;
    unsigned i;

    if (!dt_read(s->v, sector, bytes))
        return 0;
    if (dt_dword(bytes) != 0x41615252UL || dt_dword(bytes + 484) != 0x61417272UL ||
        dt_dword(bytes + 508) != 0xaa550000UL) {
        issue(s, "FSINFO_SIGNATURE", sector, "Invalid FSInfo signatures");
        return 1;
    }
    for (i = 4; i < 508; i++) {
        if (i >= 484 && i < 496)
            continue;
        if (bytes[i]) {
            issue(s, "FSINFO_RESERVED", sector, "FSInfo reserved bytes are not zero");
            break;
        }
    }
    /* Backup hints may legitimately lag behind the primary copy. */
    if (!check_counts)
        return 1;
    value = dt_dword(bytes + 488);
    if (value != 0xffffffffUL && value != s->r->free_clusters)
        issue(s, "FSINFO_FREE", sector, "Free cluster count is stale");
    value = dt_dword(bytes + 492);
    if (value != 0xffffffffUL && (value < 2 || value >= s->v->clusters + 2))
        issue(s, "FSINFO_NEXT", sector, "Invalid next-free hint");
    return 1;
}

static void free_queue(Scan *s)
{
    size_t i;

    for (i = 0; i < s->count; i++)
        free(s->queue[i].path);
    free(s->queue);
}

static int scan(DtVolume *v, DtResult *r, unsigned char **owned, DtReport report, void *ctx,
                DtProgress callback, void *progress_ctx, DtFileVisit visit, void *visit_ctx)
{
    Scan s;
    dt_u32 c, x, sec;
    unsigned copy;
    Directory root;
    unsigned char a[512], b[512];
    if (owned)
        *owned = NULL;
    memset(&root, 0, sizeof(root));
    root.root = 1;
    root.path = "";
    memset(&s, 0, sizeof(s));
    memset(r, 0, sizeof(*r));
    s.visit = visit;
    s.visit_ctx = visit_ctx;
    s.progress = callback;
    s.progress_ctx = progress_ctx;
    s.v = v;
    s.r = r;
    s.report = report;
    s.ctx = ctx;
    s.used = (unsigned char *)calloc((size_t)((v->clusters + 9) / 8), 1);
    if (!s.used)
        goto memory;
    if (v->mirrored)
        for (sec = 0; sec < v->fatsz; sec++) {
            if (!progress(&s, "FAT copies", sec, v->fatsz))
                goto io;
            if (!dt_read(v, v->fat + sec, a))
                goto io;
            for (copy = 1; copy < v->copies; copy++) {
                if (!dt_read(v, v->fat + copy * v->fatsz + sec, b))
                    goto io;
                if (memcmp(a, b, 512))
                    issue(&s, "FAT_MIRROR", sec, "FAT copies differ at relative FAT sector");
            }
        }
    if (v->bits == 32) {
        if (!enqueue(&s, v->rootclus, 0, 2, 0, "", "", 0, 0))
            goto memory;
    } else {
        for (sec = 0; sec < v->rootsz; sec++)
            if (!directory_sector(&s, v->root + sec, &root))
                goto io;
        directory_end(&s, &root);
    }
    while (s.head < s.count) {
        Item item = s.queue[s.head++];
        s.path = item.path;
        if (!walk(&s, item))
            goto io;
        free(item.path);
        s.queue[s.head - 1].path = NULL;
        s.path = NULL;
    }
    for (c = 2; c < v->clusters + 2; c++) {
        if ((c & 31) == 0 && !progress(&s, "Allocation", c - 2, v->clusters))
            goto io;
        if (!dt_fat(v, c, &x))
            goto io;
        if (!x)
            r->free_clusters++;
        else if (x == bad(v))
            r->bad_clusters++;
        else {
            if ((x < 2 || x >= v->clusters + 2) && x < bad(v) + 1)
                issue(&s, "FAT_RANGE", c, "Invalid FAT link or reserved cluster value");
            if (!used(&s, c))
                r->lost_clusters++;
        }
    }
    if (r->lost_clusters)
        issue(&s,
              "LOST_CLUSTERS",
              r->lost_clusters,
              "Allocated clusters are not owned by a directory entry");
    if (v->boot_source) {
        if (!dt_read(v, 0, a))
            goto io;
        if (memcmp(a, v->boot, 512))
            issue(&s,
                  "BOOT_PRIMARY",
                  v->start,
                  "Primary boot sector differs from the selected recovery backup (disk LBA)");
    }
    if (v->bits == 32) {
        if (v->fsinfo && v->fsinfo != 0xffff) {
            if (!check_fsinfo(&s, v->fsinfo, 1))
                goto io;
            if (v->backup && v->backup != 0xffff) {
                if (v->fsinfo == v->backup || v->backup + v->fsinfo >= v->fat)
                    issue(&s,
                          "FSINFO_LAYOUT",
                          v->fsinfo,
                          "FSInfo locations overlap boot metadata or exceed reserved sectors");
                else if (!check_fsinfo(&s, v->backup + v->fsinfo, 0))
                    goto io;
            }
        }
        if (v->backup && v->backup != 0xffff) {
            if (!dt_read(v, v->backup, a))
                goto io;
            if (memcmp(a, v->boot, 512))
                issue(&s, "BOOT_BACKUP", v->backup, "Backup boot sector differs");
        }
    }
    r->complete = 1;
    if (owned)
        *owned = s.used;
    else
        free(s.used);
    free_queue(&s);
    return 1;
io:
    free_names(&root);
    issue(&s,
          s.cancelled ? "SCAN_CANCELLED" : "SCAN_INCOMPLETE",
          0,
          s.cancelled ? "Inspection cancelled by user"
                      : "Read failure or directory queue allocation failure");
    free(s.used);
    free_queue(&s);
    return 0;
memory:
    free_names(&root);
    issue(&s, "SCAN_INCOMPLETE", 0, "Insufficient memory");
    free(s.used);
    free_queue(&s);
    return 0;
}

int dt_scan_owned(DtVolume *v, DtResult *r, unsigned char **owned, DtReport report, void *ctx,
                  DtProgress callback, void *progress_ctx)
{
    return scan(v, r, owned, report, ctx, callback, progress_ctx, NULL, NULL);
}

int dt_scan_files(DtVolume *v, DtResult *r, DtFileVisit visit, void *visit_ctx, DtReport report,
                  void *report_ctx, DtProgress progress, void *progress_ctx)
{
    return scan(v, r, NULL, report, report_ctx, progress, progress_ctx, visit, visit_ctx);
}

int dt_scan_ex(DtVolume *v, DtResult *r, DtReport report, void *ctx, DtProgress callback,
               void *progress_ctx)
{
    return dt_scan_owned(v, r, NULL, report, ctx, callback, progress_ctx);
}

int dt_scan(DtVolume *v, DtResult *r, DtReport report, void *ctx)
{
    return dt_scan_ex(v, r, report, ctx, NULL, NULL);
}
