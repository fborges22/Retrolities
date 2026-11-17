"""Read-only discovery with missing entries, backups, decoys and cancellation."""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
from directory_fixture import create

probe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-volume-search-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data = volume.read_bytes()
        disk = root / 'DISK.IMG'
        original = bytes(63 * 512) + data
        disk.write_bytes(original)
        size = len(original) // 512

        def run(first=0, count=None, cancel=False, status=0, mode=None):
            before = hashlib.sha256(disk.read_bytes()).digest()
            arguments = [str(probe), str(disk), str(first), str(size if count is None else count)]
            if cancel or mode:
                arguments.append('cancel' if cancel else mode)
            result = subprocess.run(arguments, capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            return result.stdout

        expected = f'CANDIDATE 63 {len(data) // 512} {bits} 0\n'
        output = run()
        assert output.count('CANDIDATE') == 1 and expected in output, output
        assert 'COUNT 0' in run(count=63)
        assert expected in run(first=63, count=1)
        assert 'incomplete' in run(cancel=True, status=1)
        assert expected in run(mode='fault', status=1), 'Search stopped before surviving volume'
        assert 'stopped by visitor' in run(mode='stop', status=1)
        assert 'Invalid volume-search range' in run(first=size, count=1, status=1)
        # A plausible standalone boot sector with no matching FAT is a decoy.
        damaged = bytearray(original)
        reserved = int.from_bytes(data[14:16], 'little')
        fat_size = int.from_bytes(data[22:24], 'little') or int.from_bytes(data[36:40], 'little')
        for copy in range(data[16]):
            at = (63 + reserved + copy * fat_size) * 512
            damaged[at:at + 8] = bytes(8)
        disk.write_bytes(damaged)
        assert 'COUNT 0' in run(), 'Accepted boot geometry without FAT evidence'
        # One damaged mirror must not hide a candidate supported by another.
        at = (63 + reserved) * 512
        damaged[at:at + 8] = original[at:at + 8]
        disk.write_bytes(damaged)
        assert expected in run()
        if bits == 32:
            damaged = bytearray(original)
            damaged[63 * 512 + 510:63 * 512 + 512] = bytes(2)
            disk.write_bytes(damaged)
            output = run()
            assert output.count('CANDIDATE') == 1, output
            assert f'CANDIDATE 63 {len(data) // 512} 32 6\n' in output, output
            assert 'CANDIDATE 63' in run(first=69, count=1)
        print(f'PASS FAT{bits}: missing MBR, bounded search, FAT evidence, cancellation, unchanged source')
