#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "disk.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __WATCOMC__
#include <io.h>
#include <share.h>
#include <i86.h>
#else
#include <unistd.h>
#endif

unsigned dt_word(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

dt_u32 dt_dword(const unsigned char *p)
{
    return (dt_u32)dt_word(p) | ((dt_u32)dt_word(p + 2) << 16);
}

static int fail(char *err, const char *s)
{
    strcpy(err, s);
    return 0;
}

static int image_read(void *ctx, dt_u32 sector, unsigned char *buf)
{
#ifdef __WATCOMC__
    if ((dt_u64)sector * 512 > LONG_MAX || fseek((FILE *)ctx, (long)sector * 512, SEEK_SET))
        return 0;
#else
    if (fseeko((FILE *)ctx, (off_t)sector * 512, SEEK_SET))
        return 0;
#endif
    return fread(buf, 1, 512, (FILE *)ctx) == 512;
}

static int image_write(void *ctx, dt_u32 sector, const unsigned char *buf)
{
#ifdef __WATCOMC__
    if ((dt_u64)sector * 512 > LONG_MAX || fseek((FILE *)ctx, (long)sector * 512, SEEK_SET))
        return 0;
#else
    if (fseeko((FILE *)ctx, (off_t)sector * 512, SEEK_SET))
        return 0;
#endif
    return fwrite(buf, 1, 512, (FILE *)ctx) == 512;
}

int dt_file_commit(FILE *file)
{
    if (fflush(file))
        return 0;
#ifdef __WATCOMC__
    {
        union REGS r;
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x6800;
        r.w.bx = (unsigned short)fileno(file);
        int386(0x21, &r, &r);
        return !r.x.cflag;
    }
#else
    return fsync(fileno(file)) == 0;
#endif
}

int dt_file_commit_directory(const char *path)
{
#ifdef __WATCOMC__
    (void)path;
    /* DOS commit-file flushes the file's directory entry as well. */
    return 1;
#else
    char *parent, *slash;
    int fd, ok;
    size_t length = strlen(path);

    parent = (char *)malloc(length + 2);
    if (!parent)
        return 0;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (slash)
        slash[slash == parent ? 1 : 0] = 0;
    else
        strcpy(parent, ".");
    fd = open(parent, O_RDONLY | O_DIRECTORY);
    free(parent);
    if (fd < 0)
        return 0;
    ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}

static int image_sync(void *ctx)
{
    return dt_file_commit((FILE *)ctx);
}

/* Exclusive creation prevents a journal path from truncating an existing image. */
FILE *dt_file_create(const char *path)
{
    int fd;
    FILE *file;
#ifdef __WATCOMC__
    fd = _sopen(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, SH_DENYRW, S_IREAD | S_IWRITE);
#else
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (fd < 0)
        return NULL;
    file = fdopen(fd, "wb");
    if (!file)
        close(fd);
    return file;
}

static FILE *image_writable(const char *path)
{
    int fd;
    FILE *file;
    struct stat st;
#ifndef __WATCOMC__
    struct flock lock;
#endif
#ifdef __WATCOMC__
    fd = _sopen(path, O_RDWR | O_BINARY, SH_DENYRW);
#else
    fd = open(path, O_RDWR);
#endif
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
#ifndef __WATCOMC__
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        close(fd);
        return NULL;
    }
#endif
    file = fdopen(fd, "r+b");
    if (!file)
        close(fd);
    return file;
}

static int image_open(DtDisk *d, const char *path, int writable, char *err)
{
    FILE *f = writable ? image_writable(path) : fopen(path, "rb");
    dt_u64 size;
    memset(d, 0, sizeof(*d));
    if (!f)
        return fail(err, "Cannot open or lock image");
    setvbuf(f, NULL, _IONBF, 0);
#ifdef __WATCOMC__
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) {
        fclose(f);
        return fail(err, "Cannot size image");
    }
    size = (unsigned long)ftell(f);
#else
    if (fseeko(f, 0, SEEK_END) || ftello(f) < 0) {
        fclose(f);
        return fail(err, "Cannot size image");
    }
    size = (dt_u64)ftello(f);
#endif
    if (!size || size % 512 || size / 512 > 0xffffffffUL) {
        fclose(f);
        return fail(err, "Image size must be whole 512-byte sectors, at most 2 TiB");
    }
    d->ctx = f;
    d->read = image_read;
    d->write = writable ? image_write : NULL;
    d->sync = writable ? image_sync : NULL;
    d->sectors = (dt_u32)(size / 512);
    return 1;
}

