/* Whole-disk EBR transactions. These sectors are never volume-relative. */
#include "disk.h"
#include <string.h>

#define EBR_JOURNAL_SIZE 2112

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

    for (i = 0; i < EBR_JOURNAL_SIZE; i++) {
        crc ^= i >= 60 && i < 64 ? 0 : data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc ^ 0xffffffffUL;
}

static void encode(const DtEbrPlan *plan, unsigned char *data)
{
    memset(data, 0, EBR_JOURNAL_SIZE);
    memcpy(data, "MDEBR001", 8);
    put32(data + 8, EBR_JOURNAL_SIZE);
    put32(data + 12, plan->entry.disk_sectors);
    put32(data + 16, plan->entry.start);
    put32(data + 20, plan->entry.total);
    put32(data + 24, plan->entry.slot);
    put32(data + 28, plan->entry.boot_source);
    put32(data + 32, plan->entry.heads);
    put32(data + 36, plan->entry.sectors);
    put32(data + 40, plan->entry.bits);
    memcpy(data + 64, plan->entry.before, 512);
    memcpy(data + 576, plan->entry.after, 512);
    memcpy(data + 1088, plan->entry.volume_boot, 512);
    put32(data + 44, plan->sector);
    memcpy(data + 1600, plan->mbr, 512);
    put32(data + 60, checksum(data));
}

static int load(DtDisk *disk, const char *path, DtEbrPlan *plan, char *err)
{
    unsigned char data[EBR_JOURNAL_SIZE];
    FILE *file = fopen(path, "rb");
    unsigned i;
    int ok;

    if (!file)
        return fail(err, "Cannot open EBR journal");
    ok = fread(data, 1, sizeof(data), file) == sizeof(data);
    if (ok)
        ok = fgetc(file) == EOF && !ferror(file);
    if (fclose(file))
        ok = 0;
    if (!ok || memcmp(data, "MDEBR001", 8) || dt_dword(data + 8) != sizeof(data) ||
        dt_dword(data + 60) != checksum(data))
        return fail(err, "Invalid or damaged EBR journal");
    for (i = 48; i < 60; i++)
        if (data[i])
            return fail(err, "Unsupported EBR journal fields");
    memset(plan, 0, sizeof(*plan));
    plan->entry.disk = disk;
    plan->entry.disk_sectors = dt_dword(data + 12);
    plan->entry.start = dt_dword(data + 16);
    plan->entry.total = dt_dword(data + 20);
    plan->entry.slot = dt_dword(data + 24);
    plan->entry.boot_source = dt_dword(data + 28);
    plan->entry.heads = dt_dword(data + 32);
    plan->entry.sectors = dt_dword(data + 36);
    plan->entry.bits = dt_dword(data + 40);
    memcpy(plan->entry.before, data + 64, 512);
    memcpy(plan->entry.after, data + 576, 512);
    memcpy(plan->entry.volume_boot, data + 1088, 512);
    plan->sector = dt_dword(data + 44);
    memcpy(plan->mbr, data + 1600, 512);
    return 1;
}

static int write_access(DtDisk *disk, const char *path, char *err)
{
    if (!disk || !disk->read || !disk->write || !disk->sync)
        return fail(err, "EBR transaction requires writable, flushable disk access");
    if (!path || !*path)
        return fail(err, "An EBR journal path is required");
    if (disk->separate_destination && !disk->separate_destination(disk->ctx, path))
        return fail(err, "EBR journal must be on a separate known disk");
    return 1;
}

static int save(const DtEbrPlan *plan, const char *path, char *err)
{
    unsigned char data[EBR_JOURNAL_SIZE], verified[EBR_JOURNAL_SIZE];
    DtEbrPlan loaded;
    FILE *file;
    int ok;

    encode(plan, data);
    file = dt_file_create(path);
    if (!file)
        return fail(err, "Cannot create new EBR journal; existing files are preserved");
    ok = fwrite(data, 1, sizeof(data), file) == sizeof(data);
    if (ok)
        ok = dt_file_commit(file);
    if (fclose(file))
        ok = 0;
    if (ok)
        ok = dt_file_commit_directory(path);
    if (ok && plan->entry.disk->sync_journal)
        ok = plan->entry.disk->sync_journal(plan->entry.disk->ctx, path);
    if (!ok)
        return fail(err, "EBR journal could not be committed; no sectors written");
    if (!load(plan->entry.disk, path, &loaded, err))
        return 0;
    encode(&loaded, verified);
    if (memcmp(data, verified, sizeof(data)))
        return fail(err, "EBR journal verification failed; no sectors written");
    return 1;
}

