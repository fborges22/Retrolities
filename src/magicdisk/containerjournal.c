/* Extended-container journals bind every recovered record and volume boot sector. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>

#define CONTAINER_HEADER 1088U
#define CONTAINER_WITNESS 1032U

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

static dt_u32 checksum(const unsigned char *data, unsigned size)
{
    dt_u32 crc = 0xffffffffUL;
    unsigned i, bit;

    for (i = 0; i < size; i++) {
        crc ^= i >= 60 && i < 64 ? 0 : data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc ^ 0xffffffffUL;
}

static unsigned char *encode(const DtContainerPlan *plan, unsigned *size)
{
    unsigned char *data, *record;
    unsigned i;

    if (!plan->witness || !plan->witness_count || plan->witness_count > 128)
        return NULL;
    *size = CONTAINER_HEADER + plan->witness_count * CONTAINER_WITNESS;
    data = (unsigned char *)calloc(1, *size);
    if (!data)
        return NULL;
    memcpy(data, "MDCTR001", 8);
    put32(data + 8, *size);
    put32(data + 12, plan->disk_sectors);
    put32(data + 16, plan->slot);
    put32(data + 20, plan->base);
    put32(data + 24, plan->span);
    put32(data + 28, plan->type);
    put32(data + 32, plan->heads);
    put32(data + 36, plan->sectors);
    put32(data + 44, plan->witness_count);
    memcpy(data + 64, plan->before, 512);
    memcpy(data + 576, plan->after, 512);
    for (i = 0; i < plan->witness_count; i++) {
        record = data + CONTAINER_HEADER + i * CONTAINER_WITNESS;
        put32(record, plan->witness[i].record_sector);
        put32(record + 4, plan->witness[i].volume_start);
        memcpy(record + 8, plan->witness[i].record, 512);
        memcpy(record + 520, plan->witness[i].boot, 512);
    }
    put32(data + 60, checksum(data, *size));
    return data;
}

static int load(DtDisk *disk, const char *path, DtContainerPlan *plan, char *err)
{
    unsigned char header[64], *data = NULL, *record;
    unsigned size = 0, count, i;
    int ok = 0;
    FILE *file;

    memset(plan, 0, sizeof(*plan));
    file = fopen(path, "rb");
    if (!file)
        return fail(err, "Cannot open container journal");
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "MDCTR001", 8))
        goto done;
    count = dt_dword(header + 44);
    if (!count || count > 128 ||
        dt_dword(header + 8) != CONTAINER_HEADER + count * CONTAINER_WITNESS)
        goto done;
    if (dt_dword(header + 40))
        goto done;
    for (i = 48; i < 60; i++)
        if (header[i])
            goto done;
    size = CONTAINER_HEADER + count * CONTAINER_WITNESS;
    data = (unsigned char *)malloc(size);
    if (!data)
        goto done;
    memcpy(data, header, 64);
    if (fread(data + 64, 1, size - 64, file) != size - 64 || fgetc(file) != EOF || ferror(file) ||
        dt_dword(data + 60) != checksum(data, size))
        goto done;
    plan->witness = (DtLinkWitness *)calloc(count, sizeof(DtLinkWitness));
    if (!plan->witness)
        goto done;
    plan->disk = disk;
    plan->disk_sectors = dt_dword(data + 12);
    plan->slot = dt_dword(data + 16);
    plan->base = dt_dword(data + 20);
    plan->span = dt_dword(data + 24);
    plan->type = dt_dword(data + 28);
    plan->heads = dt_dword(data + 32);
    plan->sectors = dt_dword(data + 36);
    plan->witness_count = count;
    memcpy(plan->before, data + 64, 512);
    memcpy(plan->after, data + 576, 512);
    for (i = 0; i < count; i++) {
        record = data + CONTAINER_HEADER + i * CONTAINER_WITNESS;
        plan->witness[i].record_sector = dt_dword(record);
        plan->witness[i].volume_start = dt_dword(record + 4);
        memcpy(plan->witness[i].record, record + 8, 512);
        memcpy(plan->witness[i].boot, record + 520, 512);
    }
    ok = 1;
done:
    free(data);
    if (fclose(file))
        ok = 0;
    if (!ok) {
        dt_container_clear(plan);
        return fail(err, "Invalid, damaged or unreadable container journal");
    }
    return 1;
}

static int save(const DtContainerPlan *plan, const char *path, char *err)
{
    unsigned char *data, *verified = NULL;
    unsigned size, checked_size;
    DtContainerPlan checked;
    FILE *file;
    int ok;

    data = encode(plan, &size);
    if (!data)
        return fail(err, "Cannot serialize reviewed extended chain");
    file = dt_file_create(path);
    if (!file) {
        free(data);
        return fail(err, "Cannot create new container journal; existing files are preserved");
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
    if (ok && load(plan->disk, path, &checked, err)) {
        verified = encode(&checked, &checked_size);
        ok = verified && checked_size == size && !memcmp(data, verified, size);
        dt_container_clear(&checked);
    } else {
        ok = 0;
    }
    free(verified);
    free(data);
    return ok ? 1 : fail(err, "Container journal could not be committed and verified; no writes");
}

static int write_access(DtDisk *disk, const char *path, char *err)
{
    if (!disk || !disk->read || !disk->write || !disk->sync || !path || !*path)
        return fail(err, "Container transaction needs writable disk access and a journal");
    if (disk->separate_destination && !disk->separate_destination(disk->ctx, path))
        return fail(err, "Container journal must be on a separate known disk");
    return 1;
}

static int write_verified(DtDisk *disk, dt_u32 sector, const unsigned char *data)
{
    unsigned char actual[512];
    int written = disk->write(disk->ctx, sector, data);
    int synced = disk->sync(disk->ctx);

    return written && synced && disk->read(disk->ctx, sector, actual) && !memcmp(actual, data, 512);
}

int dt_container_apply(const DtContainerPlan *plan, const char *journal, char *err)
{
    if (!plan || !write_access(plan->disk, journal, err))
        return fail(err, "Missing plan or safe container write access");
    if (!dt_container_revalidate(plan, err) || !save(plan, journal, err) ||
        !dt_container_revalidate(plan, err))
        return 0;
    if (write_verified(plan->disk, 0, plan->after))
        return 1;
    if (write_verified(plan->disk, 0, plan->before))
        return fail(err, "Container write failed; original MBR restored and verified");
    return fail(err, "Container rollback incomplete; preserve journal and stop using disk");
}

static int original_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    const DtContainerPlan *plan = (const DtContainerPlan *)ctx;

    if (sector == 0) {
        memcpy(data, plan->before, 512);
        return 1;
    }
    return plan->disk->read(plan->disk->ctx, sector, data);
}

int dt_container_review_journal(DtDisk *disk, const char *journal, DtContainerPlan *plan, char *err)
{
    DtDisk overlay;
    DtContainerPlan original;

    memset(plan, 0, sizeof(*plan));
    if (!disk || !disk->read || !journal || !*journal)
        return fail(err, "Missing disk or container journal");
    if (!load(disk, journal, plan, err))
        return 0;
    memset(&overlay, 0, sizeof(overlay));
    overlay.ctx = plan;
    overlay.sectors = disk->sectors;
    overlay.read = original_read;
    /* Borrow the loaded witnesses; only the owning plan disposes them. */
    original = *plan;
    original.disk = &overlay;
    if (!dt_container_revalidate(&original, err)) {
        dt_container_clear(plan);
        return 0;
    }
    return 1;
}

