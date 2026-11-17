MAGICDISK 0.3 - DOS DEVELOPMENT BUILD
===================================
Developer: Felipe Andrade Allemand Borges
Part of the Retrolities project.

MagicDisk inspects and repairs FAT12, FAT16 and FAT32 volumes. It covers the
core of ScanDisk and Norton Disk Doctor - one-step automatic repair, cross-link
copies, lost chains saved as FILEnnnn.CHK, surface and write tests with bad
cluster retirement - and adds verified journals that undo every repair.
Compressed (DoubleSpace/DriveSpace/Stacker) volumes are not supported.
Keep the recovery journals and export important files before repairing them.

GETTING STARTED
---------------
Requires a 386 or newer CPU and an 80-column color text display. DOS/32A is
embedded in MAGICDSK.EXE; a separate extender executable is not required.
Tested in QEMU with MS-DOS 6.22, FreeDOS and Windows 98 SE DOS mode.
Windows desktop DOS windows and every physical controller are not validated.

Extract every file in this archive into a directory accessible from DOS.
Use DOS-compatible paths, normally 8.3 names or aliases. Run:

  MAGICDSK

Press F10, choose Disk, then select a DOS volume, BIOS device or image.
Select a DOS drive letter for a mounted volume or floppy that needs repair.
Select a BIOS device to inspect the physical disk and its MBR partitions.
BIOS IDs are hexadecimal: 00 = first floppy, 80 = first hard disk.
F2 selects a primary or logical partition. GPT is not supported.

KEYBOARD AND MAP
----------------
  F10  Menus: Disk, Diagnose, View, Reports, Help, Fix
       Arrows navigate; Home/End select first/last; Enter opens; Esc closes.
       Page Up/Down move through long menus; both status bars stay visible.
  F2   Select a volume           F3  Check filesystem
  F4   Read every volume sector  F5  Allocation map
  F6   Findings                  F7  Save report
  F8   Read-only hex/ASCII sector viewer
  F9   Automatic repair (Fix menu)
  Arrows  Select map blocks      +   Zoom in     -   Whole volume
  Enter   Block details; Enter again opens its first sector
  O       Cluster owner (map, View menu, or sector viewer)
  Esc     Cancel inspection, close a dialog, or exit

Allocation colors describe FAT entries, not file health. Red errors can mean
unreadable FAT entries or invalid FAT values. A bad marker does not prove a
sector is still unreadable. Surface reads and retests never change markers.
Pending blocks were not inspected. Incomplete results are not a clean bill
of health. F6 findings retain locations; F7 reports preserve full UTF-8 names.
The summary row retains surface-read totals when returning to the allocation
map, marks incomplete checks in amber and distinguishes previews from repairs.
After repair, run a new check; previous results are cleared.
Owner lookup shows the first catalog owner, path and directory-entry location.
On the map it uses the block's first data cluster; in F8 it uses that sector.
Scan findings or incomplete reads make ownership uncertain. Left/Right scroll
long paths; Esc returns without changing findings or disk contents.
For regular-file owners, R opens export review for that exact cluster. Choose
a separate destination; directory owners and unknown owners have no R action.
The findings list grows with available memory. If memory runs out, earlier
findings remain available and the omitted count appears in the saved report.
Report saving commits the file and refuses to overwrite an existing file.

AUTOMATIC REPAIR (F9)
---------------------
Fix > Automatic repair plans every applicable repair in a safe order: FAT
headers, FAT copies (only when one copy is unambiguous), cross-link copies,
file/directory trees, directory entries, names, labels, long names, dates,
lost chains, FSInfo and the backup boot sector. Review the pending steps,
type FIX, then give an EXISTING directory on another disk for journals.
Each step is journaled, written, verified and followed by a fresh plan.
Journals are named MDFX0001.MDJ, MDFX0002.MDJ, ...; restore them newest
first with Diagnose > Restore journal to undo the whole sequence.
Automatic + surface also reads every sector. Unreadable free clusters are
marked bad and readable copies of damaged file clusters are relocated.

