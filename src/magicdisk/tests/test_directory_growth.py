#!/usr/bin/env python3
"""Directory extension journals allocation, records, hints and original free bytes."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import Layout, command
from growth_fixture import growth_fixture, NAME
from dot_lfn_fixture import PAYLOAD
from source_fixture import rebuilt_info
from chain_fixture import set_fat

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-growth-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        original, fixed, added = growth_fixture(path, bits)
        layout = Layout(original, bits)
        for source in [None, 0, 1, 2]:
            damaged, expected = bytearray(original), bytearray(fixed)
            options = ['--repair-directory-entries'] if source is None else ['--repair-tree']
            if source:
                other = 2 - source
                start = (layout.reserved + other * layout.fatsz) * 512
                damaged[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                options += ['--fat-source', str(source)]
                expected = rebuilt_info(expected, bits)
            path.write_bytes(damaged)
            preview = run(path, *options)
            assert 'DIR_GROW' in preview and path.read_bytes() == damaged
            journal = root / f'{bits}-{source}.mdj'
            run(path, *options, '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path)
            assert command('mtype', '-i', str(path), '::/Empty/' + NAME + '/DATA.TXT').stdout == PAYLOAD
            assert command('mtype', '-i', str(path), '::/Empty/FIRST.TXT').stdout == PAYLOAD
            check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            raw = journal.read_bytes()
            count = struct.unpack_from('<I', raw, 12)[0]
            sectors = {struct.unpack_from('<I', raw, 1088 + i * 1032)[0] for i in range(count)}
            for cluster in added:
                start = layout.data_sector + (cluster - 2) * layout.spc
                assert all(sector in sectors for sector in range(start, start + layout.spc))
            # Even an originally zero added sector must reject unrelated later edits.
            changed = bytearray(expected)
            start = layout.data_sector + (added[-1] - 2) * layout.spc
            changed[(start + layout.spc) * 512 - 1] ^= 1
            path.write_bytes(changed)
            run(path, '--restore-journal', str(journal), '--apply', code=2)
            assert path.read_bytes() == changed
            path.write_bytes(expected)
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == damaged
            for n in [1, count // 2, count - 1]:
                partial = bytearray(damaged)
                for index in range(n):
                    record = raw[1088 + index * 1032:1088 + (index + 1) * 1032]
                    sector = struct.unpack_from('<I', record)[0]
                    partial[sector * 512:(sector + 1) * 512] = record[516:1028]
                path.write_bytes(partial)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == damaged
        if bits == 32:
            # A stale hint can coincidentally equal the post-growth count.
            # Selected-source recount then removes already-planned hint patches.
            damaged = bytearray(original)
            expected = rebuilt_info(fixed, bits)
            info, backup = struct.unpack_from('<HH', damaged, 48)
            for sector in [info, backup + info]:
                damaged[sector * 512:(sector + 1) * 512] = expected[sector * 512:(sector + 1) * 512]
            path.write_bytes(damaged)
            journal = root / 'matching-hints.mdj'
            run(path, '--repair-tree', '--fat-source', '1', '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == damaged
        referenced, ref_fixed, _ = growth_fixture(path, bits, referenced=True)
        path.write_bytes(referenced)
        unused = root / f'referenced-entry-{bits}.mdj'
        run(path, '--repair-directory-entries', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == referenced and not unused.exists()
        journal = root / f'referenced-tree-{bits}.mdj'
        run(path, '--repair-tree', '--apply', '--journal', str(journal))
        assert path.read_bytes() == ref_fixed
        run(path)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == referenced
        full = bytearray(original)
        total = (len(full) // 512 - layout.data_sector) // layout.spc
        bad = {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits]
        for cluster in range(2, total + 2):
            if not layout.fat(cluster):
                set_fat(full, bits, cluster, bad)
        path.write_bytes(full)
        unused = root / f'full{bits}.mdj'
        run(path, '--repair-directory-entries', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == full and not unused.exists()
print('PASS: FAT12/16/32 directory growth, original free bytes, FAT authority, FSInfo, full-volume refusal and exact/partial guarded undo')
