/* Data-preserving repairs: copy cross-linked clusters and adopt lost chains.
   Unlike truncating chain repair, these keep every readable byte reachable. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static void put16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, dt_u32 value)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

static dt_u32 bad_mark(const DtVolume *v)
{
    return v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7UL;
}

static dt_u32 end_mark(const DtVolume *v)
{
    return v->bits == 12 ? 0xfff : v->bits == 16 ? 0xffff : 0x0fffffffUL;
}

/* True for a link to another data cluster (not free, bad, reserved or end). */
static int data_link(const DtVolume *v, dt_u32 value)
{
    return value >= 2 && value < v->clusters + 2 && value < bad_mark(v) - 7;
}

static int set_fat_all(DtPlan *plan, dt_u32 cluster, dt_u32 value, char *err)
{
    DtVolume *v = plan->volume;
    unsigned copy;

    for (copy = 0; copy < v->copies; copy++) {
        if (!v->mirrored && copy != v->active)
            continue;
        if (!dt_plan_set_fat(plan, cluster, value, copy, err))
            return 0;
    }
    return 1;
}

static void set_start(const DtVolume *v, unsigned char *entry, dt_u32 cluster)
{
    put16(entry + 26, (unsigned)(cluster & 0xffff));
    if (v->bits == 32)
        put16(entry + 20, (unsigned)(cluster >> 16));
}

/* Copying reads use the same three-attempt policy as relocation. */
static int read_retry(DtVolume *v, dt_u32 sector, unsigned char *data)
{
    unsigned attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        if (dt_read(v, sector, data))
            return 1;
        v->cache_sector = 0xffffffffUL;
    }
    return 0;
}

typedef struct {
    DtPlan *plan;
    unsigned char *owned;
    dt_u32 search, allocated, free_clusters;
    DtProgress progress;
    void *progress_ctx;
    char *err;
} Space;

/* Lowest unowned, free and fully readable cluster. Unreadable candidates are
   skipped without being written or marked. */
static int take_free(Space *s, dt_u32 *selected)
{
    DtVolume *v = s->plan->volume;
    unsigned char data[512];
    dt_u32 cluster, value;
    unsigned i;

    for (cluster = s->search; cluster < v->clusters + 2 && cluster < bad_mark(v) - 7; cluster++) {
        if (s->progress && !(cluster & 63) &&
            !s->progress(s->progress_ctx, "Find free space", cluster - 2, v->clusters))
            return fail(s->err, "Repair preparation cancelled; no writes made");
        if (s->owned[cluster / 8] & (1U << (cluster % 8)))
            continue;
        if (!dt_fat(v, cluster, &value))
            return fail(s->err, "Cannot read allocation while finding free space");
        if (value)
            continue;
        for (i = 0; i < v->spc; i++)
            if (!dt_read(v, v->data + (cluster - 2) * v->spc + i, data))
                break;
        if (i < v->spc)
            continue;
        s->owned[cluster / 8] |= (unsigned char)(1U << (cluster % 8));
        s->search = cluster + 1;
        s->allocated++;
        *selected = cluster;
        return 1;
    }
    return fail(s->err, "Not enough readable free clusters are available for this repair");
}

/* Keep FAT32 free counts exact after allocation; leave invalid FSInfo alone. */
static int update_hints(Space *s)
{
    DtVolume *v = s->plan->volume;
    dt_u32 sectors[2];
    unsigned i, count = 0;

    if (v->bits != 32 || !s->allocated)
        return 1;
    if (s->allocated > s->free_clusters)
        return fail(s->err, "Free allocation changed while preparing the repair");
    if (v->fsinfo && v->fsinfo != 0xffff && v->fsinfo < v->fat && v->fsinfo != v->backup) {
        sectors[count++] = v->fsinfo;
        if (v->backup && v->backup != 0xffff && v->backup + v->fsinfo < v->fat)
            sectors[count++] = v->backup + v->fsinfo;
    }
    for (i = 0; i < count; i++) {
        unsigned char data[512];
        DtPatch *patch;

        if (!dt_read(v, sectors[i], data))
            return fail(s->err, "Cannot read FAT32 FSInfo while preparing the repair");
        if (dt_dword(data) != 0x41615252UL || dt_dword(data + 484) != 0x61417272UL ||
            dt_dword(data + 508) != 0xaa550000UL)
            continue;
        patch = dt_plan_sector(s->plan, sectors[i], s->err);
        if (!patch)
            return 0;
        put32(patch->after + 488, s->free_clusters - s->allocated);
        put32(patch->after + 492, 0xffffffffUL);
    }
    return 1;
}

