/* Bad-cluster retesting and non-destructive FAT marker changes. */
#include "disk.h"
#include <string.h>

static dt_u32 bad_value(const DtVolume *v)
{
    return v->bits == 12 ? 0xff7 : (v->bits == 16 ? 0xfff7 : 0x0ffffff7UL);
}

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static void put32(unsigned char *p, dt_u32 n)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (8 * i));
}

/* Return 1 for readable, 0 for failed sectors, -1 for cancellation. */
static int retest_cluster(DtVolume *v, dt_u32 cluster, dt_u32 *failed, dt_u32 *tested,
                          DtProgress progress, void *ctx, DtReport report, void *report_ctx)
{
    unsigned sector;
    unsigned char buffer[512];
    dt_u32 start = v->data + (cluster - 2) * v->spc;

    *failed = 0;
    for (sector = 0; sector < v->spc; sector++) {
        if (progress && !progress(ctx, "Retest cluster", sector, v->spc))
            return -1;
        if (!dt_read(v, start + sector, buffer)) {
            ++*failed;
            if (report)
                report(report_ctx,
                       "BAD_SECTOR",
                       v->start + start + sector,
                       "Disk LBA failed the read retest");
        }
        ++*tested;
    }
    return *failed == 0;
}

int dt_bad_retest(DtVolume *v, DtRetest *result, DtReport report, void *report_ctx,
                  DtProgress progress, void *progress_ctx)
{
    dt_u32 cluster, value, failed;
    int status;

    memset(result, 0, sizeof(*result));
    v->cache_sector = 0xffffffffUL;
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (!(cluster & 31) && progress &&
            !progress(progress_ctx, "Find bad clusters", cluster - 2, v->clusters)) {
            if (report)
                report(
                    report_ctx, "RETEST_INCOMPLETE", cluster, "Retest cancelled while reading FAT");
            return 0;
        }
        if (!dt_fat(v, cluster, &value)) {
            if (report)
                report(report_ctx, "RETEST_INCOMPLETE", cluster, "Cannot read FAT entry");
            return 0;
        }
        if (value != bad_value(v))
            continue;
        result->marked++;
        status = retest_cluster(v,
                                cluster,
                                &failed,
                                &result->tested_sectors,
                                progress,
                                progress_ctx,
                                report,
                                report_ctx);
        result->failed_sectors += failed;
        if (status < 0) {
            if (failed)
                result->failed++;
            if (report)
                report(report_ctx,
                       "RETEST_INCOMPLETE",
                       cluster,
                       "Retest cancelled before completing this cluster; markers retained");
            return 0;
        }
        if (status) {
            result->readable++;
            if (report)
                report(report_ctx,
                       "BAD_READABLE",
                       cluster,
                       "All sectors currently readable; bad marker retained");
        } else {
            result->failed++;
            if (report)
                report(report_ctx,
                       "BAD_UNREADABLE",
                       cluster,
                       "One or more sectors failed; bad marker retained");
        }
    }
    result->complete = 1;
    return 1;
}

static int update_fsinfo(DtPlan *plan, dt_u32 sector, dt_u32 free_count, char *err)
{
    DtPatch *patch = dt_plan_sector(plan, sector, err);

    if (!patch)
        return 0;
    if (dt_dword(patch->before) != 0x41615252UL || dt_dword(patch->before + 484) != 0x61417272UL ||
        dt_dword(patch->before + 508) != 0xaa550000UL)
        return fail(err, "FSInfo signatures are invalid; repair metadata first");
    put32(patch->after + 488, free_count);
    put32(patch->after + 492, 0xffffffffUL);
    return 1;
}

static void marker_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    int *unsafe = (int *)ctx;

    (void)where;
    (void)message;
    /* A malformed timestamp does not change allocation or ownership. */
    if (strcmp(code, "DIR_TIMESTAMP"))
        *unsafe = 1;
}

