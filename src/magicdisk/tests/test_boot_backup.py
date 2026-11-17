#!/usr/bin/env python3
"""Backup BPB repair: independent format/check, exact write bounds and journal guards."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-boot-') as directory:
    root = Path(directory)
    path = root / 'volume.img'
    with path.open('wb') as file:
        file.truncate(64 * 1024 * 1024)
    subprocess.run(['mkfs.fat', '-F', '32', str(path)], check=True, capture_output=True)
    original = path.read_bytes()
    backup = struct.unpack_from('<H', original, 50)[0]
    reserved = struct.unpack_from('<H', original, 14)[0]
    damaged = bytearray(original)
    damaged[backup * 512:(backup + 1) * 512] = bytes(512)
    path.write_bytes(damaged)
    run(path, code=1, contains='BOOT_BACKUP')
    run(path, '--repair-boot-backup', contains='Preview only')
    assert path.read_bytes() == damaged
    journal = root / 'boot.mdj'
    run(path, '--repair-boot-backup', '--apply', '--journal', str(journal), contains='committed')
    assert path.read_bytes() == original
    check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout + check.stderr
    unused = root / 'unused.mdj'
    run(path, '--repair-boot-backup', '--apply', '--journal', str(unused), contains='already up to date')
    assert not unused.exists()
    run(path, '--restore-journal', str(journal), '--apply')
    run(path, '--restore-journal', str(journal), '--apply')
    assert path.read_bytes() == damaged
    run(path, '--repair-boot-backup', '--apply', '--journal', str(journal), code=2,
        contains='Cannot create journal')
    assert path.read_bytes() == damaged
    # Valid checksums must not allow a backup journal to target other metadata
    # or to substitute an after-image different from the primary boot sector.
    for fault in ['sector', 'after', 'count', 'cluster', 'marker']:
        bad = bytearray(journal.read_bytes())
        if fault == 'sector':
            struct.pack_into('<I', bad, 1088, reserved)
        elif fault == 'after':
            bad[1088 + 516 + 3] ^= 1
        else:
            struct.pack_into('<I', bad, {'count': 12, 'cluster': 28, 'marker': 32}[fault], 2)
        struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
        struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
        invalid = root / 'invalid.mdj'
        invalid.write_bytes(bad)
        run(path, '--restore-journal', str(invalid), '--apply', code=2, contains='Journal is invalid')
        assert path.read_bytes() == damaged
    for fault in ['mirror', 'absent', 'overlap', 'outside', 'primary']:
        broken = bytearray(damaged)
        if fault == 'mirror':
            broken[reserved * 512 + 100] ^= 1
        elif fault == 'primary':
            broken[510] = 0
        else:
            struct.pack_into('<H', broken, 50, {'absent': 0, 'overlap': 1, 'outside': reserved}[fault])
        path.write_bytes(broken)
        run(path, '--repair-boot-backup', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == broken and not unused.exists()
    # A non-default declared backup and a partition offset must both be honored.
    shifted = bytearray(original)
    struct.pack_into('<H', shifted, 50, 12)
    shifted[12 * 512:13 * 512] = bytes(512)
    shifted[13 * 512:14 * 512] = original[512:1024]
    mbr = bytearray(63 * 512)
    mbr[510:512] = b'\x55\xaa'
    mbr[450] = 12
    struct.pack_into('<II', mbr, 454, 63, len(shifted) // 512)
    path.write_bytes(mbr + shifted)
    partition_journal = root / 'partition.mdj'
    run(path, '--partition', '1', '--repair-boot-backup', contains='Metadata disk LBA 75')
    run(path, '--partition', '1', '--repair-boot-backup', '--apply', '--journal', str(partition_journal))
    expected = bytearray(mbr + shifted)
    expected[75 * 512:76 * 512] = shifted[:512]
    assert path.read_bytes() == expected
    run(path, '--partition', '1', '--restore-journal', str(partition_journal), '--apply')
    assert path.read_bytes() == mbr + shifted
    # No FAT12/16 boot-sector fallback is guessed.
    with path.open('wb') as file:
        file.truncate(1440 * 1024)
    subprocess.run(['mkfs.fat', '-F', '12', str(path)], check=True, capture_output=True)
    run(path, '--repair-boot-backup', code=2, contains='requires a FAT32 volume')

print('PASS: backup boot preview, exact repair/undo, journal guards, bounds, partition offsets and fsck')
