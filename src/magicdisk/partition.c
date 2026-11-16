/* Whole-disk partition previews are separate from filesystem-relative plans. */
#include "disk.h"
#include <string.h>

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static int zero_entry(const unsigned char *entry)
{
    unsigned i;

    for (i = 0; i < 16; i++)
        if (entry[i])
            return 0;
    return 1;
}

static void put32(unsigned char *out, dt_u32 value)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        out[i] = (unsigned char)(value >> (8 * i));
}

static void chs(unsigned char *out, dt_u32 lba, unsigned heads, unsigned sectors)
{
    dt_u32 track = lba / sectors, cylinder = track / heads;

    if (cylinder > 1023) {
        memset(out, 255, 3);
        return;
    }
    out[0] = (unsigned char)(track % heads);
    out[1] = (unsigned char)((lba % sectors + 1) | ((cylinder >> 2) & 192));
    out[2] = (unsigned char)cylinder;
}

typedef struct {
    dt_u32 start;
    unsigned boot_source, found;
    DtVolume volume;
} SelectedVolume;

static int select_volume(void *ctx, const DtVolume *volume)
{
    SelectedVolume *selected = (SelectedVolume *)ctx;

    if (volume->start == selected->start && volume->boot_source == selected->boot_source) {
        selected->volume = *volume;
        selected->found++;
    }
    return 1;
}

static int proposed_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    DtMbrPlan *plan = (DtMbrPlan *)ctx;

    if (!sector) {
        memcpy(data, plan->after, 512);
        return 1;
    }
    return plan->disk->read(plan->disk->ctx, sector, data);
}

