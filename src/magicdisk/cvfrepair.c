/* Journaled repair of DoubleSpace/DriveSpace compressed volume files.
   Only bookkeeping is rewritten: compressed data is copied byte-for-byte,
   never decompressed or re-encoded. The CVF must be unmounted and writable. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    dt_u32 sector;
    unsigned char before[512], after[512];
} CvfPatch;

typedef struct {
    FILE *file;
    dt_u32 sectors;
    unsigned char md[512];
    dt_u32 boot, res, fatsz, root, heap, last, mdstart, mdsectors, first_data, clusters;
    dt_u32 heap_count, bit_sectors, end_mark, bad_mark;
    int fat12, complete;
    unsigned char *table, *table0, *fat, *fat0, *bitfat, *bitfat0, *rootdir, *rootdir0;
    unsigned char *claimed, *owned;
    CvfPatch *extra;
    unsigned extra_count, extra_capacity, next_name;
    DtReport report;
    void *report_ctx;
    char *err;
    DtCvfRepair *out;
} Fix;

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, dt_u32 v)
{
    put16(p, (unsigned)(v & 0xffff));
    put16(p + 2, (unsigned)(v >> 16));
}

static dt_u32 crc32(dt_u32 crc, const unsigned char *data, size_t length)
{
    size_t i;
    unsigned bit;

    crc = ~crc;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
    }
    return ~crc;
}

static int raw_sector(FILE *file, dt_u32 sectors, dt_u32 n, unsigned char *out)
{
    return n < sectors && !fseek(file, (long)n * 512L, SEEK_SET) && fread(out, 1, 512, file) == 512;
}

static void note(Fix *f, const char *code, dt_u32 where, const char *message)
{
    if (f->report)
        f->report(f->report_ctx, code, where, message);
}

static void fixed(Fix *f, const char *code, dt_u32 where, const char *message)
{
    f->out->fixes++;
    note(f, code, where, message);
}

static void unresolved(Fix *f, dt_u32 where, const char *message)
{
    f->out->unresolved++;
    note(f, "CVF_UNRESOLVED", where, message);
}

static dt_u32 fat_get(Fix *f, dt_u32 c)
{
    if (!f->fat12)
        return dt_word(f->fat + c * 2);
    {
        unsigned pair = dt_word(f->fat + c + c / 2);
        return c & 1 ? pair >> 4 : pair & 0xfff;
    }
}

static void fat_set(Fix *f, dt_u32 c, dt_u32 v)
{
    if (!f->fat12) {
        put16(f->fat + c * 2, (unsigned)v);
        return;
    }
    {
        unsigned char *p = f->fat + c + c / 2;
        unsigned pair = dt_word(p);

        pair = c & 1 ? (pair & 0x000f) | ((unsigned)v << 4) : (pair & 0xf000) | (unsigned)v;
        put16(p, pair);
    }
}

static dt_u32 md_get(Fix *f, dt_u32 c)
{
    return dt_dword(f->table + (c + f->first_data) * 4);
}

static void md_set(Fix *f, dt_u32 c, dt_u32 v)
{
    put32(f->table + (c + f->first_data) * 4, v);
}

static dt_u32 md_location(dt_u32 e)
{
    return (e & 0x1fffffUL) + 1;
}

static dt_u32 md_count(dt_u32 e)
{
    return ((e >> 22) & 15) + 1;
}

static int allocated(Fix *f, dt_u32 v)
{
    return v && v != f->bad_mark;
}

static int is_claimed(Fix *f, dt_u32 s)
{
    return (f->claimed[(s - f->heap) / 8] >> ((s - f->heap) % 8)) & 1;
}

static void claim(Fix *f, dt_u32 s)
{
    f->claimed[(s - f->heap) / 8] |= (unsigned char)(1u << ((s - f->heap) % 8));
}

/* A planned heap write; returns the working (after) image of that sector. */
static unsigned char *heap_patch(Fix *f, dt_u32 sector)
{
    unsigned i;
    CvfPatch *p;

    for (i = 0; i < f->extra_count; i++)
        if (f->extra[i].sector == sector)
            return f->extra[i].after;
    if (f->extra_count == f->extra_capacity) {
        unsigned grown = f->extra_capacity ? f->extra_capacity * 2 : 16;
        CvfPatch *next = (CvfPatch *)realloc(f->extra, grown * sizeof(CvfPatch));

        if (!next) {
            fail(f->err, "Insufficient memory for the compressed volume repair");
            return NULL;
        }
        f->extra = next;
        f->extra_capacity = grown;
    }
    p = &f->extra[f->extra_count];
    if (!raw_sector(f->file, f->sectors, sector, p->before)) {
        fail(f->err, "Cannot read a sector-heap sector");
        return NULL;
    }
    memcpy(p->after, p->before, 512);
    p->sector = sector;
    f->extra_count++;
    return p->after;
}