Fix > CMOS check reads the setup CMOS and reports a dead battery, POST error
flags, a bad checksum, an invalid clock and floppy or memory mismatches. It
never writes CMOS; correct problems in the BIOS setup program.

Fix > Copy cross-linked files gives each later owner of a shared chain its
own copy of the shared clusters (type COPY). The first owner is unchanged.
Fix > Convert lost chains creates root FILEnnnn.CHK files for every lost
chain (type ADOPT). Unused root slots are used before deleted entries so
undelete candidates survive; a full FAT32 root gains one cluster.

INSPECT, EXPORT, THEN REPAIR
---------------------------
Cluster relocation skips unreadable free destinations and checks every sector
of a candidate before selecting it. Skipped clusters remain unchanged.
Source-data copying tries each sector up to three times. Esc cancels between
retries; persistent read failures stop relocation without substituting bytes.

1. Open the intended source and check the displayed drive/partition and size.
2. Use F3 and F4. Review the findings with F6; save the report with F7.
3. Export valuable data to an existing directory on a different disk.
4. Choose the relevant Diagnose repair. Review its proposed changes.
5. Enter its confirmation word and a NEW journal filename on a separate disk.
6. Keep the journal. Restore journal can undo that operation if the current
   metadata still matches the transaction. Later changes can prevent undo.

Writes show Wait and cannot be cancelled midway. MagicDisk saves original
and proposed sectors, verifies writes, and attempts rollback after a failure.
If rollback or DOS flush/unlock fails, preserve the journal and stop using
the volume until recovery is reviewed. Do not delete the journal to retry.

Mark bad sector accepts a sector LBA within the selected data area. A free,
unowned cluster can be marked. Readable live data may be relocated first.
Unmark requires an unowned bad cluster whose sectors all pass a read retest.
If live-file data cannot be relocated, Mark bad sector offers SALVAGE:
readable sectors move, but each persistently unreadable sector becomes 512
ZERO bytes in the replacement. Missing bytes are NOT recovered. Export needed
data first. The review shows the filename and loss count and requires typing
SALVAGE. Cancel to inspect F6 findings or save the loss preview with F7.
CLI: --salvage-cluster N previews; --apply --journal NEW.MDJ commits.
Directories are excluded. Original source sectors are never written.
Undo restores the old allocation, not the missing bytes; it requires unchanged
source data and readability. Version-16 journals record each zero-filled gap.
Retest reports attempted sectors and failed reads separately. Cancellation
retains observed failures and all markers, and reports incomplete results.

The Diagnose menu also provides FAT-copy, FAT32 FSInfo and boot-sector repairs,
file/directory chain repair, directory-entry correction, invalid timestamp
clearing, and lost-allocation reclaim. Preview is not permission to discard
unknown data: ownership conflicts and unproven repairs are refused.

Reports > Export live files saves healthy files and recoverable prefixes.
It can skip an unreadable directory sector after three attempts and export
other visible files. The manifest lists DIRECTORY_GAP locations and marks the
export INCOMPLETE; skipped entries may hide additional files or owners.
LFN groups are never joined across gaps. Repair scans remain strict.
The results screen labels an incomplete directory catalog explicitly.
Zero unreadable payload sectors does not mean that every file was found.
Reports > Recover deleted files exports UNVERIFIED candidates; cleared FAT
links can require a contiguous guess. Diagnose > Recover lost chains exports
unowned chains with unknown original names and lengths. Exports use numbered
.CHK files and a RECOVERY.TXT manifest. Read gaps are recorded, not hidden.
A deleted directory with a retained unowned FAT chain can be exported when
one dot slot is entirely erased and the other dot entry matches. Raw dot
metadata is recorded as UNVERIFIED evidence. Conflicting dot pointers, two
missing dot slots, or missing dots with cleared FAT links stop traversal.
Deleted-directory metadata reads retry up to three times. Cancellation is
available between attempts. DIRECTORY_RETRY records the outcome; persistent
metadata errors stop traversal. Metadata is never filled with guessed bytes.
Recovery exports do not undelete entries or alter source allocation.

