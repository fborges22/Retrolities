/* Whole-disk MBR transactions. These sectors are never volume-relative. */
#include "disk.h"
#include <string.h>

#define MBR_JOURNAL_SIZE 1600

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

static dt_u32 checksum(const unsigned char *data)
{
    dt_u32 crc = 0xffffffffUL;
    unsigned i, bit;

    for (i = 0; i < MBR_JOURNAL_SIZE; i++) {
        crc ^= i >= 60 && i < 64 ? 0 : data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc ^ 0xffffffffUL;
}

static void encode(const DtMbrPlan *plan, unsigned char *data)
{
    memset(data, 0, MBR_JOURNAL_SIZE);
    memcpy(data, "MDMBR001", 8);
    put32(data + 8, MBR_JOURNAL_SIZE);
    put32(data + 12, plan->disk_sectors);
    put32(data + 16, plan->start);
    put32(data + 20, plan->total);
    put32(data + 24, plan->slot);
    put32(data + 28, plan->boot_source);
    put32(data + 32, plan->heads);
    put32(data + 36, plan->sectors);
    put32(data + 40, plan->bits);
    memcpy(data + 64, plan->before, 512);
    memcpy(data + 576, plan->after, 512);
    memcpy(data + 1088, plan->volume_boot, 512);
    put32(data + 60, checksum(data));
}

static int load(DtDisk *disk, const char *path, DtMbrPlan *plan, char *err)
{
    unsigned char data[MBR_JOURNAL_SIZE];
    FILE *file = fopen(path, "rb");
    unsigned i;
    int ok;

    if (!file)
        return fail(err, "Cannot open MBR journal");
    ok = fread(data, 1, sizeof(data), file) == sizeof(data);
    if (ok)
        ok = fgetc(file) == EOF && !ferror(file);
    if (fclose(file))
        ok = 0;
    if (!ok || memcmp(data, "MDMBR001", 8) || dt_dword(data + 8) != sizeof(data) ||
        dt_dword(data + 60) != checksum(data))
        return fail(err, "Invalid or damaged MBR journal");
    for (i = 44; i < 60; i++)
        if (data[i])
            return fail(err, "Unsupported MBR journal fields");
    memset(plan, 0, sizeof(*plan));
    plan->disk = disk;
    plan->disk_sectors = dt_dword(data + 12);
    plan->start = dt_dword(data + 16);
    plan->total = dt_dword(data + 20);
    plan->slot = dt_dword(data + 24);
    plan->boot_source = dt_dword(data + 28);
    plan->heads = dt_dword(data + 32);
    plan->sectors = dt_dword(data + 36);
    plan->bits = dt_dword(data + 40);
    memcpy(plan->before, data + 64, 512);
    memcpy(plan->after, data + 576, 512);
    memcpy(plan->volume_boot, data + 1088, 512);
    return 1;
}

static int write_access(DtDisk *disk, const char *path, char *err)
{
    if (!disk || !disk->read || !disk->write || !disk->sync)
        return fail(err, "MBR transaction requires writable, flushable disk access");
    if (!path || !*path)
        return fail(err, "An MBR journal path is required");
    if (disk->separate_destination && !disk->separate_destination(disk->ctx, path))
        return fail(err, "MBR journal must be on a separate known disk");
    return 1;
}

static int save(const DtMbrPlan *plan, const char *path, char *err)
{
    unsigned char data[MBR_JOURNAL_SIZE], verified[MBR_JOURNAL_SIZE];
    DtMbrPlan loaded;
    FILE *file;
    int ok;

    encode(plan, data);
    file = dt_file_create(path);
    if (!file)
        return fail(err, "Cannot create new MBR journal; existing files are preserved");
    ok = fwrite(data, 1, sizeof(data), file) == sizeof(data);
    if (ok)
        ok = dt_file_commit(file);
    if (fclose(file))
        ok = 0;
    if (ok)
        ok = dt_file_commit_directory(path);
    if (ok && plan->disk->sync_journal)
        ok = plan->disk->sync_journal(plan->disk->ctx, path);
    if (!ok)
        return fail(err, "MBR journal could not be committed; no sectors written");
    if (!load(plan->disk, path, &loaded, err))
        return 0;
    encode(&loaded, verified);
    if (memcmp(data, verified, sizeof(data)))
        return fail(err, "MBR journal verification failed; no sectors written");
    return 1;
}

static int write_verified(DtDisk *disk, const unsigned char *data)
{
    unsigned char actual[512];
    int written = disk->write(disk->ctx, 0, data);
    int synced = disk->sync(disk->ctx);

    return written && synced && disk->read(disk->ctx, 0, actual) && !memcmp(actual, data, 512);
}

int dt_mbr_apply(const DtMbrPlan *plan, const char *journal, char *err)
{
    if (!plan || !write_access(plan->disk, journal, err))
        return fail(err, "Missing plan or safe MBR write access");
    if (!dt_mbr_revalidate(plan, err) || !save(plan, journal, err) || !dt_mbr_revalidate(plan, err))
        return 0;
    if (write_verified(plan->disk, plan->after))
        return 1;
    if (write_verified(plan->disk, plan->before))
        return fail(err, "MBR write failed; original sector restored and verified");
    return fail(err, "MBR rollback incomplete; preserve journal and stop using disk");
}

static int original_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    const DtMbrPlan *plan = (const DtMbrPlan *)ctx;

    if (!sector) {
        memcpy(data, plan->before, 512);
        return 1;
    }
    return plan->disk->read(plan->disk->ctx, sector, data);
}

int dt_mbr_restore(DtDisk *disk, const char *journal, int allow_torn, char *err)
{
    DtMbrPlan plan, original;
    DtDisk overlay;
    unsigned char current[512], check[512];
    unsigned i;

    if (!write_access(disk, journal, err) || !load(disk, journal, &plan, err))
        return 0;
    /* Reconstruct the before-state to prove this journal describes exactly the
       supported insertion, not arbitrary bytes with a valid checksum. */
    memset(&overlay, 0, sizeof(overlay));
    overlay.ctx = &plan;
    overlay.sectors = disk->sectors;
    overlay.read = original_read;
    original = plan;
    original.disk = &overlay;
    if (!dt_mbr_revalidate(&original, err))
        return 0;
    if (!disk->read(disk->ctx, 0, current))
        return fail(err, "Cannot read current MBR for undo");
    if (memcmp(current, plan.before, 512) && memcmp(current, plan.after, 512)) {
        if (!allow_torn)
            return fail(err, "MBR differs from journal; interrupted-write review required");
        for (i = 0; i < 512; i++)
            if (current[i] != plan.before[i] && current[i] != plan.after[i])
                return fail(err, "MBR contains changes unrelated to this journal");
    }
    if (!disk->read(disk->ctx, 0, check) || memcmp(current, check, 512))
        return fail(err, "MBR changed before undo");
    if (!memcmp(current, plan.before, 512))
        return 1;
    if (write_verified(disk, plan.before))
        return 1;
    if (write_verified(disk, current))
        return fail(err, "MBR undo failed; previous sector restored and verified");
    return fail(err, "MBR undo rollback incomplete; preserve journal and stop using disk");
}
