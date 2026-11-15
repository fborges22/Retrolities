/* Immutable sector journals: persist before writing, verify, and recover. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>

#define JOURNAL_HEADER 64
#define JOURNAL_RECORD 1032

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

DtPlan *dt_plan_create(void)
{
    return (DtPlan *)calloc(1, sizeof(DtPlan));
}

void dt_plan_clear(DtPlan *plan)
{
    free(plan->patch);
    memset(plan, 0, sizeof(*plan));
}

void dt_plan_free(DtPlan *plan)
{
    if (plan) {
        dt_plan_clear(plan);
        free(plan);
    }
}

static int reserve_patches(DtPlan *plan, unsigned count, char *err)
{
    DtPatch *patch;

    if (count <= plan->capacity)
        return 1;
    if ((dt_u64)count * sizeof(*patch) > (size_t)-1)
        return fail(err, "Repair plan exceeds addressable memory");
    patch = (DtPatch *)realloc(plan->patch, (size_t)count * sizeof(*patch));
    if (!patch)
        return fail(err, "Insufficient memory for metadata repair sectors");
    plan->patch = patch;
    plan->capacity = count;
    return 1;
}

int dt_plan_init(DtPlan *plan, DtVolume *v, char *err)
{
    dt_plan_clear(plan);
    plan->volume = v;
    plan->disk_sectors = v->disk->sectors;
    plan->start = v->start;
    plan->total = v->total;
    memcpy(plan->volume_boot, v->boot, 512);
    if (!v->disk->read(v->disk->ctx, 0, plan->disk_boot))
        return fail(err, "Cannot read disk identity");
    return 1;
}

static dt_u32 sector_limit(const DtPlan *plan)
{
    return (plan->kind == DT_REPAIR_CHAINS || plan->kind == DT_REPAIR_TREE ||
            plan->kind == DT_REPAIR_TREE_SOURCE || plan->kind == DT_REPAIR_ENTRIES ||
            plan->kind == DT_REPAIR_RELOCATE)
               ? plan->volume->total
               : plan->volume->root;
}

DtPatch *dt_plan_sector(DtPlan *plan, dt_u32 sector, char *err)
{
    unsigned i;
    DtPatch *patch;
    dt_u32 limit = sector_limit(plan);

    for (i = 0; i < plan->count; i++)
        if (plan->patch[i].sector == sector)
            return &plan->patch[i];
    if ((!sector && plan->kind != DT_REPAIR_BOOT_PRIMARY) || sector >= limit ||
        plan->count >= limit - 1) {
        fail(err, "Repair plan exceeds sector bounds");
        return NULL;
    }
    if (plan->count == plan->capacity) {
        unsigned capacity = plan->capacity ? plan->capacity * 2 : 8;

        if (capacity < plan->capacity || capacity >= limit)
            capacity = limit - 1;
        if (!reserve_patches(plan, capacity, err))
            return NULL;
    }
    patch = &plan->patch[plan->count];
    if (!dt_read(plan->volume, sector, patch->before)) {
        fail(err, "Cannot back up a metadata sector");
        return NULL;
    }
    patch->sector = sector;
    memcpy(patch->after, patch->before, 512);
    plan->count++;
    return patch;
}

int dt_plan_set_fat(DtPlan *plan, dt_u32 cluster, dt_u32 value, unsigned copy, char *err)
{
    DtVolume *v = plan->volume;
    dt_u32 offset = v->bits == 12 ? cluster + cluster / 2 : cluster * (v->bits / 8);
    unsigned i, bytes = v->bits == 32 ? 4 : 2;
    dt_u32 old = 0;
    DtPatch *patch;

    for (i = 0; i < bytes; i++) {
        patch = dt_plan_sector(plan, v->fat + copy * v->fatsz + (offset + i) / 512, err);
        if (!patch)
            return 0;
        old |= (dt_u32)patch->after[(offset + i) % 512] << (8 * i);
    }
    /* FAT12 shares nibbles; FAT32 reserves the upper four bits. */
    if (v->bits == 12)
        value = cluster & 1 ? (old & 15) | (value << 4) : (old & 0xf000) | value;
    else if (v->bits == 32)
        value |= old & 0xf0000000UL;
    /* Reacquire after all additions: growing the plan may move its array. */
    for (i = 0; i < bytes; i++) {
        patch = dt_plan_sector(plan, v->fat + copy * v->fatsz + (offset + i) / 512, err);
        if (!patch)
            return 0;
        patch->after[(offset + i) % 512] = (unsigned char)(value >> (8 * i));
    }
    return 1;
}

