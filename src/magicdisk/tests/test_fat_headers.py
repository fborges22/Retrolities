#!/usr/bin/env python3
"""Validate reserved FAT entries even when every copy has identical damage."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import command, Layout
from chain_fixture import set_fat

exe = Path(sys.argv[1]).resolve()


def run(path, *options, code=0):
    result = subprocess.run([str(exe), str(path), *options], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-fat-headers-') as directory:
    root = Path(directory)
    for bits, size in [(12, 1440 * 1024), (16, 32 * 1024 * 1024), (32, 64 * 1024 * 1024)]:
        path = root / f'FAT{bits}.IMG'
        with path.open('wb') as file:
            file.truncate(size)
        command('mkfs.fat', '-F', str(bits), str(path))
        clean = path.read_bytes()
        layout = Layout(clean, bits)
        width = {12: 3, 16: 4, 32: 8}[bits]
        starts = [(layout.reserved + copy * layout.fatsz) * 512 for copy in range(clean[16])]
        for kind in ['zero', 'media', 'entry-one']:
            damaged = bytearray(clean)
            for start in starts:
                if kind == 'zero':
                    damaged[start:start + width] = bytes(width)
                elif kind == 'media':
                    damaged[start] = 0xf8 if clean[21] != 0xf8 else 0xf0
                elif bits == 12:
                    damaged[start + 1] &= 15
                    damaged[start + 2] = 0
                else:
                    damaged[start + bits // 8:start + width] = bytes(bits // 8)
            path.write_bytes(damaged)
            report = run(path, code=1)
            assert report.count('FAT_HEADER') == len(starts) * (2 if kind == 'zero' else 1), report
            assert 'FAT_MIRROR' not in report, report
            journal = root / f'{bits}-{kind}.MDJ'
            run(path, '--mark-bad', '1000', '--apply', '--journal', str(journal), code=2)
            assert not journal.exists() and path.read_bytes() == damaged
            expected = bytearray(clean)
            for start in starts:
                if bits == 16:
                    struct.pack_into('<H', expected, start + 2,
                                     (struct.unpack_from('<H', damaged, start + 2)[0] & 0xc000) | 0x3fff)
                elif bits == 32:
                    struct.pack_into('<I', expected, start + 4,
                                     struct.unpack_from('<I', damaged, start + 4)[0] | 0x03ffffff)
            run(path, '--repair-fat-headers')
            assert path.read_bytes() == damaged
            run(path, '--repair-fat-headers', '--apply', '--journal', str(journal))
            assert path.read_bytes() == expected
            run(path)
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 15
            records = [(struct.unpack_from('<I', raw, p)[0], raw[p + 4:p + 516],
                        raw[p + 516:p + 1028]) for p in range(1088, len(raw), 1032)]
            assert len(records) == len(starts)
            for sector, before, after in records:
                assert before == damaged[sector * 512:(sector + 1) * 512]
                assert after == expected[sector * 512:(sector + 1) * 512]
            # A structurally valid journal may not invent status-bit changes,
            # even when all current FAT copies agree with the forged image.
            forged = bytearray(raw)
            forged_disk = bytearray(expected)
            offset, mask = {12: (1, 0x70), 16: (3, 0xc0), 32: (7, 0x0c)}[bits]
            for index, (sector, _, _) in enumerate(records):
                record = 1088 + index * 1032
                forged[record + 516 + offset] ^= mask
                struct.pack_into('<I', forged, record + 1028,
                                 zlib.crc32(forged[record:record + 1028]))
                forged_disk[sector * 512 + offset] ^= mask
            forged_path = root / f'{bits}-{kind}-forged.MDJ'
            forged_path.write_bytes(forged)
            path.write_bytes(forged_disk)
            run(path, '--restore-journal', str(forged_path), '--apply', code=2)
            assert path.read_bytes() == forged_disk
            for applied in range(len(records) + 1):
                partial = bytearray(damaged)
                for sector, _, after in records[:applied]:
                    partial[sector * 512:(sector + 1) * 512] = after
                path.write_bytes(partial)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == damaged
            # Explicit torn recovery still accepts only before/after bytes.
            mixed = bytearray(damaged)
            sector, before, after = records[0]
            changed = [index for index in range(512) if before[index] != after[index]]
            if len(changed) > 1:
                mixed[sector * 512 + changed[0]] = after[changed[0]]
                path.write_bytes(mixed)
                run(path, '--restore-journal', str(journal), '--apply', code=2)
                run(path, '--restore-journal', str(journal), '--apply', '--accept-torn')
                assert path.read_bytes() == damaged
        unowned = bytearray(damaged)
        set_fat(unowned, bits, 1000, {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits])
        path.write_bytes(unowned)
        refused = root / f'{bits}-owned.MDJ'
        run(path, '--repair-fat-headers', '--apply', '--journal', str(refused), code=2)
        assert not refused.exists() and path.read_bytes() == unowned
        unknown = bytearray(clean)
        unknown[21] = 0
        path.write_bytes(unknown)
        run(path, '--repair-fat-headers', '--apply', '--journal', str(refused), code=2)
        assert not refused.exists() and path.read_bytes() == unknown
        # Normalizing headers must not select an allocation or status authority.
        mismatch = bytearray(clean)
        mismatch[starts[0]] = 0
        mismatch[starts[1]] = 0
        set_fat(mismatch, bits, 1000, {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits])
        offset = starts[1] + 1000 * bits // 8
        mismatch[offset:offset + 4] = clean[offset:offset + 4]
        path.write_bytes(mismatch)
        run(path, '--repair-fat-headers', '--apply', '--journal', str(refused), code=2)
        assert not refused.exists() and path.read_bytes() == mismatch
        if bits != 12:
            mismatch = bytearray(clean)
            mismatch[starts[0]] = mismatch[starts[1]] = 0
            mismatch[starts[0] + (3 if bits == 16 else 7)] ^= (0x40 if bits == 16 else 0x04)
            path.write_bytes(mismatch)
            run(path, '--repair-fat-headers', '--apply', '--journal', str(refused), code=2)
            assert not refused.exists() and path.read_bytes() == mismatch
        # A valid authoritative copy can repair the other header, with exact undo.
        damaged = bytearray(clean)
        damaged[starts[1]:starts[1] + width] = bytes(width)
        path.write_bytes(damaged)
        journal = root / f'{bits}-mirror.MDJ'
        run(path, '--repair-fat', '1', '--apply', '--journal', str(journal))
        run(path)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == damaged
        if bits != 12:
            for flags in range(4):
                status = bytearray(clean)
                for start in starts:
                    if bits == 16:
                        struct.pack_into('<H', status, start + 2, 0x3fff | flags << 14)
                    else:
                        struct.pack_into('<I', status, start + 4, 0x03ffffff | flags << 26)
                path.write_bytes(status)
                run(path)
                assert path.read_bytes() == status
                noop = root / f'{bits}-flags-{flags}.MDJ'
                run(path, '--repair-fat-headers', '--apply', '--journal', str(noop))
                assert not noop.exists() and path.read_bytes() == status
        if bits == 32:
            status = bytearray(clean)
            for start in starts:
                for offset in [0, 4]:
                    value = struct.unpack_from('<I', status, start + offset)[0]
                    struct.pack_into('<I', status, start + offset, value | 0xa0000000)
            path.write_bytes(status)
            run(path)
            status[starts[0]:starts[0] + 8] = bytes(8)
            for sector in [0, 6]:
                struct.pack_into('<H', status, sector * 512 + 40, 0x81)
            path.write_bytes(status)
            run(path)
            for sector in [0, 6]:
                struct.pack_into('<H', status, sector * 512 + 40, 0x80)
            path.write_bytes(status)
            assert 'FAT_HEADER' in run(path, code=1)
            journal = root / 'active-header.MDJ'
            run(path, '--repair-fat-headers', '--apply', '--journal', str(journal))
            expected = bytearray(status)
            struct.pack_into('<II', expected, starts[0], 0x0fffff00 | status[21], 0x03ffffff)
            assert path.read_bytes() == expected
            run(path)
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == status

        print(f'PASS: FAT{bits}: identical header damage, media mismatch, repair refusal, copy repair/undo and valid status bits')

    # The FAT12 limit includes reserved marker values numerically inside the
    # cluster range. Allocation scanning must still identify them as invalid.
    path = root / 'LIMIT.IMG'
    with path.open('wb') as file:
        file.truncate(4123 * 512)
    command('mkfs.fat', '-a', '-F', '12', '-s', '1', '-r', '224', '-R', '1', str(path))
    boundary = bytearray(path.read_bytes())
    layout = Layout(boundary, 12)
    assert (4123 - layout.data_sector) // layout.spc == 4084
    for copy in range(boundary[16]):
        offset = (layout.reserved + copy * layout.fatsz) * 512 + 3
        boundary[offset] = 0xf0
        boundary[offset + 1] = (boundary[offset + 1] & 0xf0) | 0x0f
    path.write_bytes(boundary)
    assert 'FAT_RANGE' in run(path, code=1)
    assert path.read_bytes() == boundary
    print('PASS: maximum FAT12 geometry still diagnoses reserved allocation values')
