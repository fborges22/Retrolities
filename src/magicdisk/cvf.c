/* Read-only check of MS-DOS 6 DoubleSpace/DriveSpace compressed volume files
   (DBLSPACE.nnn / DRVSPACE.nnn), following the Tech Help! CVF layout:
   MDBPB, BitFAT, MDFAT, BootSect, FAT (single physical copy), RootDir (32
   sectors), Sector Heap, MdStamp1 'F8 D R 0' after BootSect and MdStamp2
   'MDR0' in the last complete sector. One-sector differences between
   "logical" and file sector numbers are resolved from the stamps. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *file;
    dt_u32 sectors;
    DtReport report;
    void *report_ctx;
    DtCvfResult *r;
} Cvf;

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static int sector(Cvf *c, dt_u32 n, unsigned char *out)
{
    return n < c->sectors && !fseek(c->file, (long)n * 512L, SEEK_SET) &&
           fread(out, 1, 512, c->file) == 512;
}

static void issue(Cvf *c, const char *code, dt_u32 where, const char *message)
{
    c->r->errors++;
    if (c->report)
        c->report(c->report_ctx, code, where, message);
}

static int inner_boot(const unsigned char *b)
{
    return dt_word(b + 11) == 512 && b[13] == 16 && dt_word(b + 14) >= 1 && b[21] == 0xf8 &&
           dt_word(b + 17) == 512 && dt_word(b + 22) >= 1;
}

static dt_u32 mdfat(const unsigned char *table, dt_u32 index)
{
    return dt_dword(table + index * 4);
}

static dt_u32 fat_value(const unsigned char *fat, int fat12, dt_u32 cluster)
{
    if (!fat12)
        return dt_word(fat + cluster * 2);
    {
        unsigned pair = dt_word(fat + cluster + cluster / 2);
        return cluster & 1 ? pair >> 4 : pair & 0xfff;
    }
}

typedef struct {
    dt_u32 cluster;
    char path[96];
} Pending;

int dt_cvf_check(const char *path, DtCvfResult *result, DtReport report, void *report_ctx,
                 DtProgress progress, void *ctx, char *err)
{
    Cvf c;
    unsigned char b[512], md[512], *bitfat = NULL, *table = NULL, *fat = NULL, *heap_owner = NULL,
                                   *owned = NULL, *dir = NULL;
    Pending *queue = NULL;
    dt_u32 boot, res, fatsz, root, heap, last, mdstart, entries, first_data, clusters, i, total;
    dt_u32 heap_count, end_mark, bad_mark, base_heap = 0, base_logical = 0;
    unsigned head = 0, tail = 0, capacity = 0;
    long length;
    char message[200];
    int fat12, ok = 0;

    memset(result, 0, sizeof(*result));
    memset(&c, 0, sizeof(c));
    c.r = result;
    c.report = report;
    c.report_ctx = report_ctx;
    c.file = fopen(path, "rb");
    if (!c.file)
        return fail(err, "Cannot open the compressed volume file");
    if (fseek(c.file, 0, SEEK_END) || (length = ftell(c.file)) < 512 * 80) {
        fclose(c.file);
        return fail(err, "File is too small to be a DoubleSpace/DriveSpace volume");
    }
    c.sectors = (dt_u32)(length / 512);
    if (!sector(&c, 0, md)) {
        fclose(c.file);
        return fail(err, "Cannot read the MDBPB sector");
    }
    memcpy(result->oem, md + 3, 8);
    result->drivespace = !memcmp(md + 3, "MSDSP6.0", 8);
    if (!result->drivespace && memcmp(md + 3, "MSDBL6.0", 8)) {
        fclose(c.file);
        return fail(err, "No MSDBL6.0/MSDSP6.0 MDBPB signature; not a DoubleSpace or DriveSpace "
                         "volume");
    }
    if (dt_word(md + 11) != 512 || md[13] != 16 || md[0x26] != 9 || md[0x32] != 4 ||
        md[0x3d] > 1 || !md[0x2f]) {
        fclose(c.file);
        return fail(err, "MDBPB geometry is not the DoubleSpace 512-byte/8 KB layout");
    }
    last = c.sectors - 1;
    if (!sector(&c, last, b) || memcmp(b, "MDR\0", 4))
        issue(&c, "CVF_STAMP", last, "MdStamp2 'MDR0' is missing from the last complete sector");
    /* The boot sector is followed by MdStamp1; accept either sector convention. */
    for (boot = dt_word(md + 0x27); boot <= (dt_u32)dt_word(md + 0x27) + 1; boot++) {
        unsigned char stamp[512];

        if (sector(&c, boot, b) && inner_boot(b) && sector(&c, boot + 1, stamp) &&
            !memcmp(stamp, "\xf8\x44\x52\x00", 4))
            break;
    }
    if (boot > (dt_u32)dt_word(md + 0x27) + 1) {
        issue(&c, "CVF_STAMP", dt_word(md + 0x27),
              "No valid inner boot sector followed by MdStamp1 at wMdResSects");
        fclose(c.file);
        result->complete = 0;
        return 1;
    }
    res = dt_word(b + 14);
    fatsz = dt_word(b + 22);
    total = dt_word(b + 19) ? dt_word(b + 19) : dt_dword(b + 32);
    fat12 = md[0x3d] == 1;
    result->fat12 = fat12;
    root = boot + res + fatsz;
    heap = root + 32 + 2;
    if (root != dt_word(md + 0x27) + dt_word(md + 0x29) &&
        root != dt_word(md + 0x27) + dt_word(md + 0x29) + 1)
        issue(&c, "CVF_LAYOUT", root, "MDBPB wRootStart disagrees with the inner FAT size");
    if (heap != dt_word(md + 0x2b) && heap != (dt_u32)dt_word(md + 0x2b) + 1)
        issue(&c, "CVF_LAYOUT", heap, "MDBPB wHeapStart is not two sectors after the root");
    mdstart = dt_word(md + 0x24) + 1;
    if (mdstart < 1 + (dt_u32)md[0x2f] * 4 || mdstart >= boot) {
        issue(&c, "CVF_LAYOUT", mdstart, "MDFAT overlaps the BitFAT or the boot sector");
        fclose(c.file);
        return 1;
    }
    first_data = dt_word(md + 0x2d);
    if (total < res + 2 * fatsz + 32 + 16) {
        issue(&c, "CVF_LAYOUT", boot, "Inner boot sector describes no data clusters");
        fclose(c.file);
        return 1;
    }
    clusters = (total - res - 2 * fatsz - 32) / 16;
    {
        dt_u32 capacity_entries = fat12 ? fatsz * 1024UL / 3 : fatsz * 256UL;

        if (clusters > (fat12 ? 4084UL : 65524UL))
            clusters = fat12 ? 4084UL : 65524UL;
        if (clusters + 2 > capacity_entries)
            clusters = capacity_entries - 2;
    }
    entries = (boot - mdstart) * 128;
    if (first_data + clusters + 2 > entries) {
        issue(&c, "CVF_LAYOUT", mdstart, "MDFAT is too small for the inner FAT's clusters");
        clusters = entries > first_data + 2 ? entries - first_data - 2 : 0;
    }
    result->clusters = clusters;
    end_mark = fat12 ? 0xff8 : 0xfff8;
    bad_mark = fat12 ? 0xff7 : 0xfff7;
    heap_count = last > heap ? last - heap : 0;
    result->heap_sectors = heap_count;
    table = (unsigned char *)malloc((size_t)(boot - mdstart) * 512);
    fat = (unsigned char *)malloc((size_t)fatsz * 512 + 2);
    bitfat = (unsigned char *)malloc((size_t)md[0x2f] * 2048);
    heap_owner = (unsigned char *)calloc((size_t)heap_count / 8 + 1, 1);
    owned = (unsigned char *)calloc((size_t)clusters / 8 + 2, 1);
    dir = (unsigned char *)malloc(32 * 512);
    if (!table || !fat || !bitfat || !heap_owner || !owned || !dir) {
        fail(err, "Insufficient memory for the compressed volume check");
        goto done;
    }
    memset(fat, 0, (size_t)fatsz * 512 + 2);
    for (i = 0; i < boot - mdstart; i++)
        if (!sector(&c, mdstart + i, table + i * 512))
            goto unreadable;
    for (i = 0; i < fatsz; i++)
        if (!sector(&c, boot + res + i, fat + i * 512))
            goto unreadable;
    for (i = 0; i < (dt_u32)md[0x2f] * 4; i++)
        if (!sector(&c, 1 + i, bitfat + i * 512))
            goto unreadable;
    if (fat[0] != 0xf8 || fat[1] != 0xff || (!fat12 && fat[2] != 0xff))
        issue(&c, "CVF_FAT_HEADER", boot + res, "Inner FAT reserved entries do not match F8");

    /* MDFAT against the FAT and the sector heap. */
    for (i = 2; i < clusters + 2; i++) {
        dt_u32 entry = mdfat(table, first_data + i), value = fat_value(fat, fat12, i);
        dt_u32 location = (entry & 0x1fffffUL) + 1, count = ((entry >> 22) & 15) + 1, s;
        int used = (entry >> 31) & 1, allocated = value && value != bad_mark;

        if (progress && !(i & 63) && !progress(ctx, "Check MDFAT", i - 2, clusters))
            goto cancelled;
        if (allocated)
            result->used++;
        if (!entry) {
            if (allocated)
                result->sparse++;
            continue;
        }
        if (entry & (1UL << 21)) {
            sprintf(message, "Cluster %lu: MDFAT reserved bit 21 is set", (unsigned long)i);
            issue(&c, "CVF_MDFAT", i, message);
        }
        if (allocated && !used) {
            sprintf(message, "Cluster %lu is allocated in the FAT but unused in the MDFAT",
                    (unsigned long)i);
            issue(&c, "CVF_MDFAT", i, message);
            continue;
        }
        if (!allocated && used) {
            sprintf(message, "Free FAT cluster %lu still holds %lu heap sectors in the MDFAT",
                    (unsigned long)i, (unsigned long)count);
            issue(&c, "CVF_MDFAT_STALE", i, message);
        }
        if (!used)
            continue;
        if ((entry >> 30) & 1)
            result->raw++;
        else
            result->compressed++;
        if (location < heap || location + count > last) {
            sprintf(message, "Cluster %lu data sectors %lu-%lu lie outside the sector heap",
                    (unsigned long)i, (unsigned long)location,
                    (unsigned long)(location + count - 1));
            issue(&c, "CVF_HEAP_RANGE", i, message);
            continue;
        }
        for (s = location; s < location + count; s++) {
            dt_u32 bit = s - heap;

            if (heap_owner[bit / 8] & (1u << (bit % 8))) {
                sprintf(message, "Cluster %lu shares heap sector %lu with another cluster",
                        (unsigned long)i, (unsigned long)s);
                issue(&c, "CVF_HEAP_CROSSLINK", s, message);
                break;
            }
            heap_owner[bit / 8] |= (unsigned char)(1u << (bit % 8));
            result->heap_used++;
        }
        if (!sector(&c, location, b)) {
            sprintf(message, "Cluster %lu heap sector %lu is unreadable", (unsigned long)i,
                    (unsigned long)location);
            issue(&c, "CVF_UNREADABLE", location, message);
        } else if (!((entry >> 30) & 1) &&
                   (b[0] != 'M' || b[1] != 'D' || b[2] != 0 || b[3] > 4)) {
            sprintf(message, "Compressed cluster %lu lacks its MD data tag at heap sector %lu",
                    (unsigned long)i, (unsigned long)location);
            issue(&c, "CVF_TAG", location, message);
        }
    }
    /* The BitFAT is rebuilt at mount; compare under both documented bases. */
    for (i = 0; i < heap_count; i++) {
        int want = (heap_owner[i / 8] >> (i % 8)) & 1;
        dt_u32 h = i, l = i + heap - 1;

        if (h / 8 < (dt_u32)md[0x2f] * 2048 &&
            (((bitfat[(h / 16) * 2 + 1 - (h % 16) / 8] >> (7 - h % 8)) & 1) != want))
            base_heap++;
        if (l / 8 < (dt_u32)md[0x2f] * 2048 &&
            (((bitfat[(l / 16) * 2 + 1 - (l % 16) / 8] >> (7 - l % 8)) & 1) != want))
            base_logical++;
    }
    result->bitfat_mismatch = base_heap < base_logical ? base_heap : base_logical;
    if (result->bitfat_mismatch && report) {
        sprintf(message, "%lu BitFAT bits disagree with the MDFAT (DoubleSpace rebuilds the "
                         "BitFAT at mount)", (unsigned long)result->bitfat_mismatch);
        report(report_ctx, "CVF_BITFAT", 1, message);
    }

    /* Inner FAT tree: root, then raw or sparse subdirectories. */
    result->complete = 1;
    capacity = 64;
    queue = (Pending *)malloc(capacity * sizeof(Pending));
    if (!queue) {
        fail(err, "Insufficient memory for the compressed volume check");
        goto done;
    }
    queue[tail].cluster = 0;
    strcpy(queue[tail++].path, "");
    while (head < tail) {
        Pending item = queue[head++];
        dt_u32 cluster = item.cluster, steps = 0, next;
        unsigned slots, k;

        for (;;) {
            if (!item.cluster) {
                slots = 512;
                for (i = 0; i < 32; i++)
                    if (!sector(&c, root + i, dir + i * 512))
                        goto unreadable;
            } else {
                dt_u32 entry = mdfat(table, first_data + cluster);

                slots = 256;
                memset(dir, 0, 16 * 512);
                if (entry && !((entry >> 30) & 1)) {
                    result->complete = 0;
                    result->compressed_dirs++;
                    break;
                }
                if (entry) {
                    dt_u32 location = (entry & 0x1fffffUL) + 1, count = ((entry >> 22) & 15) + 1;

                    for (i = 0; i < count && location + i < last; i++)
                        if (!sector(&c, location + i, dir + i * 512))
                            break;
                }
            }
            for (k = 0; k < slots; k++) {
                unsigned char *e = dir + k * 32;
                dt_u32 start, size, need, length_found = 0, walk, value;
                char name[16];

                if (!e[0])
                    break;
                if (e[0] == 0xe5 || (e[11] & 0x3f) == 0x0f || (e[11] & 8) || e[0] == '.')
                    continue;
                sprintf(name, "%.8s.%.3s", (char *)e, (char *)e + 8);
                start = dt_word(e + 26);
                size = dt_dword(e + 28);
                if (e[11] & 0x10) {
                    result->dirs++;
                    if (start >= 2 && start < clusters + 2) {
                        if (tail == capacity) {
                            Pending *grown = (Pending *)realloc(queue,
                                                                capacity * 2 * sizeof(Pending));

                            if (!grown) {
                                fail(err, "Insufficient memory for the compressed volume check");
                                goto done;
                            }
                            queue = grown;
                            capacity *= 2;
                        }
                        queue[tail].cluster = start;
                        sprintf(queue[tail++].path, "%.60s\\%.12s", item.path, name);
                    }
                } else
                    result->files++;
                for (walk = start; walk; walk = value) {
                    if (walk < 2 || walk >= clusters + 2) {
                        sprintf(message, "%.60s\\%.12s: chain leaves the data area",
                                item.path, name);
                        issue(&c, "CVF_CHAIN", walk, message);
                        break;
                    }
                    if (owned[walk / 8] & (1u << (walk % 8))) {
                        sprintf(message, "%.60s\\%.12s: cluster %lu is cross-linked",
                                item.path, name, (unsigned long)walk);
                        issue(&c, "CVF_CROSSLINK", walk, message);
                        break;
                    }
                    owned[walk / 8] |= (unsigned char)(1u << (walk % 8));
                    length_found++;
                    value = fat_value(fat, fat12, walk);
                    if (value >= end_mark)
                        break;
                    if (!value || value == bad_mark || value >= bad_mark - 7) {
                        sprintf(message, "%.60s\\%.12s: chain uses a free, bad or reserved "
                                         "cluster", item.path, name);
                        issue(&c, "CVF_CHAIN", walk, message);
                        break;
                    }
                }
                need = size / 8192 + (size % 8192 != 0);
                if (!(e[11] & 0x10) && need != length_found) {
                    sprintf(message, "%.60s\\%.12s: size %lu needs %lu clusters; chain has %lu",
                            item.path, name, (unsigned long)size, (unsigned long)need,
                            (unsigned long)length_found);
                    issue(&c, "CVF_FILE_SIZE", start, message);
                }
            }
            if (!item.cluster)
                break;
            next = fat_value(fat, fat12, cluster);
            if (next >= end_mark || next < 2 || next >= clusters + 2 || ++steps > clusters)
                break;
            cluster = next;
        }
    }
    if (result->complete) {
        for (i = 2; i < clusters + 2; i++) {
            dt_u32 value = fat_value(fat, fat12, i);

            if (value && value != bad_mark && !(owned[i / 8] & (1u << (i % 8))))
                result->lost++;
        }
        if (result->lost) {
            sprintf(message, "%lu allocated clusters belong to no file or directory",
                    (unsigned long)result->lost);
            issue(&c, "CVF_LOST", 0, message);
        }
    } else if (report) {
        sprintf(message, "%lu compressed directories were not expanded; lost-cluster analysis "
                         "skipped", (unsigned long)result->compressed_dirs);
        report(report_ctx, "CVF_PARTIAL", 0, message);
    }
    ok = 1;
    goto done;
unreadable:
    fail(err, "A CVF metadata sector is unreadable");
    goto done;
cancelled:
    fail(err, "Compressed volume check cancelled");
done:
    free(queue);
    free(dir);
    free(owned);
    free(heap_owner);
    free(bitfat);
    free(fat);
    free(table);
    fclose(c.file);
    return ok;
}