SELECTED BLOCK RETEST
--------------------
Press T or choose View > Retest map block to read the selected block's sectors.
This works in allocation and surface maps, without changing bad markers.
RANGE COMPLETE applies only to that range. F7 saves the exact tested sector
range and disk LBAs; F5 keeps the summary visible beside the allocation map.
F4 still reads the whole volume. New tests replace the previous findings.

MAP SIZE LABELS
----------------
Sizes use rounded B, KiB, MiB, GiB or TiB labels. Exact disk LBA ranges are
shown separately. A single-sector block displays 512 B. Map shortcuts have
their own line so long sector numbers do not hide the controls.

ACTIONS FROM FINDINGS
----------------------
After a surface read or bad-cluster retest, F6 opens the findings. Select a row
and press M to review marking its cluster bad, or U to retest and review
unmarking a bad cluster. The footer shows which action is available. No sector
number needs to be copied. The usual review, confirmation and journal are
required. Unreadable clusters cannot be unmarked. Metadata is inspect-only.
R finds and exports the regular file owning the selected sector. Choose a
separate recovery destination; missing owners and directory clusters are
refused. Unreadable bytes become recorded zero-filled gaps in RECOVERY.TXT.
CLI: MAGICDSK FLOPPY.IMG --export-cluster 1000 C:\RECOVER
F8 opens the selected sector in the read-only viewer.

BIOS WRITES AND DESTINATIONS
----------------------------
BIOS repair requires an unmounted IDE partition, a clean DOS boot without
disk caches or disk TSRs, and a journal on a different known local IDE disk.
The entire source physical disk must have no mounted DOS partitions.
Only verified legacy IDE port mappings with ATA flush support are accepted.
Unsupported controllers, unknown local devices and mounted sources are refused.
For mounted media, select Disk > DOS volume instead.

BIOS reports and recovery exports require a different physical disk. Another
partition on the same disk is not a separate destination. Raw BIOS floppy
writes are not enabled; use the mounted DOS floppy drive for repairs.

RESERVED FAT HEADERS
---------------------
Diagnose > Rebuild FAT headers repairs reserved entries when every FAT copy
has header damage. It retains clean-shutdown and I/O-error flags and checks
allocation and directory ownership before offering changes. Remaining mirror
disagreements are refused. Only the active FAT changes if mirroring is off.
The boot sector must contain a recognized media descriptor. A new journal
(version 15) saves original bytes for undo or interrupted-write recovery.
CLI: MAGICDSK FLOPPY.IMG --repair-fat-headers
Add --apply --journal HEADERS.MDJ after reviewing the preview.

COMMAND LINE EXAMPLES
----------------------
  MAGICDSK --drive D --autofix                 Preview every repair
  MAGICDSK --drive D --autofix --apply --journal A:\
  MAGICDSK --drive D --autofix --surface --write-test --apply --journal A:\
      --write-test inverts and restores free-cluster sectors only.
      --salvage also zero-fills unreadable sectors of live files.
      --lost-chains files|free|keep chooses .CHK files (default), freeing
      the clusters, or leaving lost chains untouched.
  MAGICDSK --cmos                              Check CMOS setup (read-only)
  MAGICDSK FLOPPY.IMG --repair-crosslinks --apply --journal A:\X.MDJ
  MAGICDSK FLOPPY.IMG --convert-lost --apply --journal A:\L.MDJ
  MAGICDSK --help
  MAGICDSK --devices
  MAGICDSK --dos-map
  MAGICDSK --drive A
  MAGICDSK --drive D --surface
  MAGICDSK --drive D --retest-bad
  MAGICDSK --bios 80 --list
  MAGICDSK --bios 80 --partition 1
  MAGICDSK FLOPPY.IMG
  MAGICDSK --tui FLOPPY.IMG
  MAGICDSK HDD.IMG --partition 1
  MAGICDSK FLOPPY.IMG --mark-bad 1000
  MAGICDSK FLOPPY.IMG --mark-bad 1000 --apply --journal MARK.MDJ
  MAGICDSK FLOPPY.IMG --restore-journal MARK.MDJ --apply

