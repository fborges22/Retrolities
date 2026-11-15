/* Filesystem repair plans are recomputed and journaled before they are applied. */
#include "disk.h"
#include <string.h>
#include <stdlib.h>

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static void put32(unsigned char *p, dt_u32 value)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

/* Auxiliary hints, timestamps and the backup boot copy do not determine ownership.
   Their damage may coexist; layout, primary-mount and live-tree checks still apply. */
static void metadata_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    int *other_damage = (int *)ctx;

    (void)where;
    (void)message;
    if (strcmp(code, "FSINFO_SIGNATURE") && strcmp(code, "FSINFO_FREE") &&
        strcmp(code, "FSINFO_NEXT") && strcmp(code, "FSINFO_RESERVED") &&
        strcmp(code, "BOOT_BACKUP") && strcmp(code, "DIR_TIMESTAMP"))
        *other_damage = 1;
}

int dt_fsinfo_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    dt_u32 sectors[2], first_free = 0xffffffffUL, cluster, value;
    unsigned i, count = 1;
    unsigned char rebuilt[512];
    int other_damage = 0;

    dt_plan_clear(plan);
    if (v->bits != 32)
        return fail(err, "FSInfo repair requires a FAT32 volume");
    if (!v->fsinfo || v->fsinfo == 0xffff || v->fsinfo >= v->fat)
        return fail(err, "No valid FSInfo location is declared in the boot sector");
    sectors[0] = v->fsinfo;
    if (v->backup && v->backup != 0xffff) {
        if (v->fsinfo == v->backup || v->backup + v->fsinfo >= v->fat)
            return fail(err, "FSInfo locations overlap boot metadata or exceed reserved sectors");
        sectors[count++] = v->backup + v->fsinfo;
    }
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_ex(v, &result, metadata_issue, &other_damage, progress, ctx) || other_damage)
        return fail(
            err, "Repair requires a complete scan with no issues outside FAT32 auxiliary metadata");
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (!(cluster & 31) && progress &&
            !progress(ctx, "Find free cluster", cluster - 2, v->clusters))
            return fail(err, "FSInfo repair cancelled; no writes made");
        if (!dt_fat(v, cluster, &value))
            return fail(err, "Cannot read FAT while rebuilding FSInfo");
        if (!value) {
            first_free = cluster;
            break;
        }
    }
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_FSINFO;
    memset(rebuilt, 0, sizeof(rebuilt));
    put32(rebuilt, 0x41615252UL);
    put32(rebuilt + 484, 0x61417272UL);
    put32(rebuilt + 488, result.free_clusters);
    put32(rebuilt + 492, first_free);
    put32(rebuilt + 508, 0xaa550000UL);
    for (i = 0; i < count; i++) {
        DtPatch *patch = dt_plan_sector(plan, sectors[i], err);

        if (!patch)
            return 0;
        memcpy(patch->after, rebuilt, 512);
        if (!memcmp(patch->before, patch->after, 512))
            plan->count--;
    }
    return 1;
}

int dt_boot_backup_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    DtPatch *patch;
    unsigned char primary[512];
    int other_damage = 0;

    dt_plan_clear(plan);
    if (v->bits != 32)
        return fail(err, "Backup boot repair requires a FAT32 volume");
    if (!v->backup || v->backup == 0xffff || v->backup >= v->fat || v->backup == v->fsinfo)
        return fail(err, "No separate, valid backup boot sector is declared");
    if (!dt_read(v, 0, primary) || memcmp(primary, v->boot, 512))
        return fail(err, "Primary boot sector changed or cannot be read; remount the volume");
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_ex(v, &result, metadata_issue, &other_damage, progress, ctx) || other_damage)
        return fail(
            err, "Repair requires a complete scan with no issues outside FAT32 auxiliary metadata");
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_BOOT_BACKUP;
    patch = dt_plan_sector(plan, v->backup, err);
    if (!patch)
        return 0;
    memcpy(patch->after, primary, 512);
    if (!memcmp(patch->before, patch->after, 512))
        plan->count = 0;
    return 1;
}

int dt_mirror_plan(DtVolume *v, unsigned source, DtPlan *plan, DtProgress progress, void *ctx,
                   char *err)
{
    DtVolume candidate;
    DtResult result;
    DtPlan *info;
    DtPatch *patch;
    dt_u32 sector, first, second;
    unsigned copy, i;
    unsigned char data[512], target[512];
    int other_damage = 0, ok;

    dt_plan_clear(plan);
    if (!v->mirrored || v->copies < 2)
        return fail(err, "FAT-copy repair requires a volume with mirroring enabled");
    if (!source || source > v->copies)
        return fail(err, "Source FAT number is outside this volume's copies");
    candidate = *v;
    candidate.active = source - 1;
    candidate.mirrored = 0;
    candidate.cache_sector = 0xffffffffUL;
    if (!dt_fat(&candidate, 0, &first) || !dt_fat(&candidate, 1, &second))
        return fail(err, "Cannot read the selected source FAT");
    if (first != ((v->bits == 12   ? 0xf00UL
                   : v->bits == 16 ? 0xff00UL
                                   : 0x0fffff00UL) |
                  v->boot[21]) ||
        (v->bits == 12   ? second < 0xff8UL
         : v->bits == 16 ? (second & 0x3fffUL) != 0x3fffUL
                         : (second & 0x03ffffffUL) != 0x03ffffffUL))
        return fail(err, "Selected source FAT has invalid reserved entries or media descriptor");
    if (!dt_scan_ex(&candidate, &result, metadata_issue, &other_damage, progress, ctx) ||
        other_damage)
        return fail(err, "Selected source FAT does not produce a complete, consistent filesystem");
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_MIRROR;
    plan->cluster = source;
    for (copy = 0; copy < v->copies; copy++) {
        if (copy == source - 1)
            continue;
        for (sector = 0; sector < v->fatsz; sector++) {
            if (progress &&
                !progress(
                    ctx, "Prepare FAT copies", copy * v->fatsz + sector, v->copies * v->fatsz))
                return fail(err, "FAT-copy repair cancelled; no writes made");
            if (!dt_read(v, v->fat + (source - 1) * v->fatsz + sector, data) ||
                !dt_read(v, v->fat + copy * v->fatsz + sector, target))
                return fail(err, "Cannot read source or destination FAT sector");
            if (!memcmp(data, target, 512))
                continue;
            patch = dt_plan_sector(plan, v->fat + copy * v->fatsz + sector, err);
            if (!patch)
                return 0;
            memcpy(patch->after, data, 512);
        }
    }
    if (v->bits != 32 || !v->fsinfo || v->fsinfo == 0xffff)
        return 1;
    /* FSInfo must describe the selected source, even if the old primary FAT was damaged. */
    info = dt_plan_create();
    if (!info)
        return fail(err, "Insufficient memory for FAT32 free-space repair");
    ok = dt_fsinfo_plan(&candidate, info, progress, ctx, err);
    for (i = 0; ok && i < info->count; i++) {
        patch = dt_plan_sector(plan, info->patch[i].sector, err);
        if (!patch)
            ok = 0;
        else
            memcpy(patch->after, info->patch[i].after, 512);
    }
    dt_plan_free(info);
    return ok;
}

