/* Export unowned allocation chains without modifying their source filesystem. */
#include "disk.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __WATCOMC__
#include <direct.h>
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

#define PART_BYTES 0x40000000UL

typedef struct {
    DtVolume *volume;
    DtRecovery *result;
    unsigned char *lost, *incoming, *visited;
    FILE *manifest, *output;
    dt_u32 bytes;
    char file[16];
    DtProgress progress;
    void *ctx;
} Recovery;

static int fail(char *err, const char *message)
{
    strcpy(err, message);
    return 0;
}

static int has(const unsigned char *map, dt_u32 cluster)
{
    return (map[cluster / 8] >> (cluster % 8)) & 1;
}

static void set(unsigned char *map, dt_u32 cluster)
{
    map[cluster / 8] |= (unsigned char)(1U << (cluster % 8));
}

static int join(char *out, size_t capacity, const char *directory, const char *name)
{
    size_t length = strlen(directory);

    if (!length || length + strlen(name) + 2 > capacity)
        return 0;
    strcpy(out, directory);
    if (out[length - 1] != '/' && out[length - 1] != '\\')
        out[length++] = SEPARATOR;
    strcpy(out + length, name);
    return 1;
}

static int make_directory(char *path, const char *destination)
{
    unsigned index;
    char name[16];
    struct stat existing;

    for (index = 1; index <= 99999; index++) {
        sprintf(name, "REC%05u", index);
        if (!join(path, 260, destination, name))
            break;
#ifdef __WATCOMC__
        if (!mkdir(path))
#else
        if (!mkdir(path, 0700))
#endif
            return dt_file_commit_directory(path);
        /* DOS mkdir may report EACCES for an existing directory. */
        if (errno != EEXIST && stat(path, &existing))
            break;
    }
    path[0] = 0;
    return 0;
}

static void recovery_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    int *unsafe = (int *)ctx;

    (void)where;
    (void)message;
    if (strcmp(code, "LOST_CLUSTERS") && strcmp(code, "FAT_RANGE") && strcmp(code, "FSINFO_FREE") &&
        strcmp(code, "FSINFO_NEXT") && strcmp(code, "FSINFO_SIGNATURE") &&
        strcmp(code, "FSINFO_RESERVED") && strcmp(code, "BOOT_BACKUP") &&
        strcmp(code, "DIR_TIMESTAMP"))
        *unsafe = 1;
}

static int finish_file(Recovery *r)
{
    int ok = 1;

    if (r->output) {
        ok = dt_file_commit(r->output);
        if (fclose(r->output))
            ok = 0;
        r->output = NULL;
        if (fprintf(r->manifest,
                    "FILE_END %s bytes=%lu committed=%s\n",
                    r->file,
                    (unsigned long)r->bytes,
                    ok ? "yes" : "NO") < 0)
            ok = 0;
    }
    if (!dt_file_commit(r->manifest))
        ok = 0;
    return ok;
}

static int begin_file(Recovery *r)
{
    char path[280];

    if (r->result->files == 0x0fffffffUL)
        return 0;
    sprintf(r->file, "F%07lX.CHK", (unsigned long)(r->result->files + 1));
    if (!join(path, sizeof(path), r->result->directory, r->file))
        return 0;
    r->output = dt_file_create(path);
    if (!r->output)
        return 0;
    r->result->files++;
    r->bytes = 0;
    return fprintf(r->manifest, "FILE_BEGIN %s\n", r->file) >= 0;
}

/* Retry only payload reads: metadata failures still stop ownership analysis.
   -1 means cancelled/output failure, 0 means three failed reads, 1 means data. */
static int read_payload(Recovery *r, dt_u32 sector, unsigned char *data, char *err)
{
    unsigned attempt;
    int readable;

    for (attempt = 1; attempt <= 3; attempt++) {
        if (attempt > 1 && r->progress &&
            !r->progress(r->ctx, "Retry recovery sector", attempt - 1, 3)) {
            fail(err, "Recovery retry cancelled; partial files retained");
            return -1;
        }
        readable = dt_read(r->volume, sector, data);
        if (attempt > 1 &&
            fprintf(r->manifest,
                    "READ_RETRY file=%s offset=%lu disk_lba=%lu attempt=%u result=%s\n",
                    r->file,
                    (unsigned long)r->bytes,
                    (unsigned long)(r->volume->start + sector),
                    attempt,
                    readable ? "readable" : "failed") < 0) {
            fail(err, "Cannot record recovery retry in manifest");
            return -1;
        }
        if (readable)
            return 1;
    }
    return 0;
}

static int export_chain(Recovery *r, dt_u32 cluster, char *err)
{
    DtVolume *v = r->volume;
    dt_u32 next, sector, start, bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7;
    unsigned char data[512];
    int readable;
    const char *end = "end-of-chain";

    if (fprintf(r->manifest, "CHAIN_BEGIN cluster=%lu\n", (unsigned long)cluster) < 0)
        return fail(err, "Cannot write recovery manifest");
    while (cluster >= 2 && cluster < v->clusters + 2 && has(r->lost, cluster) &&
           !has(r->visited, cluster)) {
        if (!dt_fat(v, cluster, &next))
            return fail(err, "FAT read failed during recovery; partial output retained");
        if (!r->output || r->bytes == PART_BYTES) {
            if ((r->output && !finish_file(r)) || !begin_file(r))
                return fail(err,
                            "Cannot create or commit a recovery file; partial output retained");
        }
        start = v->data + (cluster - 2) * v->spc;
        if (fprintf(r->manifest,
                    "CLUSTER %lu file=%s offset=%lu disk_lba=%lu sectors=%u\n",
                    (unsigned long)cluster,
                    r->file,
                    (unsigned long)r->bytes,
                    (unsigned long)(v->start + start),
                    v->spc) < 0)
            return fail(err, "Cannot write recovery manifest");
        for (sector = 0; sector < v->spc; sector++) {
            if (r->progress &&
                !r->progress(r->ctx, "Recover lost chains", r->result->clusters, r->result->lost))
                return fail(err, "Recovery cancelled; incomplete files are listed in the manifest");
            readable = read_payload(r, start + sector, data, err);
            if (readable < 0)
                return 0;
            if (!readable) {
                memset(data, 0, sizeof(data));
                r->result->unreadable++;
                if (fprintf(r->manifest,
                            "UNREADABLE file=%s offset=%lu disk_lba=%lu bytes=512\n",
                            r->file,
                            (unsigned long)r->bytes,
                            (unsigned long)(v->start + start + sector)) < 0)
                    return fail(err, "Cannot record unreadable sector in recovery manifest");
            }
            if (fwrite(data, 1, sizeof(data), r->output) != sizeof(data))
                return fail(err, "Recovery output write failed; partial output retained");
            r->bytes += 512;
        }
        set(r->visited, cluster);
        r->result->clusters++;
        if (next >= bad + 1) {
            cluster = next;
            break;
        }
        cluster = next;
        end = "leaves-lost-set";
        if (cluster >= 2 && cluster < v->clusters + 2 && has(r->lost, cluster))
            end = has(r->visited, cluster) ? "cycle-or-shared-tail" : "end-of-chain";
    }
    if (fprintf(r->manifest, "CHAIN_END reason=%s next=%lu\n", end, (unsigned long)cluster) < 0 ||
        !finish_file(r))
        return fail(err, "Recovery output could not be committed; partial output retained");
    return 1;
}