/* Findings that do not change who owns which cluster. */
static int ownership_neutral(const char *code)
{
    static const char *const neutral[] = {
        "FSINFO_SIGNATURE", "FSINFO_FREE", "FSINFO_NEXT", "FSINFO_RESERVED", "BOOT_BACKUP",
        "DIR_TIMESTAMP", "DIR_LABEL_NAME", "DIR_LABEL", "DIR_NAME", "DIR_DUPLICATE",
        "DIR_ATTRIBUTE", "DIR_SIZE", "DIR_DOT_ROOT", "DIR_DOT_POSITION", "DIR_DOT_ORDER",
        "DIR_DOT_MISSING", "DIR_DOT_METADATA", "LFN_ORDER", "LFN_CHECKSUM", "LFN_ORPHAN",
        "LFN_NAME", "LFN_METADATA", "FILE_SIZE", "CHAIN_SHARED", "CHAIN_RANGE",
        "CHAIN_INVALID", "LOST_CLUSTERS", "FAT_RANGE", NULL};
    unsigned i;

    for (i = 0; neutral[i]; i++)
        if (!strcmp(code, neutral[i]))
            return 1;
    return 0;
}

typedef struct {
    Space space;
    int unsafe;
    DtReport report;
    void *report_ctx;
    unsigned char *buffer;
} Xlink;

static void xlink_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    (void)where;
    (void)message;
    if (!ownership_neutral(code))
        ((Xlink *)ctx)->unsafe = 1;
}

/* Give a later owner private copies of the shared tail it references. */
static int copy_shared(void *ctx, const DtFile *file)
{
    Xlink *x = (Xlink *)ctx;
    DtPlan *plan = x->space.plan;
    DtVolume *v = plan->volume;
    dt_u32 cluster_bytes = v->spc * 512UL;
    dt_u32 expected = file->size / cluster_bytes + (file->size % cluster_bytes != 0);
    dt_u32 source, previous = file->last, copy, copied = 0, next, steps = 0, size = file->size;
    DtPatch *patch;
    unsigned i;
    char *message;

    if (file->ending != DT_CHAIN_SHARED || file->directory)
        return 1;
    if (file->clusters) {
        if (!dt_fat(v, file->last, &source))
            return fail(x->space.err, "Cannot read a cross-linked chain");
    } else
        source = file->start;
    if (expected <= file->clusters) {
        /* The shared tail lies beyond the recorded length: detach it only. */
        if (file->clusters) {
            if (!set_fat_all(plan, file->last, end_mark(v), x->space.err))
                return 0;
        } else {
            patch = dt_plan_sector(plan, file->sector, x->space.err);
            if (!patch)
                return 0;
            set_start(v, patch->after + file->offset, 0);
        }
    } else {
        while (copied < expected - file->clusters) {
            if (!data_link(v, source) || ++steps > v->clusters)
                break;
            if (!take_free(&x->space, &copy))
                return 0;
            for (i = 0; i < v->spc; i++) {
                if (!read_retry(v, v->data + (source - 2) * v->spc + i, x->buffer))
                    return fail(x->space.err,
                                "A cross-linked cluster is unreadable; salvage it first");
                patch = dt_plan_sector(plan, v->data + (copy - 2) * v->spc + i, x->space.err);
                if (!patch)
                    return 0;
                memcpy(patch->after, x->buffer, 512);
            }
            if (previous) {
                if (!set_fat_all(plan, previous, copy, x->space.err))
                    return 0;
            } else {
                patch = dt_plan_sector(plan, file->sector, x->space.err);
                if (!patch)
                    return 0;
                set_start(v, patch->after + file->offset, copy);
            }
            if (!set_fat_all(plan, copy, end_mark(v), x->space.err))
                return 0;
            previous = copy;
            copied++;
            if (!dt_fat(v, source, &next))
                return fail(x->space.err, "Cannot read a cross-linked chain");
            source = next;
        }
        if (!copied) {
            if (file->clusters) {
                if (!set_fat_all(plan, file->last, end_mark(v), x->space.err))
                    return 0;
            } else {
                patch = dt_plan_sector(plan, file->sector, x->space.err);
                if (!patch)
                    return 0;
                set_start(v, patch->after + file->offset, 0);
            }
        }
        if (file->clusters + copied < expected) {
            size = (file->clusters + copied) * cluster_bytes;
            patch = dt_plan_sector(plan, file->sector, x->space.err);
            if (!patch)
                return 0;
            put32(patch->after + file->offset + 28, size);
        }
    }
    plan->cluster++;
    if (x->report) {
        message = (char *)malloc(strlen(file->path) + 160);
        if (!message)
            return fail(x->space.err, "Insufficient memory for cross-link preview");
        sprintf(message,
                "%s: keep %lu private clusters, copy %lu shared clusters; size %lu -> %lu",
                file->path,
                (unsigned long)file->clusters,
                (unsigned long)copied,
                (unsigned long)file->size,
                (unsigned long)size);
        x->report(x->report_ctx, "XLINK_COPY", v->start + file->sector, message);
        free(message);
    }
    return 1;
}