static void lost_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    if (strcmp(code, "LOST_CLUSTERS") && strcmp(code, "FAT_RANGE"))
        metadata_issue(ctx, code, where, message);
}

int dt_lost_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    unsigned char *owned = NULL;
    unsigned char rebuilt[512];
    dt_u32 cluster, value, first_free = 0xffffffffUL;
    dt_u32 bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7UL;
    unsigned copy, i, count;
    dt_u32 sectors[2];
    int unsafe = 0, ok = 0;

    dt_plan_clear(plan);
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_owned(v, &result, &owned, lost_issue, &unsafe, progress, ctx) || unsafe) {
        free(owned);
        return fail(err, "Reclaim requires a complete ownership scan and consistent FAT copies");
    }
    if (!dt_plan_init(plan, v, err))
        goto done;
    plan->kind = DT_REPAIR_LOST;
    plan->cluster = result.lost_clusters;
    if (!result.lost_clusters) {
        ok = 1;
        goto done;
    }
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (progress && !(cluster & 31) &&
            !progress(ctx, "Prepare lost allocation", cluster - 2, v->clusters)) {
            fail(err, "Reclaim cancelled; no writes made");
            goto done;
        }
        if (!dt_fat(v, cluster, &value)) {
            fail(err, "Cannot read allocation while preparing reclaim");
            goto done;
        }
        if (!value && first_free == 0xffffffffUL)
            first_free = cluster;
        if ((owned[cluster / 8] & (1U << (cluster % 8))) || !value || value == bad)
            continue;
        if (first_free == 0xffffffffUL)
            first_free = cluster;
        for (copy = 0; copy < v->copies; copy++) {
            if (!v->mirrored && copy != v->active)
                continue;
            if (!dt_plan_set_fat(plan, cluster, 0, copy, err))
                goto done;
        }
    }
    if (v->bits == 32 && v->fsinfo && v->fsinfo != 0xffff) {
        /* The ownership scan has already checked reserved-sector layout. */
        sectors[0] = v->fsinfo;
        count = 1;
        if (v->backup && v->backup != 0xffff)
            sectors[count++] = v->backup + v->fsinfo;
        memset(rebuilt, 0, sizeof(rebuilt));
        put32(rebuilt, 0x41615252UL);
        put32(rebuilt + 484, 0x61417272UL);
        put32(rebuilt + 488, result.free_clusters + result.lost_clusters);
        put32(rebuilt + 492, first_free);
        put32(rebuilt + 508, 0xaa550000UL);
        for (i = 0; i < count; i++) {
            DtPatch *patch = dt_plan_sector(plan, sectors[i], err);

            if (!patch)
                goto done;
            memcpy(patch->after, rebuilt, 512);
            if (!memcmp(patch->before, patch->after, 512))
                plan->count--;
        }
    }
    ok = 1;
done:
    free(owned);
    return ok;
}

typedef struct EntryMove {
    dt_u32 source, destination;
    unsigned source_offset, destination_offset;
    struct EntryMove *next;
} EntryMove;

typedef struct {
    EntryMove *moves;
    unsigned char *owned;
    dt_u32 allocated;
    DtProgress progress;
    void *progress_ctx;
    DtPlan *plan;
    char *err;
    int unsafe;
    DtReport report;
    void *report_ctx;
} ChainRepair;

static int growth_hints(ChainRepair *repair, dt_u32 free_clusters)
{
    DtVolume *v = repair->plan->volume;
    dt_u32 sectors[2];
    unsigned i, count = 0;

    if (v->bits != 32 || !repair->allocated)
        return 1;
    if (repair->allocated > free_clusters)
        return fail(repair->err, "Free allocation changed while preparing directory growth");
    if (v->fsinfo && v->fsinfo != 0xffff && v->fsinfo < v->fat && v->fsinfo != v->backup) {
        sectors[count++] = v->fsinfo;
        if (v->backup && v->backup != 0xffff && v->backup + v->fsinfo < v->fat)
            sectors[count++] = v->backup + v->fsinfo;
    }
    for (i = 0; i < count; i++) {
        unsigned char data[512];
        DtPatch *patch;

        if (!dt_read(v, sectors[i], data))
            return fail(repair->err, "Cannot read allocation hints after directory growth");
        /* Invalid hints remain an independent finding; never interpret them. */
        if (dt_dword(data) != 0x41615252UL || dt_dword(data + 484) != 0x61417272UL ||
            dt_dword(data + 508) != 0xaa550000UL)
            continue;
        patch = dt_plan_sector(repair->plan, sectors[i], repair->err);
        if (!patch)
            return 0;
        put32(patch->after + 488, free_clusters - repair->allocated);
        put32(patch->after + 492, 0xffffffffUL);
    }
    return 1;
}