static void put32(unsigned char *p, dt_u32 n)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (8 * i));
}

static dt_u32 crc32(dt_u32 crc, const unsigned char *bytes, size_t size)
{
    unsigned bit;
    crc = ~crc;
    while (size--) {
        crc ^= *bytes++;
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320UL & (0 - (crc & 1)));
    }
    return ~crc;
}

static int identity(DtPlan *plan, char *err)
{
    unsigned char buffer[512];
    DtVolume *v = plan->volume;
    int primary = plan->kind == DT_REPAIR_BOOT_PRIMARY;

    if (v->disk->sectors != plan->disk_sectors || v->start != plan->start ||
        v->total != plan->total)
        return fail(err, "Journal/plan belongs to a different disk extent");
    if (primary &&
        (plan->count != 1 || plan->patch[0].sector || !plan->cluster ||
         plan->cluster != v->backup || v->bits != 32 ||
         memcmp(plan->volume_boot, plan->patch[0].before, 512) ||
         !dt_read(v, plan->cluster, buffer) || memcmp(buffer, plan->patch[0].after, 512)))
        return fail(err, "Primary recovery source changed or the journal is inconsistent");
    if (!v->disk->read(v->disk->ctx, 0, buffer) ||
        (memcmp(buffer, plan->disk_boot, 512) &&
         !(primary && !v->start && !memcmp(buffer, plan->patch[0].after, 512))))
        return fail(err, "Disk identity changed; no sectors written");
    if (!dt_read(v, 0, buffer) || (memcmp(buffer, plan->volume_boot, 512) &&
                                   !(primary && !memcmp(buffer, plan->patch[0].after, 512))))
        return fail(err, "Volume identity changed; no sectors written");
    return 1;
}

static int save_journal(DtPlan *plan, const char *path, DtWriteProgress writing, void *ctx,
                        char *err)
{
    unsigned char header[JOURNAL_HEADER], record[JOURNAL_RECORD];
    unsigned i;
    dt_u32 crc;
    FILE *file;
    int ok;

    if (writing)
        writing(ctx, "Save journal", 0, plan->count);
    file = dt_file_create(path);
    if (!file)
        return fail(err, "Cannot create journal; choose a new filename on a separate disk");
    memset(header, 0, sizeof(header));
    memcpy(header, "MDJNL001", 8);
    put32(header + 8,
          plan->kind == DT_REPAIR_RELOCATE       ? 11
          : plan->kind == DT_REPAIR_ENTRIES      ? 10
          : plan->kind == DT_REPAIR_TREE_SOURCE  ? 9
          : plan->kind == DT_REPAIR_TREE         ? 8
          : plan->kind == DT_REPAIR_BOOT_PRIMARY ? 7
          : plan->kind == DT_REPAIR_DIRECTORIES  ? 6
          : plan->kind == DT_REPAIR_CHAINS       ? 5
          : plan->kind == DT_REPAIR_LOST         ? 4
          : plan->kind == DT_REPAIR_MIRROR       ? 3
          : plan->kind == DT_REPAIR_BAD          ? 1
                                                 : 2);
    put32(header + 12, plan->count);
    put32(header + 16, plan->disk_sectors);
    put32(header + 20, plan->start);
    put32(header + 24, plan->total);
    put32(header + 28, plan->cluster);
    put32(header + 32, (dt_u32)plan->mark_bad);
    put32(header + 36, (dt_u32)plan->kind);
    crc = crc32(0, header, 60);
    crc = crc32(crc, plan->disk_boot, 512);
    crc = crc32(crc, plan->volume_boot, 512);
    put32(header + 60, crc);
    ok = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
         fwrite(plan->disk_boot, 1, 512, file) == 512 &&
         fwrite(plan->volume_boot, 1, 512, file) == 512;
    for (i = 0; ok && i < plan->count; i++) {
        put32(record, plan->patch[i].sector);
        memcpy(record + 4, plan->patch[i].before, 512);
        memcpy(record + 516, plan->patch[i].after, 512);
        put32(record + 1028, crc32(0, record, 1028));
        ok = fwrite(record, 1, sizeof(record), file) == sizeof(record);
        if (ok && writing && i + 1 < plan->count)
            writing(ctx, "Save journal", i + 1, plan->count);
    }
    if (ok)
        ok = dt_file_commit(file);
    if (fclose(file))
        ok = 0;
    if (ok)
        ok = dt_file_commit_directory(path);
    if (ok && writing)
        writing(ctx, "Save journal", plan->count, plan->count);
    return ok ? 1 : fail(err, "Journal could not be committed; no sectors written");
}

