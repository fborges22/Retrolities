#!/usr/bin/env python3
"""FAT32 primary recovery: exact writes, undo identity and backup validation."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-primary-') as directory:
    root = Path(directory)
    path = root / 'volume.img'
    create(path, 32)
    original = path.read_bytes()
    backup = struct.unpack_from('<H', original, 50)[0]
    assert backup == 6
    for fault in ['zero', 'signature', 'geometry', 'code']:
        damaged = bytearray(original)
        if fault == 'zero':
            damaged[:512] = bytes(512)
        elif fault == 'signature':
            damaged[510] = 0
        elif fault == 'geometry':
            damaged[13] = 0
        else:
            damaged[100] ^= 1
        path.write_bytes(damaged)
        run(path, '--boot-source', '6', code=1, contains='BOOT_PRIMARY')
        run(path, '--repair-boot-primary', contains='Preview only')
        assert path.read_bytes() == damaged
        journal = root / (fault + '.mdj')
        run(path, '--repair-boot-primary', '--apply', '--journal', str(journal), contains='committed')
        assert path.read_bytes() == original
        run(path)
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        run(path, '--restore-journal', str(journal), '--apply')
        run(path, '--boot-source', '6', '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == damaged
    # Checksummed records cannot authorize other sectors or a forged backup.
    for fault in ['sector', 'after', 'count', 'cluster', 'marker', 'before']:
        bad = bytearray(journal.read_bytes())
        if fault == 'sector':
            struct.pack_into('<I', bad, 1088, 2)
        elif fault == 'after':
            bad[1088 + 516 + 3] ^= 1
        elif fault == 'before':
            bad[1088 + 4 + 3] ^= 1
        else:
            struct.pack_into('<I', bad, {'count': 12, 'cluster': 28, 'marker': 32}[fault], 2)
        struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
        struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
        invalid = root / 'invalid.mdj'
        invalid.write_bytes(bad)
        run(path, '--boot-source', '6', '--restore-journal', str(invalid), '--apply', code=2)
        assert path.read_bytes() == damaged
    unused = root / 'unused.mdj'
    path.write_bytes(original)
    run(path, '--repair-boot-primary', '--apply', '--journal', str(unused), contains='already up to date')
    assert not unused.exists()
    for fault in ['backup', 'mirror', 'reserved', 'source', 'primary-changed']:
        broken = bytearray(damaged)
        if fault == 'backup':
            broken[6 * 512 + 510] = 0
        elif fault == 'mirror':
            reserved = struct.unpack_from('<H', original, 14)[0]
            broken[reserved * 512 + 100] ^= 1
        elif fault == 'reserved':
            reserved = struct.unpack_from('<H', original, 14)[0]
            fatsz = struct.unpack_from('<I', original, 36)[0]
            for n in range(original[16]):
                broken[(reserved + n * fatsz) * 512] = 0
        elif fault == 'source':
            broken[6 * 512 + 100] ^= 1
        else:
            broken[101] ^= 1
        path.write_bytes(broken)
        if fault in ['source', 'primary-changed']:
            run(path, '--boot-source', '6', '--restore-journal', str(journal), '--apply', code=2)
        else:
            run(path, '--repair-boot-primary', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == broken and not unused.exists()
    # Non-default source and an MBR offset: only the partition's primary changes.
    shifted = bytearray(original)
    struct.pack_into('<H', shifted, 50, 12)
    shifted[12 * 512:13 * 512] = shifted[:512]
    shifted[13 * 512:14 * 512] = shifted[512:1024]
    shifted[:512] = bytes(512)
    mbr = bytearray(63 * 512)
    mbr[510:512] = b'\x55\xaa'
    mbr[450] = 12
    struct.pack_into('<II', mbr, 454, 63, len(shifted) // 512)
    path.write_bytes(mbr + shifted)
    partition_journal = root / 'partition.mdj'
    opts = ['--partition', '1', '--boot-source', '12']
    run(path, *opts, '--repair-boot-primary', '--apply', '--journal', str(partition_journal))
    expected = bytearray(mbr + shifted)
    expected[63 * 512:64 * 512] = shifted[12 * 512:13 * 512]
    assert path.read_bytes() == expected
    run(path, '--partition', '1', '--restore-journal', str(partition_journal), '--apply')
    run(path, *opts, '--restore-journal', str(partition_journal), '--apply')
    assert path.read_bytes() == mbr + shifted
    for source in ['0', '65535', '9999999999999999999999', '131072', '-1']:
        run(path, '--boot-source', source, code=2)
    # Backup geometry must name this source and keep it out of FSInfo/FAT data.
    for fault in ['declaration', 'overlap', 'outside']:
        broken = bytearray(original)
        if fault == 'declaration':
            struct.pack_into('<H', broken, 6 * 512 + 50, 12)
        elif fault == 'overlap':
            struct.pack_into('<H', broken, 6 * 512 + 48, 6)
        else:
            struct.pack_into('<H', broken, 6 * 512 + 14, 6)
        broken[:512] = bytes(512)
        path.write_bytes(broken)
        run(path, '--repair-boot-primary', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == broken and not unused.exists()
    path.write_bytes(original)
    run(path, '--boot-source', '6', '--repair-files', code=2)
    run(path, '--boot-source', '1', code=2)
    for bits in [12, 16]:
        create(path, bits)
        smaller = bytearray(path.read_bytes())
        smaller[6 * 512:7 * 512] = smaller[:512]
        path.write_bytes(smaller)
        run(path, '--repair-boot-primary', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == smaller and not unused.exists()


print('PASS: primary boot recovery, exact undo, independent fsck, backup/journal guards and offsets')