int dt_recover_lost(DtVolume *v, const char *destination, DtRecovery *result, DtProgress progress,
                    void *ctx, char *err)
{
    Recovery r;
    DtResult scan;
    unsigned char *owned = NULL;
    dt_u32 cluster, next, bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7;
    size_t bytes = (size_t)((v->clusters + 9) / 8);
    char path[280];
    unsigned pass;
    int unsafe = 0, ok = 0;

    memset(result, 0, sizeof(*result));
    memset(&r, 0, sizeof(r));
    err[0] = 0;
    if (!join(path, sizeof(path), destination, "RECOVERY.TXT"))
        return fail(err, "Recovery destination is empty or too long");
    if (v->disk->separate_destination && !v->disk->separate_destination(v->disk->ctx, path))
        return fail(
            err,
            "Recovery needs a separate DOS volume, or a different physical disk for BIOS sources");
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_owned(v, &scan, &owned, recovery_issue, &unsafe, progress, ctx) || unsafe) {
        free(owned);
        return fail(err, "Recovery requires a complete directory scan and consistent FAT copies");
    }
    result->lost = scan.lost_clusters;
    if (!result->lost) {
        free(owned);
        result->complete = 1;
        return 1;
    }
    r.volume = v;
    r.result = result;
    r.progress = progress;
    r.ctx = ctx;
    r.lost = owned;
    r.incoming = (unsigned char *)calloc(bytes, 1);
    r.visited = (unsigned char *)calloc(bytes, 1);
    if (!r.incoming || !r.visited) {
        fail(err, "Insufficient memory for lost-chain recovery");
        goto done;
    }
    /* Convert ownership to eligibility, retaining only allocated unowned clusters. */
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (progress && !(cluster & 31) &&
            !progress(ctx, "Find lost clusters", cluster - 2, v->clusters)) {
            fail(err, "Recovery cancelled; no output created");
            goto done;
        }
        if (!dt_fat(v, cluster, &next)) {
            fail(err, "Cannot read allocation while finding lost clusters");
            goto done;
        }
        if (!has(owned, cluster) && next && next != bad)
            set(r.lost, cluster);
        else
            r.lost[cluster / 8] &= (unsigned char)~(1U << (cluster % 8));
    }
    for (cluster = 2; cluster < v->clusters + 2; cluster++) {
        if (progress && !(cluster & 31) &&
            !progress(ctx, "Find chain roots", cluster - 2, v->clusters)) {
            fail(err, "Recovery cancelled; no output created");
            goto done;
        }
        if (!has(r.lost, cluster))
            continue;
        if (!dt_fat(v, cluster, &next)) {
            fail(err, "Cannot read lost-chain links");
            goto done;
        }
        if (next >= 2 && next < v->clusters + 2 && has(r.lost, next))
            set(r.incoming, next);
    }
    if (!make_directory(result->directory, destination) ||
        !join(path, sizeof(path), result->directory, "RECOVERY.TXT") ||
        !(r.manifest = dt_file_create(path))) {
        fail(err, "Cannot create a new recovery directory and manifest");
        goto done;
    }
    if (fprintf(r.manifest,
                "MagicDisk lost-chain recovery\nSTATE STARTED (incomplete until FINAL)\n"
                "FAT%u volume_lba=%lu lost_clusters=%lu\n"
                "Names and original lengths are unknown; files include full clusters.\n"
                "Unreadable sectors are zero-filled and individually listed.\n",
                v->bits,
                (unsigned long)v->start,
                (unsigned long)result->lost) < 0 ||
        fprintf(r.manifest, "OEM code page: %u (0 means unknown)\n", dt_oem_codepage()) < 0 ||
        !dt_file_commit(r.manifest)) {
        fail(err, "Cannot commit recovery manifest");
        goto done;
    }
    /* Roots first; the second pass visits cycles with no root and any remaining components. */
    for (pass = 0; pass < 2; pass++) {
        for (cluster = 2; cluster < v->clusters + 2; cluster++) {
            if (progress && !(cluster & 31) &&
                !progress(ctx, "Find remaining chains", cluster - 2, v->clusters)) {
                fail(err, "Recovery cancelled; partial output retained");
                goto done;
            }
            if (has(r.lost, cluster) && !has(r.visited, cluster) &&
                (pass || !has(r.incoming, cluster)))
                if (!export_chain(&r, cluster, err))
                    goto done;
        }
    }
    ok = 1;
done:
    if (r.manifest) {
        if (!finish_file(&r) || !dt_file_commit_directory(path))
            ok = 0;
        if (fprintf(r.manifest,
                    "FINAL %s files=%lu clusters=%lu unreadable=%lu\n",
                    ok ? (result->unreadable ? "COMPLETE_WITH_GAPS" : "COMPLETE") : "INCOMPLETE",
                    (unsigned long)result->files,
                    (unsigned long)result->clusters,
                    (unsigned long)result->unreadable) < 0 ||
            !dt_file_commit(r.manifest))
            ok = 0;
        if (fclose(r.manifest))
            ok = 0;
    }
    if (!ok && !*err)
        fail(err, "Recovery output could not be committed; inspect the manifest");
    result->complete = ok;
    free(r.lost);
    free(r.incoming);
    free(r.visited);
    return ok;
}

/* Live files use the scanner's bounded allocated prefix. Shared tails belong to
   the first catalog owner; later aliases are explicitly recorded as damaged. */
typedef struct {
    Recovery recovery;
    char *err;
    int failed, unsafe, selecting;
    dt_u32 target, matches, selected_clusters;
} LiveRecovery;

static int manifest_hex(FILE *file, const char *text)
{
    const unsigned char *byte = (const unsigned char *)text;

    while (*byte) {
        if (fprintf(file, "%02X", (unsigned)*byte++) < 0)
            return 0;
    }
    return 1;
}

