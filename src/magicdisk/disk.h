#ifndef MAGICDISK_DISK_H
#define MAGICDISK_DISK_H

#include <stdio.h>
#include <stdint.h>

typedef uint32_t dt_u32;
typedef uint64_t dt_u64;

/* Zero-initialize before installing callbacks; absent optional hooks are NULL. */
typedef struct {
    void *ctx;
    dt_u32 sectors;
    int (*read)(void *, dt_u32, unsigned char *);
    int (*write)(void *, dt_u32, const unsigned char *);
    int (*sync)(void *);
    int (*separate_destination)(void *, const char *);
    /* Optional extra durability barrier after the journal file is closed. */
    int (*sync_journal)(void *, const char *);
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
/* Enables only LBA zero, with the same unmounted-disk and durability checks. */
int dt_bios_enable_mbr_write(DtDisk *disk, const char *journal, char *err);
int dt_bios_enable_writes(DtDisk *disk, dt_u32 first, dt_u32 count, const char *journal, char *err);
#ifdef MAGICDISK_BIOS_TEST_WRITES
int dt_bios_test_writes(DtDisk *disk, dt_u32 first, dt_u32 count, int force_chs);
#endif
/* DDT inventory only: unknown entries include aliases and third-party drivers.
   A successful query does not prove that unlisted devices are absent. */
#define DT_BIOS_UNKNOWN 256
int dt_dos_bios_map(unsigned drives[26], char *err);
/* True only for a local canonical path mapped to a different physical disk. */
int dt_dos_bios_destination(unsigned source, const char *path);
int dt_dos_bios_path(const char *path, unsigned *drive);
int dt_dos_flush_buffers(void);
/* Preflight only: require an unmounted disk and a separate known journal disk. */
int dt_dos_bios_write_check(unsigned source, const char *journal, char *err);
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
/* Search every sector in the requested range without using a partition table.
   Candidates require valid BPB geometry and at least one reserved FAT header.
   They may overlap or be stale; they are not partition-repair authority.
   A backup-derived candidate may start before the searched range. The volume
   pointer is valid only during the callback. Zero return means incomplete. */
typedef int (*DtVolumeVisit)(void *, const DtVolume *);
int dt_find_volumes(DtDisk *disk, dt_u32 first, dt_u32 count, DtVolumeVisit visit, void *visit_ctx,
                    DtProgress progress, void *progress_ctx, char *err);
int dt_mount(DtDisk *disk, dt_u32 start, dt_u32 limit, DtVolume *volume, char *err);
int dt_mount_backup(DtDisk *disk, dt_u32 start, dt_u32 limit, unsigned source, DtVolume *volume,
                    char *err);
int dt_read(DtVolume *volume, dt_u32 sector, unsigned char *buffer);
int dt_fat(DtVolume *volume, dt_u32 cluster, dt_u32 *value);

/* Read-only whole-disk preview for one missing primary MBR entry. Geometry
   is supplied explicitly, not guessed from the candidate BPB. Callers must
   use this result only when the builder succeeds. Apply revalidates it. */
typedef struct {
    DtDisk *disk;
    dt_u32 disk_sectors, start, total;
    unsigned slot, boot_source, heads, sectors, bits;
    unsigned char before[512], after[512], volume_boot[512];
} DtMbrPlan;

int dt_mbr_plan(DtDisk *disk, unsigned slot, dt_u32 start, unsigned boot_source, unsigned heads,
                unsigned sectors, DtMbrPlan *plan, char *err);
/* Recompute and compare a reviewed preview before creating its journal. */
int dt_mbr_revalidate(const DtMbrPlan *plan, char *err);

/* Read-only repair of an empty logical entry in a reachable EBR.
   Existing links are preserved. The embedded entry is not an MBR apply plan. */
typedef struct {
    DtMbrPlan entry;
    dt_u32 sector;
    unsigned char mbr[512];
} DtEbrPlan;

int dt_ebr_plan(DtDisk *disk, dt_u32 sector, dt_u32 start, unsigned boot_source, unsigned heads,
                unsigned sectors, DtEbrPlan *plan, char *err);

/* Exact reviewed metadata for each newly linked volume. */
typedef struct {
    dt_u32 record_sector, volume_start;
    unsigned char record[512], boot[512];
} DtLinkWitness;

/* Read-only preview for one missing link to a surviving EBR chain.
   A successful result owns witness memory: clear it before reuse or disposal.
   Failed builders leave a zeroed result. Do not shallow-copy ownership. */
typedef struct {
    DtDisk *disk;
    dt_u32 disk_sectors, sector, target, base, span;
    unsigned heads, sectors, recovered;
    unsigned char mbr[512], before[512], after[512];
    unsigned witness_count;
    DtLinkWitness *witness;
} DtEbrLinkPlan;

/* Recreate an entirely empty reachable EBR table and, optionally, its link.
   The logical plan binds the selected volume; witnesses bind the surviving tail.
   Successful results own witness memory and must be cleared before reuse. */
typedef struct {
    DtEbrPlan logical;
    dt_u32 next;
    unsigned char after[512];
    unsigned witness_count;
    DtLinkWitness *witness;
} DtEbrRecordPlan;

int dt_ebr_record_plan(DtDisk *disk, dt_u32 sector, dt_u32 start, unsigned boot_source, dt_u32 next,
                       unsigned heads, unsigned sectors, DtEbrRecordPlan *plan, char *err);
int dt_ebr_record_revalidate(const DtEbrRecordPlan *plan, char *err);
void dt_ebr_record_clear(DtEbrRecordPlan *plan);
int dt_ebr_record_apply(const DtEbrRecordPlan *plan, const char *journal, char *err);
int dt_ebr_record_restore(DtDisk *disk, const char *journal, int allow_torn, char *err);
int dt_ebr_record_review_journal(DtDisk *disk, const char *journal, DtEbrRecordPlan *plan,
                                 char *err);

/* Container reconstruction preview. Successful plans own their witnesses;
   clear them before reuse. A failed builder returns a zeroed plan. */
typedef struct {
    DtDisk *disk;
    dt_u32 disk_sectors, base, span;
    unsigned slot, type, heads, sectors;
    unsigned char before[512], after[512];
    unsigned witness_count;
    DtLinkWitness *witness;
} DtContainerPlan;

int dt_container_plan(DtDisk *disk, unsigned slot, dt_u32 base, dt_u32 span, unsigned type,
                      unsigned heads, unsigned sectors, DtContainerPlan *plan, char *err);
void dt_container_clear(DtContainerPlan *plan);
int dt_container_revalidate(const DtContainerPlan *plan, char *err);
int dt_container_apply(const DtContainerPlan *plan, const char *journal, char *err);
int dt_container_restore(DtDisk *disk, const char *journal, int allow_torn, char *err);
int dt_container_review_journal(DtDisk *disk, const char *journal, DtContainerPlan *plan,
                                char *err);

int dt_ebr_link_apply(const DtEbrLinkPlan *plan, const char *journal, char *err);
int dt_ebr_link_restore(DtDisk *disk, const char *journal, int allow_torn, char *err);
int dt_ebr_link_review_journal(DtDisk *disk, const char *journal, DtEbrLinkPlan *plan, char *err);
void dt_ebr_link_clear(DtEbrLinkPlan *plan);
int dt_ebr_link_revalidate(const DtEbrLinkPlan *plan, char *err);
int dt_ebr_link_plan(DtDisk *disk, dt_u32 sector, dt_u32 target, unsigned heads, unsigned sectors,
                     DtEbrLinkPlan *plan, char *err);

int dt_ebr_revalidate(const DtEbrPlan *plan, char *err);
int dt_ebr_review_journal(DtDisk *disk, const char *journal, DtEbrPlan *plan, char *err);
int dt_ebr_apply(const DtEbrPlan *plan, const char *journal, char *err);
int dt_ebr_restore(DtDisk *disk, const char *journal, int allow_torn, char *err);
int dt_mbr_apply(const DtMbrPlan *plan, const char *journal, char *err);
int dt_mbr_restore(DtDisk *disk, const char *journal, int allow_torn, char *err);

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
/* Read-only export only: skips unreadable directory sectors after retries.
   Return 1 means traversal finished; result.complete stays false if any were skipped.
   Its partial ownership catalog must never authorize repairs or lost-chain reclaim. */
int dt_scan_recoverable_files(DtVolume *volume, DtResult *result, DtFileVisit visit,
                              void *visit_ctx, DtReport report, void *report_ctx,
                              DtProgress progress, void *progress_ctx);

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
    DT_REPAIR_RELOCATE,
    DT_REPAIR_RELOCATE_DIR,
    DT_REPAIR_RELOCATE_ROOT,
    DT_REPAIR_TIMESTAMPS,
    DT_REPAIR_FAT_HEADERS,
    DT_REPAIR_SALVAGE,
    DT_REPAIR_LFN,
    DT_REPAIR_ALIASES,
    DT_REPAIR_LABELS,
    DT_REPAIR_CONFLICTS,
    DT_REPAIR_ATTRIBUTES,
    DT_REPAIR_EXTRA_DOTS,
    DT_REPAIR_LABEL_TEXT
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
    /* Version 16 records zero-filled source sectors, one bit per cluster sector. */
    unsigned char gaps[16];
    DtPatch *patch;
} DtPlan;

