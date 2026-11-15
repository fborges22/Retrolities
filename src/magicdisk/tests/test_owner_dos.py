#!/usr/bin/env python3
"""Inspect cluster owners through the production map and sector viewer in QEMU."""
import argparse
from pathlib import Path
import struct
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
with tempfile.TemporaryDirectory(prefix='magicdisk-owner-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    export_index = 0
    for bits in [12, 16, 32]:
        data, layout, chain, payload, entry = create_selected(root / 'VOLUME.IMG', bits)
        data = bytearray(data)
        directory_entry = next(offset for offset in layout.entries() if data[offset:offset + 11] == b'DIRECTRY   ')
        directory_cluster = layout.cluster(directory_entry)
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        mbr = bytearray(63 * 512)
        partition(mbr, len(data) // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + len(data) // 512 + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        hard.write_bytes(original)
        data_start = 63 + layout.data_sector
        for phase in ['complete', 'incomplete', 'shared']:
            expected_disk = bytearray(original)
            if phase == 'shared':
                other = next(offset for offset in layout.entries() if data[offset:offset + 11] == b'OTHER   TXT')
                struct.pack_into('<H', expected_disk, 63 * 512 + other + 26, chain[1])
                struct.pack_into('<I', expected_disk, 63 * 512 + other + 28, len(payload) - layout.spc * 512)
            hard.write_bytes(expected_disk)
            state = ('Lookup INCOMPLETE;' if phase == 'incomplete' else
                     'Scan findings may affect ownership.' if phase == 'shared' else 'Catalog complete;')
            keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                    step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                    step('Allocation view ready.', ord('o')) +
                    step('CLUSTER OWNER', 'home') +
                    step('FAT32 root directory' if bits == 32 else 'Regular file', 'home') +
                    step(state, 'esc') +
                    step('Allocation view ready.', 'f10') + step('Open image...', 'right', True) +
                    step('Filesystem check', 'right', True) + step('Allocation map', 'up', True) +
                    step('Cluster owner...', 'enter', True) + step(state, 'esc') +
                    step('Allocation view ready.', 'f8'))
            current = data_start
            targets = chain + [directory_cluster, 1000] if phase == 'complete' else [chain[1]]
            for cluster in targets:
                lba = data_start + (cluster - 2) * layout.spc
                keys += (step(f'Disk LBA {current} /', ord('g')) +
                         typing('GO TO ABSOLUTE DISK LBA', str(lba)) +
                         step(f'Disk LBA {lba} /', ord('o')) +
                         step(f'Cluster {cluster} ', 'home'))
                if cluster in chain:
                    keys += (step('Regular file', 'home') +
                             step(f'First cluster {chain[0]}', 'home') +
                             step(f'{len(payload)} bytes', 'home') +
                             step('anual de manuten', 'home') +
                             step(f'Directory entry: disk LBA {63 + entry // 512} byte {entry % 512}', 'home'))
                elif cluster == directory_cluster:
                    keys += (step('DIRECTRY', 'home') +
                             step(f'First cluster {directory_cluster}', 'home') +
                             step(f'Directory entry: disk LBA {63 + directory_entry // 512} byte {directory_entry % 512}', 'home'))
                else:
                    keys += step('No owner found in the scanned allocated prefixes.', 'home')
                if cluster == chain[1]:
                    if phase == 'complete':
                        keys += (step('R: export file', ord('r')) +
                                 step('Export the regular file owning the selected cluster.', 'esc') +
                                 step(f'Disk LBA {lba} /', ord('o')))
                    keys += (step('R: export file', ord('r')) +
                             step('Export the regular file owning the selected cluster.', 'enter') +
                             typing('RECOVERY DESTINATION DIRECTORY', 'C:\\') +
                             step('Directory catalog INCOMPLETE:' if phase == 'incomplete' else
                                  'Export finished. Source allocation is unchanged.', 'enter'))
                else:
                    if cluster not in chain:
                        keys += step(state, ord('r'))
                    keys += step(state, 'esc')
                current = lba
            keys += (step(f'Disk LBA {current} /', ord('g')) + typing('GO TO ABSOLUTE DISK LBA', '0') +
                     step('Disk LBA 0 /', ord('o')) + step('NOT A DATA CLUSTER', 'esc') +
                     step('Disk LBA 0 /', 'esc') +
                     step('Enter: details', 'esc', absent='READ-ONLY SECTOR VIEWER'))
            put(system_image, text_file(root, 'OWNER.KEY', keys))
            batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY OWNER.KEY',
                     'if errorlevel 1 echo FAIL > A:\\RESULT.TXT',
                     'MAGICDSK --tui', 'echo UI completed >> A:\\RESULT.TXT', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            backend = None
            if phase == 'incomplete':
                errors = root / 'read-error.conf'
                failed = data_start + (directory_cluster - 2) * layout.spc
                errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                                  f'sector = "{failed}"\nerrno = "5"\nonce = "off"\n')
                backend = f'blkdebug:{errors}:{hard}'
            run_vm(boot, system, cylinders, hard, target_cylinders, backend=backend)
            report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
            assert 'UI completed' in report and 'FAIL' not in report, report
            export_index += 1
            folder = f'::/REC{export_index:05d}/'
            manifest = command('mtype', '-i', system_image, folder + 'RECOVERY.TXT').stdout.decode()
            assert manifest.count('LIVE_BEGIN ') == 1
            assert f'SELECTION cluster={chain[1]} ' in manifest
            assert 'path_hex=' + UNICODE_FILE.encode().hex().upper() in manifest
            parts = [line.split()[1] for line in manifest.splitlines() if line.startswith('FILE_BEGIN ')]
            assert len(parts) == 1
            assert command('mtype', '-i', system_image, folder + parts[0]).stdout == payload
            expected_state = ('INCOMPLETE' if phase == 'incomplete' else
                              'COMPLETE_WITH_FINDINGS' if phase == 'shared' else 'COMPLETE')
            assert 'FINAL ' + expected_state + ' ' in manifest
            assert hard.read_bytes() == expected_disk
            print(f'PASS: {args.kernel} FAT{bits} {phase}: map/sector owner lookup, LFN, file/directory/free clusters and unchanged disk')
assert digest(args.boot) == source_hash
