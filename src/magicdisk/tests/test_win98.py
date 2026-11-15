#!/usr/bin/env python3
"""Boot a user-supplied Windows 98 DOS disk in QEMU; never modify the original."""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from directory_fixture import CHILD, PARENT, UNICODE_FILE, create, damaged_names, command, Layout
from boot_fixture import copy_boot
from lost_fixture import lost_fixture
from chain_fixture import chain_fixture
from directory_chain_fixture import directory_chain_fixture
from duplicate_fixture import duplicate_alias
from recovery_check import verify
from source_fixture import rebuilt_info
from entry_fixture import entry_fixture, missing_dot_fixture, reversed_dot_fixture, damaged_dot_pair_fixture
from ui_scripts import live_export_keys, raw_sector_keys, sector_keys, mounted_entry_keys, transaction_keys, mounted_source_keys, mounted_tree_keys, primary_keys, main_keys, marker_keys, lfn_keys, fsinfo_keys, boot_keys, floppy_keys, hard_keys, mounted_keys, mounted_mirror_keys, mounted_recovery_keys, mounted_reclaim_keys, mounted_chain_keys, mounted_directory_keys

exe, source_boot = (Path(arg).resolve() for arg in sys.argv[1:3])
viewer_only = len(sys.argv) > 3 and sys.argv[3] == "--viewer"
pairs_only = len(sys.argv) > 3 and sys.argv[3] == "--dot-pairs"
order_only = len(sys.argv) > 3 and sys.argv[3] == "--dot-order"
missing_only = len(sys.argv) > 3 and sys.argv[3] == "--missing-dots"
entry_only = pairs_only or order_only or missing_only or (len(sys.argv) > 3 and sys.argv[3] == "--entries")
progress_only = len(sys.argv) > 3 and sys.argv[3] == "--progress"
source_only = len(sys.argv) > 3 and sys.argv[3] == "--tree-source"
tree_only = source_only or (len(sys.argv) > 3 and sys.argv[3] == "--tree")
primary_only = len(sys.argv) > 3 and sys.argv[3] == "--primary"
mixed_only = len(sys.argv) > 3 and sys.argv[3] == "--mixed"
directory_only = len(sys.argv) > 3 and sys.argv[3] == "--directory-chains"
chain_only = len(sys.argv) > 3 and sys.argv[3] == "--chains"
reclaim_only = len(sys.argv) > 3 and sys.argv[3] == "--reclaim"
recovery_only = len(sys.argv) > 3 and sys.argv[3] == "--recover"
probe_only = len(sys.argv) > 3 and sys.argv[3] == "--probe"
raw_only = len(sys.argv) > 3 and sys.argv[3] == "--raw"
metadata_kind = sys.argv[3][2:] if len(sys.argv) > 3 and sys.argv[3] in ['--fsinfo', '--boot', '--mirror'] else None
if progress_only:
    metadata_kind = 'mirror'


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def copy_into(image, source, destination):
    command('mcopy', '-o', '-i', str(image), str(source), '::/' + destination)


def text_file(root, name, text):
    path = root / name
    path.write_bytes(text.replace('\n', '\r\n').encode('ascii'))
    return path


def partition(mbr, index, kind, start, count):
    offset = 446 + index * 16
    mbr[offset + 4] = kind
    for pos, lba in [(offset + 1, start), (offset + 5, start + count - 1)]:
        cylinder, rest = divmod(lba, 16 * 63)
        head, sector = divmod(rest, 63)
        mbr[pos:pos + 3] = bytes([head, sector + 1 | (cylinder >> 2 & 0xc0), cylinder & 255])
    struct.pack_into('<II', mbr, offset + 8, start, count)


