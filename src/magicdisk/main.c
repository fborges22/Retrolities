/* MagicDisk - command-line diagnostics and DOS workspace entry point. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>
#ifdef __WATCOMC__
#include <io.h>
#endif

static void report(void *ctx, const char *code, dt_u32 where, const char *message)
{
    (void)ctx;
#ifdef __WATCOMC__
    /* Redirected reports remain UTF-8; only console output uses VGA glyphs. */
    if (isatty(fileno(stdout))) {
        char *display = (char *)malloc(strlen(message) + 1);
        if (display) {
            dt_display_utf8(message, display, strlen(message) + 1, 0);
            printf("  %-18s %10lu  %s\n", code, (unsigned long)where, display);
            free(display);
            return;
        }
    }
#endif
    printf("  %-18s %10lu  %s\n", code, (unsigned long)where, message);
}

static int close_disk(DtDisk *disk, int bios)
{
    if (bios == 2) {
        if (!dt_dos_close(disk)) {
            puts("DOS flush/unlock failed; stop using the volume and preserve the journal.");
            return 0;
        }
    } else if (bios)
        dt_bios_close(disk);
    else
        dt_image_close(disk);
    return 1;
}

static void usage(void)
{
    puts("Usage: MAGICDSK                         Open the DOS workspace\n"
         "       MAGICDSK --tui [image]           Open an image in the workspace\n"
         "       MAGICDSK image [options]         Inspect an image\n"
         "       MAGICDSK --bios HEX [options]    Inspect a BIOS device\n"
         "       MAGICDSK --drive LETTER [options] Inspect/repair a mounted DOS volume\n"
         "       MAGICDSK --devices               List BIOS-reported devices\n"
         "       MAGICDSK --dos-map               Show DOS-to-BIOS drive inventory\n"
         "       MAGICDSK --cmos [DUMP]           Check CMOS setup (or a 128-byte dump)\n"
         "       MAGICDSK --cvf FILE              Check a DBLSPACE/DRVSPACE.nnn volume\n"
         "Options: --list | --partition N | --surface | --retest-bad\n"
         "         --find-volumes [--search-first LBA] [--search-count SECTORS]\n"
         "         --volume-start LBA             Inspect without a partition entry\n"
         "         --rebuild-record EBR_LBA NEXT_LBA --volume-start LBA --chs H S\n"
         "                                       NEXT_LBA zero ends the chain\n"
         "         --restore-record FILE --apply  Undo whole-EBR reconstruction\n"
         "         --rebuild-container SLOT BASE SECTORS TYPE --chs H S\n"
         "                                       TYPE is hex 05, 0F or 85\n"
         "         --restore-container FILE --apply  Undo container reconstruction\n"
         "         --link-ebr FROM_LBA TO_LBA --chs H S  Preview a missing chain link\n"
         "         --restore-link FILE --apply    Undo a chain-link journal\n"
         "         --rebuild-ebr EBR_LBA --volume-start LBA --chs H S\n"
         "                                       Preview a missing logical entry\n"
         "         --restore-ebr FILE --apply     Undo an EBR reconstruction journal\n"
         "         --rebuild-mbr SLOT --volume-start LBA --chs H S\n"
         "                                       Preview an inactive missing MBR entry\n"
         "         --restore-mbr FILE --apply     Undo an MBR reconstruction journal\n"
         "         --code-page 0|437|850|852|860|866 (0 keeps unknown OEM bytes escaped)\n"
         "         --mark-bad CLUSTER | --unmark-bad CLUSTER\n"
         "         --mark-sector DISK_LBA | --unmark-sector DISK_LBA\n"
         "         --repair-fat-headers           Rebuild reserved FAT entries only\n"
         "         --repair-fsinfo                Rebuild FAT32 free-space metadata\n"
         "         --repair-boot-backup           Copy primary FAT32 boot sector to backup\n"
         "         --repair-boot-primary          Restore primary FAT32 boot from backup\n"
         "         --boot-source SECTOR           Inspect via backup (default recovery: 6)\n"
         "         --repair-fat SOURCE           Rebuild FAT copies from source 1, 2, ...\n"
         "         --fat-source N                Explicit authority for --repair-tree\n"
         "         --repair-tree                 Repair file/directory chains and dot entries\n"
         "         --repair-label-text           Normalize invalid label bytes (preview)\n"
         "         --clear-extra-dots            Remove redundant dot entries (preview)\n"
         "         --repair-attributes           Clear reserved attribute bits (preview)\n"
         "         --repair-labels               Remove duplicate/misplaced labels (preview)\n"
         "         --repair-name-conflicts       Rename duplicate names (preview)\n"
         "         --repair-short-names          Replace invalid DOS aliases (preview)\n"
         "         --clear-invalid-lfn           Remove invalid long-name records (preview)\n"
         "         --clear-invalid-timestamps    Clear invalid date/time fields (preview)\n"
         "         --repair-directory-entries    Correct dot entries/directory sizes (preview)\n"
         "         --repair-directories          Repair directory loops/endings (preview)\n"
         "         --repair-files                Repair file chains and lengths (preview)\n"
         "         --reclaim-lost                Free unowned allocation (recover data first)\n"
         "         --repair-crosslinks           Copy shared clusters to each later owner\n"
         "         --convert-lost                Adopt lost chains as root FILEnnnn.CHK files\n"
         "         --autofix [--surface] [--write-test] [--salvage]\n"
         "                   [--lost-chains files|free|keep]\n"
         "                                       Plan every repair in safe order (preview)\n"
         "         --autofix --apply --journal DIR  Commit, one MDFXnnnn.MDJ per step\n"
         "         --export-files DIRECTORY      Export live files before repairs\n"
         "         --export-cluster N DIRECTORY  Export the file owning cluster N\n"
         "         --recover-directories DIRECTORY  Export orphan-directory contents\n"
         "         --recover-deleted DIRECTORY   Export unverified deleted-file candidates\n"
         "         --recover-lost DIRECTORY      Export lost chains to a separate location\n"
         "         --apply --journal NEWFILE     Apply a previewed repair\n"
         "         --restore-journal FILE --apply  Restore journal sectors\n"
         "         --accept-torn                 Accept only saved before/after byte mixtures\n"
         "Repair writes support images and MS-DOS 6.22/7.1/FreeDOS --drive volumes.\n"
         "BIOS filesystem writes require --partition N, an unmounted IDE disk and a separate IDE "
         "journal "
         "disk.\n"
         "Boot clean DOS without disk caches or disk-writing TSRs before BIOS repairs.\n"
         "BIOS device: 00=A, 01=B, 80=first hard disk. Partition N starts at 1.\n"
         "--relocate-cluster N copies readable file data and marks the source bad.\n"
         "--salvage-cluster N also permits ZERO-filled unreadable file sectors.\n"
         "All inspection and surface operations are read-only.");
}

enum {
    OP_SCAN,
    OP_MBR,
    OP_RECORD,
    OP_RECORD_RESTORE,
    OP_CONTAINER,
    OP_CONTAINER_RESTORE,
    OP_LINK,
    OP_LINK_RESTORE,
    OP_EBR,
    OP_EBR_RESTORE,
    OP_MBR_RESTORE,
    OP_RETEST,
    OP_MARK,
    OP_UNMARK,
    OP_RESTORE,
    OP_FSINFO,
    OP_FAT_HEADERS,
    OP_BOOT_BACKUP,
    OP_MIRROR,
    OP_RECOVER,
    OP_EXPORT_FILES,
    OP_DELETED,
    OP_ORPHAN_DIR,
    OP_RECLAIM,
    OP_CHAINS,
    OP_DIRECTORIES,
    OP_BOOT_PRIMARY,
    OP_TREE,
    OP_ENTRIES,
    OP_TIMESTAMPS,
    OP_LFN,
    OP_CONFLICTS,
    OP_ALIASES,
    OP_LABEL_TEXT,
    OP_EXTRA_DOTS,
    OP_ATTRIBUTES,
    OP_LABELS,
    OP_RELOCATE,
    OP_SALVAGE,
    OP_CROSSLINKS,
    OP_LOST_FILES,
    OP_AUTOFIX
};

