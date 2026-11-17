/* One-step automatic repair: plan, journal and verify every applicable repair
   in a dependency-safe order, rescanning after each committed change. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

enum {
    STEP_FAT_HEADERS,
    STEP_FAT_COPIES,
    STEP_TREE_SOURCE,
    STEP_CROSSLINKS,
    STEP_TREE,
    STEP_DIRECTORIES,
    STEP_FILES,
    STEP_ENTRIES,
    STEP_EXTRA_DOTS,
    STEP_ATTRIBUTES,
    STEP_LABELS,
    STEP_LABEL_TEXT,
    STEP_CONFLICTS,
    STEP_ALIASES,
    STEP_LFN,
    STEP_TIMESTAMPS,
    STEP_LOST_FILES,
    STEP_FSINFO,
    STEP_BOOT_BACKUP,
    STEP_RETIRE_BAD,
    STEP_COUNT
};

static const char *const step_names[STEP_COUNT] = {
    "Rebuild FAT headers",
    "Rebuild FAT copies",
    "Repair tree from FAT copy",
    "Copy cross-linked clusters",
    "Repair filesystem tree",
    "Repair directory chains",
    "Repair file chains",
    "Repair directory entries",
    "Remove extra dot entries",
    "Clear reserved attributes",
    "Repair volume labels",
    "Repair volume label text",
    "Rename conflicting names",
    "Replace invalid DOS names",
    "Clear invalid long names",
    "Clear invalid timestamps",
    "Convert lost chains to files",
    "Rebuild FAT32 FSInfo",
    "Rebuild backup boot sector",
    "Retire unreadable clusters",
};

/* Current local time as a DOS date (high word) and time (low word). */
dt_u32 dt_dos_now(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    unsigned date, clock;

    if (!t || t->tm_year < 80 || t->tm_year > 207)
        return (dt_u32)((1 << 5) | 1) << 16;
    date = ((unsigned)(t->tm_year - 80) << 9) | ((unsigned)(t->tm_mon + 1) << 5) |
           (unsigned)t->tm_mday;
    clock = ((unsigned)t->tm_hour << 11) | ((unsigned)t->tm_min << 5) |
            (unsigned)(t->tm_sec > 59 ? 29 : t->tm_sec / 2);
    return ((dt_u32)date << 16) | clock;
}

const char *dt_autofix_step_name(unsigned step)
{
    return step < STEP_COUNT ? step_names[step] : "";
}

typedef struct {
    DtVolume *v;
    const DtAutofixOptions *options;
    DtAutofix *out;
    DtProgress progress;
    void *progress_ctx;
    DtReport report;
    void *report_ctx;
    int cancelled;
    dt_u32 *pending;
    unsigned pending_count, pending_capacity;
    char *err;
} Autofix;

static int tracked(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    Autofix *a = (Autofix *)ctx;

    if (a->progress && !a->progress(a->progress_ctx, phase, done, total)) {
        a->cancelled = 1;
        return 0;
    }
    return 1;
}

static int same_changes(const DtPlan *a, const DtPlan *b)
{
    unsigned i;

    if (a->count != b->count)
        return 0;
    for (i = 0; i < a->count; i++)
        if (a->patch[i].sector != b->patch[i].sector ||
            memcmp(a->patch[i].after, b->patch[i].after, 512))
            return 0;
    return 1;
}

/* 1 when mirrored FAT copies differ, 0 when identical, -1 on read failure. */
static int mirrors_differ(Autofix *a)
{
    DtVolume *v = a->v;
    unsigned char first[512], other[512];
    dt_u32 sector;
    unsigned copy;

    if (!v->mirrored || v->copies < 2)
        return 0;
    for (sector = 0; sector < v->fatsz; sector++) {
        if (!(sector & 63) && !tracked(a, "Compare FAT copies", sector, v->fatsz))
            return -1;
        if (!dt_read(v, v->fat + sector, first))
            return -1;
        for (copy = 1; copy < v->copies; copy++) {
            if (!dt_read(v, v->fat + copy * v->fatsz + sector, other))
                return -1;
            if (memcmp(first, other, 512))
                return 1;
        }
    }
    return 0;
}

