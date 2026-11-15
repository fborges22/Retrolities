#ifndef MAGICDISK_DISK_H
#define MAGICDISK_DISK_H

#include <stdio.h>
#include <stdint.h>

typedef uint32_t dt_u32;
typedef uint64_t dt_u64;

typedef struct {
    void *ctx;
    dt_u32 sectors;
    int (*read)(void *, dt_u32, unsigned char *);
    int (*write)(void *, dt_u32, const unsigned char *);
    int (*sync)(void *);
    int (*separate_destination)(void *, const char *);
} DtDisk;

typedef struct {
    dt_u32 start, size;
    unsigned type;
} DtPartition;

typedef struct {
    DtDisk *disk;
    dt_u32 start, total, fat, fatsz, root, rootsz, data, clusters, rootclus;
    unsigned spc, copies, bits, active, mirrored, fsinfo, backup, boot_source;
    unsigned char boot[512];
    unsigned char cache[512];
    dt_u32 cache_sector;
} DtVolume;

typedef int (*DtProgress)(void *, const char *, dt_u32, dt_u32);
/* Transaction notifications cannot cancel durable journal/metadata writes. */
typedef void (*DtWriteProgress)(void *, const char *, dt_u32, dt_u32);
typedef void (*DtReport)(void *, const char *, dt_u32, const char *);

typedef struct {
    dt_u32 files, dirs, free_clusters, bad_clusters, lost_clusters, errors;
    int complete;
} DtResult;

#define DT_MAP_CELLS 384

typedef struct {
    dt_u32 done, used, free, bad, unreadable;
} DtMapCell;

typedef struct {
    dt_u32 first, count;
    unsigned cells;
    int surface, complete;
    DtMapCell cell[DT_MAP_CELLS];
} DtMap;

/* Map ranges use cluster numbers for allocation and relative sectors for surface reads. */
void dt_map_range(const DtMap *map, unsigned cell, dt_u32 *first, dt_u32 *count);
int dt_map_build(DtVolume *volume, dt_u32 first, dt_u32 count, DtMap *map, DtProgress progress,
                 void *ctx);
int dt_surface(DtVolume *volume, dt_u32 first, dt_u32 count, DtMap *map, DtProgress progress,
               void *ctx);

/* Surface findings use absolute disk LBAs; messages include volume-relative locations. */
int dt_surface_ex(DtVolume *volume, dt_u32 first, dt_u32 count, DtMap *map, DtProgress progress,
                  void *ctx, DtReport report, void *report_ctx);

/* Open functions return a diagnostic in an error buffer of at least 160 bytes. */
#define DT_BIOS_MAX 132

/* Lists BIOS-reported IDs without reading media; output holds DT_BIOS_MAX entries. */
int dt_bios_devices(unsigned *drives, unsigned *count, char *err);
int dt_bios_open(DtDisk *disk, unsigned drive, char *err);
void dt_bios_close(DtDisk *disk);
/* A non-null journal requests locked write access to a mounted DOS volume. */
int dt_dos_open(DtDisk *disk, unsigned drive, const char *journal, char *err);
int dt_dos_close(DtDisk *disk);
int dt_image_open(DtDisk *disk, const char *path, char *err);
void dt_image_close(DtDisk *disk);
int dt_image_open_write(DtDisk *disk, const char *path, char *err);
int dt_file_commit(FILE *file);
int dt_file_commit_directory(const char *path);
FILE *dt_file_create(const char *path);

unsigned dt_word(const unsigned char *bytes);
dt_u32 dt_dword(const unsigned char *bytes);

/* The partition output array must hold 128 entries. All sector units are 512 bytes. */
int dt_partitions(DtDisk *disk, DtPartition *partitions, unsigned *count, char *err);
int dt_mount(DtDisk *disk, dt_u32 start, dt_u32 limit, DtVolume *volume, char *err);
int dt_mount_backup(DtDisk *disk, dt_u32 start, dt_u32 limit, unsigned source, DtVolume *volume,
                    char *err);
int dt_read(DtVolume *volume, dt_u32 sector, unsigned char *buffer);
int dt_fat(DtVolume *volume, dt_u32 cluster, dt_u32 *value);

/* A progress callback returns zero to cancel; cancelled scans are never complete. */
int dt_scan_ex(DtVolume *volume, DtResult *result, DtReport report, void *report_ctx,
               DtProgress progress, void *progress_ctx);
/* On success the caller owns the cluster ownership bitmap; free it with free(). */
int dt_scan_owned(DtVolume *volume, DtResult *result, unsigned char **owned, DtReport report,
                  void *report_ctx, DtProgress progress, void *progress_ctx);
int dt_scan(DtVolume *volume, DtResult *result, DtReport report, void *ctx);

/* Catalog paths are borrowed for the duration of the callback. Cluster counts
   include only the allocated prefix before a shared, free, bad or invalid link. */
enum {
    DT_CHAIN_END,
    DT_CHAIN_RANGE,
    DT_CHAIN_SHARED,
    DT_CHAIN_INVALID,
    DT_CHAIN_LOOP
};

