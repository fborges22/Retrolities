#!/usr/bin/env python3
"""Reserved-FAT diagnostics, guarded repair and undo in DOS/QEMU."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, Layout
from ui_scripts import device_keys, diagnose, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)

with tempfile.TemporaryDirectory(prefix='magicdisk-fat-header-') as directory:
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
        width = {12: 3, 16: 4, 32: 8}[bits]
        mbr = bytearray(63 * 512)
        partition(mbr, size // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + size // 512 + 1007) // 1008 + 1
        tail = bytes(target_cylinders * 1008 * 512 - len(mbr) - len(clean))
        hard = root / 'TARGET.IMG'
        for phase in ['identical', 'repair-undo']:
            damaged = bytearray(clean)
            for copy in ([0, 1] if phase == 'identical' else [1]):
                start = (layout.reserved + copy * layout.fatsz) * 512
                damaged[start:start + width] = bytes(width)
            original = mbr + damaged + tail
            hard.write_bytes(original)
            batch = ['@echo off', 'C:', 'cd \\',
                     'MAGICDSK --bios 81 --partition 1 > A:\\SCAN.TXT',
                     'if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT',
                     'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT']
            journal = f'H{bits}.MDJ'
            if phase == 'identical':
                batch += [f'MAGICDSK --bios 81 --partition 1 --mark-bad 1000 --apply --journal C:\\{journal} > A:\\RESULT.TXT',
                          'if not errorlevel 2 echo FAIL >> A:\\RESULT.TXT']
            else:
                batch += [f'MAGICDSK --bios 81 --partition 1 --repair-fat 1 --apply --journal C:\\{journal} > A:\\RESULT.TXT',
                          'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
                          'MAGICDSK --bios 81 --partition 1 > A:\\CLEAN.TXT',
                          'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
                          f'MAGICDSK --bios 81 --partition 1 --restore-journal C:\\{journal} --apply >> A:\\RESULT.TXT',
                          'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders)
            scan = command('mtype', '-i', str(boot), '::/SCAN.TXT').stdout.decode()
            result = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            assert 'FAIL' not in scan + result, scan + result
            assert scan.count('FAT_HEADER') == (4 if phase == 'identical' else 2), scan
            if phase == 'identical':
                assert 'FAT_MIRROR' not in scan, scan
                exists = subprocess.run(['mtype', '-i', system_image, '::/' + journal], capture_output=True)
                assert exists.returncode != 0
            else:
                assert 'committed and verified' in result and 'restored and verified' in result, result
                report = command('mtype', '-i', str(boot), '::/CLEAN.TXT').stdout.decode()
                assert '0 issues' in report and 'FAT_HEADER' not in report, report
            assert hard.read_bytes() == original, (bits, phase)
            print(f'PASS: {args.kernel} FAT{bits} {phase}: header findings, guarded writes and exact source bytes')

        # Rebuild headers even when no intact copy remains. Each boot checks
        # exact on-disk bytes, including rollback after a second-copy failure.
        damaged = bytearray(clean)
        expected = bytearray(clean)
        for copy in range(clean[16]):
            start = (layout.reserved + copy * layout.fatsz) * 512
            damaged[start:start + width] = bytes(width)
            if bits == 16:
                struct.pack_into('<H', expected, start + 2, 0x3fff)
            elif bits == 32:
                struct.pack_into('<I', expected, start + 4, 0x03ffffff)
        original = mbr + damaged + tail
        rebuilt = mbr + expected + tail
        journal = f'R{bits}.MDJ'
        for phase in ['rebuild', 'undo', 'rollback', 'tui', 'tui-undo']:
            hard.write_bytes(rebuilt if phase in ['undo', 'tui-undo'] else original)
            backend = None
            if phase == 'rollback':
                errors = root / 'header-error.conf'
                errors.write_text('[inject-error]\nevent = "write_aio"\niotype = "write"\n'
                                  f'sector = "{63 + layout.reserved + layout.fatsz}"\n'
                                  'errno = "5"\nonce = "on"\n')
                backend = f'blkdebug:{errors}:{hard}'
            name = f'F{bits}.MDJ' if phase == 'rollback' else journal
            if phase == 'tui':
                name = f'U{bits}.MDJ'
                select = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                          step('Other device...', 'enter', True) +
                          typing('BIOS DEVICE: 00=A', '81'))
                keys = (select + diagnose() + step('Filesystem check', 'up', True) +
                        step('Clear invalid timestamps', 'up', True) +
                        step('Clear invalid long names', 'up', True) +
                        step('Repair invalid DOS names', 'up', True) +
        step('Repair volume labels', 'up', True) +
        step('Repair volume label text', 'up', True) +
        step('Repair name conflicts', 'up', True) +
        step('Repair reserved attributes', 'up', True) +
        step('Remove extra dot entries', 'up', True) +
                        step('Rebuild FAT32 FSInfo', 'up', bits == 32) +
                        step('Rebuild backup boot', 'up', bits == 32) +
                        step('Rebuild FAT copies', 'up', True) +
                        step('Rebuild FAT headers', 'enter', True) +
                        step('REVIEW FAT HEADERS', 'enter') +
                        typing('TYPE REBUILD TO REPAIR FAT HEADERS', 'REBUILD') +
                        typing('NEW JOURNAL FILE ON A SEPARATE DISK', 'C:\\' + name) +
                        step('BIOS REPAIR REQUIREMENTS', 'enter') +
                        step('Repair verified.', 'esc'))
                put(system_image, text_file(root, 'HEADER.KEY', keys))
                batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY HEADER.KEY',
                         'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
                         'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT']
            else:
                if phase == 'tui-undo':
                    name = f'U{bits}.MDJ'
                operation = ('--restore-journal C:\\' + name
                             if phase in ['undo', 'tui-undo'] else
                             '--repair-fat-headers --journal C:\\' + name)
                batch = ['@echo off', 'C:', 'cd \\',
                         f'MAGICDSK --bios 81 --partition 1 {operation} --apply > A:\\RESULT.TXT',
                         ('if not errorlevel 2' if phase == 'rollback' else 'if errorlevel 1') +
                         ' echo FAIL >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            result = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            assert 'FAIL' not in result, result
            assert hard.read_bytes() == (rebuilt if phase in ['rebuild', 'tui'] else original), (bits, phase, result)
            print(f'PASS: {args.kernel} FAT{bits} {phase}: exact header repair/rollback/undo bytes')

assert digest(args.boot) == source_hash