static int load_journal(DtVolume *v, const char *path, DtPlan *plan, DtProgress progress, void *ctx,
                        char *err)
{
    FILE *file = fopen(path, "rb");
    unsigned char header[JOURNAL_HEADER], record[JOURNAL_RECORD];
    unsigned i, j;
    dt_u32 crc, count, version, kind;
    long length;
    int ok;

    if (!file)
        return fail(err, "Cannot open journal");
    dt_plan_clear(plan);
    plan->volume = v;
    ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
         fread(plan->disk_boot, 1, 512, file) == 512 &&
         fread(plan->volume_boot, 1, 512, file) == 512;
    if (!ok || memcmp(header, "MDJNL001", 8))
        goto invalid;
    crc = crc32(0, header, 60);
    crc = crc32(crc, plan->disk_boot, 512);
    crc = crc32(crc, plan->volume_boot, 512);
    if (crc != dt_dword(header + 60))
        goto invalid;
    version = dt_dword(header + 8);
    kind = dt_dword(header + 36);
    if ((version < 1 || version > 11) || kind > DT_REPAIR_RELOCATE ||
        (version == 1 && kind != DT_REPAIR_BAD) ||
        (version == 3 ? kind != DT_REPAIR_MIRROR : kind == DT_REPAIR_MIRROR) ||
        (version == 4 ? kind != DT_REPAIR_LOST : kind == DT_REPAIR_LOST) ||
        (version == 5 ? kind != DT_REPAIR_CHAINS : kind == DT_REPAIR_CHAINS) ||
        (version == 6 ? kind != DT_REPAIR_DIRECTORIES : kind == DT_REPAIR_DIRECTORIES) ||
        (version == 7 ? kind != DT_REPAIR_BOOT_PRIMARY : kind == DT_REPAIR_BOOT_PRIMARY) ||
        (version == 8 ? kind != DT_REPAIR_TREE : kind == DT_REPAIR_TREE) ||
        (version == 9 ? kind != DT_REPAIR_TREE_SOURCE : kind == DT_REPAIR_TREE_SOURCE) ||
        (version == 10 ? kind != DT_REPAIR_ENTRIES : kind == DT_REPAIR_ENTRIES) ||
        (version == 11 ? kind != DT_REPAIR_RELOCATE : kind == DT_REPAIR_RELOCATE))
        goto invalid;
    plan->kind = (int)kind;
    count = dt_dword(header + 12);
    if (!count || count >= sector_limit(plan) ||
        (kind != DT_REPAIR_TREE_SOURCE && dt_dword(header + 32) > 1))
        goto invalid;
    /* Check the file extent before trusting an untrusted allocation count. */
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (dt_u64)length != JOURNAL_HEADER + 1024 + (dt_u64)count * JOURNAL_RECORD ||
        fseek(file, JOURNAL_HEADER + 1024, SEEK_SET))
        goto invalid;
    if (!reserve_patches(plan, (unsigned)count, err)) {
        fclose(file);
        return 0;
    }
    plan->count = (unsigned)count;
    plan->disk_sectors = dt_dword(header + 16);
    plan->start = dt_dword(header + 20);
    plan->total = dt_dword(header + 24);
    plan->cluster = dt_dword(header + 28);
    plan->mark_bad = (int)dt_dword(header + 32);
    if (kind == DT_REPAIR_RELOCATE &&
        (plan->cluster < 2 || plan->cluster >= v->clusters + 2 || plan->mark_bad))
        goto invalid;
    if (kind == DT_REPAIR_FSINFO &&
        (v->bits != 32 || plan->cluster || plan->mark_bad || count > 2 || !v->fsinfo ||
         v->fsinfo == 0xffff || v->fsinfo == v->backup))
        goto invalid;
    if (kind == DT_REPAIR_BOOT_BACKUP &&
        (v->bits != 32 || plan->cluster || plan->mark_bad || count != 1 || !v->backup ||
         v->backup == 0xffff || v->backup >= v->fat || v->backup == v->fsinfo))
        goto invalid;
    if (kind == DT_REPAIR_MIRROR && (!v->mirrored || v->copies < 2 || !plan->cluster ||
                                     plan->cluster > v->copies || plan->mark_bad))
        goto invalid;
    if (kind == DT_REPAIR_LOST && (!plan->cluster || plan->cluster > v->clusters || plan->mark_bad))
        goto invalid;
    if ((kind == DT_REPAIR_CHAINS || kind == DT_REPAIR_DIRECTORIES || kind == DT_REPAIR_TREE ||
         kind == DT_REPAIR_ENTRIES) &&
        (!plan->cluster || plan->mark_bad))
        goto invalid;
    if (kind == DT_REPAIR_BOOT_PRIMARY &&
        (v->bits != 32 || count != 1 || plan->mark_bad || !plan->cluster ||
         plan->cluster != v->backup || plan->cluster >= v->fat))
        goto invalid;
    if (kind == DT_REPAIR_TREE_SOURCE && (!v->mirrored || v->copies < 2 || plan->mark_bad < 1 ||
                                          (unsigned)plan->mark_bad > v->copies))
        goto invalid;
    for (i = 0; i < plan->count; i++) {
        DtPatch *patch = &plan->patch[i];

        if (progress && !progress(ctx, "Read journal", i, plan->count))
            goto cancelled;
        if (fread(record, 1, sizeof(record), file) != sizeof(record) ||
            crc32(0, record, 1028) != dt_dword(record + 1028))
            goto invalid;
        patch->sector = dt_dword(record);
        /* File repairs may include directory sectors; boot-sector writes are excluded. */
        if ((!patch->sector && kind != DT_REPAIR_BOOT_PRIMARY) ||
            patch->sector >= sector_limit(plan))
            goto invalid;
        if (kind == DT_REPAIR_BOOT_PRIMARY &&
            (patch->sector || memcmp(record + 4, plan->volume_boot, 512) ||
             memcmp(record + 516, v->boot, 512) ||
             (!v->start && memcmp(plan->disk_boot, plan->volume_boot, 512))))
            goto invalid;
        if (kind == DT_REPAIR_FSINFO &&
            (patch->sector >= v->fat ||
             (patch->sector != v->fsinfo &&
              (!v->backup || v->backup == 0xffff || patch->sector != v->backup + v->fsinfo))))
            goto invalid;
        if (kind == DT_REPAIR_BOOT_BACKUP &&
            (patch->sector != v->backup || memcmp(record + 516, plan->volume_boot, 512)))
            goto invalid;
        if (kind == DT_REPAIR_MIRROR) {
            if (patch->sector >= v->fat) {
                unsigned char source[512];
                dt_u32 relative = patch->sector - v->fat;

                if (relative / v->fatsz == plan->cluster - 1 ||
                    !dt_read(
                        v, v->fat + (plan->cluster - 1) * v->fatsz + relative % v->fatsz, source) ||
                    memcmp(source, record + 516, 512))
                    goto invalid;
            } else if (v->bits != 32 || !v->fsinfo || v->fsinfo == 0xffff ||
                       v->fsinfo == v->backup ||
                       (patch->sector != v->fsinfo && (!v->backup || v->backup == 0xffff ||
                                                       patch->sector != v->backup + v->fsinfo)))
                goto invalid;
        }
        if (kind == DT_REPAIR_LOST) {
            if (patch->sector >= v->fat) {
                if (!v->mirrored && (patch->sector - v->fat) / v->fatsz != v->active)
                    goto invalid;
            } else if (v->bits != 32 || !v->fsinfo || v->fsinfo == 0xffff ||
                       (patch->sector != v->fsinfo && (!v->backup || v->backup == 0xffff ||
                                                       patch->sector != v->backup + v->fsinfo)))
                goto invalid;
        }
        if (kind == DT_REPAIR_DIRECTORIES &&
            (patch->sector < v->fat ||
             (!v->mirrored && (patch->sector - v->fat) / v->fatsz != v->active)))
            goto invalid;
        for (j = 0; j < i; j++)
            if (plan->patch[j].sector == patch->sector)
                goto invalid;
        memcpy(patch->before, record + 4, 512);
        memcpy(patch->after, record + 516, 512);
    }
    if (fgetc(file) != EOF || ferror(file))
        goto invalid;
    if (progress && !progress(ctx, "Read journal", plan->count, plan->count))
        goto cancelled;
    fclose(file);
    return identity(plan, err);
