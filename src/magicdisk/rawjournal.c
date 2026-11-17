/* Raw sector edits from the sector viewer: one contiguous range of absolute
   disk LBAs, journaled with every sector's before/after bytes, written only
   after the journal is durable and verified, and rolled back on failure. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>

#define RAW_HEADER 64U
#define RAW_IDENTITY 512U
#define RAW_RECORD 1024U

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static void put32(unsigned char *out, dt_u32 value)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        out[i] = (unsigned char)(value >> (i * 8));
}

static dt_u32 checksum(const unsigned char *data, dt_u32 size)
{
    dt_u32 crc = 0xffffffffUL, i;
    unsigned bit;

    for (i = 0; i < size; i++) {
        crc ^= i >= 60 && i < 64 ? 0 : data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc ^ 0xffffffffUL;
}

static dt_u32 journal_size(unsigned count)
{
    return RAW_HEADER + RAW_IDENTITY + (dt_u32)count * RAW_RECORD;
}

void dt_raw_clear(DtRawPlan *plan)
{
    free(plan->patch);
    memset(plan, 0, sizeof(*plan));
}

static int bounded(DtDisk *disk, dt_u32 first, unsigned count)
{
    return disk && count && count <= DT_RAW_MAX && first < disk->sectors &&
           count <= disk->sectors - first;
}

static int allocate(DtRawPlan *plan, unsigned count, char *err)
{
    plan->patch = (DtPatch *)calloc(count, sizeof(DtPatch));
    return plan->patch ? 1 : fail(err, "Insufficient memory for the sector edit");
}

int dt_raw_plan(DtDisk *disk, dt_u32 first, unsigned count, DtRawPlan *plan, char *err)
{
    unsigned i;

    memset(plan, 0, sizeof(*plan));
    if (!bounded(disk, first, count))
        return fail(err, "Sector range is outside the disk or larger than the edit limit");
    if (!allocate(plan, count, err))
        return 0;
    plan->disk = disk;
    plan->disk_sectors = disk->sectors;
    plan->first = first;
    plan->count = count;
    if (!disk->read(disk->ctx, 0, plan->identity)) {
        dt_raw_clear(plan);
        return fail(err, "Cannot read disk identity sector");
    }
    for (i = 0; i < count; i++) {
        DtPatch *patch = &plan->patch[i];

        patch->sector = first + i;
        if (!disk->read(disk->ctx, patch->sector, patch->before)) {
            sprintf(err, "Cannot read disk LBA %lu; no edit is possible", (unsigned long)patch->sector);
            dt_raw_clear(plan);
            return 0;
        }
        memcpy(patch->after, patch->before, 512);
    }
    return 1;
}

unsigned dt_raw_changed(const DtRawPlan *plan)
{
    unsigned i, changed = 0;

    for (i = 0; i < plan->count; i++)
        if (memcmp(plan->patch[i].before, plan->patch[i].after, 512))
            changed++;
    return changed;
}

static unsigned char *encode(const DtRawPlan *plan, dt_u32 *size)
{
    unsigned char *data;
    unsigned i;

    *size = journal_size(plan->count);
    data = (unsigned char *)calloc(1, *size);
    if (!data)
        return NULL;
    memcpy(data, "MDRAW001", 8);
    put32(data + 8, *size);
    put32(data + 12, plan->disk_sectors);
    put32(data + 16, plan->first);
    put32(data + 20, plan->count);
    memcpy(data + RAW_HEADER, plan->identity, 512);
    for (i = 0; i < plan->count; i++) {
        unsigned char *record = data + RAW_HEADER + RAW_IDENTITY + i * RAW_RECORD;

        memcpy(record, plan->patch[i].before, 512);
        memcpy(record + 512, plan->patch[i].after, 512);
    }
    put32(data + 60, checksum(data, *size));
    return data;
}

static int load(DtDisk *disk, const char *path, DtRawPlan *plan, char *err)
{
    unsigned char header[RAW_HEADER], *data = NULL;
    dt_u32 size, count;
    unsigned i;
    FILE *file = fopen(path, "rb");
    int ok;

    memset(plan, 0, sizeof(*plan));
    if (!file)
        return fail(err, "Cannot open sector edit journal");
    ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
         !memcmp(header, "MDRAW001", 8);
    count = ok ? dt_dword(header + 20) : 0;
    size = ok ? dt_dword(header + 8) : 0;
    ok = ok && count && count <= DT_RAW_MAX && size == journal_size((unsigned)count);
    if (ok)
        ok = (data = (unsigned char *)malloc(size)) != NULL;
    if (ok) {
        memcpy(data, header, sizeof(header));
        ok = fread(data + RAW_HEADER, 1, size - RAW_HEADER, file) == size - RAW_HEADER &&
             fgetc(file) == EOF && !ferror(file);
    }
    if (fclose(file))
        ok = 0;
    if (ok && dt_dword(data + 60) != checksum(data, size))
        ok = 0;
    for (i = 24; ok && i < 60; i++)
        if (data[i])
            ok = 0;
    if (!ok) {
        free(data);
        return fail(err, "Sector edit journal is invalid, truncated, or has a checksum error");
    }
    if (!allocate(plan, (unsigned)count, err)) {
        free(data);
        return 0;
    }
    plan->disk = disk;
    plan->disk_sectors = dt_dword(data + 12);
    plan->first = dt_dword(data + 16);
    plan->count = (unsigned)count;
    memcpy(plan->identity, data + RAW_HEADER, 512);
    for (i = 0; i < plan->count; i++) {
        const unsigned char *record = data + RAW_HEADER + RAW_IDENTITY + i * RAW_RECORD;

        plan->patch[i].sector = plan->first + i;
        memcpy(plan->patch[i].before, record, 512);
        memcpy(plan->patch[i].after, record + 512, 512);
    }
    free(data);
    if (plan->first >= plan->disk_sectors || plan->count > plan->disk_sectors - plan->first) {
        dt_raw_clear(plan);
        return fail(err, "Sector edit journal describes sectors outside its disk");
    }
    return 1;
}

static int write_access(DtDisk *disk, const char *path, char *err)
{
    if (!disk || !disk->read || !disk->write || !disk->sync)
        return fail(err, "Sector edits require writable, flushable disk access");
    if (!path || !*path)
        return fail(err, "A sector edit journal path is required");
    if (disk->separate_destination && !disk->separate_destination(disk->ctx, path))
        return fail(err, "Sector edit journal must be on a separate known disk");
    return 1;
}

static int save(const DtRawPlan *plan, const char *path, char *err)
{
    unsigned char *data, *verified = NULL;
    dt_u32 size, check_size;
    DtRawPlan loaded;
    FILE *file;
    int ok;

    data = encode(plan, &size);
    if (!data)
        return fail(err, "Insufficient memory for the sector edit journal");
    file = dt_file_create(path);
    if (!file) {
        free(data);
        return fail(err, "Cannot create new sector edit journal; existing files are preserved");
    }
    ok = fwrite(data, 1, size, file) == size;
    if (ok)
        ok = dt_file_commit(file);
    if (fclose(file))
        ok = 0;
    if (ok)
        ok = dt_file_commit_directory(path);
    if (ok && plan->disk->sync_journal)
        ok = plan->disk->sync_journal(plan->disk->ctx, path);
    if (!ok) {
        free(data);
        return fail(err, "Sector edit journal could not be committed; no sectors written");
    }
    ok = load(plan->disk, path, &loaded, err);
    if (ok) {
        verified = encode(&loaded, &check_size);
        ok = verified && check_size == size && !memcmp(data, verified, size);
        dt_raw_clear(&loaded);
        if (!ok)
            fail(err, "Sector edit journal verification failed; no sectors written");
    }
    free(verified);
    free(data);
    return ok;
}

static int write_verified(DtDisk *disk, dt_u32 sector, const unsigned char *data)
{
    unsigned char actual[512];
    int written = disk->write(disk->ctx, sector, data);
    int synced = disk->sync(disk->ctx);

    return written && synced && disk->read(disk->ctx, sector, actual) &&
           !memcmp(actual, data, 512);
}

/* LBA 0 binds the journal to one disk. When the edit covers LBA 0, its saved
   before/after bytes (checked per sector) take the place of the identity. */
