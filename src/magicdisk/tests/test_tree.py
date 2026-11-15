#!/usr/bin/env python3
"""Combined chain repairs and ownership-validated version-eight undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, UNICODE_FILE
from chain_fixture import chain_fixture, set_fat
from directory_chain_fixture import directory_chain_fixture
from entry_fixture import entry_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-tree-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        entry_damage, _ = entry_fixture(layout.data, bits)
        path.write_bytes(entry_damage)
        entry_journal = root / f'entry-only{bits}.mdj'
        entry_preview = run(path, '--repair-tree', '--apply', '--journal', str(entry_journal))
        assert 'Repair 2 files/directories' in entry_preview
        assert entry_preview.count('ENTRY_REPAIR') == 2
        assert 'FILE_REPAIR' not in entry_preview and 'DIR_REPAIR' not in entry_preview
        assert path.read_bytes() == layout.data
        run(path, '--restore-journal', str(entry_journal), '--apply')
        assert path.read_bytes() == entry_damage
        file_damage, _, entries = chain_fixture(entry_damage, bits)
        _, file_fixed, _ = chain_fixture(layout.data, bits)
        broken, _, starts, ends = directory_chain_fixture(file_damage, bits)
        _, expected, _, _ = directory_chain_fixture(file_fixed, bits)
        path.write_bytes(broken)
        run(path, '--repair-files', code=2)
        run(path, '--repair-directories', code=2)
        run(path, '--repair-directory-entries', code=2)
        preview = run(path, '--repair-tree')
        assert preview.count('ENTRY_REPAIR') == 2
        assert f'Repair {9 + len(starts)} files/directories' in preview
        assert preview.count('FILE_REPAIR') == 9 and preview.count('DIR_REPAIR') == len(starts)
        assert path.read_bytes() == broken
        journal = root / f'tree{bits}.mdj'
        run(path, '--repair-tree', '--apply', '--journal', str(journal))
        assert path.read_bytes() == expected
        run(path)
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        raw = journal.read_bytes()
        assert struct.unpack_from('<I', raw, 8)[0] == 8
        count = struct.unpack_from('<I', raw, 12)[0]
        records = [raw[1088 + n * 1032:1088 + (n + 1) * 1032] for n in range(count)]
        unused = root / f'unused{bits}.mdj'
        assert 'already up to date' in run(path, '--repair-tree', '--apply', '--journal', str(unused))
        assert not unused.exists()
        run(path, '--restore-journal', str(journal), '--apply')
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # Any prefix of completed writes can be restored in one transaction.
        for n in range(1, count):
            partial = bytearray(broken)
            for record in records[:n]:
                sector = struct.unpack_from('<I', record)[0]
                partial[sector * 512:(sector + 1) * 512] = record[516:1028]
            path.write_bytes(partial)
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == broken
        # A valid checksum does not authorize arbitrary directory/data changes.
        bad = bytearray(raw)
        bad[1088 + 516 + 100] ^= 1
        struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
        invalid = root / f'invalid{bits}.mdj'
        invalid.write_bytes(bad)
        run(path, '--restore-journal', str(invalid), '--apply', code=2)
        assert path.read_bytes() == broken
        # A later directory-owner change outside the journal blocks undo.
        changed = bytearray(expected)
        parent_entry = layout.child()
        struct.pack_into('<H', changed, parent_entry + 26, starts[1])
        path.write_bytes(changed)
        run(path, '--restore-journal', str(journal), '--apply', code=2)
        assert path.read_bytes() == changed
        shared = bytearray(broken)
        set_fat(shared, bits, ends[1], starts[0])
        path.write_bytes(shared)
        shared_journal = root / f'shared{bits}.mdj'
        run(path, '--repair-tree', '--apply', '--journal', str(shared_journal))
        assert path.read_bytes() == expected
        run(path, '--restore-journal', str(shared_journal), '--apply')
        assert path.read_bytes() == shared

        for corruption in ['missing', 'misplaced']:
            invalid_data = bytearray(broken)
            prefix = (layout.data_sector + (starts[0] - 2) * layout.spc) * 512
            if corruption == 'missing':
                invalid_data[prefix] = ord('X')
            else:
                invalid_data[prefix:prefix + 11] = b'KEEP    TXT'
            path.write_bytes(invalid_data)
            run(path, '--repair-tree', '--apply', '--journal', str(unused), code=2)
            assert path.read_bytes() == invalid_data and not unused.exists()

        if bits == 32:
            active, fixed = bytearray(broken), bytearray(expected)
            for sector in [0, 6]:
                struct.pack_into('<H', active, sector * 512 + 40, 0x81)
                struct.pack_into('<H', fixed, sector * 512 + 40, 0x81)
            first, second = layout.reserved * 512, (layout.reserved + layout.fatsz) * 512
            fixed[first:second] = active[first:second]
            path.write_bytes(active)
            active_journal = root / 'active.mdj'
            run(path, '--repair-tree', '--apply', '--journal', str(active_journal))
            assert path.read_bytes() == fixed
            run(path)
            run(path, '--restore-journal', str(active_journal), '--apply')
            assert path.read_bytes() == active
            mbr = bytearray(63 * 512)
            mbr[510:512] = b'\x55\xaa'
            mbr[450] = 12
            struct.pack_into('<II', mbr, 454, 63, len(broken) // 512)
            path.write_bytes(mbr + broken)
            shifted_journal = root / 'shifted.mdj'
            run(path, '--partition', '1', '--repair-tree', '--apply', '--journal', str(shifted_journal))
            assert path.read_bytes() == mbr + expected
            run(path, '--partition', '1', '--restore-journal', str(shifted_journal), '--apply')
            assert path.read_bytes() == mbr + broken

print('PASS: combined FAT12/16/32 file/directory repair, exact sectors, fsck, partial/repeated undo and guarded journals')
