#!/usr/bin/env python3
"""Clear only reserved attribute bits, including recoverable VFAT records."""
from pathlib import Path
import struct
import subprocess
import zlib
import sys
import tempfile
from directory_fixture import create, command, Layout
from test_alias_repair import run


def fixture(path, bits):
    create(path, bits)
    command('mlabel', '-i', str(path), '::ATTRTEST')
    layout = Layout(path.read_bytes(), bits)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    expected = bytearray(layout.data)
    entries = [p for cluster in [layout.root, parent, child] for p in layout.entries(cluster)]
    # Exercise normal flags on regular files independently of the reserved bits.
    files = [p for p in entries if not expected[p + 11] & 0x18]
    for i, p in enumerate(files):
        expected[p + 11] = [0, 1, 2, 4, 0x20, 0x27][i % 6]
    damaged = bytearray(expected)
    for i, p in enumerate(entries):
        damaged[p + 11] |= [0x40, 0x80, 0xc0][i % 3]
    path.write_bytes(damaged)
    return bytes(damaged), bytes(expected), entries


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='magicdisk-attributes-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / 'SOURCE.IMG'
            damaged, expected, entries = fixture(path, bits)
            if len(sys.argv) > 2:
                subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(root / 'FAULT.MDJ')], check=True)
            report = run(exe, path, '--repair-attributes')
            assert report.count('CLEAR_ATTRIBUTE_BITS') == len(entries), report
            assert path.read_bytes() == damaged
            journal = root / f'{bits}.mdj'
            run(exe, path, '--repair-attributes', '--apply', '--journal', journal)
            assert path.read_bytes() == expected
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 21
            for count in range((len(raw) - 1088) // 1032 + 1):
                partial = bytearray(damaged)
                for index in range(count):
                    p = 1088 + index * 1032
                    sector = struct.unpack_from('<I', raw, p)[0]
                    partial[sector * 512:(sector + 1) * 512] = raw[p + 516:p + 1028]
                path.write_bytes(partial)
                run(exe, path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == damaged
            run(exe, path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == damaged
            # Even a self-consistent CRC cannot authorize unrelated byte changes.
            forged = bytearray(raw)
            record = 1088
            forged[record + 516 + 12] ^= 1
            struct.pack_into('<I', forged, record + 1028, zlib.crc32(forged[record:record + 1028]))
            forged_path = root / f'forged{bits}.mdj'
            forged_path.write_bytes(forged)
            changed = bytearray(expected)
            sector = struct.unpack_from('<I', raw, record)[0]
            changed[sector * 512 + 12] ^= 1
            path.write_bytes(changed)
            run(exe, path, '--restore-journal', forged_path, '--apply', code=2)
            assert path.read_bytes() == changed
            # Defined type bits and malformed long-name content are not guessed.
            file = next(p for p in entries if not expected[p + 11] & 0x18)
            lfn = next(p for p in entries if expected[p + 11] == 15)
            for mode in ['ambiguous-type', 'lfn-type', 'lfn-text', 'lfn-checksum']:
                invalid = bytearray(damaged)
                if mode == 'ambiguous-type':
                    invalid[file + 11] |= 0x18
                elif mode == 'lfn-type':
                    invalid[lfn + 12] = 1
                elif mode == 'lfn-text':
                    struct.pack_into('<H', invalid, lfn + 1, ord('?'))
                else:
                    invalid[lfn + 13] ^= 1
                path.write_bytes(invalid)
                unused = root / 'REFUSED.MDJ'
                run(exe, path, '--repair-attributes', '--apply', '--journal', unused, code=2)
                assert not unused.exists() and path.read_bytes() == invalid
            print(f'PASS FAT{bits}: exact reserved-bit cleanup, preserved flags/LFNs/payload, fsck, partial/repeated undo and ambiguous metadata refusal', flush=True)