Without --apply, repair commands preview changes. --help lists the repair,
export and recovery switches. --accept-torn is only for reviewed interrupted
journal writes whose current bytes match saved before/after bytes. It does
not override unrelated changes or reconstruct unreadable data.

Image/BIOS sector LBAs are absolute within that source; --drive LBAs start
at zero within the DOS volume. Cluster numbers are not sector numbers.
Enter unsigned decimal numbers without signs or spaces. BIOS drive numbers
use hex; the sector viewer also accepts 0x-prefixed hex. Overflow and trailing
characters are rejected before selecting a disk location.
Exit codes: 0 = completed without detected issues, 1 = completed with issues,
2 = refused, invalid input, unsupported geometry, or incomplete inspection.

LIMITS AND LONG NAMES
---------------------
512-byte sectors and MBR partitions only. DOS images can be up to 4 GiB minus
512 bytes when the host filesystem supports the file. Windows 98 DOS and
FreeDOS can host large images on FAT32; MS-DOS 6.22 host files remain subject
to that kernel's filesystem limits. Image access never extends the file.
FAT32 can also be inspected via BIOS on MS-DOS 6.22 even though that kernel
cannot mount it as a DOS drive.

VFAT long names are decoded from disk without requiring DOS LFN services.
Reports retain UTF-8; the VGA screen uses CP437 or '?' for unavailable glyphs.
DIR_NAME identifies invalid short-alias bytes even beneath a valid long name.
Findings include the byte value and location; inspection preserves the name.
OEM/DBCS punctuation is left undecided when the original code page is unknown.
DIR_LABEL identifies extra root volume labels and points to the first label.
Conflicting labels are preserved and block ownership-sensitive repairs.
Unicode 3.2 simple-uppercase comparisons detect long-name case collisions.
Unknown OEM code pages are not guessed. Host paths still need to be accessible
to the running DOS kernel, normally through 8.3 aliases.

Unreadable data cannot be reconstructed from nothing. Missing directory data,
ambiguous ownership and cleared fragmented deleted chains may remain
unresolved.
Real hardware power-loss durability and broader controller support remain
unvalidated. See src/magicdisk/README.md in the source tree for full details.

CREDITS AND FILES
-----------------
Developer: Felipe Andrade Allemand Borges
MagicDisk: MIT License, reproduced in LICENSE.TXT.
This product uses DOS/32 Advanced DOS Extender technology.
DOS/32A notice: DOS32A.TXT. Open Watcom notice: WATCOM.TXT.
Unicode character data notice: UNICODE.TXT.
SHA256.TXT lists the archive payload checksums (excluding itself).
No DOS operating system, boot image or proprietary driver is included.

OEM SHORT NAMES
---------------
FAT does not identify the OEM code page that created short names. MagicDisk
escapes unknown bytes by default. Disk > OEM code page accepts 0 (unknown),
437, 850, 852 (Central European), 860 (Portuguese), or 866 (Cyrillic).
Select a page only when the source encoding is known; the policy
applies to all sources in this session. Changing it clears prior findings.
CLI examples:
  MAGICDSK FLOPPY.IMG --code-page 850
  MAGICDSK --code-page 437 --tui
Reports and all recovery/export manifests record the policy. Filename and
extension lowercase flags also apply to decoded accented short-name letters.
Decoded aliases participate
in Unicode name collision checks. Source name bytes are never rewritten.
These choices do not cover DBCS encodings or every DOS locale's casing rules.

REPAIR FROM FINDINGS
-------------------
When the findings footer offers P, press it to review the existing repair
workflow for that issue type. The proposed changes may cover multiple entries
on the volume. Normal review, typed confirmation and a separate journal are
still required before writes. FAT-copy disagreements ask which copy to trust.
Esc cancels. Unsupported findings and preview lists do not offer this action.

