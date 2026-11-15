#!/usr/bin/env python3
"""Fully shared directory aliases become independent empty directories, with undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import Layout, command, PARENT, CHILD, UNICODE_FILE
from shared_empty_fixture import shared_empty_fixture
from source_fixture import rebuilt_info
from chain_fixture import set_fat
from growth_fixture import growth_fixture, NAME

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-shared-empty-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        for nested in [False, True]:
            original, fixed, added, aliases, owner_entry = shared_empty_fixture(path, bits, 2, nested)
            layout = Layout(original, bits)
            path.write_bytes(original)
            unused = root / f'chain-only-{bits}-{nested}.mdj'
            run(path, '--repair-directories', '--apply', '--journal', str(unused), code=2)
            assert path.read_bytes() == original and not unused.exists()
            for source in [0, 1, 2]:
                damaged, expected = bytearray(original), bytearray(fixed)
                options = ['--repair-tree']
                if source:
                    start = (layout.reserved + (2 - source) * layout.fatsz) * 512
                    damaged[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                    expected = rebuilt_info(expected, bits)
                    options += ['--fat-source', str(source)]
                path.write_bytes(damaged)
                preview = run(path, *options)
                assert preview.count('DIR_REPLACE') == 2 and preview.count('EMPTY directory') == 2
                assert path.read_bytes() == damaged
                journal = root / f'{bits}-{nested}-{source}.mdj'
                run(path, *options, '--apply', '--journal', str(journal))
                assert path.read_bytes() == expected
                run(path)
                assert command('mtype', '-i', str(path), f'::/{PARENT}/{CHILD}/{UNICODE_FILE}').stdout == b'MagicDisk LFN preservation test.\r\n'
                final = Layout(expected, bits)
                for entry in aliases:
                    records = list(final.entries(final.cluster(entry)))
                    assert len(records) == 2
                    assert expected[records[0]:records[0] + 11] == b'.          '
                    assert expected[records[1]:records[1] + 11] == b'..         '
                check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
                assert check.returncode == 0, check.stdout + check.stderr
                raw = journal.read_bytes()
                count = struct.unpack_from('<I', raw, 12)[0]
                sectors = {struct.unpack_from('<I', raw, 1088 + i * 1032)[0] for i in range(count)}
                for cluster in added:
                    first = layout.data_sector + (cluster - 2) * layout.spc
                    assert all(sector in sectors for sector in range(first, first + layout.spc))
                # An unrelated new use of the allocated directory prevents undo.
                changed = bytearray(expected)
                changed[(layout.data_sector + (added[-1] - 1) * layout.spc) * 512 - 1] ^= 1
                path.write_bytes(changed)
                run(path, '--restore-journal', str(journal), '--apply', code=2)
                assert path.read_bytes() == changed
                path.write_bytes(expected)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == damaged
                for n in [1, count // 2, count - 1]:
                    partial = bytearray(damaged)
                    for i in range(n):
                        record = raw[1088 + i * 1032:1088 + (i + 1) * 1032]
                        sector = struct.unpack_from('<I', record)[0]
                        partial[sector * 512:(sector + 1) * 512] = record[516:1028]
                    path.write_bytes(partial)
                    run(path, '--restore-journal', str(journal), '--apply')
                    assert path.read_bytes() == damaged
        # Replace a fully shared child whose parent has already relocated its
        # long-name group into newly allocated directory space.
        moved, moved_fixed, parent_added = growth_fixture(path, bits)
        moved_layout = Layout(moved, bits)
        owner = moved_layout.cluster(moved_layout.child())
        _, parent_entry = next((parts, entry) for parts, entry in moved_layout.long_names(moved_layout.root)
                               if moved_layout.decoded_name(parts) == 'Empty')
        parent = moved_layout.cluster(parent_entry)
        _, child_entry = next((parts, entry) for parts, entry in moved_layout.long_names(parent)
                              if moved_layout.decoded_name(parts) == NAME)
        orphan = moved_layout.cluster(child_entry)
        struct.pack_into('<H', moved, child_entry + 26, owner & 0xffff)
        struct.pack_into('<H', moved_fixed, child_entry + 26, owner & 0xffff)
        if bits == 32:
            struct.pack_into('<H', moved, child_entry + 20, owner >> 16)
            struct.pack_into('<H', moved_fixed, child_entry + 20, owner >> 16)
        relocated = Layout(moved_fixed, bits)
        _, new_entry = next((parts, entry) for parts, entry in relocated.long_names(parent)
                            if relocated.decoded_name(parts) == NAME)
        total = (len(moved) // 512 - moved_layout.data_sector) // moved_layout.spc
        new_cluster = next(c for c in range(2, total + 2)
                           if not moved_layout.fat(c) and c not in parent_added)
        first = (moved_layout.data_sector + (new_cluster - 2) * moved_layout.spc) * 512
        moved[first:first + moved_layout.spc * 512] = b'\x5a' * (moved_layout.spc * 512)
        moved_fixed[first:first + moved_layout.spc * 512] = bytes(moved_layout.spc * 512)
        set_fat(moved_fixed, bits, new_cluster, {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits])
        struct.pack_into('<H', moved_fixed, new_entry + 26, new_cluster & 0xffff)
        if bits == 32:
            struct.pack_into('<H', moved_fixed, new_entry + 20, new_cluster >> 16)
        for index, target in [(0, new_cluster), (1, parent)]:
            offset = first + index * 32
            moved_fixed[offset:offset + 11] = b'.          ' if not index else b'..         '
            moved_fixed[offset + 11] = 16
            struct.pack_into('<H', moved_fixed, offset + 26, target & 0xffff)
            if bits == 32:
                struct.pack_into('<H', moved_fixed, offset + 20, target >> 16)
        # The old child's now-orphaned metadata is never repaired or overwritten.
        old = (moved_layout.data_sector + (orphan - 2) * moved_layout.spc) * 512
        moved_fixed[old:old + 512] = moved[old:old + 512]
        if bits == 32:
            info, backup = struct.unpack_from('<HH', moved, 48)
            for sector in [info, backup + info]:
                free = struct.unpack_from('<I', moved_fixed, sector * 512 + 488)[0]
                struct.pack_into('<I', moved_fixed, sector * 512 + 488, free - 1)
        path.write_bytes(moved)
        journal = root / f'moved-alias-{bits}.mdj'
        preview = run(path, '--repair-tree', '--apply', '--journal', str(journal))
        assert 'DIR_REPLACE' in preview and 'DIR_GROW' in preview
        assert path.read_bytes() == moved_fixed
        assert 'LOST_CLUSTERS' in run(path, code=1)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == moved
        full = bytearray(original)
        total = (len(full) // 512 - layout.data_sector) // layout.spc
        for cluster in range(2, total + 2):
            if not layout.fat(cluster):
                set_fat(full, bits, cluster, {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits])
        path.write_bytes(full)
        unused = root / f'full-{bits}.mdj'
        run(path, '--repair-tree', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == full and not unused.exists()
    # At the FAT12/16 type boundary, free-looking entries whose cluster
    # numbers are reserved markers cannot be allocated as directory chains.
    for bits, clusters, fatsz in [(12, 4084, 12), (16, 65524, 256)]:
        root_sector = 1 + 2 * fatsz
        data_sector = root_sector + 32
        total = data_sector + clusters
        data = bytearray(total * 512)
        data[0:3] = b'\xeb\x3c\x90'
        data[3:11] = b'MSDOS5.0'
        struct.pack_into('<H', data, 11, 512)
        data[13] = 1
        struct.pack_into('<H', data, 14, 1)
        data[16] = 2
        struct.pack_into('<H', data, 17, 512)
        struct.pack_into('<H' if total < 65536 else '<I', data, 19 if total < 65536 else 32, total)
        data[21] = 0xf0 if bits == 12 else 0xf8
        struct.pack_into('<H', data, 22, fatsz)
        data[510:512] = b'\x55\xaa'
        end, reserved = (0xfff, 0xff0) if bits == 12 else (0xffff, 0xfff0)
        for cluster, value in [(0, (end & ~255) | data[21]), (1, end), (2, end)]:
            set_fat(data, bits, cluster, value)
        for cluster in range(3, reserved):
            set_fat(data, bits, cluster, end - 8)
        for index, name in enumerate([b'OWNER      ', b'ALIAS      ']):
            offset = root_sector * 512 + index * 32
            data[offset:offset + 11] = name
            data[offset + 11] = 16
            struct.pack_into('<H', data, offset + 26, 2)
        for index, name in enumerate([b'.          ', b'..         ']):
            offset = data_sector * 512 + index * 32
            data[offset:offset + 11] = name
            data[offset + 11] = 16
            struct.pack_into('<H', data, offset + 26, 2 if not index else 0)
        path = root / f'reserved{bits}.img'
        path.write_bytes(data)
        unused = root / f'reserved{bits}.mdj'
        refused = run(path, '--repair-tree', '--apply', '--journal', str(unused), code=2)
        assert 'No unowned free cluster' in refused
        assert path.read_bytes() == data and not unused.exists()
print('PASS: FAT12/16/32 independent empty aliases, original-owner contents, nested parents, selected FAT, free-space/undo guards and full-volume refusal')
