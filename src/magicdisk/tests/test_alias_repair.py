#!/usr/bin/env python3
"""Exact alias replacement with collision skips, intact LFNs and journal undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, command
from short_name_fixture import invalid_aliases, entries, replace_alias
from duplicate_fixture import long_name, checksum
from oem_fixture import fixture as oem_damage


def fixture(path, bits, limit=None, directory=False):
    layout = create(path, bits)
    broken, invalid = invalid_aliases(layout.data, bits)
    names = entries(broken, bits)
    if limit is not None:
        for parts, alias in names[limit:len(invalid)]:
            for offset in parts + [alias]:
                broken[offset:offset + 32] = layout.data[offset:offset + 32]
        invalid = invalid[:limit]
    parts, alias = names[-1]
    long_name(broken, parts, alias, 'R0000001.CHK')
    parts, alias = names[-2]
    replace_alias(broken, parts, alias, b'R0000002CHK')
    if directory:
        parent_parts, parent_entry = next((parts, entry) for parts, entry in layout.long_names(layout.root)
                                          if entry == layout.child())
        damaged = bytearray(broken[parent_entry:parent_entry + 11])
        damaged[2] = ord('?')
        replace_alias(broken, parent_parts, parent_entry, damaged)
    expected = bytearray(broken)
    if directory:
        expected[parent_entry:parent_entry + 11] = b'R0000001CHK'
        for part in parent_parts:
            expected[part + 13] = checksum(b'R0000001CHK')
    for index, (parts, alias) in enumerate(names[:len(invalid)]):
        new = f'R{index + 3:07d}CHK'.encode()
        expected[alias:alias + 11] = new
        for part in parts:
            expected[part + 13] = checksum(new)
    path.write_bytes(broken)
    return bytes(broken), bytes(expected), len(invalid) + int(directory)


def run(exe, path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *map(str, args)], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='magicdisk-alias-repair-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            journal = root / f'{bits}.mdj'
            broken, expected, count = fixture(path, bits)
            if len(sys.argv) > 2:
                result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(journal)],
                                        capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
            preview = run(exe, path, '--repair-short-names')
            assert preview.count('RENAME_ALIAS') == count, preview
            assert 'to R0000003.CHK' in preview
            assert path.read_bytes() == broken
            run(exe, path, '--repair-short-names', '--apply', '--journal', journal)
            assert path.read_bytes() == expected
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 18
            patches = [(struct.unpack_from('<I', raw, p)[0], raw[p + 516:p + 1028])
                       for p in range(1088, len(raw), 1032)]
            for applied in range(len(patches) + 1):
                partial = bytearray(broken)
                for sector, after in patches[:applied]:
                    partial[sector * 512:(sector + 1) * 512] = after
                path.write_bytes(partial)
                run(exe, path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == broken
            run(exe, path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == broken
            # A forged after-image with a valid CRC cannot authorize extra changes.
            forged = bytearray(raw)
            record = 1088
            forged[record + 516 + 12] ^= 1
            struct.pack_into('<I', forged, record + 1028, zlib.crc32(forged[record:record + 1028]))
            forged_path = root / f'forged{bits}.mdj'
            forged_path.write_bytes(forged)
            changed = bytearray(expected)
            changed[patches[0][0] * 512 + 12] ^= 1
            path.write_bytes(changed)
            run(exe, path, '--restore-journal', forged_path, '--apply', code=2)
            assert path.read_bytes() == changed
            # An explicit SBCS policy proves punctuation invalid; undo must not
            # depend on the policy selected by a later DOS/CLI session.
            layout = create(path, bits)
            oem, entry = oem_damage(layout.data, bits, 'punctuation')
            path.write_bytes(oem)
            run(exe, path, '--repair-short-names')
            assert path.read_bytes() == oem
            oem_journal = root / f'oem{bits}.mdj'
            run(exe, path, '--code-page', 850, '--repair-short-names', '--apply', '--journal', oem_journal)
            corrected = bytearray(oem)
            corrected[entry:entry + 11] = b'R0000001CHK'
            assert path.read_bytes() == corrected
            assert struct.unpack_from('<I', oem_journal.read_bytes(), 32)[0] == 850
            run(exe, path, '--restore-journal', oem_journal, '--apply')
            assert path.read_bytes() == oem
            for policy in [999, 0xffffffff, 0]:
                forged = bytearray(oem_journal.read_bytes())
                struct.pack_into('<I', forged, 32, policy)
                crc = zlib.crc32(forged[:60])
                crc = zlib.crc32(forged[64:1088], crc)
                struct.pack_into('<I', forged, 60, crc)
                policy_path = root / f'policy{bits}-{policy}.mdj'
                policy_path.write_bytes(forged)
                path.write_bytes(corrected)
                run(exe, path, '--restore-journal', policy_path, '--apply', code=2)
                assert path.read_bytes() == corrected
            run(exe, path, '--restore-journal', oem_journal, '--apply')
            assert path.read_bytes() == oem
            # Directory aliases change without changing their dot entries,
            # child ownership, valid long paths or allocation.
            broken, expected, count = fixture(path, bits, limit=3, directory=True)
            directory_journal = root / f'directory{bits}.mdj'
            preview = run(exe, path, '--repair-short-names')
            assert preview.count('RENAME_ALIAS') == count
            run(exe, path, '--repair-short-names', '--apply', '--journal', directory_journal)
            assert path.read_bytes() == expected
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            run(exe, path, '--restore-journal', directory_journal, '--apply')
            assert path.read_bytes() == broken
            print(f'PASS FAT{bits}: exact alias bytes and LFN checksums, alias/LFN collision skips, clean fsck, partial/repeated undo')
