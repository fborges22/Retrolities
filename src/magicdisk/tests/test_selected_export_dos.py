#!/usr/bin/env python3
"""Selected-file export through DOS CLI and failed-sector findings in QEMU."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, UNICODE_FILE
from selected_export_fixture import create_selected
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-selected-dos-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    export_index = 0
    for bits in [12, 16, 32]:
        data, layout, chain, payload, _ = create_selected(root / 'VOLUME.IMG', bits)
        data = bytearray(data)
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        mbr = bytearray(63 * 512)
        partition(mbr, len(data) // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + len(data) // 512 + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        hard.write_bytes(original)
        target = chain[1]
        failed_sector = 63 + layout.data_sector + (target - 2) * layout.spc
        errors = root / 'read-error.conf'
        errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                          f'sector = "{failed_sector}"\nerrno = "5"\nonce = "off"\n')
        for phase in ['healthy', 'gap', 'ui', 'no-owner']:
            backend = f'blkdebug:{errors}:{hard}' if phase in ['gap', 'ui'] else None
            if phase == 'ui':
                keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                        step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                        step('Allocation view ready.', 'f4') + step('Surface read finished.', 'f6') +
                        step('R: find and export the regular file', ord('r')) +
                        step('Export the regular file owning the selected cluster.', 'enter') +
                        typing('RECOVERY DESTINATION DIRECTORY', 'C:\\') +
                        step('LIVE-FILE EXPORT RESULTS', 'enter') +
                        step('Recovery finished with recorded gaps or damage.', 'esc'))
                put(system_image, text_file(root, 'EXPORT.KEY', keys))
                batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY EXPORT.KEY',
                         'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
                         'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT']
            else:
                cluster = 1000 if phase == 'no-owner' else target
                batch = ['@echo off', 'C:', 'cd \\',
                         f'MAGICDSK --bios 81 --partition 1 --export-cluster {cluster} C:\\ > A:\\RESULT.TXT']
                if phase == 'healthy':
                    batch += ['if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
                elif phase == 'gap':
                    batch += ['if not errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
                              'if errorlevel 2 echo FAIL >> A:\\RESULT.TXT']
                else:
                    batch += ['if not errorlevel 2 echo FAIL >> A:\\RESULT.TXT']
            batch += ['EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            assert 'FAIL' not in report, report
            assert hard.read_bytes() == original
            if phase == 'no-owner':
                assert 'No regular file owns this cluster' in report, report
                exists = subprocess.run(['mdir', '-i', system_image, f'::/REC{export_index + 1:05d}'], capture_output=True)
                assert exists.returncode != 0
            else:
                export_index += 1
                directory = f'::/REC{export_index:05d}/'
                manifest = command('mtype', '-i', system_image, directory + 'RECOVERY.TXT').stdout.decode()
                assert manifest.count('LIVE_BEGIN ') == 1, manifest
                assert 'path_hex=' + UNICODE_FILE.encode().hex().upper() in manifest
                assert f'SELECTION cluster={target} ' in manifest
                parts = [line.split()[1] for line in manifest.splitlines() if line.startswith('FILE_BEGIN ')]
                assert len(parts) == 1
                recovered = command('mtype', '-i', system_image, directory + parts[0]).stdout
                expected = bytearray(payload)
                if phase != 'healthy':
                    offset = layout.spc * 512
                    expected[offset:offset + 512] = bytes(512)
                    assert f'disk_lba={failed_sector} bytes=512' in manifest
                    assert 'attempt=3 result=failed' in manifest
                    assert 'FINAL COMPLETE_WITH_GAPS' in manifest
                else:
                    assert 'FINAL COMPLETE ' in manifest
                assert recovered == expected
            print(f'PASS: {args.kernel} FAT{bits} selected export {phase}: LFN, exact bytes, gaps and unchanged source')
assert digest(args.boot) == source_hash