int dt_image_open(DtDisk *d, const char *path, char *err)
{
    return image_open(d, path, 0, err);
}

int dt_image_open_write(DtDisk *d, const char *path, char *err)
{
    return image_open(d, path, 1, err);
}

void dt_image_close(DtDisk *d)
{
    if (d->ctx)
        fclose((FILE *)d->ctx);
    memset(d, 0, sizeof(*d));
}

static int extended(unsigned t)
{
    return t == 5 || t == 15 || t == 0x85;
}

static int add_partition(DtDisk *d, DtPartition *p, unsigned *n, dt_u32 s, dt_u32 z, unsigned t,
                         char *err)
{
    unsigned i;
    if (!z || !s || s >= d->sectors || z > d->sectors - s)
        return fail(err, "Partition outside disk");
    if (*n == 128)
        return fail(err, "Too many partitions");
    for (i = 0; i < *n; i++)
        if ((dt_u64)s < p[i].start + (dt_u64)p[i].size && (dt_u64)p[i].start < s + (dt_u64)z)
            return fail(err, "Overlapping partitions");
    p[*n].start = s;
    p[*n].size = z;
    p[*n].type = t;
    ++*n;
    return 1;
}

int dt_partitions(DtDisk *d, DtPartition *p, unsigned *n, char *err)
{
    unsigned char m[512], b[512];
    unsigned i, j, k, seen_n;
    dt_u32 base = 0, span = 0, at, next, s, z, seen[128];
    *n = 0;
    if (!d->read(d->ctx, 0, m))
        return fail(err, "Cannot read partition table");
    if (dt_word(m + 510) != 0xaa55)
        return fail(err, "Missing partition table signature");
    for (i = 0; i < 4; i++) {
        unsigned char *e = m + 446 + i * 16;
        if (!e[4])
            continue;
        if (e[4] == 0xee)
            return fail(err, "GPT disks are not supported");
        s = dt_dword(e + 8);
        z = dt_dword(e + 12);
        if (extended(e[4])) {
            if (base)
                return fail(err, "Multiple extended containers");
            if (!s || !z || s >= d->sectors || z > d->sectors - s)
                return fail(err, "Invalid extended container");
            base = s;
            span = z;
        } else if (!add_partition(d, p, n, s, z, e[4], err))
            return 0;
    }
    for (i = 0; i < *n; i++)
        if (base && (dt_u64)base < p[i].start + (dt_u64)p[i].size &&
            (dt_u64)p[i].start < base + (dt_u64)span)
            return fail(err, "Primary partition overlaps extended container");
    at = base;
    seen_n = 0;
    while (at) {
        if (seen_n == 128)
            return fail(err, "Extended chain too long");
        for (j = 0; j < seen_n; j++)
            if (seen[j] == at)
                return fail(err, "Loop in extended partition chain");
        seen[seen_n++] = at;
        if (!d->read(d->ctx, at, b) || dt_word(b + 510) != 0xaa55)
            return fail(err, "Invalid extended partition record");
        next = 0;
        for (k = 0; k < 4; k++) {
            unsigned char *e = b + 446 + k * 16;
            if (!e[4])
                continue;
            s = dt_dword(e + 8);
            z = dt_dword(e + 12);
            if (extended(e[4])) {
                if (next || !s || s >= span || !z || z > span - s)
                    return fail(err, "Invalid extended link");
                next = base + s;
            } else {
                if (!s || s >= base + span - at || !z || z > base + span - at - s)
                    return fail(err, "Logical partition outside container");
                if (!add_partition(d, p, n, at + s, z, e[4], err))
                    return 0;
            }
        }
        at = next;
    }
    for (i = 0; i < seen_n; i++)
        for (j = 0; j < *n; j++)
            if (seen[i] >= p[j].start && seen[i] - p[j].start < p[j].size)
                return fail(err, "Partition overlaps extended record");
    return *n ? 1 : fail(err, "No partitions found");
}