int dt_bad_plan(DtVolume *v, dt_u32 cluster, int mark_bad, DtPlan *plan, DtProgress progress,
                void *ctx, char *err)
{
    DtResult result;
    dt_u32 value, failed, tested = 0, backup_info;
    unsigned copy;
    int status, unsafe = 0;

    dt_plan_clear(plan);
    if (cluster < 2 || cluster >= v->clusters + 2)
        return fail(err, "Cluster is outside the data area");
    v->cache_sector = 0xffffffffUL;
    if (!dt_fat(v, cluster, &value))
        return fail(err, "Cannot read FAT entry");
    if (mark_bad && value)
        return fail(err,
                    "Only free clusters can be marked bad; allocated data must be recovered first");
    if (!mark_bad && value != bad_value(v))
        return fail(err, "Cluster is not marked bad");

    /* This also rejects directory entries or chains that reference free/bad clusters. */
    if (!dt_scan_ex(v, &result, marker_issue, &unsafe, progress, ctx) || unsafe)
        return fail(err,
                    "Filesystem check must complete without issues before changing bad markers");
    if (!mark_bad) {
        status = retest_cluster(v, cluster, &failed, &tested, progress, ctx, NULL, NULL);
        if (status < 0)
            return fail(err, "Retest cancelled; marker unchanged");
        if (!status)
            return fail(err, "Cluster still contains unreadable sectors; marker unchanged");
    }

    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->cluster = cluster;
    plan->mark_bad = mark_bad != 0;
    for (copy = 0; copy < v->copies; copy++) {
        if (!v->mirrored && copy != v->active)
            continue;
        if (!dt_plan_set_fat(plan, cluster, mark_bad ? bad_value(v) : 0, copy, err))
            return 0;
    }
    if (v->bits == 32 && v->fsinfo && v->fsinfo != 0xffff) {
        dt_u32 free_count = mark_bad ? result.free_clusters - 1 : result.free_clusters + 1;
        if (!update_fsinfo(plan, v->fsinfo, free_count, err))
            return 0;
        if (v->backup && v->backup != 0xffff) {
            backup_info = v->backup + v->fsinfo;
            if (backup_info >= v->fat || backup_info == v->backup)
                return fail(err, "Backup FSInfo is outside reserved metadata");
            if (!update_fsinfo(plan, backup_info, free_count, err))
                return 0;
        }
    }
    return 1;
}

/* Relocate a readable allocation cluster before retiring its original location. */
typedef struct {
    DtVolume *volume;
    dt_u32 target, previous, entry_sector;
    unsigned entry_offset, owners;
    int directory;
    DtProgress progress;
    void *ctx;
    DtPlan *plan;
    dt_u32 replacement;
    char *err;
    DtReport report;
    void *report_ctx;
} Relocate;

static int relocate_owner(void *ctx, const DtFile *file)
{
    Relocate *r = (Relocate *)ctx;
    dt_u32 cluster = file->start, previous = 0, next, i;

    for (i = 0; i < file->clusters; i++) {
        if (!(i & 31) && r->progress &&
            !r->progress(r->ctx, "Locate cluster owner", i, file->clusters))
            return 0;
        if (cluster == r->target) {
            r->owners++;
            r->previous = previous;
            r->entry_sector = file->sector;
            r->entry_offset = file->offset;
            r->directory = file->directory;
            if (r->report && !file->directory)
                r->report(r->report_ctx, "SALVAGE_FILE", r->target, file->path);
            break;
        }
        if (!dt_fat(r->volume, cluster, &next))
            return 0;
        previous = cluster;
        cluster = next;
    }
    return 1;
}

static void relocate_start(DtVolume *v, unsigned char *entry, dt_u32 cluster)
{
    entry[26] = (unsigned char)cluster;
    entry[27] = (unsigned char)(cluster >> 8);
    if (v->bits == 32) {
        entry[20] = (unsigned char)(cluster >> 16);
        entry[21] = (unsigned char)(cluster >> 24);
    }
}

