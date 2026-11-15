#!/usr/bin/env python3
"""Production DOS export around unreadable directory sectors; repairs remain strict."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, create
from duplicate_fixture import alias_name
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--root-gap', action='store_true')
parser.add_argument('--ui', action='store_true')
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-directory-gap-dos-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    if args.ui:
        put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for index, bits in enumerate([12, 16, 32], 1):
        volume = root / 'VOLUME.IMG'
        layout = create(volume, bits)
        data = bytearray(volume.read_bytes())
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        failed = layout.slots(child)[16] // 512
        files = list(layout.long_names(child))
        expected = {entry for _, entry in files if entry // 512 != failed}
        expected.update(entry for _, entry in layout.long_names(layout.root)
                        if not (data[entry + 11] & 16))
        if args.root_gap:
            failed = layout.slots(layout.root)[0] // 512
            assert layout.child() // 512 == failed
            files = [(parts, entry) for parts, entry in layout.long_names(layout.root)
                     if not (data[entry + 11] & 16)]
            expected = {entry for _, entry in files if entry // 512 != failed}
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        mbr = bytearray(63 * 512)
        partition(mbr, len(data) // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + len(data) // 512 + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        hard.write_bytes(original)
        errors = root / 'read-error.conf'
        errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                          f'sector = "{63 + failed}"\nerrno = "5"\nonce = "off"\n')
        batch = ['@echo off', 'C:', 'cd \\',
                 'MAGICDSK --bios 81 --partition 1 > A:\\SCAN.TXT',
                 'if not errorlevel 2 echo FAIL >> A:\\SCAN.TXT',
                 'MAGICDSK --bios 81 --partition 1 --export-files C:\\ > A:\\EXPORT.TXT',
                 'if not errorlevel 2 echo FAIL >> A:\\EXPORT.TXT',
                 'MAGICDSK --bios 81 --partition 1 --repair-files --apply --journal C:\\REFUSED.MDJ > A:\\REPAIR.TXT',
                 'if not errorlevel 2 echo FAIL >> A:\\REPAIR.TXT', 'EXITTEST.COM']
        if args.ui:
            keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                    step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                    step('Allocation view ready.', 'f10') + step('Open image...', 'right', True) +
                    step('Filesystem check', 'right', True) + step('Allocation map', 'right', True) +
                    step('Save report...', 'down', True) + step('Export live files...', 'enter', True) +
                    step('Export live files before repairing their chains.', 'enter') +
                    typing('RECOVERY DESTINATION DIRECTORY', 'C:\\') +
                    step('Directory catalog INCOMPLETE: files or owners may be missing.', 'home') +
                    step('0 unreadable payload sectors;', 'enter') +
                    step('File export incomplete;', 'esc'))
            put(system_image, text_file(root, 'GAPS.KEY', keys))
            batch[-1:-1] = ['KEYPLAY GAPS.KEY', 'if errorlevel 1 echo FAIL >> A:\\EXPORT.TXT',
                            'MAGICDSK --tui']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders, backend=f'blkdebug:{errors}:{hard}')
        for name in ['SCAN', 'EXPORT', 'REPAIR']:
            report = command('mtype', '-i', str(boot), f'::/{name}.TXT').stdout.decode()
            assert 'FAIL' not in report, report
            if name == 'SCAN':
                assert 'SCAN_INCOMPLETE' in report, report
            if name == 'EXPORT':
                assert 'incomplete' in report, report
                assert 'Directory catalog INCOMPLETE:' in report, report
                assert 'counts above describe file payload only.' in report, report
        folder = f'::/REC{index * (2 if args.ui else 1):05d}/'
        manifest = command('mtype', '-i', system_image, folder + 'RECOVERY.TXT').stdout.decode()
        exported = {(int(lba) - 63) * 512 + int(offset) for lba, offset in
                    re.findall(r'LIVE_BEGIN entry_lba=(\d+) offset=(\d+)', manifest)}
        assert exported == expected, exported ^ expected
        parts_written = [line.split()[1] for line in manifest.splitlines() if line.startswith('FILE_BEGIN ')]
        assert len(parts_written) == len(expected)
        for name in parts_written:
            assert command('mtype', '-i', system_image, folder + name).stdout == b'MagicDisk LFN preservation test.\r\n'
        assert f'ISSUE code=DIRECTORY_GAP where={63 + failed}' in manifest
        assert 'UNRESOLVED_CLUSTERS' in manifest and 'LOST_CLUSTERS' not in manifest
        assert 'Directory catalog: INCOMPLETE' in manifest and 'FINAL INCOMPLETE' in manifest
        for parts, entry in files:
            if entry // 512 != failed and any(part // 512 == failed for part in parts):
                line = next(line for line in manifest.splitlines()
                            if line.startswith(f'LIVE_BEGIN entry_lba={63 + entry // 512} offset={entry % 512} '))
                name = bytes.fromhex(line.split('path_hex=')[1]).decode()
                assert name.rsplit('/', 1)[-1] == alias_name(data, entry), name
        assert subprocess.run(['mtype', '-i', system_image, '::/REFUSED.MDJ'], capture_output=True).returncode != 0
        assert hard.read_bytes() == original
        print(f'PASS: {args.kernel} FAT{bits}: {"root" if args.root_gap else "child"} directory gap, later entries, LFN boundary, exact exports, incomplete result and repair refusal')
assert digest(args.boot) == source_hash
