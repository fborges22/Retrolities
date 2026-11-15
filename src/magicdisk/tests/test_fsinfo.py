#!/usr/bin/env python3
"""Rebuild FAT32 hints from allocation, retaining exact journal undo."""
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


with tempfile.TemporaryDirectory(prefix='magicdisk-fsinfo-') as directory:
    root = Path(directory)
    path = root / 'fat32.img'
    with path.open('wb') as file:
        file.truncate(64 * 1024 * 1024)
    subprocess.run(['mkfs.fat', '-F', '32', str(path)], check=True, capture_output=True)
    original = path.read_bytes()
    info = struct.unpack_from('<H', original, 48)[0]
    backup = struct.unpack_from('<H', original, 50)[0]
    reserved = struct.unpack_from('<H', original, 14)[0]
    fatsz = struct.unpack_from('<I', original, 36)[0]
    data_sector = reserved + original[16] * fatsz
    clusters = (len(original) // 512 - data_sector) // original[13]
    damaged = bytearray(original)
    for sector in (info, backup + info):
        damaged[sector * 512:(sector + 1) * 512] = b'\x5a' * 512
    path.write_bytes(damaged)
    run(path, code=1, contains='FSINFO_SIGNATURE')
    run(path, '--repair-fsinfo', contains='2 metadata sectors')
    assert path.read_bytes() == damaged
    journal = root / 'repair.mdj'
    run(path, '--repair-fsinfo', '--apply', '--journal', str(journal), contains='committed')
    repaired = path.read_bytes()
    changed = [sector for sector in range(len(original) // 512)
               if damaged[sector * 512:(sector + 1) * 512] !=
               repaired[sector * 512:(sector + 1) * 512]]
    assert changed == [info, backup + info], changed
    for sector in changed:
        block = repaired[sector * 512:(sector + 1) * 512]
        assert struct.unpack_from('<I', block)[0] == 0x41615252
        assert struct.unpack_from('<IIII', block, 484)[:3] == (0x61417272, clusters - 1, 3)
        assert block[4:484] == bytes(480) and block[496:508] == bytes(12)
        assert struct.unpack_from('<I', block, 508)[0] == 0xaa550000
    run(path, contains='0 issues')
    unused = root / 'unused.mdj'
    run(path, '--repair-fsinfo', '--apply', '--journal', str(unused), contains='already up to date')
    assert not unused.exists() and path.read_bytes() == repaired
    # Validate with an independent filesystem checker, without writing.
    check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
    assert check.returncode == 0, check.stdout + check.stderr
    run(path, '--restore-journal', str(journal), '--apply')
    assert path.read_bytes() == damaged
    run(path, '--repair-fsinfo', '--apply', '--journal', str(journal), code=2,
        contains='Cannot create journal')
    assert path.read_bytes() == damaged
    # A valid CRC must not allow an unknown journal operation/version.
    journal_bytes = bytearray(journal.read_bytes())
    assert struct.unpack_from('<I', journal_bytes, 8)[0] == 2
    for offset in [8, 36]:
        bad = bytearray(journal_bytes)
        struct.pack_into('<I', bad, offset, 99)
        crc = zlib.crc32(bad[:60])
        crc = zlib.crc32(bad[64:1088], crc)
        struct.pack_into('<I', bad, 60, crc)
        invalid = root / 'invalid.mdj'
        invalid.write_bytes(bad)
        run(path, '--restore-journal', str(invalid), '--apply', code=2, contains='Journal is invalid')
        assert path.read_bytes() == damaged
    # Restore a transaction interrupted after its first sector.
    partial = bytearray(damaged)
    partial[info * 512:(info + 1) * 512] = repaired[info * 512:(info + 1) * 512]
    path.write_bytes(partial)
    run(path, '--restore-journal', str(journal), '--apply')
    assert path.read_bytes() == damaged
    # Structural faults must not be hidden by a successful hint rebuild.
    for fault in ['mirror', 'lost']:
        broken = bytearray(damaged)
        if fault == 'mirror':
            broken[reserved * 512 + 100] ^= 1
        else:
            for copy in range(original[16]):
                struct.pack_into('<I', broken, (reserved + copy * fatsz) * 512 + 40, 0x0fffffff)
        path.write_bytes(broken)
        run(path, '--repair-fsinfo', code=2, contains='no issues outside FAT32 auxiliary metadata')
        assert path.read_bytes() == broken
    # Bounds/overlap checks happen before any repair plan can be applied.
    for info_location in [0, 0xffff, backup, reserved - 1]:
        broken = bytearray(original)
        struct.pack_into('<H', broken, 48, info_location)
        struct.pack_into('<H', broken, backup * 512 + 48, info_location)
        path.write_bytes(broken)
        run(path, '--repair-fsinfo', code=2)
        assert path.read_bytes() == broken
    # No backup is allowed, and a nonzero active FAT is authoritative.
    active = bytearray(damaged)
    struct.pack_into('<H', active, 50, 0)
    struct.pack_into('<H', active, 40, 0x81)
    active[reserved * 512 + 100] ^= 1
    path.write_bytes(active)
    run(path, '--repair-fsinfo', '--apply', '--journal', str(root / 'active.mdj'),
        contains='1 metadata sectors')
    result = path.read_bytes()
    assert result[:512] == active[:512] and result[1024:] == active[1024:]
    run(path, contains='0 issues')
    # Advisory backup counts may be stale without diagnosing structural damage.
    advisory = bytearray(repaired)
    struct.pack_into('<I', advisory, (backup + info) * 512 + 488, 0)
    path.write_bytes(advisory)
    run(path, contains='0 issues')
    run(path, '--repair-fsinfo', contains='1 metadata sectors')
    # Invalid signatures and reserved bytes in the backup are still diagnosed.
    advisory[(backup + info) * 512 + 10] = 1
    path.write_bytes(advisory)
    run(path, code=1, contains='FSINFO_RESERVED')

print('PASS: FSInfo preview, rebuild, no-op, undo, bounds, active FAT and unrelated-damage refusal')