static void autofix_note(void *ctx, const char *step, int outcome, const DtPlan *plan,
                         const char *detail)
{
    (void)ctx;
    if (outcome == DT_AUTOFIX_PLANNED)
        printf("PLAN    %-30s %lu items, %u sectors\n",
               step,
               (unsigned long)plan->cluster,
               plan->count);
    else if (outcome == DT_AUTOFIX_APPLIED)
        printf("FIXED   %-30s %u sectors; journal %s\n", step, plan->count, detail);
    else if (outcome == DT_AUTOFIX_FAILED)
        printf("FAILED  %-30s %s\n", step, detail);
    else
        printf("SKIPPED %-30s %s\n", step, detail);
}

static int autofix_command(DtVolume *v, const DtAutofixOptions *options)
{
    DtAutofix result;
    char err[160];
    int ok;

    puts(options->apply ? "Automatic repair: every step is journaled and verified."
                        : "Automatic repair preview: no sectors are written.");
    ok = dt_autofix(
        v, options, &result, autofix_note, NULL, report, NULL, NULL, NULL, NULL, NULL, err);
    if (!ok) {
        puts(err);
        if (result.applied)
            printf("%u steps were committed before stopping. Restore MDFX journals in reverse "
                   "order to undo.\n",
                   result.applied);
        return 2;
    }
    printf("Before: %lu issues, %lu lost clusters, %lu bad clusters.\n",
           (unsigned long)result.initial.errors,
           (unsigned long)result.initial.lost_clusters,
           (unsigned long)result.initial.bad_clusters);
    if (options->surface || options->write_test)
        printf("Surface: %s, %lu unreadable sectors, %lu not retired.\n",
               result.surface_complete ? "complete" : "INCOMPLETE",
               (unsigned long)result.surface_failed,
               (unsigned long)result.unresolved_bad);
    if (options->write_test && options->apply)
        printf("Write test: %s, %lu free sectors verified, %lu clusters failed%s.\n",
               result.write_complete ? "complete" : "INCOMPLETE",
               (unsigned long)result.write_tested,
               (unsigned long)result.write_failed,
               result.write_unrestored ? ", SOME FREE SECTORS NOT RESTORED" : "");
    else if (options->write_test)
        puts("Write test runs only with --apply; the preview performed read tests.");
    if (options->apply) {
        printf("Committed %u repairs in %u passes.\n", result.applied, result.passes);
        printf("After: %s, %lu issues, %lu lost clusters, %lu bad clusters.\n",
               result.final.complete ? "complete" : "INCOMPLETE",
               (unsigned long)result.final.errors,
               (unsigned long)result.final.lost_clusters,
               (unsigned long)result.final.bad_clusters);
        if (!result.converged)
            puts("Stopped at the pass limit; run --autofix again to continue.");
        if (result.applied)
            puts("Keep the journals; restore them newest first to undo the whole sequence.");
    } else {
        printf("%u repair steps apply to the current state.\n", result.planned);
        if (result.planned)
            puts("Later steps are re-planned after each commit; repeat with --apply --journal "
                 "DIR.");
    }
    if (!result.final.complete ||
        ((options->surface || options->write_test) && !result.surface_complete) ||
        (options->write_test && options->apply && !result.write_complete))
        return 2;
    return result.final.errors || result.unresolved_bad || (!options->apply && result.planned)
               ? 1
               : 0;
}