static int directory_space(ChainRepair *repair, dt_u32 *selected)
{
    DtVolume *v = repair->plan->volume;
    DtResult result;
    dt_u32 cluster, value;
    dt_u32 reserved = v->bits == 12 ? 0xff0 : v->bits == 16 ? 0xfff0 : 0x0ffffff0UL;

    if (!repair->owned &&
        !dt_scan_owned(
            v, &result, &repair->owned, NULL, NULL, repair->progress, repair->progress_ctx))
        return fail(repair->err, "Directory growth requires a complete ownership scan");
    for (cluster = 2; cluster < v->clusters + 2 && cluster < reserved; cluster++) {
        if ((cluster == 2 || !(cluster & 31)) && repair->progress &&
            !repair->progress(
                repair->progress_ctx, "Find directory space", cluster - 2, v->clusters))
            return fail(repair->err, "Directory growth cancelled; no writes made");
        if (repair->owned[cluster / 8] & (1U << (cluster % 8)))
            continue;
        if (!dt_fat(v, cluster, &value))
            return fail(repair->err, "Cannot read allocation while finding directory space");
        if (!value) {
            repair->owned[cluster / 8] |= 1U << (cluster % 8);
            *selected = cluster;
            return 1;
        }
    }
    return fail(repair->err, "No unowned free cluster is available to grow the directory");
}

static int scan_repairs(ChainRepair *repair, DtResult *result, DtFileVisit visit, DtReport report,
                        DtProgress progress, void *ctx)
{
    int ok;
    EntryMove *move;

    repair->moves = NULL;
    repair->owned = NULL;
    repair->allocated = 0;
    repair->progress = progress;
    repair->progress_ctx = ctx;
    ok = dt_scan_files(repair->plan->volume, result, visit, repair, report, repair, progress, ctx);
    if (ok && !repair->unsafe)
        ok = growth_hints(repair, result->free_clusters);
    free(repair->owned);
    while (repair->moves) {
        move = repair->moves;
        repair->moves = move->next;
        free(move);
    }
    return ok;
}

/* Scan callbacks retain original locations even when a parent plan has moved
   an entry. Resolve the intact record before applying any child correction. */
static unsigned char *planned_entry(ChainRepair *repair, const DtFile *file)
{
    DtPatch *patch = dt_plan_sector(repair->plan, file->sector, repair->err);
    unsigned offset, found = 0, destination = 0;
    EntryMove *move;

    if (!patch)
        return NULL;
    for (move = repair->moves; move; move = move->next) {
        if (move->source == file->sector && move->source_offset == file->offset) {
            patch = dt_plan_sector(repair->plan, move->destination, repair->err);
            return patch ? patch->after + move->destination_offset : NULL;
        }
    }
    for (offset = 0; offset < 512; offset += 32) {
        if (!memcmp(patch->before + file->offset, patch->after + offset, 32)) {
            destination = offset;
            found++;
        }
    }
    if (found != 1) {
        fail(repair->err, "Cannot locate a unique entry after relocation");
        return NULL;
    }
    return patch->after + destination;
}

static void chain_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    ChainRepair *repair = (ChainRepair *)ctx;

    if (strcmp(code, "FILE_SIZE") && strcmp(code, "CHAIN_RANGE") && strcmp(code, "CHAIN_SHARED") &&
        strcmp(code, "CHAIN_INVALID"))
        lost_issue(&repair->unsafe, code, where, message);
}

static int repair_file(void *ctx, const DtFile *file)
{
    ChainRepair *repair = (ChainRepair *)ctx;
    DtPlan *plan = repair->plan;
    DtVolume *v = plan->volume;
    dt_u32 cluster_bytes = v->spc * 512UL;
    dt_u32 expected = file->size / cluster_bytes + (file->size % cluster_bytes != 0);
    dt_u32 size = file->size;
    unsigned copy;
    unsigned char *entry;

    if (file->directory) {
        if (file->ending != DT_CHAIN_END)
            repair->unsafe = 1;
        return 1;
    }
    if (file->ending == DT_CHAIN_END && file->clusters == expected)
        return 1;
    if (file->clusters > 0xffffffffUL / cluster_bytes + 1)
        return fail(repair->err, "A file chain exceeds the FAT file-size limit; recover it first");
    if (file->clusters != expected) {
        dt_u64 capacity = (dt_u64)file->clusters * cluster_bytes;

        size = capacity > 0xffffffffUL ? 0xffffffffUL : (dt_u32)capacity;
    }
    if (file->ending != DT_CHAIN_END && file->last) {
        dt_u32 end = v->bits == 12 ? 0xfff : v->bits == 16 ? 0xffff : 0x0fffffffUL;

        for (copy = 0; copy < v->copies; copy++) {
            if (!v->mirrored && copy != v->active)
                continue;
            if (!dt_plan_set_fat(plan, file->last, end, copy, repair->err))
                return 0;
        }
    }
    if (size != file->size || (!file->clusters && file->start)) {
        entry = planned_entry(repair, file);
        if (!entry)
            return 0;
        put32(entry + 28, size);
        if (!file->clusters) {
            entry[26] = entry[27] = 0;
            if (v->bits == 32)
                entry[20] = entry[21] = 0;
        }
    }
    plan->cluster++;
    if (repair->report) {
        char *message = (char *)malloc(strlen(file->path) + 200);

        if (!message)
            return fail(repair->err, "Insufficient memory for file-repair preview");
        sprintf(message,
                "%s: entry byte %u; size %lu -> %lu; keep %lu clusters; start %lu -> %lu; %s",
                file->path,
                file->offset,
                (unsigned long)file->size,
                (unsigned long)size,
                (unsigned long)file->clusters,
                (unsigned long)file->start,
                (unsigned long)(file->clusters ? file->start : 0),
                file->ending == DT_CHAIN_END ? "length correction" : "terminate damaged chain");
        repair->report(repair->report_ctx, "FILE_REPAIR", v->start + file->sector, message);
        free(message);
    }
    return 1;
}

int dt_chain_plan_ex(DtVolume *v, DtPlan *plan, DtReport report, void *report_ctx,
                     DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    ChainRepair repair;

    err[0] = 0;
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_CHAINS;
    repair.plan = plan;
    repair.err = err;
    repair.unsafe = 0;
    repair.report = report;
    repair.report_ctx = report_ctx;
    v->cache_sector = 0xffffffffUL;
    if (!scan_repairs(&repair, &result, repair_file, chain_issue, progress, ctx)) {
        if (!*err)
            fail(err, "File repair requires a complete scan; cancelled or unreadable metadata");
        return 0;
    }
    if (repair.unsafe)
        return fail(err, "File repair requires consistent FAT copies and undamaged directories");
    return 1;
}

int dt_chain_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    return dt_chain_plan_ex(v, plan, NULL, NULL, progress, ctx, err);
}