static int write_verified(DtDisk *disk, dt_u32 sector, const unsigned char *data)
{
    unsigned char actual[512];
    int written = disk->write(disk->ctx, sector, data);
    int synced = disk->sync(disk->ctx);

    return written && synced && disk->read(disk->ctx, sector, actual) && !memcmp(actual, data, 512);
}

int dt_ebr_apply(const DtEbrPlan *plan, const char *journal, char *err)
{
    if (!plan || !write_access(plan->entry.disk, journal, err))
        return fail(err, "Missing plan or safe EBR write access");
    if (!dt_ebr_revalidate(plan, err) || !save(plan, journal, err) || !dt_ebr_revalidate(plan, err))
        return 0;
    if (write_verified(plan->entry.disk, plan->sector, plan->entry.after))
        return 1;
    if (write_verified(plan->entry.disk, plan->sector, plan->entry.before))
        return fail(err, "EBR write failed; original sector restored and verified");
    return fail(err, "EBR rollback incomplete; preserve journal and stop using disk");
}

static int original_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    const DtEbrPlan *plan = (const DtEbrPlan *)ctx;

    if (sector == plan->sector) {
        memcpy(data, plan->entry.before, 512);
        return 1;
    }
    return plan->entry.disk->read(plan->entry.disk->ctx, sector, data);
}

/* Read-only journal review also identifies the one-sector BIOS write range. */
int dt_ebr_review_journal(DtDisk *disk, const char *journal, DtEbrPlan *plan, char *err)
{
    DtEbrPlan original;
    DtDisk overlay;

    memset(plan, 0, sizeof(*plan));
    if (!disk || !disk->read || !journal || !*journal)
        return fail(err, "Missing disk or EBR journal");
    if (!load(disk, journal, plan, err))
        return 0;
    memset(&overlay, 0, sizeof(overlay));
    overlay.ctx = plan;
    overlay.sectors = disk->sectors;
    overlay.read = original_read;
    original = *plan;
    original.entry.disk = &overlay;
    if (!dt_ebr_revalidate(&original, err)) {
        memset(plan, 0, sizeof(*plan));
        return 0;
    }
    return 1;
}

int dt_ebr_restore(DtDisk *disk, const char *journal, int allow_torn, char *err)
{
    DtEbrPlan plan;
    unsigned char current[512], check[512];
    unsigned i;

    if (!write_access(disk, journal, err) || !dt_ebr_review_journal(disk, journal, &plan, err))
        return 0;
    if (!disk->read(disk->ctx, plan.sector, current))
        return fail(err, "Cannot read current EBR for undo");
    if (memcmp(current, plan.entry.before, 512) && memcmp(current, plan.entry.after, 512)) {
        if (!allow_torn)
            return fail(err, "EBR differs from journal; interrupted-write review required");
        for (i = 0; i < 512; i++)
            if (current[i] != plan.entry.before[i] && current[i] != plan.entry.after[i])
                return fail(err, "EBR contains changes unrelated to this journal");
    }
    if (!disk->read(disk->ctx, plan.sector, check) || memcmp(current, check, 512))
        return fail(err, "EBR changed before undo");
    if (!memcmp(current, plan.entry.before, 512))
        return 1;
    if (write_verified(disk, plan.sector, plan.entry.before))
        return 1;
    if (write_verified(disk, plan.sector, current))
        return fail(err, "EBR undo failed; previous sector restored and verified");
    return fail(err, "EBR undo rollback incomplete; preserve journal and stop using disk");
}