static int build_plan(DtDisk *disk, unsigned slot, dt_u32 start, unsigned boot_source,
                      unsigned heads, unsigned sectors, DtMbrPlan *plan, char *err)
{
    SelectedVolume selected;
    DtPartition partitions[128];
    DtVolume superfloppy;
    DtDisk proposed;
    unsigned char header[512], *entry;
    unsigned i, count, occupied = 0, matches = 0;
    dt_u32 end;
    dt_u64 chs_limit;
    char detail[160];

    memset(plan, 0, sizeof(*plan));
    err[0] = 0;
    if (!disk || !disk->read || slot < 1 || slot > 4 || !start || start >= disk->sectors ||
        boot_source >= disk->sectors - start || !heads || heads > 256 || !sectors || sectors > 63)
        return fail(err, "Invalid partition slot, volume extent or CHS geometry");
    if (!disk->read(disk->ctx, 0, plan->before) || !disk->read(disk->ctx, 1, header))
        return fail(err, "Cannot read disk partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "GPT disks cannot be reconstructed as DOS MBR disks");
    /* A damaged primary GPT header may leave only its end-of-disk backup. */
    if (!disk->read(disk->ctx, disk->sectors - 1, header))
        return fail(err, "Cannot read backup partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "Backup GPT header prevents DOS MBR reconstruction");
    if (dt_mount(disk, 0, disk->sectors, &superfloppy, detail))
        return fail(err, "Sector zero is a FAT volume; do not replace its boot metadata");
    for (i = 0; i < 4; i++) {
        entry = plan->before + 446 + i * 16;
        if (zero_entry(entry))
            continue;
        occupied++;
        if (!entry[4] || (entry[0] != 0 && entry[0] != 128))
            return fail(err, "Existing partition entry has ambiguous type or active flag");
        if (entry[4] == 0xee)
            return fail(err, "Protective GPT partition cannot be reconstructed as DOS MBR");
    }
    if (!zero_entry(plan->before + 446 + (slot - 1) * 16))
        return fail(err, "Choose an entirely empty MBR slot; existing entries are preserved");
    if (occupied && dt_word(plan->before + 510) != 0xaa55)
        return fail(err, "Existing partition table has an invalid signature");
    memset(&selected, 0, sizeof(selected));
    selected.start = start;
    selected.boot_source = boot_source;
    if (!dt_find_volumes(
            disk, start + boot_source, 1, select_volume, &selected, NULL, NULL, detail))
        return fail(err, "Candidate validation encountered unreadable metadata");
    if (selected.found != 1)
        return fail(err, "Selected location is not a corroborated FAT volume candidate");
    plan->disk = disk;
    plan->disk_sectors = disk->sectors;
    plan->start = start;
    plan->total = selected.volume.total;
    plan->slot = slot;
    plan->heads = heads;
    plan->sectors = sectors;
    plan->boot_source = boot_source;
    plan->bits = selected.volume.bits;
    memcpy(plan->volume_boot, selected.volume.boot, 512);
    memcpy(plan->after, plan->before, 512);
    entry = plan->after + 446 + (slot - 1) * 16;
    end = start + plan->total - 1;
    chs_limit = (dt_u64)heads * sectors * 1024;
    if (plan->bits == 12 && end >= chs_limit)
        return fail(err, "FAT12 partition lies beyond the supplied CHS geometry");
    entry[4] = plan->bits == 12   ? 1
               : plan->bits == 16 ? (end >= chs_limit      ? 0x0e
                                     : plan->total < 65536 ? 4
                                                           : 6)
                                  : (end >= chs_limit ? 0x0c : 0x0b);
    chs(entry + 1, start, heads, sectors);
    chs(entry + 5, end, heads, sectors);
    put32(entry + 8, start);
    put32(entry + 12, plan->total);
    plan->after[510] = 0x55;
    plan->after[511] = 0xaa;
    memset(&proposed, 0, sizeof(proposed));
    proposed.ctx = plan;
    proposed.sectors = disk->sectors;
    proposed.read = proposed_read;
    if (!dt_partitions(&proposed, partitions, &count, detail))
        return fail(err, detail);
    for (i = 0; i < count; i++)
        if (partitions[i].start == start && partitions[i].size == plan->total)
            matches++;
    if (matches != 1)
        return fail(err, "Proposed partition is not uniquely represented in the table");
    return 1;
}

/* Some filesystem probes treat an unreadable sector as an absent signature.
   Reconstruction must distinguish that from evidence that the signature is absent. */
typedef struct {
    DtDisk *disk;
    int failed;
} PlanReader;

static int checked_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    PlanReader *reader = (PlanReader *)ctx;

    if (sector >= reader->disk->sectors || !reader->disk->read(reader->disk->ctx, sector, data)) {
        reader->failed = 1;
        return 0;
    }
    return 1;
}

int dt_mbr_plan(DtDisk *disk, unsigned slot, dt_u32 start, unsigned boot_source, unsigned heads,
                unsigned sectors, DtMbrPlan *plan, char *err)
{
    DtDisk guarded;
    PlanReader reader;
    int ok;

    memset(plan, 0, sizeof(*plan));
    if (!disk || !disk->read)
        return fail(err, "Missing disk read interface");
    memset(&guarded, 0, sizeof(guarded));
    reader.disk = disk;
    reader.failed = 0;
    guarded.ctx = &reader;
    guarded.sectors = disk->sectors;
    guarded.read = checked_read;
    ok = build_plan(&guarded, slot, start, boot_source, heads, sectors, plan, err);
    if (reader.failed)
        ok = fail(err, "Partition reconstruction encountered unreadable metadata");
    if (ok)
        plan->disk = disk;
    else
        memset(plan, 0, sizeof(*plan));
    return ok;
}

/* Rebuild from current media, rather than trusting a caller-owned plan buffer. */
int dt_mbr_revalidate(const DtMbrPlan *plan, char *err)
{
    DtMbrPlan current;

    if (!plan || !plan->disk)
        return fail(err, "Missing reviewed partition plan");
    if (!dt_mbr_plan(plan->disk,
                     plan->slot,
                     plan->start,
                     plan->boot_source,
                     plan->heads,
                     plan->sectors,
                     &current,
                     err))
        return 0;
    if (current.disk_sectors != plan->disk_sectors || current.total != plan->total ||
        current.bits != plan->bits || memcmp(current.before, plan->before, 512) ||
        memcmp(current.after, plan->after, 512) ||
        memcmp(current.volume_boot, plan->volume_boot, 512))
        return fail(err, "Disk metadata or proposed partition changed since review");
    return 1;
}