/* Choose a FAT authority only when it is unambiguous: exactly one copy yields
   a valid plan, or every valid plan writes identical bytes. */
static int authority_plan(Autofix *a, int tree, DtPlan *plan, char *err)
{
    DtVolume *v = a->v;
    DtPlan *candidate;
    unsigned source, found = 0;
    int differ = 0;

    if (!v->mirrored || v->copies < 2)
        return fail(err, "FAT mirroring is disabled; there is no copy to choose");
    candidate = dt_plan_create();
    if (!candidate)
        return fail(err, "Insufficient memory for FAT authority selection");
    for (source = 1; source <= v->copies; source++) {
        char local[160];
        int ok = tree ? dt_tree_source_plan(v, source, candidate, tracked, a, local)
                      : dt_mirror_plan(v, source, candidate, tracked, a, local);

        if (a->cancelled)
            break;
        if (!ok || !candidate->count)
            continue;
        if (!found++) {
            DtPlan *swap = candidate;

            /* Keep the first valid plan; continue comparing in a fresh plan. */
            dt_plan_clear(plan);
            *plan = *swap;
            memset(swap, 0, sizeof(*swap));
        } else if (!same_changes(plan, candidate))
            differ = 1;
    }
    dt_plan_free(candidate);
    if (a->cancelled)
        return fail(err, "Automatic repair cancelled");
    if (!found) {
        dt_plan_clear(plan);
        return fail(err, tree ? "No FAT copy yields a consistent repaired tree"
                              : "No FAT copy is self-consistent; tree repair is needed first");
    }
    if (differ) {
        dt_plan_clear(plan);
        return fail(err, "Several FAT copies are plausible; choose one with --repair-fat N");
    }
    return 1;
}

static int add_pending(Autofix *a, dt_u32 cluster)
{
    unsigned i;

    for (i = 0; i < a->pending_count; i++)
        if (a->pending[i] == cluster)
            return 1;
    if (a->pending_count == a->pending_capacity) {
        unsigned grown = a->pending_capacity ? a->pending_capacity * 2 : 16;
        dt_u32 *next = (dt_u32 *)realloc(a->pending, grown * sizeof(dt_u32));

        if (!next)
            return 0;
        a->pending = next;
        a->pending_capacity = grown;
    }
    a->pending[a->pending_count++] = cluster;
    return 1;
}

static void drop_pending(Autofix *a)
{
    memmove(a->pending, a->pending + 1, (a->pending_count - 1) * sizeof(dt_u32));
    a->pending_count--;
}

typedef struct {
    Autofix *a;
    int memory;
} SurfaceFailures;

static void surface_failure(void *ctx, const char *code, dt_u32 where, const char *message)
{
    SurfaceFailures *s = (SurfaceFailures *)ctx;
    Autofix *a = s->a;
    DtVolume *v = a->v;
    dt_u32 relative = where - v->start;

    if (a->report)
        a->report(a->report_ctx, code, where, message);
    if (strcmp(code, "SURFACE_READ"))
        return;
    a->out->surface_failed++;
    if (relative < v->data) {
        a->out->unresolved_bad++;
        return;
    }
    if (!add_pending(a, (relative - v->data) / v->spc + 2))
        s->memory = 1;
}

static int surface_scan(Autofix *a)
{
    DtMap *map = (DtMap *)calloc(1, sizeof(DtMap));
    SurfaceFailures failures;
    int ok;

    if (!map)
        return fail(a->err, "Insufficient memory for the surface scan");
    failures.a = a;
    failures.memory = 0;
    ok = dt_surface_ex(a->v, 0, a->v->total, map, tracked, a, surface_failure, &failures);
    a->out->surface_complete = ok && map->complete;
    free(map);
    if (a->cancelled)
        return fail(a->err, "Automatic repair cancelled during the surface scan");
    if (failures.memory)
        return fail(a->err, "Insufficient memory to record surface failures");
    return 1;
}

