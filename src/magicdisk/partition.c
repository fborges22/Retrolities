/* Whole-disk partition previews are separate from filesystem-relative plans. */
#include "disk.h"
#include <string.h>
#include <stdlib.h>

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

static int extended_type(unsigned type)
{
    return type == 5 || type == 15 || type == 0x85;
}

static int ebr_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    DtEbrPlan *plan = (DtEbrPlan *)ctx;

    if (sector == plan->sector) {
        memcpy(data, plan->entry.after, 512);
        return 1;
    }
    return plan->entry.disk->read(plan->entry.disk->ctx, sector, data);
}

static int build_ebr(DtDisk *disk, dt_u32 sector, dt_u32 start, unsigned boot_source,
                     unsigned heads, unsigned sectors, DtEbrPlan *plan, char *err)
{
    unsigned char header[512], record[512], *entry;
    dt_u32 base = 0, span = 0, at, relative, size, seen[128], end;
    dt_u64 chs_limit;
    unsigned i, j, seen_count = 0, count, matches = 0, found = 0;
    SelectedVolume selected;
    DtPartition partitions[128];
    DtVolume superfloppy;
    DtDisk proposed;
    DtMbrPlan *result = &plan->entry;
    char detail[160];

    if (!sector || sector >= disk->sectors || start <= sector || start >= disk->sectors ||
        boot_source >= disk->sectors - start || !heads || heads > 256 || !sectors || sectors > 63)
        return fail(err, "Invalid EBR sector, volume extent or CHS geometry");
    if (!disk->read(disk->ctx, 0, plan->mbr) || !disk->read(disk->ctx, 1, header))
        return fail(err, "Cannot read partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "GPT disks cannot be reconstructed as DOS EBR disks");
    if (!disk->read(disk->ctx, disk->sectors - 1, header))
        return fail(err, "Cannot read backup partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "Backup GPT header prevents DOS EBR reconstruction");
    if (dt_word(plan->mbr + 510) != 0xaa55)
        return fail(err, "A valid MBR signature is required for EBR reconstruction");
    if (dt_mount(disk, 0, disk->sectors, &superfloppy, detail))
        return fail(err, "Sector zero is a FAT volume, not an MBR");
    for (i = 0; i < 4; i++) {
        entry = plan->mbr + 446 + i * 16;
        if (zero_entry(entry))
            continue;
        if (!entry[4] || (entry[0] && entry[0] != 128) || entry[4] == 0xee)
            return fail(err, "Ambiguous or protective MBR entry");
        if (!extended_type(entry[4]))
            continue;
        if (base)
            return fail(err, "Multiple extended containers");
        base = dt_dword(entry + 8);
        span = dt_dword(entry + 12);
        if (!base || base >= disk->sectors || !span || span > disk->sectors - base)
            return fail(err, "Invalid extended container");
    }
    if (!base || sector < base || sector - base >= span)
        return fail(err, "Selected EBR is outside the extended container");
    at = base;
    for (;;) {
        if (seen_count == 128)
            return fail(err, "Extended chain too long");
        for (j = 0; j < seen_count; j++)
            if (seen[j] == at)
                return fail(err, "Loop in extended partition chain");
        seen[seen_count++] = at;
        if (!disk->read(disk->ctx, at, record))
            return fail(err, "Cannot read extended partition record");
        if (!zero_entry(record + 478) || !zero_entry(record + 494))
            return fail(err, "Ambiguous extra entries in extended record");
        if (!zero_entry(record + 446) &&
            (!record[450] || extended_type(record[450]) || record[450] == 0xee ||
             (record[446] && record[446] != 128)))
            return fail(err, "Ambiguous logical partition entry");
        if (at == sector) {
            memcpy(result->before, record, 512);
            found = 1;
        }
        if (dt_word(record + 510) != 0xaa55 &&
            (at != sector || !zero_entry(record + 446) || !zero_entry(record + 462)))
            return fail(err, "Existing EBR has an invalid signature");
        entry = record + 462;
        if (zero_entry(entry))
            break;
        relative = dt_dword(entry + 8);
        size = dt_dword(entry + 12);
        if (!extended_type(entry[4]) || entry[0] || !relative || relative >= span || !size ||
            size > span - relative)
            return fail(err, "Selected EBR is not reachable through a valid existing link");
        at = base + relative;
    }
    if (!found)
        return fail(err, "Selected EBR is not reachable through the existing chain");
    if (!zero_entry(result->before + 446))
        return fail(err, "Logical partition entry must be entirely empty");
    memset(&selected, 0, sizeof(selected));
    selected.start = start;
    selected.boot_source = boot_source;
    if (!dt_find_volumes(
            disk, start + boot_source, 1, select_volume, &selected, NULL, NULL, detail))
        return fail(err, "Candidate validation encountered unreadable metadata");
    if (selected.found != 1)
        return fail(err, "Selected location is not a corroborated FAT volume candidate");
    if (start - base >= span || selected.volume.total > span - (start - base))
        return fail(err, "Logical partition extends outside its container");
    result->disk = disk;
    result->disk_sectors = disk->sectors;
    result->slot = 1;
    result->start = start;
    result->total = selected.volume.total;
    result->bits = selected.volume.bits;
    result->boot_source = boot_source;
    result->heads = heads;
    result->sectors = sectors;
    memcpy(result->volume_boot, selected.volume.boot, 512);
    memcpy(result->after, result->before, 512);
    plan->sector = sector;
    entry = result->after + 446;
    end = start + result->total - 1;
    chs_limit = (dt_u64)heads * sectors * 1024;
    if (result->bits == 12 && end >= chs_limit)
        return fail(err, "FAT12 partition lies beyond the supplied CHS geometry");
    entry[4] = result->bits == 12   ? 1
               : result->bits == 16 ? (end >= chs_limit        ? 0x0e
                                       : result->total < 65536 ? 4
                                                               : 6)
                                    : (end >= chs_limit ? 0x0c : 0x0b);
    chs(entry + 1, start, heads, sectors);
    chs(entry + 5, end, heads, sectors);
    put32(entry + 8, start - sector);
    put32(entry + 12, result->total);
    result->after[510] = 0x55;
    result->after[511] = 0xaa;
    memset(&proposed, 0, sizeof(proposed));
    proposed.ctx = plan;
    proposed.sectors = disk->sectors;
    proposed.read = ebr_read;
    if (!dt_partitions(&proposed, partitions, &count, detail))
        return fail(err, detail);
    for (i = 0; i < count; i++)
        if (partitions[i].start == start && partitions[i].size == result->total)
            matches++;
    if (matches != 1)
        return fail(err, "Proposed logical partition is not uniquely represented");
    return 1;
}

int dt_ebr_plan(DtDisk *disk, dt_u32 sector, dt_u32 start, unsigned boot_source, unsigned heads,
                unsigned sectors, DtEbrPlan *plan, char *err)
{
    DtDisk guarded;
    PlanReader reader;
    int ok;

    memset(plan, 0, sizeof(*plan));
    err[0] = 0;
    if (!disk || !disk->read)
        return fail(err, "Missing disk read interface");
    memset(&guarded, 0, sizeof(guarded));
    reader.disk = disk;
    reader.failed = 0;
    guarded.ctx = &reader;
    guarded.sectors = disk->sectors;
    guarded.read = checked_read;
    ok = build_ebr(&guarded, sector, start, boot_source, heads, sectors, plan, err);
    if (reader.failed)
        ok = fail(err, "EBR reconstruction encountered unreadable metadata");
    if (ok)
        plan->entry.disk = disk;
    else
        memset(plan, 0, sizeof(*plan));
    return ok;
}

int dt_ebr_revalidate(const DtEbrPlan *plan, char *err)
{
    DtEbrPlan current;
    const DtMbrPlan *entry;

    if (!plan || !plan->entry.disk)
        return fail(err, "Missing reviewed EBR plan");
    entry = &plan->entry;
    if (!dt_ebr_plan(entry->disk,
                     plan->sector,
                     entry->start,
                     entry->boot_source,
                     entry->heads,
                     entry->sectors,
                     &current,
                     err))
        return 0;
    if (entry->slot != 1 || current.entry.disk_sectors != entry->disk_sectors ||
        current.entry.total != entry->total || current.entry.bits != entry->bits ||
        memcmp(current.mbr, plan->mbr, 512) || memcmp(current.entry.before, entry->before, 512) ||
        memcmp(current.entry.after, entry->after, 512) ||
        memcmp(current.entry.volume_boot, entry->volume_boot, 512))
        return fail(err, "Disk metadata or proposed logical partition changed since review");
    return 1;
}

static int link_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    DtEbrLinkPlan *plan = (DtEbrLinkPlan *)ctx;

    if (sector == plan->sector) {
        memcpy(data, plan->after, 512);
        return 1;
    }
    return plan->disk->read(plan->disk->ctx, sector, data);
}

static int link_record(DtDisk *disk, dt_u32 at, dt_u32 base, dt_u32 span, unsigned char *record,
                       dt_u32 *next, char *err)
{
    const unsigned char *entry;
    dt_u32 relative, count;

    if (at < base || at - base >= span || !disk->read(disk->ctx, at, record) ||
        dt_word(record + 510) != 0xaa55)
        return fail(err, "Unreadable or invalid EBR in link reconstruction");
    if (!zero_entry(record + 478) || !zero_entry(record + 494))
        return fail(err, "Ambiguous extra EBR entries");
    entry = record + 446;
    if (!entry[4] || extended_type(entry[4]) || entry[4] == 0xee || (entry[0] && entry[0] != 128))
        return fail(err, "Restore a valid logical entry before reconstructing its link");
    relative = dt_dword(entry + 8);
    count = dt_dword(entry + 12);
    if (!relative || relative >= base + span - at || !count || count > base + span - at - relative)
        return fail(err, "Logical volume exceeds its extended container");
    entry = record + 462;
    *next = 0;
    if (zero_entry(entry))
        return 1;
    relative = dt_dword(entry + 8);
    count = dt_dword(entry + 12);
    if (!extended_type(entry[4]) || entry[0] || !relative || relative >= span || !count ||
        count > span - relative)
        return fail(err, "Invalid existing EBR link");
    *next = base + relative;
    return 1;
}

static int build_link(DtDisk *disk, dt_u32 sector, dt_u32 target, unsigned heads, unsigned sectors,
                      DtEbrLinkPlan *plan, char *err)
{
    DtPartition partitions[128];
    DtDisk proposed;
    SelectedVolume selected;
    DtVolume superfloppy;
    unsigned char record[512], header[512], *entry;
    dt_u32 base = 0, span = 0, at, next, seen[128], start, length;
    unsigned count, original_count, i, seen_count = 0, type = 0;
    char detail[160];

    if (!sector || !target || sector == target || sector >= disk->sectors ||
        target >= disk->sectors || !heads || heads > 256 || !sectors || sectors > 63)
        return fail(err, "Invalid EBR link locations or CHS geometry");
    if (!disk->read(disk->ctx, 0, plan->mbr) || !disk->read(disk->ctx, 1, header))
        return fail(err, "Cannot read disk partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "GPT disks cannot be reconstructed as DOS EBR chains");
    if (!disk->read(disk->ctx, disk->sectors - 1, header))
        return fail(err, "Cannot read backup partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "Backup GPT header prevents DOS EBR link reconstruction");
    if (dt_mount(disk, 0, disk->sectors, &superfloppy, detail))
        return fail(err, "Sector zero is a FAT volume, not an MBR");
    if (!dt_partitions(disk, partitions, &original_count, detail))
        return fail(err, detail);
    for (i = 0; i < 4; i++) {
        entry = plan->mbr + 446 + i * 16;
        if (zero_entry(entry))
            continue;
        if (!entry[4] || (entry[0] && entry[0] != 128))
            return fail(err, "Ambiguous MBR entry");
        if (extended_type(entry[4])) {
            base = dt_dword(entry + 8);
            span = dt_dword(entry + 12);
            type = entry[4];
        }
    }
    if (!base || sector < base || sector - base >= span || target < base || target - base >= span)
        return fail(err, "EBR link locations must lie within one existing container");
    at = base;
    for (;;) {
        if (seen_count == 128)
            return fail(err, "Extended chain too long");
        for (i = 0; i < seen_count; i++)
            if (seen[i] == at)
                return fail(err, "Loop in existing EBR chain");
        seen[seen_count++] = at;
        if (at == target)
            return fail(err, "Target EBR is already reachable");
        if (!link_record(disk, at, base, span, record, &next, err))
            return 0;
        if (at == sector) {
            if (next)
                return fail(err, "Selected EBR link must be entirely empty");
            memcpy(plan->before, record, 512);
            break;
        }
        if (!next)
            return fail(err, "Source EBR is not reachable");
        at = next;
    }
    plan->witness = (DtLinkWitness *)calloc(128, sizeof(DtLinkWitness));
    if (!plan->witness)
        return fail(err, "Insufficient memory for reviewed EBR chain metadata");
    /* Every newly linked logical volume must have corroborated FAT geometry. */
    at = target;
    for (;;) {
        if (seen_count == 128)
            return fail(err, "Proposed extended chain too long");
        for (i = 0; i < seen_count; i++)
            if (seen[i] == at)
                return fail(err, "Proposed EBR link creates a loop");
        seen[seen_count++] = at;
        if (!link_record(disk, at, base, span, record, &next, err))
            return 0;
        start = at + dt_dword(record + 454);
        length = dt_dword(record + 458);
        memset(&selected, 0, sizeof(selected));
        selected.start = start;
        if (!dt_find_volumes(disk, start, 1, select_volume, &selected, NULL, NULL, detail))
            return fail(err, "Linked volume validation encountered unreadable metadata");
        if (selected.found != 1 || selected.volume.total > length)
            return fail(err, "Linked EBR does not describe a corroborated FAT volume");
        if ((selected.volume.bits == 12 && record[450] != 1) ||
            (selected.volume.bits == 16 && record[450] != 4 && record[450] != 6 &&
             record[450] != 14) ||
            (selected.volume.bits == 32 && record[450] != 11 && record[450] != 12))
            return fail(err, "Linked partition type disagrees with its FAT geometry");
        plan->witness[plan->witness_count].record_sector = at;
        plan->witness[plan->witness_count].volume_start = start;
        memcpy(plan->witness[plan->witness_count].record, record, 512);
        memcpy(plan->witness[plan->witness_count].boot, selected.volume.boot, 512);
        plan->witness_count++;
        if (!next)
            break;
        at = next;
    }
    plan->disk = disk;
    plan->disk_sectors = disk->sectors;
    plan->sector = sector;
    plan->target = target;
    plan->base = base;
    plan->span = span;
    plan->heads = heads;
    plan->sectors = sectors;
    memcpy(plan->after, plan->before, 512);
    entry = plan->after + 462;
    entry[4] = (unsigned char)type;
    chs(entry + 1, target, heads, sectors);
    chs(entry + 5, base + span - 1, heads, sectors);
    put32(entry + 8, target - base);
    put32(entry + 12, span - (target - base));
    memset(&proposed, 0, sizeof(proposed));
    proposed.ctx = plan;
    proposed.sectors = disk->sectors;
    proposed.read = link_read;
    if (!dt_partitions(&proposed, partitions, &count, detail))
        return fail(err, detail);
    if (count <= original_count)
        return fail(err, "Proposed link does not recover a logical volume");
    plan->recovered = count - original_count;
    return 1;
}

int dt_ebr_link_plan(DtDisk *disk, dt_u32 sector, dt_u32 target, unsigned heads, unsigned sectors,
                     DtEbrLinkPlan *plan, char *err)
{
    DtDisk guarded;
    PlanReader reader;
    int ok;

    memset(plan, 0, sizeof(*plan));
    err[0] = 0;
    if (!disk || !disk->read)
        return fail(err, "Missing disk read interface");
    memset(&guarded, 0, sizeof(guarded));
    reader.disk = disk;
    reader.failed = 0;
    guarded.ctx = &reader;
    guarded.sectors = disk->sectors;
    guarded.read = checked_read;
    ok = build_link(&guarded, sector, target, heads, sectors, plan, err);
    if (reader.failed)
        ok = fail(err, "EBR link reconstruction encountered unreadable metadata");
    if (ok)
        plan->disk = disk;
    else
        dt_ebr_link_clear(plan);
    return ok;
}

void dt_ebr_link_clear(DtEbrLinkPlan *plan)
{
    if (plan) {
        free(plan->witness);
        memset(plan, 0, sizeof(*plan));
    }
}

int dt_ebr_link_revalidate(const DtEbrLinkPlan *plan, char *err)
{
    DtEbrLinkPlan current;
    unsigned i;
    int same;

    if (!plan || !plan->disk || !plan->witness || !plan->witness_count || plan->witness_count > 128)
        return fail(err, "Missing reviewed EBR chain plan");
    if (!dt_ebr_link_plan(
            plan->disk, plan->sector, plan->target, plan->heads, plan->sectors, &current, err))
        return 0;
    same = current.disk_sectors == plan->disk_sectors && current.base == plan->base &&
           current.span == plan->span && current.recovered == plan->recovered &&
           current.witness_count == plan->witness_count && !memcmp(current.mbr, plan->mbr, 512) &&
           !memcmp(current.before, plan->before, 512) && !memcmp(current.after, plan->after, 512);
    for (i = 0; same && i < current.witness_count; i++) {
        const DtLinkWitness *a = &current.witness[i], *b = &plan->witness[i];

        same = a->record_sector == b->record_sector && a->volume_start == b->volume_start &&
               !memcmp(a->record, b->record, 512) && !memcmp(a->boot, b->boot, 512);
    }
    dt_ebr_link_clear(&current);
    return same ? 1 : fail(err, "Extended chain metadata changed since review");
}

/* A missing container needs an explicit extent; surviving links do not prove
   how much unused space originally belonged to the extended partition. */
static int container_read(void *ctx, dt_u32 sector, unsigned char *data)
{
    DtContainerPlan *plan = (DtContainerPlan *)ctx;

    if (!sector) {
        memcpy(data, plan->after, 512);
        return 1;
    }
    return plan->disk->read(plan->disk->ctx, sector, data);
}

static int build_container(DtDisk *disk, unsigned slot, dt_u32 base, dt_u32 span, unsigned type,
                           unsigned heads, unsigned sectors, DtContainerPlan *plan, char *err)
{
    DtPartition partitions[128];
    DtVolume superfloppy;
    SelectedVolume selected;
    DtDisk proposed;
    unsigned char header[512], record[512], *entry;
    dt_u32 at, next, start, length;
    unsigned i, count, occupied = 0;
    char detail[160];

    if (slot < 1 || slot > 4 || !base || base >= disk->sectors || !span ||
        span > disk->sectors - base || !extended_type(type) || !heads || heads > 256 || !sectors ||
        sectors > 63)
        return fail(err, "Invalid container slot, extent, type or CHS geometry");
    if (!disk->read(disk->ctx, 0, plan->before) || !disk->read(disk->ctx, 1, header))
        return fail(err, "Cannot read disk partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "GPT prevents DOS extended-container reconstruction");
    if (!disk->read(disk->ctx, disk->sectors - 1, header))
        return fail(err, "Cannot read backup partition identity");
    if (!memcmp(header, "EFI PART", 8))
        return fail(err, "Backup GPT prevents DOS extended-container reconstruction");
    if (dt_mount(disk, 0, disk->sectors, &superfloppy, detail))
        return fail(err, "Sector zero is a FAT volume, not an MBR");
    for (i = 0; i < 4; i++) {
        entry = plan->before + 446 + i * 16;
        if (zero_entry(entry))
            continue;
        occupied++;
        if (!entry[4] || (entry[0] && entry[0] != 128) || entry[4] == 0xee)
            return fail(err, "Ambiguous or protective MBR entry");
        if (extended_type(entry[4]))
            return fail(err, "An extended container already exists");
    }
    if (!zero_entry(plan->before + 446 + (slot - 1) * 16))
        return fail(err, "Choose an entirely empty MBR slot");
    if (occupied && dt_word(plan->before + 510) != 0xaa55)
        return fail(err, "Existing partition table has an invalid signature");
    plan->witness = (DtLinkWitness *)calloc(128, sizeof(DtLinkWitness));
    if (!plan->witness)
        return fail(err, "Insufficient memory for container review metadata");
    at = base;
    for (;;) {
        DtLinkWitness *witness;

        if (plan->witness_count == 128)
            return fail(err, "Extended chain too long");
        for (i = 0; i < plan->witness_count; i++)
            if (plan->witness[i].record_sector == at)
                return fail(err, "Loop in recovered extended chain");
        if (!link_record(disk, at, base, span, record, &next, err))
            return 0;
        start = at + dt_dword(record + 454);
        length = dt_dword(record + 458);
        memset(&selected, 0, sizeof(selected));
        selected.start = start;
        if (!dt_find_volumes(disk, start, 1, select_volume, &selected, NULL, NULL, detail))
            return fail(err, "Recovered volume metadata is unreadable");
        if (selected.found != 1 || selected.volume.total > length)
            return fail(err, "Recovered entry is not a corroborated FAT volume");
        if ((selected.volume.bits == 12 && record[450] != 1) ||
            (selected.volume.bits == 16 && record[450] != 4 && record[450] != 6 &&
             record[450] != 14) ||
            (selected.volume.bits == 32 && record[450] != 11 && record[450] != 12))
            return fail(err, "Recovered partition type disagrees with FAT geometry");
        witness = &plan->witness[plan->witness_count++];
        witness->record_sector = at;
        witness->volume_start = start;
        memcpy(witness->record, record, 512);
        memcpy(witness->boot, selected.volume.boot, 512);
        if (!next)
            break;
        at = next;
    }
    plan->disk = disk;
    plan->disk_sectors = disk->sectors;
    plan->slot = slot;
    plan->base = base;
    plan->span = span;
    plan->type = type;
    plan->heads = heads;
    plan->sectors = sectors;
    memcpy(plan->after, plan->before, 512);
    entry = plan->after + 446 + (slot - 1) * 16;
    entry[4] = (unsigned char)type;
    chs(entry + 1, base, heads, sectors);
    chs(entry + 5, base + span - 1, heads, sectors);
    put32(entry + 8, base);
    put32(entry + 12, span);
    plan->after[510] = 0x55;
    plan->after[511] = 0xaa;
    memset(&proposed, 0, sizeof(proposed));
    proposed.ctx = plan;
    proposed.sectors = disk->sectors;
    proposed.read = container_read;
    if (!dt_partitions(&proposed, partitions, &count, detail))
        return fail(err, detail);
    if (count != occupied + plan->witness_count)
        return fail(err, "Recovered container has ambiguous partition membership");
    return 1;
}

void dt_container_clear(DtContainerPlan *plan)
{
    if (plan) {
        free(plan->witness);
        memset(plan, 0, sizeof(*plan));
    }
}

int dt_container_plan(DtDisk *disk, unsigned slot, dt_u32 base, dt_u32 span, unsigned type,
                      unsigned heads, unsigned sectors, DtContainerPlan *plan, char *err)
{
    DtDisk guarded;
    PlanReader reader;
    int ok;

    memset(plan, 0, sizeof(*plan));
    err[0] = 0;
    if (!disk || !disk->read)
        return fail(err, "Missing disk read interface");
    memset(&guarded, 0, sizeof(guarded));
    reader.disk = disk;
    reader.failed = 0;
    guarded.ctx = &reader;
    guarded.sectors = disk->sectors;
    guarded.read = checked_read;
    ok = build_container(&guarded, slot, base, span, type, heads, sectors, plan, err);
    if (reader.failed)
        ok = fail(err, "Container reconstruction encountered unreadable metadata");
    if (ok)
        plan->disk = disk;
    else
        dt_container_clear(plan);
    return ok;
}

int dt_container_revalidate(const DtContainerPlan *plan, char *err)
{
    DtContainerPlan current;
    unsigned i;
    int same;

    if (!plan || !plan->disk || !plan->witness || !plan->witness_count || plan->witness_count > 128)
        return fail(err, "Missing reviewed container plan");
    if (!dt_container_plan(plan->disk,
                           plan->slot,
                           plan->base,
                           plan->span,
                           plan->type,
                           plan->heads,
                           plan->sectors,
                           &current,
                           err))
        return 0;
    same = current.disk_sectors == plan->disk_sectors &&
           current.witness_count == plan->witness_count &&
           !memcmp(current.before, plan->before, 512) && !memcmp(current.after, plan->after, 512);
    for (i = 0; same && i < current.witness_count; i++) {
        const DtLinkWitness *a = &current.witness[i], *b = &plan->witness[i];

        same = a->record_sector == b->record_sector && a->volume_start == b->volume_start &&
               !memcmp(a->record, b->record, 512) && !memcmp(a->boot, b->boot, 512);
    }
    dt_container_clear(&current);
    return same ? 1 : fail(err, "Container or recovered chain changed since review");
}

void dt_ebr_record_clear(DtEbrRecordPlan *plan)
{
    if (plan) {
        free(plan->witness);
        memset(plan, 0, sizeof(*plan));
    }
}

int dt_ebr_record_plan(DtDisk *disk, dt_u32 sector, dt_u32 start, unsigned boot_source, dt_u32 next,
                       unsigned heads, unsigned sectors, DtEbrRecordPlan *plan, char *err)
{
    DtDisk proposed;
    DtEbrLinkPlan link;
    unsigned i;

    memset(plan, 0, sizeof(*plan));
    if (!dt_ebr_plan(disk, sector, start, boot_source, heads, sectors, &plan->logical, err))
        return 0;
    for (i = 0; i < 4; i++) {
        if (!zero_entry(plan->logical.entry.before + 446 + i * 16)) {
            fail(err, "Whole EBR reconstruction requires four entirely empty entries");
            goto failed;
        }
    }
    plan->next = next;
    memcpy(plan->after, plan->logical.entry.after, 512);
    if (!next)
        return 1;
    /* Validate the onward link against a read-only view of the recovered
       logical entry. The real disk is unchanged throughout both previews. */
    memset(&proposed, 0, sizeof(proposed));
    proposed.ctx = &plan->logical;
    proposed.sectors = disk->sectors;
    proposed.read = ebr_read;
    if (!dt_ebr_link_plan(&proposed, sector, next, heads, sectors, &link, err))
        goto failed;
    memcpy(plan->after, link.after, 512);
    plan->witness_count = link.witness_count;
    plan->witness = link.witness;
    link.witness = NULL;
    dt_ebr_link_clear(&link);
    return 1;
failed:
    dt_ebr_record_clear(plan);
    return 0;
}

int dt_ebr_record_revalidate(const DtEbrRecordPlan *plan, char *err)
{
    DtEbrRecordPlan current;
    const DtMbrPlan *entry;
    unsigned i;
    int same;

    if (!plan || !plan->logical.entry.disk || plan->witness_count > 128 ||
        (plan->next && (!plan->witness || !plan->witness_count)) ||
        (!plan->next && (plan->witness || plan->witness_count)))
        return fail(err, "Missing or inconsistent reviewed EBR record plan");
    entry = &plan->logical.entry;
    if (!dt_ebr_record_plan(entry->disk,
                            plan->logical.sector,
                            entry->start,
                            entry->boot_source,
                            plan->next,
                            entry->heads,
                            entry->sectors,
                            &current,
                            err))
        return 0;
    same = entry->slot == 1 && current.logical.entry.disk_sectors == entry->disk_sectors &&
           current.logical.entry.total == entry->total &&
           current.logical.entry.bits == entry->bits &&
           current.witness_count == plan->witness_count &&
           !memcmp(current.logical.mbr, plan->logical.mbr, 512) &&
           !memcmp(current.logical.entry.before, entry->before, 512) &&
           !memcmp(current.logical.entry.after, entry->after, 512) &&
           !memcmp(current.logical.entry.volume_boot, entry->volume_boot, 512) &&
           !memcmp(current.after, plan->after, 512);
    for (i = 0; same && i < current.witness_count; i++) {
        const DtLinkWitness *a = &current.witness[i], *b = &plan->witness[i];

        same = a->record_sector == b->record_sector && a->volume_start == b->volume_start &&
               !memcmp(a->record, b->record, 512) && !memcmp(a->boot, b->boot, 512);
    }
    dt_ebr_record_clear(&current);
    return same ? 1 : fail(err, "EBR record or recovered chain changed since review");
}