typedef struct {
    dt_u32 marked, readable, failed, tested_sectors, failed_sectors;
    int complete;
} DtRetest;

int dt_bad_retest(DtVolume *volume, DtRetest *result, DtReport report, void *report_ctx,
                  DtProgress progress, void *progress_ctx);
int dt_bad_plan(DtVolume *volume, dt_u32 cluster, int mark_bad, DtPlan *plan, DtProgress progress,
                void *ctx, char *err);
int dt_relocate_plan(DtVolume *volume, dt_u32 cluster, DtPlan *plan, DtProgress progress, void *ctx,
                     char *err);
int dt_salvage_plan(DtVolume *volume, dt_u32 cluster, DtPlan *plan, DtReport report,
                    void *report_ctx, DtProgress progress, void *ctx, char *err);
/* Create once, reuse with plan builders, then free. Sector pointers may move on growth. */
DtPlan *dt_plan_create(void);
void dt_plan_clear(DtPlan *plan);
void dt_plan_free(DtPlan *plan);
int dt_plan_init(DtPlan *plan, DtVolume *volume, char *err);
DtPatch *dt_plan_sector(DtPlan *plan, dt_u32 sector, char *err);
int dt_plan_set_fat(DtPlan *plan, dt_u32 cluster, dt_u32 value, unsigned copy, char *err);
/* Field offsets are 13, 14, 16, 18, 22 and 24 in a short directory entry. */
int dt_label_text_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                          DtProgress progress, void *ctx, char *err);
int dt_label_text_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_extra_dot_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                         DtProgress progress, void *ctx, char *err);
int dt_extra_dot_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_attribute_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                         DtProgress progress, void *ctx, char *err);
int dt_attribute_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_label_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                     DtProgress progress, void *ctx, char *err);
int dt_label_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
/* 1 available, 0 collision, -1 unreadable/malformed/cancelled. Directory zero
   means the root; skip_sector is volume-relative. Does not prove ownership. */