static int mount_boot(DtDisk *d, dt_u32 start, dt_u32 limit, unsigned source, DtVolume *v,
                      char *err)
{
    unsigned char *b;
    dt_u64 data, capacity;
    dt_u32 reserved;
    unsigned entries;
    memset(v, 0, sizeof(*v));
    v->cache_sector = 0xffffffffUL;
    if (start >= d->sectors || !limit || limit > d->sectors - start || source >= limit)
        return fail(err, "Volume outside disk");
    v->disk = d;
    v->start = start;
    v->boot_source = source;
    b = v->boot;
    if (!d->read(d->ctx, start + source, b))
        return fail(err, "Cannot read boot sector");
    if (dt_word(b + 510) != 0xaa55)
        return fail(err, "Missing boot signature");
    if (dt_word(b + 11) != 512)
        return fail(err, "Only 512-byte sectors are supported");
    v->spc = b[13];
    reserved = dt_word(b + 14);
    v->copies = b[16];
    entries = dt_word(b + 17);
    if (!v->spc || v->spc > 128 || (v->spc & (v->spc - 1)) || !reserved || !v->copies ||
        v->copies > 4)
        return fail(err, "Invalid FAT geometry");
    v->total = dt_word(b + 19);
    if (!v->total)
        v->total = dt_dword(b + 32);
    v->fatsz = dt_word(b + 22);
    if (!v->fatsz)
        v->fatsz = dt_dword(b + 36);
    v->rootsz = ((dt_u32)entries * 32 + 511) / 512;
    data = reserved + (dt_u64)v->copies * v->fatsz + v->rootsz;
    if (!v->fatsz || !v->total || v->total > limit || data >= v->total)
        return fail(err, "Invalid FAT extent");
    v->data = (dt_u32)data;
    v->root = v->data - v->rootsz;
    v->fat = reserved;
    v->clusters = (v->total - v->data) / v->spc;
    v->bits = v->clusters < 4085 ? 12 : (v->clusters < 65525 ? 16 : 32);
    capacity = (dt_u64)v->fatsz * 512 * 8 / v->bits;
    if (capacity < v->clusters + 2 || v->clusters > 0x0fffffeeUL)
        return fail(err, "FAT cannot address volume");
    v->mirrored = 1;
    if (v->bits == 32) {
        unsigned flags = dt_word(b + 40);
        if (entries || dt_word(b + 22) || dt_word(b + 42))
            return fail(err, "Invalid FAT32 BPB or version");
        v->rootclus = dt_dword(b + 44);
        v->fsinfo = dt_word(b + 48);
        v->backup = dt_word(b + 50);
        if (v->rootclus < 2 || v->rootclus >= v->clusters + 2)
            return fail(err, "Invalid root cluster");
        if (flags & 128) {
            v->mirrored = 0;
            v->active = flags & 15;
        }
        if (v->active >= v->copies)
            return fail(err, "Invalid active FAT");
        if ((v->fsinfo != 0xffff && v->fsinfo >= reserved) ||
            (v->backup != 0xffff && v->backup >= reserved))
            return fail(err, "FAT32 auxiliary sector outside reserved region");
    } else if (!entries || !dt_word(b + 22))
        return fail(err, "Invalid FAT12/16 root or FAT");
    return 1;
}

int dt_mount(DtDisk *d, dt_u32 start, dt_u32 limit, DtVolume *v, char *err)
{
    return mount_boot(d, start, limit, 0, v, err);
}

int dt_mount_backup(DtDisk *d, dt_u32 start, dt_u32 limit, unsigned source, DtVolume *v, char *err)
{
    if (!source || source == 0xffff)
        return fail(err, "Choose a nonzero FAT32 backup boot sector");
    if (!mount_boot(d, start, limit, source, v, err))
        return 0;
    if (v->bits != 32 || v->backup != source || source >= v->fat || source == v->fsinfo)
        return fail(err, "Backup BPB must describe FAT32 and declare its own source location");
    return 1;
}

int dt_read(DtVolume *v, dt_u32 s, unsigned char *b)
{
    return s < v->total && v->disk->read(v->disk->ctx, v->start + s, b);
}

int dt_fat(DtVolume *v, dt_u32 c, dt_u32 *out)
{
    dt_u32 offset, sec, value = 0;
    unsigned n, i;
    if (c >= v->clusters + 2)
        return 0;
    offset = v->bits == 12 ? c + c / 2 : c * (v->bits / 8);
    n = v->bits == 32 ? 4 : 2;
    for (i = 0; i < n; i++) {
        sec = v->fat + v->active * v->fatsz + (offset + i) / 512;
        if (sec != v->cache_sector) {
            if (!dt_read(v, sec, v->cache))
                return 0;
            v->cache_sector = sec;
        }
        value |= (dt_u32)v->cache[(offset + i) % 512] << (i * 8);
    }
    if (v->bits == 12)
        value = (c & 1) ? value >> 4 : value & 4095;
    if (v->bits == 32)
        value &= 0x0fffffffUL;
    *out = value;
    return 1;
}