/* Non-destructive write test of free space only: invert every bit of each
   sector, verify, then restore and verify the original bytes. Owned clusters
   (including cross-linked or invalid-chain data) are never written. */
static int write_test(Autofix *a)
{
    DtVolume *v = a->v;
    DtDisk *d = v->disk;
    DtResult scanned;
    unsigned char *owned = NULL, original[512], pattern[512], check[512];
    dt_u32 cluster, value, sector, lba;
    unsigned i, k;
    int bad;

    if (!dt_scan_owned(v, &scanned, &owned, NULL, NULL, tracked, a) || !scanned.complete) {
        free(owned);
        return fail(a->err, a->cancelled ? "Automatic repair cancelled"
                                         : "The write test requires a complete ownership scan");
    }
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (!(cluster & 15) && !tracked(a, "Write-test free space", cluster - 2, v->clusters))
            break;
        if (owned[cluster / 8] & (1U << (cluster % 8)))
            continue;
        if (!dt_fat(v, cluster, &value) || value)
            continue;
        for (k = 0; k < a->pending_count && a->pending[k] != cluster; k++)
            ;
        if (k < a->pending_count)
            continue;
        bad = 0;
        for (i = 0; i < v->spc && !bad; i++) {
            sector = v->data + (cluster - 2) * v->spc + i;
            lba = v->start + sector;
            if (!dt_read(v, sector, original)) {
                bad = 1;
                break;
            }
            for (k = 0; k < 512; k++)
                pattern[k] = (unsigned char)(original[k] ^ 0xff);
            if (!d->write(d->ctx, lba, pattern) || !d->sync(d->ctx) || !dt_read(v, sector, check) ||
                memcmp(check, pattern, 512))
                bad = 1;
            if (!d->write(d->ctx, lba, original) || !d->sync(d->ctx) ||
                !dt_read(v, sector, check) || memcmp(check, original, 512)) {
                bad = 1;
                a->out->write_unrestored++;
            }
            if (!bad)
                a->out->write_tested++;
        }
        if (!bad)
            continue;
        a->out->write_failed++;
        if (a->report) {
            char message[96];

            sprintf(message, "Free cluster %lu failed the write/verify test", (unsigned long)cluster);
            a->report(a->report_ctx, "WRITE_TEST", v->start + v->data + (cluster - 2) * v->spc,
                      message);
        }
        if (!add_pending(a, cluster)) {
            free(owned);
            return fail(a->err, "Insufficient memory to record write-test failures");
        }
    }
    free(owned);
    v->cache_sector = 0xffffffffUL;
    if (a->cancelled)
        return fail(a->err, "Automatic repair cancelled during the write test");
    a->out->write_complete = 1;
    return 1;
}

/* Build one step's plan. Return 1 with a plan (possibly empty), 0 when the
   step is refused for the current state (reason in err). */