/* Current planned content of any sector (heap patches first). */
static int planned_sector(Fix *f, dt_u32 sector, unsigned char *out)
{
    unsigned i;

    for (i = 0; i < f->extra_count; i++)
        if (f->extra[i].sector == sector) {
            memcpy(out, f->extra[i].after, 512);
            return 1;
        }
    return raw_sector(f->file, f->sectors, sector, out);
}

/* Copy count heap sectors to the lowest free run; return its first sector. */
static int copy_heap(Fix *f, dt_u32 from, dt_u32 count, dt_u32 *to)
{
    dt_u32 s, run = 0, i;
    unsigned char data[512], *dest;

    for (s = f->heap; s < f->last; s++) {
        run = is_claimed(f, s) ? 0 : run + 1;
        if (run == count)
            break;
    }
    if (s >= f->last)
        return fail(f->err, "No free sector-heap space for a copy");
    *to = s + 1 - count;
    for (i = 0; i < count; i++) {
        if (!planned_sector(f, from + i, data))
            return fail(f->err, "Cannot read cross-linked compressed data");
        dest = heap_patch(f, *to + i);
        if (!dest)
            return 0;
        memcpy(dest, data, 512);
        claim(f, *to + i);
    }
    return 1;
}

static int load(Fix *f, const char *path, int writable)
{
    unsigned char b[512];
    dt_u32 i, total, base;
    long length;

    f->file = fopen(path, writable ? "r+b" : "rb");
    if (!f->file)
        return fail(f->err, writable ? "Cannot open the CVF for writing; unmount it and clear "
                                       "its read-only/system/hidden attributes"
                                     : "Cannot open the compressed volume file");
    if (fseek(f->file, 0, SEEK_END) || (length = ftell(f->file)) < 512 * 80)
        return fail(f->err, "File is too small to be a compressed volume");
    f->sectors = (dt_u32)(length / 512);
    if (!raw_sector(f->file, f->sectors, 0, f->md) ||
        (memcmp(f->md + 3, "MSDSP6.0", 8) && memcmp(f->md + 3, "MSDBL6.0", 8)) ||
        dt_word(f->md + 11) != 512 || f->md[13] != 16 || f->md[0x3d] > 1 || !f->md[0x2f])
        return fail(f->err, "Not a DoubleSpace or DriveSpace volume");
    f->last = f->sectors - 1;
    if (!raw_sector(f->file, f->sectors, f->last, b) || memcmp(b, "MDR\0", 4))
        return fail(f->err, "MdStamp2 is missing; the CVF end cannot be trusted for repair");
    base = dt_word(f->md + 0x27);
    for (f->boot = base; f->boot <= base + 1; f->boot++) {
        unsigned char stamp[512];

        if (raw_sector(f->file, f->sectors, f->boot, b) && dt_word(b + 11) == 512 &&
            b[13] == 16 && b[21] == 0xf8 && dt_word(b + 17) == 512 && dt_word(b + 22) &&
            raw_sector(f->file, f->sectors, f->boot + 1, stamp) &&
            !memcmp(stamp, "\xf8\x44\x52\x00", 4))
            break;
    }
    if (f->boot > base + 1)
        return fail(f->err, "Inner boot sector or MdStamp1 is missing; repair refused");
    f->res = dt_word(b + 14);
    f->fatsz = dt_word(b + 22);
    total = dt_word(b + 19) ? dt_word(b + 19) : dt_dword(b + 32);
    f->fat12 = f->md[0x3d] == 1;
    f->root = f->boot + f->res + f->fatsz;
    f->heap = f->root + 34;
    f->mdstart = dt_word(f->md + 0x24) + 1;
    f->mdsectors = f->boot > f->mdstart ? f->boot - f->mdstart : 0;
    f->first_data = dt_word(f->md + 0x2d);
    f->bit_sectors = (dt_u32)f->md[0x2f] * 4;
    if (f->mdstart < 1 + f->bit_sectors || !f->mdsectors || f->heap >= f->last ||
        total < f->res + 2 * f->fatsz + 48)
        return fail(f->err, "CVF regions overlap; repair refused");
    f->clusters = (total - f->res - 2 * f->fatsz - 32) / 16;
    i = f->fat12 ? f->fatsz * 1024UL / 3 : f->fatsz * 256UL;
    if (f->clusters + 2 > i)
        f->clusters = i - 2;
    if (f->first_data + f->clusters + 2 > f->mdsectors * 128)
        return fail(f->err, "MDFAT is too small for the inner FAT; repair refused");
    f->heap_count = f->last - f->heap;
    f->end_mark = f->fat12 ? 0xfff : 0xffff;
    f->bad_mark = f->fat12 ? 0xff7 : 0xfff7;
    f->table = (unsigned char *)malloc((size_t)f->mdsectors * 512);
    f->table0 = (unsigned char *)malloc((size_t)f->mdsectors * 512);
    f->fat = (unsigned char *)calloc((size_t)f->fatsz * 512 + 2, 1);
    f->fat0 = (unsigned char *)calloc((size_t)f->fatsz * 512 + 2, 1);
    f->bitfat = (unsigned char *)malloc((size_t)f->bit_sectors * 512);
    f->bitfat0 = (unsigned char *)malloc((size_t)f->bit_sectors * 512);
    f->rootdir = (unsigned char *)malloc(32 * 512);
    f->rootdir0 = (unsigned char *)malloc(32 * 512);
    f->claimed = (unsigned char *)calloc((size_t)f->heap_count / 8 + 1, 1);
    f->owned = (unsigned char *)calloc((size_t)f->clusters / 8 + 2, 1);
    if (!f->table || !f->table0 || !f->fat || !f->fat0 || !f->bitfat || !f->bitfat0 ||
        !f->rootdir || !f->rootdir0 || !f->claimed || !f->owned)
        return fail(f->err, "Insufficient memory for the compressed volume repair");
    for (i = 0; i < f->mdsectors; i++)
        if (!raw_sector(f->file, f->sectors, f->mdstart + i, f->table + i * 512))
            return fail(f->err, "An MDFAT sector is unreadable");
    for (i = 0; i < f->fatsz; i++)
        if (!raw_sector(f->file, f->sectors, f->boot + f->res + i, f->fat + i * 512))
            return fail(f->err, "An inner FAT sector is unreadable");
    for (i = 0; i < f->bit_sectors; i++)
        if (!raw_sector(f->file, f->sectors, 1 + i, f->bitfat + i * 512))
            return fail(f->err, "A BitFAT sector is unreadable");
    for (i = 0; i < 32; i++)
        if (!raw_sector(f->file, f->sectors, f->root + i, f->rootdir + i * 512))
            return fail(f->err, "A root-directory sector is unreadable");
    memcpy(f->table0, f->table, (size_t)f->mdsectors * 512);
    memcpy(f->fat0, f->fat, (size_t)f->fatsz * 512);
    memcpy(f->bitfat0, f->bitfat, (size_t)f->bit_sectors * 512);
    memcpy(f->rootdir0, f->rootdir, 32 * 512);
    return 1;
}

