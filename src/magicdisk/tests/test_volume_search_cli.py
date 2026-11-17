"""Discover and inspect intact volumes without partition entries; reject writes."""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
from directory_fixture import create, PARENT, CHILD, UNICODE_FILE

exe = Path(sys.argv[1]).resolve()
def verify_export(directory):
    manifest = (directory / 'REC00001/RECOVERY.TXT').read_text()
    names = set()
    payloads = []
    for line in manifest.splitlines():
        if line.startswith('LIVE_BEGIN '):
            names.add(bytes.fromhex(line.split('path_hex=')[1]).decode('utf-8'))
        elif line.startswith('FILE_BEGIN '):
            payloads.append((directory / 'REC00001' / line.split()[1]).read_bytes())
    expected = {f'{PARENT}/{CHILD}/Long report number {i:02d}.txt' for i in range(50)}
    expected.update([f'{PARENT}/{CHILD}/' + 'a' * 13,
                     f'{PARENT}/{CHILD}/' + 'b' * 26,
                     'x' * 255, f'{PARENT}/{CHILD}/{UNICODE_FILE}'])
    assert names == expected and len(payloads) == 54, names
    assert all(data == b'MagicDisk LFN preservation test.\r\n' for data in payloads)


with tempfile.TemporaryDirectory(prefix='magicdisk-discovery-cli-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data = volume.read_bytes()
        disk = root / 'DISK.IMG'
        disk.write_bytes(bytes(63 * 512) + data)
        before = hashlib.sha256(disk.read_bytes()).digest()
        def run(*args, status=0):
            result = subprocess.run([str(exe), str(disk), *args], capture_output=True, text=True)
            assert result.returncode == status, (args, result.stdout, result.stderr)
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            return result.stdout
        assert f'FAT{bits}  LBA 63' in run('--find-volumes', '--search-count', '72')
        assert f'FAT{bits} |' in run('--volume-start', '63')
        destination = root / f'OUT{bits}'
        destination.mkdir()
        run('--volume-start', '63', '--export-files', str(destination))
        verify_export(destination)
        for options in [('--find-volumes', '--partition', '1'),
                        ('--find-volumes', '--volume-start', '63'),
                        ('--search-first', '0'),
                        ('--find-volumes', '--search-count', '0'),
                        ('--find-volumes', '--search-first', '4294967295'),
                        ('--volume-start', '4294967295'),
                        ('--volume-start', '-1'),
                        ('--volume-start', '63', '--partition', '1'),
                        ('--volume-start', '63', '--repair-fsinfo', '--apply', '--journal', str(root/'NO.MDJ'))]:
            run(*options, status=2)
        assert not (root / 'NO.MDJ').exists()
        if bits == 32:
            broken = bytearray(disk.read_bytes())
            broken[63 * 512 + 510:63 * 512 + 512] = bytes(2)
            disk.write_bytes(broken)
            before = hashlib.sha256(broken).digest()
            assert 'FAT32  LBA 63' in run('--find-volumes', '--search-count', '72')
            assert 'boot-source 6' in run('--find-volumes', '--search-count', '72')
            assert 'BOOT_PRIMARY' in run('--volume-start', '63', '--boot-source', '6', status=1)
            destination = root / 'BACKUP'
            destination.mkdir()
            run('--volume-start', '63', '--boot-source', '6', '--export-files', str(destination), status=1)
            verify_export(destination)
        print(f'PASS FAT{bits}: CLI discovery/inspection, strict options, write refusal, exact source')
