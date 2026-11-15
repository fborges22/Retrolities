#!/usr/bin/env python3
"""Explicit FAT authority plus damaged tree: exact repair and guarded undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create
from chain_fixture import chain_fixture
from directory_chain_fixture import directory_chain_fixture
from source_fixture import rebuilt_info
from entry_fixture import entry_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-source-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        entry_damage, _ = entry_fixture(layout.data, bits)
        file_damage, _, _ = chain_fixture(entry_damage, bits)
        _, file_fixed, _ = chain_fixture(layout.data, bits)
        tree_damage, _, _, _ = directory_chain_fixture(file_damage, bits)
        _, expected, _, _ = directory_chain_fixture(file_fixed, bits)
        expected = rebuilt_info(expected, bits)
        for source in [1, 2]:
            broken = bytearray(tree_damage)
            other = 2 - source
            start = (layout.reserved + other * layout.fatsz) * 512
            broken[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
            path.write_bytes(broken)
            run(path, '--repair-tree', code=2)
            run(path, '--repair-fat', str(source), code=2)
            options = ['--repair-tree', '--fat-source', str(source)]
            preview = run(path, *options)
            assert 'FAT_SOURCE' in preview and preview.count('FILE_REPAIR') == 9
            assert preview.count('ENTRY_REPAIR') == 2
            assert preview.count('DIR_REPAIR') == (3 if bits == 32 else 2)
            assert path.read_bytes() == broken
            unused = root / 'unused.mdj'
            run(path, '--repair-tree', '--fat-source', str(3 - source), '--apply',
                '--journal', str(unused), code=2)
            assert path.read_bytes() == broken and not unused.exists()
            journal = root / f'source{source}-{bits}.mdj'
            run(path, *options, '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path)
            check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 9
            assert struct.unpack_from('<I', raw, 32)[0] == source
            count = struct.unpack_from('<I', raw, 12)[0]
            run(path, *options, '--apply', '--journal', str(unused))
            assert not unused.exists()
            run(path, '--restore-journal', str(journal), '--apply')
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == broken
            for n in sorted(set([1, count // 2, count - 1])):
                partial = bytearray(broken)
                for index in range(n):
                    record = raw[1088 + index * 1032:1088 + (index + 1) * 1032]
                    sector = struct.unpack_from('<I', record)[0]
                    partial[sector * 512:(sector + 1) * 512] = record[516:1028]
                path.write_bytes(partial)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == broken
            for fault in ['source', 'data']:
                bad = bytearray(raw)
                if fault == 'source':
                    struct.pack_into('<I', bad, 32, 3 - source)
                    struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
                else:
                    bad[1088 + 516 + 100] ^= 1
                    struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
                invalid = root / 'invalid.mdj'
                invalid.write_bytes(bad)
                run(path, '--restore-journal', str(invalid), '--apply', code=2)
                assert path.read_bytes() == broken
        # Mirroring alone is valid even when there are zero tree corrections.
        broken = bytearray(layout.data)
        start = (layout.reserved + layout.fatsz) * 512
        broken[start:start + layout.fatsz * 512] = bytes(layout.fatsz * 512)
        path.write_bytes(broken)
        journal = root / f'mirror{bits}.mdj'
        run(path, '--repair-tree', '--fat-source', '1', '--apply', '--journal', str(journal))
        assert path.read_bytes() == rebuilt_info(layout.data, bits)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        run(path, '--fat-source', '1', code=2)

print('PASS: FAT12/16/32 selected-source tree repair, both authorities, exact writes/fsck, large journals and partial undo')
