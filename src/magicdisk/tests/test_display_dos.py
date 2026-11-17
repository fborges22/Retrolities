#!/usr/bin/env python3
"""Check map size labels and visible controls at ten-digit disk LBAs in QEMU."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import command, Layout
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-display-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    volume = root / 'VOLUME.IMG'
    with volume.open('wb') as file:
        file.truncate(1440 * 1024)
    command('mkfs.fat', '-F', '12', str(volume))
    contents = volume.read_bytes()
    layout = Layout(contents, 12)
    start = 0xffffe000
    sectors = 0xffffffff
    mbr = bytearray(512)
    mbr[447:450] = mbr[451:454] = b'\xfe\xff\xff'
    mbr[450] = 0x11
    struct.pack_into('<II', mbr, 454, start, len(contents) // 512)
    mbr[510:512] = b'\x55\xaa'
    hard = root / 'TARGET.IMG'
    with hard.open('wb') as file:
        file.truncate(sectors * 512)
        file.write(mbr)
        file.seek(start * 512)
        file.write(contents)
    data_lba = start + layout.data_sector
    keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
            step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
            step('Allocation view ready.', 'home') +
            step('No current check results.', 'home') + step('1.4 MiB', 'home') + step(f'LBA {start}', 'home') +
            step('Enter: details   +: zoom   -: whole volume', ord('+')) +
            step('512 B', 'home') + step(f'Disk LBA {data_lba}-{data_lba}', 'enter') +
            step('BLOCK DETAILS', 'enter') + step(f'Disk LBA {data_lba} / {sectors - 1}', 'esc') +
            step('512 B', ord('-'), absent='READ-ONLY SECTOR VIEWER') +
            step('Allocation view ready.', 'f4') +
            step('Surface read finished.', 'home') +
            step('SURFACE COMPLETE', 'home') + step('2880/2880 read', 'home') + step('0 failed', 'home') +
            step('F5: allocation map', 'f5') + step('Allocation view ready.', 'home') +
            step('SURFACE COMPLETE', 'home') + step('2880/2880 read', 'home') + step('0 failed', 'esc'))
    navigation = ''
    for invalid in [' -2', '-2', '+2', '4294967296', '0x100000000', '0x', '1x', ' 0', '0 ']:
        navigation += (step(f'Disk LBA {data_lba} / {sectors - 1}', ord('g')) +
                       typing('GO TO ABSOLUTE DISK LBA', invalid) +
                       step('Invalid disk LBA;', ord('v')))
    navigation += (step(f'Disk LBA {data_lba} / {sectors - 1}', ord('g')) +
                   typing('GO TO ABSOLUTE DISK LBA', '0xFFFFFFFE') +
                   step(f'Disk LBA {sectors - 1} / {sectors - 1}', ord('g')) +
                   typing('GO TO ABSOLUTE DISK LBA', '4294967295') +
                   step('Invalid disk LBA;', ord('g')) +
                   typing('GO TO ABSOLUTE DISK LBA', str(data_lba)))
    keys = keys.replace(step(f'Disk LBA {data_lba} / {sectors - 1}', 'esc'),
                        navigation + step(f'Disk LBA {data_lba} / {sectors - 1}', 'esc'))
    put(system_image, text_file(root, 'DISPLAY.KEY', keys))
    batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY DISPLAY.KEY',
             'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
             'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT', 'EXITTEST.COM']
    invalid_numbers = ['-2', ' -2', '-4294967294', '-4294967295', '+2', '4294967296', '0x10', '2x', '2 ']
    numeric_checks = ['echo Numeric checks > A:\\NUMBERS.TXT']
    for value in invalid_numbers:
        numeric_checks += [f'MAGICDSK MISSING.IMG --mark-sector "{value}" --apply --journal C:\\REJECT.MDJ >> A:\\NUMBERS.TXT',
                           'if not errorlevel 2 echo FAIL >> A:\\NUMBERS.TXT']
    batch[3:3] = numeric_checks
    put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
    run_vm(boot, system, cylinders, hard, 16383)
    report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
    assert 'UI completed' in report and 'FAIL' not in report, report
    numbers = command('mtype', '-i', str(boot), '::/NUMBERS.TXT').stdout.decode()
    assert numbers.count('Invalid cluster or disk sector number') == len(invalid_numbers), numbers
    assert 'FAIL' not in numbers and 'Cannot open' not in numbers, numbers

    with hard.open('rb') as file:
        assert file.read(512) == mbr
        file.seek(start * 512)
        assert file.read(len(contents)) == contents
    assert hard.stat().st_size == sectors * 512
    print(f'PASS: {args.kernel}: rounded volume size, 512-byte zoom, complete high LBAs, strict numeric inputs and visible controls')
assert digest(args.boot) == source_hash
