#!/usr/bin/env python3
"""Extra-dot cleanup must preserve every other byte and validate journal undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import command, Layout
from extra_dot_fixture import fixture
from test_alias_repair import run


def verify(exe, path, journal, broken, expected, count):
    preview = run(exe, path, '--clear-extra-dots')
    assert preview.count('REMOVE_EXTRA_DOT') == count, preview
    assert path.read_bytes() == broken
    run(exe, path, '--clear-extra-dots', '--apply', '--journal', journal)
    assert path.read_bytes() == expected
    run(exe, path)
    command('fsck.fat', '-n', str(path))
    raw = journal.read_bytes()
    assert struct.unpack_from('<I', raw, 8)[0] == 22
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
    with tempfile.TemporaryDirectory(prefix='magicdisk-extra-dots-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            broken, expected, removed = fixture(path, bits)
            if len(sys.argv) > 2:
                result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path),
                                         str(root / 'fault.mdj')], capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
            verify(exe, path, root / f'{bits}.mdj', broken, expected, len(removed))
            for offset in removed:
                for field, fmt, value in [(11, 'B', 32), (11, 'B', 24),
                                           (26, '<H', 1234), (28, '<I', 1)]:
                    damaged = bytearray(broken)
                    struct.pack_into(fmt, damaged, offset + field, value)
                    path.write_bytes(damaged)
                    journal = root / 'refused.mdj'
                    run(exe, path, '--clear-extra-dots', '--apply', '--journal', journal, code=2)
                    assert not journal.exists()
                    assert path.read_bytes() == damaged
            # Extra records cannot substitute for a missing leading pair.
            layout = Layout(broken, bits)
            parent = layout.cluster(layout.child())
            damaged = bytearray(broken)
            damaged[layout.slots(parent)[0]] = 0xe5
            path.write_bytes(damaged)
            run(exe, path, '--clear-extra-dots', '--apply', '--journal', root / 'missing.mdj', code=2)
            assert not (root / 'missing.mdj').exists()
            assert path.read_bytes() == damaged
            print(f'PASS FAT{bits}: extra root/nested dots, exact bytes, LFN preservation, fsck, partial/repeated undo, ambiguous metadata and forged journal refusal')