int dt_crosslink_plan_ex(DtVolume *v, DtPlan *plan, DtReport report, void *report_ctx,
                         DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    Xlink x;
    int ok = 0;

    err[0] = 0;
    memset(&x, 0, sizeof(x));
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_CROSSLINKS;
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_owned(v, &result, &x.space.owned, xlink_issue, &x, progress, ctx) ||
        !result.complete) {
        free(x.space.owned);
        return fail(err, "Cross-link repair requires a complete ownership scan");
    }
    if (x.unsafe) {
        free(x.space.owned);
        return fail(err, "Cross-link repair requires consistent FAT copies and readable directories");
    }
    x.buffer = (unsigned char *)malloc(512);
    if (!x.buffer) {
        fail(err, "Insufficient memory for cross-link repair");
        goto done;
    }
    x.space.plan = plan;
    x.space.search = 2;
    x.space.free_clusters = result.free_clusters;
    x.space.progress = progress;
    x.space.progress_ctx = ctx;
    x.space.err = err;
    x.report = report;
    x.report_ctx = report_ctx;
    if (!dt_scan_files(v, &result, copy_shared, &x, NULL, NULL, progress, ctx) || !result.complete) {
        if (!*err)
            fail(err, "Cross-link repair scan was cancelled or incomplete");
        goto done;
    }
    ok = update_hints(&x.space);
done:
    free(x.buffer);
    free(x.space.owned);
    v->cache_sector = 0xffffffffUL;
    return ok;
}

int dt_crosslink_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    return dt_crosslink_plan_ex(v, plan, NULL, NULL, progress, ctx, err);
}

int dt_dos_stamp_valid(dt_u32 stamp)
{
    unsigned date = (unsigned)(stamp >> 16), time = (unsigned)(stamp & 0xffff);
    unsigned month = (date >> 5) & 15, day = date & 31;

    return month >= 1 && month <= 12 && day >= 1 && (time >> 11) <= 23 &&
           ((time >> 5) & 63) <= 59 && (time & 31) <= 29;
}

/* ---- Lost-chain conversion into root-directory FILEnnnn.CHK files ---- */

typedef struct {
    dt_u32 sector;
    unsigned offset;
} Slot;

typedef struct {
    Space space;
    int unsafe;
    unsigned char *state;
    Slot *ends, *deleted;
    unsigned end_count, end_used, end_capacity, deleted_count, deleted_used, deleted_capacity;
    unsigned char *names;
    unsigned name_count, name_capacity, number;
    dt_u32 root_tail;
    DtReport report;
    void *report_ctx;
} Adopt;

#define LOST 1
#define POINTED 2
#define VISITED 4

