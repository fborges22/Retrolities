#!/usr/bin/env python3
"""Check DOS DDT identity against independently constructed QEMU disks."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from bios_fixture import digest, put, text_file, partition, prepare, run_vm
from directory_fixture import command
from ui_scripts import device_keys, live_export_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--exports', action='store_true', help='Exercise BIOS export destination guards')
parser.add_argument('--preflight', type=Path, help='Test-only BIOSCHK executable')
parser.add_argument('--ramdrive', type=Path, help='User-owned, kernel-compatible RAMDRIVE.SYS for unknown-device refusal')
parser.add_argument('--smartdrv', type=Path, help='User-owned SMARTDRV.EXE for resident-cache refusal')
parser.add_argument('--himem', type=Path, help='Matching HIMEM.SYS for the SMARTDrive fixture')
parser.add_argument('--write-behind', action='store_true', help='Enable write-behind in the SMARTDrive fixture')
parser.add_argument('--repair-refusals', action='store_true', help='Check production BIOS repair refusals')
parser.add_argument('--tui-exports', action='store_true', help='Exercise BIOS live export menus and destination refusal')
parser.add_argument('--timeout', type=int, default=120, help='QEMU timeout per boot in seconds')
args = parser.parse_args()
assert not args.tui_exports or args.exports
if args.smartdrv:
    assert args.kernel == 'msdos622' and args.himem and args.preflight
    assert not args.exports and not args.ramdrive
    assert args.smartdrv.stat().st_size > 0 and args.himem.stat().st_size > 0
else:
    assert not args.himem and not args.write_behind
if args.ramdrive:
    assert args.kernel in ['msdos622', 'win98'] and args.preflight and not args.exports
    assert args.ramdrive.stat().st_size > 0


def chs(table, offset, lba):
    cylinder, rest = divmod(lba, 1008)
    head, sector = divmod(rest, 63)
    table[offset:offset + 3] = bytes([head, sector + 1 | (cylinder >> 2 & 0xc0), cylinder & 255])


source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-dos-map-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    if args.ramdrive:
        put(boot, args.ramdrive, 'RAMDRIVE.SYS')
        config = 'DEVICE=A:\\RAMDRIVE.SYS 64 512 16\nFILES=30\nBUFFERS=10\nLASTDRIVE=Z\n'
        put(boot, text_file(root, 'CONFIG.SYS', config))
    if args.smartdrv:
        put(boot, args.himem, 'HIMEM.SYS')
        config = 'DEVICE=A:\\HIMEM.SYS\nFILES=30\nBUFFERS=10\nLASTDRIVE=Z\n'
        put(boot, text_file(root, 'CONFIG.SYS', config))
    system_image = str(system) + '@@' + str(63 * 512)
    if args.tui_exports:
        put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    payload = root / 'PAYLOAD.TXT'
    payload.write_bytes(b'MagicDisk BIOS recovery identity test.\r\n' * 37)
    put(system_image, payload)
    if args.preflight:
        put(system_image, args.preflight, 'BIOSCHK.EXE')
    if args.smartdrv:
        put(system_image, args.smartdrv, 'SMARTDRV.EXE')
    volume = system.read_bytes()[63 * 512:(63 + 65536) * 512]
    hard = root / 'TARGET.IMG'
    for phase in (['hidden'] if args.ramdrive or args.smartdrv else ['clones', 'hidden', 'fat32']):
        data = volume
        if phase == 'fat32':
            image = root / 'FAT32.IMG'
            with image.open('wb') as file:
                file.truncate(64 * 1024 * 1024)
            command('mkfs.fat', '-F', '32', str(image))
            put(image, payload)
            data = bytearray(image.read_bytes())
            struct.pack_into('<HHI', data, 24, 63, 16, 63)
            data[6 * 512:7 * 512] = data[:512]
        sectors = len(data) // 512
        mbr = bytearray(63 * 512)
        partition(mbr, sectors, 0x16 if phase == 'hidden' else 0x0c if phase == 'fat32' else 6)
        target = mbr + data
        if phase == 'clones':
            # A primary and an extended logical volume on the second disk.
            # All three FAT16 BPBs are byte-identical, including serial numbers.
            extended = (63 + sectors + 1007) // 1008 * 1008
            target += bytes(extended * 512 - len(target))
            target[462 + 4] = 5
            chs(target, 463, extended)
            chs(target, 467, extended + 63 + sectors - 1)
            struct.pack_into('<II', target, 462 + 8, extended, 63 + sectors)
            ebr = bytearray(63 * 512)
            partition(ebr, sectors)
            ebr[446] = 0
            chs(ebr, 447, extended + 63)
            chs(ebr, 451, extended + 63 + sectors - 1)
            target += ebr + data
        target_cylinders = (len(target) // 512 + 1007) // 1008 + 1
        target += bytes(target_cylinders * 1008 * 512 - len(target))
        hard.write_bytes(target)
        batch = ['@echo off', 'C:', 'cd \\', 'MAGICDSK --dos-map > A:\\RESULT.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        if args.exports:
            batch = batch[:-1]
            batch += ['md C:\\EXPORT',
                      'MAGICDSK --bios 81 --partition 1 --export-files C:\\EXPORT > A:\\EXPORT.TXT',
                      'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
            if args.tui_exports:
                select = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                          step('Other device...', 'enter', True) +
                          typing('BIOS DEVICE: 00=A', '81'))
                if phase == 'clones':
                    select += step('SELECT VOLUME', 'enter')
                for filename, destination, outcome in [
                        ('EXPORT.KEY', 'C:\\EXPORT', 'Recovery finished; source allocation unchanged.'),
                        ('DENIED.KEY', 'D:\\', 'File export needs a separate DOS volume')]:
                    keys = live_export_keys.replace(
                        step('Allocation view ready.', 'f10'),
                        step('Allocation view ready.', 'f10', absent='SELECT VOLUME'))
                    keys = keys.replace(
                        typing('RECOVERY DESTINATION DIRECTORY', 'C:\\'),
                        typing('RECOVERY DESTINATION DIRECTORY', destination))
                    keys = keys.replace('Recovery finished with recorded gaps or damage.', outcome)
                    put(system_image, text_file(root, filename, select + keys))
                batch = batch[:-2] + ['KEYPLAY EXPORT.KEY',
                                      'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
                                      'MAGICDSK --tui', 'KEYPLAY DENIED.KEY',
                                      'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
                                      'MAGICDSK --tui']
            for number, (operation, destination) in enumerate([
                    ('--export-files', 'D:\\'), ('--recover-deleted', 'D:\\'),
                    ('--recover-lost', 'D:\\'), ('--export-files', 'Z:\\'),
                    *([('--export-files', 'E:\\')] if phase == 'clones' else [])]):
                batch += [f'MAGICDSK --bios 81 --partition 1 {operation} {destination} > A:\\DENY{number}.TXT',
                          'if not errorlevel 2 echo FAIL >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        if args.smartdrv:
            options = '' if args.write_behind else '/X '
            batch = batch[:-1] + [f'C:\\SMARTDRV.EXE {options}1024 > A:\\CACHE.TXT',
                                  'EXITTEST.COM']
        if args.preflight:
            expected = 'hidden' if phase == 'hidden' or (phase == 'fat32' and args.kernel == 'msdos622') else 'mounted'
            if args.smartdrv:
                expected = 'cached'
            if args.ramdrive:
                expected = 'unknown'
                batch = batch[:-1] + ['echo RAM drive available > D:\\RAMTEST.TXT',
                                      'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
                                      'dir D:\\ > A:\\RAMDIR.TXT', 'EXITTEST.COM']
            batch = batch[:-1] + [f'BIOSCHK {expected} > A:\\PREFLT.TXT',
                                  'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        refusal_checks = []
        if args.repair_refusals:
            refusal_checks = [
                ('--bios 81', 'C:\\NOPART.MDJ', 'BIOS writes require --partition'),
                ('--bios 80 --partition 1', 'A:\\MOUNT.MDJ', 'mounted DOS volume'),
                ('--bios 81 --partition 1', 'Z:\\UNKNOWN.MDJ',
                 'mounted DOS volume' if phase == 'clones' or (phase == 'fat32' and args.kernel != 'msdos622') else
                 'different known local physical disk')]
            if phase == 'clones' or (phase == 'fat32' and args.kernel != 'msdos622'):
                refusal_checks += [('--bios 81 --partition 1', 'C:\\MOUNT.MDJ', 'mounted DOS volume')]
            if args.smartdrv:
                refusal_checks += [('--bios 81 --partition 1', 'C:\\CACHE.MDJ', 'Resident disk cache detected')]
            batch = batch[:-1]
            for number, (source, journal, _) in enumerate(refusal_checks):
                batch += [f'MAGICDSK {source} --mark-bad 1000 --apply --journal {journal} > A:\\REFUSE{number}.TXT',
                          'if not errorlevel 2 echo FAIL >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders, timeout=args.timeout)
        report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
        preflight_report = command('mtype', '-i', str(boot), '::/PREFLT.TXT').stdout.decode() if args.preflight else ''
        assert 'DOS  BIOS device (kernel drive-data table)' in report and 'FAIL' not in report, (report, preflight_report)
        found = {letter: int(number, 16) for letter, number in re.findall(r'([A-Z]):   ([0-9A-F]+)h', report)}
        assert found.get('A') == 0 and found.get('B') == 0, (phase, found)
        assert found.get('C') == 0x80, (phase, found)
        if phase == 'clones':
            assert found.get('D') == 0x81 and found.get('E') == 0x81, found
        elif phase == 'fat32' and args.kernel != 'msdos622':
            assert found.get('D') == 0x81 and 'E' not in found, found
        else:
            assert 'D' not in found and 'E' not in found, (phase, found)
        assert 'Z' not in found, found
        if args.exports:
            # Each phase creates another recovery directory on the separate C:.
            recovery = '::/EXPORT/REC' + f'{["clones", "hidden", "fat32"].index(phase) + 1:05d}'
            manifest = command('mtype', '-i', system_image, recovery + '/RECOVERY.TXT').stdout.decode()
            assert 'FINAL COMPLETE ' in manifest, manifest
            exported = {}
            name = None
            for line in manifest.splitlines():
                if line.startswith('LIVE_BEGIN '):
                    name = bytes.fromhex(line.split('path_hex=')[1]).decode()
                    exported[name] = bytearray()
                elif line.startswith('FILE_BEGIN '):
                    exported[name] += command('mtype', '-i', system_image,
                                              recovery + '/' + line.split()[1]).stdout
            assert exported['PAYLOAD.TXT'] == payload.read_bytes(), exported.keys()
            if phase != 'fat32':
                assert exported['MAGICDSK.EXE'] == args.executable.read_bytes()
            for number in range(5 if phase == 'clones' else 4):
                denied = command('mtype', '-i', str(boot), f'::/DENY{number}.TXT').stdout.decode()
                assert 'separate DOS volume' in denied, denied
        if args.preflight:
            preflight = command('mtype', '-i', str(boot), '::/PREFLT.TXT').stdout.decode()
            assert ('PASS: resident disk cache refused' if args.smartdrv else
                    'PASS: BIOS write preflight') in preflight, preflight
        for number, (_, journal, message) in enumerate(refusal_checks):
            refused = command('mtype', '-i', str(boot), f'::/REFUSE{number}.TXT').stdout.decode()
            assert message in refused, refused
            if journal.startswith('C:'):
                exists = subprocess.run(['mtype', '-i', system_image, '::/' + journal[3:]], capture_output=True)
                assert exists.returncode != 0, journal
        assert hard.read_bytes() == target, phase
        checks = ('; export checks' if args.exports else '') + ('; write preflight' if args.preflight else '')
        if args.smartdrv:
            checks += '; SMARTDrive refused'
        if args.ramdrive:
            checks += '; unknown local RAM disk refused'
        print(f'PASS: {args.kernel} {phase}: {found}; target unchanged{checks}')
assert digest(args.boot) == source_hash
