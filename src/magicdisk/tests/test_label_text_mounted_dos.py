#!/usr/bin/env python3
"""Mounted-DOS label repair refreshes VOL output and preserves exact undo."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command
from label_text_fixture import fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-label-mounted-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    for bits in ([12, 16] if args.kernel == 'msdos622' else [12, 16, 32]):
        volume = root / 'VOLUME.IMG'
        original, repaired, _ = fixture(volume, bits)
        broken = bytearray(original)
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

        def boot_batch(lines):
            batch = ['@echo off', 'C:', 'cd \\'] + lines + ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders,
                   floppy=volume if bits == 12 else None)

        # Establish the kernel's mounted-state bytes before comparing repairs.
        boot_batch([f'VOL {drive}: > A:\\BEFORE.TXT',
                    f'MAGICDSK --drive {drive} > A:\\SCAN.TXT',
                    'if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT',
                    'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT'])
        report = command('mtype', '-i', str(boot), '::/SCAN.TXT').stdout.decode()
        assert 'FAIL' not in report and report.count('DIR_LABEL_NAME') == 1, report
        before_display = command('mtype', '-i', str(boot), '::/BEFORE.TXT').stdout
        baseline = target.read_bytes()
        expected = bytearray(baseline)
        base = 0 if bits == 12 else 63 * 512
        for offset, (before, after) in enumerate(zip(original, repaired)):
            if before != after:
                expected[base + offset] = after
        boot_batch([f'VOL {drive}: > A:\\BEFORE.TXT',
                    f'MAGICDSK --drive {drive} --repair-label-text --apply --journal C:\\A{bits}.MDJ > A:\\REPAIR.TXT',
                    'if errorlevel 1 echo FAIL >> A:\\REPAIR.TXT',
                    f'VOL {drive}: > A:\\AFTER.TXT',
                    f'MAGICDSK --drive {drive} > A:\\CLEAN.TXT',
                    'if errorlevel 1 echo FAIL >> A:\\CLEAN.TXT'])
        for name in ['REPAIR.TXT', 'CLEAN.TXT']:
            report = command('mtype', '-i', str(boot), '::/' + name).stdout.decode()
            assert 'FAIL' not in report, report
        shown = command('mtype', '-i', str(boot), '::/AFTER.TXT').stdout
        assert b'_A_B_C_D E' in shown, shown
        assert target.read_bytes() == expected, 'Unexpected source changes after mounted repair'
        boot_batch([f'MAGICDSK --drive {drive} --restore-journal C:\\A{bits}.MDJ --apply > A:\\UNDO.TXT',
                    'if errorlevel 1 echo FAIL >> A:\\UNDO.TXT',
                    f'VOL {drive}: > A:\\RESTORED.TXT'])
        assert b'FAIL' not in command('mtype', '-i', str(boot), '::/UNDO.TXT').stdout
        restored = command('mtype', '-i', str(boot), '::/RESTORED.TXT').stdout
        assert restored == before_display, (before_display, restored)
        assert target.read_bytes() == baseline
        print(f'PASS: {args.kernel} mounted FAT{bits}: immediate VOL output, exact root/boot label bytes and undo', flush=True)
assert digest(args.boot) == source_hash