cancelled:
    fclose(file);
    return fail(err, "Journal validation cancelled; no sectors restored");
invalid:
    fclose(file);
    return fail(err, "Journal is invalid, truncated, or has a checksum error");
}

static int write_verified(DtVolume *v, dt_u32 sector, const unsigned char *data)
{
    unsigned char check[512];
    DtDisk *disk = v->disk;

    if (!disk->write(disk->ctx, v->start + sector, data) || !disk->sync(disk->ctx))
        return 0;
    return dt_read(v, sector, check) && !memcmp(check, data, 512);
}

static int same_plan(const DtPlan *a, const DtPlan *b)
{
    unsigned i;
    if (a->count != b->count || a->cluster != b->cluster || a->mark_bad != b->mark_bad ||
        a->kind != b->kind)
        return 0;
    for (i = 0; i < a->count; i++) {
        if (a->patch[i].sector != b->patch[i].sector ||
            memcmp(a->patch[i].before, b->patch[i].before, 512) ||
            memcmp(a->patch[i].after, b->patch[i].after, 512))
            return 0;
    }
    return 1;
}

typedef struct {
    DtWriteProgress callback;
    void *ctx;
} JournalProgress;

static int verify_progress(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    JournalProgress *progress = (JournalProgress *)ctx;

    (void)phase;
    progress->callback(progress->ctx, "Verify journal", done, total);
    return 1;
}

