#!/usr/bin/env python3
"""CP852/860/866 CLI and TUI name policies under real DOS in QEMU."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, create
from oem_fixture import extended_fixture, EXTENDED_PAGES
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-oem-pages-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        for cp in EXTENDED_PAGES:
            volume = root / 'VOLUME.IMG'
            create(volume, bits)
            data, expected = extended_fixture(volume.read_bytes(), bits, cp, lfn=True)
            struct.pack_into('<HHI', data, 24, 63, 16, 63)
            if bits == 32:
                data[6 * 512:7 * 512] = data[:512]
            sectors = len(data) // 512
            mbr = bytearray(63 * 512)
            partition(mbr, sectors, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
            target_cylinders = (63 + sectors + 1007) // 1008 + 1
            original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
            hard = root / 'TARGET.IMG'
            hard.write_bytes(original)
            keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                    step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                    step('Allocation view ready.', 'f10') + step('Open image...', 'up', True) +
                    step('Exit', 'up', True) + step('OEM code page...', 'enter', True) +
                    typing('OEM CODE PAGE:', str(cp)) +
                    step('OEM policy changed.', 'f10') + step('Open image...', 'up', True) +
                    step('Exit', 'up', True) + step('OEM code page...', 'enter', True) +
                    typing('OEM CODE PAGE:', '65536') +
                    step('Use 0 (unknown),', 'f3') + step('Check finished.', 'f7') +
                    typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\UI.TXT') +
                    step('Report saved.', 'esc'))
            put(system_image, text_file(root, 'PAGES.KEY', keys))
            folder = f'P{bits}{cp}'
            batch = ['@echo off', 'C:', 'cd \\', f'mkdir {folder}',
                     f'MAGICDSK --code-page {cp} --bios 81 --partition 1 > A:\\SCAN.TXT',
                     'if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT',
                     'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT',
                     f'MAGICDSK --bios 81 --partition 1 --code-page {cp} --export-files C:\\{folder} > A:\\EXPORT.TXT',
                     'if not errorlevel 1 echo FAIL >> A:\\EXPORT.TXT',
                     'if errorlevel 2 echo FAIL >> A:\\EXPORT.TXT',
                     'KEYPLAY PAGES.KEY', 'MAGICDSK --tui', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            # The same floppy is reused: reports deliberately overwrite disposable files.
            if bits != 12 or cp != 852:
                command('mdel', '-i', str(boot), '::/UI.TXT')
            run_vm(boot, system, cylinders, hard, target_cylinders)
            for name in ['SCAN.TXT', 'UI.TXT']:
                report = command('mtype', '-i', str(boot), '::/' + name).stdout.decode('utf-8')
                assert 'FAIL' not in report and report.count('DIR_DUPLICATE') == 1, report
                assert f'OEM code page: {cp}' in report, report
            assert b'FAIL' not in command('mtype', '-i', str(boot), '::/EXPORT.TXT').stdout
            manifest = command('mtype', '-i', system_image, f'::/{folder}/REC00001/RECOVERY.TXT').stdout.decode()
            assert f'OEM code page: {cp}' in manifest
            for name in expected:
                assert name.encode().hex().upper() in manifest, name
            assert hard.read_bytes() == original
            print(f'PASS: {args.kernel} FAT{bits} CP{cp}: CLI/TUI, invalid selection retained, UTF-8 names, exact source', flush=True)
assert digest(args.boot) == source_hash