original_boot_hash = digest(source_boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-win98-') as directory:
    root = Path(directory)
    boot, primary, secondary, disk = (root / name for name in ['BOOT.IMG', 'P16.IMG', 'P32.IMG', 'DISK.IMG'])
    copy_boot(source_boot, boot)
    boot_layout = Layout(boot.read_bytes(), 12)
    floppy_clusters = (boot.stat().st_size // 512 - boot_layout.data_sector) // boot_layout.spc
    floppy_cluster = next(c for c in range(floppy_clusters + 1, 2, -1) if not boot_layout.fat(c))
    create(primary, 16, 256 * 1024 * 1024)
    create(secondary, 32)
    primary_start = 63
    extended_start = primary_start + primary.stat().st_size // 512
    secondary_start = extended_start + 63
    for image, bits, start in [(primary, 16, primary_start), (secondary, 32, secondary_start)]:
        with image.open('r+b') as file:
            boot_sector = bytearray(file.read(512))
            struct.pack_into('<HHI', boot_sector, 24, 63, 16, start)
            file.seek(0)
            file.write(boot_sector)
            if bits == 32:
                file.seek(struct.unpack_from('<H', boot_sector, 50)[0] * 512)
                file.write(boot_sector)
    marker = root / 'MARK.IMG'
    marker_layout = create(marker, 12)
    assert marker_layout.fat(341) == 0 and marker_layout.fat(1000) == 0
    marker_hash = digest(marker)
    # A known free cluster near the end of the floppy; its LFN tree remains intact.
    lfn = root / 'LFN.IMG'
    layout = create(lfn, 12)
    damaged_lfn = bytearray(duplicate_alias(damaged_names(layout), 12))
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    _, timestamp_entry = next((a, b) for a, b in layout.long_names(child)
                              if layout.decoded_name(a) == UNICODE_FILE)
    damaged_lfn[timestamp_entry + 13] = 200
    for field in [14, 16, 18, 22, 24]:
        struct.pack_into('<H', damaged_lfn, timestamp_entry + field, 0xffff)
    lfn.write_bytes(damaged_lfn)
    lfn_hash = digest(lfn)
    info = root / 'INFO.IMG'
    shutil.copy2(secondary, info)
    with info.open('r+b') as file:
        for sector in [1, 7]:
            file.seek(sector * 512)
            file.write(bytes(512))
    info_hash = digest(info)
    raw_image = root / "RAW.IMG"
    raw_image.write_bytes(bytes(range(256)) * 2)
    raw_hash = digest(raw_image)
    backup_image = root / 'BACKUP.IMG'
    shutil.copy2(secondary, backup_image)
    with backup_image.open('r+b') as file:
        file.seek(6 * 512)
        file.write(bytes(512))
    backup_hash = digest(backup_image)
    for source, name in [(exe.with_name('LOCKTEST.EXE'), 'LOCKTEST.EXE'),
                         (backup_image, 'BACKUP.IMG'), (exe, 'MAGICDSK.EXE'), (exe.with_name('KEYPLAY.EXE'), 'KEYPLAY.EXE'),
                         (raw_image, 'RAW.IMG'), (marker, 'MARK.IMG'), (lfn, 'LFN.IMG'), (info, 'INFO.IMG')]:
        copy_into(primary, source, name)
    # This test device exists only in the QEMU command below. Flush DOS buffers,
    # then signal success through isa-debug-exit (host process status 33).
    exit_test = root / 'EXITTEST.COM'
    exit_test.write_bytes(bytes.fromhex('b4 0d cd 21 ba f4 00 b0 10 ee eb fe'))
    copy_into(primary, exit_test, exit_test.name)
    # The marker tour's sector 372 maps to the verified free cluster 341.
    tours = [
        ('MAIN', 'MARK.IMG', main_keys),
        ('VIEW', 'MARK.IMG', sector_keys(marker.read_bytes()[:512], marker_layout.data_sector, 60 if viewer_only else 0)),
        ('RAWV', 'RAW.IMG', raw_sector_keys),
        ('MARK', 'MARK.IMG', marker_keys),
        ('LFN', 'LFN.IMG', lfn_keys),
        ('LIVE', 'LFN.IMG', live_export_keys),
        ('FS', 'INFO.IMG', fsinfo_keys),
        ('BOOT', 'BACKUP.IMG', boot_keys),
        ('FD', None, floppy_keys),
        ('HD', None, hard_keys),
        ('DU', None, mounted_keys),
    ]
    for name, target, keys in tours:
        copy_into(primary, text_file(root, name + '.KEY', keys), name + '.KEY')
    commands = ['@echo off', 'C:', 'cd \\', 'ver > A:\\VERSION.TXT']
    if source_only:
        copy_into(primary, exe.with_name('SECTGET.EXE'), 'SECTGET.EXE')
        commands += [f'SECTGET 80 {secondary_start + 1} > A:\\S0.BIN']
    commands += ['LOCKTEST', 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
    checks = [
        ('--devices', 'A:\\DEVICES.TXT'),
        ('--bios 80 --partition 1', 'A:\\FAT16.TXT'),
        ('--bios 80 --partition 2', 'A:\\FAT32.TXT'),
        ('--bios 00', 'C:\\FLOPPY.TXT'),
        ('MARK.IMG --surface', 'A:\\SURFACE.TXT'),
        ('MARK.IMG --mark-bad 1000 --apply --journal MARK.MDJ', 'A:\\MARK.TXT'),
        ('MARK.IMG --retest-bad', 'A:\\RETEST.TXT'),
        ('MARK.IMG --unmark-bad 1000 --apply --journal UNMARK.MDJ', 'A:\\UNMARK.TXT'),
        ('MARK.IMG --restore-journal UNMARK.MDJ --apply', 'A:\\UNDO.TXT'),
        ('MARK.IMG --restore-journal MARK.MDJ --apply', 'A:\\RESTORE.TXT'),
        ('INFO.IMG --repair-fsinfo --apply --journal INFO.MDJ', 'A:\\FSAPPLY.TXT'),
        ('INFO.IMG', 'A:\\FSCHECK.TXT'),
        ('INFO.IMG --restore-journal INFO.MDJ --apply', 'A:\\FSUNDO.TXT'),
        ('BACKUP.IMG --repair-boot-backup', 'A:\\BPREV.TXT'),
        ('BACKUP.IMG --repair-boot-backup --apply --journal BOOT.MDJ', 'A:\\BAPPLY.TXT'),
        ('BACKUP.IMG', 'A:\\BCHECK.TXT'),
        ('BACKUP.IMG --restore-journal BOOT.MDJ --apply', 'A:\\BUNDO.TXT'),
    ]
    raw_checks = [
        ('--drive C', 'A:\\DOS16.TXT'),
        ('--drive D', 'A:\\DOS32.TXT'),
        ('--drive A', 'C:\\DOS12.TXT'),
        ('--drive D --mark-bad 1000 --apply --journal C:\\RAW.MDJ', 'A:\\RAW.TXT'),
        ('--bios 80 --partition 2 --retest-bad', 'A:\\RAWTEST.TXT'),
        ('--drive D --unmark-bad 1000 --apply --journal C:\\RAWFREE.MDJ', 'A:\\RAWFREE.TXT'),
        ('--drive D --restore-journal C:\\RAWFREE.MDJ --apply', 'A:\\RAWBACK.TXT'),
        ('--drive D --restore-journal C:\\RAW.MDJ --apply', 'A:\\RAWUNDO.TXT'),
        ('--drive C --mark-bad 60000 --apply --journal A:\\RAW16.MDJ', 'A:\\RAW16.TXT'),
        ('--bios 80 --partition 1 --retest-bad', 'A:\\RAW16T.TXT'),
        ('--drive C --restore-journal A:\\RAW16.MDJ --apply', 'A:\\RAW16U.TXT'),
    ]
    raw_checks += [
        (f'--drive A --mark-bad {floppy_cluster} --apply --journal C:\\RAW12.MDJ', 'C:\\RAW12.TXT'),
        ('--bios 00 --retest-bad', 'C:\\RAW12T.TXT'),
        ('--drive A --restore-journal C:\\RAW12.MDJ --apply', 'C:\\RAW12U.TXT'),
    ]
    if metadata_kind:
        with secondary.open('r+b') as file:
            if metadata_kind == 'mirror':
                layout = Layout(secondary.read_bytes(), 32)
                file.seek((layout.reserved + layout.fatsz) * 512)
                file.write(b'\x5a' * (layout.fatsz * 512))
            else:
                for sector in ([1, 7] if metadata_kind == 'fsinfo' else [6]):
                    file.seek(sector * 512)
                    file.write(bytes(512))
        operation = {'fsinfo': '--repair-fsinfo', 'boot': '--repair-boot-backup',
                     'mirror': '--repair-fat 1'}[metadata_kind]
        checks = [
            ('--drive D ' + operation, 'A:\\MPREV.TXT'),
            ('--drive D ' + operation + ' --apply --journal C:\\METADATA.MDJ', 'A:\\MAPPLY.TXT'),
            ('--bios 80 --partition 2', 'A:\\MCHECK.TXT'),
            ('--drive D --restore-journal C:\\METADATA.MDJ --apply', 'A:\\MUNDO.TXT'),
        ]
        mirror_tour = transaction_keys if progress_only else mounted_mirror_keys
        tours = [('FM', None, mirror_tour)] if metadata_kind == 'mirror' else []
        if tours:
            copy_into(primary, text_file(root, 'FM.KEY', mirror_tour), 'FM.KEY')
    elif primary_only:
        # Use a file image: DOS cannot mount a volume with a missing primary BPB.
        primary_image = root / 'PRIMARY.IMG'
        damaged_primary = bytearray(secondary.read_bytes())
        damaged_primary[:512] = bytes(512)
        primary_image.write_bytes(damaged_primary)
        copy_into(primary, primary_image, 'PRIMARY.IMG')
        checks = [
            ('PRIMARY.IMG --repair-boot-primary', 'A:\\PPREV.TXT'),
            ('PRIMARY.IMG --repair-boot-primary --apply --journal PRIMARY.MDJ', 'A:\\PAPPLY.TXT'),
            ('PRIMARY.IMG', 'A:\\PCHECK.TXT'),
            ('PRIMARY.IMG --restore-journal PRIMARY.MDJ --apply', 'A:\\PUNDO.TXT'),
            ('PRIMARY.IMG --boot-source 6 --restore-journal PRIMARY.MDJ --apply', 'A:\\PREPEAT.TXT'),
        ]
        tours = [('PB', 'PRIMARY.IMG', primary_keys)]
        copy_into(primary, text_file(root, 'PB.KEY', primary_keys), 'PB.KEY')
    elif mixed_only:
        broken, repaired_files, entries = chain_fixture(secondary.read_bytes(), 32)
        for sector in [1, 6, 7]:
            broken[sector * 512:(sector + 1) * 512] = bytes(512)
        secondary.write_bytes(broken)
        checks = [
            ('--drive D --repair-files --apply --journal C:\\MFILE.MDJ', 'A:\\MFILE.TXT'),
            ('--drive D --repair-boot-backup --apply --journal C:\\MBOOT.MDJ', 'A:\\MBOOT.TXT'),
            ('--drive D --repair-fsinfo --apply --journal C:\\MINFO.MDJ', 'A:\\MINFO.TXT'),
            ('--bios 80 --partition 2', 'A:\\MIXCHECK.TXT'),
            ('--drive D --restore-journal C:\\MINFO.MDJ --apply', 'A:\\MIUNDO.TXT'),
            ('--drive D --restore-journal C:\\MBOOT.MDJ --apply', 'A:\\MBUNDO.TXT'),
            ('--drive D --restore-journal C:\\MFILE.MDJ --apply', 'A:\\MFUNDO.TXT'),
        ]
        tours = [('MC', None, mounted_chain_keys)]
        copy_into(primary, text_file(root, 'MC.KEY', mounted_chain_keys), 'MC.KEY')
    elif directory_only or tree_only or entry_only:
        source = secondary.read_bytes()
        if tree_only:
            damaged_entries, _ = entry_fixture(source, 32)
            _, files_fixed, _ = chain_fixture(source, 32)
            source, _, _ = chain_fixture(damaged_entries, 32)
        broken, repaired_directories, starts, ends = directory_chain_fixture(source, 32)
        if tree_only:
            _, repaired_directories, _, _ = directory_chain_fixture(files_fixed, 32)
        operation = '--repair-tree' if tree_only else '--repair-directories'
        keys = mounted_tree_keys if tree_only else mounted_directory_keys
        if entry_only:
            broken, repaired_directories = (damaged_dot_pair_fixture(source, 32) if pairs_only
                                              else reversed_dot_fixture(source, 32) if order_only
                                              else missing_dot_fixture(source, 32) if missing_only
                                              else entry_fixture(source, 32))
            operation = '--repair-directory-entries'
            keys = mounted_entry_keys
        if source_only:
            source_layout = Layout(broken, 32)
            start = source_layout.reserved * 512
            broken[start:start + source_layout.fatsz * 512] = b'\x5a' * (source_layout.fatsz * 512)
            operation += ' --fat-source 2'
            keys = mounted_source_keys
            repaired_directories = rebuilt_info(repaired_directories, 32)
            dos_baseline = bytearray(broken)
            # DOS flushes its UNKNOWN count before raw-lock acquisition/journal creation.
            struct.pack_into('<I', dos_baseline, 512 + 488, 0xffffffff)
        secondary.write_bytes(broken)
        checks = [
            ('--drive D ' + operation, 'A:\\DPREV.TXT'),
            ('--drive D ' + operation + ' --apply --journal C:\\DIRS.MDJ', 'A:\\DAPPLY.TXT'),
            ('--bios 80 --partition 2', 'A:\\DCHECK.TXT'),
            ('--drive D --restore-journal C:\\DIRS.MDJ --apply', 'A:\\DUNDO.TXT'),
        ]
        tours = [('DC', None, keys)]
        copy_into(primary, text_file(root, 'DC.KEY', keys), 'DC.KEY')
    elif chain_only:
        broken, repaired_files, repaired_entries = chain_fixture(secondary.read_bytes(), 32)
        secondary.write_bytes(broken)
        checks = [
            ('--drive D --repair-files', 'A:\\CPREV.TXT'),
            ('--drive D --repair-files --apply --journal C:\\FILES.MDJ', 'A:\\CAPPLY.TXT'),
            ('--bios 80 --partition 2', 'A:\\CCHECK.TXT'),
            ('--drive D --restore-journal C:\\FILES.MDJ --apply', 'A:\\CUNDO.TXT'),
        ]
        tours = [('FC', None, mounted_chain_keys)]
        copy_into(primary, text_file(root, 'FC.KEY', mounted_chain_keys), 'FC.KEY')
    elif reclaim_only:
        broken, recovery_payloads = lost_fixture(secondary.read_bytes(), 32)
        secondary.write_bytes(broken)
        checks = [
            ('--drive D --reclaim-lost', 'A:\\LPREV.TXT'),
            ('--drive D --reclaim-lost --apply --journal C:\\LOST.MDJ', 'A:\\LAPPLY.TXT'),
            ('--bios 80 --partition 2', 'A:\\LCHECK.TXT'),
            ('--drive D --restore-journal C:\\LOST.MDJ --apply', 'A:\\LUNDO.TXT'),
        ]
        tours = [('CL', None, mounted_reclaim_keys)]
        copy_into(primary, text_file(root, 'CL.KEY', mounted_reclaim_keys), 'CL.KEY')
    elif recovery_only:
        broken, recovery_payloads = lost_fixture(secondary.read_bytes(), 32)
        broken[512:1024] = bytes(512)
        broken[7 * 512 + 8] = 1
        secondary.write_bytes(broken)
        checks = [('--drive D --recover-lost C:\\', 'A:\\RECOVER.TXT')]
        tours = [('RC', None, mounted_recovery_keys)]
        copy_into(primary, text_file(root, 'RC.KEY', mounted_recovery_keys), 'RC.KEY')
        commands += [
            'MAGICDSK --drive D --recover-lost D:\\ > A:\\RECDENY.TXT',
            'if not errorlevel 2 echo FAIL >> A:\\STATUS.TXT',
        ]
    elif viewer_only:
        checks = []
        tours = [tour for tour in tours if tour[0] in ["VIEW", "RAWV", "HD"]]
    elif raw_only:
        checks = raw_checks
        tours = []
    else:
        checks += raw_checks
    for check_index, (args, output) in enumerate(checks):
        commands += ['MAGICDSK ' + args + ' > ' + output,
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if source_only:
            commands += [f'SECTGET 80 {secondary_start + 1} > A:\\S{check_index + 1}.BIN']
    commands += [
        'MAGICDSK --drive B --mark-bad 1000 --apply --journal C:\\READONLY.MDJ > A:\\READONLY.TXT',
        'if not errorlevel 2 echo FAIL >> A:\\STATUS.TXT',
        'MAGICDSK --drive D --mark-bad 1000 --apply --journal D:\\BAD.MDJ > A:\\DENIED.TXT',
        'if not errorlevel 2 echo FAIL >> A:\\STATUS.TXT',
        'MAGICDSK --drive C --mark-bad 60000 --apply --journal LOCAL.MDJ > A:\\DENIED2.TXT',
        'if not errorlevel 2 echo FAIL >> A:\\STATUS.TXT',
    ]
    for name, target, keys in tours:
        commands += [
            'KEYPLAY ' + name + '.KEY > A:\\' + name + 'KEY.TXT',
            'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
            'MAGICDSK' + (' --tui ' + target if target else ''),
            'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
            'echo RETURNED > A:\\' + name + 'EXIT.TXT',
        ]
        if name == 'MARK':
            commands += ['MAGICDSK MARK.IMG --retest-bad > A:\\UIMARK.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if name == 'MC':
            commands += [
                'MAGICDSK --drive D --repair-boot-backup --apply --journal C:\\UIBOOT.MDJ > A:\\UIBOOT.TXT',
                'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                'MAGICDSK --drive D --repair-fsinfo --apply --journal C:\\UIINFO.MDJ > A:\\UIINFO.TXT',
                'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                'MAGICDSK --bios 80 --partition 2 > A:\\UIMIX.TXT',
                'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                'MAGICDSK --drive D --restore-journal C:\\UIINFO.MDJ --apply > A:\\UIIUNDO.TXT',
                'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                'MAGICDSK --drive D --restore-journal C:\\UIBOOT.MDJ --apply > A:\\UIBUNDO.TXT',
                'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                'MAGICDSK --drive D --restore-journal C:\\FILESUI.MDJ --apply > A:\\UIFUNDO.TXT',
                'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
            ]
        if name == 'DC':
            commands += ['MAGICDSK --bios 80 --partition 2 > A:\\DIRBIOS.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                         'MAGICDSK --drive D --restore-journal C:\\DIRSUI.MDJ --apply > A:\\DIRUNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if name == 'FC':
            commands += ['MAGICDSK --bios 80 --partition 2 > A:\\FILEBIOS.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                         'MAGICDSK --drive D --restore-journal C:\\FILESUI.MDJ --apply > A:\\FILEUNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if name == 'CL':
            commands += ['MAGICDSK --bios 80 --partition 2 > A:\\LOSTBIOS.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                         'MAGICDSK --drive D --restore-journal C:\\LOSTUI.MDJ --apply > A:\\LOSTUNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if name == 'FM' and progress_only:
            commands += ['MAGICDSK --drive D --restore-journal C:\\FATUI.MDJ --apply > A:\\FATUNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if name == 'FM' and not progress_only:
            commands += ['MAGICDSK --bios 80 --partition 2 > A:\\FATBIOS.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                         'MAGICDSK --drive D --restore-journal C:\\FATUI.MDJ --apply > A:\\FATUNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if name == 'DU':
            commands += ['MAGICDSK --bios 80 --partition 2 --retest-bad > A:\\RAWUIT.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                         'MAGICDSK --drive D --restore-journal C:\\RAWUI.MDJ --apply > A:\\RAWUIU.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        journal = {'MARK': 'UI.MDJ', 'FS': 'FSUI.MDJ', 'BOOT': 'BOOTUI.MDJ', 'PB': 'PRIMARYU.MDJ'}.get(name)
        if journal:
            commands += ['MAGICDSK ' + target + ' --restore-journal ' + journal +
                         ' --apply > A:\\' + name + 'UNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
    commands += ['echo DONE >> A:\\STATUS.TXT', 'EXITTEST.COM']
    if probe_only:
        commands = ['@echo off', 'C:', 'cd \\', 'LOCKTEST',
                    'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                    'echo DONE >> A:\\STATUS.TXT', 'EXITTEST.COM']
    copy_into(primary, text_file(root, 'RUN.BAT', '\n'.join(commands) + '\n'), 'RUN.BAT')
    copy_into(boot, text_file(root, 'CONFIG.SYS',
                             'DEVICE=A:\\HIMEM.SYS /TESTMEM:OFF\nDOS=HIGH,UMB\nFILES=60\n'
                             'BUFFERS=20\nLASTDRIVE=Z\n'), 'CONFIG.SYS')
    copy_into(boot, exit_test, exit_test.name)
    copy_into(boot, text_file(root, 'AUTOEXEC.BAT',
                             '@echo off\nver > A:\\BOOTVER.TXT\nC:\\RUN.BAT\nA:\\EXITTEST.COM\n'), 'AUTOEXEC.BAT')
    mbr = bytearray(primary_start * 512)
    mbr[510:512] = b'\x55\xaa'
    mbr[446] = 0x80
    partition(mbr, 0, 6, primary_start, primary.stat().st_size // 512)
    partition(mbr, 1, 15, extended_start, 63 + secondary.stat().st_size // 512)
    ebr = bytearray(63 * 512)
    ebr[510:512] = b'\x55\xaa'
    partition(ebr, 0, 12, secondary_start, secondary.stat().st_size // 512)
    struct.pack_into('<I', ebr, 454, 63)
    with disk.open('wb') as file:
        file.write(mbr)
        for image in [primary, secondary]:
            if image == secondary:
                file.write(ebr)
            with image.open('rb') as source:
                shutil.copyfileobj(source, file)
        cylinders = (file.tell() // 512 + 1007) // 1008
        file.truncate(cylinders * 1008 * 512)
    process = subprocess.Popen([
        'qemu-system-i386', '-accel', 'tcg', '-m', '64', '-display', 'none',
        '-serial', 'none', '-monitor', 'stdio', '-no-reboot', '-boot', 'a',
        '-drive', f'file={boot},format=raw,if=floppy,index=0',
        '-drive', f'file={marker},format=raw,if=floppy,index=1,readonly=on',
        '-drive', f'file={disk},format=raw,if=none,id=systemdisk',
        '-device', f'ide-hd,drive=systemdisk,cyls={cylinders},heads=16,secs=63,bios-chs-trans=none',
        '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            progress_image = exe.parents[2] / 'magicdisk-test' / 'win98-progress.ppm'
            progress_image.parent.mkdir(parents=True, exist_ok=True)
            process.stdin.write(('screendump ' + str(progress_image) + '\n').encode('ascii'))
            process.stdin.flush()
            for attempt in range(10):
                try:
                    process.wait(timeout=29.5)
                    break
                except subprocess.TimeoutExpired:
                    if attempt == 9:
                        raise
                    process.stdin.write(('screendump ' + str(progress_image) + '\n').encode('ascii'))
                    process.stdin.flush()
    except subprocess.TimeoutExpired:
        snapshot = exe.parents[2] / 'magicdisk-test' / 'win98-timeout.ppm'
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.unlink(missing_ok=True)
        process.stdin.write(('screendump ' + str(snapshot) + '\n').encode('ascii'))
        process.stdin.flush()
        for _ in range(20):
            if snapshot.exists():
                break
            time.sleep(0.1)
        process.kill()
        process.wait()
        raise AssertionError('Windows 98 test timed out; VGA snapshot: ' + str(snapshot))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    stdout, stderr = process.communicate()
    assert process.returncode == 33, stderr.decode(errors='replace')

    def report(name, image=boot):
        return command('mtype', '-i', str(image), '::/' + name).stdout.decode('utf-8', errors='replace')

    lock_report = report('LOCKS.TXT')
    artifact = exe.parents[2] / 'magicdisk-test' / 'win98-locks.txt'
    artifact.parent.mkdir(parents=True, exist_ok=True)
    artifact.write_text(lock_report)
    if probe_only:
        assert "FAIL" not in report("STATUS.TXT") and "DONE" in report("STATUS.TXT")
        print(lock_report)
        assert digest(source_boot) == original_boot_hash
        print("PASS: QEMU Windows 98 lock capability probe; original boot image unchanged")
        sys.exit(0)
    primary_view = str(disk) + '@@' + str(primary_start * 512)
    if viewer_only:
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT')
        for name, target, keys in tours:
            assert 'RETURNED' in report(name + 'EXIT.TXT'), name
        for name, expected_hash in [('MARK.IMG', marker_hash), ('RAW.IMG', raw_hash)]:
            result_image = root / ('RESULT-' + name)
            command('mcopy', '-i', primary_view, '::/' + name, str(result_image))
            assert digest(result_image) == expected_hash
        assert 'Issues: 0' in report('HD.TXT', primary_view)
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            assert file.read(secondary.stat().st_size) == secondary.read_bytes()
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU sector viewer hex/ASCII, pages, LBA bounds/jump, map/menu/F8, BIOS and unformatted image; sources unchanged')
        sys.exit(0)
    if primary_only:
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT'), report('STATUS.TXT')
        assert 'Preview only' in report('PPREV.TXT')
        assert 'committed and verified' in report('PAPPLY.TXT')
        for name in ['PUNDO.TXT', 'PREPEAT.TXT', 'PBUNDO.TXT']:
            assert 'restored and verified' in report(name), report(name)
        assert '0 issues' in report('PCHECK.TXT')
        assert 'Issues: 0' in report('PRIMARYU.TXT', primary_view)
        assert 'RETURNED' in report('PBEXIT.TXT')
        result_image = root / 'RESULT.IMG'
        command('mcopy', '-i', primary_view, '::/PRIMARY.IMG', str(result_image))
        assert result_image.read_bytes() == damaged_primary
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU Windows 98 primary boot CLI/TUI recovery, clean checks, repeated undo and exact image preservation')
        sys.exit(0)
    if mixed_only:
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT'), report('STATUS.TXT')
        for name in ['MFILE.TXT', 'MBOOT.TXT', 'MINFO.TXT', 'UIBOOT.TXT', 'UIINFO.TXT']:
            assert 'committed and verified' in report(name), report(name)
        for name in ['MIUNDO.TXT', 'MBUNDO.TXT', 'MFUNDO.TXT', 'UIIUNDO.TXT', 'UIBUNDO.TXT', 'UIFUNDO.TXT']:
            assert 'restored and verified' in report(name), report(name)
        assert 'RETURNED' in report('MCEXIT.TXT')
        assert '0 issues' in report('MIXCHECK.TXT') and '0 issues' in report('UIMIX.TXT')
        after_files = report('FILESUI.TXT', primary_view)
        assert 'BOOT_BACKUP' in after_files and 'FSINFO_SIGNATURE' in after_files
        assert 'FILE_SIZE' not in after_files and 'CHAIN_' not in after_files
        assert report('FPREV.TXT', primary_view).count('FILE_REPAIR') == 9
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            assert file.read(secondary.stat().st_size) == secondary.read_bytes(), 'Mixed repair undo differs'
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU Windows 98 simultaneous file/FSInfo/backup damage, CLI/TUI staged repairs, BIOS verification and reverse undo')
        sys.exit(0)
    if directory_only or tree_only or entry_only:
        if source_only:
            counts = []
            for index in range(5):
                # DOS/32A prints a banner before the probe switches stdout to binary.
                snapshot = command('mtype', '-i', str(boot), f'::/S{index}.BIN').stdout[-512:]
                assert len(snapshot) == 512 and snapshot[:4] == b'RRaA'
                counts.append(struct.unpack_from('<I', snapshot, 488)[0])
            old_count = struct.unpack_from('<I', broken, 512 + 488)[0]
            fixed_count = struct.unpack_from('<I', repaired_directories, 512 + 488)[0]
            assert counts == [old_count, old_count, fixed_count, fixed_count, 0xffffffff], counts
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT'), report('STATUS.TXT')
        assert ('Repair entries in 2 directories' if entry_only else 'Repair 12 files/directories' if tree_only else 'Repair 3 directory chains') in report('DPREV.TXT')
        assert 'Pasta de projetos/Relatórios longos' in report('DPREV.TXT')
        assert 'committed and verified' in report('DAPPLY.TXT')
        assert '0 issues' in report('DCHECK.TXT')
        assert 'restored and verified' in report('DUNDO.TXT')
        assert 'RETURNED' in report('DCEXIT.TXT')
        assert ('Directory-entry-repair preview' if entry_only else 'Combined-repair preview' if tree_only else 'Directory-repair preview') in report('DPREV.TXT', primary_view)
        if tree_only:
            assert report('DPREV.TXT', primary_view).count('FILE_REPAIR') == 9
            assert report('DPREV.TXT', primary_view).count('ENTRY_REPAIR') == 2
        assert 'No repair applied' in report('DPREV.TXT', primary_view)
        if pairs_only:
            preview = report('DPREV.TXT', primary_view)
            assert preview.count('replace duplicate dot entry') == 1
            assert preview.count('restore dot/dotdot order') == 1
            assert preview.count('rebuild 1 missing dot entry with unset timestamps') == 2
        if order_only:
            assert report('DPREV.TXT', primary_view).count('restore dot/dotdot order and correct metadata') == 2
        if missing_only:
            assert report('DPREV.TXT', primary_view).count('rebuild 2 missing dot entries with unset timestamps') == 2
        if source_only:
            assert 'Use FAT 2 as authority' in report('DPREV.TXT', primary_view)
        assert report('DPREV.TXT', primary_view).count('ENTRY_REPAIR' if entry_only else 'DIR_REPAIR') == (2 if entry_only else 3)
        assert '0 issues' in report('DIRBIOS.TXT')
        assert 'Issues: 0' in report('DIRSUI.TXT', primary_view)
        assert 'restored and verified' in report('DIRUNDO.TXT')
        for name in ['DIRS.MDJ', 'DIRSUI.MDJ']:
            saved = root / name
            command('mcopy', '-i', primary_view, '::/' + name, str(saved))
            raw = saved.read_bytes()
            journal_before = dos_baseline if source_only else broken
            reconstructed = bytearray(journal_before)
            for index in range(struct.unpack_from('<I', raw, 12)[0]):
                record = 1088 + index * 1032
                sector = struct.unpack_from('<I', raw, record)[0]
                assert raw[record + 4:record + 516] == journal_before[sector * 512:(sector + 1) * 512], (name, sector, raw[record + 4 + 488:record + 4 + 496].hex())
                reconstructed[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
            assert reconstructed == repaired_directories
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            actual = file.read(secondary.stat().st_size)
            original = bytes(dos_baseline) if source_only else secondary.read_bytes()
            differences = [i for i in range(len(actual)) if actual[i] != original[i]]
            assert not differences, 'Directory repair undo differs: ' + repr([
                (i // 512, i % 512, original[i], actual[i]) for i in differences[:32]])
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU Windows 98 CLI/TUI ' + ('dot-pair' if pairs_only else 'dot-order' if order_only else 'missing-dot' if missing_only else 'directory-entry' if entry_only else 'selected-source tree' if source_only else 'combined tree' if tree_only else 'directory') + ' repair, preview/export, exact journals and whole-volume undo')
        sys.exit(0)
    if chain_only:
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT'), report('STATUS.TXT')
        assert 'Repair 9 file chains/lengths' in report('CPREV.TXT')
        assert 'Long report number' in report('CPREV.TXT')
        assert 'committed and verified' in report('CAPPLY.TXT')
        assert '0 issues' in report('CCHECK.TXT')
        assert 'restored and verified' in report('CUNDO.TXT')
        assert 'RETURNED' in report('FCEXIT.TXT')
        assert 'No repair applied' in report('FPREV.TXT', primary_view)
        assert report('FPREV.TXT', primary_view).count('FILE_REPAIR') == 9
        assert '0 issues' in report('FILEBIOS.TXT')
        assert 'Issues: 0' in report('FILESUI.TXT', primary_view)
        assert 'restored and verified' in report('FILEUNDO.TXT')
        # Both journals must describe the independently calculated repair exactly.
        for name in ['FILES.MDJ', 'FILESUI.MDJ']:
            saved = root / name
            command('mcopy', '-i', primary_view, '::/' + name, str(saved))
            raw = saved.read_bytes()
            reconstructed = bytearray(broken)
            for index in range(struct.unpack_from('<I', raw, 12)[0]):
                record = 1088 + index * 1032
                sector = struct.unpack_from('<I', raw, record)[0]
                assert raw[record + 4:record + 516] == broken[sector * 512:(sector + 1) * 512], (name, sector, raw[record + 4 + 488:record + 4 + 496].hex())
                reconstructed[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
            assert reconstructed == repaired_files
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            assert file.read(secondary.stat().st_size) == secondary.read_bytes(), 'File repair undo differs'
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU Windows 98 CLI/TUI file repair, LFN preservation, exact repair journals and source-volume undo')
        sys.exit(0)
    if reclaim_only:
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT'), report('STATUS.TXT')
        assert 'Reclaim 13 lost clusters' in report('LPREV.TXT')
        assert 'committed and verified' in report('LAPPLY.TXT')
        assert '0 issues' in report('LCHECK.TXT')
        assert 'restored and verified' in report('LUNDO.TXT')
        assert 'RETURNED' in report('CLEXIT.TXT')
        assert '0 issues' in report('LOSTBIOS.TXT')
        assert 'Issues: 0' in report('LOSTUI.TXT', primary_view)
        assert 'restored and verified' in report('LOSTUNDO.TXT')
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            assert file.read(secondary.stat().st_size) == secondary.read_bytes(), 'Reclaim undo differs'
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU Windows 98 CLI/TUI orphan reclaim, BIOS verification and exact source-volume undo')
        sys.exit(0)
    if recovery_only:
        assert 'FAIL' not in report('STATUS.TXT') and 'DONE' in report('STATUS.TXT'), report('STATUS.TXT')
        assert '13/13 clusters' in report('RECOVER.TXT'), report('RECOVER.TXT')
        assert 'separate DOS volume' in report('RECDENY.TXT'), report('RECDENY.TXT')
        assert 'RETURNED' in report('RCEXIT.TXT')
        for name in ['REC00001', 'REC00002']:
            command('mcopy', '-s', '-i', primary_view, '::/' + name, str(root))
            verify(root / name, recovery_payloads)
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            assert file.read(secondary.stat().st_size) == secondary.read_bytes(), 'Recovery changed source'
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU Windows 98 CLI/TUI lost-chain recovery, exact payloads, separate destination and unchanged source')
        sys.exit(0)
    if metadata_kind:
        if metadata_kind == 'mirror':
            assert 'RETURNED' in report('FMEXIT.TXT')
            if progress_only:
                assert 'Issues: 0' in report('C.TXT', primary_view)
            else:
                assert '0 issues' in report('FATBIOS.TXT')
            assert 'restored and verified' in report('FATUNDO.TXT')
            assert 'Issues: 0' in report('FATUI.TXT', primary_view)
            saved = root / 'checked-mirror.mdj'
            command('mcopy', '-i', primary_view, '::/METADATA.MDJ', str(saved))
            assert struct.unpack_from('<II', saved.read_bytes(), 8)[1] > 16
        assert 'FAIL'  not in report('STATUS.TXT'), report('STATUS.TXT')
        assert 'committed and verified' in report('MAPPLY.TXT'), report('MAPPLY.TXT')
        assert '0 issues' in report('MCHECK.TXT'), report('MCHECK.TXT')
        assert 'restored and verified' in report('MUNDO.TXT'), report('MUNDO.TXT')
        with disk.open('rb') as file:
            file.seek(secondary_start * 512)
            assert file.read(secondary.stat().st_size) == secondary.read_bytes(), 'Metadata undo differs'
        assert digest(source_boot) == original_boot_hash
        print('PASS: QEMU transaction progress, cancellable restore validation, ignored write-phase Esc and exact undo'
              if progress_only else 'PASS: QEMU mounted FAT32 ' + metadata_kind + ' repair, BIOS verification and exact undo')
        sys.exit(0)
    for name in ['RAW.TXT' , 'RAWFREE.TXT', 'RAW16.TXT']:
        assert 'committed and verified' in report(name), report(name)
    for name in ['RAWTEST.TXT', 'RAW16T.TXT']:
        assert '1 marked, 1 readable' in report(name), report(name)
    for name in ['RAWUNDO.TXT', 'RAWBACK.TXT', 'RAW16U.TXT']:
        assert 'restored and verified' in report(name), report(name)
    assert 'committed and verified' in report('RAW12.TXT', primary_view), report('RAW12.TXT', primary_view)
    assert '1 marked, 1 readable' in report('RAW12T.TXT', primary_view)
    assert 'restored and verified' in report('RAW12U.TXT', primary_view)
    assert digest(marker) == marker_hash, 'Write-protected floppy changed'
    readonly = report('READONLY.TXT')
    assert 'Repair committed' not in readonly and ('Rollback incomplete' in readonly or
           'Cannot obtain' in readonly or 'original sectors restored' in readonly), readonly
    for name in ['DENIED.TXT', 'DENIED2.TXT']:
        assert 'different local DOS volume' in report(name), report(name)
    assert Layout(boot.read_bytes(), 12).fat(floppy_cluster) == 0
    with disk.open('rb') as file:
        file.seek(secondary_start * 512)
        assert file.read(secondary.stat().st_size) == secondary.read_bytes(), 'FAT32 volume undo differs'
    assert 'DONE' in report('STATUS.TXT') and 'FAIL' not in report('STATUS.TXT')
    assert digest(source_boot) == original_boot_hash
    if raw_only:
        print('PASS: QEMU mounted FAT12/16/32 mark/unmark, BIOS retest, exact FAT32 undo and journal location guards')
        sys.exit(0)
    assert '1 marked, 1 readable' in report('RAWUIT.TXT'), report('RAWUIT.TXT')
    assert 'restored and verified' in report('RAWUIU.TXT')
    for name, target, keys in tours:
        assert 'RETURNED' in report(name + 'EXIT.TXT'), name
    status = report('STATUS.TXT')
    assert 'DONE' in status and 'FAIL' not in status, status
    version = report('VERSION.TXT')
    assert 'Windows 98' in version, version
    for name, kind in [('FAT16.TXT', 'FAT16'), ('FAT32.TXT', 'FAT32')]:
        output = report(name)
        assert kind in output and '0 issues' in output, output
    assert 'Preview only' in report('BPREV.TXT')
    assert 'committed and verified' in report('BAPPLY.TXT')
    assert '0 issues' in report('BCHECK.TXT')
    assert '2880 sectors, 0 unreadable' in report('SURFACE.TXT')
    devices = report('DEVICES.TXT')
    assert '00h   Floppy drive 1' in devices and '80h   Hard disk 1' in devices
    assert 'committed and verified' in report('FSAPPLY.TXT')
    assert '0 issues' in report('FSCHECK.TXT')
    assert '1 marked, 1 readable' in report('RETEST.TXT')
    assert '1 marked, 1 readable' in report('UIMARK.TXT')
    primary_view = str(disk) + '@@' + str(primary_start * 512)
    for name, target, keys in tours:
        if name in ['FS', 'BOOT', 'MARK']:
            assert 'restored and verified' in report(name + 'UNDO.TXT')
    for name in ['FS.TXT', 'BOOTUI.TXT', 'FD.TXT', 'HD.TXT']:
        output = report(name, primary_view)
        assert 'Inspection: complete' in output and 'Issues: 0' in output, output
    assert 'BIOS 00h' in report('FD.TXT', primary_view)
    assert 'Volume LBA: ' + str(secondary_start) in report('HD.TXT', primary_view)
    lfn_report = report('LFN.TXT', primary_view)
    assert f'{PARENT}/{CHILD}/{UNICODE_FILE}' in lfn_report and 'x' * 255 in lfn_report
    assert lfn_report.count('DIR_TIMESTAMP') == 6, lfn_report
    assert 'Issues: 9' in lfn_report and 'DIR_DUPLICATE' in lfn_report, lfn_report
    assert '0 issues' in report('FLOPPY.TXT', primary_view)
    for name, expected in [('RAW.IMG', raw_hash), ('MARK.IMG', marker_hash), ('INFO.IMG', info_hash), ('LFN.IMG', lfn_hash),
                           ('BACKUP.IMG', backup_hash)]:
        extracted = root / ('checked-' + name)
        command('mcopy', '-i', primary_view, '::/' + name, str(extracted))
        assert digest(extracted) == expected, name + ' changed unexpectedly'
    live_manifest = report('REC00001/RECOVERY.TXT', primary_view)
    assert 'FINAL COMPLETE_WITH_GAPS' in live_manifest
    assert live_manifest.count('LIVE_BEGIN ') == 54
    assert UNICODE_FILE.encode().hex().upper() in live_manifest
    assert digest(source_boot) == original_boot_hash, 'Original boot image changed'
    print('PASS: ' + version.strip().replace('\n', ' ') +
          ' | DOS/32A, BIOS FAT12/16/32, LFN reports, marker/FSInfo/backup boot journals, mounted-volume repairs, all TUI tours and logical partitions')