static void adopt_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    (void)where;
    (void)message;
    if (strcmp(code, "LOST_CLUSTERS") && strcmp(code, "FAT_RANGE") &&
        strcmp(code, "FSINFO_SIGNATURE") && strcmp(code, "FSINFO_FREE") &&
        strcmp(code, "FSINFO_NEXT") && strcmp(code, "FSINFO_RESERVED") &&
        strcmp(code, "BOOT_BACKUP") && strcmp(code, "DIR_TIMESTAMP") &&
        strcmp(code, "DIR_LABEL_NAME"))
        ((Adopt *)ctx)->unsafe = 1;
}

static int add_slot(Slot **slots, unsigned *count, unsigned *capacity, dt_u32 sector,
                    unsigned offset, char *err)
{
    if (*count == *capacity) {
        unsigned grown = *capacity ? *capacity * 2 : 64;
        Slot *next = (Slot *)realloc(*slots, grown * sizeof(Slot));

        if (!next || grown < *capacity)
            return fail(err, "Insufficient memory for root-directory slots");
        *slots = next;
        *capacity = grown;
    }
    (*slots)[*count].sector = sector;
    (*slots)[*count].offset = offset;
    (*count)++;
    return 1;
}

static int add_name(Adopt *a, const unsigned char *name)
{
    if (a->name_count == a->name_capacity) {
        unsigned grown = a->name_capacity ? a->name_capacity * 2 : 64;
        unsigned char *next = (unsigned char *)realloc(a->names, (size_t)grown * 11);

        if (!next || grown < a->name_capacity)
            return fail(a->space.err, "Insufficient memory for root-directory names");
        a->names = next;
        a->name_capacity = grown;
    }
    memcpy(a->names + (size_t)a->name_count * 11, name, 11);
    a->name_count++;
    return 1;
}

static int root_sector(Adopt *a, dt_u32 sector, int *ended)
{
    DtVolume *v = a->space.plan->volume;
    unsigned char data[512];
    unsigned offset;

    if (!dt_read(v, sector, data))
        return fail(a->space.err, "Cannot read the root directory");
    for (offset = 0; offset < 512; offset += 32) {
        const unsigned char *entry = data + offset;

        if (!*ended && !entry[0])
            *ended = 1;
        if (*ended) {
            if (!add_slot(&a->ends, &a->end_count, &a->end_capacity, sector, offset, a->space.err))
                return 0;
        } else if (entry[0] == 0xe5) {
            if (!add_slot(&a->deleted,
                          &a->deleted_count,
                          &a->deleted_capacity,
                          sector,
                          offset,
                          a->space.err))
                return 0;
        } else if ((entry[11] & 63) != 15 && !add_name(a, entry))
            return 0;
    }
    return 1;
}

static int read_root(Adopt *a)
{
    DtVolume *v = a->space.plan->volume;
    dt_u32 cluster = v->rootclus, next, steps = 0, i;
    int ended = 0;

    if (v->bits != 32) {
        for (i = 0; i < v->rootsz; i++)
            if (!root_sector(a, v->root + i, &ended))
                return 0;
        return 1;
    }
    for (;;) {
        if (!data_link(v, cluster) || ++steps > v->clusters)
            return fail(a->space.err, "The FAT32 root-directory chain is invalid");
        for (i = 0; i < v->spc; i++)
            if (!root_sector(a, v->data + (cluster - 2) * v->spc + i, &ended))
                return 0;
        if (!dt_fat(v, cluster, &next))
            return fail(a->space.err, "Cannot read the root-directory chain");
        a->root_tail = cluster;
        if (next >= bad_mark(v) + 1)
            return 1;
        cluster = next;
    }
}

