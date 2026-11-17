#!/usr/bin/env python3
"""Reclaim orphan graphs, preserve live/data bytes, and undo original metadata."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, Layout
from lost_fixture import lost_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-reclaim-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        clean = path.read_bytes()
        broken, payloads = lost_fixture(clean, bits)
        if bits == 32:
            # Free-space signatures can be rebuilt while reclaiming orphan allocation.
            broken[512:1024] = bytes(512)
            broken[7 * 512:8 * 512] = bytes(512)
        path.write_bytes(broken)
        run(path, '--reclaim-lost', contains='Reclaim 13 lost clusters')
        assert path.read_bytes() == broken
        journal = root / f'{bits}.mdj'
        run(path, '--reclaim-lost', '--apply', '--journal', str(journal), contains='committed')
        repaired = path.read_bytes()
        after = Layout(repaired, bits)
        for cluster in payloads:
            assert after.fat(cluster) == 0
        assert after.fat(1011) == {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits]
        assert repaired[layout.root_sector * 512:] == broken[layout.root_sector * 512:]
        run(path, contains='0 issues')
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        raw = journal.read_bytes()
        version, count = struct.unpack_from('<II', raw, 8)
        assert version == 4 and count >= 2
        unused = root / f'unused{bits}.mdj'
        run(path, '--reclaim-lost', '--apply', '--journal', str(unused), contains='already up to date')
        assert not unused.exists()
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # Restore a transaction interrupted between the FAT copies.
        partial = bytearray(broken)
        record = 1088
        sector = struct.unpack_from('<I', raw, record)[0]
        partial[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
        path.write_bytes(partial)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # New live references outside journal sectors must prevent reclaim undo.
        changed = bytearray(repaired)
        slot = next(pos for pos in after.slots() if changed[pos] == 0)
        changed[slot:slot + 32] = bytes(32)
        changed[slot:slot + 11] = b'NEWFILE BIN'
        changed[slot + 11] = 32
        struct.pack_into('<H', changed, slot + 26, 1000)
        struct.pack_into('<I', changed, slot + 28, 3 * layout.spc * 512)
        path.write_bytes(changed)
        run(path, '--restore-journal', str(journal), '--apply', code=2, contains='ownership')
        assert path.read_bytes() == changed
        run(path, '--reclaim-lost', code=2, contains='ownership scan')
        # Checksummed but semantically altered journals cannot inject allocation changes.
        path.write_bytes(broken)
        for fault in ['count', 'after', 'version']:
            bad = bytearray(raw)
            if fault == 'count':
                struct.pack_into('<I', bad, 28, 12)
            elif fault == 'after':
                bad[1088 + 516] ^= 1
            else:
                struct.pack_into('<I', bad, 8, 3)
            struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
            struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
            invalid = root / 'invalid.mdj'
            invalid.write_bytes(bad)
            run(path, '--restore-journal', str(invalid), '--apply', code=2)
            assert path.read_bytes() == broken
        if bits == 32:
            active = bytearray(broken)
            for sector in [0, 6]:
                struct.pack_into('<H', active, sector * 512 + 40, 0x81)
            # Disabled mirrors remain untouched; only FAT 2 is authoritative.
            start = layout.reserved * 512
            active[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
            second = (layout.reserved + layout.fatsz) * 512
            entry = struct.unpack_from('<I', active, second + 1000 * 4)[0]
            struct.pack_into('<I', active, second + 1000 * 4, entry | 0xa0000000)
            path.write_bytes(active)
            saved = root / 'active.mdj'
            run(path, '--reclaim-lost', '--apply', '--journal', str(saved))
            changed = path.read_bytes()
            assert changed[start:second] == active[start:second]
            assert struct.unpack_from('<I', changed, second + 1000 * 4)[0] == 0xa0000000
            run(path, contains='0 issues')
            run(path, '--restore-journal', str(saved), '--apply')
            assert path.read_bytes() == active
        if bits != 12:
            scattered = bytearray(clean)
            clusters = list(range(1000, 12000, 128))
            for cluster in clusters:
                assert layout.fat(cluster) == 0
                for copy in range(scattered[16]):
                    offset = (layout.reserved + copy * layout.fatsz) * 512 + cluster * (bits // 8)
                    struct.pack_into('<H' if bits == 16 else '<I', scattered, offset,
                                     0xffff if bits == 16 else 0x0fffffff)
            path.write_bytes(scattered)
            saved = root / f'scattered{bits}.mdj'
            run(path, '--reclaim-lost', '--apply', '--journal', str(saved),
                contains=f'Reclaim {len(clusters)} lost clusters')
            assert struct.unpack_from('<I', saved.read_bytes(), 12)[0] > 16
            run(path, contains='0 issues')
            run(path, '--restore-journal', str(saved), '--apply')
            assert path.read_bytes() == scattered
        inconsistent = bytearray(broken)
        inconsistent[layout.reserved * 512 + 100] ^= 1
        path.write_bytes(inconsistent)
        run(path, '--reclaim-lost', code=2, contains='consistent FAT copies')
        assert path.read_bytes() == inconsistent

print('PASS: FAT12/16/32 orphan reclaim, data preservation, fsck, exact/partial undo and ownership guards')
