#!/usr/bin/env python3
"""Vacant dot slots may be rebuilt; advancing an end marker cannot expose entries."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, Layout, command
from directory_chain_fixture import directory_chain_fixture
from entry_fixture import missing_dot_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-missing-dots-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        missing, rebuilt = missing_dot_fixture(layout.data, bits)
        for mode in ['entries', 'tree', 'source']:
            broken, expected = bytearray(missing), bytearray(rebuilt)
            options = ['--repair-directory-entries'] if mode == 'entries' else ['--repair-tree']
            if mode != 'entries':
                # Directory prefixes and missing dots need repair at the same time.
                broken, _, _, _ = directory_chain_fixture(broken, bits)
                _, expected, _, _ = directory_chain_fixture(expected, bits)
            if mode == 'source':
                start = layout.reserved * 512
                broken[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                options += ['--fat-source', '2']
                from source_fixture import rebuilt_info
                expected = rebuilt_info(expected, bits)
            path.write_bytes(broken)
            preview = run(path, *options)
            assert preview.count('rebuild 2 missing dot entries with unset timestamps') == 2
            assert 'Pasta de projetos/Relatórios longos' in preview
            assert path.read_bytes() == broken
            journal = root / f'{bits}-{mode}.mdj'
            run(path, *options, '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path)
            checked = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
            assert checked.returncode == 0, checked.stdout + checked.stderr
            raw = journal.read_bytes()
            count = struct.unpack_from('<I', raw, 12)[0]
            for n in sorted(set([0, 1, count // 2, count])):
                partial = bytearray(broken)
                for index in range(n):
                    record = raw[1088 + index * 1032:1088 + (index + 1) * 1032]
                    sector = struct.unpack_from('<I', record)[0]
                    partial[sector * 512:(sector + 1) * 512] = record[516:1028]
                path.write_bytes(partial)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == broken

        # Start from a genuinely empty subdirectory for end-marker cases.
        path.write_bytes(layout.data)
        command('mmd', '-i', str(path), '::/Empty')
        empty = Layout(path.read_bytes(), bits)
        entry = next(pos for pos in empty.entries() if empty.data[pos:pos + 11] == b'EMPTY      ')
        cluster = empty.cluster(entry)
        prefix = (empty.data_sector + (cluster - 2) * empty.spc) * 512
        assert empty.data[prefix + 64] == 0
        for case, markers in enumerate([(0xe5, None), (None, 0xe5), (0xe5, 0xe5), (0, 0), (None, 0), (0xe5, 0), (0, 0xe5)]):
            broken, expected = bytearray(empty.data), bytearray(empty.data)
            for index, marker in enumerate(markers):
                if marker is None:
                    continue
                offset = prefix + index * 32
                broken[offset] = marker
                expected[offset:offset + 32] = bytes(32)
                expected[offset:offset + 11] = b'.          ' if not index else b'..         '
                expected[offset + 11] = 16
                target = cluster if not index else 0
                struct.pack_into('<H', expected, offset + 26, target & 0xffff)
                if bits == 32:
                    struct.pack_into('<H', expected, offset + 20, target >> 16)
            path.write_bytes(broken)
            journal = root / f'empty{bits}-{case}.mdj'
            run(path, '--repair-directory-entries', '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path)
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == broken
        for hidden in [32, 64]:
            broken = bytearray(empty.data)
            broken[prefix] = broken[prefix + 32] = 0
            broken[prefix + hidden:prefix + hidden + 11] = b'HIDDEN  TXT'
            broken[prefix + hidden + 11] = 32
            path.write_bytes(broken)
            unused = root / 'unused.mdj'
            for operation in ['--repair-directory-entries', '--repair-tree']:
                run(path, operation, '--apply', '--journal', str(unused), code=2)
                assert path.read_bytes() == broken and not unused.exists()
        # The entry action may relocate a short file, but never discard its bytes.
        occupied = bytearray(empty.data)
        occupied[prefix:prefix + 64] = bytes(64)
        occupied[prefix:prefix + 11] = b'KEEP    TXT'
        occupied[prefix + 11] = 32
        path.write_bytes(occupied)
        run(path, '--repair-tree')
        run(path, '--repair-directory-entries')
        assert path.read_bytes() == occupied
        journal = root / f'occupied{bits}.mdj'
        run(path, '--repair-directory-entries', '--apply', '--journal', str(journal))
        expected = bytearray(occupied)
        expected[prefix + 64:prefix + 96] = occupied[prefix:prefix + 32]
        expected[prefix:prefix + 64] = bytes(64)
        for index, target in [(0, cluster), (1, 0)]:
            offset = prefix + index * 32
            expected[offset:offset + 11] = b'.          ' if not index else b'..         '
            expected[offset + 11] = 16
            struct.pack_into('<H', expected, offset + 26, target & 0xffff)
            if bits == 32:
                struct.pack_into('<H', expected, offset + 20, target >> 16)
        assert path.read_bytes() == expected
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == occupied

print('PASS: FAT12/16/32 deleted/empty dot slots, nested parents, unset dates, combined/source repairs, exact undo and hidden-entry guards')