static int identity(const DtRawPlan *plan, DtDisk *disk, char *err)
{
    unsigned char current[512];

    if (disk->sectors != plan->disk_sectors)
        return fail(err, "Disk size changed; this sector edit belongs to another disk");
    if (!plan->first)
        return 1;
    if (!disk->read(disk->ctx, 0, current) || memcmp(current, plan->identity, 512))
        return fail(err, "Disk identity changed; no sectors written");
    return 1;
}

int dt_raw_apply(const DtRawPlan *plan, const char *journal, DtWriteProgress writing,
                 void *writing_ctx, char *err)
{
    unsigned char current[512];
    unsigned i, touched = 0, changed;
    int rollback = 1;

    if (!plan || !plan->patch)
        return fail(err, "Missing sector edit");
    if (!write_access(plan->disk, journal, err))
        return 0;
    if (!bounded(plan->disk, plan->first, plan->count))
        return fail(err, "Sector edit range is invalid");
    changed = dt_raw_changed(plan);
    if (!changed)
        return fail(err, "No bytes changed; nothing to write");
    if (!identity(plan, plan->disk, err))
        return 0;
    for (i = 0; i < plan->count; i++)
        if (plan->patch[i].sector != plan->first + i ||
            !plan->disk->read(plan->disk->ctx, plan->patch[i].sector, current) ||
            memcmp(current, plan->patch[i].before, 512))
            return fail(err, "Sectors changed since the review; reload and edit again");
    if (writing)
        writing(writing_ctx, "Save journal", 0, 1);
    if (!save(plan, journal, err))
        return 0;
    if (writing) {
        writing(writing_ctx, "Save journal", 1, 1);
        writing(writing_ctx, "Commit repair", 0, plan->count);
    }
    /* No cancellation after the durable journal: finish or roll back. */
    for (i = 0; i < plan->count; i++) {
        const DtPatch *patch = &plan->patch[i];

        if (memcmp(patch->before, patch->after, 512)) {
            if (!plan->disk->read(plan->disk->ctx, patch->sector, current) ||
                memcmp(current, patch->before, 512)) {
                fail(err, "Sector changed before writing; earlier writes will be rolled back");
                goto undo;
            }
            touched = i + 1;
            if (!write_verified(plan->disk, patch->sector, patch->after)) {
                fail(err, "Write/verify failed; earlier writes will be rolled back");
                goto undo;
            }
        }
        if (writing)
            writing(writing_ctx, "Commit repair", i + 1, plan->count);
    }
    return 1;
undo:
    if (writing)
        writing(writing_ctx, "Roll back repair", 0, touched);
    while (touched) {
        const DtPatch *patch = &plan->patch[--touched];

        if (memcmp(patch->before, patch->after, 512) &&
            !write_verified(plan->disk, patch->sector, patch->before))
            rollback = 0;
    }
    return fail(err,
                rollback ? "Sector edit failed; original sectors restored and verified"
                         : "Rollback incomplete: preserve the journal and stop using this disk");
}

