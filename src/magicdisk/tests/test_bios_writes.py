#!/usr/bin/env python3
"""Test-only BIOS writes on signed disposable media, checked after QEMU exits."""
import argparse
from pathlib import Path
import tempfile
from bios_fixture import digest, put, text_file, prepare, run_vm
from directory_fixture import command

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()


source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-bios-write-') as directory:
    root = Path(directory)
    boot, disk, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'BIOSWRT.EXE')
    floppy, hard = root / 'FLOPPY.IMG', root / 'HARD.IMG'
    originals = {}
    for target, sectors in [(floppy, 2880), (hard, 32 * 1008)]:
        data = bytearray(bytes(range(256)) * (sectors * 2))
        data[:512] = bytes(512)
        signature = b'MAGICDISK BIOS TEST MEDIA'
        data[:len(signature)] = signature
        target.write_bytes(data)
        originals[target] = bytes(data)
    cases = [(1, 17, 3, 'BF', 'chs', floppy),
             (0x81, 1007, 3, 'HC', 'chs', hard),
             (0x81, 32 * 1008 - 2, 2, 'HE', 'edd', hard)]
    system = str(disk) + '@@' + str(63 * 512)
    for phase in ['write', 'restore', 'protected']:
        lines = ['@echo off', 'C:', 'cd \\', 'echo START > A:\\STATUS.TXT']
        for drive, first, count, name, mode, target in cases:
            backup = name + ('P' if phase == 'protected' else '') + '.SAV'
            operation = 'restore' if phase == 'restore' else 'write'
            lines += [f'BIOSWRT {drive:x} {first} {count} {operation} C:\\{backup} {mode} > A:\\{name}.TXT',
                      ('if not errorlevel 2' if phase == 'protected' else 'if errorlevel 1') +
                      ' echo FAIL >> A:\\STATUS.TXT']
        lines += ['echo DONE >> A:\\STATUS.TXT', 'EXITTEST.COM']
        put(system, text_file(root, 'RUN.BAT', '\n'.join(lines) + '\n'))
        hard_backend = str(hard)
        if phase == 'protected':
            errors = root / 'write-error.conf'
            errors.write_text('[inject-error]\nevent = "write_aio"\niotype = "write"\nerrno = "5"\nimmediately = "on"\nonce = "off"\n')
            hard_backend = f'blkdebug:{errors}:{hard}'
        run_vm(boot, disk, cylinders, hard, 32, floppy,
               protected=phase == 'protected', backend=hard_backend)
        status = command('mtype', '-i', str(boot), '::/STATUS.TXT').stdout.decode()
        reports = '\n'.join(command('mtype', '-i', str(boot), '::/' + name + '.TXT').stdout.decode()
                            for _, _, _, name, _, _ in cases)
        assert 'DONE' in status and 'FAIL' not in status, (phase, status, reports)
        if phase != 'protected':
            assert reports.count('PASS: bounded BIOS transfer verified') == len(cases), reports
        else:
            assert reports.count('FAIL: BIOS transfer test') == len(cases), reports
            assert reports.count('WRITE_FAILED sector=') == len(cases), reports
        expected = {path: bytearray(data) for path, data in originals.items()}
        if phase == 'write':
            for drive, first, count, _, _, target in cases:
                for lba in range(first, first + count):
                    expected[target][lba * 512:(lba + 1) * 512] = bytes(
                        (lba * 17 + byte * 13 + drive) & 255 for byte in range(512))
        for target, data in expected.items():
            assert target.read_bytes() == data, (phase, target.name)
        print(f'PASS: {args.kernel} BIOS {phase}: CHS track/cylinder boundaries, EDD final sectors, exact host bytes')
assert digest(args.boot) == source_hash
