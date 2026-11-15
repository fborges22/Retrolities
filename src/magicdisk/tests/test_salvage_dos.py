#!/usr/bin/env python3
"""Retire an unreadable live-file cluster using the production DOS executable."""
import argparse
from pathlib import Path
import struct
import tempfile
import zlib
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, Layout, UNICODE_FILE
from selected_export_fixture import create_selected
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--torn-undo', action='store_true')
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-salvage-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        data, layout, chain, payload, entry = create_selected(root / 'VOLUME.IMG', bits)
        data = bytearray(data)
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        mbr = bytearray(63 * 512)
        partition(mbr, len(data) // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + len(data) // 512 + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        target = chain[1]
        failed_sector = 63 + layout.data_sector + (target - 2) * layout.spc
        errors = root / 'read-error.conf'
        errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                          f'sector = "{failed_sector}"\nerrno = "5"\nonce = "off"\n')
        backend = f'blkdebug:{errors}:{hard}'
        for phase in ['cli', 'ui'] if bits == 12 and not args.torn_undo else ['cli']:
            hard.write_bytes(original)
            journal = f'S{bits}{phase[0]}.MDJ'
            if phase == 'ui':
                keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                        step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                        step('Allocation view ready.', 'f4') + step('Surface read finished.', 'home') + step('1 failed', 'f6') +
                        step('M: review marking this cluster bad', ord('m')) + step('UNREADABLE FILE DATA', 'enter') +
                        step('REVIEW BAD-CLUSTER CHANGE', 'esc') +
                        step('Marker change cancelled; no writes made.', 'home') +
                        step('SALVAGE PREVIEW: missing sectors become ZERO.', 'f7') +
                        typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\SALVAGE.TXT') +
                        step('Report saved.', 'f6') +
                        step('FILE SALVAGE PREVIEW', 'esc') +
                        step('Report saved.', 'f4'))
                keys += (step('Surface read finished.', 'home') + step('1 failed', 'f6') + step('M: review marking this cluster bad', ord('m')) +
                         step('UNREADABLE FILE DATA', 'enter') +
                         step('REVIEW BAD-CLUSTER CHANGE', 'enter') +
                         typing('CONFIRM MARKER CHANGE', 'SALVAGE') +
                         typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\' + journal) +
                         step('BIOS REPAIR REQUIREMENTS', 'enter') +
                         step('Repair verified.', 'home') +
                         step('No current check results.', 'esc'))
                put(system_image, text_file(root, 'SALVAGE.KEY', keys))
                batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY SALVAGE.KEY',
                         'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
                         'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT']
            else:
                batch = ['@echo off', 'C:', 'cd \\',
                         f'MAGICDSK --bios 81 --partition 1 --salvage-cluster {target} --apply --journal C:\\{journal} > A:\\RESULT.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            assert 'FAIL' not in report, report
            if phase == 'cli':
                assert 'SALVAGE_GAP' in report and '1 unreadable sectors become ZERO (512 bytes)' in report, report
                assert 'SALVAGE_FILE' in report and UNICODE_FILE in report, report
            if phase == 'ui':
                preview = command('mtype', '-i', str(boot), '::/SALVAGE.TXT').stdout.decode()
                assert 'SALVAGE_GAP' in preview and 'Missing bytes are NOT recovered' in preview
                assert 'SALVAGE_FILE' in preview and UNICODE_FILE in preview
                assert 'No repair applied.' in preview
            changed = hard.read_bytes()
            updated = Layout(changed[63 * 512:63 * 512 + len(data)], bits)
            replacement = updated.fat(chain[0])
            assert replacement != target and updated.fat(replacement) == chain[2]
            assert updated.fat(target) == {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits]
            assert changed[failed_sector * 512:(failed_sector + layout.spc) * 512] == original[failed_sector * 512:(failed_sector + layout.spc) * 512]
            recovered = command('mtype', '-i', str(hard) + '@@32256', '::/' + UNICODE_FILE).stdout
            expected = bytearray(payload)
            expected[layout.spc * 512:layout.spc * 512 + 512] = bytes(512)
            assert recovered == expected
            raw = command('mtype', '-i', system_image, '::/' + journal).stdout
            assert struct.unpack_from('<I', raw, 8)[0] == 16 and raw[40:56] == b'\1' + bytes(15)
            assert zlib.crc32(raw[:60] + raw[64:1088]) == struct.unpack_from('<I', raw, 60)[0]
            expected_disk = bytearray(original)
            for offset in range(1088, len(raw), 1032):
                record = raw[offset:offset + 1032]
                assert zlib.crc32(record[:1028]) == struct.unpack_from('<I', record, 1028)[0]
                sector = 63 + struct.unpack_from('<I', record)[0]
                assert record[4:516] == original[sector * 512:(sector + 1) * 512]
                expected_disk[sector * 512:(sector + 1) * 512] = record[516:1028]
            assert changed == expected_disk
            if args.torn_undo:
                record = next(raw[offset:offset + 1032]
                              for offset in range(1088, len(raw), 1032)
                              if sum(a != b for a, b in zip(raw[offset + 4:offset + 516],
                                                           raw[offset + 516:offset + 1028])) >= 2)
                sector = 63 + struct.unpack_from('<I', record)[0]
                before, after = record[4:516], record[516:1028]
                differences = [index for index in range(512) if before[index] != after[index]]
                assert len(differences) >= 2
                mixed = bytearray(after)
                mixed[differences[0]] = before[differences[0]]
                with hard.open('r+b') as file:
                    file.seek(sector * 512)
                    file.write(mixed)
            batch = ['@echo off', 'C:', 'cd \\',
                     f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\{journal} --apply > A:\\UNDO.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\UNDO.TXT', 'EXITTEST.COM']
            if args.torn_undo:
                batch = ['@echo off', 'C:', 'cd \\',
                         f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\{journal} --apply > A:\\STRICT.TXT',
                         'if not errorlevel 2 echo FAIL >> A:\\STRICT.TXT',
                         f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\{journal} --apply --accept-torn > A:\\UNDO.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\UNDO.TXT', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            undo = command('mtype', '-i', str(boot), '::/UNDO.TXT').stdout.decode()
            assert 'restored and verified' in undo and 'FAIL' not in undo, undo
            if args.torn_undo:
                strict = command('mtype', '-i', str(boot), '::/STRICT.TXT').stdout.decode()
                assert 'torn write; no sectors restored' in strict and 'FAIL' not in strict, strict
            assert hard.read_bytes() == original
            print(f'PASS: {args.kernel} FAT{bits} {phase}: salvage gaps, LFN/payload, source preserved, journal and {"torn" if args.torn_undo else "exact"} undo')
assert digest(args.boot) == source_hash