/* Grow the FAT32 root by one zeroed cluster when no free slot remains. */
static int grow_root(Adopt *a)
{
    DtPlan *plan = a->space.plan;
    DtVolume *v = plan->volume;
    dt_u32 cluster;
    unsigned i, offset;

    if (v->bits != 32)
        return fail(a->space.err,
                    "Root directory is full; export with --recover-lost, then reclaim");
    if (!take_free(&a->space, &cluster))
        return 0;
    for (i = 0; i < v->spc; i++) {
        dt_u32 sector = v->data + (cluster - 2) * v->spc + i;
        DtPatch *patch = dt_plan_sector(plan, sector, a->space.err);

        if (!patch)
            return 0;
        memset(patch->after, 0, 512);
        for (offset = 0; offset < 512; offset += 32)
            if (!add_slot(&a->ends, &a->end_count, &a->end_capacity, sector, offset, a->space.err))
                return 0;
    }
    if (!set_fat_all(plan, a->root_tail, cluster, a->space.err) ||
        !set_fat_all(plan, cluster, end_mark(v), a->space.err))
        return 0;
    a->root_tail = cluster;
    return 1;
}

static int next_name(Adopt *a, unsigned char *name)
{
    char text[24], display[32];
    unsigned i;
    int available;

    for (; a->number <= 9999; a->number++) {
        sprintf(text, "FILE%04u", a->number);
        memcpy(name, text, 8);
        memcpy(name + 8, "CHK", 3);
        for (i = 0; i < a->name_count; i++)
            if (!memcmp(a->names + (size_t)i * 11, name, 11))
                break;
        if (i < a->name_count)
            continue;
        sprintf(display, "%s.CHK", text);
        available = dt_directory_name_available(a->space.plan->volume,
                                                0,
                                                display,
                                                0xffffffffUL,
                                                0,
                                                a->space.progress,
                                                a->space.progress_ctx);
        if (available < 0)
            return fail(a->space.err, "Root namespace validation failed or was cancelled");
        if (!available)
            continue;
        a->number++;
        return add_name(a, name);
    }
    return fail(a->space.err, "No unused FILEnnnn.CHK name remains in the root directory");
}

static int write_entry(Adopt *a, dt_u32 head, dt_u32 clusters)
{
    DtPlan *plan = a->space.plan;
    DtVolume *v = plan->volume;
    dt_u32 stamp = (dt_u32)plan->mark_bad;
    unsigned char name[11];
    Slot slot;
    DtPatch *patch;
    unsigned char *entry;
    int from_end;

    if (!next_name(a, name))
        return 0;
    if (a->end_used == a->end_count && a->deleted_used == a->deleted_count && !grow_root(a))
        return 0;
    /* Prefer never-used slots so deleted entries stay available for undelete. */
    from_end = a->end_used < a->end_count;
    slot = from_end ? a->ends[a->end_used++] : a->deleted[a->deleted_used++];
    patch = dt_plan_sector(plan, slot.sector, a->space.err);
    if (!patch)
        return 0;
    entry = patch->after + slot.offset;
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = 0x20;
    put16(entry + 14, (unsigned)(stamp & 0xffff));
    put16(entry + 16, (unsigned)(stamp >> 16));
    put16(entry + 18, (unsigned)(stamp >> 16));
    put16(entry + 22, (unsigned)(stamp & 0xffff));
    put16(entry + 24, (unsigned)(stamp >> 16));
    set_start(v, entry, head);
    put32(entry + 28, clusters * v->spc * 512UL);
    /* Keep the directory terminated after a consumed end slot. */
    if (from_end && a->end_used < a->end_count) {
        Slot after = a->ends[a->end_used];

        patch = dt_plan_sector(plan, after.sector, a->space.err);
        if (!patch)
            return 0;
        if (patch->after[after.offset])
            memset(patch->after + after.offset, 0, 32);
    }
    plan->cluster++;
    if (a->report) {
        char message[120];

        sprintf(message,
                "Root \\%.8s.%.3s: lost chain at cluster %lu, %lu clusters, %lu bytes",
                name,
                name + 8,
                (unsigned long)head,
                (unsigned long)clusters,
                (unsigned long)(clusters * v->spc * 512UL));
        a->report(a->report_ctx, "LOST_TO_FILE", v->start + slot.sector, message);
    }
    return 1;
}

/* Follow one lost chain from head, terminating it where it leaves the lost
   set, revisits a cluster, or would exceed the FAT file-size limit. */