static int build_step(Autofix *a, unsigned step, DtPlan *plan, char *err)
{
    DtVolume *v = a->v;
    DtReport report = a->report;
    void *rctx = a->report_ctx;

    err[0] = 0;
    dt_plan_clear(plan);
    switch (step) {
    case STEP_FAT_HEADERS:
        return dt_fat_header_plan(v, plan, tracked, a, err);
    case STEP_FAT_COPIES:
    case STEP_TREE_SOURCE: {
        int differ = mirrors_differ(a);

        if (differ < 0)
            return fail(err, a->cancelled ? "Automatic repair cancelled"
                                          : "Cannot read the FAT copies");
        if (!differ)
            return dt_plan_init(plan, v, err);
        return authority_plan(a, step == STEP_TREE_SOURCE, plan, err);
    }
    case STEP_CROSSLINKS:
        return dt_crosslink_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_TREE:
        return dt_tree_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_DIRECTORIES:
        return dt_directory_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_FILES:
        return dt_chain_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_ENTRIES:
        return dt_entry_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_EXTRA_DOTS:
        return dt_extra_dot_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_ATTRIBUTES:
        return dt_attribute_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_LABELS:
        return dt_label_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_LABEL_TEXT:
        return dt_label_text_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_CONFLICTS:
        return dt_conflict_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_ALIASES:
        return dt_alias_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_LFN:
        return dt_lfn_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_TIMESTAMPS:
        return dt_timestamp_plan_ex(v, plan, report, rctx, tracked, a, err);
    case STEP_LOST_FILES:
        if (a->options->lost_mode == DT_AUTOFIX_LOST_KEEP)
            return dt_plan_init(plan, v, err);
        if (a->options->lost_mode == DT_AUTOFIX_LOST_FREE)
            return dt_lost_plan(v, plan, tracked, a, err);
        return dt_lost_files_plan_ex(v, a->options->stamp, plan, report, rctx, tracked, a, err);
    case STEP_RETIRE_BAD:
        while (a->pending_count) {
            dt_u32 cluster = a->pending[0], value;
            char reason[160];

            if (!dt_fat(v, cluster, &value)) {
                a->out->unresolved_bad++;
                drop_pending(a);
                continue;
            }
            if (value == (v->bits == 12 ? 0xff7UL : v->bits == 16 ? 0xfff7UL : 0x0ffffff7UL)) {
                drop_pending(a);
                continue;
            }
            if (!value) {
                if (dt_bad_plan(v, cluster, 1, plan, tracked, a, err))
                    return 1;
            } else if (dt_relocate_plan(v, cluster, plan, tracked, a, err) ||
                       (a->options->salvage && !a->cancelled &&
                        dt_salvage_plan(v, cluster, plan, report, rctx, tracked, a, err)))
                return 1;
            if (a->cancelled)
                return 0;
            sprintf(reason, "Cluster %lu not retired: ", (unsigned long)cluster);
            strncat(reason, err, sizeof(reason) - strlen(reason) - 1);
            if (a->report)
                a->report(a->report_ctx, "BAD_UNRESOLVED", v->start + v->data +
                                                               (cluster - 2) * v->spc,
                          reason);
            a->out->unresolved_bad++;
            drop_pending(a);
        }
        return dt_plan_init(plan, v, err);
    case STEP_FSINFO:
        if (v->bits != 32)
            return dt_plan_init(plan, v, err);
        return dt_fsinfo_plan(v, plan, tracked, a, err);
    case STEP_BOOT_BACKUP:
        if (v->bits != 32)
            return dt_plan_init(plan, v, err);
        return dt_boot_backup_plan(v, plan, tracked, a, err);
    }
    return fail(err, "Unknown automatic repair step");
}

static int journal_name(Autofix *a, char *path, size_t capacity)
{
    const char *dir = a->options->journal_dir ? a->options->journal_dir : "";
    size_t length = strlen(dir);
    const char *separator = "";
    FILE *probe;

#ifdef __WATCOMC__
    if (length && dir[length - 1] != '\\' && dir[length - 1] != '/' && dir[length - 1] != ':')
        separator = "\\";
#else
    if (length && dir[length - 1] != '/')
        separator = "/";
#endif
    if (length + 16 > capacity)
        return fail(a->err, "Journal directory path is too long");
    while (++a->out->journal_number <= 9999) {
        sprintf(path, "%s%sMDFX%04u.MDJ", dir, separator, a->out->journal_number);
        probe = fopen(path, "rb");
        if (!probe)
            return 1;
        fclose(probe);
    }
    return fail(a->err, "No unused MDFXnnnn.MDJ journal name remains");
}

/* Repairs may change boot-sector fields the mount cached (label, FAT32 root). */
static void refresh_volume(DtVolume *v)
{
    unsigned char boot[512];

    v->cache_sector = 0xffffffffUL;
    if (v->boot_source || !v->disk->read(v->disk->ctx, v->start, boot))
        return;
    memcpy(v->boot, boot, 512);
    if (v->bits == 32)
        v->rootclus = dt_dword(boot + 44);
}