static void unload(Fix *f)
{
    if (f->file)
        fclose(f->file);
    free(f->table);
    free(f->table0);
    free(f->fat);
    free(f->fat0);
    free(f->bitfat);
    free(f->bitfat0);
    free(f->rootdir);
    free(f->rootdir0);
    free(f->claimed);
    free(f->owned);
    free(f->extra);
}

static int range_ok(Fix *f, dt_u32 e)
{
    return md_location(e) >= f->heap && md_location(e) + md_count(e) <= f->last;
}

/* MDFAT bookkeeping and heap ownership. */
static int repair_mdfat(Fix *f)
{
    unsigned char b[512];
    char message[160];
    dt_u32 c, e, s;
    int pass;

    if (f->fat[0] != 0xf8 || f->fat[1] != 0xff || f->fat[2] != 0xff ||
        (!f->fat12 && f->fat[3] != 0xff)) {
        f->fat[0] = 0xf8;
        f->fat[1] = f->fat[2] = 0xff;
        if (!f->fat12)
            f->fat[3] = 0xff;
        fixed(f, "CVF_FIX_HEADER", f->boot + f->res, "Rebuilt the inner FAT reserved entries");
    }
    for (c = 2; c < f->clusters + 2; c++) {
        e = md_get(f, c);
        if (e & (1UL << 21)) {
            md_set(f, c, e &= ~(1UL << 21));
            sprintf(message, "Cluster %lu: cleared MDFAT reserved bit 21", (unsigned long)c);
            fixed(f, "CVF_FIX_MDFAT", c, message);
        }
        if (!allocated(f, fat_get(f, c)) && (e >> 31)) {
            md_set(f, c, 0);
            sprintf(message, "Free cluster %lu: released %lu stale heap sectors",
                    (unsigned long)c, (unsigned long)md_count(e));
            fixed(f, "CVF_FIX_STALE", c, message);
        } else if (allocated(f, fat_get(f, c)) && e && !(e >> 31)) {
            int tag_ok = range_ok(f, e) && raw_sector(f->file, f->sectors, md_location(e), b) &&
                         ((e >> 30) & 1 || (b[0] == 'M' && b[1] == 'D' && !b[2] && b[3] <= 4));

            if (tag_ok) {
                md_set(f, c, e | (1UL << 31));
                sprintf(message, "Cluster %lu: restored the MDFAT in-use flag", (unsigned long)c);
                fixed(f, "CVF_FIX_MDFAT", c, message);
            } else {
                sprintf(message, "Cluster %lu has no recoverable compressed data",
                        (unsigned long)c);
                unresolved(f, c, message);
            }
        }
    }
    /* First claimant keeps heap sectors; later claimants receive copies. */
    for (pass = 0; pass < 2; pass++) {
        for (c = 2; c < f->clusters + 2; c++) {
            int shared = 0;

            e = md_get(f, c);
            if (!(e >> 31) || !range_ok(f, e))
                continue;
            for (s = md_location(e); s < md_location(e) + md_count(e); s++)
                shared |= is_claimed(f, s);
            if (!pass && !shared)
                for (s = md_location(e); s < md_location(e) + md_count(e); s++)
                    claim(f, s);
            if (pass && shared) {
                dt_u32 to;
                int owner_is_self = 1;

                for (s = md_location(e); s < md_location(e) + md_count(e); s++) {
                    dt_u32 other;

                    for (other = 2; other < c && owner_is_self; other++) {
                        dt_u32 o = md_get(f, other);

                        if ((o >> 31) && range_ok(f, o) && s >= md_location(o) &&
                            s < md_location(o) + md_count(o))
                            owner_is_self = 0;
                    }
                }
                if (owner_is_self)
                    continue;
                if (!copy_heap(f, md_location(e), md_count(e), &to)) {
                    sprintf(message, "Cluster %lu shares heap sectors: %s", (unsigned long)c,
                            f->err);
                    unresolved(f, c, message);
                    f->err[0] = 0;
                    continue;
                }
                md_set(f, c, (e & ~0x1fffffUL) | (to - 1));
                sprintf(message, "Cluster %lu: copied %lu shared heap sectors to %lu",
                        (unsigned long)c, (unsigned long)md_count(e), (unsigned long)to);
                fixed(f, "CVF_FIX_HEAP", c, message);
            }
        }
    }
    for (c = 2; c < f->clusters + 2; c++) {
        e = md_get(f, c);
        if ((e >> 31) && !range_ok(f, e)) {
            sprintf(message, "Cluster %lu points outside the sector heap", (unsigned long)c);
            unresolved(f, c, message);
        }
    }
    return 1;
}