ORPHAN DIRECTORY DISCOVERY
  MAGICDSK IMAGE.IMG --recover-directories DESTINATION
  Report > Find orphan directories searches allocated, unowned clusters for
  intact dot pairs and exports surviving contents as UNVERIFIED candidates.
  It requires readable, unambiguous live ownership and never changes the
  source or reattaches directories. RECOVERY.TXT preserves raw evidence,
  names and gaps. Unreadable search sectors leave the export incomplete.

INVALID LONG-NAME CLEANUP
  MAGICDSK IMAGE.IMG --clear-invalid-lfn
  Add --apply --journal NEW.MDJ after reviewing the preview.
  Diagnose > Clear invalid long names requires typing CLEAR. This deletes
  invalid/orphaned LFN records; it does not reconstruct the lost name.
  DOS aliases, valid long names, contents and allocation remain unchanged.
  Readable, unambiguous directory ownership is required. Version-17 journals
  preserve original directory sectors for validated undo.

INVALID DOS ALIAS REPAIR
  MAGICDSK IMAGE.IMG --repair-short-names
  Add --apply --journal NEW.MDJ after reviewing the replacement names.
  Diagnose > Repair invalid DOS names requires typing RENAME.
  Invalid aliases become unique R0000001.CHK-style names. Valid LFN text,
  contents, dates and allocation stay intact; LFN checksums follow the alias.
  Version-18 journals retain the OEM policy for validated undo in later
  sessions. Candidate checks read their directory namespace. Full ownership
  and proposed-volume scans still run before approval. Preparation can be
  cancelled. Unknown DBCS punctuation is not guessed.

VOLUME-LABEL REPAIR
  MAGICDSK IMAGE.IMG --repair-labels
  Add --apply --journal NEW.MDJ after reviewing the preview.
  Diagnose > Repair volume labels requires typing LABELS.
  Keeps the first root label; removes extra and subdirectory labels.
  Labels with a cluster pointer or file size cause the repair to be refused.
  Boot labels, names, contents, dates and allocation stay intact.
  Version-19 journals preserve original directory sectors for validated undo.

DUPLICATE FILENAME REPAIR
  MAGICDSK IMAGE.IMG --repair-name-conflicts
  Preview first; add --apply --journal NEW.MDJ to write.
  Diagnose > Repair name conflicts requires typing NAMES.
  Keep the first name; later conflicts receive unique R0000001.CHK names.
  Conflicting LFNs are replaced. Other LFN text, contents and allocation stay.
  F6/F7 show or export the planned names and original replaced LFN slots.
  Version-20 journals preserve original sectors and the source OEM policy.