typedef struct {
    dt_u32 sector, start, parent, size, clusters, last;
    unsigned offset;
    int directory, ending;
    const char *path;
} DtFile;

typedef int (*DtFileVisit)(void *, const DtFile *);
int dt_scan_files(DtVolume *volume, DtResult *result, DtFileVisit visit, void *visit_ctx,
                  DtReport report, void *report_ctx, DtProgress progress, void *progress_ctx);

/* Immutable before/after sectors support reviewed changes and journal recovery. */
enum {
    DT_REPAIR_BAD,
    DT_REPAIR_FSINFO,
    DT_REPAIR_BOOT_BACKUP,
    DT_REPAIR_MIRROR,
    DT_REPAIR_LOST,
    DT_REPAIR_CHAINS,
    DT_REPAIR_DIRECTORIES,
    DT_REPAIR_BOOT_PRIMARY,
    DT_REPAIR_TREE,
    DT_REPAIR_TREE_SOURCE,
    DT_REPAIR_ENTRIES,
    DT_REPAIR_RELOCATE
};

typedef struct {
    dt_u32 sector;
    unsigned char before[512], after[512];
} DtPatch;

typedef struct {
    DtVolume *volume;
    dt_u32 disk_sectors, start, total, cluster;
    unsigned count, capacity;
    /* mark_bad holds the 1-based authority for DT_REPAIR_TREE_SOURCE. */
    int mark_bad, kind;
    unsigned char disk_boot[512], volume_boot[512];
    DtPatch *patch;
} DtPlan;

typedef struct {
    dt_u32 marked, readable, failed, tested_sectors;
    int complete;
} DtRetest;

int dt_bad_retest(DtVolume *volume, DtRetest *result, DtReport report, void *report_ctx,
                  DtProgress progress, void *progress_ctx);
int dt_bad_plan(DtVolume *volume, dt_u32 cluster, int mark_bad, DtPlan *plan, DtProgress progress,
                void *ctx, char *err);
int dt_relocate_plan(DtVolume *volume, dt_u32 cluster, DtPlan *plan, DtProgress progress, void *ctx,
                     char *err);
/* Create once, reuse with plan builders, then free. Sector pointers may move on growth. */
DtPlan *dt_plan_create(void);
void dt_plan_clear(DtPlan *plan);
void dt_plan_free(DtPlan *plan);
int dt_plan_init(DtPlan *plan, DtVolume *volume, char *err);
DtPatch *dt_plan_sector(DtPlan *plan, dt_u32 sector, char *err);
int dt_plan_set_fat(DtPlan *plan, dt_u32 cluster, dt_u32 value, unsigned copy, char *err);
int dt_entry_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                     DtProgress progress, void *ctx, char *err);
int dt_entry_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_tree_source_plan_ex(DtVolume *volume, unsigned source, DtPlan *plan, DtReport report,
                           void *report_ctx, DtProgress progress, void *ctx, char *err);
int dt_tree_source_plan(DtVolume *volume, unsigned source, DtPlan *plan, DtProgress progress,
                        void *ctx, char *err);
int dt_tree_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                    DtProgress progress, void *ctx, char *err);
int dt_tree_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_directory_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                         DtProgress progress, void *ctx, char *err);
int dt_directory_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_chain_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                     DtProgress progress, void *ctx, char *err);
int dt_chain_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_lost_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_fsinfo_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_boot_primary_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_boot_backup_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_mirror_plan(DtVolume *volume, unsigned source, DtPlan *plan, DtProgress progress, void *ctx,
                   char *err);
int dt_plan_apply_ex(DtPlan *plan, const char *journal, DtProgress progress, void *ctx,
                     DtWriteProgress writing, void *writing_ctx, char *err);
int dt_plan_restore_ex(DtVolume *volume, const char *journal, DtProgress progress, void *ctx,
                       DtWriteProgress writing, void *writing_ctx, char *err);
int dt_plan_apply(DtPlan *plan, const char *journal, DtProgress progress, void *ctx, char *err);
/* Explicit recovery of sectors composed only of saved before/after bytes. */
int dt_plan_restore_torn_ex(DtVolume *volume, const char *journal, DtProgress progress, void *ctx,
                            DtWriteProgress writing, void *writing_ctx, char *err);
int dt_plan_restore(DtVolume *volume, const char *journal, char *err);

typedef struct {
    dt_u32 lost, clusters, files, unreadable, damaged, issues;
    int complete;
    char directory[260];
} DtRecovery;

int dt_recover_lost(DtVolume *volume, const char *destination, DtRecovery *result,
                    DtProgress progress, void *ctx, char *err);

int dt_export_files(DtVolume *volume, const char *destination, DtRecovery *result,
                    DtProgress progress, void *ctx, char *err);

/* Render UTF-8 through VGA CP437, substituting unavailable glyphs with '?'. */
void dt_display_utf8(const char *input, char *output, size_t capacity, size_t skip);
int dt_ui(const char *path);

#endif