int dt_autofix(DtVolume *v, const DtAutofixOptions *options, DtAutofix *out, DtAutofixNote note,
               void *note_ctx, DtReport report, void *report_ctx, DtProgress progress, void *ctx,
               DtWriteProgress writing, void *writing_ctx, char *err)
{
    Autofix a;
    DtPlan *plan;
    char reasons[STEP_COUNT][160];
    unsigned step, repeats[STEP_COUNT], limit;
    int ok = 0, changed, scanned;

    err[0] = 0;
    memset(out, 0, sizeof(*out));
    memset(&a, 0, sizeof(a));
    memset(repeats, 0, sizeof(repeats));
    a.v = v;
    a.options = options;
    a.out = out;
    a.progress = progress;
    a.progress_ctx = ctx;
    a.report = report;
    a.report_ctx = report_ctx;
    a.err = err;
    if (v->boot_source)
        return fail(err, "Recover the primary boot sector before automatic repair");
    if (options->apply && (!v->disk->write || !v->disk->sync))
        return fail(err, "This target is read-only; reopen with write access");
    if (options->lost_mode == DT_AUTOFIX_LOST_FILES && !dt_dos_stamp_valid(options->stamp))
        return fail(err, "Automatic repair needs a valid DOS date and time for .CHK files");
    plan = dt_plan_create();
    if (!plan)
        return fail(err, "Insufficient memory for automatic repair");
    v->cache_sector = 0xffffffffUL;
    scanned = dt_scan_ex(v, &out->initial, NULL, NULL, tracked, &a);
    if (a.cancelled) {
        fail(err, "Automatic repair cancelled");
        goto done;
    }
    if (!scanned)
        out->initial.complete = 0;
    if ((options->surface || options->write_test) && !surface_scan(&a))
        goto done;
    if (options->write_test && options->apply && !write_test(&a))
        goto done;
    /* Each retired cluster costs one pass; other steps converge quickly. */
    limit = 64 + a.pending_count;
    do {
        changed = 0;
        out->passes++;
        for (step = 0; step < STEP_COUNT; step++) {
            reasons[step][0] = 0;
            if (repeats[step] >= 4 && step != STEP_RETIRE_BAD)
                continue;
            if (!build_step(&a, step, plan, reasons[step])) {
                if (a.cancelled) {
                    fail(err, "Automatic repair cancelled; earlier committed steps keep journals");
                    goto done;
                }
                continue;
            }
            reasons[step][0] = 0;
            if (!plan->count)
                continue;
            out->planned++;
            if (!options->apply) {
                if (note)
                    note(note_ctx, step_names[step], DT_AUTOFIX_PLANNED, plan, "");
                continue;
            }
            {
                char journal[300];

                if (!journal_name(&a, journal, sizeof(journal)))
                    goto done;
                if (!dt_plan_apply_ex(plan, journal, tracked, &a, writing, writing_ctx, err)) {
                    if (note)
                        note(note_ctx, step_names[step], DT_AUTOFIX_FAILED, plan, err);
                    goto done;
                }
                if (note)
                    note(note_ctx, step_names[step], DT_AUTOFIX_APPLIED, plan, journal);
            }
            out->applied++;
            repeats[step]++;
            refresh_volume(v);
            changed = 1;
            break;
        }
    } while (changed && options->apply && out->passes < limit);
    out->converged = !changed;
    for (step = 0; step < STEP_COUNT; step++) {
        if (!reasons[step][0])
            continue;
        out->refused++;
        if (note)
            note(note_ctx, step_names[step], DT_AUTOFIX_REFUSED, NULL, reasons[step]);
    }
    out->unresolved_bad += a.pending_count;
    v->cache_sector = 0xffffffffUL;
    scanned = dt_scan_ex(v, &out->final, NULL, NULL, tracked, &a);
    if (a.cancelled) {
        fail(err, "Final check cancelled; run a filesystem check");
        goto done;
    }
    if (!scanned)
        out->final.complete = 0;
    ok = 1;
done:
    free(a.pending);
    dt_plan_free(plan);
    v->cache_sector = 0xffffffffUL;
    return ok;
}
