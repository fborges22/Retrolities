#!/usr/bin/env python3
"""Read-only orphan discovery on all FAT formats under real DOS in QEMU."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command
from test_orphan_directories import fixture, verify
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-orphan-dos-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        data, expected, _, _ = fixture(volume, bits)
        data = bytearray(data)
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
                step('Allocation view ready.', 'f10') + step('Open image...', 'right', True) +
                step('Filesystem check', 'right', True) + step('Allocation map', 'right', True) +
                step('Save report...', 'up', True) +
                step('Find orphan directories...', 'enter', True) +
                step('FIND ORPHAN DIRECTORIES', 'enter') +
                typing('RECOVERY DESTINATION DIRECTORY', f'C:\\U{bits}') +
                step('ORPHAN-DIRECTORY RESULTS', 'enter') +
                step('Unverified candidates exported;', 'esc'))
        put(system_image, text_file(root, 'ORPHAN.KEY', keys))
        batch = ['@echo off', 'C:', 'cd \\', f'mkdir O{bits}', f'mkdir U{bits}',
                 f'MAGICDSK --bios 81 --partition 1 --recover-directories C:\\O{bits} > A:\\ORPHAN.TXT',
                 'if not errorlevel 1 echo FAIL >> A:\\ORPHAN.TXT',
                 'if errorlevel 2 echo FAIL >> A:\\ORPHAN.TXT',
                 'KEYPLAY ORPHAN.KEY', 'MAGICDSK --tui', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/ORPHAN.TXT').stdout.decode()
        assert 'FAIL' not in report and 'Orphan-directory export finished' in report, report
        for prefix in ['O', 'U']:
            folder = f'::/{prefix}{bits}/REC00001/'
            def read_file(name):
                return command('mtype', '-i', system_image, folder + name).stdout
            verify(read_file('RECOVERY.TXT').decode(), read_file, expected, 63)
        assert hard.read_bytes() == original
        print(f'PASS: {args.kernel} FAT{bits}: orphan CLI/TUI, LFN payloads, exact source', flush=True)
assert digest(args.boot) == source_hash
