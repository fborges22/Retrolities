#!/usr/bin/env python3
"""Choose a structurally consistent FAT, journal every difference, and undo exactly."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, Layout

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-mirrors-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        original = path.read_bytes()
        # Destroy every sector of the second FAT, including its reserved entries.
        broken = bytearray(original)
        target = (layout.reserved + layout.fatsz) * 512
        broken[target:target + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
        if bits == 32:
            info = struct.unpack_from('<H', original, 48)[0]
            backup = struct.unpack_from('<H', original, 50)[0]
            for sector in [info, backup + info]:
                broken[sector * 512:(sector + 1) * 512] = b'\x5a' * 512
        path.write_bytes(broken)
        run(path, '--repair-fat', '2', code=2, contains='invalid reserved entries')
        run(path, '--repair-fat', '1', contains='Preview only')
        assert path.read_bytes() == broken
        journal = root / f'fat{bits}.mdj'
        run(path, '--repair-fat', '1', '--apply', '--journal', str(journal), contains='committed')
        repaired = path.read_bytes()
        assert repaired[layout.reserved * 512:target] == original[layout.reserved * 512:target]
        assert repaired[target:target + layout.fatsz * 512] == repaired[layout.reserved * 512:target]
        assert repaired[layout.root_sector * 512:] == original[layout.root_sector * 512:]
        run(path, contains='0 issues')
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        raw = journal.read_bytes()
        version, count = struct.unpack_from('<II', raw, 8)
        assert version == 3 and count >= layout.fatsz
        if bits != 12:
            assert count > 16, count
        unused = root / f'unused{bits}.mdj'
        run(path, '--repair-fat', '1', '--apply', '--journal', str(unused), contains='already up to date')
        assert not unused.exists()
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # A partially completed large transaction is recoverable.
        partial = bytearray(broken)
        for index in range(count // 2):
            record = 1088 + index * 1032
            sector = struct.unpack_from('<I', raw, record)[0]
            partial[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
        path.write_bytes(partial)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # The secondary FAT can be authoritative when the first is destroyed.
        reverse = bytearray(original)
        reverse[layout.reserved * 512:target] = b'\x5a' * (layout.fatsz * 512)
        path.write_bytes(reverse)
        reverse_journal = root / f'reverse{bits}.mdj'
        run(path, '--repair-fat', '2', '--apply', '--journal', str(reverse_journal))
        run(path, contains='0 issues')
        run(path, '--restore-journal', str(reverse_journal), '--apply')
        assert path.read_bytes() == reverse
        # Invalid large counts and source-target overlap fail even with valid CRCs.
        path.write_bytes(broken)
        for fault in ['count', 'source', 'destination', 'after', 'version']:
            bad = bytearray(raw)
            if fault == 'count':
                struct.pack_into('<I', bad, 12, 0xffffffff)
            elif fault == 'source':
                struct.pack_into('<I', bad, 28, 2)
            elif fault == 'destination':
                struct.pack_into('<I', bad, 1088, layout.reserved)
            elif fault == 'after':
                bad[1088 + 516] ^= 1
            else:
                struct.pack_into('<I', bad, 8, 2)
            struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
            struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
            invalid = root / 'invalid.mdj'
            invalid.write_bytes(bad)
            run(path, '--restore-journal', str(invalid), '--apply', code=2, contains='Journal is invalid')
            assert path.read_bytes() == broken
        # Preserve the reserved entries but break the chosen source's file chains.
        invalid_source = bytearray(broken)
        first = layout.reserved * 512
        invalid_source[first + 8:first + 512] = bytes(504)
        path.write_bytes(invalid_source)
        run(path, '--repair-fat', '1', code=2, contains='consistent filesystem')
        assert path.read_bytes() == invalid_source
        if bits == 32:
            disabled = bytearray(original)
            struct.pack_into('<H', disabled, 40, 0x81)
            path.write_bytes(disabled)
            run(path, '--repair-fat', '2', code=2, contains='mirroring enabled')

print('PASS: full FAT12/16/32 copy repair, large journals, either source, exact/partial undo, guards and fsck')
