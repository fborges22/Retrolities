"""Recreate first, middle and terminal EBR tables with independent expected bytes."""
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
    cylinder, remainder = divmod(lba, 1008)
    head, sector = divmod(remainder, 63)
    return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])


def entry(kind, absolute, relative, count):
    return bytes([0]) + chs(absolute) + bytes([kind]) + chs(absolute + count - 1) + struct.pack('<II', relative, count)


with tempfile.TemporaryDirectory(prefix='magicdisk-record-') as temporary:
    root = Path(temporary)
    for bits in [12, 16, 32]:
        volume, disk, output = [root / name for name in ['VOL.IMG', 'DISK.IMG', 'EBR.BIN']]
        create(volume, bits)
        data = volume.read_bytes()
        sectors = len(data) // 512
        records = [63 + i * (sectors + 126) for i in range(3)]
        total = records[-1] + 63 + sectors + 1
        raw = bytearray(total * 512)
        raw[446:462] = entry(15, 63, 63, total - 63)
        raw[510:512] = b'\x55\xaa'
        for i, at in enumerate(records):
            start = at + 63
            raw[start * 512:(start + sectors) * 512] = data
            raw[at * 512 + 446:at * 512 + 462] = entry({12: 1, 16: 6, 32: 11}[bits], start, 63, sectors)
            raw[at * 512 + 510:at * 512 + 512] = b'\x55\xaa'
            if i < 2:
                target = records[i+1]
                raw[at * 512 + 462:at * 512 + 478] = entry(15, target, target-63, total-target)

        def run(at, start, target, boot=0, status=0):
            output.unlink(missing_ok=True)
            before = hashlib.sha256(disk.read_bytes()).digest()
            result = subprocess.run([str(probe), str(disk), str(at), str(start), str(target),
                                     str(boot), '16', '63', str(output)], capture_output=True, text=True)
            assert result.returncode == status, result.stdout + result.stderr
            assert hashlib.sha256(disk.read_bytes()).digest() == before
            if status:
                assert not output.exists()
                return
            return output.read_bytes()

        for i, at in enumerate(records):
            target = records[i+1] if i < 2 else 0
            damaged = bytearray(raw)
            damaged[at * 512:(at + 1) * 512] = bytes(512)
            disk.write_bytes(damaged)
            proposed = run(at, at + 63, target)
            assert proposed == raw[at * 512:(at+1) * 512]
            repaired = bytearray(damaged)
            repaired[at * 512:(at+1) * 512] = proposed
            assert repaired == raw
            checked = root / 'CHECK.IMG'
            checked.write_bytes(repaired)
            result = subprocess.run([str(exe), str(checked), '--list'], capture_output=True, text=True)
            assert result.returncode == 0 and result.stdout.count('type ') == 3, result.stdout
            options = ['--rebuild-record', str(at), str(target), '--volume-start', str(at+63),
                       '--chs', '16', '63']
            journal = root / f'R{bits}{i}.MDJ'

            def cli(arguments, status=0):
                result = subprocess.run([str(exe), str(disk)] + arguments, capture_output=True, text=True)
                assert result.returncode == status, result.stdout + result.stderr
                return result.stdout

            assert f'{2-i} tail volumes' in cli(options)
            for extra in [['--apply'], ['--journal', str(journal)], ['--accept-torn'],
                          ['--partition', '1'], ['--list'], ['--surface'], ['--find-volumes']]:
                cli(options + extra, 2)
                assert disk.read_bytes() == damaged and not journal.exists()
            cli(['--rebuild-record', str(at), str(target), '--chs', '16', '63'], 2)
            assert 'EBR record reconstructed and verified.' in cli(options + ['--apply', '--journal', str(journal)])
            assert disk.read_bytes() == raw
            assert cli(['--list']).count('type ') == 3
            saved = journal.read_bytes()
            assert len(saved) == 2624 + (2-i)*1032 and saved[:8] == b'MDREC001'
            assert saved[64:576] == raw[:512]
            assert saved[576:1088] == bytes(512) and saved[1600:2112] == proposed
            for invalid in [saved[:-1], saved + b'X']:
                journal.write_bytes(invalid)
                cli(['--restore-record', str(journal), '--apply'], 2)
                assert disk.read_bytes() == raw
            forged = bytearray(saved)
            forged[2624 + 520 + 100 if i < 2 else 2212] ^= 1
            forged[60:64] = bytes(4)
            struct.pack_into('<I', forged, 60, zlib.crc32(forged))
            journal.write_bytes(forged)
            cli(['--restore-record', str(journal), '--apply'], 2)
            assert disk.read_bytes() == raw
            journal.write_bytes(saved)
            assert 'EBR record restored and verified.' in cli(['--restore-record', str(journal), '--apply'])
            assert disk.read_bytes() == damaged
            torn = bytearray(raw)
            torn[at*512+450] = 0
            disk.write_bytes(torn)
            cli(['--restore-record', str(journal), '--apply'], 2)
            assert disk.read_bytes() == torn
            cli(['--restore-record', str(journal), '--apply', '--accept-torn'])
            assert disk.read_bytes() == damaged
            # Surviving bytes outside the table are retained exactly.
            damaged[at * 512 + 100] = 42
            disk.write_bytes(damaged)
            expected = bytearray(proposed)
            expected[100] = 42
            assert run(at, at+63, target) == expected
            for offset in [446, 462, 478, 494]:
                damaged[at * 512 + offset] = 1
                disk.write_bytes(damaged)
                run(at, at+63, target, status=2)
                damaged[at * 512 + offset] = 0
            disk.write_bytes(damaged)
            run(at, at+63, at, status=2)
            run(at, at, target, status=2)
            run(at, at+63, total, status=2)
            if i > 0:
                run(at, at+63, records[0], status=2)
            if i < 2:
                for offset, value in [(target*512+450, 7), (target*512+478, 1),
                                      ((target+63)*512+12, 0), (512, ord('E'))]:
                    broken = bytearray(damaged)
                    broken[offset] = value
                    if offset == 512:
                        broken[512:520] = b'EFI PART'
                    disk.write_bytes(broken)
                    run(at, at+63, target, status=2)
            if bits == 32:
                broken = bytearray(damaged)
                broken[(at+63)*512+510] = 0
                disk.write_bytes(broken)
                run(at, at+63, target, status=2)
                assert run(at, at+63, target, boot=6) == expected
                backup_journal = root / f'B{bits}{i}.MDJ'
                cli(options + ['--boot-source', '6', '--apply', '--journal', str(backup_journal)])
                backup_repaired = bytearray(broken)
                backup_repaired[at*512:(at+1)*512] = expected
                assert disk.read_bytes() == backup_repaired
                cli(['--restore-record', str(backup_journal), '--apply'])
                assert disk.read_bytes() == broken
            print(f'PASS FAT{bits} EBR {i+1}: exact whole-record preview, chain restoration, source preservation and invalid metadata refusal', flush=True)