/* A fully shared directory has no private records to retain. Give the later
   entry an independent empty chain; the first owner's allocated data is never
   copied, cleared or freed. All newly used space is covered by the journal. */
static int replace_shared_directory(ChainRepair *repair, const DtFile *file)
{
    DtPlan *plan = repair->plan;
    DtVolume *v = plan->volume;
    dt_u32 cluster, value, sector,
        end = v->bits == 12   ? 0xfff
              : v->bits == 16 ? 0xffff
                              : 0x0fffffffUL;
    unsigned i, copy;
    unsigned char *entry;

    if (file->directory != 1 || file->ending != DT_CHAIN_SHARED || file->clusters ||
        file->start >= end - 15) {
        repair->unsafe = 1;
        return 1;
    }
    if (!dt_fat(v, file->start, &value))
        return fail(repair->err, "Cannot validate the shared directory allocation");
    if (!(value >= end - 7 || (value >= 2 && value < v->clusters + 2 && value < end - 15))) {
        repair->unsafe = 1;
        return 1;
    }
    if (!directory_space(repair, &cluster))
        return 0;
    sector = v->data + (cluster - 2) * v->spc;
    for (i = 0; i < v->spc; i++) {
        DtPatch *patch;

        if (repair->progress &&
            !repair->progress(repair->progress_ctx, "Prepare empty directory", i, v->spc))
            return fail(repair->err, "Shared-directory replacement cancelled; no writes made");
        patch = dt_plan_sector(plan, sector + i, repair->err);
        if (!patch)
            return 0;
        memset(patch->after, 0, 512);
        if (!i) {
            unsigned dot;

            for (dot = 0; dot < 2; dot++) {
                dt_u32 target = dot ? file->parent : cluster;

                entry = patch->after + dot * 32;
                memcpy(entry, dot ? "..         " : ".          ", 11);
                entry[11] = 16;
                entry[26] = (unsigned char)target;
                entry[27] = (unsigned char)(target >> 8);
                if (v->bits == 32) {
                    entry[20] = (unsigned char)(target >> 16);
                    entry[21] = (unsigned char)(target >> 24);
                }
            }
        }
    }
    for (copy = 0; copy < v->copies; copy++) {
        if (!v->mirrored && copy != v->active)
            continue;
        if (!dt_plan_set_fat(plan, cluster, end, copy, repair->err))
            return 0;
    }
    entry = planned_entry(repair, file);
    if (!entry)
        return 0;
    entry[26] = (unsigned char)cluster;
    entry[27] = (unsigned char)(cluster >> 8);
    if (v->bits == 32) {
        entry[20] = (unsigned char)(cluster >> 16);
        entry[21] = (unsigned char)(cluster >> 24);
    }
    put32(entry + 28, 0);
    repair->allocated++;
    plan->cluster++;
    if (repair->report) {
        char *message = (char *)malloc(strlen(file->path) + 256);

        if (!message)
            return fail(repair->err, "Insufficient memory for shared-directory preview");
        sprintf(message,
                "%s: no unique prefix; replace with EMPTY directory at cluster %lu; "
                "first owner retains shared cluster %lu; new dots have unset timestamps",
                file->path,
                (unsigned long)cluster,
                (unsigned long)file->start);
        repair->report(repair->report_ctx, "DIR_REPLACE", v->start + file->sector, message);
        free(message);
    }
    return 1;
}

static int repair_directory(void *ctx, const DtFile *file)
{
    ChainRepair *repair = (ChainRepair *)ctx;
    DtPlan *plan = repair->plan;
    DtVolume *v = plan->volume;
    dt_u32 bytes = v->spc * 512UL;
    unsigned copy;

    if (!file->directory) {
        if (file->ending != DT_CHAIN_END ||
            file->clusters != file->size / bytes + (file->size % bytes != 0))
            repair->unsafe = 1;
        return 1;
    }
    if (file->ending == DT_CHAIN_END)
        return 1;
    if (!file->last) {
        /* Chain-only repair keeps its FAT-only journal contract. Replacing
           a directory entry and allocating space belongs to tree repair. */
        if (plan->kind == DT_REPAIR_TREE)
            return replace_shared_directory(repair, file);
        repair->unsafe = 1;
        return 1;
    }
    for (copy = 0; copy < v->copies; copy++) {
        dt_u32 end = v->bits == 12 ? 0xfff : v->bits == 16 ? 0xffff : 0x0fffffffUL;

        if (!v->mirrored && copy != v->active)
            continue;
        if (!dt_plan_set_fat(plan, file->last, end, copy, repair->err))
            return 0;
    }
    plan->cluster++;
    if (repair->report) {
        char *message = (char *)malloc(strlen(file->path) + 160);
        dt_u32 offset = v->bits == 12 ? file->last + file->last / 2 : file->last * (v->bits / 8);

        if (!message)
            return fail(repair->err, "Insufficient memory for directory-repair preview");
        sprintf(message,
                "%s: keep %lu directory clusters; terminate after cluster %lu; %s",
                *file->path ? file->path : "/",
                (unsigned long)file->clusters,
                (unsigned long)file->last,
                file->ending == DT_CHAIN_LOOP     ? "remove loop"
                : file->ending == DT_CHAIN_SHARED ? "detach shared tail; first owner retains it"
                                                  : "remove invalid ending");
        repair->report(repair->report_ctx,
                       "DIR_REPAIR",
                       v->start + v->fat + v->active * v->fatsz + offset / 512,
                       message);
        free(message);
    }
    return 1;
}

int dt_directory_plan_ex(DtVolume *v, DtPlan *plan, DtReport report, void *report_ctx,
                         DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    ChainRepair repair;

    err[0] = 0;
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_DIRECTORIES;
    repair.plan = plan;
    repair.err = err;
    repair.unsafe = 0;
    repair.report = report;
    repair.report_ctx = report_ctx;
    v->cache_sector = 0xffffffffUL;
    if (!scan_repairs(&repair, &result, repair_directory, chain_issue, progress, ctx)) {
        if (!*err)
            fail(err,
                 "Directory repair requires a complete scan; cancelled or unreadable metadata");
        return 0;
    }
    if (repair.unsafe)
        return fail(err,
                    "Directory repair refuses missing unique prefixes or other live-tree damage");
    return 1;
}

