#!/usr/bin/env python3
"""Act on exact surface/retest findings through production DOS menus in QEMU."""
import argparse
from pathlib import Path
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

with tempfile.TemporaryDirectory(prefix='magicdisk-finding-actions-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits, size in [(12, 1440 * 1024), (16, 32 * 1024 * 1024), (32, 64 * 1024 * 1024)]:
        volume = root / 'VOLUME.IMG'
        with volume.open('wb') as file:
            file.truncate(size)
        command('mkfs.fat', '-F', str(bits), str(volume))
        clean = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', clean, 24, 63, 16, 63)
        if bits == 32:
            clean[6 * 512:7 * 512] = clean[:512]
        layout = Layout(clean, bits)
        marked = bytearray(clean)
        set_fat(marked, bits, 1000, {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits])
        unmarked = bytearray(clean)
        if bits == 32:
            free = struct.unpack_from('<I', clean, 512 + 488)[0]
            for sector in [1, 7]:
                struct.pack_into('<II', marked, sector * 512 + 488, free - 1, 0xffffffff)
                struct.pack_into('<I', unmarked, sector * 512 + 492, 0xffffffff)
        mbr = bytearray(63 * 512)
        partition(mbr, size // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + size // 512 + 1007) // 1008 + 1
        tail = bytes(target_cylinders * 1008 * 512 - len(mbr) - size)
        hard = root / 'TARGET.IMG'
        hard.write_bytes(mbr + clean + tail)
        failed_sector = 63 + layout.data_sector + 998 * layout.spc
        errors = root / 'read-error.conf'
        select = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                  step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81'))
        for phase in ['cancel', 'mark', 'unmark-failed', 'unmark', 'undo-unmark', 'undo-mark', 'metadata']:
            if phase == 'metadata' and bits != 32:
                continue
            error_lba = 63 + 2 if phase == 'metadata' else failed_sector
            errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                              f'sector = "{error_lba}"\nerrno = "5"\nonce = "off"\n')
            if phase in ['cancel', 'mark']:
                with errors.open('a') as file:
                    file.write('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                               f'sector = "{failed_sector - layout.spc}"\nerrno = "5"\nonce = "off"\n')
            backend = f'blkdebug:{errors}:{hard}' if phase in ['cancel', 'mark', 'unmark-failed', 'metadata'] else None
            if phase in ['undo-mark', 'undo-unmark']:
                journal = f'{"M" if phase == "undo-mark" else "U"}{bits}.MDJ'
                batch = ['@echo off', 'C:', 'cd \\',
                         f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\{journal} --apply > A:\\RESULT.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
            else:
                if phase in ['cancel', 'mark', 'metadata']:
                    keys = (select + step('Allocation view ready.', 'f4') +
                            step('Surface read finished.', 'f6'))
                    if phase == 'metadata':
                        keys += (step('F8: inspect selected sector', ord('m')) +
                                 step('F8: inspect selected sector', ord('u')) +
                                 step('F8: inspect selected sector', 'esc') +
                                 step('Surface read finished.', 'esc', absent='SURFACE READ FINDINGS'))
                    else:
                        keys += (step('M: review marking this cluster bad', 'down') +
                                 step(f'Disk sector {failed_sector}', ord('m'), True))
                else:
                    keys = (select + diagnose() + step('Filesystem check', 'down', True) +
                            step('Surface read', 'down', True) + step('Retest marked bad', 'enter', True) +
                            step('Bad markers retained.', 'f6') +
                            step('U: retest and review unmarking this cluster', ord('u')))
                if phase == 'cancel':
                    keys += (step('REVIEW BAD-CLUSTER CHANGE', 'esc') +
                             step('Marker change cancelled;', 'esc'))
                elif phase == 'unmark-failed':
                    keys += step('Cluster still contains unreadable sectors; marker unchanged', 'esc')
                elif phase != 'metadata':
                    action = 'MARK' if phase == 'mark' else 'UNMARK'
                    journal = f'{"M" if phase == "mark" else "U"}{bits}.MDJ'
                    keys += (step('REVIEW BAD-CLUSTER CHANGE', 'enter') +
                             typing('CONFIRM MARKER CHANGE', action) +
                             typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\' + journal) +
                             step('BIOS REPAIR REQUIREMENTS', 'enter') +
                             step('Repair verified.', 'esc'))
                put(system_image, text_file(root, 'ACTION.KEY', keys))
                batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY ACTION.KEY',
                         'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
                         'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            assert 'FAIL' not in report, report
            expected = (marked if phase in ['mark', 'unmark-failed', 'undo-unmark'] else
                        unmarked if phase == 'unmark' else clean)
            assert hard.read_bytes() == mbr + expected + tail, (bits, phase)
            print(f'PASS: {args.kernel} FAT{bits} selected finding {phase}: exact source bytes')

assert digest(args.boot) == source_hash
