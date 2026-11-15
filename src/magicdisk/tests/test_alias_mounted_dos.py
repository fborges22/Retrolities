#!/usr/bin/env python3
"""Mounted-DOS alias repair refreshes directory lookup and preserves exact undo."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import create, command, Layout

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-alias-mounted-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    for bits in ([12, 16] if args.kernel == 'msdos622' else [12, 16, 32]):
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        payload = b'MagicDisk mounted alias directory-cache test.\r\n' * 100
        source = root / 'PAYLOAD.BIN'
        source.write_bytes(payload)
        command('mcopy', '-i', str(volume), str(source), '::/BADFILE.TXT')
        layout = Layout(volume.read_bytes(), bits)
        entry = next(e for e in layout.entries() if layout.data[e:e + 11] == b'BADFILE TXT')
        broken = bytearray(layout.data)
        broken[entry + 2] = ord('?')
        if bits != 12:
            struct.pack_into('<HHI', broken, 24, 63, 16, 63)
            if bits == 32:
                broken[6 * 512:7 * 512] = broken[:512]
        volume.write_bytes(broken)
        sectors = len(broken) // 512
        mbr = bytearray(63 * 512)
        partition(mbr, sectors, {12: 0x11, 16: 6, 32: 0x0c}[bits])
        target_cylinders = (63 + sectors + 1007) // 1008 + 1
        hard = root / 'TARGET.IMG'
        hard.write_bytes(mbr + broken + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(broken)))
        drive = 'B' if bits == 12 else 'D'
        target = volume if bits == 12 else hard
        file_offset = entry if bits == 12 else 63 * 512 + entry
        def boot_batch(lines):
            batch = ['@echo off', 'C:', 'cd \\'] + lines + ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders,
                   floppy=volume if bits == 12 else None)
        # Establish the kernel's mounted-state bytes before comparing repairs.
        boot_batch([f'DIR {drive}:\\ > A:\\BEFORE.TXT',
                    f'MAGICDSK --drive {drive} > A:\\SCAN.TXT',
                    'if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT',
                    'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT'])
        report = command('mtype', '-i', str(boot), '::/SCAN.TXT').stdout.decode()
        assert 'FAIL' not in report and report.count('DIR_NAME') == 1, report
        baseline = target.read_bytes()
        expected = bytearray(baseline)
        expected[file_offset:file_offset + 11] = b'R0000001CHK'
        boot_batch([f'DIR {drive}:\\ > A:\\BEFORE.TXT',
                    f'MAGICDSK --drive {drive} --repair-short-names --apply --journal C:\\A{bits}.MDJ > A:\\REPAIR.TXT',
                    'if errorlevel 1 echo FAIL >> A:\\REPAIR.TXT',
                    f'copy {drive}:\\R0000001.CHK C:\\COPY{bits}.BIN > A:\\COPY.TXT',
                    f'MAGICDSK --drive {drive} > A:\\CLEAN.TXT',
                    'if errorlevel 1 echo FAIL >> A:\\CLEAN.TXT'])
        for name in ['REPAIR.TXT', 'CLEAN.TXT']:
            report = command('mtype', '-i', str(boot), '::/' + name).stdout.decode()
            assert 'FAIL' not in report, report
        assert command('mtype', '-i', system_image, f'::/COPY{bits}.BIN').stdout == payload
        assert target.read_bytes() == expected, 'Unexpected source changes after mounted repair'
        boot_batch([f'MAGICDSK --drive {drive} --restore-journal C:\\A{bits}.MDJ --apply > A:\\UNDO.TXT',
                    'if errorlevel 1 echo FAIL >> A:\\UNDO.TXT'])
        assert b'FAIL' not in command('mtype', '-i', str(boot), '::/UNDO.TXT').stdout
        assert target.read_bytes() == baseline
        print(f'PASS: {args.kernel} mounted FAT{bits}: cached directory lookup, exact payload, alias bytes and undo', flush=True)
assert digest(args.boot) == source_hash