int dt_directory_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    return dt_directory_plan_ex(v, plan, NULL, NULL, progress, ctx, err);
}

static int repair_entry(void *ctx, const DtFile *file);

static void tree_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    if (strcmp(code, "DIR_DOT_METADATA") && strcmp(code, "DIR_DOT_MISSING") &&
        strcmp(code, "DIR_DOT_ORDER") && strcmp(code, "DIR_SIZE"))
        chain_issue(ctx, code, where, message);
}

static int repair_tree(void *ctx, const DtFile *file)
{
    ChainRepair *repair = (ChainRepair *)ctx;
    dt_u32 affected = repair->plan->cluster;
    DtFile retained;

    if (!file->directory)
        return repair_file(ctx, file);
    if (!repair_directory(ctx, file))
        return 0;
    if (!file->clusters)
        return 1;
    /* The directory planner has retained this unique prefix and terminated its
       damaged ending. Entry corrections refer only to that retained prefix. */
    retained = *file;
    retained.ending = DT_CHAIN_END;
    if (!repair_entry(ctx, &retained))
        return 0;
    if (repair->plan->cluster > affected)
        repair->plan->cluster = affected + 1;
    return 1;
}

int dt_tree_plan_ex(DtVolume *v, DtPlan *plan, DtReport report, void *report_ctx,
                    DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    ChainRepair repair;

    err[0] = 0;
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_TREE;
    repair.plan = plan;
    repair.err = err;
    repair.unsafe = 0;
    repair.report = report;
    repair.report_ctx = report_ctx;
    v->cache_sector = 0xffffffffUL;
    if (!scan_repairs(&repair, &result, repair_tree, tree_issue, progress, ctx)) {
        if (!*err)
            fail(err, "Combined repair requires a complete scan; cancelled or unreadable metadata");
        return 0;
    }
    if (repair.unsafe)
        return fail(
            err, "Combined repair refuses missing unique directory prefixes or other tree damage");
    return 1;
}

int dt_tree_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    return dt_tree_plan_ex(v, plan, NULL, NULL, progress, ctx, err);
}

int dt_tree_source_plan_ex(DtVolume *v, unsigned source, DtPlan *plan, DtReport report,
                           void *report_ctx, DtProgress progress, void *ctx, char *err)
{
    DtVolume candidate = *v;
    dt_u32 first, second, sector;
    unsigned copy, i;
    unsigned char data[512], target[512];
    int ok;

    dt_plan_clear(plan);
    if (!v->mirrored || v->copies < 2 || !source || source > v->copies)
        return fail(err, "Select an existing source FAT on a volume with mirroring enabled");
    candidate.active = source - 1;
    candidate.mirrored = 0;
    candidate.cache_sector = 0xffffffffUL;
    if (!dt_fat(&candidate, 0, &first) || !dt_fat(&candidate, 1, &second))
        return fail(err, "Cannot read the selected source FAT");
    if (first != ((v->bits == 12   ? 0xf00UL
                   : v->bits == 16 ? 0xff00UL
                                   : 0x0fffff00UL) |
                  v->boot[21]) ||
        (v->bits == 12   ? second < 0xff8UL
         : v->bits == 16 ? (second & 0x3fffUL) != 0x3fffUL
                         : (second & 0x03ffffffUL) != 0x03ffffffUL))
        return fail(err, "Selected source FAT has invalid reserved entries or media descriptor");
    if (report) {
        char message[160];

        sprintf(message,
                "Use FAT %u as authority; its repaired allocation replaces all copies. "
                "This does not prove which copy is newest.",
                source);
        report(report_ctx, "FAT_SOURCE", v->start + v->fat + (source - 1) * v->fatsz, message);
    }
    ok = dt_tree_plan_ex(&candidate, plan, report, report_ctx, progress, ctx, err);
    /* Do not retain the temporary candidate in either successful or failed plans. */
    plan->volume = v;
    if (!ok)
        return 0;
    plan->kind = DT_REPAIR_TREE_SOURCE;
    plan->mark_bad = (int)source;
    for (sector = 0; sector < v->fatsz; sector++) {
        dt_u32 source_sector = v->fat + (source - 1) * v->fatsz + sector;

        if (progress && !progress(ctx, "Prepare repaired FAT copies", sector, v->fatsz))
            return fail(err, "Combined FAT/tree repair cancelled; no writes made");
        if (!dt_read(v, source_sector, data))
            return fail(err, "Cannot read source FAT sector");
        /* Mirror the corrected source, including changes already in this plan. */
        for (i = 0; i < plan->count; i++) {
            if (plan->patch[i].sector == source_sector) {
                memcpy(data, plan->patch[i].after, 512);
                break;
            }
        }
        for (copy = 0; copy < v->copies; copy++) {
            DtPatch *patch;
            dt_u32 destination = v->fat + copy * v->fatsz + sector;

            if (copy == source - 1)
                continue;
            if (!dt_read(v, destination, target))
                return fail(err, "Cannot read destination FAT sector");
            if (!memcmp(data, target, 512))
                continue;
            patch = dt_plan_sector(plan, destination, err);
            if (!patch)
                return 0;
            memcpy(patch->after, data, 512);
        }
    }
    if (v->bits == 32 && v->fsinfo && v->fsinfo != 0xffff) {
        unsigned char rebuilt[512];
        dt_u32 cluster, value, free_count = 0, first_free = 0xffffffffUL;
        dt_u32 sectors[2];
        unsigned count = 1;

        if (v->fsinfo >= v->fat || v->fsinfo == v->backup)
            return fail(err, "FSInfo overlaps boot metadata or lies outside reserved sectors");
        sectors[0] = v->fsinfo;
        if (v->backup && v->backup != 0xffff) {
            if (v->backup + v->fsinfo >= v->fat)
                return fail(err, "Backup FSInfo lies outside reserved sectors");
            sectors[count++] = v->backup + v->fsinfo;
        }
        /* Include directory growth in the selected source's allocation count. */
        candidate.cache_sector = 0xffffffffUL;
        for (cluster = 2; cluster < v->clusters + 2; cluster++) {
            if (progress && !(cluster & 31) &&
                !progress(ctx, "Recount selected FAT", cluster - 2, v->clusters))
                return fail(err, "Selected FAT recount cancelled; no writes made");
            if (!dt_fat(&candidate, cluster, &value))
                return fail(err, "Cannot recount selected FAT");
            for (i = 0; i < plan->count; i++) {
                dt_u32 sector = candidate.fat + candidate.active * candidate.fatsz + cluster / 128;

                if (plan->patch[i].sector == sector) {
                    value = dt_dword(plan->patch[i].after + (cluster % 128) * 4) & 0x0fffffffUL;
                    break;
                }
            }
            if (!value) {
                free_count++;
                if (first_free == 0xffffffffUL)
                    first_free = cluster;
            }
        }
        memset(rebuilt, 0, sizeof(rebuilt));
        put32(rebuilt, 0x41615252UL);
        put32(rebuilt + 484, 0x61417272UL);
        put32(rebuilt + 488, free_count);
        put32(rebuilt + 492, first_free);
        put32(rebuilt + 508, 0xaa550000UL);
        for (i = 0; i < count; i++) {
            DtPatch *patch = dt_plan_sector(plan, sectors[i], err);

            if (!patch)
                return 0;
            memcpy(patch->after, rebuilt, 512);
            if (!memcmp(patch->before, patch->after, 512)) {
                unsigned index = (unsigned)(patch - plan->patch);

                memmove(patch, patch + 1, (plan->count - index - 1) * sizeof(*patch));
                plan->count--;
            }
        }
    }
    return 1;
}