static void live_issue(void *ctx, const char *code, dt_u32 where, const char *message)
{
    LiveRecovery *live = (LiveRecovery *)ctx;
    Recovery *r = &live->recovery;

    r->result->issues++;
    if (!strcmp(code, "DIRECTORY_GAP"))
        r->result->catalog_incomplete = 1;
    if (!strcmp(code, "FAT_MIRROR"))
        live->unsafe = 1;
    if (r->manifest &&
        (fprintf(r->manifest, "ISSUE code=%s where=%lu message_hex=", code, (unsigned long)where) <
             0 ||
         !manifest_hex(r->manifest, message) || fputc('\n', r->manifest) == EOF))
        live->failed = 1;
}

/* Search only the bounded prefix whose ownership the scanner established. */
static int selected_file(LiveRecovery *live, const DtFile *file)
{
    dt_u32 cluster = file->start, index, next;

    if (!live->target)
        return 1;
    if (file->directory)
        return 0;
    for (index = 0; index < file->clusters; index++) {
        if (!(index & 31) && live->recovery.progress &&
            !live->recovery.progress(
                live->recovery.ctx, "Locate selected file", index, file->clusters)) {
            live->failed = 1;
            fail(live->err, "File selection cancelled; no source sectors changed");
            return -1;
        }
        if (cluster == live->target)
            return 1;
        if (!dt_fat(live->recovery.volume, cluster, &next)) {
            live->failed = 1;
            fail(live->err, "Cannot read allocation while locating the selected file");
            return -1;
        }
        cluster = next;
    }
    return 0;
}

static int live_file(void *ctx, const DtFile *file)
{
    LiveRecovery *live = (LiveRecovery *)ctx;
    Recovery *r = &live->recovery;
    DtVolume *v = r->volume;
    int readable;
    dt_u32 cluster = file->start, index, sector, next, start;
    dt_u32 cluster_bytes = (dt_u32)v->spc * 512;
    dt_u32 needed = file->size / cluster_bytes + (file->size % cluster_bytes != 0);
    dt_u32 remaining = file->size;
    unsigned char data[512];
    int exact = file->ending == DT_CHAIN_END && needed == file->clusters;
    unsigned count;

    readable = selected_file(live, file);
    if (readable < 0)
        return 0;
    if (!readable)
        return 1;
    if (live->selecting) {
        live->matches++;
        live->selected_clusters += file->clusters;
        return 1;
    }
    if (file->directory) {
        if (file->ending != DT_CHAIN_END)
            r->result->damaged++;
        return 1;
    }
    if (!exact)
        r->result->damaged++;
    if (fprintf(
            r->manifest,
            "LIVE_BEGIN entry_lba=%lu offset=%u size=%lu clusters=%lu ending=%d mode=%s path_hex=",
            (unsigned long)(v->start + file->sector),
            file->offset,
            (unsigned long)file->size,
            (unsigned long)file->clusters,
            file->ending,
            exact ? "logical-length" : "allocated-prefix") < 0)
        goto output_error;
    if (!manifest_hex(r->manifest, file->path))
        goto output_error;
    if (fputc('\n', r->manifest) == EOF || !begin_file(r))
        goto output_error;
    for (index = 0; index < file->clusters; index++) {
        if (cluster < 2 || cluster >= v->clusters + 2 || !dt_fat(v, cluster, &next)) {
            fail(live->err, "Allocation changed or became unreadable during file export");
            goto failed;
        }
        start = v->data + (cluster - 2) * v->spc;
        for (sector = 0; sector < v->spc && (!exact || remaining); sector++) {
            if (r->progress &&
                !r->progress(r->ctx, "Export live files", r->result->clusters, r->result->lost)) {
                fail(live->err, "File export cancelled; partial files retained");
                goto failed;
            }
            if (r->bytes == PART_BYTES && (!finish_file(r) || !begin_file(r)))
                goto output_error;
            count = exact && remaining < 512 ? (unsigned)remaining : 512;
            if (fprintf(r->manifest,
                        "DATA file=%s offset=%lu disk_lba=%lu bytes=%u\n",
                        r->file,
                        (unsigned long)r->bytes,
                        (unsigned long)(v->start + start + sector),
                        count) < 0)
                goto output_error;
            readable = read_payload(r, start + sector, data, live->err);
            if (readable < 0)
                goto failed;
            if (!readable) {
                memset(data, 0, sizeof(data));
                r->result->unreadable++;
                if (fprintf(r->manifest,
                            "UNREADABLE file=%s offset=%lu disk_lba=%lu bytes=%u\n",
                            r->file,
                            (unsigned long)r->bytes,
                            (unsigned long)(v->start + start + sector),
                            count) < 0)
                    goto output_error;
            }
            if (fwrite(data, 1, count, r->output) != count)
                goto output_error;
            r->bytes += count;
            if (exact)
                remaining -= count;
        }
        r->result->clusters++;
        cluster = next;
    }
    if (!finish_file(r) || fprintf(r->manifest, "LIVE_END\n") < 0)
        goto output_error;
    return 1;
output_error:
    fail(live->err, "Cannot create, write or commit file export; partial output retained");
failed:
    live->failed = 1;
    return 0;
}

