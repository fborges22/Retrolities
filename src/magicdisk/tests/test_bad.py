#!/usr/bin/env python3
"""Marker changes, FAT variants, journaling and interrupted-write restoration."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    p = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert p.returncode == code, p.stdout + p.stderr
    if contains:
        assert contains in p.stdout, p.stdout
    return p.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-bad-') as directory:
    root = Path(directory)
    for bits, size in [(12, 1440 * 1024), (16, 32 * 1024 * 1024), (32, 64 * 1024 * 1024)]:
        path = root / f'fat{bits}.img'
        with path.open('wb') as f:
            f.truncate(size)
        subprocess.run(['mkfs.fat', '-F', str(bits), str(path)], check=True, capture_output=True)
        original = path.read_bytes()
        if bits == 12:
            # Entry 340 shares a FAT12 byte with 341. It belongs to a file.
            data = bytearray(original)
            reserved = struct.unpack_from('<H', data, 14)[0]
            fatsz = struct.unpack_from('<H', data, 22)[0]
            for copy in range(data[16]):
                off = (reserved + copy * fatsz) * 512 + 510
                value = struct.unpack_from('<H', data, off)[0]
                struct.pack_into('<H', data, off, (value & 0xf000) | 0xfff)
            off = (reserved + data[16] * fatsz) * 512
            data[off:off + 11] = b'NEIGHBORBIN'
            data[off + 11] = 32
            struct.pack_into('<H', data, off + 26, 340)
            struct.pack_into('<I', data, off + 28, 512)
            path.write_bytes(data)
            original = bytes(data)
        if bits == 32:
            # High FAT32 bits belong to the filesystem and must be preserved.
            data = bytearray(original)
            reserved = struct.unpack_from('<H', data, 14)[0]
            fatsz = struct.unpack_from('<I', data, 36)[0]
            for copy in range(data[16]):
                struct.pack_into('<I', data, (reserved + copy * fatsz) * 512 + 341 * 4, 0xa0000000)
            path.write_bytes(data)
            original = bytes(data)
        reserved = struct.unpack_from('<H', original, 14)[0]
        fatsz = struct.unpack_from('<H', original, 22)[0] or struct.unpack_from('<I', original, 36)[0]
        rootsz = (struct.unpack_from('<H', original, 17)[0] * 32 + 511) // 512
        target_sector = reserved + original[16] * fatsz + rootsz + 339 * original[13]
        run(path, '--mark-sector', str(target_sector), contains='cluster 341;')
        run(path, '--mark-sector', '0', code=2, contains='metadata cannot be marked')
        run(path, '--mark-bad', '341', contains='Preview only')
        assert path.read_bytes() == original
        mark = root / f'mark{bits}.mdj'
        run(path, '--mark-bad', '341', '--apply', '--journal', str(mark), contains='committed')
        marked = path.read_bytes()
        assert marked != original
        run(path, contains='1 bad')
        run(path, '--retest-bad', contains='1 marked, 1 readable, 0 still failing')
        assert path.read_bytes() == marked
        run(path, '--mark-bad', '341', code=2, contains='Only free')
        run(path, '--unmark-bad', '341', contains='Preview only')
        assert path.read_bytes() == marked
        undo = root / f'undo{bits}.mdj'
        run(path, '--unmark-bad', '341', '--apply', '--journal', str(undo))
        run(path, contains='0 bad')
        # FSInfo next-free hint is intentionally reset to unknown, so use the
        # journal to prove byte-for-byte reversibility, including hints.
        run(path, '--restore-journal', str(undo), '--apply')
        assert path.read_bytes() == marked
        run(path, '--restore-journal', str(mark), '--apply')
        assert path.read_bytes() == original
        run(path, '--restore-journal', str(mark), '--apply')
        assert path.read_bytes() == original
        run(path, '--mark-bad', '341', '--apply', '--journal', str(mark), code=2,
            contains='Cannot create journal')
        assert path.read_bytes() == original

        # Simulate interruption after the first sector was written.
        journal = mark.read_bytes()
        record = journal[1088:2120]
        sector = struct.unpack_from('<I', record)[0]
        with path.open('r+b') as f:
            f.seek(sector * 512)
            f.write(record[516:1028])
        run(path, '--restore-journal', str(mark), '--apply')
        assert path.read_bytes() == original
        broken = root / f'broken{bits}.mdj'
        broken.write_bytes(journal[:-1])
        run(path, '--restore-journal', str(broken), '--apply', code=2, contains='Journal is invalid')
        corrupt = bytearray(journal)
        corrupt[1200] ^= 1
        broken.write_bytes(corrupt)
        run(path, '--restore-journal', str(broken), '--apply', code=2, contains='checksum')
        assert path.read_bytes() == original
        wrong_identity = bytearray(original)
        wrong_identity[3] ^= 1  # OEM byte: geometry still mounts, identity no longer matches.
        path.write_bytes(wrong_identity)
        run(path, '--restore-journal', str(mark), '--apply', code=2, contains='identity changed')
        assert path.read_bytes() == wrong_identity
        changed = bytearray(original)
        changed[sector * 512 + 100] ^= 1
        path.write_bytes(changed)
        run(path, '--restore-journal', str(mark), '--apply', code=2, contains='unrelated changes')
        assert path.read_bytes() == changed

    # FAT32 can disable mirroring and select a nonzero active FAT.
    path.write_bytes(original)
    data = bytearray(original)
    backup = struct.unpack_from('<H', data, 50)[0]
    struct.pack_into('<H', data, 40, 0x81)
    struct.pack_into('<H', data, backup * 512 + 40, 0x81)
    reserved = struct.unpack_from('<H', data, 14)[0]
    fatsz = struct.unpack_from('<I', data, 36)[0]
    data[reserved * 512 + 100] ^= 0x33  # stale inactive FAT contents
    path.write_bytes(data)
    inactive = bytes(data[reserved * 512:(reserved + fatsz) * 512])
    run(path, '--mark-bad', '341', '--apply', '--journal', str(root / 'active.mdj'))
    changed = path.read_bytes()
    assert changed[reserved * 512:(reserved + fatsz) * 512] == inactive
    assert struct.unpack_from('<I', changed, (reserved + fatsz) * 512 + 341 * 4)[0] == 0xaffffff7
    run(path, contains='1 bad')

    # An allocated cluster must never be detached by marking it bad.
    run(path, '--mark-bad', '2', code=2, contains='Only free')
    run(path, '--mark-bad', '1', code=2)
    run(path, '--mark-bad', '341', '--apply', code=2)
    run(path, '--restore-journal', str(mark), code=2)
    # Absolute disk LBAs include the selected partition's start offset.
    volume = root / 'volume.img'
    with volume.open('wb') as f:
        f.truncate(1440 * 1024)
    subprocess.run(['mkfs.fat', '-F', '12', str(volume)], check=True, capture_output=True)
    disk = root / 'partitioned.img'
    mbr = bytearray(63 * 512)
    mbr[510:512] = b'\x55\xaa'
    mbr[450] = 1
    struct.pack_into('<II', mbr, 454, 63, 2880)
    original_disk = bytes(mbr) + volume.read_bytes()
    disk.write_bytes(original_disk)
    journal = root / 'partition.mdj'
    run(disk, '--partition', '1', '--mark-sector', '435', '--apply', '--journal', str(journal),
        contains='cluster 341; disk sectors 435-435')
    run(disk, '--partition', '1', '--retest-bad', contains='1 marked, 1 readable')
    assert disk.read_bytes()[:63 * 512] == mbr
    run(disk, '--partition', '1', '--restore-journal', str(journal), '--apply')
    assert disk.read_bytes() == original_disk
print('PASS: FAT12/16/32 marker preview/apply/retest/undo, journals and interruption recovery')
