"""Missing extended-container previews preserve the original disk exactly."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create

probe, exe = [Path(x).resolve() for x in sys.argv[1:3]]


def entry(kind, start, size):
    def chs(lba):
        cylinder, remainder = divmod(lba, 1008)
        head, sector = divmod(remainder, 63)
        return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])
    return bytes([0]) + chs(start) + bytes([kind]) + chs(start + size - 1) + struct.pack('<II', start, size)


with tempfile.TemporaryDirectory(prefix='magicdisk-container-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        volume, disk, output = [root / name for name in ['VOL.IMG', 'DISK.IMG', 'MBR.BIN']]
        create(volume, bits)
        data = volume.read_bytes()
        size = len(data) // 512
        records = [63, size + 189]
        total = records[-1] + 63 + size + 1
        raw = bytearray(total * 512)
        raw[:446] = bytes((i * 17 + 3) % 256 for i in range(446))
        raw[510:512] = b'\x55\xaa'
        for i, at in enumerate(records):
            raw[(at + 63) * 512:(at + 63 + size) * 512] = data
            raw[at * 512 + 446:at * 512 + 462] = entry({12: 1, 16: 6, 32: 11}[bits], 63, size)
            raw[at * 512 + 510:at * 512 + 512] = b'\x55\xaa'
            if i == 0:
                raw[at * 512 + 462:at * 512 + 478] = entry(15, records[1] - 63, total - records[1])
        disk.write_bytes(raw)

        def run(status=0, slot=2, base=63, span=total-63, kind=15, heads=16, sectors=63):
            output.unlink(missing_ok=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            result = subprocess.run([str(probe), str(disk), str(slot), str(base), str(span),
                                     str(kind), str(heads), str(sectors), str(output)],
                                    capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            if status:
                assert not output.exists()
                return
            assert 'recovered=2' in result.stdout
            return output.read_bytes()

        for kind in [5, 15, 133]:
            expected = bytearray(raw[:512])
            expected[462:478] = entry(kind, 63, total - 63)
            assert run(kind=kind) == expected
        options = ['--rebuild-container', '2', '63', str(total-63), '0F', '--chs', '16', '63']
        journal = root / f'CONT{bits}.MDJ'

        def cli(arguments, status=0):
            result = subprocess.run([str(exe), str(disk)] + arguments, capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            return result.stdout

        assert '2 recovered volumes' in cli(options)
        assert disk.read_bytes() == raw
        for extra in [['--apply'], ['--journal', str(journal)], ['--partition', '1'],
                      ['--volume-start', '126'], ['--surface'], ['--find-volumes'],
                      ['--accept-torn'], ['--boot-source', '6']]:
            cli(options + extra, 2)
            assert disk.read_bytes() == raw and not journal.exists()
        for invalid in [['--rebuild-container', '5', '63', str(total-63), '0F'],
                        ['--rebuild-container', '2', '63', str(total-63), '6'],
                        ['--rebuild-container', '2', '63', str(total-63), '0F']]:
            cli(invalid, 2)
            assert disk.read_bytes() == raw
        assert 'Container inserted and verified.' in cli(options + ['--apply', '--journal', str(journal)])
        repaired = bytearray(raw)
        repaired[462:478] = entry(15, 63, total-63)
        assert disk.read_bytes() == repaired
        assert cli(['--list']).count('type ') == 2
        saved = journal.read_bytes()
        assert len(saved) == 3152 and saved[:8] == b'MDCTR001'
        assert saved[64:576] == raw[:512] and saved[576:1088] == repaired[:512]
        for damaged in [saved[:-1], saved + b'X']:
            journal.write_bytes(damaged)
            cli(['--restore-container', str(journal), '--apply'], 2)
            assert disk.read_bytes() == repaired
        forged = bytearray(saved)
        forged[1088 + 1032 + 520 + 100] ^= 1
        forged[60:64] = bytes(4)
        struct.pack_into('<I', forged, 60, zlib.crc32(forged))
        journal.write_bytes(forged)
        cli(['--restore-container', str(journal), '--apply'], 2)
        assert disk.read_bytes() == repaired
        journal.write_bytes(saved)
        assert 'Container restored and verified.' in cli(['--restore-container', str(journal), '--apply'])
        assert disk.read_bytes() == raw
        torn = bytearray(repaired)
        torn[466] = raw[466]
        disk.write_bytes(torn)
        cli(['--restore-container', str(journal), '--apply'], 2)
        assert disk.read_bytes() == torn
        cli(['--restore-container', str(journal), '--apply', '--accept-torn'])
        assert disk.read_bytes() == raw
        for options in [dict(slot=0), dict(slot=5), dict(base=0), dict(span=0),
                        dict(span=total), dict(span=size), dict(kind=6), dict(heads=257),
                        dict(sectors=64)]:
            run(status=2, **options)
        for offset, replacement in [
                (512, b'EFI PART'), ((total-1)*512, b'EFI PART'),
                (446, entry(0xee, 1, total-1)), (446, entry(15, 63, total-63)),
                (446, entry(6, 64, 10)), (462, b'\x01'), (510, b'\x00'),
                (records[1]*512+478, b'\x01'), (records[1]*512+450, b'\x07'),
                ((records[1]+63)*512+11, b'\x00\x00'),
                (records[1]*512+462, entry(15, records[1]-63, total-records[1]))]:
            damaged = bytearray(raw)
            damaged[offset:offset+len(replacement)] = replacement
            # A missing signature is allowed only when the entire table is empty.
            if offset == 510:
                damaged[446:462] = entry(1, 1, 10)
            disk.write_bytes(damaged)
            run(status=2)
        # An unrelated primary outside the container is preserved exactly.
        good = bytearray(raw)
        good[446:462] = entry(1, 1, 10)
        disk.write_bytes(good)
        expected = bytearray(good[:512])
        expected[462:478] = entry(15, 63, total-63)
        assert run() == expected
        print(f'PASS FAT{bits}: two-volume container preview, exact bytes, preserved primary/bootstrap, bounds/GPT/overlap/loop/metadata refusals', flush=True)
