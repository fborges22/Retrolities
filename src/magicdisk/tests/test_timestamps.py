#!/usr/bin/env python3
"""Calendar/time boundaries on independent FAT12/16/32 directory trees."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, UNICODE_FILE
from chain_fixture import chain_fixture
from directory_chain_fixture import directory_chain_fixture
from lost_fixture import lost_fixture

exe = Path(sys.argv[1]).resolve()


def date(year, month, day):
    return (year - 1980) << 9 | month << 5 | day


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-times-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        original = layout.data
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        parts, entry = next((a, b) for a, b in layout.long_names(child)
                            if layout.decoded_name(a) == UNICODE_FILE)
        cases = [(13, 200), (13, 255)]
        for field in [14, 22]:
            cases += [(field, 30), (field, 31), (field, 60 << 5), (field, 24 << 11)]
        for field in [16, 18, 24]:
            cases += [(field, date(2026, 0, 1)), (field, date(2026, 13, 1)),
                      (field, date(2026, 1, 0)), (field, date(2026, 4, 31)),
                      (field, date(2026, 2, 29)), (field, date(2100, 2, 29)),
                      (field, date(2000, 2, 30))]
        for field, value in cases:
            damaged = bytearray(original)
            struct.pack_into('<B' if field == 13 else '<H', damaged, entry + field, value)
            path.write_bytes(damaged)
            output = run(path, code=1)
            assert output.count('DIR_TIMESTAMP') == 1, output
            assert UNICODE_FILE in output and f'sector byte {entry % 512 + field}' in output
            assert path.read_bytes() == damaged
        for value in [0, date(1980, 1, 1), date(1980, 2, 29), date(2000, 2, 29),
                      date(2100, 2, 28), date(2107, 12, 31)]:
            valid = bytearray(original)
            for field in [16, 18, 24]:
                struct.pack_into('<H', valid, entry + field, value)
            for field in [14, 22]:
                struct.pack_into('<H', valid, entry + field, 23 << 11 | 59 << 5 | 29)
            valid[entry + 13] = 199
            path.write_bytes(valid)
            run(path)
        # Unavailable optional dates and midnight are valid; no ordering is guessed.
        valid[entry + 13:entry + 20] = bytes(7)
        valid[entry + 22:entry + 26] = bytes(4)
        path.write_bytes(valid)
        run(path)
        # Dot entries are checked, but volume labels and unused slots are not stamps.
        dot = layout.slots(child)[0]
        damaged = bytearray(original)
        struct.pack_into('<H', damaged, dot + 24, date(2100, 2, 29))
        path.write_bytes(damaged)
        assert 'DIR_TIMESTAMP' in run(path, code=1)
        for attr, name in [(8, b'LABEL      '), (32, b'\xe5ELETED TXT')]:
            damaged = bytearray(original)
            # Append to root's first unused slot, avoiding orphaning existing LFNs.
            slot = next(x for x in layout.slots(layout.root) if not damaged[x])
            damaged[slot:slot + 11] = name
            damaged[slot + 11] = attr
            damaged[slot + 13:slot + 20] = b'\xff' * 7
            damaged[slot + 22:slot + 26] = b'\xff' * 4
            path.write_bytes(damaged)
            run(path)
        # Unrelated allocation repair preserves malformed timestamps and exact undo.
        damaged = bytearray(original)
        damaged[entry + 13] = 255
        path.write_bytes(damaged)
        free = next(c for c in range(3, 1000) if layout.fat(c) == 0)
        journal = root / f'time{bits}.mdj'
        run(path, '--mark-bad', str(free), '--apply', '--journal', str(journal))
        assert path.read_bytes()[entry:entry + 32] == damaged[entry:entry + 32]
        assert 'DIR_TIMESTAMP' in run(path, code=1)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == damaged

        for mode in ['files', 'directories', 'tree', 'lost', 'fsinfo', 'backup', 'primary', 'mirror']:
            if bits != 32 and mode in ['fsinfo', 'backup', 'primary', 'mirror']:
                continue
            base = bytearray(original)
            base[entry + 13] = 255
            if mode == 'tree':
                broken = directory_chain_fixture(chain_fixture(base, bits)[0], bits)[0]
                operation = ['--repair-tree']
            elif mode == 'files':
                broken = chain_fixture(base, bits)[0]
                operation = ['--repair-files']
            elif mode == 'directories':
                broken = directory_chain_fixture(base, bits)[0]
                operation = ['--repair-directories']
            elif mode == 'lost':
                broken = lost_fixture(base, bits)[0]
                operation = ['--reclaim-lost']
            else:
                broken = base[:]
                if mode == 'mirror':
                    start = (layout.reserved + layout.fatsz) * 512
                    broken[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                    operation = ['--repair-fat', '1']
                else:
                    sector = {'fsinfo': 1, 'backup': 6, 'primary': 0}[mode]
                    broken[sector * 512:(sector + 1) * 512] = bytes(512)
                    operation = ['--repair-' + {'fsinfo': 'fsinfo', 'backup': 'boot-backup',
                                               'primary': 'boot-primary'}[mode]]
            path.write_bytes(broken)
            if mode == 'lost':
                destination = root / f'export{bits}'
                destination.mkdir()
                run(path, '--recover-lost', str(destination))
                assert path.read_bytes() == broken
            journal = root / f'{mode}{bits}.mdj'
            run(path, *operation, '--apply', '--journal', str(journal))
            repaired = path.read_bytes()
            for first, last in [(13, 20), (22, 26)]:
                assert repaired[entry + first:entry + last] == broken[entry + first:entry + last]
            assert 'DIR_TIMESTAMP' in run(path, code=1)
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == broken

print('PASS: FAT12/16/32 timestamp ranges, leap years, Unicode findings, ignored slots and repair preservation')
