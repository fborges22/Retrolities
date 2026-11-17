#!/usr/bin/env python3
"""Exact timestamp field corrections, complete journal undo, and ownership guards."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, UNICODE_FILE
from chain_fixture import chain_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *map(str, args)], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


def fixture(path, bits):
    layout = create(path, bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    _, entry = next((a, b) for a, b in layout.long_names(child)
                    if layout.decoded_name(a) == UNICODE_FILE)
    broken = bytearray(layout.data)
    expected = bytearray(broken)
    changes = [(layout.child(), 13, 255), (layout.slots(parent)[0], 14, 24 << 11),
               (layout.slots(child)[1], 16, (120 << 9) | (2 << 5) | 29),
               (entry, 18, (46 << 9) | (4 << 5) | 31),
               (entry, 22, 60 << 5), (entry, 24, (46 << 9) | 1)]
    for offset, field, value in changes:
        form = '<B' if field == 13 else '<H'
        struct.pack_into(form, broken, offset + field, value)
        struct.pack_into(form, expected, offset + field, 0)
    path.write_bytes(broken)
    return broken, expected


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='magicdisk-time-repair-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            journal = root / f'{bits}.mdj'
            broken, expected = fixture(path, bits)
            if len(sys.argv) > 2:
                result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(journal)],
                                        capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
            preview = run(path, '--clear-invalid-timestamps')
            assert preview.count('CLEAR_TIMESTAMP') == 6
            assert path.read_bytes() == broken
            run(path, '--clear-invalid-timestamps', '--apply', '--journal', journal)
            assert path.read_bytes() == expected
            run(path)
            raw = journal.read_bytes()
            version, count = struct.unpack_from('<II', raw, 8)
            assert version == 14 and count >= 3
            # Journal includes the two identity sectors before the records.
            patches = [(struct.unpack_from('<I', raw, p)[0], raw[p + 4:p + 516],
                        raw[p + 516:p + 1028]) for p in range(1088, len(raw), 1032)]
            assert len(patches) == count
            for applied in range(count + 1):
                partial = bytearray(broken)
                for sector, before, after in patches[:applied]:
                    partial[sector * 512:(sector + 1) * 512] = after
                path.write_bytes(partial)
                run(path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == broken
            path.write_bytes(expected)
            changed = bytearray(expected)
            changed[patches[0][0] * 512 + 12] ^= 1
            path.write_bytes(changed)
            run(path, '--restore-journal', journal, '--apply', code=2)
            assert path.read_bytes() == changed
            forged = bytearray(raw)
            record = 1088
            forged[record + 516 + 12] ^= 1
            struct.pack_into('<I', forged, record + 1028,
                             zlib.crc32(forged[record:record + 1028]))
            forged_path = root / f'forged{bits}.mdj'
            forged_path.write_bytes(forged)
            # The disk matches the forged after-image; ownership/plan validation
            # must still reject the unrelated byte without writing anything.
            path.write_bytes(changed)
            run(path, '--restore-journal', forged_path, '--apply', code=2)
            assert path.read_bytes() == changed
            path.write_bytes(expected)
            run(path, '--restore-journal', journal, '--apply', '--accept-torn')
            assert path.read_bytes() == broken
            ambiguous = chain_fixture(broken, bits)[0]
            path.write_bytes(ambiguous)
            run(path, '--clear-invalid-timestamps', code=2)
            assert path.read_bytes() == ambiguous
    print('PASS: FAT12/16/32 timestamp field corrections, preview, partial/exact undo and ownership guards')