/* Copy one inner cluster (its MDFAT data) into a free FAT cluster. */
static int clone_cluster(Fix *f, dt_u32 source, dt_u32 *target)
{
    dt_u32 c, e = md_get(f, source), to;

    for (c = 2; c < f->clusters + 2; c++)
        if (!fat_get(f, c) && !md_get(f, c) && !((f->owned[c / 8] >> (c % 8)) & 1))
            break;
    if (c >= f->clusters + 2)
        return fail(f->err, "No free inner cluster for a cross-link copy");
    if (e && range_ok(f, e)) {
        if (!copy_heap(f, md_location(e), md_count(e), &to))
            return 0;
        md_set(f, c, (e & ~0x1fffffUL) | (to - 1));
    } else
        md_set(f, c, 0);
    fat_set(f, c, f->end_mark);
    f->owned[c / 8] |= (unsigned char)(1u << (c % 8));
    *target = c;
    return 1;
}

typedef struct {
    dt_u32 cluster;
} Queue;

/* Directory slots live in the root region or a raw heap cluster. */
static unsigned char *slot(Fix *f, dt_u32 dir_cluster, unsigned k)
{
    if (!dir_cluster)
        return f->rootdir + k * 32;
    {
        dt_u32 e = md_get(f, dir_cluster);
        unsigned char *sector = heap_patch(f, md_location(e) + k / 16);

        return sector ? sector + (k % 16) * 32 : NULL;
    }
}