int dt_raw_review_journal(DtDisk *disk, const char *journal, DtRawPlan *plan, char *err)
{
    if (!load(disk, journal, plan, err))
        return 0;
    if (!identity(plan, disk, err)) {
        dt_raw_clear(plan);
        return 0;
    }
    return 1;
}

int dt_raw_restore(DtDisk *disk, const char *journal, int allow_torn, DtWriteProgress writing,
                   void *writing_ctx, char *err)
{
    DtRawPlan plan;
    unsigned char *current;
    unsigned i, j, pending = 0, restored = 0, touched = 0;
    int ok = 0, rollback = 1;

    if (!write_access(disk, journal, err) || !load(disk, journal, &plan, err))
        return 0;
    current = (unsigned char *)malloc((size_t)plan.count * 512);
    if (!current) {
        dt_raw_clear(&plan);
        return fail(err, "Insufficient memory to restore the sector edit");
    }
    if (!identity(&plan, disk, err))
        goto done;
    /* Every sector must hold its saved before or after bytes (or, for an
       interrupted write, only a mixture of them) before anything is written. */
    for (i = 0; i < plan.count; i++) {
        unsigned char *now = current + (size_t)i * 512;
        const DtPatch *patch = &plan.patch[i];

        if (!disk->read(disk->ctx, patch->sector, now)) {
            sprintf(err, "Cannot read disk LBA %lu for undo", (unsigned long)patch->sector);
            goto done;
        }
        if (!memcmp(now, patch->before, 512))
            continue;
        if (memcmp(now, patch->after, 512)) {
            if (!allow_torn) {
                sprintf(err,
                        "LBA %lu differs from journal; interrupted-write review required",
                        (unsigned long)patch->sector);
                goto done;
            }
            for (j = 0; j < 512; j++)
                if (now[j] != patch->before[j] && now[j] != patch->after[j]) {
                    sprintf(err,
                            "LBA %lu contains changes unrelated to this journal",
                            (unsigned long)patch->sector);
                    goto done;
                }
        }
        pending++;
    }
    if (writing)
        writing(writing_ctx, "Restore metadata", 0, pending);
    for (i = 0; i < plan.count; i++) {
        const DtPatch *patch = &plan.patch[i];

        if (!memcmp(current + (size_t)i * 512, patch->before, 512))
            continue;
        touched = i + 1;
        if (!write_verified(disk, patch->sector, patch->before)) {
            fail(err, "Sector undo failed; previous sectors will be put back");
            goto undo;
        }
        if (writing)
            writing(writing_ctx, "Restore metadata", ++restored, pending);
    }
    ok = 1;
    goto done;
undo:
    while (touched) {
        --touched;
        if (memcmp(current + (size_t)touched * 512, plan.patch[touched].before, 512) &&
            !write_verified(disk, plan.patch[touched].sector, current + (size_t)touched * 512))
            rollback = 0;
    }
    fail(err,
         rollback ? "Sector undo failed; previous sectors restored and verified"
                  : "Sector undo rollback incomplete; preserve journal and stop using disk");
done:
    free(current);
    dt_raw_clear(&plan);
    return ok;
}
