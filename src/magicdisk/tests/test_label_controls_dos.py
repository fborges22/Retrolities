#!/usr/bin/env python3
"""Check label control bytes in CLI/TUI reports in DOS/QEMU."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command
from label_fixture import create_labels
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)

with tempfile.TemporaryDirectory(prefix='magicdisk-label-controls-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        data, first, second = create_labels(volume, bits, duplicate=False)
        value = {12: 9, 16: 31, 32: 5}[bits]
        data[first + 2] = value
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
                step('Allocation view ready.', 'f3') + step('Check finished.', 'f6') +
                step('DIR_LABEL_NAME', 'esc') + step('Check finished.', 'f7') +
                typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\UI.TXT') +
                step('Report saved.', 'esc'))
        put(system_image, text_file(root, 'LABEL.KEY', keys))
        batch = ['@echo off', 'C:', 'cd \\',
                 'MAGICDSK --bios 81 --partition 1 > A:\\SCAN.TXT',
                 'if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT',
                 'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT',
                 'KEYPLAY LABEL.KEY', 'MAGICDSK --tui', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        for name in ['SCAN.TXT', 'UI.TXT']:
            report = command('mtype', '-i', str(boot), '::/' + name).stdout.decode('utf-8')
            assert 'FAIL' not in report and report.count('DIR_LABEL_NAME') == 1, report
            assert f'0x{value:02X}' in report and 'sector byte 2' in report, report
        assert hard.read_bytes() == original
        command('mdel', '-i', str(boot), '::/UI.TXT')
        print(f'PASS: {args.kernel} FAT{bits}: label control-byte CLI/TUI findings, exact offsets and unchanged source', flush=True)

assert digest(args.boot) == source_hash
