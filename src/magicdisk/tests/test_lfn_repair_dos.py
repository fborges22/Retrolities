#!/usr/bin/env python3
"""Invalid-LFN review, journaled BIOS repair and exact undo in QEMU DOS."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command
from lfn_repair_fixture import fixture
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-lfn-dos-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        broken, expected, removed = fixture(volume, bits)
        images = []
        for content in [broken, expected]:
            data = bytearray(content)
            struct.pack_into('<HHI', data, 24, 63, 16, 63)
            if bits == 32:
                data[6 * 512:7 * 512] = data[:512]
            sectors = len(data) // 512
            mbr = bytearray(63 * 512)
            partition(mbr, sectors, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
            target_cylinders = (63 + sectors + 1007) // 1008 + 1
            images.append(mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data)))
        hard = root / 'TARGET.IMG'
        hard.write_bytes(images[0])
        menu = (step('Open image...', 'right', True) + step('Filesystem check', 'up', True) +
                step('Clear invalid timestamps', 'up', True) + step('Clear invalid long names', 'enter', True))
        open_review = (step('Allocation view ready.', 'f3') +
                       step('Check finished.', 'f6') + step('INSPECTION FINDINGS', ord('p'))
                       if bits == 32 else step('Allocation view ready.', 'f10') + menu)
        keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                open_review +
                step('REVIEW INVALID LONG NAMES', 'esc') +
                step('Long-name cleanup cancelled;', 'f7') +
                typing('SAVE REPORT TO A SEPARATE DISK', f'A:\\LFN{bits}.TXT') +
                step('Report saved.', 'f10') + menu +
                step('REVIEW INVALID LONG NAMES', 'enter') +
                typing('TYPE CLEAR TO DELETE INVALID LFN', 'CLEAR') +
                typing('NEW JOURNAL FILE ON A SEPARATE DISK', f'C:\\LFN{bits}.MDJ') +
                step('BIOS REPAIR REQUIREMENTS', 'enter') + step('Repair verified.', 'esc'))
        put(system_image, text_file(root, 'LFN.KEY', keys))
        batch = ['@echo off', 'C:', 'cd \\',
                 'MAGICDSK --bios 81 --partition 1 --clear-invalid-lfn > A:\\PREVIEW.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\PREVIEW.TXT',
                 'KEYPLAY LFN.KEY', 'MAGICDSK --tui',
                 'MAGICDSK --bios 81 --partition 1 > A:\\CLEAN.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\CLEAN.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        assert hard.read_bytes() == images[1], 'Repair changed unexpected bytes'
        for name in ['PREVIEW.TXT', f'LFN{bits}.TXT']:
            report = command('mtype', '-i', str(boot), '::/' + name).stdout.decode()
            assert report.count('CLEAR_LFN') == len(removed) and 'FAIL' not in report, report
        assert b'FAIL' not in command('mtype', '-i', str(boot), '::/CLEAN.TXT').stdout
        raw = command('mtype', '-i', system_image, f'::/LFN{bits}.MDJ').stdout
        assert struct.unpack_from('<I', raw, 8)[0] == 17
        batch = ['@echo off', 'C:', 'cd \\',
                 f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\LFN{bits}.MDJ --apply > A:\\UNDO.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\UNDO.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        assert b'FAIL' not in command('mtype', '-i', str(boot), '::/UNDO.TXT').stdout
        assert hard.read_bytes() == images[0], 'Undo did not restore original bytes'
        print(f'PASS: {args.kernel} FAT{bits}: LFN CLI/TUI preview, exact repair, clean scan, version-17 journal and exact undo', flush=True)
assert digest(args.boot) == source_hash
