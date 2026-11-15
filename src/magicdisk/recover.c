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

static int export_chain(Recovery *r, dt_u32 cluster, char *err)
{
    DtVolume *v = r->volume;
    dt_u32 next, sector, start, bad = v->bits == 12 ? 0xff7 : v->bits == 16 ? 0xfff7 : 0x0ffffff7;
    unsigned char data[512];
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
            if (!dt_read(v, start + sector, data)) {
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
            "Recovery needs a separate DOS volume; BIOS targets must be opened by drive letter");
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
    int failed, unsafe;
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
    if (!strcmp(code, "FAT_MIRROR"))
        live->unsafe = 1;
    if (r->manifest &&
        (fprintf(r->manifest, "ISSUE code=%s where=%lu message_hex=", code, (unsigned long)where) <
             0 ||
         !manifest_hex(r->manifest, message) || fputc('\n', r->manifest) == EOF))
        live->failed = 1;
}

static int live_file(void *ctx, const DtFile *file)
{
    LiveRecovery *live = (LiveRecovery *)ctx;
    Recovery *r = &live->recovery;
    DtVolume *v = r->volume;
    dt_u32 cluster = file->start, index, sector, next, start;
    dt_u32 cluster_bytes = (dt_u32)v->spc * 512;
    dt_u32 needed = file->size / cluster_bytes + (file->size % cluster_bytes != 0);
    dt_u32 remaining = file->size;
    unsigned char data[512];
    int exact = file->ending == DT_CHAIN_END && needed == file->clusters;
    unsigned count;

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
            if (!dt_read(v, start + sector, data)) {
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

int dt_export_files(DtVolume *v, const char *destination, DtRecovery *result, DtProgress progress,
                    void *ctx, char *err)
{
    LiveRecovery live;
    Recovery *r = &live.recovery;
    DtResult scan;
    char path[280];
    int ok = 0;

    memset(&live, 0, sizeof(live));
    memset(result, 0, sizeof(*result));
    err[0] = 0;
    r->result = result;
    if (!join(path, sizeof(path), destination, "RECOVERY.TXT"))
        return fail(err, "Export destination is empty or too long");
    if (v->disk->separate_destination && !v->disk->separate_destination(v->disk->ctx, path))
        return fail(err,
                    "File export needs a separate DOS volume; open BIOS targets by drive letter");
    v->cache_sector = 0xffffffffUL;
    if (!dt_scan_ex(v, &scan, live_issue, &live, progress, ctx) || live.unsafe)
        return fail(err, "File export requires a complete scan and consistent FAT copies");
    r->volume = v;
    r->result = result;
    r->progress = progress;
    r->ctx = ctx;
    live.err = err;
    result->lost = v->clusters - scan.free_clusters - scan.bad_clusters - scan.lost_clusters;
    if (!make_directory(result->directory, destination) ||
        !join(path, sizeof(path), result->directory, "RECOVERY.TXT") ||
        !(r->manifest = dt_file_create(path)))
        return fail(err, "Cannot create a new export directory and manifest");
    if (fprintf(r->manifest,
                "MagicDisk live-file export\nSTATE STARTED (incomplete until FINAL)\n"
                "FAT%u volume_lba=%lu scan_issues=%lu\n"
                "path_hex encodes the original UTF-8 path. Numbered CHK files are export parts.\n"
                "Healthy files use their logical length; damaged files retain full allocated "
                "prefixes.\n"
                "Shared tails are exported through their first owner. Unreadable bytes are "
                "zero-filled.\n",
                v->bits,
                (unsigned long)v->start,
                (unsigned long)scan.errors) < 0 ||
        !dt_file_commit(r->manifest))
        goto done;
    v->cache_sector = 0xffffffffUL;
    result->issues = 0;
    ok = dt_scan_files(v, &scan, live_file, &live, live_issue, &live, progress, ctx) &&
         !live.failed && !live.unsafe;
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