static int relocate_child(void *ctx, const DtFile *file)
{
    Relocate *r = (Relocate *)ctx;
    DtVolume *v = r->volume;
    DtPatch *patch;

    if (file->directory != 1 || file->parent != r->target)
        return 1;
    patch = dt_plan_sector(r->plan, v->data + (file->start - 2) * v->spc, r->err);
    if (!patch)
        return 0;
    relocate_start(v, patch->after + 32, r->replacement);
    return 1;
}

static int relocate_plan(DtVolume *v, dt_u32 cluster, DtPlan *plan, int salvage, DtReport report,
                         void *report_ctx, DtProgress progress, void *ctx, char *err)
{
    Relocate owner;
    DtResult result;
    dt_u32 next, replacement, value, reserved, from, to, backup_info;
    unsigned sector, copy, attempt;
    int unsafe = 0;
    unsigned char bytes[512];
    DtPatch *patch;

    dt_plan_clear(plan);
    err[0] = 0;
    reserved = bad_value(v) - 7;
    if (cluster < 2 || cluster >= v->clusters + 2 || cluster >= reserved)
        return fail(err, "Cluster is outside the allocatable data area");
    memset(&owner, 0, sizeof(owner));
    owner.volume = v;
    owner.target = cluster;
    owner.progress = progress;
    owner.ctx = ctx;
    owner.report = salvage ? report : NULL;
    owner.report_ctx = report_ctx;
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_files(v, &result, relocate_owner, &owner, marker_issue, &unsafe, progress, ctx) ||
        unsafe)
        return fail(err,
                    "Relocation requires a complete filesystem check without allocation issues");
    if (owner.owners != 1)
        return fail(err, "Relocation requires one file or directory owner");
    if (salvage && owner.directory)
        return fail(err,
                    "Salvage permits regular file data only; directory bytes cannot be guessed");
    if (!dt_fat(v, cluster, &next))
        return fail(err, "Cannot read source cluster allocation");
    for (replacement = 2; replacement < v->clusters + 2 && replacement < reserved; replacement++) {
        if (!(replacement & 31) && progress &&
            !progress(ctx, "Find replacement cluster", replacement - 2, v->clusters))
            return fail(err, "Relocation cancelled; no sectors changed");
        if (!dt_fat(v, replacement, &value))
            return fail(err, "Cannot read replacement cluster allocation");
        if (value)
            continue;
        to = v->data + (replacement - 2) * v->spc;
        for (sector = 0; sector < v->spc; sector++) {
            if (progress && !progress(ctx, "Check replacement cluster", sector, v->spc))
                return fail(err, "Relocation cancelled; no sectors changed");
            if (!dt_read(v, to + sector, bytes))
                break;
        }
        if (sector == v->spc)
            break;
    }
    if (replacement >= v->clusters + 2 || replacement >= reserved)
        return fail(err, "No readable free cluster is available for relocation");
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = salvage                ? DT_REPAIR_SALVAGE
                 : owner.directory == 2 ? DT_REPAIR_RELOCATE_ROOT
                 : owner.directory      ? DT_REPAIR_RELOCATE_DIR
                                        : DT_REPAIR_RELOCATE;
    plan->cluster = cluster;
    from = v->data + (cluster - 2) * v->spc;
    to = v->data + (replacement - 2) * v->spc;
    /* Destination data is journaled and written before changing any owner link.
       The suspected source sectors are never written, including during undo. */
    for (sector = 0; sector < v->spc; sector++) {
        if (progress && !progress(ctx, "Copy file cluster", sector, v->spc))
            return fail(err, "Relocation cancelled; no sectors changed");
        for (attempt = 0; attempt < 3; attempt++) {
            if (attempt && progress && !progress(ctx, "Retry relocation sector", attempt, 3))
                return fail(err, "Relocation retry cancelled; no sectors changed");
            if (dt_read(v, from + sector, bytes))
                break;
        }
        if (attempt == 3) {
            if (!salvage)
                return fail(err,
                            "Source cluster is unreadable; export recoverable file data first");
            memset(bytes, 0, sizeof(bytes));
            plan->gaps[sector / 8] |= (unsigned char)(1u << (sector % 8));
            if (report) {
                char message[160];

                sprintf(message,
                        "Cluster %lu sector %u: all 512 replacement bytes will be ZERO; source "
                        "unreadable after 3 attempts",
                        (unsigned long)cluster,
                        sector);
                report(report_ctx, "SALVAGE_GAP", v->start + from + sector, message);
            }
        }
        patch = dt_plan_sector(plan, to + sector, err);
        if (!patch)
            return 0;
        memcpy(patch->after, bytes, sizeof(bytes));
    }
    for (copy = 0; copy < v->copies; copy++) {
        if (!v->mirrored && copy != v->active)
            continue;
        if (!dt_plan_set_fat(plan, replacement, next, copy, err) ||
            (owner.previous && !dt_plan_set_fat(plan, owner.previous, replacement, copy, err)) ||
            !dt_plan_set_fat(plan, cluster, bad_value(v), copy, err))
            return 0;
    }
    if (!owner.previous && owner.directory != 2) {
        patch = dt_plan_sector(plan, owner.entry_sector, err);
        if (!patch)
            return 0;
        relocate_start(v, patch->after + owner.entry_offset, replacement);
        if (owner.directory) {
            patch = dt_plan_sector(plan, to, err);
            if (!patch)
                return 0;
            relocate_start(v, patch->after, replacement);
            owner.plan = plan;
            owner.replacement = replacement;
            owner.err = err;
            if (!dt_scan_files(
                    v, &result, relocate_child, &owner, marker_issue, &unsafe, progress, ctx) ||
                unsafe) {
                if (!*err)
                    fail(err, "Directory relocation ownership check failed or was cancelled");
                return 0;
            }
        }
    }
    if (v->bits == 32 && v->fsinfo && v->fsinfo != 0xffff) {
        if (!update_fsinfo(plan, v->fsinfo, result.free_clusters - 1, err))
            return 0;
        if (v->backup && v->backup != 0xffff) {
            backup_info = v->backup + v->fsinfo;
            if (backup_info >= v->fat || backup_info == v->backup)
                return fail(err, "Backup FSInfo is outside reserved metadata");
            if (!update_fsinfo(plan, backup_info, result.free_clusters - 1, err))
                return 0;
        }
    }
    if (owner.directory == 2 && v->fsinfo && v->fsinfo != 0xffff) {
        /* A concrete search-start hint avoids a DOS BPB reload normalizing
           the unknown hint after commit and invalidating exact journal undo. */
        patch = dt_plan_sector(plan, v->fsinfo, err);
        if (!patch)
            return 0;
        put32(patch->after + 492, 2);
        if (v->backup && v->backup != 0xffff) {
            patch = dt_plan_sector(plan, v->backup + v->fsinfo, err);
            if (!patch)
                return 0;
            put32(patch->after + 492, 2);
        }
    }
    /* Publish a changed root pointer after allocation and FSInfo. DOS may
       persist cached hints while synchronizing a boot-sector write. */
    if (!owner.previous && owner.directory == 2) {
        if (v->backup && v->backup != 0xffff) {
            patch = dt_plan_sector(plan, v->backup, err);
            if (!patch)
                return 0;
            put32(patch->after + 44, replacement);
        }
        patch = dt_plan_sector(plan, 0, err);
        if (!patch)
            return 0;
        put32(patch->after + 44, replacement);
        /* Root children use parent zero, so no dot/dotdot entries change. */
    }
    return 1;
}

int dt_relocate_plan(DtVolume *v, dt_u32 cluster, DtPlan *plan, DtProgress progress, void *ctx,
                     char *err)
{
    return relocate_plan(v, cluster, plan, 0, NULL, NULL, progress, ctx, err);
}

int dt_salvage_plan(DtVolume *v, dt_u32 cluster, DtPlan *plan, DtReport report, void *report_ctx,
                    DtProgress progress, void *ctx, char *err)
{
    return relocate_plan(v, cluster, plan, 1, report, report_ctx, progress, ctx, err);
}