int dt_tree_source_plan(DtVolume *v, unsigned source, DtPlan *plan, DtProgress progress, void *ctx,
                        char *err)
{
    return dt_tree_source_plan_ex(v, source, plan, NULL, NULL, progress, ctx, err);
}

static void primary_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    if (strcmp(code, "BOOT_PRIMARY"))
        metadata_issue(ctx, code, where, message);
}

int dt_boot_primary_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    DtPatch *patch;
    unsigned char backup[512];
    dt_u32 first, second;
    int unsafe = 0;

    dt_plan_clear(plan);
    if (v->bits != 32 || !v->boot_source || v->boot_source != v->backup)
        return fail(err, "Mount through the selected FAT32 backup before primary boot recovery");
    if (!dt_read(v, v->boot_source, backup) || memcmp(backup, v->boot, 512))
        return fail(err, "Recovery backup changed or cannot be read; select it again");
    v->cache_sector = 0xffffffffUL;
    if (!dt_fat(v, 0, &first) || !dt_fat(v, 1, &second) || first != (0x0fffff00UL | v->boot[21]) ||
        (second & 0x03ffffffUL) != 0x03ffffffUL)
        return fail(err, "Recovery geometry does not identify valid FAT32 reserved entries");
    if (!dt_scan_ex(v, &result, primary_issue, &unsafe, progress, ctx) || unsafe)
        return fail(err,
                    "Recovery backup must produce a complete, consistent file and directory scan");
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_BOOT_PRIMARY;
    plan->cluster = v->boot_source;
    patch = dt_plan_sector(plan, 0, err);
    if (!patch)
        return 0;
    memcpy(plan->volume_boot, patch->before, 512);
    memcpy(patch->after, backup, 512);
    if (!memcmp(patch->before, patch->after, 512))
        plan->count = 0;
    return 1;
}

static void entry_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    ChainRepair *repair = (ChainRepair *)ctx;

    if (strcmp(code, "DIR_DOT_METADATA") && strcmp(code, "DIR_DOT_MISSING") &&
        strcmp(code, "DIR_DOT_ORDER") && strcmp(code, "DIR_SIZE"))
        lost_issue(&repair->unsafe, code, where, message);
}

/* Relocate a leading LFN group only as one contiguous block, including its
   short entry. The scanner still validates sequence, checksum and name. */
