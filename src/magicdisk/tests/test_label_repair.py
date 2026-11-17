#!/usr/bin/env python3
"""Label cleanup changes deletion bytes only, with strict journal undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, command, Layout
from label_fixture import create_labels
from test_alias_repair import run


def fixture(path, bits):
    create(path, bits)
    command('mlabel', '-i', str(path), '::FIRST')
    data = bytearray(path.read_bytes())
    layout = Layout(data, bits)
    first = next(p for p in layout.entries() if data[p + 11] == 8)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    removed = []
    for cluster in [layout.root, parent, child]:
        slots = layout.slots(cluster)
        index = next(i for i, p in enumerate(slots) if not data[p])
        assert index + 1 < len(slots)
        offset = slots[index]
        data[offset:offset + 32] = data[first:first + 32]
        data[offset:offset + 11] = b'EXTRA      '
        data[slots[index + 1]] = 0
        removed.append(offset)
    expected = bytearray(data)
    for offset in removed:
        expected[offset] = 0xe5
    path.write_bytes(data)
    return bytes(data), bytes(expected), first, removed


def verify(exe, path, journal, broken, expected, count):
    preview = run(exe, path, '--repair-labels')
    assert preview.count('KEEP_LABEL') == 1, preview
    assert preview.count('REMOVE_LABEL') == count, preview
    assert path.read_bytes() == broken
    run(exe, path, '--repair-labels', '--apply', '--journal', journal)
    assert path.read_bytes() == expected
    run(exe, path)
    command('fsck.fat', '-n', str(path))
    raw = journal.read_bytes()
    assert struct.unpack_from('<I', raw, 8)[0] == 19
    records = list(range(1088, len(raw), 1032))
    for applied in range(len(records) + 1):
        partial = bytearray(broken)
        for p in records[:applied]:
            sector = struct.unpack_from('<I', raw, p)[0]
            partial[sector * 512:(sector + 1) * 512] = raw[p + 516:p + 1028]
        path.write_bytes(partial)
        run(exe, path, '--restore-journal', journal, '--apply')
        assert path.read_bytes() == broken
    run(exe, path, '--restore-journal', journal, '--apply')
    assert path.read_bytes() == broken
    forged = bytearray(raw)
    p = records[0]
    forged[p + 516 + 12] ^= 1
    struct.pack_into('<I', forged, p + 1028, zlib.crc32(forged[p:p + 1028]))
    forged_path = journal.with_suffix('.forged')
    forged_path.write_bytes(forged)
    changed = bytearray(expected)
    sector = struct.unpack_from('<I', raw, p)[0]
    changed[sector * 512 + 12] ^= 1
    path.write_bytes(changed)
    run(exe, path, '--restore-journal', forged_path, '--apply', code=2)
    assert path.read_bytes() == changed


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='magicdisk-label-repair-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            broken, expected, first, removed = fixture(path, bits)
            if len(sys.argv) > 2:
                result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path),
                                         str(root / 'fault.mdj')], capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
            verify(exe, path, root / f'{bits}.mdj', broken, expected, len(removed))
            for offset in [first, *removed]:
                for field, fmt, value in [(26, '<H', 2), (28, '<I', 1)] + ([(20, '<H', 1)] if bits == 32 else []):
                    damaged = bytearray(broken)
                    struct.pack_into(fmt, damaged, offset + field, value)
                    path.write_bytes(damaged)
                    journal = root / 'refused.mdj'
                    run(exe, path, '--repair-labels', '--apply', '--journal', journal, code=2)
                    assert not journal.exists()
                    assert path.read_bytes() == damaged
            for same in [False, True]:
                data, first, second = create_labels(path, bits, same=same)
                expected = bytearray(data)
                expected[second] = 0xe5
                path.write_bytes(data)
                verify(exe, path, root / f'{bits}-{same}.mdj', data, expected, 1)
            print(f'PASS FAT{bits}: root/subdirectory labels, exact bytes, intact LFN tree, fsck, partial/repeated undo, ambiguous metadata and forged journal refusal')
