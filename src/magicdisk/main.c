/* MagicDisk - command-line diagnostics and DOS workspace entry point. */
#include "disk.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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
         "Options: --list | --partition N | --surface | --retest-bad\n"
         "         --mark-bad CLUSTER | --unmark-bad CLUSTER\n"
         "         --mark-sector DISK_LBA | --unmark-sector DISK_LBA\n"
         "         --repair-fsinfo                Rebuild FAT32 free-space metadata\n"
         "         --repair-boot-backup           Copy primary FAT32 boot sector to backup\n"
         "         --repair-boot-primary          Restore primary FAT32 boot from backup\n"
         "         --boot-source SECTOR           Inspect via backup (default recovery: 6)\n"
         "         --repair-fat SOURCE           Rebuild FAT copies from source 1, 2, ...\n"
         "         --fat-source N                Explicit authority for --repair-tree\n"
         "         --repair-tree                 Repair file/directory chains and dot entries\n"
         "         --repair-directory-entries    Correct dot entries/directory sizes (preview)\n"
         "         --repair-directories          Repair directory loops/endings (preview)\n"
         "         --repair-files                Repair file chains and lengths (preview)\n"
         "         --reclaim-lost                Free unowned allocation (recover data first)\n"
         "         --export-files DIRECTORY      Export live files before repairs\n"
         "         --recover-lost DIRECTORY      Export lost chains to a separate location\n"
         "         --apply --journal NEWFILE     Apply a previewed repair\n"
         "         --restore-journal FILE --apply  Restore journal sectors\n"
         "         --accept-torn                 Accept only saved before/after byte mixtures\n"
         "Repair writes support images and MS-DOS 6.22/7.1/FreeDOS --drive volumes.\n"
         "BIOS device access is read-only.\n"
         "BIOS device: 00=A, 01=B, 80=first hard disk. Partition N starts at 1.\n"
         "--relocate-cluster N copies readable file data and marks the source bad.\n"
         "All inspection and surface operations are read-only.");
}