static int repair_tree(Fix *f)
{
    Queue *queue = (Queue *)malloc(sizeof(Queue) * (f->clusters + 2));
    unsigned head = 0, tail = 0;
    char message[160];

    if (!queue)
        return fail(f->err, "Insufficient memory for the compressed volume repair");
    f->complete = 1;
    queue[tail++].cluster = 0;
    while (head < tail) {
        dt_u32 dir = queue[head++].cluster, e = dir ? md_get(f, dir) : 0;
        unsigned k, slots = 512;

        if (dir) {
            if (!e) {
                continue;
            }
            if (!((e >> 30) & 1) || !range_ok(f, e)) {
                f->complete = 0;
                continue;
            }
            slots = md_count(e) * 16;
        }
        for (k = 0; k < slots; k++) {
            unsigned char *entry = slot(f, dir, k);
            dt_u32 start, size, walk, prev = 0, count = 0, need, value;
            int directory;

            if (!entry) {
                free(queue);
                return 0;
            }
            if (!entry[0])
                break;
            if (entry[0] == 0xe5 || (entry[11] & 0x3f) == 0x0f || (entry[11] & 8) ||
                entry[0] == '.')
                continue;
            directory = (entry[11] & 0x10) != 0;
            start = dt_word(entry + 26);
            size = dt_dword(entry + 28);
            need = size / 8192 + (size % 8192 != 0);
            if (directory && start >= 2 && start < f->clusters + 2 &&
                !((f->owned[start / 8] >> (start % 8)) & 1))
                queue[tail++].cluster = start;
            for (walk = start; walk && count <= f->clusters;) {
                if (walk < 2 || walk >= f->clusters + 2 || !allocated(f, fat_get(f, walk))) {
                    if (prev)
                        fat_set(f, prev, f->end_mark);
                    else
                        put16(entry + 26, 0);
                    sprintf(message, "%.11s: ended an invalid chain", (char *)entry);
                    fixed(f, "CVF_FIX_CHAIN", walk, message);
                    break;
                }
                if ((f->owned[walk / 8] >> (walk % 8)) & 1) {
                    dt_u32 copy, from = walk, last_copy = prev, steps = 0;

                    if (directory) {
                        if (prev)
                            fat_set(f, prev, f->end_mark);
                        else
                            put16(entry + 26, 0);
                        sprintf(message, "%.11s: detached a cross-linked directory tail",
                                (char *)entry);
                        unresolved(f, walk, message);
                        break;
                    }
                    while (from && (need ? count < need : 1) && ++steps <= f->clusters) {
                        if (!clone_cluster(f, from, &copy)) {
                            sprintf(message, "%.11s: %s", (char *)entry, f->err);
                            unresolved(f, from, message);
                            f->err[0] = 0;
                            break;
                        }
                        if (last_copy)
                            fat_set(f, last_copy, copy);
                        else
                            put16(entry + 26, (unsigned)copy);
                        last_copy = copy;
                        count++;
                        value = fat_get(f, from);
                        from = value >= f->bad_mark - 7 || !value ? 0 : value;
                        if (from >= f->clusters + 2)
                            from = 0;
                    }
                    if (last_copy)
                        fat_set(f, last_copy, f->end_mark);
                    sprintf(message, "%.11s: copied cross-linked clusters", (char *)entry);
                    fixed(f, "CVF_FIX_CROSSLINK", walk, message);
                    break;
                }
                f->owned[walk / 8] |= (unsigned char)(1u << (walk % 8));
                count++;
                prev = walk;
                value = fat_get(f, walk);
                if (value >= f->bad_mark + 1)
                    break;
                walk = value;
            }
            if (!directory && count != need) {
                put32(entry + 28, count * 8192UL);
                sprintf(message, "%.11s: size %lu -> %lu to match its chain", (char *)entry,
                        (unsigned long)size, (unsigned long)(count * 8192UL));
                fixed(f, "CVF_FIX_SIZE", start, message);
            }
        }
    }
    free(queue);
    return 1;
}

