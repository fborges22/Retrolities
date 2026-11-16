"""A missing link hides two intact volumes; preview restores only its sixteen bytes."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create

probe, exe = [Path(x).resolve() for x in sys.argv[1:3]]

def chs(lba):
    cylinder, rem = divmod(lba, 1008)
    head, sector = divmod(rem, 63)
    return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])

def entry(kind, absolute, relative, count):
    return bytes([0]) + chs(absolute) + bytes([kind]) + chs(absolute + count - 1) + struct.pack('<II', relative, count)

with tempfile.TemporaryDirectory(prefix='magicdisk-ebr-link-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume, disk, output = [root / n for n in ['VOL.IMG', 'DISK.IMG', 'LINK.BIN']]
        create(volume, bits)
        data = volume.read_bytes()
        size = len(data) // 512
        records = [63 + i * (size + 126) for i in range(3)]
        total = records[-1] + 63 + size + 1
        raw = bytearray(total * 512)
        raw[446:462] = entry(15, 63, 63, total - 63)
        raw[510:512] = b'\x55\xaa'
        for i, record in enumerate(records):
            start = record + 63
            raw[start * 512:(start + size) * 512] = data
            raw[record * 512:record * 512 + 446] = bytes((j * 11 + i) % 256 for j in range(446))
            raw[record * 512 + 446:record * 512 + 462] = entry({12:1,16:6,32:11}[bits], start, 63, size)
            raw[record * 512 + 510:record * 512 + 512] = b'\x55\xaa'
            if i < 2:
                target = records[i + 1]
                raw[record * 512 + 462:record * 512 + 478] = entry(15, target, target - 63, total - target)
        damaged = bytearray(raw)
        damaged[63 * 512 + 462:63 * 512 + 478] = bytes(16)
        disk.write_bytes(damaged)
        listed = subprocess.run([str(exe), str(disk), '--list'], capture_output=True, text=True)
        assert listed.returncode == 0 and listed.stdout.count('type ') == 1, listed.stdout

        def run(source=63, target=records[1], status=0):
            output.unlink(missing_ok=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            result = subprocess.run([str(probe), str(disk), str(source), str(target), '16', '63', str(output)], capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            if status:
                assert not output.exists()
                return result.stdout
            assert 'recovered=2' in result.stdout
            return output.read_bytes()

        proposed = run()
        assert proposed == raw[63 * 512:64 * 512]
        repaired = bytearray(damaged)
        repaired[63 * 512:64 * 512] = proposed
        assert repaired == raw
        options = ['--link-ebr', '63', str(records[1]), '--chs', '16', '63']
        journal = root / f'LINK{bits}.MDJ'

        def cli(arguments, status=0):
            result = subprocess.run([str(exe), str(disk)] + arguments, capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            return result.stdout

        preview = cli(options)
        assert '2 recovered volumes' in preview and 'Preview only' in preview
        assert disk.read_bytes() == damaged and not journal.exists()
        for extra in [['--apply'], ['--journal', str(journal)], ['--volume-start', '126'],
                      ['--partition', '1'], ['--surface'], ['--find-volumes'], ['--accept-torn']]:
            cli(options + extra, 2)
            assert disk.read_bytes() == damaged and not journal.exists()
        assert 'link inserted and verified' in cli(options + ['--apply', '--journal', str(journal)])
        assert disk.read_bytes() == raw
        saved = journal.read_bytes()
        assert len(saved) == 3664 and saved[:8] == b'MDLNK001'
        assert saved[64:576] == raw[:512]
        assert saved[576:1088] == damaged[63 * 512:64 * 512]
        assert saved[1088:1600] == raw[63 * 512:64 * 512]
        forged = bytearray(saved)
        forged[1600 + 1032 + 520 + 100] ^= 1
        forged[60:64] = bytes(4)
        struct.pack_into('<I', forged, 60, zlib.crc32(forged))
        for invalid in [saved[:63], saved[:-1], saved + b'X', forged]:
            journal.write_bytes(invalid)
            cli(['--restore-link', str(journal), '--apply'], 2)
            assert disk.read_bytes() == raw
        journal.write_bytes(saved)
        assert 'link restored and verified' in cli(['--restore-link', str(journal), '--apply'])
        assert disk.read_bytes() == damaged
        torn = bytearray(raw)
        torn[63 * 512 + 466] = damaged[63 * 512 + 466]
        disk.write_bytes(torn)
        cli(['--restore-link', str(journal), '--apply'], 2)
        assert disk.read_bytes() == torn
        cli(['--restore-link', str(journal), '--apply', '--accept-torn'])
        assert disk.read_bytes() == damaged
        disk.write_bytes(repaired)
        listed = subprocess.run([str(exe), str(disk), '--list'], capture_output=True, text=True)
        assert listed.returncode == 0 and listed.stdout.count('type ') == 3, listed.stdout
        assert 'empty' in run(status=2)
        disk.write_bytes(damaged)
        run(source=records[1], target=records[2], status=2)
        # A bad later volume is not hidden by an intact first target.
        for offset, value in [(records[2] * 512 + 478, 1),
                              ((records[2] + 63) * 512 + 510, 0),
                              (records[1] * 512 + 446, 1),
                              (records[1] * 512 + 450, 7)]:
            corrupt = bytearray(damaged)
            corrupt[offset] = value
            disk.write_bytes(corrupt)
            run(status=2)
        corrupt = bytearray(damaged)
        last = records[2] * 512
        corrupt[last + 462:last + 478] = entry(15, records[1], records[1] - 63, total - records[1])
        disk.write_bytes(corrupt)
        assert 'loop' in run(status=2).lower()
        for signature in [512, len(damaged) - 512]:
            corrupt = bytearray(damaged)
            corrupt[signature:signature + 8] = b'EFI PART'
            disk.write_bytes(corrupt)
            assert 'GPT' in run(status=2)
        print(f'PASS FAT{bits}: missing-link reproducer, two recovered volumes, exact proposed bytes, unchanged source, tail/loop/GPT/occupied-link refusals')
