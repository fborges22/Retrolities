"""QEMU BIOS discovery and inspection without MBR entries, including FAT32 backup."""
import argparse
from pathlib import Path
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import create, command, PARENT, CHILD, UNICODE_FILE
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', required=True, choices=['msdos622', 'freedos', 'win98'])
parser.add_argument('--ui', action='store_true')
parser.add_argument('--bounds', type=Path)
args = parser.parse_args()
boot_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-discovery-dos-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    if args.bounds:
        put(system_image, args.bounds, 'VSBOUND.EXE')
    if args.ui:
        put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data = volume.read_bytes()
        target_cylinders = (63 + len(data) // 512 + 1007) // 1008
        hard = root / 'TARGET.IMG'
        raw = bytearray(target_cylinders * 1008 * 512)
        raw[63 * 512:63 * 512 + len(data)] = data
        # Exercise backup inference in the FAT32 case and primary detection in others.
        if bits == 32:
            raw[63 * 512 + 510:63 * 512 + 512] = bytes(2)
        hard.write_bytes(raw)
        before = digest(hard)
        options = ' --boot-source 6' if bits == 32 else ''
        batch = ['@echo off', 'C:', 'cd \\',
                 'MAGICDSK --bios 81 --find-volumes --search-count 72 > A:\\FOUND.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\FOUND.TXT',
                 'MAGICDSK --bios 81 --volume-start 63' + options + ' > A:\\SCAN.TXT',
                 ('if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT' if bits == 32 else
                  'if errorlevel 1 echo FAIL >> A:\\SCAN.TXT'),
                 'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT', 'EXITTEST.COM']
        batch[3:3] = [f'MD C:\\CLI{bits}',
                      'MAGICDSK --bios 81 --volume-start 63' + options +
                      f' --export-files C:\\CLI{bits} > A:\\EXPORT.TXT',
                      ('if not errorlevel 1 echo FAIL >> A:\\EXPORT.TXT' if bits == 32 else
                       'if errorlevel 1 echo FAIL >> A:\\EXPORT.TXT'),
                      'if errorlevel 2 echo FAIL >> A:\\EXPORT.TXT']
        if args.bounds and bits == 12:
            batch[3:3] = ['VSBOUND > A:\\BOUNDS.TXT', 'if errorlevel 1 echo FAIL >> A:\\BOUNDS.TXT']
        if args.ui:
            menu = (step('Open image...', 'down', True) +
                    step('BIOS device...', 'down', True) +
                    step('DOS volume...', 'down', True) +
                    step('Select volume...', 'down', True) +
                    step('Search lost volumes...', 'enter', True))
            search = (typing('VOLUME SEARCH: FIRST DISK LBA', '0') +
                      typing('VOLUME SEARCH: SECTOR COUNT', '72'))
            keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                    step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                    step('BIOS 81h', 'f10', absent='BIOS DEVICE:') + menu + search +
                    step('FOUND FAT VOLUMES - READ ONLY', 'esc') +
                    step('BIOS 81h', 'f10', absent='FOUND FAT VOLUMES') + menu + search +
                    step('FOUND FAT VOLUMES - READ ONLY', 'enter') +
                    step('Discovered volume: read-only inspection', 'f3') +
                    step('Check finished.', 'f7') +
                    typing('SAVE REPORT TO A SEPARATE DISK', f'A:\\UI{bits}.TXT') +
                    step('Report saved.', 0x3c00) +
                    step('DISCOVERED VOLUMES (READ ONLY)', 'esc') +
                    step('Report saved.', 'f10', absent='DISCOVERED VOLUMES') +
                    step('Open image...', 'right', True) + step('Filesystem check', 'right', True) +
                    step('Allocation map', 'right', True) + step('Save report...', 'down', True) +
                    step('Export live files...', 'enter', True) +
                    step('EXPORT LIVE FILES', 'enter') +
                    typing('RECOVERY DESTINATION DIRECTORY', f'C:\\OUT{bits}') +
                    step('LIVE-FILE EXPORT RESULTS', 'enter') + step('Recovery finished', 'esc'))
            put(system_image, text_file(root, 'UIVOL.KEY', keys))
            batch[-1:-1] = [f'MD C:\\OUT{bits}', 'KEYPLAY UIVOL.KEY', 'MAGICDSK --tui']
        put(system_image, text_file(root, 'RUN.BAT' , '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        found = command('mtype', '-i', str(boot), '::/FOUND.TXT').stdout.decode()
        scan = command('mtype', '-i', str(boot), '::/SCAN.TXT').stdout.decode()
        assert 'FAIL' not in found + scan, found + scan
        assert f'FAT{bits}  LBA 63' in found and '1 candidates' in found, found
        assert f'FAT{bits} |' in scan, scan
        if bits == 32:
            assert 'boot-source 6' in found and 'BOOT_PRIMARY' in scan, found + scan
        if args.ui:
            report = command('mtype', '-i', str(boot), f'::/UI{bits}.TXT').stdout.decode()
            assert f'FAT{bits}' in report, report
            assert ('BOOT_PRIMARY' in report) == (bits == 32), report
        exported = command('mtype', '-i', str(boot), '::/EXPORT.TXT').stdout
        assert b'FAIL' not in exported and b'54 files' in exported, exported
        for prefix in (['CLI', 'OUT'] if args.ui else ['CLI']):
            directory = f'::/{prefix}{bits}/REC00001/'
            manifest = command('mtype', '-i', system_image, directory + 'RECOVERY.TXT').stdout.decode('utf-8')
            names = set()
            payloads = []
            for line in manifest.splitlines():
                if line.startswith('LIVE_BEGIN '):
                    names.add(bytes.fromhex(line.split('path_hex=')[1]).decode('utf-8'))
                elif line.startswith('FILE_BEGIN '):
                    payloads.append(command('mtype', '-i', system_image, directory + line.split()[1]).stdout)
            expected = {f'{PARENT}/{CHILD}/Long report number {i:02d}.txt' for i in range(50)}
            expected.update([f'{PARENT}/{CHILD}/' + 'a' * 13,
                             f'{PARENT}/{CHILD}/' + 'b' * 26,
                             'x' * 255, f'{PARENT}/{CHILD}/{UNICODE_FILE}'])
            assert names == expected and len(payloads) == 54, names
            assert all(data == b'MagicDisk LFN preservation test.\r\n' for data in payloads)

        if args.bounds and bits == 12:
            bounds = command('mtype', '-i', str(boot), '::/BOUNDS.TXT').stdout
            assert b'PASS:' in bounds and b'FAIL' not in bounds, bounds
        assert digest(hard) == before
        print(f'PASS {args.kernel} FAT{bits}: BIOS discovery, scan and LFN export, exact payloads and unchanged source', flush=True)
assert digest(args.boot) == boot_hash