RESERVED ATTRIBUTE REPAIR
  MAGICDSK IMAGE.IMG --clear-extra-dots

  Diagnose > Remove extra dot entries requires typing DOTS.
  Removes root dot entries and redundant dots beyond a valid leading pair.
  Requires matching pointers, directory attributes and zero size; ambiguous
  metadata or unrelated structural damage is refused. Only deletion bytes
  change. File data, allocation, timestamps and LFN records stay intact.
  Preview first; use --apply --journal NEW.MDJ on a separate disk to apply.
  Version-22 journals preserve original sectors for exact or partial undo.

  MAGICDSK IMAGE.IMG --repair-label-text

  DIR_LABEL_NAME reports control bytes, DOS punctuation and a leading space.
  Preview replaces invalid bytes with underscores; interior spaces and valid
  OEM bytes are preserved. Unknown high-byte text needs an explicit supported
  code page before punctuation is changed. Diagnose > Repair volume label text
  requires typing TEXT and a new journal on a separate disk. Findings P opens
  the same review. Raw before/after label bytes appear in the preview.
  Root-label changes synchronize extended boot labels (signature 29h) and an
  exactly matching FAT32 backup. Repair a damaged backup first. Version-23
  journals preserve OEM policy and all original sectors for exact/partial undo.

  If all three defects occur together, clear reserved attributes, repair
  volume labels, then repair name conflicts. Label cleanup preserves and
  reports existing filename collisions. Undo the three journals in reverse.

  Duplicate names and extra labels can coexist with reserved attribute bits.
  Clear attributes first, then repair names or volume labels as reported by
  FOLLOWUP_CONFLICTS or FOLLOWUP_LABELS. Undo the follow-up journal first.
  Labels with cluster pointers, nonzero sizes or directory bits are refused,
  even if their reserved bits already are clear.

  Extra dot records with reserved attribute bits can be repaired in stages:
  clear attributes first, then remove extra dots. FOLLOWUP_DOTS lists the
  remaining dot records. Ambiguous pointers, sizes and missing leading pairs
  are refused. Undo the dot journal before the attribute journal.

  MAGICDSK IMAGE.IMG --repair-attributes
  Preview first; add --apply --journal NEW.MDJ to write.
  Diagnose > Repair reserved attributes requires typing FLAGS.
  Clears only reserved attribute bits 40h and 80h in live records.
  Defined flags, entry types, names, LFN text, contents and allocation stay.
  Ambiguous types and unrelated LFN damage are refused.
  Version-21 journals preserve original sectors for validated undo.
  Invalid DOS aliases may remain unchanged; FOLLOWUP_NAMES reports them.
  Repair attributes first, then DOS aliases. Undo the journals in reverse
  order to restore the original combined damage.

FINDING VOLUMES WITHOUT PARTITION ENTRIES
  Disk > Search lost volumes scans a disk LBA range. Zero count searches
  to the end in the TUI. Esc cancels; incomplete results remain identified.
  Enter inspects a candidate read-only; F2 selects other candidates.
  Checks, maps, sector inspection and file export remain available.
  Filesystem repairs and filesystem undo stay disabled for candidates.

  MAGICDSK DISK.IMG --find-volumes
  MAGICDSK --bios 81 --find-volumes --search-first 0 --search-count 100000
  MAGICDSK --bios 81 --volume-start 63
  MAGICDSK --bios 81 --volume-start 63 --boot-source 6

  Omit CLI search-count to search to the end; explicit zero is invalid.
  Candidate BPBs need matching reserved entries in at least one FAT.
  FAT32 backups can identify volumes with an invalid primary boot sector.
  Candidates can overlap or be stale. Inspect and export before recovery.
  Read failures make results incomplete. The TUI holds 128 candidates.
  Discovery itself never writes partition entries or filesystem sectors.
  Backup-based file export may combine --volume-start, --boot-source and
  --export-files. The primary-boot finding stays visible in the manifest.

MBR RECONSTRUCTION
  Disk > Reconstruct MBR: enter empty slot, candidate LBA, boot source,
  BIOS heads and sectors/track. Review, choose a new journal, type MBR.
  Inspect candidate files first. Hidden-sector mismatches are displayed.
  MAGICDSK DISK.IMG --rebuild-mbr 2 --volume-start 63 --chs 16 63
  Add --apply --journal NEW.MDJ to write after reviewing the preview.
  MAGICDSK DISK.IMG --restore-mbr NEW.MDJ --apply
  Use --bios 81 instead of DISK.IMG for the second BIOS disk.
  Keep commands within DOS's line limit, including redirection.
  Add --boot-source 6 for a corroborated FAT32 backup.
  Inserts one inactive primary entry; preserves bootstrap and other slots.
  Does not repair EBRs, boot code, BPB fields, or missing volume geometry.
  Refuses overlaps, ambiguous entries, GPT evidence and unreadable metadata.
  Separate MBR journal is committed and verified before writing sector zero.
  Failed writes attempt exact rollback. Keep the journal after success.
  Undo interrupted MBR / --accept-torn accepts only saved byte mixtures.
  BIOS writes require an unmounted source and separate IDE journal disk.
  DOS may mount the reconstructed partition after reboot and prevent undo.

