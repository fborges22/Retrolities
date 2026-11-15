#!/usr/bin/env python3
"""Read-only retest accounting and cancellation through DOS/QEMU."""
import argparse
from pathlib import Path
import re
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from chain_fixture import set_fat
from directory_fixture import command, Layout
from ui_scripts import device_keys, diagnose, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)

with tempfile.TemporaryDirectory(prefix='magicdisk-retest-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits, size in [(12, 1440 * 1024), (16, 32 * 1024 * 1024), (32, 128 * 1024 * 1024)]:
        volume = root / 'VOLUME.IMG'
        with volume.open('wb') as file:
            file.truncate(size)
        command('mkfs.fat', '-F', str(bits), '-s', '2', str(volume))
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        layout = Layout(data, bits)
        marker = {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits]
        set_fat(data, bits, 1000, marker)
        mbr = bytearray(63 * 512)
        partition(mbr, size // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + size // 512 + 1007) // 1008 + 1
        tail = bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        failed_sector = 63 + layout.data_sector + 998 * layout.spc
        errors = root / 'read-error.conf'
        errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                          f'sector = "{failed_sector}"\nerrno = "5"\nonce = "off"\n')
        for phase in ['readable', 'failed', 'cancel']:
            if phase == 'cancel':
                # Keep the read-only operation active long enough for BIOS
                # timer keystrokes; never rely on a wall-clock key delay.
                for cluster in range(1001, 1200):
                    set_fat(data, bits, cluster, marker)
            original = mbr + data + tail
            hard.write_bytes(original)
            batch = ['@echo off', 'C:', 'cd \\',
                     'MAGICDSK --bios 81 --partition 1 --retest-bad > A:\\RESULT.TXT',
                     ('if not errorlevel 1' if phase == 'failed' else 'if errorlevel 1') +
                     ' echo FAIL >> A:\\RESULT.TXT',
                     'if errorlevel 2 echo FAIL >> A:\\RESULT.TXT', 'EXITTEST.COM']
            if phase == 'cancel':
                keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                        step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                        diagnose() + step('Filesystem check', 'down', True) +
                        step('Surface read', 'down', True) + step('Retest marked bad', 'enter', True) +
                        step('Retest cluster', 'esc') + step('Retest incomplete.', 'f7') +
                        typing('SAVE REPORT TO A SEPARATE DISK', f'A:\\R{bits}.TXT') +
                        step('Report saved.', 'esc'))
                put(system_image, text_file(root, 'CANCEL.KEY', keys))
                batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY CANCEL.KEY',
                         'MAGICDSK --tui', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            backend = f'blkdebug:{errors}:{hard}' if phase == 'failed' else None
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            report_name = f'R{bits}.TXT' if phase == 'cancel' else 'RESULT.TXT'
            report = command('mtype', '-i', str(boot), '::/' + report_name).stdout.decode()
            if phase == 'cancel':
                assert 'Bad-cluster retest: INCOMPLETE' in report, report
                assert report.count('RETEST_INCOMPLETE') == 1, report
                counts = re.search(r'Sectors attempted: (\d+)  Failed reads: (\d+)', report)
                assert counts and int(counts[1]) <= size // 512 and int(counts[2]) == 0, report
            else:
                assert 'FAIL' not in report, report
                assert f'{layout.spc} sectors attempted, {int(phase == "failed")} failed.' in report, report
                assert ('BAD_UNREADABLE' if phase == 'failed' else 'BAD_READABLE') in report, report
            assert hard.read_bytes() == original, phase
            print(f'PASS: {args.kernel} FAT{bits} retest {phase}: counts and findings verified; exact source bytes')

assert digest(args.boot) == source_hash