static int export_files(DtVolume *v, dt_u32 target, const char *destination, DtRecovery *result,
                        DtProgress progress, void *ctx, char *err)
{
    LiveRecovery live;
    Recovery *r = &live.recovery;
    DtResult scan;
    char path[280];
    int ok = 0, partial_catalog = 0;

    memset(&live, 0, sizeof(live));
    memset(result, 0, sizeof(*result));
    err[0] = 0;
    r->result = result;
    r->volume = v;
    live.err = err;
    live.target = target;
    r->progress = progress;
    r->ctx = ctx;
    if (target && (target < 2 || target >= v->clusters + 2))
        return fail(err, "Selected cluster is outside the data area");
    if (!join(path, sizeof(path), destination, "RECOVERY.TXT"))
        return fail(err, "Export destination is empty or too long");
    if (v->disk->separate_destination && !v->disk->separate_destination(v->disk->ctx, path))
        return fail(err,
                    "File export needs a separate DOS volume, or a different physical disk for "
                    "BIOS sources");
    v->cache_sector = 0xffffffffUL;
    live.selecting = 1;
    if (!dt_scan_recoverable_files(v, &scan, live_file, &live, live_issue, &live, progress, ctx) ||
        live.unsafe || live.failed)
        return fail(err,
                    "File export requires readable allocation metadata and consistent FAT copies");
    partial_catalog = !scan.complete;
    if (target && !live.matches)
        return fail(
            err,
            partial_catalog
                ? "No visible file owns this cluster; unreadable directory entries remain unknown"
                : "No regular file owns this cluster in the validated allocation prefixes");
    live.selecting = 0;
    r->volume = v;
    r->result = result;
    r->progress = progress;
    r->ctx = ctx;
    live.err = err;
    result->lost = target
                       ? live.selected_clusters
                       : v->clusters - scan.free_clusters - scan.bad_clusters - scan.lost_clusters;
    if (!make_directory(result->directory, destination) ||
        !join(path, sizeof(path), result->directory, "RECOVERY.TXT") ||
        !(r->manifest = dt_file_create(path)))
        return fail(err, "Cannot create a new export directory and manifest");
    if (fprintf(r->manifest,
                "MagicDisk live-file export\nSTATE STARTED (incomplete until FINAL)\n"
                "FAT%u volume_lba=%lu scan_issues=%lu\n"
                "Directory catalog: %s. Skipped entries may hide additional files or owners.\n"
                "path_hex encodes the original UTF-8 path. Numbered CHK files are export parts.\n"
                "Healthy files use their logical length; damaged files retain full allocated "
                "prefixes.\n"
                "Shared tails are exported through their first owner. Unreadable bytes are "
                "zero-filled.\n",
                v->bits,
                (unsigned long)v->start,
                (unsigned long)scan.errors,
                partial_catalog ? "INCOMPLETE" : "complete") < 0 ||
        fprintf(r->manifest, "OEM code page: %u (0 means unknown)\n", dt_oem_codepage()) < 0 ||
        !dt_file_commit(r->manifest))
        goto done;
    if (target && fprintf(r->manifest,
                          "SELECTION cluster=%lu scope=validated-regular-file-prefix\n",
                          (unsigned long)target) < 0)
        goto done;
    v->cache_sector = 0xffffffffUL;
    result->issues = 0;
    ok = dt_scan_recoverable_files(v, &scan, live_file, &live, live_issue, &live, progress, ctx) &&
         !live.failed && !live.unsafe && scan.complete && !partial_catalog;
done:
    if (!finish_file(r) || !dt_file_commit_directory(path))
        ok = 0;
    if (fprintf(r->manifest,
                "FINAL %s files=%lu clusters=%lu damaged=%lu unreadable=%lu issues=%lu\n",
                ok ? (result->damaged || result->unreadable ? "COMPLETE_WITH_GAPS"
                      : result->issues                      ? "COMPLETE_WITH_FINDINGS"
                                                            : "COMPLETE")
                   : "INCOMPLETE",
                (unsigned long)result->files,
                (unsigned long)result->clusters,
                (unsigned long)result->damaged,
                (unsigned long)result->unreadable,
                (unsigned long)result->issues) < 0 ||
        !dt_file_commit(r->manifest))
        ok = 0;
    if (fclose(r->manifest))
        ok = 0;
    if (!ok && !*err)
        fail(err, "File export incomplete; inspect the retained manifest");
    result->complete = ok;
    return ok;
}

int dt_export_files(DtVolume *v, const char *destination, DtRecovery *result, DtProgress progress,
                    void *ctx, char *err)
{
    return export_files(v, 0, destination, result, progress, ctx, err);
}

int dt_export_cluster_file(DtVolume *v, dt_u32 cluster, const char *destination, DtRecovery *result,
                           DtProgress progress, void *ctx, char *err)
{
    if (!cluster) {
        memset(result, 0, sizeof(*result));
        return fail(err, "Selected cluster is outside the data area");
    }
    return export_files(v, cluster, destination, result, progress, ctx, err);
}

/* Deleted entries are evidence, not proof that their old contents survived. */
typedef struct {
    dt_u32 start, parent, sector;
    unsigned offset;
    int discovered;
} DeletedDirectory;

typedef struct {
    Recovery recovery;
    unsigned char *owned, *queued, *directories;
    DeletedDirectory *queue;
    size_t queue_count, queue_capacity;
    dt_u32 parent_cluster;
    int orphan;
    size_t bitmap_bytes;
    unsigned lfn_count;
    unsigned char lfn[20][32];
    int unsafe, failed;
    char *err;
} DeletedRecovery;

static int manifest_bytes(FILE *file, const unsigned char *data, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++)
        if (fprintf(file, "%02X", data[i]) < 0)
            return 0;
    return 1;
}

/* Deletion erases ordinals. Infer physical reverse order only when the entire
   adjacent run has consistent metadata, padding, Unicode and alias checksum. */
