#!/usr/bin/env python3
"""Explicit OEM policy reaches CLI, TUI, exports and repair guards in DOS/QEMU."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, create
from oem_fixture import fixture, lowercase_alias
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-oem-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data, _ = fixture(volume.read_bytes(), bits, 'distinct-pages')
        lowercase_alias(data, bits, 24, index=2)
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
                step('Allocation view ready.', 'f3') + step('Check finished.', 'f7') +
                typing('SAVE REPORT TO A SEPARATE DISK', f'A:\\UI{bits}.TXT') +
                step('Report saved.', 'f10') + step('Open image...', 'up', True) +
                step('Exit', 'up', True) + step('OEM code page...', 'enter', True) +
                typing('OEM CODE PAGE: 0 UNKNOWN, 437, 850', '437') +
                step('OEM policy changed.', 'f3') + step('Check finished.', 'f7') +
                typing('SAVE REPORT TO A SEPARATE DISK', f'A:\\NEW{bits}.TXT') +
                step('Report saved.', 'esc'))
        put(system_image, text_file(root, 'OEM.KEY', keys))
        journal = f'O{bits}.MDJ'
        batch = ['@echo off', 'C:', 'cd \\']
        for cp in [0, 437, 850]:
            batch += [f'MAGICDSK --bios 81 --partition 1 --code-page {cp} > A:\\C{cp}.TXT',
                      ('if not errorlevel 1' if cp == 850 else 'if errorlevel 1') + f' echo FAIL >> A:\\C{cp}.TXT',
                      f'if errorlevel 2 echo FAIL >> A:\\C{cp}.TXT']
        batch += [f'MAGICDSK --code-page 850 --bios 81 --partition 1 --repair-files --apply --journal C:\\{journal} > A:\\REPAIR.TXT',
                  'if not errorlevel 2 echo FAIL >> A:\\REPAIR.TXT',
                  f'mkdir O{bits}',
                  f'MAGICDSK --code-page 437 --bios 81 --partition 1 --export-files C:\\O{bits} > A:\\EXPORT.TXT',
                  'if errorlevel 1 echo FAIL >> A:\\EXPORT.TXT',
                  'KEYPLAY OEM.KEY', 'MAGICDSK --code-page 850 --tui', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        for cp in [0, 437, 850]:
            report = command('mtype', '-i', str(boot), f'::/C{cp}.TXT').stdout.decode('utf-8')
            assert 'FAIL' not in report, report
            assert report.count('DIR_DUPLICATE') == int(cp == 850), report
        report = command('mtype', '-i', str(boot), f'::/UI{bits}.TXT').stdout.decode('utf-8')
        assert 'OEM code page: 850' in report and report.count('DIR_DUPLICATE') == 1, report
        report = command('mtype', '-i', str(boot), f'::/NEW{bits}.TXT').stdout.decode('utf-8')
        assert 'OEM code page: 437' in report and 'DIR_DUPLICATE' not in report, report
        for name in ['REPAIR.TXT', 'EXPORT.TXT']:
            assert b'FAIL' not in command('mtype', '-i', str(boot), '::/' + name).stdout
        manifest = command('mtype', '-i', system_image, f'::/O{bits}/REC00001/RECOVERY.TXT').stdout.decode()
        assert 'OEM code page: 437' in manifest
        assert '¢FILE.TXT'.encode().hex().upper() in manifest
        assert '¥FILE.TXT'.encode().hex().upper() in manifest
        assert 'élower.öxt'.encode().hex().upper() in manifest
        assert subprocess.run(['mtype', '-i', system_image, '::/' + journal], capture_output=True).returncode != 0
        assert hard.read_bytes() == original
        print(f'PASS: {args.kernel} FAT{bits}: explicit OEM CLI/TUI, UTF-8 export, collision repair refusal; exact source')
assert digest(args.boot) == source_hash