int dt_plan_apply_ex(DtPlan *plan, const char *journal, DtProgress progress, void *ctx,
                     DtWriteProgress writing, void *writing_ctx, char *err)
{
    DtVolume *v = plan->volume;
    DtPlan *check;
    unsigned i, touched = 0, undo_total;
    unsigned char buffer[512];
    int ok = 0, rollback = 1;
    JournalProgress notification;

    notification.callback = writing;
    notification.ctx = writing_ctx;

    if (!v || !v->disk->write || !v->disk->sync)
        return fail(err,
                    "This target is read-only; reopen an image or DOS volume with write access");
    if (!plan->count || plan->count > plan->capacity || plan->count >= sector_limit(plan) ||
        !identity(plan, err))
        return fail(err, "Repair plan is invalid or disk identity changed");
    if (v->boot_source && plan->kind != DT_REPAIR_BOOT_PRIMARY)
        return fail(err,
                    "Recovery mounts permit only primary boot repair; remount for other writes");
    check = dt_plan_create();
    if (!check)
        return fail(err, "Insufficient memory for repair validation");
    /* Recompute from current metadata after the review dialog, before any write. */
    if (plan->kind == DT_REPAIR_RELOCATE) {
        if (!dt_relocate_plan(v, plan->cluster, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_BAD) {
        if (!dt_bad_plan(v, plan->cluster, plan->mark_bad, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_FSINFO) {
        if (!dt_fsinfo_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_BOOT_BACKUP) {
        if (!dt_boot_backup_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_MIRROR) {
        if (!dt_mirror_plan(v, (unsigned)plan->cluster, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_LOST) {
        if (!dt_lost_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_CHAINS) {
        if (!dt_chain_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_DIRECTORIES) {
        if (!dt_directory_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_TREE_SOURCE) {
        if (!dt_tree_source_plan(v, (unsigned)plan->mark_bad, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_ENTRIES) {
        if (!dt_entry_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_TREE) {
        if (!dt_tree_plan(v, check, progress, ctx, err))
            goto done;
    } else if (plan->kind == DT_REPAIR_BOOT_PRIMARY) {
        if (!dt_boot_primary_plan(v, check, progress, ctx, err))
            goto done;
    } else {
        fail(err, "Unknown repair operation");
        goto done;
    }
    if (!same_plan(plan, check)) {
        fail(err, "Filesystem changed since the preview; create a new repair plan");
        goto done;
    }
    if (!save_journal(plan, journal, writing, writing_ctx, err) ||
        !load_journal(v, journal, check, writing ? verify_progress : NULL, &notification, err))
        goto done;
    if (!same_plan(plan, check)) {
        fail(err, "Journal verification failed; no sectors written");
        goto done;
    }
    /* No cancellation after the durable journal: finish or roll back the transaction. */
    if (writing)
        writing(writing_ctx, "Commit repair", 0, plan->count);
    for (i = 0; i < plan->count; i++) {
        DtPatch *patch = &plan->patch[i];
        if (!dt_read(v, patch->sector, buffer) || memcmp(buffer, patch->before, 512)) {
            fail(err, "Metadata changed before writing; earlier writes will be rolled back");
            goto undo;
        }
        touched = i + 1;
        if (!write_verified(v, patch->sector, patch->after)) {
            fail(err, "Write/verify failed; earlier writes will be rolled back");
            goto undo;
        }
        if (writing)
            writing(writing_ctx, "Commit repair", i + 1, plan->count);
    }
    ok = 1;
    goto done;
undo:
    undo_total = touched;
    if (writing)
        writing(writing_ctx, "Roll back repair", 0, undo_total);
    while (touched) {
        DtPatch *patch = &plan->patch[--touched];
        if (!write_verified(v, patch->sector, patch->before))
            rollback = 0;
        if (writing)
            writing(writing_ctx, "Roll back repair", undo_total - touched, undo_total);
    }
    fail(err,
         rollback ? "Change failed; original sectors restored and verified"
                  : "Rollback incomplete: preserve the journal and stop using this volume");
done:
    v->cache_sector = 0xffffffffUL;
    dt_plan_free(check);
    return ok;
}

/* Reconstruct original metadata in memory to validate ownership-dependent undo,
   including interrupted transactions with mixed FAT and directory sectors. */
int dt_plan_apply(DtPlan *plan, const char *journal, DtProgress progress, void *ctx, char *err)
{
    return dt_plan_apply_ex(plan, journal, progress, ctx, NULL, NULL, err);
}

static int original_read(void *ctx, dt_u32 sector, unsigned char *buffer)
{
    DtPlan *plan = (DtPlan *)ctx;
    unsigned i;

    if (sector >= plan->start && sector - plan->start < plan->total) {
        for (i = 0; i < plan->count; i++)
            if (plan->patch[i].sector == sector - plan->start) {
                memcpy(buffer, plan->patch[i].before, 512);
                return 1;
            }
    }
    return plan->volume->disk->read(plan->volume->disk->ctx, sector, buffer);
}

typedef struct {
    DtProgress callback;
    void *ctx;
    int cancelled;
} RestoreProgress;

static int restore_progress(void *ctx, const char *phase, dt_u32 done, dt_u32 total)
{
    RestoreProgress *progress = (RestoreProgress *)ctx;

    if (!progress->callback(progress->ctx, phase, done, total)) {
        progress->cancelled = 1;
        return 0;
    }
    return 1;
}

static int validate_owned_restore(DtPlan *plan, DtProgress progress, void *ctx, char *err)
{
    DtDisk disk;
    DtVolume original = *plan->volume;
    DtPlan *expected = dt_plan_create();
    int ok;
    RestoreProgress tracking;

    tracking.callback = progress;
    tracking.ctx = ctx;
    tracking.cancelled = 0;

    if (!expected)
        return fail(err, "Insufficient memory to validate ownership before undo");
    memset(&disk, 0, sizeof(disk));
    disk.ctx = plan;
    disk.sectors = plan->disk_sectors;
    disk.read = original_read;
    original.disk = &disk;
    original.cache_sector = 0xffffffffUL;
    if (plan->kind == DT_REPAIR_RELOCATE)
        ok = dt_relocate_plan(
            &original, plan->cluster, expected, progress ? restore_progress : NULL, &tracking, err);
    else if (plan->kind == DT_REPAIR_TREE_SOURCE)
        ok = dt_tree_source_plan(&original,
                                 (unsigned)plan->mark_bad,
                                 expected,
                                 progress ? restore_progress : NULL,
                                 &tracking,
                                 err);
    else if (plan->kind == DT_REPAIR_ENTRIES)
        ok = dt_entry_plan(&original, expected, progress ? restore_progress : NULL, &tracking, err);
    else if (plan->kind == DT_REPAIR_TREE)
        ok = dt_tree_plan(&original, expected, progress ? restore_progress : NULL, &tracking, err);
    else if (plan->kind == DT_REPAIR_DIRECTORIES)
        ok = dt_directory_plan(
            &original, expected, progress ? restore_progress : NULL, &tracking, err);
    else if (plan->kind == DT_REPAIR_CHAINS)
        ok = dt_chain_plan(&original, expected, progress ? restore_progress : NULL, &tracking, err);
    else
        ok = dt_lost_plan(&original, expected, progress ? restore_progress : NULL, &tracking, err);
    if (!ok && tracking.cancelled) {
        dt_plan_free(expected);
        return fail(err, "Ownership validation cancelled; no sectors restored");
    }
    ok = ok && same_plan(plan, expected);
    dt_plan_free(expected);
    return ok ? 1
              : fail(err,
                     plan->kind == DT_REPAIR_CHAINS || plan->kind == DT_REPAIR_DIRECTORIES ||
                             plan->kind == DT_REPAIR_TREE || plan->kind == DT_REPAIR_TREE_SOURCE ||
                             plan->kind == DT_REPAIR_ENTRIES || plan->kind == DT_REPAIR_RELOCATE
                         ? "Tree-repair journal no longer matches directory ownership or allocation"
                         : "Reclaim journal no longer matches directory ownership or allocation");
}

/* A byte-compatible mixture is plausible interrupted I/O, not proof of it.
   Accept it only through the caller's explicit interrupted-write action. */
static int restorable_sector(const DtPatch *patch, const unsigned char *data, int torn)
{
    unsigned i;

    if (!memcmp(data, patch->before, 512) || !memcmp(data, patch->after, 512))
        return 1;
    if (!torn)
        return 0;
    for (i = 0; i < 512; i++) {
        if (data[i] != patch->before[i] && data[i] != patch->after[i])
            return 0;
    }
    return 1;
}

static int restore_plan(DtVolume *v, const char *journal, DtProgress progress, void *ctx,
                        DtWriteProgress writing, void *writing_ctx, int torn, char *err)
{
    DtPlan *plan;
    unsigned i;
    unsigned char buffer[512];
    dt_u32 *observed = NULL;
    int ok = 0;

    if (!v->disk->write || !v->disk->sync)
        return fail(err,
                    "This target is read-only; reopen an image or DOS volume with write access");
    plan = dt_plan_create();
    if (!plan)
        return fail(err, "Insufficient memory for journal");
    if (!load_journal(v, journal, plan, progress, ctx, err))
        goto done;
    if (v->boot_source && plan->kind != DT_REPAIR_BOOT_PRIMARY) {
        fail(err, "Recovery mounts permit only primary boot journal restoration");
        goto done;
    }
    if ((plan->kind == DT_REPAIR_LOST || plan->kind == DT_REPAIR_CHAINS ||
         plan->kind == DT_REPAIR_DIRECTORIES || plan->kind == DT_REPAIR_TREE ||
         plan->kind == DT_REPAIR_TREE_SOURCE || plan->kind == DT_REPAIR_ENTRIES ||
         plan->kind == DT_REPAIR_RELOCATE) &&
        !validate_owned_restore(plan, progress, ctx, err))
        goto done;
    if (torn && plan->count) {
        observed = (dt_u32 *)calloc(plan->count, sizeof(*observed));
        if (!observed) {
            fail(err, "Insufficient memory to track interrupted-write recovery");
            goto done;
        }
    }
    /* Validate every sector before restoring any: refuse unrelated later changes. */
    for (i = 0; i < plan->count; i++) {
        DtPatch *patch = &plan->patch[i];

        if (progress && !progress(ctx, "Check undo sectors", i, plan->count)) {
            fail(err, "Journal restoration cancelled; no sectors restored");
            goto done;
        }
        if (!dt_read(v, patch->sector, buffer) || !restorable_sector(patch, buffer, torn)) {
            fail(err,
                 "A journal sector has unrelated changes or a torn write; no sectors restored");
            goto done;
        }
        if (torn)
            observed[i] = crc32(0, buffer, 512);
    }
    if (progress && !progress(ctx, "Check undo sectors", plan->count, plan->count)) {
        fail(err, "Journal restoration cancelled; no sectors restored");
        goto done;
    }
    if (writing)
        writing(writing_ctx, "Restore metadata", 0, plan->count);
    for (i = plan->count; i; i--) {
        DtPatch *patch = &plan->patch[i - 1];
        if (!dt_read(v, patch->sector, buffer)) {
            fail(err, "Restore read failed; preserve journal and retry");
            goto done;
        }
        if (torn && observed[i - 1] != crc32(0, buffer, 512)) {
            fail(err, "Volume changed during restore; preserve journal and stop using volume");
            goto done;
        }
        if (!memcmp(buffer, patch->before, 512)) {
            if (writing)
                writing(writing_ctx, "Restore metadata", plan->count - i + 1, plan->count);
            continue;
        }
        if (!restorable_sector(patch, buffer, torn)) {
            fail(err, "Volume changed during restore; preserve journal and stop using volume");
            goto done;
        }
        if (!write_verified(v, patch->sector, patch->before)) {
            fail(err, "Restore write/verify failed; preserve journal and stop using volume");
            goto done;
        }
        if (writing)
            writing(writing_ctx, "Restore metadata", plan->count - i + 1, plan->count);
    }
    ok = 1;
done:
    v->cache_sector = 0xffffffffUL;
    free(observed);
    dt_plan_free(plan);
    return ok;
}

int dt_plan_restore_ex(DtVolume *v, const char *journal, DtProgress progress, void *ctx,
                       DtWriteProgress writing, void *writing_ctx, char *err)
{
    return restore_plan(v, journal, progress, ctx, writing, writing_ctx, 0, err);
}

int dt_plan_restore_torn_ex(DtVolume *v, const char *journal, DtProgress progress, void *ctx,
                            DtWriteProgress writing, void *writing_ctx, char *err)
{
    return restore_plan(v, journal, progress, ctx, writing, writing_ctx, 1, err);
}

int dt_plan_restore(DtVolume *v, const char *journal, char *err)
{
    return dt_plan_restore_ex(v, journal, NULL, NULL, NULL, NULL, err);
}