static int deleted_name(DeletedRecovery *deleted, const unsigned char *entry)
{
    static const unsigned fields[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned short units[260];
    char name[1024];
    unsigned count = deleted->lfn_count, i, j, length, used = 0, first, checksum;
    unsigned matches = 0, restored = 0, expected;
    FILE *manifest = deleted->recovery.manifest;

    if (!count || count > 20)
        return 1;
    expected = deleted->lfn[0][13];
    for (i = 0; i < count; i++) {
        const unsigned char *slot = deleted->lfn[count - i - 1];

        if ((slot[0] != 0xe5 && slot[0] != (i + 1 + (i + 1 == count ? 0x40 : 0))) ||
            slot[11] != 15 || slot[12] || dt_word(slot + 26) || slot[13] != expected)
            return 1;
        for (j = 0; j < 13; j++)
            units[i * 13 + j] = (unsigned short)dt_word(slot + fields[j]);
    }
    for (length = 0; length < count * 13 && units[length]; length++) {
        unsigned ch = units[length];

        if (ch == 0xffff || ch < 32 || (ch < 128 && strchr("\"*/:<>?\\|", ch)))
            return 1;
        if (ch >= 0xd800 && ch <= 0xdbff &&
            (length + 1 == count * 13 || units[length + 1] < 0xdc00 || units[length + 1] > 0xdfff))
            return 1;
        if (ch >= 0xdc00 && ch <= 0xdfff &&
            (!length || units[length - 1] < 0xd800 || units[length - 1] > 0xdbff))
            return 1;
    }
    if (!length || length > 255 || (length + 12) / 13 != count ||
        (length == 1 && units[0] == '.') || (length == 2 && units[0] == '.' && units[1] == '.'))
        return 1;
    for (i = length + 1; i < count * 13; i++)
        if (units[i] != 0xffff)
            return 1;
    for (i = 1; i < 11; i++)
        if (entry[i] < 32 || strchr("\"*+,./:;<=>?[\\]|", entry[i]))
            return 1;
    /* The original OEM lead byte is reported as hex, never guessed from UTF-8. */
    for (first = 1; first < 256; first++) {
        if ((entry[0] != 0xe5 && first != entry[0]) || first == 0xe5 || first == ' ' ||
            (first != 5 && (first < 32 || strchr("\"*+,./:;<=>?[\\]|", first))))
            continue;
        checksum = first;
        for (i = 1; i < 11; i++)
            checksum = (((checksum >> 1) | ((checksum & 1) << 7)) + entry[i]) & 255;
        if (checksum == expected) {
            matches++;
            restored = first;
        }
    }
    if (matches != 1)
        return 1;
    for (i = 0; i < length; i++) {
        dt_u32 ch = units[i];

        if (ch >= 0xd800 && ch <= 0xdbff) {
            ch = 0x10000UL + ((ch - 0xd800) << 10) + (units[++i] - 0xdc00);
            name[used++] = (char)(0xf0 | (ch >> 18));
            name[used++] = (char)(0x80 | ((ch >> 12) & 63));
            name[used++] = (char)(0x80 | ((ch >> 6) & 63));
            name[used++] = (char)(0x80 | (ch & 63));
        } else if (ch < 128) {
            name[used++] = (char)ch;
        } else if (ch < 2048) {
            name[used++] = (char)(0xc0 | (ch >> 6));
            name[used++] = (char)(0x80 | (ch & 63));
        } else {
            name[used++] = (char)(0xe0 | (ch >> 12));
            name[used++] = (char)(0x80 | ((ch >> 6) & 63));
            name[used++] = (char)(0x80 | (ch & 63));
        }
    }
    name[used] = 0;
    return fprintf(manifest,
                   "NAME_CANDIDATE source=deleted-lfn-order-inferred confidence=UNVERIFIED "
                   "short_first_hex=%02X name_hex=",
                   restored) >= 0 &&
           manifest_hex(manifest, name) && fputc('\n', manifest) != EOF;
}

static int deleted_file(DeletedRecovery *deleted, const unsigned char *entry, dt_u32 location,
                        unsigned offset, const char *parent)
{
    Recovery *r = &deleted->recovery;
    DtVolume *v = r->volume;
    dt_u32 cluster = dt_word(entry + 26), next, sector, start;
    dt_u32 remaining = dt_dword(entry + 28), size = remaining;
    dt_u32 bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7;
    unsigned char data[512];
    unsigned count;
    int contiguous = -1, readable;
    const char *stop = "declared-length";

    if (v->bits == 32)
        cluster |= (dt_u32)dt_word(entry + 20) << 16;
    if (fprintf(r->manifest,
                "DELETED_BEGIN entry_lba=%lu offset=%u start=%lu size=%lu "
                "confidence=UNVERIFIED parent_hex=",
                (unsigned long)(v->start + location),
                offset,
                (unsigned long)cluster,
                (unsigned long)size) < 0 ||
        !manifest_hex(r->manifest, parent) || fprintf(r->manifest, " entry_hex=") < 0 ||
        !manifest_bytes(r->manifest, entry, 32) || fputc('\n', r->manifest) == EOF ||
        !deleted_name(deleted, entry) || !begin_file(r))
        goto output_error;
    memset(r->visited, 0, deleted->bitmap_bytes);
    while (remaining) {
        if (cluster < 2 || cluster >= v->clusters + 2 || cluster >= bad - 7) {
            stop = "invalid-cluster";
            break;
        }
        if (has(deleted->owned, cluster)) {
            stop = "live-owned-cluster";
            break;
        }
        if (has(r->visited, cluster)) {
            stop = "chain-loop";
            break;
        }
        if (!dt_fat(v, cluster, &next)) {
            fail(deleted->err, "Allocation read failed during deleted-file export");
            return 0;
        }
        if (contiguous < 0) {
            contiguous = !next;
            if (fprintf(r->manifest,
                        "METHOD %s\n",
                        contiguous ? "CONTIGUOUS_GUESS" : "RETAINED_UNOWNED_CHAIN") < 0)
                goto output_error;
        }
        if (next == bad || (next >= bad - 7 && next < bad) || next == 1 ||
            (contiguous ? next != 0 : next == 0)) {
            stop = "allocation-conflict";
            break;
        }
        set(r->visited, cluster);
        start = v->data + (cluster - 2) * v->spc;
        for (sector = 0; sector < v->spc && remaining; sector++) {
            if (r->progress &&
                !r->progress(r->ctx, "Export deleted candidates", size - remaining, size))
                return fail(deleted->err, "Deleted-file export cancelled; partial output retained");
            if (r->bytes == PART_BYTES && (!finish_file(r) || !begin_file(r)))
                goto output_error;
            count = remaining < 512 ? (unsigned)remaining : 512;
            if (fprintf(r->manifest,
                        "DATA file=%s offset=%lu disk_lba=%lu bytes=%u\n",
                        r->file,
                        (unsigned long)r->bytes,
                        (unsigned long)(v->start + start + sector),
                        count) < 0)
                goto output_error;
            readable = read_payload(r, start + sector, data, deleted->err);
            if (readable < 0)
                return 0;
            if (!readable) {
                memset(data, 0, sizeof(data));
                r->result->unreadable++;
                if (fprintf(r->manifest,
                            "UNREADABLE file=%s offset=%lu disk_lba=%lu bytes=%u\n",
                            r->file,
                            (unsigned long)r->bytes,
                            (unsigned long)(v->start + start + sector),
                            count) < 0)
                    goto output_error;
            }
            if (fwrite(data, 1, count, r->output) != count)
                goto output_error;
            r->bytes += count;
            remaining -= count;
        }
        r->result->clusters++;
        if (!contiguous && next >= bad + 1 && remaining) {
            stop = "early-chain-end";
            break;
        }
        cluster = contiguous ? cluster + 1 : next;
    }
    if (remaining)
        r->result->damaged++;
    /* Even a full-length export may contain reused free space or a stale chain. */
    r->result->candidates++;
    if (!finish_file(r) || fprintf(r->manifest,
                                   "DELETED_END recovered=%lu missing=%lu stop=%s\n",
                                   (unsigned long)(size - remaining),
                                   (unsigned long)remaining,
                                   stop) < 0)
        goto output_error;
    return 1;
output_error:
    return fail(deleted->err, "Cannot commit deleted-file export; partial output retained");
}

static dt_u32 deleted_start(const DtVolume *v, const unsigned char *entry)
{
    dt_u32 cluster = dt_word(entry + 26);

    if (v->bits == 32)
        cluster |= (dt_u32)dt_word(entry + 20) << 16;
    return cluster;
}

static int queue_deleted_directory(DeletedRecovery *deleted, const unsigned char *entry,
                                   dt_u32 sector, unsigned offset, const char *parent)
{
    Recovery *r = &deleted->recovery;
    DtVolume *v = r->volume;
    dt_u32 start = deleted_start(v, entry);
    DeletedDirectory *queue;
    const char *stop = NULL;
    size_t capacity;

    if (start < 2 || start >= v->clusters + 2)
        stop = "invalid-start";
    else if (has(deleted->owned, start))
        stop = "live-owned-cluster";
    else if (has(deleted->queued, start))
        stop = "already-queued";
    if (fprintf(r->manifest,
                "DIRECTORY_CANDIDATE entry_lba=%lu offset=%u start=%lu parent_cluster=%lu "
                "state=%s parent_hex=",
                (unsigned long)(v->start + sector),
                offset,
                (unsigned long)start,
                (unsigned long)deleted->parent_cluster,
                stop ? stop : "queued") < 0 ||
        !manifest_hex(r->manifest, parent) || fputc('\n', r->manifest) == EOF)
        return fail(deleted->err, "Cannot record deleted-directory candidate");
    if (stop) {
        if (strcmp(stop, "already-queued"))
            r->result->damaged++;
        return 1;
    }
    if (deleted->queue_count == deleted->queue_capacity) {
        capacity = deleted->queue_capacity ? deleted->queue_capacity * 2 : 32;
        if (capacity <= deleted->queue_capacity || capacity > (size_t)-1 / sizeof(*queue))
            return fail(deleted->err, "Deleted-directory queue exceeds addressable memory");
        queue = (DeletedDirectory *)realloc(deleted->queue, capacity * sizeof(*queue));
        if (!queue)
            return fail(deleted->err, "Insufficient memory for deleted-directory queue");
        deleted->queue = queue;
        deleted->queue_capacity = capacity;
    }
    queue = &deleted->queue[deleted->queue_count++];
    queue->start = start;
    queue->parent = deleted->parent_cluster;
    queue->sector = sector;
    queue->offset = offset;
    queue->discovered = 0;
    return 1;
}

static int deleted_dot(const DtVolume *v, const unsigned char *entry, int parent, dt_u32 expected)
{
    return (entry[0] == '.' || entry[0] == 0xe5) &&
           !memcmp(entry + 1, parent ? ".         " : "          ", 10) && entry[11] == 16 &&
           !dt_dword(entry + 28) && deleted_start(v, entry) == expected;
}

/* Return 1 for data, 0 for persistent failure, -1 for cancellation/output error. */
static int read_deleted_directory(DeletedRecovery *deleted, dt_u32 sector, unsigned char *data)
{
    Recovery *r = &deleted->recovery;
    unsigned attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        if (attempt && r->progress &&
            !r->progress(r->ctx, "Retry deleted directory sector", attempt, 3)) {
            fail(deleted->err, "Deleted-directory retry cancelled; partial output retained");
            return -1;
        }
        if (dt_read(r->volume, sector, data))
            break;
    }
    if (attempt && fprintf(r->manifest,
                           "DIRECTORY_RETRY entry_lba=%lu attempts=%u state=%s\n",
                           (unsigned long)(r->volume->start + sector),
                           attempt < 3 ? attempt + 1 : 3,
                           attempt < 3 ? "readable" : "unreadable") < 0) {
        fail(deleted->err, "Cannot record deleted-directory retry evidence");
        return -1;
    }
    return attempt < 3;
}

