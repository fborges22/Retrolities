#!/usr/bin/env python3
"""Selected map-block retests retain exact range accounting in real DOS/QEMU."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, Layout
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-range-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.executable.with_name('KEYPLAY.EXE'))
    for bits, size in [(12, 1440 * 1024), (16, 32 * 1024 * 1024), (32, 128 * 1024 * 1024)]:
        volume = root / 'VOLUME.IMG'
        with volume.open('wb') as file:
            file.truncate(size)
        command('mkfs.fat', '-F', str(bits), '-s', '2', str(volume))
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        layout = Layout(data, bits)
        clusters = (size // 512 - layout.data_sector) // layout.spc
        first = layout.data_sector + (clusters // 384) * layout.spc
        count = (clusters * 2 // 384 - clusters // 384) * layout.spc
        second_count = count // min(count, 384)
        mbr = bytearray(63 * 512)
        partition(mbr, size // 512, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + size // 512 + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        errors = root / 'read-error.conf'
        errors.write_text('[inject-error]\nevent = "read_aio"\niotype = "read"\n'
                          f'sector = "{63 + first}"\nerrno = "5"\nonce = "off"\n')
        for failed in [False, True]:
            hard.write_bytes(original)
            keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                    step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                    step('Allocation view ready.', 'right') + step('Block 2', ord('t')) +
                    step('RANGE COMPLETE', 'home') + step(f'{count}/{count} read', 'f7') +
                    typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\RANGE.TXT') +
                    step('Report saved.', 'f10') + step('Open image...', 'right', True) +
                    step('Filesystem check', 'right', True) + step('Allocation map', 'up', True) +
                    step('Cluster owner...', 'up', True) + step('Retest map block', 'enter', True) +
                    step('RANGE COMPLETE', 'home') + step(f'{second_count}/{second_count} read', 'f7') +
                    typing('SAVE REPORT TO A SEPARATE DISK', 'A:\\SECOND.TXT') +
                    step('Report saved.', 'f5') + step('Allocation view ready.', 'home') +
                    step('RANGE COMPLETE', 'home') + step(f'{second_count}/{second_count} read', 'esc'))
            put(system_image, text_file(root, 'RANGE.KEY', keys))
            put(system_image, text_file(root, 'RUN.BAT',
                                       '@echo off\nC:\ncd \\\nKEYPLAY RANGE.KEY\nMAGICDSK --tui\nEXITTEST.COM\n'))
            for name in ['RANGE.TXT', 'SECOND.TXT']:
                # Reports deliberately refuse overwrite. Remove only test outputs
                # from the disposable boot copy before the next run.
                subprocess.run(['mdel', '-i', str(boot), '::/' + name], capture_output=True)
            run_vm(boot, system, cylinders, hard, target_cylinders,
                   backend=f'blkdebug:{errors}:{hard}' if failed else None)
            for name, sectors in [('RANGE.TXT', count), ('SECOND.TXT', second_count)]:
                report = command('mtype', '-i', str(boot), '::/' + name).stdout.decode()
                assert 'Surface read: complete' in report, report
                assert f'Sectors tested: {sectors} of {sectors}  Unreadable: {int(failed)}' in report, report
                assert 'Scope: selected map block' in report, report
                assert f'Volume sectors: {first}-{first + sectors - 1}' in report, report
                assert f'Disk LBAs: {63 + first}-{63 + first + sectors - 1}' in report, report
                assert report.count('SURFACE_READ') == int(failed), report
            assert hard.read_bytes() == original
            print(f'PASS: {args.kernel} FAT{bits} range retest failed={failed}: exact ranges, counts and source')
assert digest(args.boot) == source_hash
