#!/usr/bin/env python3
"""Leading dot pairs: preserve the usable named entry, rebuild absent metadata."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, Layout
from entry_fixture import damaged_dot_pair_fixture
from directory_chain_fixture import directory_chain_fixture
from source_fixture import rebuilt_info

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


def new_dot(name, cluster, bits):
    entry = bytearray(32)
    entry[:11] = name
    entry[11] = 16
    struct.pack_into('<H', entry, 26, cluster & 0xffff)
    if bits == 32:
        struct.pack_into('<H', entry, 20, cluster >> 16)
    return entry


# Expected survivors are listed explicitly; None means new, dateless metadata.
cases = [('self', 'self', (0, None)), ('parent', 'parent', (None, 1)),
         ('parent', 'self', (1, 0)), ('deleted', 'self', (1, None)),
         ('parent', 'deleted', (None, 0)), ('self', 'deleted', (0, None)),
         ('deleted', 'parent', (None, 1)), ('deleted', 'deleted', (None, None))]
with tempfile.TemporaryDirectory(prefix='magicdisk-dot-pairs-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        parent = layout.cluster(layout.child())
        prefix = (layout.data_sector + (parent - 2) * layout.spc) * 512
        for case, (first, second, survivors) in enumerate(cases):
            original = []
            for index, kind in enumerate([first, second]):
                slot = new_dot(b'.          ' if kind == 'self' else b'..         ',
                               parent if kind == 'self' else 0, bits)
                slot[13] = 17 + index
                for field in [16, 18, 24]:
                    struct.pack_into('<H', slot, field, 0x5821 + index)
                if bits != 32:
                    struct.pack_into('<H', slot, 20, 0x4321 + index)
                if kind == 'deleted':
                    slot[0] = 0xe5
                original.append(slot)
            broken, expected = bytearray(layout.data), bytearray(layout.data)
            broken[prefix:prefix + 64] = original[0] + original[1]
            for index, source in enumerate(survivors):
                entry = original[source] if source is not None else new_dot(
                    b'.          ' if index == 0 else b'..         ', parent if index == 0 else 0, bits)
                expected[prefix + index * 32:prefix + (index + 1) * 32] = entry
            for operation in ['--repair-directory-entries', '--repair-tree']:
                path.write_bytes(broken)
                journal = root / f'{bits}-{case}-{operation[2:]}.mdj'
                preview = run(path, operation)
                assert 'ENTRY_REPAIR' in preview
                assert path.read_bytes() == broken
                run(path, operation, '--apply', '--journal', str(journal))
                assert path.read_bytes() == expected, (bits, first, second, operation)
                run(path)
                check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
                assert check.returncode == 0, check.stdout + check.stderr
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == broken
        # Combine both new cases with metadata and chain damage, including FAT authority.
        for source in [False, True]:
            broken, expected = damaged_dot_pair_fixture(layout.data, bits)
            broken, _, _, _ = directory_chain_fixture(broken, bits)
            _, expected, _, _ = directory_chain_fixture(expected, bits)
            options = ['--repair-tree']
            if source:
                start = layout.reserved * 512
                broken[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                options += ['--fat-source', '2']
                expected = rebuilt_info(expected, bits)
            path.write_bytes(broken)
            journal = root / f'mixed{bits}-{source}.mdj'
            preview = run(path, *options, '--apply', '--journal', str(journal))
            assert preview.count('replace duplicate dot entry') == 1
            assert preview.count('restore dot/dotdot order') == 1
            assert preview.count('rebuild 1 missing dot entry with unset timestamps') == 2
            assert path.read_bytes() == expected
            run(path)
            raw = journal.read_bytes()
            count = struct.unpack_from('<I', raw, 12)[0]
            for completed in [0, count // 2, count]:
                partial = bytearray(broken)
                for index in range(completed):
                    record = raw[1088 + index * 1032:1088 + (index + 1) * 1032]
                    sector = struct.unpack_from('<I', record)[0]
                    partial[sector * 512:(sector + 1) * 512] = record[516:1028]
                path.write_bytes(partial)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == broken

print('PASS: FAT12/16/32 duplicate/moved/missing dot combinations, timestamp identity, both repair actions, combined/source damage and partial undo')