/* Intact dot pairs can identify an unreferenced directory head even when its
   parent entry is gone. This is evidence for export, never proof for repair. */
static int find_orphan_directories(DeletedRecovery *deleted)
{
    Recovery *r = &deleted->recovery;
    DtVolume *v = r->volume;
    dt_u32 cluster, next, sector, parent;
    dt_u32 bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7;
    unsigned char data[512];
    int readable;
    size_t queued;

    for (cluster = 2; cluster < v->clusters + 2 && cluster < bad - 7; cluster++) {
        if (!((cluster - 2) & 31) && r->progress &&
            !r->progress(r->ctx, "Find orphan directories", cluster - 2, v->clusters))
            return fail(deleted->err, "Directory discovery cancelled; partial output retained");
        if (has(deleted->owned, cluster))
            continue;
        if (!dt_fat(v, cluster, &next))
            return fail(deleted->err, "Cannot read allocation during directory discovery");
        if (!next || next == 1 || (next >= bad - 7 && next <= bad))
            continue;
        sector = v->data + (cluster - 2) * v->spc;
        readable = read_deleted_directory(deleted, sector, data);
        if (readable < 0)
            return 0;
        if (!readable) {
            deleted->failed = 1;
            r->result->unreadable++;
            if (fprintf(r->manifest,
                        "ORPHAN_SEARCH_GAP entry_lba=%lu cluster=%lu\n",
                        (unsigned long)(v->start + sector),
                        (unsigned long)cluster) < 0)
                return fail(deleted->err, "Cannot record directory discovery gap");
            continue;
        }
        parent = deleted_start(v, data + 32);
        if (data[0] != '.' || data[32] != '.' || !deleted_dot(v, data, 0, cluster) ||
            !deleted_dot(v, data + 32, 1, parent) || parent == cluster || parent == 1 ||
            parent >= v->clusters + 2 || parent >= bad - 7)
            continue;
        if (fprintf(r->manifest,
                    "ORPHAN_DIRECTORY start=%lu parent_cluster=%lu entry_lba=%lu "
                    "confidence=UNVERIFIED raw_hex=",
                    (unsigned long)cluster,
                    (unsigned long)parent,
                    (unsigned long)(v->start + sector)) < 0 ||
            !manifest_bytes(r->manifest, data, 64) || fputc('\n', r->manifest) == EOF)
            return fail(deleted->err, "Cannot record orphan-directory evidence");
        deleted->parent_cluster = parent;
        queued = deleted->queue_count;
        if (!queue_deleted_directory(deleted, data, sector, 0, "orphan-search"))
            return 0;
        if (deleted->queue_count > queued) {
            deleted->queue[queued].discovered = 1;
            set(deleted->queued, cluster);
        }
    }
    return 1;
}

