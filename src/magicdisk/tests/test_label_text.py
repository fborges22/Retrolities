#!/usr/bin/env python3
"""Root/boot label normalization, exact undo and independently forged journal guards."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import command
from label_text_fixture import fixture
from label_fixture import create_labels
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-label-text-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        broken, expected, first = fixture(path, bits)
        if len(sys.argv) > 2:
            subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(root / 'FAULT.MDJ')], check=True)
        preview = run(exe, path, '--repair-label-text')
        assert 'LABEL_TEXT' in preview and 'raw_hex=' in preview, preview
        assert path.read_bytes() == broken
        journal = root / f'{bits}.mdj'
        run(exe, path, '--repair-label-text', '--apply', '--journal', journal)
        assert path.read_bytes() == expected
        run(exe, path)
        command('fsck.fat', '-n', str(path))
        raw = journal.read_bytes()
        assert struct.unpack_from('<I', raw, 8)[0] == 23
        records = list(range(1088, len(raw), 1032))
        # Include partial application with and without the primary boot patch.
        for mask in range(1 << len(records)):
            partial = bytearray(broken)
            for index, offset in enumerate(records):
                if mask & (1 << index):
                    sector = struct.unpack_from('<I', raw, offset)[0]
                    partial[sector * 512:(sector + 1) * 512] = raw[offset + 516:offset + 1028]
            path.write_bytes(partial)
            run(exe, path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == broken
        forged = bytearray(raw)
        offset = next(p for p in records if struct.unpack_from('<I', raw, p)[0] == 0)
        forged[offset + 516 + 3] ^= 1
        struct.pack_into('<I', forged, offset + 1028, zlib.crc32(forged[offset:offset + 1028]))
        fake = root / f'{bits}-forged.mdj'
        fake.write_bytes(forged)
        changed = bytearray(expected)
        changed[3] ^= 1
        path.write_bytes(changed)
        run(exe, path, '--restore-journal', fake, '--apply', code=2)
        assert path.read_bytes() == changed
        base, first, _ = create_labels(path, bits, duplicate=False)
        for value in b'"*+,./:;<=>?[\\]|':
            data = bytearray(base)
            data[first + 2] = value
            path.write_bytes(data)
            report = run(exe, path, code=1)
            assert 'DIR_LABEL_NAME' in report and f'0x{value:02X}' in report, report
            assert path.read_bytes() == data
        # Unknown multibyte text is preserved; an explicit single-byte policy
        # permits punctuation correction and is retained by journal undo.
        data = bytearray(base)
        data[first] = 0x82
        data[first + 2] = ord('?')
        path.write_bytes(data)
        run(exe, path)
        policy_journal = root / f'{bits}-oem.mdj'
        run(exe, path, '--code-page', 850, '--repair-label-text', '--apply', '--journal', policy_journal)
        assert path.read_bytes()[first:first + 3] == bytes([0x82, ord('I'), ord('_')])
        assert struct.unpack_from('<I', policy_journal.read_bytes(), 32)[0] == 850
        run(exe, path, '--restore-journal', policy_journal, '--apply')
        assert path.read_bytes() == data
        # Reserved boot fields are not a label without the extended signature.
        data = bytearray(broken)
        signature = 66 if bits == 32 else 38
        data[signature] = 0
        if bits == 32:
            data[6 * 512 + signature] = 0
        path.write_bytes(data)
        no_extended = root / f'{bits}-noext.mdj'
        run(exe, path, '--repair-label-text', '--apply', '--journal', no_extended)
        changed = bytearray(data)
        changed[first:first + 11] = b'_A_B_C_D E '
        assert path.read_bytes() == changed
        run(exe, path, '--restore-journal', no_extended, '--apply')
        assert path.read_bytes() == data
        print(f'PASS FAT{bits}: exact root/boot labels, fsck, all partial journal subsets and forged boot-metadata refusal', flush=True)
