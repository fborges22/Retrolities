#!/usr/bin/env python3
"""Real journal apply/rollback/undo through the gated BIOS write backend."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, put, text_file, partition, prepare, run_vm
from directory_fixture import command, Layout
from chain_fixture import set_fat
from ui_scripts import device_keys, marker_keys, restore_menu, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--ata-target', choices=['secondary-master', 'primary-slave', 'secondary-slave'],
                    default='secondary-master')
parser.add_argument('--production', action='store_true', help='Exercise MAGICDSK CLI instead of the test helper')
parser.add_argument('--unsupported-controller', action='store_true', help='Require refusal of a virtio BIOS disk')
parser.add_argument('--tui', action='store_true', help='Drive production BIOS mark, cancel and undo menus')
args = parser.parse_args()
assert not args.tui or (args.production and not args.unsupported_controller)
assert not args.unsupported_controller or args.production
target_bus, target_unit = {'secondary-master': ('ide.1', 0),
                           'primary-slave': ('ide.0', 1),
                           'secondary-slave': ('ide.1', 1)}[args.ata_target]
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-bios-journal-') as directory:
    root = Path(directory)
    program = 'MAGICDSK.EXE' if args.production else 'BIOSJNL.EXE'
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, program)
    if args.tui:
        put(str(system) + '@@' + str(63 * 512), args.probe.with_name('KEYPLAY.EXE'))
    system_image = str(system) + '@@' + str(63 * 512)
    for bits, size in [(12, 1440 * 1024), (16, 32 * 1024 * 1024), (32, 64 * 1024 * 1024)]:
        image, hard = root / 'VOLUME.IMG', root / 'TARGET.IMG'
        with image.open('wb') as file:
            file.truncate(size)
        command('mkfs.fat', '-F', str(bits), str(image))
        before = bytearray(image.read_bytes())
        struct.pack_into('<HHI', before, 24, 63, 16, 63)
        if bits == 32:
            before[6 * 512:7 * 512] = before[:512]
        layout = Layout(before, bits)
        after = bytearray(before)
        set_fat(after, bits, 1000, {12: 0xff7, 16: 0xfff7, 32: 0x0ffffff7}[bits])
        if bits == 32:
            free = struct.unpack_from('<I', before, 512 + 488)[0]
            for sector in [1, 7]:
                struct.pack_into('<II', after, sector * 512 + 488, free - 1, 0xffffffff)
        unmarked_volume = bytearray(before)
        if bits == 32:
            for sector in [1, 7]:
                struct.pack_into('<I', unmarked_volume, sector * 512 + 492, 0xffffffff)
        mbr = bytearray(63 * 512)
        partition(mbr, size // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        signature = b'MAGICDISK BIOS TEST MEDIA'
        mbr[:len(signature)] = signature
        # Legacy BIOS geometry reserves the last cylinder. Keep the entire
        # FAT volume within the CHS-addressable range.
        target_cylinders = (63 + size // 512 + 1007) // 1008 + 1
        tail = bytes(target_cylinders * 1008 * 512 - len(mbr) - size)
        original = bytes(mbr + before + tail)
        marked = bytes(mbr + after + tail)
        unmarked = bytes(mbr + unmarked_volume + tail)
        hard.write_bytes(original)
        mode = 'chs' if bits == 12 else 'edd'
        changed = [i for i in range(size // 512)
                   if before[i * 512:(i + 1) * 512] != after[i * 512:(i + 1) * 512]]
        second_fat = 63 + layout.reserved + layout.fatsz + (1000 * bits // 8) // 512
        journal_name = f'J{bits}.MDJ'
        phases = ['mark', 'restore', 'rollback', 'flush-rollback', 'journal-fail',
                  'refuse-torn', 'restore-torn']
        if args.tui:
            phases = ['cancel', 'badjournal', 'mark', 'unmark', 'undo-unmark', 'restore']
            if bits != 32 or args.kernel != 'msdos622':
                phases += ['mounted']
        if args.unsupported_controller:
            phases = ['mark']
        for phase in phases:
            if phase == 'mounted':
                mounted = bytearray(original)
                mounted[446 + 4] = {12: 0x01, 16: 0x06, 32: 0x0c}[bits]
                hard.write_bytes(mounted)
            backend = None
            system_backend = None
            if phase == 'rollback':
                errors = root / 'journal-error.conf'
                errors.write_text('[inject-error]\nevent = "write_aio"\niotype = "write"\n'
                                  f'sector = "{second_fat}"\nerrno = "5"\nonce = "on"\n')
                backend = f'blkdebug:{errors}:{hard}'
            if phase == 'flush-rollback':
                errors = root / 'flush-error.conf'
                errors.write_text('[set-state]\nevent = "write_aio"\nnew_state = "2"\n'
                                  '[inject-error]\nevent = "flush_to_disk"\niotype = "flush"\n'
                                  'state = "2"\nerrno = "5"\nonce = "on"\n')
                backend = f'blkdebug:{errors}:{hard}'
            if phase == 'journal-fail':
                errors = root / 'journal-flush-error.conf'
                errors.write_text('[inject-error]\nevent = "flush_to_disk"\niotype = "flush"\n'
                                  'errno = "5"\nonce = "on"\n')
                system_backend = f'blkdebug:{errors}:{system}'
            if phase == 'refuse-torn':
                torn = bytearray(original)
                first = changed[0]
                different = [i for i in range(512)
                             if before[first * 512 + i] != after[first * 512 + i]]
                assert len(different) >= 2
                byte = different[0]
                torn[(63 + first) * 512 + byte] = after[first * 512 + byte]
                hard.write_bytes(torn)
            prefix = {'rollback': 'F', 'flush-rollback': 'G', 'journal-fail': 'H',
                      'unmark': 'U', 'undo-unmark': 'U'}.get(phase)
            name = f'{prefix}{bits}.MDJ' if prefix else journal_name
            operation = 'rollback' if phase == 'flush-rollback' else phase
            batch = ['@echo off', 'C:', 'cd \\',
                     f'BIOSJNL {operation} C:\\{name} {mode} > A:\\RESULT.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT', 'EXITTEST.COM']
            if args.production:
                marking = phase in ['mark', 'rollback', 'flush-rollback', 'journal-fail']
                options = f'--mark-bad 1000 --journal C:\\{name}' if marking else f'--restore-journal C:\\{name}'
                if phase == 'restore-torn':
                    options += ' --accept-torn'
                command_line = f'MAGICDSK --bios 81 --partition 1 {options} --apply'
                expected_failure = args.unsupported_controller or phase in ['rollback', 'flush-rollback', 'journal-fail', 'refuse-torn']
                batch = ['@echo off', 'C:', 'cd \\', command_line + ' > A:\\RESULT.TXT',
                         ('if not errorlevel 2' if expected_failure else 'if errorlevel 1') +
                         ' echo FAIL >> A:\\RESULT.TXT']
                if phase == 'restore-torn':
                    batch += [f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\{name} --apply >> A:\\RESULT.TXT',
                              'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
                batch += ['EXITTEST.COM']
            if args.tui:
                select = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                          step('Other device...', 'enter', True) +
                          typing('BIOS DEVICE: 00=A', '81'))
                if phase in ['restore', 'undo-unmark']:
                    keys = (restore_menu.replace('Report saved.', 'Allocation view ready.')
                            .replace(typing('JOURNAL TO RESTORE', 'C:\\FATUI.MDJ'),
                                     typing('JOURNAL TO RESTORE', 'C:\\' + name)) +
                            step('BIOS REPAIR REQUIREMENTS', 'enter') +
                            step('Original metadata restored and verified.', 'esc'))
                else:
                    keys = marker_keys[:marker_keys.index('0|Repair verified.')]
                    keys = keys.replace(typing('MARK BAD: ABSOLUTE DISK SECTOR', '372'),
                                        typing('MARK BAD: ABSOLUTE DISK SECTOR',
                                               str(63 + layout.data_sector + 998 * layout.spc)))
                    keys = keys.replace(typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'UI.MDJ'),
                                        typing('NEW JOURNAL FILE ON A SEPARATE DISK',
                                               ('Z:\\' if phase == 'badjournal' else 'C:\\') + name))
                    if phase == 'unmark':
                        keys = keys.replace(step('Mark bad sector', 'enter', True),
                                            step('Mark bad sector', 'down', True) +
                                            step('Unmark bad sector', 'enter', True))
                        keys = keys.replace('MARK BAD: ABSOLUTE', 'UNMARK BAD: ABSOLUTE')
                        keys = keys.replace(typing('CONFIRM MARKER CHANGE', 'MARK'),
                                            typing('CONFIRM MARKER CHANGE', 'UNMARK'))
                    keys += step('BIOS REPAIR REQUIREMENTS', 'esc' if phase == 'cancel' else 'enter')
                    outcome = {'cancel': 'Repair cancelled; no writes made.',
                               'badjournal': 'Journal must resolve to a different known local physical disk',
                               'mounted': 'BIOS disk has a mounted DOS volume'}.get(phase, 'Repair verified.')
                    if phase == 'mark':
                        keys += marker_keys[marker_keys.index('0|Repair verified.'):]
                    else:
                        keys += step(outcome, 'esc')
                put(system_image, text_file(root, 'BIOSUI.KEY', select + keys))
                batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY BIOSUI.KEY',
                         'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
                         'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend,
                   target_bus=target_bus, target_unit=target_unit, system_backend=system_backend,
                   target_kind='virtio' if args.unsupported_controller else 'ide')
            report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            if args.unsupported_controller:
                assert 'Cannot establish ATA flush support' in report and 'FAIL' not in report, report
                assert hard.read_bytes() == original
                print(f'PASS: {args.kernel} FAT{bits} unsupported controller refused; source unchanged')
                continue
            if args.tui:
                assert 'UI completed' in report and 'FAIL' not in report, report
            elif args.production:
                message = ('original sectors restored' if phase in ['rollback', 'flush-rollback'] else
                           'Journal storage could not be flushed' if phase == 'journal-fail' else
                           'torn write' if phase == 'refuse-torn' else
                           'committed and verified' if phase == 'mark' else 'restored and verified')
                assert message in report and 'FAIL' not in report, (bits, phase, report)
            else:
                assert 'PASS: BIOS journal operation' in report and 'FAIL' not in report, (bits, phase, report)
            expected = (unmarked if phase == 'unmark' else
                        marked if phase in ['mark', 'undo-unmark'] else torn if phase == 'refuse-torn' else
                        mounted if phase == 'mounted' else original)
            assert hard.read_bytes() == expected, (bits, phase)
            if phase in ['mark', 'unmark', 'rollback', 'flush-rollback', 'journal-fail']:
                old = after if phase == 'unmark' else before
                new = unmarked_volume if phase == 'unmark' else after
                differences = [i for i in range(size // 512)
                               if old[i * 512:(i + 1) * 512] != new[i * 512:(i + 1) * 512]]
                raw = command('mtype', '-i', system_image, '::/' + name).stdout
                version, count = struct.unpack_from('<II', raw, 8)
                assert version == 1 and count == len(differences)
                assert raw[64:576] == original[:512] and raw[576:1088] == before[:512]
                sectors = []
                for p in range(1088, len(raw), 1032):
                    sector = struct.unpack_from('<I', raw, p)[0]
                    sectors.append(sector)
                    assert raw[p + 4:p + 516] == old[sector * 512:(sector + 1) * 512]
                    assert raw[p + 516:p + 1028] == new[sector * 512:(sector + 1) * 512]
                assert sorted(sectors) == differences
            print(f'PASS: {args.kernel} {args.ata_target} {program} BIOS FAT{bits} {phase}, exact disk bytes and journal')
assert digest(args.boot) == source_hash