static int move_dot_lfn(unsigned char *data, unsigned slots, unsigned *origins, unsigned *moved)
{
    unsigned first, last, count, destination, i, j, pass;

    *moved = 0;
    for (first = 0; first < 2; first++) {
        if (data[first * 32] && data[first * 32] != 0xe5 && data[first * 32 + 11] == 15)
            break;
    }
    if (first == 2)
        return 1;
    for (last = first; last < slots && data[last * 32 + 11] == 15; last++) {
        if (!data[last * 32] || data[last * 32] == 0xe5)
            return 0;
    }
    if (last == slots || !data[last * 32] || data[last * 32] == 0xe5 ||
        (data[last * 32 + 11] & 0x08))
        return 0;
    count = last - first + 1;
    /* Prefer separate vacant slots. If none fit, shift the whole group using
       its own source slots as space; no live record outside it may move. */
    for (pass = 0; pass < 2; pass++) {
        for (destination = pass ? 2 : last + 1; destination + count <= slots; destination++) {
            for (i = destination; i < destination + count; i++) {
                if (pass && i >= first && i <= last)
                    continue;
                if (!data[i * 32]) {
                    /* An end marker must survive without exposing hidden entries. */
                    if (destination + count == slots)
                        break;
                    for (j = i + 1; j < slots; j++) {
                        if (data[j * 32])
                            return 0;
                    }
                } else if (data[i * 32] != 0xe5) {
                    break;
                }
            }
            if (i == destination + count) {
                memmove(data + destination * 32, data + first * 32, count * 32);
                if (origins)
                    memmove(origins + destination, origins + first, count * sizeof(*origins));
                for (i = first; i <= last; i++) {
                    if (i < destination || i >= destination + count) {
                        data[i * 32] = 0xe5;
                        if (origins)
                            origins[i] = (unsigned)-1;
                    }
                }
                *moved = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* Short entries may use separate vacant slots after a leading LFN group has
   moved intact. Child metadata corrections resolve their new positions below. */
static int free_dot_slots(unsigned char *data, unsigned slots, unsigned *origins,
                          unsigned *relocated)
{
    unsigned source[2], destination[2], count = 0, found = 0, i, j, named;

    *relocated = 0;
    if (!move_dot_lfn(data, slots, origins, &named))
        return 0;
    for (i = 0; i < 2; i++) {
        const unsigned char *entry = data + i * 32;

        if (!entry[0] || entry[0] == 0xe5 || !memcmp(entry, ".          ", 11) ||
            !memcmp(entry, "..         ", 11))
            continue;
        if (entry[11] & 0x08)
            return 0;
        source[count++] = i;
    }
    if (!count) {
        *relocated = named;
        return 1;
    }
    for (i = 2; i < slots && found < count; i++) {
        if (!data[i * 32]) {
            if (i + 1 == slots)
                break;
            /* Keep an end marker in the window and expose no hidden entries. */
            for (j = i + 1; j < slots; j++) {
                if (data[j * 32])
                    return 0;
            }
        }
        if (!data[i * 32] || data[i * 32] == 0xe5)
            destination[found++] = i;
    }
    if (found != count)
        return 0;
    for (i = 0; i < count; i++) {
        memcpy(data + destination[i] * 32, data + source[i] * 32, 32);
        data[source[i] * 32] = 0xe5;
        if (origins) {
            origins[destination[i]] = origins[source[i]];
            origins[source[i]] = (unsigned)-1;
        }
    }
    *relocated = count + named;
    return 1;
}

/* Expand only after the first-sector attempt fails. The scanner has already
   identified this directory's retained unique chain, including fragmented
   clusters. Full directories may gain free unowned clusters; every changed
   sector, including original free-space bytes, is journaled. */
static int relocate_directory(ChainRepair *repair, const DtFile *file, unsigned char *prefix,
                              unsigned *relocated)
{
    DtVolume *v = repair->plan->volume;
    dt_u64 bytes = (dt_u64)file->clusters * v->spc * 512;
    dt_u64 capacity = bytes + (dt_u64)v->spc * 1024;
    unsigned char *before = NULL, *after = NULL;
    dt_u32 *sectors = NULL, cluster = file->start, n, added[2];
    unsigned *origins = NULL, count, slots, i, j, index = 0, growth = 0, original_count;
    int ok = 0;

    if (capacity > (unsigned)-1 || capacity > (size_t)-1)
        return fail(repair->err, "Directory is too large to prepare relocation in memory");
    count = (unsigned)(bytes / 512);
    original_count = count;
    slots = count * 16;
    before = (unsigned char *)malloc((size_t)capacity);
    after = (unsigned char *)malloc((size_t)capacity);
    sectors = (dt_u32 *)malloc((size_t)(capacity / 512) * sizeof(*sectors));
    origins = (unsigned *)malloc((size_t)(capacity / 32) * sizeof(*origins));
    if (!before || !after || !sectors || !origins) {
        fail(repair->err, "Insufficient memory to prepare directory relocation");
        goto done;
    }
    for (n = 0; n < file->clusters; n++) {
        if (cluster < 2 || cluster >= v->clusters + 2) {
            fail(repair->err, "Directory chain changed while preparing relocation");
            goto done;
        }
        for (j = 0; j < v->spc; j++, index++) {
            if (repair->progress &&
                !repair->progress(
                    repair->progress_ctx, "Prepare directory relocation", index, count)) {
                fail(repair->err, "Directory relocation cancelled; no writes made");
                goto done;
            }
            sectors[index] = v->data + (cluster - 2) * v->spc + j;
            if (!dt_read(v, sectors[index], before + index * 512)) {
                fail(repair->err, "Cannot read directory while preparing relocation");
                goto done;
            }
        }
        if (n + 1 < file->clusters && !dt_fat(v, cluster, &cluster)) {
            fail(repair->err, "Cannot read directory chain while preparing relocation");
            goto done;
        }
    }
    for (;;) {
        memcpy(after, before, (size_t)bytes);
        memset(after + (size_t)bytes, 0, (count - original_count) * 512);
        slots = count * 16;
        for (i = 0; i < slots; i++)
            origins[i] = i < original_count * 16 ? i : (unsigned)-1;
        if (free_dot_slots(after, slots, origins, relocated))
            break;
        /* Two minimum-size clusters can hold a maximum-length LFN group,
           another leading entry, and an end marker. Invalid layouts still fail. */
        if (growth == 2 || repair->unsafe) {
            repair->unsafe = 1;
            ok = 1;
            goto done;
        }
        if (!directory_space(repair, &added[growth]))
            goto done;
        for (j = 0; j < v->spc; j++) {
            sectors[count] = v->data + (added[growth] - 2) * v->spc + j;
            if (!dt_read(v, sectors[count], before + count * 512)) {
                fail(repair->err, "Cannot back up free space for directory growth");
                goto done;
            }
            count++;
        }
        growth++;
    }
    for (i = 0; i < slots; i++) {
        unsigned source = origins[i];
        EntryMove *move;

        if (source == (unsigned)-1 || source == i || before[source * 32 + 11] == 15)
            continue;
        move = (EntryMove *)malloc(sizeof(*move));
        if (!move) {
            fail(repair->err, "Insufficient memory to track relocated directory entries");
            goto done;
        }
        move->source = sectors[source / 16];
        move->source_offset = (source % 16) * 32;
        move->destination = sectors[i / 16];
        move->destination_offset = (i % 16) * 32;
        move->next = repair->moves;
        repair->moves = move;
    }
    for (i = 0; i < count; i++) {
        DtPatch *patch;

        if (i < original_count && !memcmp(before + i * 512, after + i * 512, 512))
            continue;
        patch = dt_plan_sector(repair->plan, sectors[i], repair->err);
        if (!patch)
            goto done;
        for (j = 0; j < 512; j++) {
            if (before[i * 512 + j] != after[i * 512 + j])
                patch->after[j] = after[i * 512 + j];
        }
    }
    if (growth) {
        unsigned copy;
        dt_u32 previous = file->last;
        dt_u32 end = v->bits == 12 ? 0xfff : v->bits == 16 ? 0xffff : 0x0fffffffUL;

        for (i = 0; i <= growth; i++) {
            dt_u32 value = i < growth ? added[i] : end;

            for (copy = 0; copy < v->copies; copy++) {
                if (!v->mirrored && copy != v->active)
                    continue;
                if (!dt_plan_set_fat(repair->plan, previous, value, copy, repair->err))
                    goto done;
            }
            if (i < growth)
                previous = added[i];
        }
        repair->allocated += growth;
        if (repair->report) {
            char message[120];

            sprintf(
                message,
                "Append %u free cluster%s to directory at cluster %lu; original data is journaled",
                growth,
                growth == 1 ? "" : "s",
                (unsigned long)file->start);
            repair->report(repair->report_ctx, "DIR_GROW", file->start, message);
        }
    }
    memcpy(prefix, after, 512);
    ok = 1;
done:
    free(before);
    free(after);
    free(sectors);
    free(origins);
    return ok;
}

static int repair_entry(void *ctx, const DtFile *file)
{
    ChainRepair *repair = (ChainRepair *)ctx;
    DtPlan *plan = repair->plan;
    DtVolume *v = plan->volume;
    unsigned char data[512], original[512], rebuilt[64];
    dt_u32 sector;
    unsigned i, created = 0, relocated = 0;
    int dots_changed, kind[2], moved = 0, duplicate;
    DtPatch *patch;

    if (file->ending != DT_CHAIN_END) {
        repair->unsafe = 1;
        return 1;
    }
    if (file->directory != 1)
        return 1;
    if (!file->clusters || file->start < 2 || file->start >= v->clusters + 2) {
        repair->unsafe = 1;
        return 1;
    }
    sector = v->data + (file->start - 2) * v->spc;
    if (!dt_read(v, sector, data))
        return fail(repair->err, "Cannot read directory prefix for entry repair");
    /* Advancing an end marker must not expose previously hidden entries.
       Leave a preexisting end marker immediately after the rebuilt prefix. */
    if ((!data[0] && data[32] != 0 && data[32] != 0xe5) || ((!data[0] || !data[32]) && data[64])) {
        repair->unsafe = 1;
        return 1;
    }
    memcpy(original, data, sizeof(original));
    if (!free_dot_slots(data, 16, NULL, &relocated)) {
        if (!relocate_directory(repair, file, data, &relocated))
            return 0;
        if (repair->unsafe)
            return 1;
    }
    for (i = 0; i < 2; i++) {
        const unsigned char *entry = data + i * 32;

        if (!entry[0] || entry[0] == 0xe5)
            kind[i] = 0;
        else if (!memcmp(entry, ".          ", 11))
            kind[i] = 1;
        else if (!memcmp(entry, "..         ", 11))
            kind[i] = 2;
        else {
            repair->unsafe = 1;
            return 1;
        }
    }
    duplicate = kind[0] && kind[0] == kind[1];
    for (i = 0; i < 2; i++) {
        unsigned char *entry = rebuilt + i * 32;
        dt_u32 cluster = i ? file->parent : file->start;
        const char *name = i ? "..         " : ".          ";
        unsigned source = kind[i] == (int)i + 1 ? i : kind[1 - i] == (int)i + 1 ? 1 - i : 2;

        if (source < 2) {
            /* Prefer the correctly placed entry when duplicates exist. Move
               complete entries so timestamps and reserved fields follow names. */
            memcpy(entry, data + source * 32, 32);
            moved |= source != i;
        } else {
            /* Missing metadata cannot inherit a duplicate's or deleted slot's dates. */
            memset(entry, 0, 32);
            memcpy(entry, name, 11);
            created++;
        }
        entry[11] = (entry[11] & ~0xc8) | 0x10;
        entry[26] = (unsigned char)cluster;
        entry[27] = (unsigned char)(cluster >> 8);
        if (v->bits == 32) {
            entry[20] = (unsigned char)(cluster >> 16);
            entry[21] = (unsigned char)(cluster >> 24);
        }
        put32(entry + 28, 0);
    }
    dots_changed = memcmp(data, rebuilt, sizeof(rebuilt)) != 0;
    if (!dots_changed && !file->size)
        return 1;
    if (dots_changed) {
        patch = dt_plan_sector(plan, sector, repair->err);
        if (!patch)
            return 0;
        memcpy(data, rebuilt, sizeof(rebuilt));
        for (i = 0; i < sizeof(data); i++) {
            if (data[i] != original[i])
                patch->after[i] = data[i];
        }
    }
    if (file->size) {
        unsigned char *entry = planned_entry(repair, file);

        if (!entry)
            return 0;
        put32(entry + 28, 0);
    }
    plan->cluster++;
    if (repair->report) {
        char *message = (char *)malloc(strlen(file->path) + 320);

        if (!message)
            return fail(repair->err, "Insufficient memory for directory-entry preview");
        sprintf(message,
                "%s: entry byte %u; directory size %lu -> 0; %s; "
                "dot cluster %lu, parent %lu (prefix LBA %lu); rebuild %u missing dot entr%s%s; "
                "relocate %u entries within directory (LFN groups intact)",
                file->path,
                file->offset,
                (unsigned long)file->size,
                duplicate      ? "replace duplicate dot entry and correct metadata"
                : moved        ? "restore dot/dotdot order and correct metadata"
                : dots_changed ? "correct dot pointers/attributes/sizes"
                               : "dot entries unchanged",
                (unsigned long)file->start,
                (unsigned long)file->parent,
                (unsigned long)(v->start + sector),
                created,
                created == 1 ? "y" : "ies",
                created ? " with unset timestamps" : "",
                relocated);
        repair->report(repair->report_ctx, "ENTRY_REPAIR", v->start + file->sector, message);
        free(message);
    }
    return 1;
}

int dt_entry_plan_ex(DtVolume *v, DtPlan *plan, DtReport report, void *report_ctx,
                     DtProgress progress, void *ctx, char *err)
{
    DtResult result;
    ChainRepair repair;

    err[0] = 0;
    if (!dt_plan_init(plan, v, err))
        return 0;
    plan->kind = DT_REPAIR_ENTRIES;
    repair.plan = plan;
    repair.err = err;
    repair.unsafe = 0;
    repair.report = report;
    repair.report_ctx = report_ctx;
    v->cache_sector = 0xffffffffUL;
    if (!scan_repairs(&repair, &result, repair_entry, entry_issue, progress, ctx)) {
        if (!*err)
            fail(err, "Entry repair requires a complete scan; cancelled or unreadable metadata");
        return 0;
    }
    if (repair.unsafe)
        return fail(err,
                    "Entry repair refuses occupied dot slots, hidden entries, ambiguous owners or "
                    "other tree damage");
    return 1;
}

int dt_entry_plan(DtVolume *v, DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    return dt_entry_plan_ex(v, plan, NULL, NULL, progress, ctx, err);
}