/* Lost chains become root FILEnnnn.CHK entries, or are freed if the root is full. */
static int adopt_lost(Fix *f, dt_u32 stamp)
{
    unsigned char *pointed = (unsigned char *)calloc((size_t)f->clusters / 8 + 2, 1);
    char message[120];
    dt_u32 c, v;
    int pass;

    if (!pointed)
        return fail(f->err, "Insufficient memory for the compressed volume repair");
    for (c = 2; c < f->clusters + 2; c++) {
        v = fat_get(f, c);
        if (allocated(f, v) && !((f->owned[c / 8] >> (c % 8)) & 1) && v >= 2 &&
            v < f->clusters + 2)
            pointed[v / 8] |= (unsigned char)(1u << (v % 8));
    }
    for (pass = 0; pass < 2; pass++) {
        for (c = 2; c < f->clusters + 2; c++) {
            dt_u32 walk = c, count = 0;
            unsigned k;
            unsigned char *entry = NULL;

            if (!allocated(f, fat_get(f, c)) || ((f->owned[c / 8] >> (c % 8)) & 1) ||
                (!pass && ((pointed[c / 8] >> (c % 8)) & 1)))
                continue;
            for (;;) {
                f->owned[walk / 8] |= (unsigned char)(1u << (walk % 8));
                count++;
                v = fat_get(f, walk);
                if (v >= f->bad_mark + 1)
                    break;
                if (v < 2 || v >= f->clusters + 2 || !allocated(f, fat_get(f, v)) ||
                    ((f->owned[v / 8] >> (v % 8)) & 1)) {
                    fat_set(f, walk, f->end_mark);
                    break;
                }
                walk = v;
            }
            for (k = 0; k < 512 && !entry; k++)
                if (!f->rootdir[k * 32] || f->rootdir[k * 32] == 0xe5)
                    entry = f->rootdir + k * 32;
            if (!entry) {
                for (walk = c; walk >= 2 && walk < f->clusters + 2;) {
                    v = fat_get(f, walk);
                    fat_set(f, walk, 0);
                    md_set(f, walk, 0);
                    if (v >= f->bad_mark - 7)
                        break;
                    walk = v;
                }
                fixed(f, "CVF_FIX_LOST", c, "Root full: freed a lost chain");
                continue;
            }
            for (;;) {
                char name[12];
                unsigned j;

                sprintf(name, "FILE%04uCHK", f->next_name++);
                for (j = 0; j < 512; j++)
                    if (!memcmp(f->rootdir + j * 32, name, 11))
                        break;
                if (j == 512) {
                    unsigned at = (unsigned)(entry - f->rootdir) / 32;
                    int was_end = !entry[0];

                    memset(entry, 0, 32);
                    memcpy(entry, name, 11);
                    entry[11] = 0x20;
                    put16(entry + 14, (unsigned)(stamp & 0xffff));
                    put16(entry + 16, (unsigned)(stamp >> 16));
                    put16(entry + 18, (unsigned)(stamp >> 16));
                    put16(entry + 22, (unsigned)(stamp & 0xffff));
                    put16(entry + 24, (unsigned)(stamp >> 16));
                    put16(entry + 26, (unsigned)c);
                    put32(entry + 28, count * 8192UL);
                    if (was_end && at + 1 < 512 && f->rootdir[(at + 1) * 32])
                        memset(f->rootdir + (at + 1) * 32, 0, 32);
                    sprintf(message, "Lost chain at cluster %lu saved as \\%.8s.%.3s",
                            (unsigned long)c, name, name + 8);
                    fixed(f, "CVF_FIX_LOST", c, message);
                    break;
                }
            }
        }
    }
    free(pointed);
    return 1;
}

static void rebuild_bitfat(Fix *f)
{
    dt_u32 i, heap_base = 0, logical_base = 0, base, bytes = f->bit_sectors * 512;

    for (i = 0; i < f->heap_count; i++) {
        int want = is_claimed(f, f->heap + i);
        dt_u32 h = i, l = i + f->heap - 1;

        if (h / 8 < bytes && (((f->bitfat0[(h / 16) * 2 + 1 - (h % 16) / 8] >> (7 - h % 8)) & 1) !=
                              want))
            heap_base++;
        if (l / 8 < bytes && (((f->bitfat0[(l / 16) * 2 + 1 - (l % 16) / 8] >> (7 - l % 8)) & 1) !=
                              want))
            logical_base++;
    }
    base = logical_base < heap_base ? f->heap - 1 : 0;
    for (i = 0; i < f->heap_count; i++) {
        dt_u32 bit = i + base, byte = (bit / 16) * 2 + 1 - (bit % 16) / 8;
        unsigned char mask = (unsigned char)(1u << (7 - bit % 8));

        if (bit / 8 >= bytes)
            break;
        if (is_claimed(f, f->heap + i))
            f->bitfat[byte] |= mask;
        else
            f->bitfat[byte] &= (unsigned char)~mask;
    }
    if (memcmp(f->bitfat, f->bitfat0, bytes))
        fixed(f, "CVF_FIX_BITFAT", 1, "Rebuilt the BitFAT from the MDFAT");
}

static int add_region(CvfPatch **list, unsigned *count, const unsigned char *now,
                      const unsigned char *was, dt_u32 first, dt_u32 sectors)
{
    dt_u32 i;

    for (i = 0; i < sectors; i++) {
        CvfPatch *grown;

        if (!memcmp(now + i * 512, was + i * 512, 512))
            continue;
        grown = (CvfPatch *)realloc(*list, (*count + 1) * sizeof(CvfPatch));
        if (!grown)
            return 0;
        *list = grown;
        grown[*count].sector = first + i;
        memcpy(grown[*count].before, was + i * 512, 512);
        memcpy(grown[*count].after, now + i * 512, 512);
        (*count)++;
    }
    return 1;
}

