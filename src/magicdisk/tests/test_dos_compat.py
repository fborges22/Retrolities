#!/usr/bin/env python3
"""QEMU compatibility checks using copies of user-supplied DOS boot media."""
import argparse
import hashlib
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from directory_fixture import create, command, damaged_names, UNICODE_FILE, Layout
from ui_scripts import main_keys, live_export_keys, torn_restore_keys, occupied_dot_keys, mounted_tree_keys, step, typing, diagnose
from chain_fixture import chain_fixture, set_fat
from directory_chain_fixture import directory_chain_fixture
from entry_fixture import entry_fixture
from growth_fixture import growth_fixture
from shared_empty_fixture import shared_empty_fixture
from boot_fixture import copy_boot

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('exe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--tree', action='store_true')
parser.add_argument('--retry-faults', action='store_true')
parser.add_argument('--timestamps', action='store_true')
parser.add_argument('--deleted-recovery', action='store_true')
parser.add_argument('--deleted-directories', action='store_true')
parser.add_argument('--deleted-missing-dots', action='store_true')
parser.add_argument('--relocate-native', action='store_true')
parser.add_argument('--relocate-directory', action='store_true')
parser.add_argument('--relocate-root', action='store_true')
parser.add_argument('--relocate-torn-undo', action='store_true')
parser.add_argument('--growth', action='store_true', help='repair a full native directory and verify DOS cache refresh')
parser.add_argument('--shared-prefix', action='store_true', help='detach a shared directory tail while growing its unique prefix')
parser.add_argument('--empty-alias', action='store_true', help='give fully shared directories independent empty chains')
parser.add_argument('--alias-ui', action='store_true', help='review/export/commit shared directory replacement through the TUI')
parser.add_argument('--write-protected', action='store_true')
parser.add_argument('--cache-workload', action='store_true')
parser.add_argument('--mounted-bits', type=int, choices=[12, 16, 32], default=16)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
args.deleted_directories = args.deleted_directories or args.deleted_missing_dots
args.deleted_recovery = args.deleted_recovery or args.deleted_directories
args.relocate_native = args.relocate_native or args.relocate_directory or args.relocate_root
if args.relocate_root and (args.mounted_bits != 32 or args.relocate_directory):
    parser.error('--relocate-root requires FAT32 without --relocate-directory')
if args.relocate_torn_undo and not args.relocate_native:
    parser.error('--relocate-torn-undo requires a relocation test')
args.empty_alias = args.empty_alias or args.alias_ui
args.growth = args.growth or args.shared_prefix or args.empty_alias
args.tree = args.tree or args.growth
exe = args.exe.resolve()
if args.kernel == 'msdos622' and args.mounted_bits == 32:
    parser.error('MS-DOS 6.22 does not mount FAT32 volumes')
if args.tree and (args.cache_workload or args.write_protected):
    parser.error('--tree requires writable media without --cache-workload')
if args.relocate_native and (args.tree or args.cache_workload or args.write_protected):
    parser.error('--relocate-native requires ordinary writable-volume tests')
native_floppy = args.mounted_bits == 12
if args.write_protected and (not native_floppy or args.cache_workload):
    parser.error('--write-protected requires FAT12 without --cache-workload')
native_drive = 'B' if native_floppy else 'D'
free_query = 'FREESPC ' + native_drive + (' legacy' if args.kernel == 'msdos622' else '')


def digest(path):
    with path.open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def put(image, source, name=None):
    command('mcopy', '-o', '-i', str(image), str(source), '::/' + (name or source.name))


def text_file(root, name, content, encoding='ascii'):
    path = root / name
    path.write_bytes(content.replace('\n', '\r\n').encode(encoding))
    return path


original = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-' + args.kernel + '-') as directory:
    root = Path(directory)
    boot, volume, floppy, disk = [root / name for name in
                                  ['BOOT.IMG', 'VOLUME.IMG', 'FLOPPY.IMG', 'DISK.IMG']]
    copy_boot(args.boot, boot)
    create(volume, 16, 64 * 1024 * 1024)
    layout = create(floppy, 12)
    floppy.write_bytes(damaged_names(layout))
    floppy_hash = digest(floppy)
    repair_volume = root / 'REPAIR.IMG'
    create(repair_volume, args.mounted_bits, (1440 if native_floppy else 64 * 1024) * 1024)
    if args.growth:
        if args.empty_alias:
            tree_broken, tree_expected, native_added, native_aliases, owner_entry = shared_empty_fixture(
                repair_volume, args.mounted_bits, 2, nested=True)
            tree_layout = Layout(bytes(tree_broken), args.mounted_bits)
            owner = tree_layout.cluster(owner_entry)
            child_entry = tree_layout.child(owner)
            child = tree_layout.cluster(child_entry)
            data_entry = next(offset for offset in tree_layout.entries(child)
                              if tree_broken[offset + 11] == 32)

            def alias(offset):
                name = tree_broken[offset:offset + 8].decode('latin1').rstrip()
                extension = tree_broken[offset + 8:offset + 11].decode('latin1').rstrip()
                return name + ('.' + extension if extension else '')

            tree_directory = native_drive + ':\\' + alias(owner_entry) + '\\' + alias(child_entry)
            tree_file = tree_directory + '\\' + alias(data_entry)
            prewarm_token = alias(data_entry).split('.')[0].encode('latin1')
            old_alias_token = alias(child_entry).split('.')[0].encode('latin1')
            _, container_entry = next((parts, entry) for parts, entry in tree_layout.long_names(tree_layout.root)
                                      if tree_layout.decoded_name(parts) == 'Container')
            replacement_directory = native_drive + ':\\' + alias(container_entry) + '\\' + alias(native_aliases[0])
            data_cluster = tree_layout.cluster(data_entry)
            data_offset = (tree_layout.data_sector + (data_cluster - 2) * tree_layout.spc) * 512
            size = struct.unpack_from('<I', tree_broken, data_entry + 28)[0]
            tree_payload = tree_broken[data_offset:data_offset + size]
        else:
            tree_broken, tree_expected, native_added = growth_fixture(
                repair_volume, args.mounted_bits, name='Directory ' + 'x' * 210)
            tree_layout = Layout(bytes(tree_broken), args.mounted_bits)
            _, parent_entry = next((parts, entry) for parts, entry in tree_layout.long_names(tree_layout.root)
                                   if tree_layout.decoded_name(parts) == 'Empty')
            parent_cluster = tree_layout.cluster(parent_entry)
            if args.shared_prefix:
                owner = tree_layout.cluster(tree_layout.child())
                last = (tree_layout.slots(parent_cluster)[-1] // 512 - tree_layout.data_sector) // tree_layout.spc + 2
                set_fat(tree_broken, args.mounted_bits, last, owner)
            _, child_entry = next((parts, entry) for parts, entry in tree_layout.long_names(parent_cluster)
                                  if tree_layout.decoded_name(parts).startswith('Directory '))
            child_cluster = tree_layout.cluster(child_entry)
            data_entry = next(offset for offset in tree_layout.entries(child_cluster)
                              if tree_broken[offset:offset + 11] == b'DATA    TXT')

            def alias(offset):
                name = tree_broken[offset:offset + 8].decode('latin1').rstrip()
                extension = tree_broken[offset + 8:offset + 11].decode('latin1').rstrip()
                return name + ('.' + extension if extension else '')

            tree_directory = native_drive + ':\\' + alias(parent_entry) + '\\' + alias(child_entry)
            tree_file = tree_directory + '\\DATA.TXT'
            data_cluster = tree_layout.cluster(data_entry)
            data_offset = (tree_layout.data_sector + (data_cluster - 2) * tree_layout.spc) * 512
            tree_payload = tree_broken[data_offset:data_offset + struct.unpack_from('<I', tree_broken, data_entry + 28)[0]]
            prewarm_token = b'DATA'
        changed = {i // 512 for i in range(0, len(tree_broken), 512)
                   if tree_broken[i:i + 512] != tree_expected[i:i + 512]}
        for cluster in native_added:
            sector = tree_layout.data_sector + (cluster - 2) * tree_layout.spc
            changed.update(range(sector, sector + tree_layout.spc))
        tree_sectors = sorted(changed)
        repair_volume.write_bytes(tree_broken)
        put(volume, exe.with_name('SECTGET.EXE'))
    elif args.tree:
        clean = repair_volume.read_bytes()
        entry_damage, _ = entry_fixture(clean, args.mounted_bits)
        file_damage, _, file_entries = chain_fixture(entry_damage, args.mounted_bits)
        _, file_fixed, _ = chain_fixture(clean, args.mounted_bits)
        tree_broken, _, _, _ = directory_chain_fixture(file_damage, args.mounted_bits)
        _, tree_expected, _, _ = directory_chain_fixture(file_fixed, args.mounted_bits)
        tree_sectors = [i // 512 for i in range(0, len(clean), 512)
                        if tree_broken[i:i + 512] != tree_expected[i:i + 512]]
        tree_layout = Layout(clean, args.mounted_bits)
        parent_entry = tree_layout.child()
        parent_cluster = tree_layout.cluster(parent_entry)
        child_entry = tree_layout.child(parent_cluster)

        def alias(offset):
            name = clean[offset:offset + 8].decode('latin1').rstrip()
            extension = clean[offset + 8:offset + 11].decode('latin1').rstrip()
            return name + ('.' + extension if extension else '')

        tree_directory = native_drive + ':\\' + alias(parent_entry) + '\\' + alias(child_entry)
        tree_file = tree_directory + '\\' + alias(file_entries[0])
        first_cluster = tree_layout.cluster(file_entries[0])
        first_offset = (tree_layout.data_sector + (first_cluster - 2) * tree_layout.spc) * 512
        tree_payload = clean[first_offset:first_offset + tree_layout.spc * 512]
        repair_volume.write_bytes(tree_broken)
        put(volume, exe.with_name('SECTGET.EXE'))
    put(volume, exe)
    if args.retry_faults:
        put(volume, exe.with_name('RECFAULT.EXE'))
    put(volume, exe.with_name('KEYPLAY.EXE'))
    put(volume, exe.with_name('FREESPC.EXE'))
    put(volume, floppy, 'LFN.IMG')
    marker = root / 'MARK.IMG'
    marker_layout = create(marker, 12)
    marker_hash = digest(marker)
    put(volume, marker)
    torn_image = root / 'TORN.IMG'
    torn_original, _ = entry_fixture(marker.read_bytes(), 12)
    torn_image.write_bytes(torn_original)
    torn_journal = root / 'TORN.MDJ'
    command(str(exe.parents[2] / 'host' / 'magicdisk'), str(torn_image),
            '--repair-directory-entries', '--apply', '--journal', str(torn_journal))
    raw_journal = torn_journal.read_bytes()
    torn_bytes = bytearray(torn_original)
    for index in range(struct.unpack_from('<I', raw_journal, 12)[0]):
        record = raw_journal[1088 + index * 1032:1088 + (index + 1) * 1032]
        sector = struct.unpack_from('<I', record)[0]
        changed = [n for n in range(512) if record[4 + n] != record[516 + n]]
        for offset in changed[::2]:
            torn_bytes[sector * 512 + offset] = record[516 + offset]
    torn_image.write_bytes(torn_bytes)
    put(volume, torn_image)
    put(volume, torn_journal)
    put(volume, text_file(root, 'TORN.KEY', torn_restore_keys))
    space = root / 'SPACE.IMG'
    space_original, space_expected, _ = growth_fixture(space, 12)
    space_layout = Layout(space_original, 12)
    _, space_entry = next((parts, entry) for parts, entry in space_layout.long_names(space_layout.root)
                          if space_layout.decoded_name(parts) == 'Empty')
    space_parent = space_layout.cluster(space_entry)
    space_prefix = (space_layout.data_sector + (space_parent - 2) * space_layout.spc) * 512
    space.write_bytes(space_original)
    put(volume, space)
    put(volume, text_file(root, 'SPACE.KEY', occupied_dot_keys))
    relocated = root / 'TRELOC.IMG'
    relocated_original, relocated_expected = bytearray(space_original), bytearray(space_expected)
    tree_layout = Layout(relocated_original, 12)
    tree_parent = (space_prefix // 512 - tree_layout.data_sector) // tree_layout.spc + 2
    tree_last = (tree_layout.slots(tree_parent)[-1] // 512 - tree_layout.data_sector) // tree_layout.spc + 2
    set_fat(relocated_original, 12, tree_last, tree_parent)
    struct.pack_into('<I', relocated_original, space_prefix + 28, tree_layout.spc * 512 + 1)
    repaired_layout = Layout(relocated_expected, 12)
    moved_file = next(offset for offset in repaired_layout.entries(tree_parent)
                      if relocated_expected[offset:offset + 11] == b'FIRST   TXT')
    struct.pack_into('<I', relocated_expected, moved_file + 28, tree_layout.spc * 512)
    relocated.write_bytes(relocated_original)
    put(volume, relocated)
    warning = root / 'WARN.IMG' 
    warning_data = bytearray(marker.read_bytes())
    struct.pack_into('<H', warning_data, marker_layout.child() + 22, 0xffff)
    warning.write_bytes(warning_data)
    put(volume, warning)
    if native_floppy:
        if args.tree:
            floppy.write_bytes(tree_broken)
        else:
            shutil.copy2(marker, floppy)
        floppy_hash = digest(floppy)
    payload = root / 'PAYLOAD.BIN'
    payload.write_bytes(bytes(range(256)) * (1024 if native_floppy else 32768))
    if args.cache_workload:
        put(volume, payload)
    if args.alias_ui:
        keys = (mounted_tree_keys
                .replace(typing('OPEN DOS VOLUME (DRIVE LETTER)', 'D'),
                         typing('OPEN DOS VOLUME (DRIVE LETTER)', native_drive))
                .replace(typing('SAVE REPORT TO A SEPARATE DISK', 'D:\\NO.TXT'),
                         typing('SAVE REPORT TO A SEPARATE DISK', native_drive + ':\\NO.TXT'))
                .replace('DIR_REPAIR', 'DIR_REPLACE')
                .replace(typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\DIRSUI.MDJ'),
                         typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\TREE.MDJ'))
                .replace(step('REVIEW FILESYSTEM TREE REPAIR', 'enter'),
                         step('Fully shared directories become EMPTY', 'enter')))
        if args.mounted_bits != 32:
            keys = keys.replace(step('Rebuild FAT32 FSInfo', 'up', True),
                                step('Rebuild FAT32 FSInfo', 'up'))
            keys = keys.replace(step('Rebuild backup boot', 'up', True),
                                step('Rebuild backup boot', 'up'))
        put(volume, text_file(root, 'EALIAS.KEY', keys))
    put(volume, text_file(root, 'MAIN.KEY', main_keys))
    finding_image = root / 'FIND.IMG'
    finding_original = bytearray(marker.read_bytes())
    for cluster in range(1000, 1008):
        set_fat(finding_original, 12, cluster, 0xff7)
    finding_image.write_bytes(finding_original)
    put(volume, finding_image)
    finding_lba = marker_layout.data_sector + (1007 - 2) * marker_layout.spc
    finding_keys = (diagnose() + step('Filesystem check', 'down', True) +
                    step('Surface read', 'down', True) +
                    step('Retest marked bad', 'enter', True) +
                    step('Bad markers retained.', 'f6') +
                    step('Cluster 1000', 'down', True) +
                    step('Cluster 1001', 'pgdn', True) +
                    step('Cluster 1007', 'home', True) +
                    step('Cluster 1000', 'end', True) +
                    step('Cluster 1007', 'pgup', True) +
                    step('Cluster 1001', 'up', True) +
                    step('Cluster 1000', 'end', True) +
                    step('Cluster 1007', 'f8', True) +
                    step(f'Disk LBA {finding_lba} /', 'esc') +
                    step('Cluster 1007', 'esc', True) +
                    step('Bad markers retained.', 'f7') +
                    typing('SAVE REPORT TO A SEPARATE DISK', 'FINDVIEW.TXT') +
                    step('Report saved.', 'esc'))
    put(volume, text_file(root, 'FIND.KEY', finding_keys))
    relocation_image = root / 'RELOC.IMG'
    relocation_original = bytearray(marker.read_bytes())
    relocation_layout = Layout(relocation_original, 12)
    relocation_entry = next(offset for offset in relocation_layout.entries()
                            if relocation_original[offset + 11] == 32)
    if args.relocate_directory:
        relocation_entry = relocation_layout.child()
    relocation_cluster = relocation_layout.cluster(relocation_entry)
    replacement = next(c for c in range(2, 1000) if relocation_layout.fat(c) == 0)
    relocation_sector = relocation_layout.data_sector + (relocation_cluster - 2) * relocation_layout.spc
    destination = relocation_layout.data_sector + (replacement - 2) * relocation_layout.spc
    relocation_length = relocation_layout.spc * 512
    relocation_original[destination * 512:destination * 512 + relocation_length] = bytes([0xa5]) * relocation_length
    relocation_expected = bytearray(relocation_original)
    relocation_expected[destination * 512:destination * 512 + relocation_length] = relocation_original[
        relocation_sector * 512:relocation_sector * 512 + relocation_length]
    set_fat(relocation_expected, 12, replacement, relocation_layout.fat(relocation_cluster))
    set_fat(relocation_expected, 12, relocation_cluster, 0xff7)
    struct.pack_into('<H', relocation_expected, relocation_entry + 26, replacement)
    if args.relocate_directory:
        struct.pack_into('<H', relocation_expected, destination * 512 + 26, replacement)
        child = relocation_layout.cluster(relocation_layout.child(relocation_cluster))
        child_offset = (relocation_layout.data_sector + (child - 2) * relocation_layout.spc) * 512
        struct.pack_into('<H', relocation_expected, child_offset + 58, replacement)

    relocation_image.write_bytes(relocation_original)
    put(volume, relocation_image)
    relocate_menu = (step('Filesystem check', 'down', True) +
                     step('Surface read', 'down', True) +
                     step('Retest marked bad', 'down', True) +
                     step('Mark bad sector', 'enter', True) +
                     typing('MARK BAD: ABSOLUTE DISK SECTOR', str(relocation_sector)))
    relocation_keys = (diagnose() + relocate_menu +
                       step('File data will move to a free cluster', 'esc') +
                       step('Marker change cancelled;', 'f10') +
                       step('Open image...', 'right', True) + relocate_menu +
                       step('File data will move to a free cluster', 'enter') +
                       typing('CONFIRM MARKER CHANGE', 'RELOCATE') +
                       typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'RELOC.MDJ') +
                       step('Repair verified.', 'f3') + step('Check finished.', 'f7') +
                       typing('SAVE REPORT TO A SEPARATE DISK', 'RELOCTUI.TXT') +
                       step('Report saved.', 'esc'))
    if args.relocate_directory:
        relocation_keys = relocation_keys.replace('File data will move to a free cluster', 'Directory data moves to a free cluster')
    put(volume, text_file(root, 'RELOC.KEY', relocation_keys))
    if args.relocate_native:
        native_original = (floppy if native_floppy else repair_volume).read_bytes()
        native_layout = Layout(native_original, args.mounted_bits)
        native_entry = next(o for o in native_layout.entries() if native_original[o + 11] == 32)
        if args.relocate_directory:
            native_entry = native_layout.child()
        native_cluster = native_layout.root if args.relocate_root else native_layout.cluster(native_entry)
        native_replacement = next(c for c in range(2, 1000) if native_layout.fat(c) == 0)
        native_source = native_layout.data_sector + (native_cluster - 2) * native_layout.spc
        native_dest = native_layout.data_sector + (native_replacement - 2) * native_layout.spc
        native_length = native_layout.spc * 512
        native_size = struct.unpack_from('<I', native_original, native_entry + 28)[0]
        native_payload = native_original[native_source * 512:native_source * 512 + native_size]
        short = native_original[native_entry:native_entry + 8].decode('latin1').rstrip()
        extension = native_original[native_entry + 8:native_entry + 11].decode('latin1').rstrip()
        native_name = native_drive + ':\\' + short + ('.' + extension if extension else '')

        if args.relocate_directory:
            child_entry = native_layout.child(native_cluster)
            child_cluster = native_layout.cluster(child_entry)
            data_entry = next(o for o in native_layout.entries(child_cluster) if native_original[o + 11] == 32)
            def short_name(entry):
                base = native_original[entry:entry + 8].decode('latin1').rstrip()
                ext = native_original[entry + 8:entry + 11].decode('latin1').rstrip()
                return base + ('.' + ext if ext else '')
            native_name = native_drive + ':\\' + '\\'.join(short_name(e) for e in [native_entry, child_entry, data_entry])
            data_sector = native_layout.data_sector + (native_layout.cluster(data_entry) - 2) * native_layout.spc
            size = struct.unpack_from('<I', native_original, data_entry + 28)[0]
            native_payload = native_original[data_sector * 512:data_sector * 512 + size]
        if args.relocate_root:
            payload_sector = native_layout.data_sector + (native_layout.cluster(native_entry) - 2) * native_layout.spc
            native_payload = native_original[payload_sector * 512:payload_sector * 512 + native_size]
        native_expected = bytearray(native_original)
        native_expected[native_dest * 512:native_dest * 512 + native_length] = native_original[
            native_source * 512:native_source * 512 + native_length]
        set_fat(native_expected, args.mounted_bits, native_replacement, native_layout.fat(native_cluster))
        set_fat(native_expected, args.mounted_bits, native_cluster,
                {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[args.mounted_bits])
        if not args.relocate_root:
            struct.pack_into('<H', native_expected, native_entry + 26, native_replacement & 0xffff)
        if args.relocate_directory:
            child_sector = native_layout.data_sector + (child_cluster - 2) * native_layout.spc
            for entry_offset in [native_dest * 512, child_sector * 512 + 32]:
                struct.pack_into('<H', native_expected, entry_offset + 26, native_replacement & 0xffff)
                if args.mounted_bits == 32:
                    struct.pack_into('<H', native_expected, entry_offset + 20, native_replacement >> 16)

        if args.mounted_bits == 32:
            if not args.relocate_root:
                struct.pack_into('<H', native_expected, native_entry + 20, native_replacement >> 16)
            else:
                for sector in [0, 6]:
                    struct.pack_into('<I', native_expected, sector * 512 + 44, native_replacement)
            free_count = struct.unpack_from('<I', native_original, 512 + 488)[0]
            for sector in [1, 7]:
                struct.pack_into('<II', native_expected, sector * 512 + 488, free_count - 1, 2 if args.relocate_root else 0xffffffff)
        native_sectors = {i // 512 for i in range(0, len(native_original), 512)
                          if native_original[i:i + 512] != native_expected[i:i + 512]}
        native_sectors.update(range(native_dest, native_dest + native_layout.spc))
        native_sectors = sorted(native_sectors)
        put(volume, exe.with_name('SECTGET.EXE'))


    if args.deleted_recovery:
        from deleted_fixture import fixture as deleted_fixture
        deleted_image = root / 'DELETED.IMG'
        if args.deleted_directories:
            from deleted_directory_fixture import fixture as deleted_directory_fixture
            deleted_original, deleted_expected, outer, inner = deleted_directory_fixture(deleted_image, 12, True)
            put(volume, exe.with_name('RECFAULT.EXE'))
            deleted_layout = Layout(deleted_original, 12)
            deleted_dot_sector = deleted_layout.data_sector + (outer - 2) * deleted_layout.spc
            if args.deleted_missing_dots:
                data = bytearray(deleted_original)
                layout = Layout(data, 12)
                deleted_dot_evidence = []
                for cluster, slot in [(outer, 0), (inner, 32)]:
                    first = (layout.data_sector + (cluster - 2) * layout.spc) * 512
                    data[first + slot:first + slot + 32] = bytes(32)
                    deleted_dot_evidence.append(
                        f'DIRECTORY_DOT_EVIDENCE start={cluster} self_valid={int(slot != 0)} '
                        f'parent_valid={int(slot != 32)} confidence=UNVERIFIED raw_hex=' +
                        data[first:first + 64].hex().upper())
                deleted_original = bytes(data)
                deleted_image.write_bytes(data)
        else:
            deleted_original, deleted_expected, deleted_evidence = deleted_fixture(deleted_image, 12)
        put(volume, deleted_image)
        deleted_keys = (diagnose() + step('Filesystem check', 'right', True) +
                        step('Allocation map', 'right', True) +
                        step('Save report...', 'down', True) +
                        step('Export live files...', 'down', True) +
                        step('Recover interrupted write...', 'down', True) +
                        step('Recover deleted files...', 'enter', True) +
                        step('RECOVER DELETED CANDIDATES', 'enter') +
                        typing('RECOVERY DESTINATION DIRECTORY', 'C:\\DELETED') +
                        step('DELETED-CANDIDATE RESULTS', 'enter') +
                        step('Deleted candidates exported;', 'esc'))
        put(volume, text_file(root, 'DELETED.KEY', deleted_keys))

    if args.timestamps:
        from test_timestamp_repair import fixture as timestamp_fixture
        timestamp_image = root / 'TIMES.IMG'
        timestamp_broken, timestamp_expected = timestamp_fixture(timestamp_image, 12)
        put(volume, timestamp_image)
        timestamp_keys = (diagnose() + step('Filesystem check', 'up', True) +
                          step('Clear invalid timestamps', 'enter', True) +
                          step('REVIEW INVALID TIMESTAMPS', 'enter') +
                          typing('TYPE CLEAR TO RESET INVALID FIELDS', 'CLEAR') +
                          typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\TIMES.MDJ') +
                          step('Repair verified.', 'esc'))
        put(volume, text_file(root, 'TIMES.KEY', timestamp_keys))

    if args.relocate_root:
        put(volume, exe.with_name('FILECOPY.EXE'))
        root_keys = (step('Ready. F10 opens the menu', 'f10') +
                     step('Open image...', 'down', True) + step('BIOS device...', 'down', True) +
                     step('DOS volume...', 'enter', True) + typing('OPEN DOS VOLUME (DRIVE LETTER)', native_drive) +
                     diagnose() + step('Filesystem check', 'down', True) +
                     step('Surface read', 'down', True) + step('Retest marked bad', 'down', True) +
                     step('Mark bad sector', 'enter', True) +
                     typing('MARK BAD: VOLUME SECTOR', str(native_source)) +
                     step('Root directory data moves;', 'enter') +
                     typing('CONFIRM MARKER CHANGE', 'RELOCATE') +
                     typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\NRELOC.MDJ') +
                     step('Repair verified.', 'esc'))
        put(volume, text_file(root, 'NROOT.KEY', root_keys))
    put(volume, text_file(root, 'LIVE.KEY', live_export_keys))
    warning_keys = live_export_keys.replace('Recovery finished with recorded gaps or damage.',
                                            'Recovery finished with recorded scan findings.')
    put(volume, text_file(root, 'WARN.KEY', warning_keys))
    exit_test = root / 'EXITTEST.COM'
    exit_test.write_bytes(bytes.fromhex('b4 0d cd 21 ba f4 00 b0 10 ee eb fe'))
    put(volume, exit_test)
    commands = ['@echo off', 'C:', 'cd \\', 'ver > A:\\VERSION.TXT']
    commands += ['KEYPLAY RELOC.KEY',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK --tui RELOC.IMG',
                 'copy /b RELOC.IMG RLFIX.IMG > A:\\RLCOPY.TXT',
                 'MAGICDSK RELOC.IMG --restore-journal RELOC.MDJ --apply > A:\\RLUNDO.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 f'MAGICDSK RELOC.IMG --relocate-cluster {relocation_cluster} --apply --journal RLCLI.MDJ > A:\\RLCLI.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'copy /b RELOC.IMG RLCFIX.IMG > A:\\RLCCOPY.TXT',
                 'MAGICDSK RELOC.IMG --restore-journal RLCLI.MDJ --apply > A:\\RLCUNDO.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']

    for options, report, expected in [
        ('--drive C', 'MOUNT16', 0),
        ('--drive B', 'MOUNT12', 0 if native_floppy and not args.tree else 1),
        (('LFN.IMG' if args.growth else '--drive B') + ' --export-files C:\\', 'LIVE', 0 if native_floppy and not args.tree else 1),
        ('--bios 80 --partition 1', 'BIOS16', 0),
        ('LFN.IMG', 'IMAGE12', 1),
        ('MARK.IMG --mark-bad 1000 --apply --journal MARK.MDJ', 'MARK', 0),
        ('MARK.IMG --retest-bad', 'RETEST', 0),
        ('MARK.IMG --unmark-bad 1000 --apply --journal UNMARK.MDJ', 'UNMARK', 0),
        ('MARK.IMG --restore-journal UNMARK.MDJ --apply', 'UNDONE1', 0),
        ('MARK.IMG --restore-journal MARK.MDJ --apply', 'UNDONE2', 0),
    ]:
        commands += [f'MAGICDSK {options} > A:\\{report}.TXT',
                     f'if errorlevel {expected + 1} echo FAIL >> A:\\STATUS.TXT']
        if expected:
            commands += ['if not errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if report == 'MARK':
            commands += ['KEYPLAY FIND.KEY',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                         'MAGICDSK --tui FIND.IMG']
    if args.tree:
        commands += [f'dir {tree_directory} > C:\\BEFORE.TXT']
        if args.empty_alias:
            commands += [f'dir {replacement_directory} > C:\\ABEFORE.TXT']
        if args.growth:
            commands += [f'{free_query} > A:\\GFREE0.TXT']
        for options, name in [
            ('--repair-tree', 'TPREVIEW'),
            ('--repair-tree --apply --journal C:\\TREE.MDJ', 'TAPPLY'),
        ]:
            if args.alias_ui and name == 'TAPPLY':
                commands += ['KEYPLAY EALIAS.KEY',
                             'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                             'MAGICDSK --tui']
            else:
                commands += [f'MAGICDSK --drive {native_drive} {options} > A:\\{name}.TXT',
                             'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        commands += ['MAGICDSK ' + ('--bios 1' if native_floppy else '--bios 80 --partition 2') + ' > A:\\TCHECK.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if args.empty_alias:
            commands += [native_drive + ':',
                         'cd ' + replacement_directory[2:],
                         'cd > A:\\ACWD.TXT', 'dir > C:\\AEMPTY.TXT',
                         'cd \\', 'C:']
        commands += [f'copy /b {tree_file} C:\\TREAD.BIN > A:\\TCOPY.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if args.growth:
            commands += [f'{free_query} > A:\\GFREE1.TXT']
        for index, sector in enumerate(tree_sectors):
            absolute = sector if native_floppy else volume.stat().st_size // 512 + 63 + sector
            device = '1' if native_floppy else '80'
            commands += [f'SECTGET {device} {absolute} > C:\\T{index}.BIN',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        commands += [f'MAGICDSK --drive {native_drive} --restore-journal C:\\TREE.MDJ --apply > A:\\TUNDO.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        if args.growth:
            commands += [f'{free_query} > A:\\GFREE2.TXT']
    else:
        commands += [f'{free_query} > A:\\FREE0.TXT']
        for options, name in [
            ('--mark-bad 1000 --apply --journal C:\\NMARK.MDJ', 'NMARK'),
            ('--retest-bad', 'NRETEST'),
            ('--unmark-bad 1000 --apply --journal C:\\NUNMARK.MDJ', 'NUNMARK'),
            ('--restore-journal C:\\NUNMARK.MDJ --apply', 'NUNDO1'),
            ('--restore-journal C:\\NMARK.MDJ --apply', 'NUNDO2'),
        ]:
            if args.write_protected and name != 'NMARK':
                continue
            if args.cache_workload and name.startswith('NUNDO'):
                continue
            commands += [f'MAGICDSK --drive {native_drive} {options} > A:\\{name}.TXT',
                         ('if not errorlevel 1' if args.write_protected else 'if errorlevel 1') +
                         ' echo FAIL >> A:\\STATUS.TXT']
            if name == 'NMARK' and args.cache_workload:
                commands += [f'copy C:\\PAYLOAD.BIN {native_drive}:\\CACHE.BIN > A:\\COPY.TXT',
                             f'copy {native_drive}:\\CACHE.BIN C:\\BACK.BIN >> A:\\COPY.TXT',
                             f'MAGICDSK --drive {native_drive} > A:\\CACHE.TXT',
                             'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                             f'del {native_drive}:\\CACHE.BIN']
            if name in ['NMARK', 'NUNMARK']:
                commands += [f'{free_query} > A:\\{name}F.TXT']
        commands += ['MAGICDSK ' + ('--bios 1' if native_floppy else '--bios 80 --partition 2') + ' > A:\\NBIOS.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']

    if args.relocate_native:
        commands += [f'copy /b {native_name} C:\\NRBEFORE.BIN > A:\\NRPRE.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     f'{free_query} > A:\\NRFREE0.TXT',
                     f'MAGICDSK --drive {native_drive} --relocate-cluster {native_cluster} --apply --journal C:\\NRELOC.MDJ > A:\\NRELOC.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     f'copy /b {native_name} C:\\NRAFTER.BIN > A:\\NRPOST.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     f'{free_query} > A:\\NRFREE1.TXT',
                     f'MAGICDSK --drive {native_drive} > A:\\NRCHECK.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        for index, sector in enumerate(native_sectors):
            absolute = sector if native_floppy else volume.stat().st_size // 512 + 63 + sector
            device = '1' if native_floppy else '80'
            commands += [f'SECTGET {device} {absolute} > C:\\NR{index}.BIN',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        undo_mode = ' --accept-torn' if args.relocate_torn_undo else ''
        commands += [f'MAGICDSK --drive {native_drive} --restore-journal C:\\NRELOC.MDJ --apply{undo_mode} > A:\\NRUNDO.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     f'{free_query} > A:\\NRFREE2.TXT']

    if args.retry_faults:
        for directory, mode in [('RTRY', 'transient'), ('RGAP', 'gap'), ('RSTOP', 'retry-cancel')]:
            commands += [f'md C:\\{directory}',
                         f'RECFAULT MARK.IMG C:\\{directory} {mode} live > A:\\{directory}.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
    if args.relocate_root:
        post = next(i for i, line in enumerate(commands) if 'NRAFTER.BIN' in line)
        commands[post + 2:post + 2] = [f'FILECOPY {native_name} C:\\NRAPI.BIN > A:\\NRAPI.TXT',
                                      'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
        index = next(i for i, line in enumerate(commands) if '--relocate-cluster' in line and '--drive' in line)
        commands[index:index + 1] = ['KEYPLAY NROOT.KEY',
                                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                                     'MAGICDSK --tui']
    if args.timestamps:
        commands += ['MAGICDSK TIMES.IMG --clear-invalid-timestamps > A:\\TMPRE.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     'KEYPLAY TIMES.KEY',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     'MAGICDSK --tui TIMES.IMG',
                     'MAGICDSK TIMES.IMG > A:\\TMCHECK.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     'COPY TIMES.IMG TIMEFIX.IMG > NUL',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     'MAGICDSK TIMES.IMG --restore-journal TIMES.MDJ --apply > A:\\TMUNDO.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
    if args.deleted_directories:
        for directory, mode in [('DRTRY', 'directory-transient'), ('DRREAD', 'directory-read'),
                                ('DRSTOP', 'directory-retry-cancel')]:
            commands += [f'md C:\\{directory}',
                         f'RECFAULT DELETED.IMG C:\\{directory} {mode} deleted {deleted_dot_sector} > A:\\{directory}.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT']
    if args.deleted_recovery:
        commands += ['MD C:\\DELETED',
                     'KEYPLAY DELETED.KEY',
                     'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                     'MAGICDSK --tui DELETED.IMG']
    commands += ['KEYPLAY MAIN.KEY',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK --tui LFN.IMG',
                 'KEYPLAY LIVE.KEY',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK --tui LFN.IMG',
                 'MAGICDSK TORN.IMG --restore-journal TORN.MDJ --apply > A:\\TSTRICT.TXT',
                 'if not errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'KEYPLAY TORN.KEY',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK --tui TORN.IMG',
                 'KEYPLAY SPACE.KEY',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK --tui SPACE.IMG',
                 'MAGICDSK SPACE.IMG > A:\\SCHECK.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'copy /b SPACE.IMG SFIX.IMG > A:\\SCOPY.TXT',
                 'MAGICDSK SPACE.IMG --restore-journal SPACE.MDJ --apply > A:\\SUNDO.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK TRELOC.IMG --repair-tree --apply --journal TRELOC.MDJ > A:\\TRFIX.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK TRELOC.IMG > A:\\TRCHECK.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'copy /b TRELOC.IMG TRFIX.IMG > A:\\TRCOPY.TXT',
                 'MAGICDSK TRELOC.IMG --restore-journal TRELOC.MDJ --apply > A:\\TRUNDO.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'KEYPLAY WARN.KEY',
                 'if errorlevel 1 echo FAIL >> A:\\STATUS.TXT',
                 'MAGICDSK --tui WARN.IMG',
                 'echo DONE >> A:\\STATUS.TXT', 'EXITTEST.COM']
    put(volume, text_file(root, 'RUN.BAT', '\n'.join(commands) + '\n', 'latin1'))
    config = 'FILES=60\nBUFFERS=20\nLASTDRIVE=Z\n'
    if args.kernel == 'win98':
        config = 'DEVICE=A:\\HIMEM.SYS /TESTMEM:OFF\nDOS=HIGH,UMB\n' + config
    startup = '@echo off\nC:\\RUN.BAT\n'
    if args.kernel == 'freedos':
        config += 'SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\n'
        put(boot, text_file(root, 'FDCONFIG.SYS', config))
        put(boot, text_file(root, 'FDAUTO.BAT', startup))
    else:
        put(boot, text_file(root, 'CONFIG.SYS', config))
        put(boot, text_file(root, 'AUTOEXEC.BAT', startup))
    with volume.open('r+b') as file:
        sector = bytearray(file.read(512))
        struct.pack_into('<HHI', sector, 24, 63, 16, 63)
        file.seek(0)
        file.write(sector)
    mbr = bytearray(63 * 512)
    mbr[510:512] = b'\x55\xaa'
    mbr[446] = 0x80
    mbr[450] = 6
    count = volume.stat().st_size // 512
    for offset, lba in [(447, 63), (451, count + 62)]:
        cylinder, rest = divmod(lba, 1008)
        head, sector = divmod(rest, 63)
        mbr[offset:offset + 3] = bytes([head, sector + 1 | (cylinder >> 2 & 0xc0), cylinder & 255])
    struct.pack_into('<II', mbr, 454, 63, count)
    repair_start = count + 63
    repair_count = repair_volume.stat().st_size // 512
    with repair_volume.open('r+b') as file:
        sector = bytearray(file.read(512))
        struct.pack_into('<HHI', sector, 24, 63, 16, repair_start)
        file.seek(0)
        file.write(sector)
        if args.mounted_bits == 32:
            file.seek(struct.unpack_from('<H', sector, 50)[0] * 512)
            file.write(sector)
    repair_original = repair_volume.read_bytes()
    tree_original = floppy.read_bytes() if native_floppy else repair_original
    if args.relocate_native and not native_floppy:
        native_original = repair_original
        native_expected[:512] = repair_original[:512]
        if args.mounted_bits == 32:
            native_expected[6 * 512:7 * 512] = repair_original[6 * 512:7 * 512]
        if args.relocate_root:
            for sector in [0, 6]:
                struct.pack_into('<I', native_expected, sector * 512 + 44, native_replacement)

    if args.tree and not native_floppy:
        tree_expected[:512] = repair_original[:512]
        if args.mounted_bits == 32:
            tree_expected[6 * 512:7 * 512] = repair_original[6 * 512:7 * 512]
    mbr[466] = {12: 1, 16: 6, 32: 0x0c}[args.mounted_bits]
    struct.pack_into('<II', mbr, 470, repair_start, repair_count)
    for offset, lba in [(463, repair_start), (467, repair_start + repair_count - 1)]:
        cylinder, rest = divmod(lba, 1008)
        head, sector = divmod(rest, 63)
        mbr[offset:offset + 3] = bytes([head, sector + 1 | (cylinder >> 2 & 0xc0), cylinder & 255])
    cylinders = (repair_start + repair_count + 1007) // 1008
    with disk.open('wb') as file:
        file.write(mbr)
        with volume.open('rb') as source:
            shutil.copyfileobj(source, file)
        with repair_volume.open('rb') as source:
            shutil.copyfileobj(source, file)
        file.truncate(cylinders * 1008 * 512)
    # These scenarios read file data between repair and exact undo. Pin the
    # guest date to the file's existing access date so Windows 98 does not
    # legitimately update the journaled directory sector during that read.
    rtc = []
    if args.growth or args.relocate_native:
        if args.growth:
            stamp = struct.unpack_from('<H', tree_broken, data_entry + 18)[0]
        else:
            accessed = data_entry if args.relocate_directory else native_entry
            stamp = struct.unpack_from('<H', native_original, accessed + 18)[0]
        year, month, day = 1980 + (stamp >> 9), (stamp >> 5) & 15, stamp & 31
        assert month and day
        rtc = ['-rtc', f'base={year:04d}-{month:02d}-{day:02d}T12:00:00']
    with subprocess.Popen([
        'qemu-system-i386', '-accel', 'tcg', '-m', '64', '-display', 'none',
        '-serial', 'none', '-monitor', 'stdio', '-no-reboot', '-boot', 'a',
        '-drive', f'file={boot},format=raw,if=floppy,index=0',
        '-drive', f'file={floppy},format=raw,if=floppy,index=1,readonly={"off" if native_floppy and not args.write_protected else "on"}',
        '-drive', f'file={disk},format=raw,if=none,id=systemdisk',
        '-device', f'ide-hd,drive=systemdisk,cyls={cylinders},heads=16,secs=63,bios-chs-trans=none',
        '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
    ] + rtc, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        try:
            process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            snapshot = exe.parents[2] / 'magicdisk-test' / (args.kernel + '-timeout.ppm')
            snapshot.parent.mkdir(parents=True, exist_ok=True)
            process.stdin.write(('screendump ' + str(snapshot) + '\nquit\n').encode())
            process.stdin.flush()
            process.communicate(timeout=10)
            raise AssertionError(f'QEMU timed out; screenshot: {snapshot}')
        assert process.returncode == 33, process.communicate()
    target = str(disk) + '@@' + str(63 * 512)

    def report(name):
        return command('mtype', '-i', str(boot), '::/' + name + '.TXT').stdout.decode('utf-8', errors='replace')

    print(report('VERSION'))
    status = report('STATUS')
    reports = ['LIVE', 'MOUNT16', 'MOUNT12', 'BIOS16', 'IMAGE12', 'MARK', 'RETEST',
               'UNMARK', 'UNDONE1', 'UNDONE2']
    if args.timestamps:
        reports += ['TMPRE', 'TMCHECK', 'TMUNDO']
    if args.tree:
        reports += ['TPREVIEW', 'TCHECK', 'TUNDO'] + ([] if args.alias_ui else ['TAPPLY'])
    else:
        reports += ['NMARK', 'NBIOS'] if args.write_protected else ['NMARK', 'NRETEST', 'NUNMARK', 'NBIOS']
        if not args.cache_workload and not args.write_protected:
            reports += ['NUNDO1', 'NUNDO2']
    if args.relocate_native:
        if args.relocate_root:
            reports += ['NRAPI']
        reports += ['NRPRE', 'NRPOST', 'NRCHECK', 'NRUNDO', 'NRFREE0', 'NRFREE1', 'NRFREE2']
    if args.relocate_root and 'FAIL' in status:
        for sector in [1, 7]:
            index = native_sectors.index(sector)
            data = command('mtype', '-i', target, f'::/NR{index}.BIN').stdout[-512:]
            print('Root relocation FSInfo', sector, struct.unpack_from('<II', data, 488))
    assert 'DONE' in status and 'FAIL' not in status, (
        status + '\n'.join(name + ': ' + report(name) for name in reports))
    for name in ['MOUNT16', 'BIOS16']:
        value = report(name)
        assert 'FAT16' in value and '0 issues' in value, value
    for name in (['IMAGE12'] if native_floppy else ['MOUNT12', 'IMAGE12']):
        value = report(name)
        assert UNICODE_FILE in value and 'FILE_SIZE' in value and '2 issues' in value, value
    for name in ['REC00001', 'REC00002']:
        manifest = command('mtype', '-i', target, f'::/{name}/RECOVERY.TXT').stdout.decode()
        assert 'FINAL COMPLETE' in manifest and manifest.count('LIVE_BEGIN ') == 54, manifest
        encoded = UNICODE_FILE.encode().hex().upper()
        assert encoded in manifest
        first = command('mtype', '-i', target, f'::/{name}/F0000001.CHK').stdout
        assert first.startswith(b'MagicDisk LFN preservation test.\r\n')
    assert 'no sectors restored' in report('TSTRICT'), report('TSTRICT')
    assert command('mtype', '-i', target, '::/TORN.IMG').stdout == torn_original
    assert command('mtype', '-i', target, '::/TORN.MDJ').stdout == raw_journal
    assert '0 issues' in report('SCHECK'), report('SCHECK')
    surface_report = command('mtype', '-i', target, '::/SURFACE.TXT').stdout.decode()
    assert 'Surface read: complete' in surface_report, surface_report
    finding_report = command('mtype', '-i', target, '::/FINDVIEW.TXT').stdout.decode()
    assert 'Bad-cluster retest: complete' in finding_report, finding_report
    assert 'Marked: 8  Readable: 8  Failed: 0' in finding_report, finding_report
    assert finding_report.count('BAD_READABLE ') == 8, finding_report
    assert command('mtype', '-i', target, '::/FIND.IMG').stdout == finding_original
    relocation_report = command('mtype', '-i', target, '::/RELOCTUI.TXT').stdout.decode()
    assert 'Bad clusters: 1' in relocation_report and 'Issues: 0' in relocation_report, relocation_report
    assert command('mtype', '-i', target, '::/RLFIX.IMG').stdout == relocation_expected
    assert command('mtype', '-i', target, '::/RLCFIX.IMG').stdout == relocation_expected
    assert command('mtype', '-i', target, '::/RELOC.IMG').stdout == relocation_original

    assert 'Unreadable: 0' in surface_report and 'Inspection:' not in surface_report
    assert 'Omitted findings: 0' in surface_report
    assert command('mtype', '-i', target, '::/SFIX.IMG').stdout == space_expected
    assert command('mtype', '-i', target, '::/SPACE.IMG').stdout == space_original
    assert command('mtype', '-i', target, '::/TRFIX.IMG').stdout == relocated_expected
    assert command('mtype', '-i', target, '::/TRELOC.IMG').stdout == relocated_original
    warning_manifest = command('mtype', '-i', target, '::/REC00003/RECOVERY.TXT').stdout.decode()
    assert 'FINAL COMPLETE_WITH_FINDINGS' in warning_manifest
    assert 'ISSUE code=DIR_TIMESTAMP' in warning_manifest
    assert command('mtype', '-i', target, '::/WARN.IMG').stdout == warning_data
    restored = command('mtype' , '-i', target, '::/MARK.IMG').stdout
    assert hashlib.sha256(restored).hexdigest() == marker_hash
    if args.retry_faults:
        for directory in ['RTRY', 'RGAP', 'RSTOP']:
            assert 'PASS: recovery fault handled' in report(directory), report(directory)
            prefix = f'::/{directory}/REC00001/'
            manifest = command('mtype', '-i', target, prefix + 'RECOVERY.TXT').stdout.decode()
            data = command('mtype', '-i', target, prefix + 'F0000001.CHK').stdout
            if directory == 'RTRY':
                assert 'attempt=2 result=failed' in manifest and 'attempt=3 result=readable' in manifest
                assert 'FINAL COMPLETE ' in manifest and 'UNREADABLE' not in manifest
                assert data == b'MagicDisk LFN preservation test.\r\n'
            elif directory == 'RGAP':
                assert 'attempt=3 result=failed' in manifest and 'FINAL COMPLETE_WITH_GAPS' in manifest
                assert data == bytes(len(b'MagicDisk LFN preservation test.\r\n'))
            else:
                assert 'FINAL INCOMPLETE' in manifest and 'UNREADABLE' not in manifest
                assert not data

    if args.tree:
        assert command('mtype', '-i', target, '::/TREAD.BIN').stdout == tree_payload
        if args.growth:
            free = [int(re.search(r'([0-9,]+) bytes free', report(name))[1].replace(',', ''))
                    for name in ['GFREE0', 'GFREE1', 'GFREE2']]
            assert free[0] - free[1] == len(native_added) * repair_original[13] * 512, free
            assert free[0] == free[2], free
            warmed = command('mtype', '-i', target, '::/BEFORE.TXT').stdout
            assert prewarm_token in warmed, warmed
            if args.empty_alias:
                assert replacement_directory.upper() in report('ACWD').upper(), report('ACWD')
                before_alias = command('mtype', '-i', target, '::/ABEFORE.TXT').stdout
                after_alias = command('mtype', '-i', target, '::/AEMPTY.TXT').stdout
                assert old_alias_token in before_alias and old_alias_token not in after_alias
                assert 'DIR_REPLACE' in report('TPREVIEW') and 'EMPTY directory' in report('TPREVIEW')
        if args.alias_ui:
            preview = command('mtype', '-i', target, '::/DPREV.TXT').stdout
            assert b'DIR_REPLACE' in preview and b'EMPTY directory' in preview
            checked = command('mtype', '-i', target, '::/DIRSUI.TXT').stdout
            assert b'Issues: 0' in checked, checked
        else:
            assert 'committed and verified' in report('TAPPLY'), report('TAPPLY')
        assert '0 issues' in report('TCHECK'), report('TCHECK')
        assert 'restored and verified' in report('TUNDO'), report('TUNDO')
        for index, sector in enumerate(tree_sectors):
            data = command('mtype', '-i', target, f'::/T{index}.BIN').stdout[-512:]
            assert data == tree_expected[sector * 512:(sector + 1) * 512], sector
        journal = command('mtype', '-i', target, '::/TREE.MDJ').stdout
        version, count = struct.unpack_from('<II', journal, 8)
        assert version == 8 and count == len(tree_sectors)
        for index in range(count):
            record = journal[1088 + index * 1032:1088 + (index + 1) * 1032]
            sector = struct.unpack_from('<I', record)[0]
            assert sector in tree_sectors
            assert record[4:516] == tree_original[sector * 512:(sector + 1) * 512]
            assert record[516:1028] == tree_expected[sector * 512:(sector + 1) * 512]
        if native_floppy:
            assert floppy.read_bytes() == tree_original
        else:
            with disk.open('rb') as file:
                file.seek(repair_start * 512)
                assert file.read(len(tree_original)) == tree_original
        assert digest(floppy) == floppy_hash and digest(args.boot) == original
        operation = ('independent empty directory aliases, ' if args.empty_alias else '') + ('shared-tail repair, ' if args.shared_prefix else '') + (
            'directory growth and DOS cache/free-space refresh' if args.growth else 'tree repair')
        print(f'PASS: {args.kernel} FAT{args.mounted_bits} native {operation}, independent BIOS bytes, exact journal and full-volume undo')
        raise SystemExit(0)
    if args.write_protected:
        value = report('NMARK')
        assert 'Rollback incomplete' in value or 'Change failed' in value, value
        assert digest(floppy) == floppy_hash
        assert digest(args.boot) == original
        print(f'PASS: {args.kernel} write-protected floppy fails without a prompt or changed sectors')
        raise SystemExit(0)

    if args.timestamps:
        assert report('TMPRE').count('CLEAR_TIMESTAMP') == 6
        assert command('mtype', '-i', target, '::/TIMEFIX.IMG').stdout == timestamp_expected
        assert command('mtype', '-i', target, '::/TIMES.IMG').stdout == timestamp_broken
        print('PASS: timestamp CLI preview, TUI correction, clean scan and exact journal undo')

    if args.deleted_recovery:
        from test_deleted_recovery import verify as verify_deleted
        recovered = root / 'deleted-output'
        recovered.mkdir()
        command('mcopy', '-s', '-i', target, '::/DELETED/REC00001', str(recovered))
        if args.deleted_directories:
            from test_deleted_directories import verify as verify_deleted_directories
            verify_deleted_directories(recovered / 'REC00001', deleted_expected)
            for directory, state in [('DRTRY', 'readable'), ('DRREAD', 'unreadable'),
                                     ('DRSTOP', 'cancelled')]:
                assert 'PASS: recovery fault handled' in report(directory)
                retry_output = root / directory
                retry_output.mkdir()
                command('mcopy', '-s', '-i', target, f'::/{directory}/REC00001', str(retry_output))
                manifest = (retry_output / 'REC00001' / 'RECOVERY.TXT').read_text()
                if state == 'cancelled':
                    assert 'FINAL INCOMPLETE' in manifest
                else:
                    assert f'DIRECTORY_RETRY entry_lba={deleted_dot_sector} attempts=3 state={state}' in manifest
                    if state == 'readable':
                        verify_deleted_directories(retry_output / 'REC00001', deleted_expected)
                    else:
                        assert 'stop=unreadable-dot-sector' in manifest and 'DELETED_BEGIN ' not in manifest
            if args.deleted_missing_dots:
                manifest = (recovered / 'REC00001' / 'RECOVERY.TXT').read_text()
                for evidence in deleted_dot_evidence:
                    assert evidence in manifest
        else:
            verify_deleted(recovered / 'REC00001', deleted_expected, deleted_evidence)
        assert command('mtype', '-i', target, '::/DELETED.IMG').stdout == deleted_original
        print('PASS: deleted-directory TUI traversal, nested files and source unchanged' if args.deleted_directories
              else 'PASS: deleted-file TUI export, retained/free chains, live exclusion, raw LFN and source unchanged')

    def free_bytes(name):
        match = re.search(r'([0-9,]+) bytes free', report(name))
        assert match, report(name)
        return int(match[1].replace(',', ''))

    if args.relocate_native:
        free = [free_bytes(n) for n in ['NRFREE0', 'NRFREE1', 'NRFREE2']]
        assert free[0] - free[1] == native_length and free[0] == free[2], free
        assert '1 bad' in report('NRCHECK') and '0 issues' in report('NRCHECK'), report('NRCHECK')
        if args.relocate_root:
            print(report('NRAPI'))
            assert command('mtype', '-i', target, '::/NRAPI.BIN').stdout == native_payload
        for name in ['NRBEFORE.BIN', 'NRAFTER.BIN']:
            assert command('mtype', '-i', target, '::/' + name).stdout == native_payload
        for index, sector in enumerate(native_sectors):
            data = command('mtype', '-i', target, f'::/NR{index}.BIN').stdout[-512:]
            assert data == native_expected[sector * 512:(sector + 1) * 512], sector
        journal = command('mtype', '-i', target, '::/NRELOC.MDJ').stdout
        version, count = struct.unpack_from('<II', journal, 8)
        assert version == (13 if args.relocate_root else 12 if args.relocate_directory else 11) and count == len(native_sectors)
        for index in range(count):
            record = journal[1088 + index * 1032:1088 + (index + 1) * 1032]
            sector = struct.unpack_from('<I', record)[0]
            assert sector in native_sectors
            assert record[4:516] == native_original[sector * 512:(sector + 1) * 512]
            assert record[516:1028] == native_expected[sector * 512:(sector + 1) * 512]
        print(f'PASS: {args.kernel} FAT{args.mounted_bits} mounted relocation, DOS cached payload/free space, BIOS sectors and journal')
    before, marked, unmarked = [free_bytes(n) for n in ['FREE0', 'NMARKF', 'NUNMARKF']]
    assert before - marked == repair_original[13] * 512, (before, marked, unmarked, repair_original[13])
    assert before == unmarked
    assert '1 marked, 1 readable' in report('NRETEST'), report('NRETEST')
    assert '0 issues' in report('NBIOS'), report('NBIOS')
    with disk.open('rb') as file:
        file.seek(repair_start * 512)
        actual = file.read(len(repair_original))
    if args.cache_workload:
        back = command('mtype', '-i', target, '::/BACK.BIN').stdout
        assert back == payload.read_bytes(), 'DOS allocation/readback damaged file contents'
        assert '1 bad' in report('CACHE') and '0 issues' in report('CACHE'), report('CACHE')
    else:
        assert (floppy.read_bytes() == marker.read_bytes() if native_floppy else actual == repair_original), 'Native repair undo did not restore the volume exactly'
    if not (native_floppy and args.cache_workload):
        assert digest(floppy) == floppy_hash
    assert digest(args.boot) == original
    print(f'PASS: native FAT{args.mounted_bits} marker/retest/unmark and ' +
          ('DOS allocation/readback, cache refresh' if args.cache_workload else 'exact volume undo'))
    print(f'PASS: {args.kernel} QEMU mounted FAT12/FAT16, BIOS, LFN, TUI, image mark/retest/unmark/undo; original boot unchanged')
