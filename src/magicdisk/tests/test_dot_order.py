#!/usr/bin/env python3
"""Reorder the leading dot pair without swapping its timestamps or touching names."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, Layout, command
from entry_fixture import reversed_dot_fixture
from directory_chain_fixture import directory_chain_fixture
from source_fixture import rebuilt_info

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-dot-order-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        clean = bytearray(layout.data)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        for cluster in [parent, child]:
            prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
            for index in range(2):
                offset = prefix + index * 32
                clean[offset + 13] = 7 + index * 10
                # Distinct valid date values follow each named entry when moved.
                for field in [16, 18, 24]:
                    struct.pack_into('<H', clean, offset + field, 0x5821 + index)
                if bits != 32:
                    struct.pack_into('<H', clean, offset + 20, 0x1234 + index)
        for mode in ['entries', 'tree', 'source']:
            broken, expected = reversed_dot_fixture(clean, bits)
            options = ['--repair-directory-entries'] if mode == 'entries' else ['--repair-tree']
            if mode != 'entries':
                broken, _, _, _ = directory_chain_fixture(broken, bits)
                _, expected, _, _ = directory_chain_fixture(expected, bits)
            if mode == 'source':
                start = layout.reserved * 512
                broken[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                options += ['--fat-source', '2']
                expected = rebuilt_info(expected, bits)
            path.write_bytes(broken)
            preview = run(path, *options)
            assert preview.count('restore dot/dotdot order and correct metadata') == 2
            assert path.read_bytes() == broken
            journal = root / f'{bits}-{mode}.mdj'
            run(path, *options, '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path)
            check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
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
        # A dot beyond the leading pair is a separate structural error, even
        # when the first pair is reversed and otherwise repairable.
        path.write_bytes(layout.data)
        command('mmd', '-i', str(path), '::/Empty')
        empty = Layout(path.read_bytes(), bits)
        entry = next(p for p in empty.entries() if empty.data[p:p + 11] == b'EMPTY      ')
        cluster = empty.cluster(entry)
        prefix = (empty.data_sector + (cluster - 2) * empty.spc) * 512
        invalid = bytearray(empty.data)
        invalid[prefix:prefix + 32], invalid[prefix + 32:prefix + 64] = invalid[prefix + 32:prefix + 64], invalid[prefix:prefix + 32]
        invalid[prefix + 64:prefix + 96] = empty.data[prefix:prefix + 32]
        path.write_bytes(invalid)
        findings = run(path, code=1)
        assert 'DIR_DOT_ORDER' in findings and 'DIR_DOT_POSITION' in findings
        unused = root / 'unused.mdj'
        for operation in ['--repair-directory-entries', '--repair-tree']:
            run(path, operation, '--apply', '--journal', str(unused), code=2)
            assert path.read_bytes() == invalid and not unused.exists()

print('PASS: FAT12/16/32 reversed dot pairs, timestamp/reserved-field identity, combined/source repairs, partial undo and late-dot refusal')