static int deleted_sector(DeletedRecovery *deleted, dt_u32 sector, const char *parent, int *end,
                          unsigned first_offset)
{
    Recovery *r = &deleted->recovery;
    unsigned char data[512];
    unsigned offset;
    int readable;

    if (r->progress && !r->progress(r->ctx, "Find deleted entries", sector, r->volume->total))
        return fail(deleted->err, "Deleted-entry search cancelled; partial output retained");
    readable = read_deleted_directory(deleted, sector, data);
    if (readable < 0)
        return 0;
    if (!readable)
        return fail(deleted->err,
                    "Directory read failed after three attempts during deleted-entry search");
    for (offset = first_offset; offset < 512; offset += 32) {
        const unsigned char *entry = data + offset;

        if (!entry[0]) {
            *end = 1;
            break;
        }
        if (entry[0] != 0xe5 && !deleted->orphan) {
            deleted->lfn_count = 0;
            continue;
        }
        if (deleted->orphan && (entry[11] & 0xc0)) {
            *end = 2;
            break;
        }
        if (fprintf(r->manifest,
                    "DELETED_SLOT entry_lba=%lu offset=%u raw_hex=",
                    (unsigned long)(r->volume->start + sector),
                    offset) < 0 ||
            !manifest_bytes(r->manifest, entry, 32) || fputc('\n', r->manifest) == EOF)
            return fail(deleted->err, "Cannot record deleted directory evidence");
        /* Runs persist across sector/cluster boundaries, but never directories. */
        if (entry[11] == 15) {
            if (deleted->lfn_count < 20)
                memcpy(deleted->lfn[deleted->lfn_count], entry, 32);
            if (deleted->lfn_count <= 20)
                deleted->lfn_count++;
            continue;
        }
        if (entry[11] & 16) {
            if (!((entry[0] == '.' || entry[0] == 0xe5) &&
                  (!memcmp(entry + 1, "          ", 10) || !memcmp(entry + 1, ".         ", 10))) &&
                !(entry[11] & 0xc8) &&
                !queue_deleted_directory(deleted, entry, sector, offset, parent))
                return 0;
        } else if (!(entry[11] & 0xc8) && entry[1] &&
                   !deleted_file(deleted, entry, sector, offset, parent))
            return 0;
        deleted->lfn_count = 0;
    }
    return 1;
}

static int deleted_directory(void *ctx, const DtFile *file)
{
    DeletedRecovery *deleted = (DeletedRecovery *)ctx;
    DtVolume *v = deleted->recovery.volume;
    dt_u32 cluster = file->start, n, sector;
    unsigned i;
    int end = 0;

    if (!file->directory)
        return 1;
    deleted->lfn_count = 0;
    deleted->parent_cluster = file->directory == 2 ? 0 : file->start;
    for (n = 0; n < file->clusters && !end; n++) {
        sector = v->data + (cluster - 2) * v->spc;
        for (i = 0; i < v->spc && !end; i++)
            if (!deleted_sector(deleted, sector + i, file->path, &end, 0))
                goto failed;
        if (!end && n + 1 < file->clusters && !dt_fat(v, cluster, &cluster)) {
            fail(deleted->err, "Cannot follow directory chain during deleted-file export");
            goto failed;
        }
    }
    return 1;
failed:
    deleted->failed = 1;
    return 0;
}

static int walk_deleted_directories(DeletedRecovery *deleted)
{
    Recovery *r = &deleted->recovery;
    DtVolume *v = r->volume;
    dt_u32 bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7;
    size_t index;

    deleted->orphan = 1;
    for (index = 0; index < deleted->queue_count; index++) {
        /* Parsing can grow the queue: retain a value, never a pointer into it. */
        DeletedDirectory item = deleted->queue[index];
        dt_u32 cluster = item.start, next, sector, visited = 0;
        unsigned i;
        unsigned char data[512];
        int contiguous = -1, end = 0, missing_dot = 0;
        const char *stop = "end-marker";
        char parent[80];

        if (item.discovered)
            sprintf(parent, "orphan-directory@%lu", (unsigned long)item.start);
        else
            sprintf(parent,
                    "deleted-directory@%lu:%u",
                    (unsigned long)(v->start + item.sector),
                    item.offset);
        deleted->parent_cluster = item.start;
        deleted->lfn_count = 0;
        while (!end) {
            if (r->progress && !r->progress(r->ctx,
                                            "Recover deleted directories",
                                            (dt_u32)index,
                                            (dt_u32)deleted->queue_count))
                return fail(deleted->err,
                            "Deleted-directory recovery cancelled; partial output retained");
            if (cluster < 2 || cluster >= v->clusters + 2 || cluster >= bad - 7) {
                stop = "invalid-cluster";
                break;
            }
            if (has(deleted->owned, cluster)) {
                stop = "live-owned-cluster";
                break;
            }
            if (has(deleted->directories, cluster)) {
                stop = "visited-directory-cluster";
                break;
            }
            if (!dt_fat(v, cluster, &next))
                return fail(deleted->err, "Cannot read deleted-directory allocation");
            if (contiguous < 0)
                contiguous = !next;
            if (next == bad || (next >= bad - 7 && next < bad) || next == 1 ||
                (contiguous ? next != 0 : next == 0)) {
                stop = "allocation-conflict";
                break;
            }
            sector = v->data + (cluster - 2) * v->spc;
            if (!visited) {
                int readable = read_deleted_directory(deleted, sector, data);

                if (readable < 0)
                    return 0;
                if (!readable) {
                    stop = "unreadable-dot-sector";
                    r->result->unreadable++;
                    break;
                }
                {
                    static const unsigned char empty[32] = {0};
                    int self_valid = deleted_dot(v, data, 0, item.start);
                    int parent_valid = deleted_dot(v, data + 32, 1, item.parent);

                    /* An erased slot supplies no contradictory pointer. Require
                       a retained chain and the other matching dot entry. */
                    missing_dot = !contiguous && ((self_valid && !memcmp(data + 32, empty, 32)) ||
                                                  (parent_valid && !memcmp(data, empty, 32)));
                    if ((!self_valid || !parent_valid) && !missing_dot) {
                        stop = "invalid-dot-entries";
                        break;
                    }
                    if (missing_dot) {
                        r->result->damaged++;
                        if (fprintf(r->manifest,
                                    "DIRECTORY_DOT_EVIDENCE start=%lu self_valid=%d "
                                    "parent_valid=%d confidence=UNVERIFIED raw_hex=",
                                    (unsigned long)item.start,
                                    self_valid,
                                    parent_valid) < 0 ||
                            !manifest_bytes(r->manifest, data, 64) ||
                            fputc('\n', r->manifest) == EOF)
                            return fail(deleted->err, "Cannot record missing-dot evidence");
                    }
                }
                set(deleted->queued, item.start);
                if (fprintf(r->manifest,
                            "DIRECTORY_BEGIN start=%lu source=%s method=%s\n",
                            (unsigned long)item.start,
                            parent,
                            contiguous ? "CONTIGUOUS_GUESS" : "RETAINED_UNOWNED_CHAIN") < 0)
                    return fail(deleted->err, "Cannot record deleted-directory provenance");
            }
            set(deleted->directories, cluster);
            visited++;
            for (i = 0; i < v->spc && !end; i++)
                if (!deleted_sector(deleted,
                                    sector + i,
                                    parent,
                                    &end,
                                    missing_dot && visited == 1 && i == 0 ? 64 : 0))
                    return 0;
            if (end == 2)
                stop = "malformed-entry";
            if (!end && !contiguous && next >= bad + 1) {
                stop = "end-of-chain";
                break;
            }
            cluster = contiguous ? cluster + 1 : next;
        }
        if (strcmp(stop, "end-marker") && strcmp(stop, "end-of-chain"))
            r->result->damaged++;
        if (fprintf(r->manifest,
                    "DIRECTORY_END start=%lu clusters=%lu stop=%s\n",
                    (unsigned long)item.start,
                    (unsigned long)visited,
                    stop) < 0)
            return fail(deleted->err, "Cannot record deleted-directory termination");
    }
    deleted->orphan = 0;
    return 1;
}