enum {
    OP_SCAN,
    OP_RETEST,
    OP_MARK,
    OP_UNMARK,
    OP_RESTORE,
    OP_FSINFO,
    OP_BOOT_BACKUP,
    OP_MIRROR,
    OP_RECOVER,
    OP_EXPORT_FILES,
    OP_RECLAIM,
    OP_CHAINS,
    OP_DIRECTORIES,
    OP_BOOT_PRIMARY,
    OP_TREE,
    OP_ENTRIES,
    OP_RELOCATE
};

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
               "%lu sectors read. Markers unchanged.\n",
               retest.complete ? "complete" : "INCOMPLETE",
               (unsigned long)retest.marked,
               (unsigned long)retest.readable,
               (unsigned long)retest.failed,
               (unsigned long)retest.tested_sectors);
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
    if (op == OP_RELOCATE)
        ok = dt_relocate_plan(v, cluster, plan, NULL, NULL, err);
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
    else if (op == OP_ENTRIES)
        ok = dt_entry_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_DIRECTORIES)
        ok = dt_directory_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_CHAINS)
        ok = dt_chain_plan_ex(v, plan, report, NULL, NULL, NULL, err);
    else if (op == OP_RECLAIM)
        ok = dt_lost_plan(v, plan, NULL, NULL, err);
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
        if (op == OP_RELOCATE) {
            printf("Relocate file cluster %lu and mark its original allocation bad; %u journaled "
                   "sectors.\n",
                   (unsigned long)cluster,
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

int main(int argc, char **argv)
{
    DtDisk disk;
    DtVolume volume;
    DtPartition partitions[128];
    DtResult result;
    DtMap map;
    unsigned count = 0, i, index = 0, drive = 0, boot_source = 0, fat_source = 0;
    char err[160], *end;
    int listing = 0, bios = 0, surface = 0, arg = 2, status;
    int op = OP_SCAN, apply = 0, sector_arg = 0, torn = 0;
    dt_u32 cluster = 0;
    const char *journal = NULL, *destination = NULL;
    dt_u32 start = 0, size, failures = 0;

    if (argc == 1)
        return dt_ui(NULL);
    if (!strcmp(argv[1], "--tui") && argc <= 3)
        return dt_ui(argc == 3 ? argv[2] : NULL);
    if (!strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    puts("MagicDisk 0.3 - DOS disk diagnostics");
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
        unsigned long value;
        if (argc < 3) {
            usage();
            return 2;
        }
        value = strtoul(argv[2], &end, 16);
        if (!*argv[2] || *end || value > 255) {
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
        if (!strcmp(argv[arg], "--list")) {
            listing = 1;
        } else if (!strcmp(argv[arg], "--surface")) {
            surface = 1;
        } else if (!strcmp(argv[arg], "--accept-torn")) {
            torn = 1;
        } else if (!strcmp(argv[arg], "--apply")) {
            apply = 1;
        } else if (!strcmp(argv[arg], "--journal") && arg + 1 < argc) {
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--export-files") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_EXPORT_FILES;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--recover-lost") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_RECOVER;
            destination = argv[++arg];
        } else if (!strcmp(argv[arg], "--retest-bad") && op == OP_SCAN) {
            op = OP_RETEST;
        } else if (!strcmp(argv[arg], "--repair-fsinfo") && op == OP_SCAN) {
            op = OP_FSINFO;
        } else if (!strcmp(argv[arg], "--repair-boot-backup") && op == OP_SCAN) {
            op = OP_BOOT_BACKUP;
        } else if (!strcmp(argv[arg], "--repair-boot-primary") && op == OP_SCAN) {
            op = OP_BOOT_PRIMARY;
        } else if (!strcmp(argv[arg], "--boot-source") && arg + 1 < argc) {
            unsigned long n;
            errno = 0;
            n = strtoul(argv[++arg], &end, 10);
            if (!*argv[arg] || *end || errno || n < 1 || n > 65534) {
                puts("Backup sector must be numbered from 1 through 65534");
                return 2;
            }
            boot_source = (unsigned)n;
        } else if (!strcmp(argv[arg], "--repair-tree") && op == OP_SCAN) {
            op = OP_TREE;
        } else if (!strcmp(argv[arg], "--repair-directory-entries") && op == OP_SCAN) {
            op = OP_ENTRIES;
        } else if (!strcmp(argv[arg], "--repair-directories") && op == OP_SCAN) {
            op = OP_DIRECTORIES;
        } else if (!strcmp(argv[arg], "--repair-files") && op == OP_SCAN) {
            op = OP_CHAINS;
        } else if (!strcmp(argv[arg], "--reclaim-lost") && op == OP_SCAN) {
            op = OP_RECLAIM;
        } else if (!strcmp(argv[arg], "--fat-source") && arg + 1 < argc) {
            unsigned long n = strtoul(argv[++arg], &end, 10);
            if (!*argv[arg] || *end || n < 1 || n > 4) {
                puts("Source FAT must be numbered from 1 through 4");
                return 2;
            }
            fat_source = (unsigned)n;
        } else if (!strcmp(argv[arg], "--repair-fat") && op == OP_SCAN && arg + 1 < argc) {
            unsigned long n = strtoul(argv[++arg], &end, 10);
            if (!*argv[arg] || *end || n < 1 || n > 4) {
                puts("Source FAT must be numbered from 1 through 4");
                return 2;
            }
            op = OP_MIRROR;
            cluster = (dt_u32)n;
        } else if ((!strcmp(argv[arg], "--relocate-cluster") || !strcmp(argv[arg], "--mark-bad") ||
                    !strcmp(argv[arg], "--unmark-bad") || !strcmp(argv[arg], "--mark-sector") ||
                    !strcmp(argv[arg], "--unmark-sector")) &&
                   op == OP_SCAN && arg + 1 < argc) {
            unsigned long n;
            sector_arg = strstr(argv[arg], "-sector") != NULL;
            op = !strcmp(argv[arg], "--relocate-cluster") ? OP_RELOCATE
                 : (!strcmp(argv[arg], "--mark-bad") || !strcmp(argv[arg], "--mark-sector"))
                     ? OP_MARK
                     : OP_UNMARK;
            errno = 0;
            n = strtoul(argv[++arg], &end, 10);
            if (!*argv[arg] || *end || errno || n > 0xfffffffeUL || (!sector_arg && n < 2)) {
                puts("Invalid cluster or disk sector number");
                return 2;
            }
            cluster = (dt_u32)n;
        } else if (!strcmp(argv[arg], "--restore-journal") && op == OP_SCAN && arg + 1 < argc) {
            op = OP_RESTORE;
            journal = argv[++arg];
        } else if (!strcmp(argv[arg], "--partition") && arg + 1 < argc) {
            unsigned long n = strtoul(argv[++arg], &end, 10);
            if (!*argv[arg] || *end || n < 1 || n > 128) {
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
    if ((torn && (op != OP_RESTORE || !apply)) || (fat_source && op != OP_TREE) ||
        (listing && (surface || index || op != OP_SCAN)) || (surface && op != OP_SCAN) ||
        (apply && op != OP_MARK && op != OP_UNMARK && op != OP_RESTORE && op != OP_FSINFO &&
         op != OP_BOOT_BACKUP && op != OP_MIRROR && op != OP_RECLAIM && op != OP_CHAINS &&
         op != OP_DIRECTORIES && op != OP_BOOT_PRIMARY && op != OP_TREE && op != OP_ENTRIES &&
         op != OP_RELOCATE) ||
        (boot_source && op != OP_SCAN && op != OP_BOOT_PRIMARY && op != OP_RESTORE) ||
        (journal && !apply) || (apply && !journal) || (op == OP_RESTORE && !apply)) {
        puts("Conflicting options, or missing --apply / --journal for a write operation");
        return 2;
    }
    if (bios == 2 && (index || listing)) {
        puts("A DOS drive already identifies one volume; partition options do not apply.");
        return 2;
    }
    if (bios == 1 && apply) {
        puts("BIOS writes are not enabled yet. Retesting and previews are available.");
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
    size = disk.sectors;
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
    if (op == OP_RECOVER || op == OP_EXPORT_FILES) {
        DtRecovery recovered;

        status = op == OP_EXPORT_FILES
                     ? dt_export_files(&volume, destination, &recovered, NULL, NULL, err)
                     : dt_recover_lost(&volume, destination, &recovered, NULL, NULL, err);
        if (!status)
            puts(err);
        printf("%s export %s: %lu files, %lu/%lu clusters, %lu unreadable sectors.\n",
               op == OP_EXPORT_FILES ? "Live-file" : "Lost-chain",
               recovered.complete ? "finished" : "INCOMPLETE",
               (unsigned long)recovered.files,
               (unsigned long)recovered.clusters,
               (unsigned long)recovered.lost,
               (unsigned long)recovered.unreadable);
        if (*recovered.directory)
            printf("Recovery directory: %s\nSee RECOVERY.TXT for cluster locations and gaps.\n",
                   recovered.directory);
        if (recovered.damaged)
            printf("%lu damaged files/directories; inspect the manifest.\n",
                   (unsigned long)recovered.damaged);
        if (recovered.issues)
            printf("%lu scan findings recorded in the manifest.\n",
                   (unsigned long)recovered.issues);
        status = !status                                                           ? 2
                 : (recovered.unreadable || recovered.damaged || recovered.issues) ? 1
                                                                                   : 0;
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