int dt_entry_name_conflicts(DtVolume *volume, dt_u32 directory_start, const unsigned char *entry,
                            const unsigned char *lfn, unsigned count, dt_u32 sector,
                            unsigned offset, DtProgress progress, void *ctx);
int dt_directory_name_available(DtVolume *volume, dt_u32 directory_start, const char *candidate,
                                dt_u32 skip_sector, unsigned skip_offset, DtProgress progress,
                                void *ctx);
/* First invalid alias byte, or -1 when no violation is proven under the OEM policy. */
int dt_invalid_label_byte(const unsigned char *entry);
int dt_invalid_alias_byte(const unsigned char *entry);
int dt_conflict_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                        DtProgress progress, void *ctx, char *err);
int dt_conflict_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_alias_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                     DtProgress progress, void *ctx, char *err);
int dt_alias_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
/* Validate one complete on-disk LFN group against its following alias. */
int dt_lfn_text(const unsigned char *entries, unsigned count, const unsigned char *alias, char *out,
                size_t capacity);
int dt_lfn_valid(const unsigned char *entries, unsigned count, const unsigned char *alias);
int dt_lfn_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                   DtProgress progress, void *ctx, char *err);
int dt_lfn_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
int dt_timestamp_valid(const unsigned char *entry, unsigned field);
int dt_timestamp_plan_ex(DtVolume *volume, DtPlan *plan, DtReport report, void *report_ctx,
                         DtProgress progress, void *ctx, char *err);
int dt_timestamp_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
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
int dt_fat_header_plan(DtVolume *volume, DtPlan *plan, DtProgress progress, void *ctx, char *err);
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
    dt_u32 lost, clusters, files, unreadable, damaged, issues, candidates;
    int complete, catalog_incomplete;
    char directory[260];
} DtRecovery;

int dt_recover_deleted(DtVolume *volume, const char *destination, DtRecovery *result,
                       DtProgress progress, void *ctx, char *err);

/* Read-only discovery from intact dot pairs in allocated, unowned clusters.
   Exported directory contents remain unverified; no directory is reattached. */
int dt_recover_directories(DtVolume *volume, const char *destination, DtRecovery *result,
                           DtProgress progress, void *ctx, char *err);

int dt_recover_lost(DtVolume *volume, const char *destination, DtRecovery *result,
                    DtProgress progress, void *ctx, char *err);

int dt_export_files(DtVolume *volume, const char *destination, DtRecovery *result,
                    DtProgress progress, void *ctx, char *err);

int dt_export_cluster_file(DtVolume *volume, dt_u32 cluster, const char *destination,
                           DtRecovery *result, DtProgress progress, void *ctx, char *err);

/* Render UTF-8 through VGA CP437, substituting unavailable glyphs with '?'. */
void dt_display_utf8(const char *input, char *output, size_t capacity, size_t skip);
void dt_format_sectors(dt_u32 sectors, char *output, size_t capacity);
int dt_parse_u32(const char *text, unsigned base, dt_u32 *value);
int dt_ui(const char *path);

/* Process-wide OEM alias decoding; call before scanning. 0 means unknown.
   Explicit CP437, CP850, CP852, CP860 and CP866 are accepted.
   Failed selection leaves it unchanged.
 */
int dt_set_oem_codepage(unsigned codepage);
unsigned dt_oem_codepage(void);

#endif