int dt_container_restore(DtDisk *disk, const char *journal, int allow_torn, char *err)
{
    DtContainerPlan plan;
    unsigned char current[512], check[512];
    unsigned i;
    int ok = 0;

    if (!write_access(disk, journal, err) ||
        !dt_container_review_journal(disk, journal, &plan, err))
        return 0;
    if (!disk->read(disk->ctx, 0, current)) {
        fail(err, "Cannot read current MBR for undo");
        goto done;
    }
    if (memcmp(current, plan.before, 512) && memcmp(current, plan.after, 512)) {
        if (!allow_torn) {
            fail(err, "Container differs from journal; interrupted-write review required");
            goto done;
        }
        for (i = 0; i < 512; i++)
            if (current[i] != plan.before[i] && current[i] != plan.after[i]) {
                fail(err, "MBR contains changes unrelated to this container journal");
                goto done;
            }
    }
    if (!disk->read(disk->ctx, 0, check) || memcmp(current, check, 512)) {
        fail(err, "MBR changed before container undo");
        goto done;
    }
    if (!memcmp(current, plan.before, 512) || write_verified(disk, 0, plan.before)) {
        ok = 1;
        goto done;
    }
    if (write_verified(disk, 0, current))
        fail(err, "Container undo failed; previous MBR restored and verified");
    else
        fail(err, "Container undo rollback incomplete; preserve journal and stop using disk");
done:
    dt_container_clear(&plan);
    return ok;
}