static int adopt_chain(Adopt *a, dt_u32 head)
{
    DtPlan *plan = a->space.plan;
    DtVolume *v = plan->volume;
    dt_u32 limit = 0xffffffffUL / (v->spc * 512UL), cluster = head, value, count = 0;

    for (;;) {
        a->state[cluster] |= VISITED;
        count++;
        if (!dt_fat(v, cluster, &value))
            return fail(a->space.err, "Cannot read allocation while adopting lost chains");
        if (value >= bad_mark(v) + 1)
            break;
        if (count < limit && data_link(v, value) && (a->state[value] & LOST) &&
            !(a->state[value] & VISITED)) {
            cluster = value;
            continue;
        }
        if (!set_fat_all(plan, cluster, end_mark(v), a->space.err))
            return 0;
        break;
    }
    return write_entry(a, head, count);
}

int dt_lost_files_plan_ex(DtVolume *v, dt_u32 stamp, DtPlan *plan, DtReport report,
                          void *report_ctx, DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    Adopt a;
    dt_u32 cluster, value;
    int pass, ok = 0;

    err[0] = 0;
    memset(&a, 0, sizeof(a));
    if (!dt_dos_stamp_valid(stamp))
        return fail(err, "Lost-chain conversion requires a valid DOS date and time");
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_LOST_FILES;
    plan->mark_bad = (int)stamp;
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_owned(v, &result, &a.space.owned, adopt_issue, &a, progress, ctx) ||
        !result.complete || a.unsafe) {
        free(a.space.owned);
        return fail(err, "Converting lost chains requires a complete scan without other "
                         "allocation or directory damage");
    }
    a.space.plan = plan;
    a.space.search = 2;
    a.space.free_clusters = result.free_clusters;
    a.space.progress = progress;
    a.space.progress_ctx = ctx;
    a.space.err = err;
    a.report = report;
    a.report_ctx = report_ctx;
    if (!result.lost_clusters) {
        ok = 1;
        goto done;
    }
    a.state = (unsigned char *)calloc((size_t)v->clusters + 2, 1);
    if (!a.state) {
        fail(err, "Insufficient memory to classify lost chains");
        goto done;
    }
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (progress && !(cluster & 63) &&
            !progress(ctx, "Classify lost chains", cluster - 2, v->clusters)) {
            fail(err, "Lost-chain conversion cancelled; no writes made");
            goto done;
        }
        if (a.space.owned[cluster / 8] & (1U << (cluster % 8)))
            continue;
        if (!dt_fat(v, cluster, &value)) {
            fail(err, "Cannot read allocation while classifying lost chains");
            goto done;
        }
        if (value && value != bad_mark(v))
            a.state[cluster] |= LOST;
    }
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (!(a.state[cluster] & LOST))
            continue;
        if (!dt_fat(v, cluster, &value)) {
            fail(err, "Cannot read allocation while classifying lost chains");
            goto done;
        }
        if (data_link(v, value) && (a.state[value] & LOST))
            a.state[value] |= POINTED;
    }
    if (!read_root(&a))
        goto done;
    /* Chain heads first, then clusters that only belong to cycles or merges. */
    for (pass = 0; pass < 2; pass++) {
        for (cluster = 2; cluster < v->clusters + 2; cluster++) {
            if (!(a.state[cluster] & LOST) || (a.state[cluster] & VISITED) ||
                (!pass && (a.state[cluster] & POINTED)))
                continue;
            if (progress && !progress(ctx, "Adopt lost chains", cluster - 2, v->clusters)) {
                fail(err, "Lost-chain conversion cancelled; no writes made");
                goto done;
            }
            if (!adopt_chain(&a, cluster))
                goto done;
        }
    }
    ok = update_hints(&a.space);
done:
    free(a.state);
    free(a.ends);
    free(a.deleted);
    free(a.names);
    free(a.space.owned);
    v->cache_sector = 0xffffffffUL;
    return ok;
}

int dt_lost_files_plan(DtVolume *v, dt_u32 stamp, DtPlan *plan, DtProgress progress, void *ctx,
                       char *err)
{
    return dt_lost_files_plan_ex(v, stamp, plan, NULL, NULL, progress, ctx, err);
}