static int write_verified(FILE *file, dt_u32 sectors, dt_u32 sector, const unsigned char *data)
{
    unsigned char check[512];
#ifdef MAGICDISK_CVF_FAULTS
    /* Test builds only: the Nth write stores half a sector and fails. */
    static unsigned writes;
    const char *target = getenv("MAGICDISK_CVF_FAIL_WRITE");

    if (target && ++writes == (unsigned)atoi(target)) {
        if (!fseek(file, (long)sector * 512L, SEEK_SET))
            fwrite(data, 1, 256, file);
        fflush(file);
        return 0;
    }
#endif

    return !fseek(file, (long)sector * 512L, SEEK_SET) && fwrite(data, 1, 512, file) == 512 &&
           dt_file_commit(file) && raw_sector(file, sectors, sector, check) &&
           !memcmp(check, data, 512);
}

static int save_journal(const char *journal, const Fix *f, const CvfPatch *list, unsigned count,
                        char *err)
{
    FILE *probe = fopen(journal, "rb"), *file;
    unsigned char header[32], record[1032];
    unsigned i;

    if (probe) {
        fclose(probe);
        return fail(err, "Journal file already exists; choose a new name");
    }
    file = fopen(journal, "wb");
    if (!file)
        return fail(err, "Cannot create the journal; use a new file on another disk");
    memset(header, 0, sizeof(header));
    memcpy(header, "MDCVFJ01", 8);
    put32(header + 8, f->sectors);
    put32(header + 12, count);
    put32(header + 16, crc32(0, f->md, 512));
    put32(header + 28, crc32(0, header, 28));
    if (fwrite(header, 1, 32, file) != 32) {
        fclose(file);
        return fail(err, "Cannot write the journal");
    }
    for (i = 0; i < count; i++) {
        put32(record, list[i].sector);
        memcpy(record + 4, list[i].before, 512);
        memcpy(record + 516, list[i].after, 512);
        put32(record + 1028, crc32(0, record, 1028));
        if (fwrite(record, 1, sizeof(record), file) != sizeof(record)) {
            fclose(file);
            return fail(err, "Cannot write the journal");
        }
    }
    if (!dt_file_commit(file)) {
        fclose(file);
        return fail(err, "Cannot commit the journal");
    }
    fclose(file);
    return dt_file_commit_directory(journal) ? 1 : fail(err, "Cannot commit the journal");
}

static int load_journal(const char *journal, const unsigned char *md, dt_u32 sectors,
                        CvfPatch **list, unsigned *count, char *err)
{
    FILE *file = fopen(journal, "rb");
    unsigned char header[32], record[1032];
    unsigned i;

    *list = NULL;
    *count = 0;
    if (!file)
        return fail(err, "Cannot open the journal");
    if (fread(header, 1, 32, file) != 32 || memcmp(header, "MDCVFJ01", 8) ||
        dt_dword(header + 28) != crc32(0, header, 28) || dt_dword(header + 8) != sectors ||
        dt_dword(header + 16) != crc32(0, md, 512) || !dt_dword(header + 12) ||
        dt_dword(header + 12) > 1000000UL) {
        fclose(file);
        return fail(err, "Journal is invalid or belongs to another compressed volume");
    }
    *count = (unsigned)dt_dword(header + 12);
    *list = (CvfPatch *)malloc(*count * sizeof(CvfPatch));
    if (!*list) {
        fclose(file);
        return fail(err, "Insufficient memory for the journal");
    }
    for (i = 0; i < *count; i++) {
        if (fread(record, 1, sizeof(record), file) != sizeof(record) ||
            dt_dword(record + 1028) != crc32(0, record, 1028) || dt_dword(record) >= sectors) {
            fclose(file);
            free(*list);
            *list = NULL;
            return fail(err, "Journal record is damaged");
        }
        (*list)[i].sector = dt_dword(record);
        memcpy((*list)[i].before, record + 4, 512);
        memcpy((*list)[i].after, record + 516, 512);
    }
    i = fgetc(file) == EOF;
    fclose(file);
    if (!i) {
        free(*list);
        *list = NULL;
        return fail(err, "Journal has trailing data");
    }
    return 1;
}

