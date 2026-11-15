#!/usr/bin/env python3
"""Explicit recovery of byte-compatible interrupted metadata writes."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create
from entry_fixture import entry_fixture

exe = Path(sys.argv[1]).resolve()


def run(image, *args, code=0):
    result = subprocess.run([str(exe), str(image), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-torn-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        image = root / f'fat{bits}.img'
        layout = create(image, bits)
        original, expected = entry_fixture(layout.data, bits)
        image.write_bytes(original)
        journal = root / f'fat{bits}.mdj'
        run(image, '--repair-directory-entries', '--apply', '--journal', str(journal))
        assert image.read_bytes() == expected
        raw = journal.read_bytes()
        count = struct.unpack_from('<I', raw, 12)[0]
        records = []
        for index in range(count):
            record = raw[1088 + index * 1032:1088 + (index + 1) * 1032]
            records.append((struct.unpack_from('<I', record)[0], record[4:516], record[516:1028]))
        for split in ['prefix', 'suffix', 'alternating']:
            mixed = bytearray(original)
            genuine = False
            for sector, before, after in records:
                changed = [i for i in range(512) if before[i] != after[i]]
                selected = changed[:len(changed) // 2] if split == 'prefix' else (
                    changed[len(changed) // 2:] if split == 'suffix' else changed[::2])
                data = bytearray(before)
                for offset in selected:
                    data[offset] = after[offset]
                genuine |= data != before and data != after
                mixed[sector * 512:(sector + 1) * 512] = data
            assert genuine
            image.write_bytes(mixed)
            run(image, '--restore-journal', str(journal), '--apply', code=2)
            assert image.read_bytes() == mixed
            run(image, '--restore-journal', str(journal), '--apply', '--accept-torn')
            assert image.read_bytes() == original
            run(image, '--restore-journal', str(journal), '--apply', '--accept-torn')
            assert image.read_bytes() == original
        # A third byte value in the last record must prevent all writes.
        invalid = bytearray(mixed)
        sector, before, after = records[-1]
        offset = next(i for i in range(512) if before[i] == after[i])
        invalid[sector * 512 + offset] = before[offset] ^ 0xff
        image.write_bytes(invalid)
        run(image, '--restore-journal', str(journal), '--apply', '--accept-torn', code=2)
        assert image.read_bytes() == invalid
        # Directory ownership outside the journal still governs permission to undo.
        invalid = bytearray(mixed)
        parent_entry = layout.child()
        struct.pack_into('<H', invalid, parent_entry + 26, 1000)
        image.write_bytes(invalid)
        run(image, '--restore-journal', str(journal), '--apply', '--accept-torn', code=2)
        assert image.read_bytes() == invalid
        run(image, '--accept-torn', code=2)
        run(image, '--repair-tree', '--accept-torn', code=2)
        for kind in ['bad', 'mirror'] + (['fsinfo'] if bits == 32 else []):
            before = bytearray(layout.data)
            if kind == 'mirror':
                offset = (layout.reserved + 2 * layout.fatsz - 1) * 512
                before[offset:offset + 128] = bytes([0xaa]) * 128
                options = ['--repair-fat', '1']
            elif kind == 'fsinfo':
                for sector in [1, 7]:
                    struct.pack_into('<II', before, sector * 512 + 488, 0xffffffff, 0xffffffff)
                options = ['--repair-fsinfo']
            else:
                options = ['--mark-bad', '1000']
            image.write_bytes(before)
            journal = root / f'{kind}{bits}.mdj'
            run(image, *options, '--apply', '--journal', str(journal))
            raw = journal.read_bytes()
            mixed = bytearray(before)
            genuine = False
            for index in range(struct.unpack_from('<I', raw, 12)[0]):
                record = raw[1088 + index * 1032:1088 + (index + 1) * 1032]
                sector = struct.unpack_from('<I', record)[0]
                old, new = record[4:516], record[516:1028]
                data = bytearray(old)
                changed = [i for i in range(512) if old[i] != new[i]]
                for offset in changed[::2]:
                    data[offset] = new[offset]
                genuine |= data != old and data != new
                mixed[sector * 512:(sector + 1) * 512] = data
            assert genuine, kind
            image.write_bytes(mixed)
            run(image, '--restore-journal', str(journal), '--apply', code=2)
            assert image.read_bytes() == mixed
            run(image, '--restore-journal', str(journal), '--apply', '--accept-torn')
            assert image.read_bytes() == before
print('PASS: explicit FAT12/16/32 mixed-sector recovery, strict default, exact/repeated undo and unrelated-byte/ownership guards')