LOGICAL PARTITION ENTRY RECONSTRUCTION
  Disk > Reconstruct EBR restores an empty logical-volume entry in a
  record that remains reachable through the existing extended chain.
  Enter EBR disk LBA, volume disk LBA, boot source and BIOS geometry.
  Review, choose a new journal, and type EBR. Existing links stay unchanged.
  MAGICDSK DISK.IMG --rebuild-ebr 63 --volume-start 126 --chs 16 63
  Add --apply --journal E.MDJ to write after reviewing the preview.
  MAGICDSK DISK.IMG --restore-ebr E.MDJ --apply
  EBR journals bind the original MBR, selected record and volume boot.
  Undo displays the EBR sector and requires typing RESTORE in the TUI.
  Interrupted EBR undo accepts only saved before/after byte mixtures.
  Existing MBR, links, bootstrap, BPB fields and filesystem data stay.
  Unreachable records, ambiguous entries, loops and overlaps are refused.
  Missing chain links and extended containers are not reconstructed.
  BIOS writes still require an unmounted source and separate journal disk.

RECONNECTING AN EBR CHAIN
  Disk > Reconnect EBR chain previews one missing link to intact records.
  Enter source and target EBR disk LBAs, then BIOS geometry.
  Review all recovered volumes using arrows, PgUp/PgDn or Home/End.
  Choose a new journal and type LINK to apply.
  MAGICDSK DISK.IMG --link-ebr 63 3069 --chs 16 63
  Add --apply --journal LINK.MDJ after reviewing the preview.
  MAGICDSK DISK.IMG --restore-link LINK.MDJ --apply
  Every recovered record and primary FAT boot sector is saved and checked.
  Only an entirely empty link changes; loops and overlaps are refused.
  Other entries, links, MBR and filesystem data are preserved.
  Later changes to reviewed chain metadata prevent undo.
  BIOS writes require the entire source disk to be unmounted.
  Separate commands rebuild missing containers and empty EBR tables.

MISSING EXTENDED CONTAINERS
Disk > Rebuild extended container restores one empty MBR slot around a
surviving FAT logical-volume chain. Enter the slot, first EBR disk LBA,
total container sectors, type (hex 05, 0F or 85), and BIOS geometry.
Review every recovered volume, choose a new journal and type CONTAINER.
The extent is explicit: surviving links cannot prove the original free space.

  MAGICDSK DISK.IMG --rebuild-container 1 63 30000 0F --chs 16 63
Add --apply --journal C.MDJ to write after review.
  MAGICDSK DISK.IMG --restore-container C.MDJ --apply
Add --accept-torn only for interrupted writes with saved before/after bytes.

Only the selected MBR entry/signature changes. All EBR and boot metadata is
revalidated. Existing containers, overlaps, GPT and ambiguous chains refuse
repair. BIOS writes require an unmounted disk and a separate journal disk.
After reboot DOS may mount recovered volumes, preventing undo. Empty EBR
tables have a separate whole-record reconstruction command.

EMPTY EBR TABLES
Disk > Rebuild whole EBR restores a reachable record with four empty entries.
Enter the EBR disk LBA, volume disk LBA, next EBR LBA (0 ends the chain),
boot source (0 primary, 6 FAT32 backup), and BIOS geometry. Review the selected
volume and every recovered tail volume. Choose a new journal and type RECORD.

  MAGICDSK DISK.IMG --rebuild-record 63 3069 --volume-start 126 --chs 16 63
Add --apply --journal R.MDJ to write the EBR after journal persistence.
  MAGICDSK DISK.IMG --restore-record R.MDJ --apply
Use --accept-torn, or Disk > Undo interrupted record, only for interrupted
writes composed of saved before/after bytes. Both undo modes require review.

The original MBR, EBR, volume boot and recovered tail metadata are revalidated.
Only the selected EBR table/signature changes; surviving other bytes remain.
Ambiguous entries, overlaps, loops, GPT and unreadable metadata refuse repair.
BIOS repairs require an unmounted disk and a separate journal disk.