static int recover_directory_candidates(DtVolume *v, const char *destination, DtRecovery *result,
                                        DtProgress progress, void *ctx, char *err, int discover)
{
    DeletedRecovery deleted;
    Recovery *r = &deleted.recovery;
    DtResult scan;
    char path[280];
    dt_u32 sector;
    int ok = 0, end = 0;

    memset(&deleted, 0, sizeof(deleted));
    memset(result, 0, sizeof(*result));
    err[0] = 0;
    r->volume = v;
    r->result = result;
    r->progress = progress;
    r->ctx = ctx;
    deleted.err = err;
    if (!join(path, sizeof(path), destination, "RECOVERY.TXT"))
        return fail(err, "Deleted-file export destination is empty or too long");
    if (v->disk->separate_destination && !v->disk->separate_destination(v->disk->ctx, path))
        return fail(err,
                    "Deleted-file export needs a separate DOS volume, or a different physical disk "
                    "for BIOS sources");
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_owned(v, &scan, &deleted.owned, recovery_issue, &deleted.unsafe, progress, ctx) ||
        deleted.unsafe) {
        fail(err, "Deleted-file export requires readable, unambiguous live ownership");
        goto done;
    }
    deleted.bitmap_bytes = (size_t)((v->clusters + 9UL) / 8);
    r->visited = (unsigned char *)calloc(deleted.bitmap_bytes, 1);
    deleted.queued = (unsigned char *)calloc(deleted.bitmap_bytes, 1);
    deleted.directories = (unsigned char *)calloc(deleted.bitmap_bytes, 1);
    if (!r->visited || !deleted.queued || !deleted.directories) {
        fail(err, "Insufficient memory for deleted-file chain tracking");
        goto done;
    }
    if (!make_directory(result->directory, destination) ||
        !join(path, sizeof(path), result->directory, "RECOVERY.TXT") ||
        !(r->manifest = dt_file_create(path))) {
        fail(err, "Cannot create deleted-file export directory and manifest");
        goto done;
    }
    if (fprintf(r->manifest,
                "%s\nSTATE STARTED (incomplete until FINAL)\n"
                "FAT%u volume_lba=%lu\n"
                "ALL candidates are UNVERIFIED; deleted data may have been overwritten.\n"
                "Free chains use a CONTIGUOUS_GUESS, which cannot reconstruct fragmentation.\n"
                "Live-owned clusters are excluded. Unreadable sectors become recorded zeros.\n"
                "Deleted slots, including LFN slots, are preserved as raw_hex evidence.\n"
                "NAME_CANDIDATE uses inferred deleted LFN order and a validated alias checksum;\n"
                "it remains UNVERIFIED. Raw slots are retained even when decoding fails.\n",
                discover ? "MagicDisk orphan-directory candidates"
                         : "MagicDisk deleted-file candidates",
                v->bits,
                (unsigned long)v->start) < 0 ||
        fprintf(r->manifest, "OEM code page: %u (0 means unknown)\n", dt_oem_codepage()) < 0 ||
        !dt_file_commit(r->manifest))
        goto done;
    if (discover) {
        ok = find_orphan_directories(&deleted);
    } else {
        if (v->bits != 32)
            for (sector = 0; sector < v->rootsz && !end; sector++)
                if (!deleted_sector(&deleted, v->root + sector, "", &end, 0))
                    goto done;
        v->cache_sector = 0xffffffffUL;
        ok = dt_scan_files(v,
                           &scan,
                           deleted_directory,
                           &deleted,
                           recovery_issue,
                           &deleted.unsafe,
                           progress,
                           ctx) &&
             !deleted.failed && !deleted.unsafe;
    }
    if (ok)
        ok = walk_deleted_directories(&deleted);
    if (discover && deleted.failed) {
        ok = 0;
        if (!*err)
            fail(err, "Directory discovery incomplete; unreadable candidates may remain");
    }
done:
    if (r->manifest) {
        if (!finish_file(r) || !dt_file_commit_directory(path))
            ok = 0;
        if (fprintf(r->manifest,
                    "FINAL %s files=%lu clusters=%lu truncated=%lu unreadable=%lu candidates=%lu\n",
                    ok ? "COMPLETE_UNVERIFIED" : "INCOMPLETE",
                    (unsigned long)result->files,
                    (unsigned long)result->clusters,
                    (unsigned long)result->damaged,
                    (unsigned long)result->unreadable,
                    (unsigned long)result->candidates) < 0 ||
            !dt_file_commit(r->manifest))
            ok = 0;
        if (fclose(r->manifest))
            ok = 0;
    }
    if (!ok && !*err)
        fail(err, "Deleted-file export incomplete; inspect the retained manifest");
    result->complete = ok;
    free(deleted.owned);
    free(deleted.queued);
    free(deleted.directories);
    free(deleted.queue);
    free(r->visited);
    return ok;
}

int dt_recover_deleted(DtVolume *v, const char *destination, DtRecovery *result,
                       DtProgress progress, void *ctx, char *err)
{
    return recover_directory_candidates(v, destination, result, progress, ctx, err, 0);
}

int dt_recover_directories(DtVolume *v, const char *destination, DtRecovery *result,
                           DtProgress progress, void *ctx, char *err)
{
    return recover_directory_candidates(v, destination, result, progress, ctx, err, 1);
}