int dt_cvf_repair(const char *path, int apply, const char *journal, dt_u32 stamp,
                  DtCvfRepair *out, DtReport report, void *report_ctx, char *err)
{
    Fix f;
    CvfPatch *list = NULL, *check = NULL;
    unsigned count = 0, checked = 0, i, done = 0;
    int ok = 0;

    memset(&f, 0, sizeof(f));
    memset(out, 0, sizeof(*out));
    f.err = err;
    f.out = out;
    f.report = report;
    f.report_ctx = report_ctx;
    err[0] = 0;
    if (apply && !journal)
        return fail(err, "A new journal file is required to repair a compressed volume");
    if (!dt_dos_stamp_valid(stamp))
        return fail(err, "A valid DOS date and time is required");
    if (!load(&f, path, apply) || !repair_mdfat(&f) || !repair_tree(&f))
        goto done;
    if (f.complete && !adopt_lost(&f, stamp))
        goto done;
    if (!f.complete)
        note(&f, "CVF_PARTIAL", 0, "Compressed directories were not expanded; lost chains kept");
    {
        /* Claims may have changed: recompute from the final MDFAT. */
        dt_u32 c;

        memset(f.claimed, 0, (size_t)f.heap_count / 8 + 1);
        for (c = 2; c < f.clusters + 2; c++) {
            dt_u32 e = md_get(&f, c), s;

            if ((e >> 31) && range_ok(&f, e))
                for (s = md_location(e); s < md_location(e) + md_count(e); s++)
                    claim(&f, s);
        }
    }
    rebuild_bitfat(&f);
    if (!add_region(&list, &count, f.bitfat, f.bitfat0, 1, f.bit_sectors) ||
        !add_region(&list, &count, f.table, f.table0, f.mdstart, f.mdsectors) ||
        !add_region(&list, &count, f.fat, f.fat0, f.boot + f.res, f.fatsz) ||
        !add_region(&list, &count, f.rootdir, f.rootdir0, f.root, 32)) {
        fail(err, "Insufficient memory for the compressed volume repair");
        goto done;
    }
    for (i = 0; i < f.extra_count; i++) {
        CvfPatch *grown;

        if (!memcmp(f.extra[i].before, f.extra[i].after, 512))
            continue;
        grown = (CvfPatch *)realloc(list, (count + 1) * sizeof(CvfPatch));
        if (!grown) {
            fail(err, "Insufficient memory for the compressed volume repair");
            goto done;
        }
        list = grown;
        list[count++] = f.extra[i];
    }
    out->sectors = count;
    if (!apply || !count) {
        ok = 1;
        goto done;
    }
    if (!save_journal(journal, &f, list, count, err) ||
        !load_journal(journal, f.md, f.sectors, &check, &checked, err))
        goto done;
    if (checked != count) {
        fail(err, "Journal verification failed; nothing was written");
        goto done;
    }
    for (i = 0; i < count; i++) {
        if (check[i].sector != list[i].sector || memcmp(check[i].after, list[i].after, 512)) {
            fail(err, "Journal verification failed; nothing was written");
            goto done;
        }
    }
    for (i = 0; i < count; i++) {
        unsigned char now[512];

        int changed_before = !raw_sector(f.file, f.sectors, list[i].sector, now) ||
                             memcmp(now, list[i].before, 512);

        /* A failed write may have torn this sector: include it in rollback. */
        if (!changed_before)
            done = i + 1;
        if (changed_before || !write_verified(f.file, f.sectors, list[i].sector, list[i].after)) {
            int clean = 1;

            while (done) {
                done--;
                if (!write_verified(f.file, f.sectors, list[done].sector, list[done].before))
                    clean = 0;
            }
            fail(err, clean ? "Write failed; original CVF sectors restored and verified"
                            : "Rollback incomplete: keep the journal and stop using this CVF");
            goto done;
        }
    }
    out->applied = 1;
    ok = 1;
done:
    free(check);
    free(list);
    unload(&f);
    return ok;
}

int dt_cvf_restore(const char *path, const char *journal, char *err)
{
    FILE *file = fopen(path, "r+b");
    unsigned char md[512], now[512];
    CvfPatch *list = NULL;
    unsigned count = 0, i;
    dt_u32 sectors;
    long length;
    int ok = 0;

    if (!file)
        return fail(err, "Cannot open the CVF for writing; unmount it and clear its attributes");
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 512) {
        fclose(file);
        return fail(err, "Cannot size the compressed volume file");
    }
    sectors = (dt_u32)(length / 512);
    if (!raw_sector(file, sectors, 0, md) || !load_journal(journal, md, sectors, &list, &count, err))
        goto done;
    for (i = 0; i < count; i++) {
        if (!raw_sector(file, sectors, list[i].sector, now) ||
            (memcmp(now, list[i].before, 512) && memcmp(now, list[i].after, 512))) {
            fail(err, "A journal sector has unrelated changes; nothing was restored");
            goto done;
        }
    }
    for (i = count; i; i--) {
        if (!raw_sector(file, sectors, list[i - 1].sector, now))
            goto io;
        if (!memcmp(now, list[i - 1].before, 512))
            continue;
        if (!write_verified(file, sectors, list[i - 1].sector, list[i - 1].before))
            goto io;
    }
    ok = 1;
    goto done;
io:
    fail(err, "Restore write failed; keep the journal and retry");
done:
    free(list);
    fclose(file);
    return ok;
}