static int repair_command(DtVolume *v, int op, dt_u32 cluster, unsigned source, int apply,
                          const char *journal, int torn)
{
    DtPlan *plan;
    DtRetest retest;
    char err[160];
    unsigned i;
    int ok;

    if (op == OP_RETEST) {
        dt_bad_retest(v, &retest, report, NULL, NULL, NULL);
        printf("Bad-cluster retest %s: %lu marked, %lu readable, %lu still failing, "
               "%lu sectors attempted, %lu failed. Markers unchanged.\n",
               retest.complete ? "complete" : "INCOMPLETE",
               (unsigned long)retest.marked,
               (unsigned long)retest.readable,
               (unsigned long)retest.failed,
               (unsigned long)retest.tested_sectors,
               (unsigned long)retest.failed_sectors);
        return !retest.complete ? 2 : (retest.failed ? 1 : 0);
    }
    if (op == OP_RESTORE) {
        if (!(torn ? dt_plan_restore_torn_ex(v, journal, NULL, NULL, NULL, NULL, err)
                   : dt_plan_restore(v, journal, err))) {
            puts(err);
            return 2;
        }
        puts("Original metadata restored and verified. Journal retained.");
        return 0;
    }
    plan = dt_plan_create();
    if (!plan) {
        puts("Insufficient memory for repair plan");
        return 2;
    }
    if (op == OP_SALVAGE)
        ok = dt_salvage_plan(v, cluster, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_RELOCATE)
        ok = dt_relocate_plan(v, cluster, plan, NULL, NULL, err);
    else if (op == OP_FAT_HEADERS)
        ok = dt_fat_header_plan(v, plan, NULL, NULL, err);
    else if (op == OP_FSINFO)
        ok = dt_fsinfo_plan(v, plan, NULL, NULL, err);
    else if (op == OP_BOOT_BACKUP)
        ok = dt_boot_backup_plan(v, plan, NULL, NULL, err);
    else if (op == OP_BOOT_PRIMARY)
        ok = dt_boot_primary_plan(v, plan, NULL, NULL, err);
    else if (op == OP_TREE && source)
        ok = dt_tree_source_plan_ex(v, source, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_TREE)
        ok = dt_tree_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_LABEL_TEXT)
        ok = dt_label_text_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_EXTRA_DOTS)
        ok = dt_extra_dot_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_ATTRIBUTES)
        ok = dt_attribute_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_LABELS)
        ok = dt_label_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_CONFLICTS)
        ok = dt_conflict_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_ALIASES)
        ok = dt_alias_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_LFN)
        ok = dt_lfn_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_TIMESTAMPS)
        ok = dt_timestamp_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_ENTRIES)
        ok = dt_entry_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_DIRECTORIES)
        ok = dt_directory_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_CHAINS)
        ok = dt_chain_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_RECLAIM)
        ok = dt_lost_plan(v, plan, NULL, NULL, err);
    else if (op == OP_CROSSLINKS)
        ok = dt_crosslink_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_LOST_FILES)
        ok = dt_lost_files_plan_ex(v, dt_dos_now(), plan, report, NULL, NULL, NULL, err);
    else if (op == OP_MIRROR)
        ok = dt_mirror_plan(v, (unsigned)cluster, plan, NULL, NULL, err);
    else
        ok = dt_bad_plan(v, cluster, op == OP_MARK, plan, NULL, NULL, err);
    if (ok) {
        if (!plan->count) {
            puts("Metadata is already up to date; no sectors written and no journal created.");
            dt_plan_free(plan);
            return 0;
        }
        if (op == OP_SALVAGE) {
            unsigned sector, gaps = 0;

            for (sector = 0; sector < v->spc; sector++) {
                if (plan->gaps[sector / 8] & (1u << (sector % 8)))
                    gaps++;
            }
            printf("Salvage file cluster %lu: %u unreadable sectors become ZERO (%lu bytes).\n",
                   (unsigned long)cluster,
                   gaps,
                   (unsigned long)gaps * 512);
            puts("Readable bytes, name and size are preserved; original cluster is marked bad.");
            puts("Missing bytes are NOT recovered. Export needed data before applying.");
        } else if (op == OP_RELOCATE) {
            printf("Relocate cluster %lu and mark its original allocation bad; %u journaled "
                   "sectors.\n",
                   (unsigned long)cluster,
                   plan->count);
        } else if (op == OP_FAT_HEADERS) {
            printf("Rebuild reserved FAT headers; %u metadata sectors. Status bits retained.\n",
                   plan->count);
        } else if (op == OP_FSINFO) {
            printf("Rebuild FAT32 FSInfo; %u metadata sectors.\n", plan->count);
            printf("  Free clusters: %lu; next-free hint: %lu\n",
                   (unsigned long)dt_dword(plan->patch[0].after + 488),
                   (unsigned long)dt_dword(plan->patch[0].after + 492));
        } else if (op == OP_BOOT_BACKUP) {
            printf("Copy primary FAT32 boot sector at disk LBA %lu to backup; 1 metadata sector.\n",
                   (unsigned long)v->start);
        } else if (op == OP_BOOT_PRIMARY) {
            printf("Restore primary disk LBA %lu from backup disk LBA %lu; one 512-byte sector.\n",
                   (unsigned long)v->start,
                   (unsigned long)(v->start + v->boot_source));
        } else if (op == OP_TREE) {
            if (source)
                printf("Use FAT %u as authority; copy its repaired allocation to every FAT.\n",
                       source);
            printf("Repair %lu files/directories; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Keep unique directory prefixes; shared tails stay with their first owner.");
            puts("Fully shared directories become independent EMPTY directories.");
            puts("Later files may become empty; lengths may change to full cluster boundaries.");
            puts("Leading entries may move within the directory; LFN groups remain intact.");
            puts("Full directories may gain free clusters; original free bytes are journaled.");
            puts("Correct dot order/duplicates; rebuild missing dots with unset timestamps.");
        } else if (op == OP_LABEL_TEXT) {
            printf("Normalize %lu volume labels; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
        } else if (op == OP_EXTRA_DOTS) {
            printf("Remove %lu redundant dot records; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
        } else if (op == OP_ATTRIBUTES) {
            printf("Clear reserved attribute bits in %lu records; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Preserve defined flags, entry types, names, file contents and allocation.");
        } else if (op == OP_LABELS) {
            printf("Remove %lu duplicate/misplaced labels; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Keep the first root label; entries with cluster pointers or sizes are refused.");
        } else if (op == OP_CONFLICTS) {
            printf("Rename %lu conflicting/invalid entries; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Keep first names; replace later conflicts with recovery names. Preserve data.");
        } else if (op == OP_ALIASES) {
            printf("Replace %lu invalid DOS aliases; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Recovery names replace invalid aliases; valid LFNs and file contents stay "
                 "intact.");
        } else if (op == OP_LFN) {
            printf("Delete %lu invalid LFN records; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Invalid long names are lost; DOS aliases, valid names and contents stay intact.");
        } else if (op == OP_TIMESTAMPS) {
            printf("Clear %lu invalid timestamp fields; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Invalid fields become zero; valid fields, names and file data stay intact.");
        } else if (op == OP_ENTRIES) {
            printf("Repair entries in %lu directories; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Correct dot order/duplicates; rebuild missing dots with unset timestamps.");
            puts("Leading entries may move to unused slots; LFN groups remain intact.");
            puts("Full directories may gain free clusters; original free bytes are journaled.");
            puts("Insufficient space, hidden entries and ambiguous owners are "
                 "refused.");
        } else if (op == OP_DIRECTORIES) {
            printf("Repair %lu directory chains; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Keep each readable allocated prefix. Directory entries and file data are "
                 "unchanged.");
            puts("Detach shared tails; directories without a unique prefix are refused.");
        } else if (op == OP_CHAINS) {
            printf("Repair %lu file chains/lengths; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Cross-links keep the first scanned owner; later files retain only unique "
                 "prefixes.");
            puts("Short lengths may expand to full clusters. Back up needed data before applying.");
        } else if (op == OP_RECLAIM) {
            printf("Reclaim %lu lost clusters; %u metadata sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Recover needed data first. Freed clusters may be overwritten by future writes.");
        } else if (op == OP_CROSSLINKS) {
            printf("Copy cross-linked clusters for %lu files; %u sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("The first owner keeps the shared clusters; each later file gets private "
                 "copies.");
            puts("New clusters come from readable free space. Names and dates stay intact.");
        } else if (op == OP_LOST_FILES) {
            printf("Convert lost chains into %lu root FILEnnnn.CHK files; %u sectors.\n",
                   (unsigned long)plan->cluster,
                   plan->count);
            puts("Lengths are whole clusters. Unused root slots are preferred over deleted "
                 "entries.");
        } else if (op == OP_MIRROR) {
            printf("Rebuild FAT copies from source %lu; %u metadata sectors.\n",
                   (unsigned long)cluster,
                   plan->count);
        } else {
            dt_u32 first = v->start + v->data + (cluster - 2) * v->spc;
            printf("%s cluster %lu; disk sectors %lu-%lu; %u metadata sectors.\n",
                   op == OP_MARK ? "Mark bad" : "Unmark bad",
                   (unsigned long)cluster,
                   (unsigned long)first,
                   (unsigned long)(first + v->spc - 1),
                   plan->count);
        }
        for (i = 0; i < plan->count; i++)
            printf("  Metadata disk LBA %lu\n", (unsigned long)(v->start + plan->patch[i].sector));
        if (apply) {
            ok = dt_plan_apply(plan, journal, NULL, NULL, err);
            if (ok)
                puts("Repair committed and verified. Keep the journal for recovery.");
        } else {
            puts("Preview only. Repeat with --apply --journal NEWFILE to write this change.");
        }
    }
    if (!ok)
        puts(err);
    dt_plan_free(plan);
    return ok ? 0 : 2;
}

static int select_codepage(const char *value)
{
    dt_u32 codepage;

    if (!dt_parse_u32(value, 10, &codepage) ||
        (codepage > 65535 || !dt_set_oem_codepage((unsigned)codepage))) {
        puts("Code page must be 0 (unknown), 437, 850, 852, 860 or 866");
        return 0;
    }
    return 1;
}

static int found_volume(void *ctx, const DtVolume *volume)
{
    dt_u32 *count = (dt_u32 *)ctx;

    ++*count;
    printf("Candidate %lu: FAT%u  LBA %lu  sectors %lu  boot-source %u\n",
           (unsigned long)*count,
           volume->bits,
           (unsigned long)volume->start,
           (unsigned long)volume->total,
           volume->boot_source);
    return 1;
}

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPartition partitions[128];
    DtResult result;
    DtMap map;
    unsigned count = 0, i, index = 0, drive = 0, boot_source = 0, fat_source = 0;
    char err[160];
    int listing = 0, bios = 0, surface = 0, arg = 2, status;
    int find_volumes = 0, explicit_start = 0, search_range = 0;
    unsigned mbr_slot = 0, mbr_heads = 0, mbr_sectors = 0;
    unsigned container_type = 0;
    dt_u32 container_base = 0, container_span = 0;
    dt_u32 search_first = 0, search_count = 0, found_count = 0;
    int op = OP_SCAN, apply = 0, sector_arg = 0, torn = 0;
    dt_u32 record_sector = 0, record_next = 0;
    dt_u32 cluster = 0, ebr_sector = 0, link_source = 0, link_target = 0;
    const char *journal = NULL, *destination = NULL;
    dt_u32 start = 0, size, failures = 0;
    DtAutofixOptions autofix;
    int lost_option = 0;

    memset(&autofix, 0, sizeof(autofix));

    while (argc > 1 && !strcmp(argv[1], "--code-page")) {
        if (argc < 3 || !select_codepage(argv[2]))
            return 2;
        memmove(argv + 1, argv + 3, (argc - 2) * sizeof(*argv));
        argc -= 2;
    }
    if (argc == 1)
        return dt_ui(NULL);
    if (!strcmp(argv[1], "--tui") && argc <= 3)
        return dt_ui(argc == 3 ? argv[2] : NULL);
    if (!strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    puts("MagicDisk 0.3 - DOS disk diagnostics");
    if (!strcmp(argv[1], "--dos-map")) {
        unsigned drives[26];

        if (argc != 2) {
            usage();
            return 2;
        }
        if (!dt_dos_bios_map(drives, err)) {
            puts(err);
            return 2;
        }
        puts("DOS  BIOS device (kernel drive-data table)");
        for (i = 0; i < 26; i++) {
            if (drives[i] != DT_BIOS_UNKNOWN)
                printf("%c:   %02Xh\n", 'A' + i, drives[i]);
        }
        puts("Unlisted letters are unknown, including aliases and third-party drivers.");
        puts("This inventory does not authorize raw BIOS writes.");
        return 0;
    }
    if (!strcmp(argv[1], "--cvf")) {
        DtCvfResult cvf;

        if (argc != 3) {
            usage();
            return 2;
        }
        if (!dt_cvf_check(argv[2], &cvf, report, NULL, NULL, NULL, err)) {
            puts(err);
            return 2;
        }
        printf("%s volume %s: FAT%u, %lu clusters (%lu used: %lu compressed, %lu raw, %lu "
               "sparse)\n",
               cvf.drivespace ? "DriveSpace" : "DoubleSpace",
               cvf.oem,
               cvf.fat12 ? 12 : 16,
               (unsigned long)cvf.clusters,
               (unsigned long)cvf.used,
               (unsigned long)cvf.compressed,
               (unsigned long)cvf.raw,
               (unsigned long)cvf.sparse);
        printf("Sector heap %lu/%lu sectors used; %lu files, %lu directories.\n",
               (unsigned long)cvf.heap_used,
               (unsigned long)cvf.heap_sectors,
               (unsigned long)cvf.files,
               (unsigned long)cvf.dirs);
        printf("Compressed volume check %s: %lu issues. Nothing was written.\n",
               cvf.complete ? "complete" : "PARTIAL",
               (unsigned long)cvf.errors);
        return cvf.errors ? 1 : cvf.complete ? 0 : 2;
    }
    if (!strcmp(argv[1], "--cmos")) {
        unsigned char cmos[128];
        DtCmosHost host;
        unsigned issues;

        host.floppies = host.base_kb = DT_CMOS_UNKNOWN;
        if (argc > 3) {
            usage();
            return 2;
        }
        if (argc == 3) {
            FILE *dump = fopen(argv[2], "rb");

            if (!dump || fread(cmos, 1, 128, dump) != 128) {
                if (dump)
                    fclose(dump);
                puts("A CMOS dump must contain at least 128 bytes");
                return 2;
            }
            fclose(dump);
        } else if (!dt_cmos_read(cmos, &host, err)) {
            puts(err);
            return 2;
        }
        issues = dt_cmos_check(cmos, &host, report, NULL);
        printf("CMOS check complete: %u issues. Nothing was written.\n", issues);
        return issues ? 1 : 0;
    }
    if (!strcmp(argv[1], "--devices")) {
        unsigned drives[DT_BIOS_MAX];

        if (argc != 2) {
            usage();
            return 2;
        }
        if (!dt_bios_devices(drives, &count, err)) {
            puts(err);
            return 2;
        }
        puts("BIOS  Device (media is checked when opened)");
        for (i = 0; i < count; i++)
            printf("%02Xh   %s %u\n",
                   drives[i],
                   drives[i] < 128 ? "Floppy drive" : "Hard disk",
                   drives[i] < 128 ? drives[i] + 1 : drives[i] - 127);
        if (!count)
            puts("No devices reported. Use --bios HEX to try a known device directly.");
        return 0;
    }
    if (!strcmp(argv[1], "--bios")) {
        dt_u32 value;
        if (argc < 3) {
            usage();
            return 2;
        }
        if (!dt_parse_u32(argv[2], 16, &value) || value > 255) {
            puts("Invalid BIOS drive number");
            return 2;
        }
        drive = (unsigned)value;
        bios = 1;
        arg = 3;
    }
    if (!strcmp(argv[1], "--drive")) {
        unsigned letter;
        if (argc < 3 || !argv[2][0] || (argv[2][1] && (argv[2][1] != ':' || argv[2][2]))) {
            puts("Specify a single DOS drive letter");
            return 2;
        }
        letter = (unsigned char)argv[2][0];
        if (letter >= 'a' && letter <= 'z')
            letter -= 'a' - 'A';
        if (letter < 'A' || letter > 'Z') {
            puts("Invalid DOS drive letter");
            return 2;
        }
        drive = letter - 'A' + 1;
        bios = 2;
        arg = 3;
    }
    while (arg < argc) {
        if (!strcmp(argv[arg], "--rebuild-record") && op == OP_SCAN && arg + 2 < argc) {
            if (!dt_parse_u32(argv[++arg], 10, &record_sector) || !record_sector ||
                !dt_parse_u32(argv[++arg], 10, &record_next)) {
                puts("Whole-record recovery needs an EBR LBA and next EBR LBA (zero ends chain)");
                return 2;
            }
            op = OP_RECORD;
        } else if (!strcmp(argv[arg], "--restore-record") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_RECORD_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--rebuild-container") && op == OP_SCAN && arg + 4 < argc) {
            dt_u32 slot, type;

            if (!dt_parse_u32(argv[++arg], 10, &slot) || slot < 1 || slot > 4 ||
                !dt_parse_u32(argv[++arg], 10, &container_base) || !container_base ||
                !dt_parse_u32(argv[++arg], 10, &container_span) || !container_span ||
                !dt_parse_u32(argv[++arg], 16, &type) ||
                (type != 5 && type != 15 && type != 0x85)) {
                puts("Container requires slot 1-4, nonzero base/size and type 05, 0F or 85");
                return 2;
            }
            mbr_slot = (unsigned)slot;
            container_type = (unsigned)type;
            op = OP_CONTAINER;
        } else if (!strcmp(argv[arg], "--restore-container") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_CONTAINER_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--link-ebr") && op == OP_SCAN && arg + 2 < argc) {
            if (!dt_parse_u32(argv[++arg], 10, &link_source) || !link_source ||
                !dt_parse_u32(argv[++arg], 10, &link_target) || !link_target) {
                puts("EBR link requires nonzero source and target disk LBAs");
                return 2;
            }
            op = OP_LINK;
        } else if (!strcmp(argv[arg], "--restore-link") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_LINK_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--rebuild-ebr") && op == OP_SCAN && arg + 1 < argc) {
            if (!dt_parse_u32(argv[++arg], 10, &ebr_sector) || !ebr_sector) {
                puts("EBR sector must be a nonzero disk LBA");
                return 2;
            }
            op = OP_EBR;
        } else if (!strcmp(argv[arg], "--restore-ebr") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_EBR_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--rebuild-mbr") && op == OP_SCAN && arg + 1 < argc) {
            dt_u32 value;

            if (!dt_parse_u32(argv[++arg], 10, &value) || value < 1 || value > 4) {
                puts("MBR slot must be from 1 through 4");
                return 2;
            }
            mbr_slot = (unsigned)value;
            op = OP_MBR;
        } else if (!strcmp(argv[arg], "--restore-mbr") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_MBR_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--chs") && arg + 2 < argc) {
            dt_u32 heads, sectors;

            if (!dt_parse_u32(argv[++arg], 10, &heads) || !heads || heads > 256 ||
                !dt_parse_u32(argv[++arg], 10, &sectors) || !sectors || sectors > 63) {
                puts("CHS geometry requires 1..256 heads and 1..63 sectors per track");
                return 2;
            }
            mbr_heads = (unsigned)heads;
            mbr_sectors = (unsigned)sectors;
        } else if ((!strcmp(argv[arg], "--chs-heads") || !strcmp(argv[arg], "--chs-sectors")) &&
                   arg + 1 < argc) {
            dt_u32 value;
            int heads = !strcmp(argv[arg], "--chs-heads");

            if (!dt_parse_u32(argv[++arg], 10, &value) || !value || value > (heads ? 256 : 63)) {
                puts("CHS geometry requires 1..256 heads and 1..63 sectors per track");
                return 2;
            }
            if (heads)
                mbr_heads = (unsigned)value;
            else
                mbr_sectors = (unsigned)value;
        } else if (!strcmp(argv[arg], "--find-volumes")) {
            find_volumes = 1;
        } else if ((!strcmp(argv[arg], "--search-first") || !strcmp(argv[arg], "--search-count") ||
                    !strcmp(argv[arg], "--volume-start")) &&
                   arg + 1 < argc) {
            dt_u32 value;
            int range_first = !strcmp(argv[arg], "--search-first");
            int range_count = !strcmp(argv[arg], "--search-count");

            if (!dt_parse_u32(argv[++arg], 10, &value) || (range_count && !value)) {
                puts("Invalid volume-search sector or count");
                return 2;
            }
            if (range_first) {
                search_first = value;
                search_range = 1;
            } else if (range_count) {
                search_count = value;
                search_range = 1;
            } else {
                start = value;
                explicit_start = 1;
            }
        } else if (!strcmp(argv[arg], "--list")) {
            listing = 1;
        } else if (!strcmp(argv[arg], "--code-page") && arg + 1 < argc) {
            if (!select_codepage(argv[++arg]))
                return 2;
        } else if (!strcmp(argv[arg], "--surface")) {
            surface = 1;
        } else if (!strcmp(argv[arg], "--accept-torn")) {
            torn = 1;
        } else if (!strcmp(argv[arg], "--apply")) {
            apply = 1;
        } else if (!strcmp(argv[arg], "--journal") && arg + 1 < argc) {
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--recover-directories") && arg + 1 < argc && op == OP_SCAN) {
            op = OP_ORPHAN_DIR;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--recover-deleted") && arg + 1 < argc && op == OP_SCAN) {
            op = OP_DELETED;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--export-cluster") && op == OP_SCAN && arg + 2 < argc) {
            dt_u32 n;

            if (!dt_parse_u32(argv[++arg], 10, &n) || n < 2 || n > 0xfffffffeUL) {
                puts("Invalid file cluster number");
                return 2;
            }
            cluster = (dt_u32)n;
            op = OP_EXPORT_FILES;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--export-files") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_EXPORT_FILES;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--recover-lost") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_RECOVER;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--retest-bad") && op == OP_SCAN) {
            op = OP_RETEST;
        } else if (!strcmp(argv[arg], "--repair-fat-headers") && op == OP_SCAN) {
            op = OP_FAT_HEADERS;
        } else if (!strcmp(argv[arg], "--repair-fsinfo") && op == OP_SCAN) {
            op = OP_FSINFO;
        } else if (!strcmp(argv[arg], "--repair-boot-backup") && op == OP_SCAN) {
            op = OP_BOOT_BACKUP;
        } else if (!strcmp(argv[arg], "--repair-boot-primary") && op == OP_SCAN) {
            op = OP_BOOT_PRIMARY;
        } else if (!strcmp(argv[arg], "--boot-source") && arg + 1 < argc) {
            dt_u32 n;
            if (!dt_parse_u32(argv[++arg], 10, &n) || n < 1 || n > 65534) {
                puts("Backup sector must be numbered from 1 through 65534");
                return 2;
            }
            boot_source = (unsigned)n;
        } else if (!strcmp(argv[arg], "--repair-tree") && op == OP_SCAN) {
            op = OP_TREE;
        } else if (!strcmp(argv[arg], "--repair-label-text") && op == OP_SCAN) {
            op = OP_LABEL_TEXT;
        } else if (!strcmp(argv[arg], "--clear-extra-dots") && op == OP_SCAN) {
            op = OP_EXTRA_DOTS;
        } else if (!strcmp(argv[arg], "--repair-attributes") && op == OP_SCAN) {
            op = OP_ATTRIBUTES;
        } else if (!strcmp(argv[arg], "--repair-labels") && op == OP_SCAN) {
            op = OP_LABELS;
        } else if (!strcmp(argv[arg], "--repair-name-conflicts") && op == OP_SCAN) {
            op = OP_CONFLICTS;
        } else if (!strcmp(argv[arg], "--repair-short-names") && op == OP_SCAN) {
            op = OP_ALIASES;
        } else if (!strcmp(argv[arg], "--clear-invalid-lfn") && op == OP_SCAN) {
            op = OP_LFN;
        } else if (!strcmp(argv[arg], "--clear-invalid-timestamps") && op == OP_SCAN) {
            op = OP_TIMESTAMPS;
        } else if (!strcmp(argv[arg], "--repair-directory-entries") && op == OP_SCAN) {
            op = OP_ENTRIES;
        } else if (!strcmp(argv[arg], "--repair-directories") && op == OP_SCAN) {
            op = OP_DIRECTORIES;
        } else if (!strcmp(argv[arg], "--repair-files") && op == OP_SCAN) {
            op = OP_CHAINS;
        } else if (!strcmp(argv[arg], "--reclaim-lost") && op == OP_SCAN) {
            op = OP_RECLAIM;
        } else if (!strcmp(argv[arg], "--repair-crosslinks") && op == OP_SCAN) {
            op = OP_CROSSLINKS;
        } else if (!strcmp(argv[arg], "--convert-lost") && op == OP_SCAN) {
            op = OP_LOST_FILES;
        } else if (!strcmp(argv[arg], "--autofix") && op == OP_SCAN) {
            op = OP_AUTOFIX;
        } else if (!strcmp(argv[arg], "--salvage")) {
            autofix.salvage = 1;
        } else if (!strcmp(argv[arg], "--write-test")) {
            autofix.write_test = 1;
        } else if (!strcmp(argv[arg], "--lost-chains") && arg + 1 < argc) {
            const char *mode = argv[++arg];

            autofix.lost_mode = !strcmp(mode, "files") ? DT_AUTOFIX_LOST_FILES
                                : !strcmp(mode, "free") ? DT_AUTOFIX_LOST_FREE
                                : !strcmp(mode, "keep") ? DT_AUTOFIX_LOST_KEEP
                                                        : -1;
            lost_option = 1;
            if (autofix.lost_mode < 0) {
                puts("--lost-chains accepts files, free or keep");
                return 2;
            }
        } else if (!strcmp(argv[arg], "--fat-source") && arg + 1 < argc) {
            dt_u32 n;

            if (!dt_parse_u32(argv[++arg], 10, &n) || n < 1 || n > 4) {
                puts("Source FAT must be numbered from 1 through 4");
                return 2;
            }
            fat_source = (unsigned)n;
        } else if (!strcmp(argv[arg], "--repair-fat") && op == OP_SCAN && arg + 1 < argc) {
            dt_u32 n;

            if (!dt_parse_u32(argv[++arg], 10, &n) || n < 1 || n > 4) {
                puts("Source FAT must be numbered from 1 through 4");
                return 2;
            }
            op = OP_MIRROR;
            cluster = (dt_u32)n;
        } else if ((!strcmp(argv[arg], "--salvage-cluster") ||
                    !strcmp(argv[arg], "--relocate-cluster") || !strcmp(argv[arg], "--mark-bad") ||
                    !strcmp(argv[arg], "--unmark-bad") || !strcmp(argv[arg], "--mark-sector") ||
                    !strcmp(argv[arg], "--unmark-sector")) &&
                   op == OP_SCAN && arg + 1 < argc) {
            dt_u32 n;
            sector_arg = strstr(argv[arg], "-sector") != NULL;
            op = !strcmp(argv[arg], "--salvage-cluster")    ? OP_SALVAGE
                 : !strcmp(argv[arg], "--relocate-cluster") ? OP_RELOCATE
                 : (!strcmp(argv[arg], "--mark-bad") || !strcmp(argv[arg], "--mark-sector"))
                     ? OP_MARK
                     : OP_UNMARK;
            if (!dt_parse_u32(argv[++arg], 10, &n) || n > 0xfffffffeUL || (!sector_arg && n < 2)) {
                puts("Invalid cluster or disk sector number");
                return 2;
            }
            cluster = (dt_u32)n;
        } else if (!strcmp(argv[arg], "--restore-journal") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--partition") && arg + 1 < argc) {
            dt_u32 n;

            if (!dt_parse_u32(argv[++arg], 10, &n) || n < 1 || n > 128) {
                puts("Invalid partition number");
                return 2;
            }
            index = (unsigned)n;
        } else {
            usage();
            return 2;
        }
        arg++;
    }
    if (op == OP_RECORD || op == OP_RECORD_RESTORE) {
        DtEbrRecordPlan record;
        int restore = op == OP_RECORD_RESTORE;

        if (bios == 2 || index || listing || surface || find_volumes || search_range ||
            fat_source || destination || (torn && (!restore || !apply)) || (apply && !journal) ||
            (!apply && journal) ||
            (restore && (!apply || explicit_start || boot_source || mbr_heads || mbr_sectors)) ||
            (!restore && (!explicit_start || !mbr_heads || !mbr_sectors))) {
            puts("Whole-record recovery needs a whole disk, volume start and CHS geometry.");
            puts("Undo needs --restore-record FILE --apply; writes require a journal.");
            return 2;
        }
        status = bios    ? dt_bios_open(&disk, drive, err)
                 : apply ? dt_image_open_write(&disk, argv[1], err)
                         : dt_image_open(&disk, argv[1], err);
        if (!status) {
            puts(err);
            return 2;
        }
        status = restore ? dt_ebr_record_review_journal(&disk, journal, &record, err)
                         : dt_ebr_record_plan(&disk,
                                              record_sector,
                                              start,
                                              boot_source,
                                              record_next,
                                              mbr_heads,
                                              mbr_sectors,
                                              &record,
                                              err);
        if (status) {
            const DtMbrPlan *logical = &record.logical.entry;

            printf("EBR record %s: sector %lu, volume LBA %lu, next EBR %lu\n",
                   restore ? "undo" : "preview",
                   (unsigned long)record.logical.sector,
                   (unsigned long)logical->start,
                   (unsigned long)record.next);
            printf("FAT%u, %lu sectors, boot-source %u; CHS %u/%u; %u tail volumes\n",
                   logical->bits,
                   (unsigned long)logical->total,
                   logical->boot_source,
                   logical->heads,
                   logical->sectors,
                   record.witness_count);
            for (i = 0; i < record.witness_count; i++)
                printf("  EBR %lu: volume LBA %lu, sectors %lu, type %02X\n",
                       (unsigned long)record.witness[i].record_sector,
                       (unsigned long)record.witness[i].volume_start,
                       (unsigned long)dt_dword(record.witness[i].record + 458),
                       record.witness[i].record[450]);
            puts("Only the selected EBR table/signature changes. Inspect recovered files first.");
        }
        if (status && apply && bios)
            status = dt_bios_enable_writes(&disk, record.logical.sector, 1, journal, err);
        if (status && apply)
            status = restore ? dt_ebr_record_restore(&disk, journal, torn, err)
                             : dt_ebr_record_apply(&record, journal, err);
        if (!status)
            puts(err);
        else
            puts(!apply    ? "Preview only; no sectors written."
                 : restore ? "EBR record restored and verified."
                           : "EBR record reconstructed and verified.");
        dt_ebr_record_clear(&record);
        if (!close_disk(&disk, bios))
            status = 0;
        return status ? 0 : 2;
    }
    if (op == OP_CONTAINER || op == OP_CONTAINER_RESTORE) {
        DtContainerPlan container;
        int restore = op == OP_CONTAINER_RESTORE;

        if (bios == 2 || index || listing || surface || find_volumes || search_range ||
            explicit_start || boot_source || fat_source || destination ||
            (torn && (!restore || !apply)) || (apply && !journal) || (!apply && journal) ||
            (restore && (!apply || mbr_heads || mbr_sectors)) ||
            (!restore && (!mbr_heads || !mbr_sectors))) {
            puts("Container recovery needs a whole disk, explicit extent/type and CHS geometry.");
            puts("Undo needs --restore-container FILE --apply; writes require a journal.");
            return 2;
        }
        status = bios    ? dt_bios_open(&disk, drive, err)
                 : apply ? dt_image_open_write(&disk, argv[1], err)
                         : dt_image_open(&disk, argv[1], err);
        if (!status) {
            puts(err);
            return 2;
        }
        status = restore ? dt_container_review_journal(&disk, journal, &container, err)
                         : dt_container_plan(&disk,
                                             mbr_slot,
                                             container_base,
                                             container_span,
                                             container_type,
                                             mbr_heads,
                                             mbr_sectors,
                                             &container,
                                             err);
        if (status) {
            printf("Container %s: MBR slot %u, type %02X, %u recovered volumes\n",
                   restore ? "undo" : "preview",
                   container.slot,
                   container.type,
                   container.witness_count);
            printf("Container LBA %lu, sectors %lu; CHS %u heads / %u sectors\n",
                   (unsigned long)container.base,
                   (unsigned long)container.span,
                   container.heads,
                   container.sectors);
            for (i = 0; i < container.witness_count; i++)
                printf("  EBR %lu: volume LBA %lu, sectors %lu, type %02X\n",
                       (unsigned long)container.witness[i].record_sector,
                       (unsigned long)container.witness[i].volume_start,
                       (unsigned long)dt_dword(container.witness[i].record + 458),
                       container.witness[i].record[450]);
            puts("Only the MBR entry/signature changes; inspect every recovered volume first.");
        }
        if (status && apply && bios)
            status = dt_bios_enable_mbr_write(&disk, journal, err);
        if (status && apply)
            status = restore ? dt_container_restore(&disk, journal, torn, err)
                             : dt_container_apply(&container, journal, err);
        if (!status)
            puts(err);
        else
            puts(!apply    ? "Preview only; no sectors written."
                 : restore ? "Container restored and verified."
                           : "Container inserted and verified.");
        dt_container_clear(&container);
        if (!close_disk(&disk, bios))
            status = 0;
        return status ? 0 : 2;
    }
    if (op == OP_LINK || op == OP_LINK_RESTORE) {
        DtEbrLinkPlan link;
        int restore = op == OP_LINK_RESTORE;

        if (bios == 2 || index || listing || surface || find_volumes || search_range ||
            explicit_start || boot_source || fat_source || destination ||
            (torn && (!restore || !apply)) || (apply && !journal) || (!apply && journal) ||
            (restore && (!apply || mbr_heads || mbr_sectors)) ||
            (!restore && (!mbr_heads || !mbr_sectors))) {
            puts("EBR links need a whole disk, explicit source/target LBAs and CHS geometry.");
            puts("Undo needs --restore-link FILE --apply; writes require a journal.");
            return 2;
        }
        status = bios    ? dt_bios_open(&disk, drive, err)
                 : apply ? dt_image_open_write(&disk, argv[1], err)
                         : dt_image_open(&disk, argv[1], err);
        if (!status) {
            puts(err);
            return 2;
        }
        status = restore ? dt_ebr_link_review_journal(&disk, journal, &link, err)
                         : dt_ebr_link_plan(
                               &disk, link_source, link_target, mbr_heads, mbr_sectors, &link, err);
        if (status) {
            printf("EBR link %s: source %lu, target %lu, %u recovered volumes\n",
                   restore ? "undo" : "preview",
                   (unsigned long)link.sector,
                   (unsigned long)link.target,
                   link.recovered);
            printf("Container LBA %lu, sectors %lu; CHS %u heads / %u sectors\n",
                   (unsigned long)link.base,
                   (unsigned long)link.span,
                   link.heads,
                   link.sectors);
            for (i = 0; i < link.witness_count; i++)
                printf("  EBR %lu: volume LBA %lu, sectors %lu, type %02X\n",
                       (unsigned long)link.witness[i].record_sector,
                       (unsigned long)link.witness[i].volume_start,
                       (unsigned long)dt_dword(link.witness[i].record + 458),
                       link.witness[i].record[450]);
            puts("Only the selected link entry changes; inspect every recovered volume first.");
        }
        if (status && apply && bios)
            status = dt_bios_enable_writes(&disk, link.sector, 1, journal, err);
        if (status && apply)
            status = restore ? dt_ebr_link_restore(&disk, journal, torn, err)
                             : dt_ebr_link_apply(&link, journal, err);
        if (!status)
            puts(err);
        else
            puts(!apply    ? "Preview only; no sectors written."
                 : restore ? "EBR link restored and verified."
                           : "EBR link inserted and verified.");
        dt_ebr_link_clear(&link);
        if (!close_disk(&disk, bios))
            status = 0;
        return status ? 0 : 2;
    }
    if (op == OP_MBR || op == OP_MBR_RESTORE || op == OP_EBR || op == OP_EBR_RESTORE) {
        DtMbrPlan plan;
        DtEbrPlan logical;
        int ebr = op == OP_EBR || op == OP_EBR_RESTORE;
        int restore = op == OP_MBR_RESTORE || op == OP_EBR_RESTORE;

        if (bios == 2 || index || listing || surface || find_volumes || search_range ||
            fat_source || destination || (torn && (!restore || !apply)) || (apply && !journal) ||
            (!apply && journal) ||
            (restore && (!apply || explicit_start || boot_source || mbr_heads || mbr_sectors)) ||
            (!restore && (!explicit_start || !mbr_heads || !mbr_sectors))) {
            puts("Partition reconstruction needs a whole disk, an explicit volume start and CHS "
                 "geometry.");
            puts("Undo needs --restore-mbr/--restore-ebr FILE --apply; writes require a journal.");
            return 2;
        }
        status = bios    ? dt_bios_open(&disk, drive, err)
                 : apply ? dt_image_open_write(&disk, argv[1], err)
                         : dt_image_open(&disk, argv[1], err);
        if (!status) {
            puts(err);
            return 2;
        }
        if (!restore) {
            if (ebr) {
                status = dt_ebr_plan(
                    &disk, ebr_sector, start, boot_source, mbr_heads, mbr_sectors, &logical, err);
                if (status)
                    plan = logical.entry;
            } else {
                status = dt_mbr_plan(
                    &disk, mbr_slot, start, boot_source, mbr_heads, mbr_sectors, &plan, err);
            }
            if (status) {
                if (ebr)
                    printf("EBR preview: disk LBA %lu; existing links preserved.\n",
                           (unsigned long)logical.sector);
                printf("%s preview: slot %u, FAT%u, LBA %lu, sectors %lu, type %02X\n",
                       ebr ? "Logical partition" : "MBR",
                       plan.slot,
                       plan.bits,
                       (unsigned long)plan.start,
                       (unsigned long)plan.total,
                       plan.after[446 + (plan.slot - 1) * 16 + 4]);
                printf("CHS: %u heads, %u sectors/track; boot source: %u\n",
                       plan.heads,
                       plan.sectors,
                       plan.boot_source);
                if (ebr)
                    printf("BPB hidden sectors: %lu; EBR-relative start: %lu (unchanged)\n",
                           (unsigned long)dt_dword(plan.volume_boot + 28),
                           (unsigned long)(plan.start - logical.sector));
                else {
                    printf("BPB hidden sectors: %lu%s\n",
                           (unsigned long)dt_dword(plan.volume_boot + 28),
                           dt_dword(plan.volume_boot + 28) == plan.start
                               ? ""
                               : " (MISMATCH; unchanged)");
                }
                puts(
                    "Inserts an inactive partition; bootstrap and occupied entries are preserved.");
                puts("Review candidate contents first. This does not repair boot code or the BPB.");
            }
        } else {
            status = ebr ? dt_ebr_review_journal(&disk, journal, &logical, err) : 1;
            if (status && ebr)
                printf("EBR undo: disk LBA %lu\n", (unsigned long)logical.sector);
        }
        if (status && apply && bios)
            status = ebr ? dt_bios_enable_writes(&disk, logical.sector, 1, journal, err)
                         : dt_bios_enable_mbr_write(&disk, journal, err);
        if (status && apply) {
            if (ebr)
                status = restore ? dt_ebr_restore(&disk, journal, torn, err)
                                 : dt_ebr_apply(&logical, journal, err);
            else
                status = restore ? dt_mbr_restore(&disk, journal, torn, err)
                                 : dt_mbr_apply(&plan, journal, err);
        }
        if (!status)
            puts(err);
        else
            puts(!apply    ? "Preview only; no sectors written."
                 : restore ? (ebr ? "EBR restored and verified." : "MBR restored and verified.")
                           : (ebr ? "EBR reconstructed and verified."
                                  : "MBR reconstructed and verified."));
        if (!close_disk(&disk, bios))
            status = 0;
        return status ? 0 : 2;
    }
    if (mbr_heads || mbr_sectors) {
        puts("CHS geometry options apply only to partition reconstruction or --link-ebr.");
        return 2;
    }
    if (((autofix.salvage || autofix.write_test || lost_option) && op != OP_AUTOFIX) ||
        (torn && (op != OP_RESTORE || !apply)) || (fat_source && op != OP_TREE) ||
        (listing && (surface || index || op != OP_SCAN)) ||
        (surface && op != OP_SCAN && op != OP_AUTOFIX) ||
        (apply && op != OP_FAT_HEADERS && op != OP_MARK && op != OP_UNMARK && op != OP_RESTORE &&
         op != OP_FSINFO && op != OP_BOOT_BACKUP && op != OP_MIRROR && op != OP_RECLAIM &&
         op != OP_CHAINS && op != OP_DIRECTORIES && op != OP_BOOT_PRIMARY && op != OP_TREE &&
         op != OP_ENTRIES && op != OP_LFN && op != OP_ALIASES && op != OP_CONFLICTS &&
         op != OP_LABELS && op != OP_LABEL_TEXT && op != OP_EXTRA_DOTS && op != OP_ATTRIBUTES &&
         op != OP_TIMESTAMPS && op != OP_RELOCATE && op != OP_SALVAGE && op != OP_CROSSLINKS &&
         op != OP_LOST_FILES && op != OP_AUTOFIX) ||
        (boot_source && op != OP_SCAN && op != OP_BOOT_PRIMARY && op != OP_RESTORE &&
         !(explicit_start && op == OP_EXPORT_FILES)) ||
        (journal && !apply) || (apply && !journal) || (op == OP_RESTORE && !apply)) {
        puts("Conflicting options, or missing --apply / --journal for a write operation");
        return 2;
    }
    if ((search_range && !find_volumes) ||
        (find_volumes && (listing || index || explicit_start || surface || op != OP_SCAN || apply ||
                          boot_source)) ||
        (explicit_start && (listing || index || apply)) ||
        (bios == 2 && (find_volumes || explicit_start))) {
        puts("Volume discovery requires a whole disk; explicit-start access is read-only.");
        return 2;
    }
    if (dt_oem_codepage())
        printf("OEM code page: %u (explicit selection)\n", dt_oem_codepage());
    if (bios == 2 && (index || listing)) {
        puts("A DOS drive already identifies one volume; partition options do not apply.");
        return 2;
    }
    if (bios == 1 && apply && !index) {
        puts("BIOS writes require --partition N on an unmounted IDE disk.");
        return 2;
    }
    if (bios == 2)
        status = dt_dos_open(&disk, drive, apply ? journal : NULL, err);
    else if (bios)
        status = dt_bios_open(&disk, drive, err);
    else
        status =
            apply ? dt_image_open_write(&disk, argv[1], err) : dt_image_open(&disk, argv[1], err);
    if (!status) {
        puts(err);
        return 2;
    }
    if (find_volumes) {
        if (search_first >= disk.sectors ||
            (search_count && search_count > disk.sectors - search_first)) {
            puts("Volume-search range is outside the disk");
            close_disk(&disk, bios);
            return 2;
        }
        if (!search_count)
            search_count = disk.sectors - search_first;
        puts("Candidate volumes may overlap; inspect their contents before partition recovery.");
        status = dt_find_volumes(
            &disk, search_first, search_count, found_volume, &found_count, NULL, NULL, err);
        printf("Volume search %s: %lu candidates.\n",
               status ? "complete" : "incomplete",
               (unsigned long)found_count);
        if (!status)
            puts(err);
        close_disk(&disk, bios);
        return status ? 0 : 2;
    }
    if (explicit_start && start >= disk.sectors) {
        puts("Volume start is outside the disk");
        close_disk(&disk, bios);
        return 2;
    }
    size = disk.sectors - start;
    if (index || listing) {
        if (!dt_partitions(&disk, partitions, &count, err)) {
            puts(err);
            close_disk(&disk, bios);
            return 2;
        }
        for (i = 0; i < count; i++)
            printf("%3u  type %02X  LBA %10lu  sectors %10lu\n",
                   i + 1,
                   partitions[i].type,
                   (unsigned long)partitions[i].start,
                   (unsigned long)partitions[i].size);
        if (listing) {
            close_disk(&disk, bios);
            return 0;
        }
        if (index > count) {
            puts("Partition does not exist");
            close_disk(&disk, bios);
            return 2;
        }
        start = partitions[index - 1].start;
        size = partitions[index - 1].size;
    }
    if (op == OP_BOOT_PRIMARY && !boot_source)
        boot_source = 6;
    status = boot_source ? dt_mount_backup(&disk, start, size, boot_source, &volume, err)
                         : dt_mount(&disk, start, size, &volume, err);
    if (!status) {
        puts(err);
        close_disk(&disk, bios);
        return 2;
    }
    if (bios == 1 && apply &&
        !dt_bios_enable_writes(&disk, volume.start, volume.total, journal, err)) {
        puts(err);
        close_disk(&disk, bios);
        return 2;
    }
    if (bios == 1 && apply)
        puts("Unmounted IDE write access: run only from a clean DOS boot without disk TSRs.");
    if (boot_source)
        printf("Recovery geometry: FAT32 backup sector %u.\n", boot_source);
    if (bios == 2)
        puts("Mounted DOS volume: sector numbers are relative to this volume, starting at zero.");
    printf("FAT%u | %lu sectors | %lu clusters | %u sectors/cluster\n",
           volume.bits,
           (unsigned long)volume.total,
           (unsigned long)volume.clusters,
           volume.spc);
    if (sector_arg) {
        dt_u32 first_data = volume.start + volume.data;
        dt_u32 data_sectors = volume.clusters * volume.spc;
        if (cluster < first_data || cluster - first_data >= data_sectors) {
            puts("Sector is outside this volume's data clusters; metadata cannot be marked in FAT");
            close_disk(&disk, bios);
            return 2;
        }
        cluster = (cluster - first_data) / volume.spc + 2;
    }
    if (op == OP_RECOVER || op == OP_EXPORT_FILES || op == OP_DELETED || op == OP_ORPHAN_DIR) {
        DtRecovery recovered;

        status =
            op == OP_ORPHAN_DIR
                ? dt_recover_directories(&volume, destination, &recovered, NULL, NULL, err)
            : op == OP_DELETED
                ? dt_recover_deleted(&volume, destination, &recovered, NULL, NULL, err)
            : op == OP_EXPORT_FILES
                ? (cluster ? dt_export_cluster_file(
                                 &volume, cluster, destination, &recovered, NULL, NULL, err)
                           : dt_export_files(&volume, destination, &recovered, NULL, NULL, err))
                : dt_recover_lost(&volume, destination, &recovered, NULL, NULL, err);
        if (!status)
            puts(err);
        if (op == OP_DELETED || op == OP_ORPHAN_DIR)
            printf("%s export %s: %lu candidates, %lu files, %lu clusters.\n",
                   op == OP_ORPHAN_DIR ? "Orphan-directory" : "Deleted-candidate",
                   recovered.complete ? "finished" : "INCOMPLETE",
                   (unsigned long)recovered.candidates,
                   (unsigned long)recovered.files,
                   (unsigned long)recovered.clusters);
        else
            printf("%s export %s: %lu files, %lu/%lu clusters, %lu unreadable sectors.\n",
                   op == OP_DELETED        ? "Deleted-candidate"
                   : op == OP_EXPORT_FILES ? "Live-file"
                                           : "Lost-chain",
                   recovered.complete ? "finished" : "INCOMPLETE",
                   (unsigned long)recovered.files,
                   (unsigned long)recovered.clusters,
                   (unsigned long)recovered.lost,
                   (unsigned long)recovered.unreadable);
        if (*recovered.directory)
            printf("Recovery directory: %s\nSee RECOVERY.TXT for cluster locations and gaps.\n",
                   recovered.directory);
        if (recovered.catalog_incomplete)
            puts("Directory catalog INCOMPLETE: unreadable entries may hide files or owners.\n"
                 "Unreadable-sector counts above describe file payload only.");
        if (recovered.damaged)
            printf("%lu damaged files/directories; inspect the manifest.\n",
                   (unsigned long)recovered.damaged);
        if (op == OP_DELETED || op == OP_ORPHAN_DIR)
            puts("All candidates are unverified; contiguous guesses may not match deleted "
                 "contents.");
        if (recovered.issues && op != OP_DELETED && op != OP_ORPHAN_DIR)
            printf("%lu scan findings recorded in the manifest.\n",
                   (unsigned long)recovered.issues);
        status = !status ? 2
                 : (recovered.unreadable || recovered.damaged || recovered.issues ||
                    recovered.candidates)
                     ? 1
                     : 0;
        if (!close_disk(&disk, bios))
            status = 2;
        return status;
    }
    if (op == OP_AUTOFIX) {
        autofix.apply = apply;
        autofix.surface = surface;
        autofix.stamp = dt_dos_now();
        autofix.journal_dir = journal;
        status = autofix_command(&volume, &autofix);
        if (!close_disk(&disk, bios))
            status = 2;
        return status;
    }
    if (op != OP_SCAN) {
        status = repair_command(&volume, op, cluster, fat_source, apply, journal, torn);
        if (!close_disk(&disk, bios))
            status = 2;
        return status;
    }
    if (surface) {
        status = dt_surface_ex(&volume, 0, volume.total, &map, NULL, NULL, report, NULL);
        for (i = 0; i < map.cells; i++)
            failures += map.cell[i].unreadable;
        printf("Surface read %s: %lu sectors, %lu unreadable\n",
               status ? "complete" : "INCOMPLETE",
               (unsigned long)volume.total,
               (unsigned long)failures);
        close_disk(&disk, bios);
        return !status ? 2 : (failures ? 1 : 0);
    }
    dt_scan(&volume, &result, report, NULL);
    printf("%s: %lu files, %lu directories, %lu free, %lu bad, %lu lost clusters, %lu issues\n",
           result.complete ? "Inspection complete" : "Inspection INCOMPLETE",
           (unsigned long)result.files,
           (unsigned long)result.dirs,
           (unsigned long)result.free_clusters,
           (unsigned long)result.bad_clusters,
           (unsigned long)result.lost_clusters,
           (unsigned long)result.errors);
    close_disk(&disk, bios);
    return !result.complete ? 2 : (result.errors ? 1 : 0);
}
