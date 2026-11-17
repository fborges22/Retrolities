"""Independent MBR preview bytes and refusal cases; source is never written."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create

probe, exe = [Path(x).resolve() for x in sys.argv[1:3]]

def chs(lba, heads=16, spt=63):
    cylinder, rest = divmod(lba, heads * spt)
    head, sector = divmod(rest, spt)
    return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])

with tempfile.TemporaryDirectory(prefix='magicdisk-mbr-preview-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume, disk, output = [root / x for x in ['VOLUME.IMG', 'DISK.IMG', 'MBR.BIN']]
        create(volume, bits)
        data = volume.read_bytes()
        sectors = len(data) // 512
        blank = bytearray(63 * 512)
        blank[:446] = bytes((i * 17 + 3) % 256 for i in range(446))
        disk.write_bytes(blank + data)

        def run(slot=1, start=63, source=0, heads=16, spt=63, status=0):
            output.unlink(missing_ok=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            result = subprocess.run([str(probe), str(disk), str(start), str(slot), str(source),
                                     str(heads), str(spt), str(output)], capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            if status:
                assert not output.exists()
                return result.stdout
            return output.read_bytes()

        for slot in [1, 2, 3, 4]:
            proposed = run(slot=slot)
            expected = bytearray(blank[:512])
            offset = 446 + (slot - 1) * 16
            entry = bytearray(16)
            entry[1:4] = chs(63)
            entry[4] = {12: 1, 16: 6, 32: 11}[bits]
            entry[5:8] = chs(63 + sectors - 1)
            struct.pack_into('<II', entry, 8, 63, sectors)
            expected[offset:offset + 16] = entry
            expected[510:512] = b'\x55\xaa'
            assert proposed == expected
            candidate = root / 'PROPOSED.IMG'
            candidate.write_bytes(proposed + disk.read_bytes()[512:])
            checked = subprocess.run([str(exe), str(candidate), '--partition', '1'], capture_output=True, text=True)
            assert checked.returncode == 0, checked.stdout
        for options in [{'slot':0}, {'slot':5}, {'heads':0}, {'heads':257}, {'spt':0}, {'spt':64}, {'start':0}, {'start':0xffffffff}]:
            run(status=2, **options)
        # Occupied slots and inactive-entry garbage must never be overwritten.
        damaged = bytearray(blank + data)
        damaged[446] = 1
        disk.write_bytes(damaged)
        assert 'ambiguous' in run(status=2)
        damaged[446] = 0
        damaged[450] = 6
        struct.pack_into('<II', damaged, 454, 63, sectors)
        damaged[510:512] = b'\x55\xaa'
        disk.write_bytes(damaged)
        assert 'empty' in run(status=2)
        assert 'Overlapping' in run(slot=2, status=2)
        damaged[450] = 15
        disk.write_bytes(damaged)
        assert 'extended container' in run(slot=2, status=2)
        damaged = bytearray(blank + data)
        damaged[512:520] = b'EFI PART'
        disk.write_bytes(damaged)
        assert 'GPT' in run(status=2)
        damaged = bytearray(blank + data)
        damaged[-512:-504] = b'EFI PART'
        disk.write_bytes(damaged)
        assert 'GPT' in run(status=2)
        damaged = bytearray(blank + data)
        damaged[450] = 0xee
        disk.write_bytes(damaged)
        assert 'GPT' in run(slot=2, status=2)
        disk.write_bytes(data)
        assert 'Sector zero is a FAT volume' in run(status=2)
        if bits == 32:
            damaged = bytearray(blank + data)
            damaged[63 * 512 + 510:63 * 512 + 512] = bytes(2)
            disk.write_bytes(damaged)
            assert 'corroborated' in run(status=2)
            assert run(source=6)[450] == 11
        print(f'PASS FAT{bits}: four slots, exact CHS/LBA/type/signature bytes, selectable result, refusals, source preserved')
