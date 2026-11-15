#!/usr/bin/env python3
"""Retain, browse and save more than 512 findings through the DOS workspace."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, Layout
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-findings-capacity-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    volume = root / 'VOLUME.IMG'
    with volume.open('wb') as file:
        file.truncate(1440 * 1024)
    command('mkfs.fat', '-F', '12', '-r', '1024', str(volume))
    data = bytearray(volume.read_bytes())
    layout = Layout(data, 12)
    for index in range(700):
        offset = layout.root_sector * 512 + index * 32
        data[offset:offset + 11] = f'F{index:07d}TXT'.encode()
        data[offset + 11] = 32
        struct.pack_into('<H', data, offset + 24, 0xffff)
    mbr = bytearray(63 * 512)
    partition(mbr, len(data) // 512, 0x11)
    target_cylinders = (63 + len(data) // 512 + 1007) // 1008 + 1
    original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
    hard = root / 'TARGET.IMG'
    hard.write_bytes(original)
    keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
            step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81'))
    for index in [1, 2]:
        keys += (step('Allocation view ready.' if index == 1 else 'Report saved.', 'f3') +
                 step('Check finished.', 'f6') + step('Stored: 700  Omitted: 0', 'end') +
                 step('F0000699.TXT', 'esc') +
                 step('Check finished.', 'f7', absent='INSPECTION FINDINGS') +
                 typing('SAVE REPORT TO A SEPARATE DISK', f'A:\\REPORT{index}.TXT'))
    keys += (step('Report saved.', 'f7') +
             typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\REPORT1.TXT') +
             step('Cannot create report.', 'esc'))
    put(system_image, text_file(root, 'FINDINGS.KEY', keys))
    batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY FINDINGS.KEY',
             'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
             'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT', 'EXITTEST.COM']
    put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
    run_vm(boot, system, cylinders, hard, target_cylinders)
    result = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
    assert 'UI completed' in result and 'FAIL' not in result, result
    reports = [command('mtype', '-i', str(boot), f'::/REPORT{index}.TXT').stdout.decode()
               for index in [1, 2]]
    assert reports[0] == reports[1]
    for report in reports:
        assert report.count('DIR_TIMESTAMP ') == 700, report
        assert 'Issues: 700' in report and 'Omitted findings: 0' in report
        for index in range(700):
            assert f'F{index:07d}.TXT' in report
    assert hard.read_bytes() == original
    print(f'PASS: {args.kernel}: 700 retained findings, last-row browsing, repeated scans, complete saved reports and overwrite refusal')
assert digest(args.boot) == source_hash
