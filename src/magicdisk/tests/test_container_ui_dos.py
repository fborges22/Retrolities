"""QEMU direct BIOS previews of a missing extended container on FAT12/16/32 disks."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import create, command
from ui_scripts import device_keys, step, typing

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', required=True, choices=['msdos622', 'freedos', 'win98'])
args = parser.parse_args()
boot_hash = digest(args.boot)

def chs(lba):
    cylinder, remainder = divmod(lba, 1008)
    head, sector = divmod(remainder, 63)
    return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])

def entry(kind, absolute, relative, size):
    return bytes([0]) + chs(absolute) + bytes([kind]) + chs(absolute + size - 1) + struct.pack('<II', relative, size)

with tempfile.TemporaryDirectory(prefix='magicdisk-link-dos-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.probe.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume, disk = root / 'VOL.IMG', root / 'TARGET.IMG'
        create(volume, bits)
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        sectors = len(data) // 512
        target = sectors + 189
        target_cylinders = (target + 63 + sectors + 1007) // 1008
        total = target_cylinders * 1008
        raw = bytearray(total * 512)
        raw[510:512] = b'\x55\xaa'
        for record in [63, target]:
            raw[(record + 63) * 512:(record + 63 + sectors) * 512] = data
            raw[record * 512:record * 512 + 446] = bytes((i * 13 + 7) % 256 for i in range(446))
            raw[record * 512 + 446:record * 512 + 462] = entry({12:1,16:6,32:11}[bits], record + 63, 63, sectors)
            raw[record * 512 + 510:record * 512 + 512] = b'\x55\xaa'
        raw[63 * 512 + 462:63 * 512 + 478] = entry(15, target, target - 63, total - target)
        disk.write_bytes(raw)
        original = digest(disk)
        menu = (step('Open image...', 'pgdn', True) +
                step('Undo interrupted record...', 'up', True) +
                step('Undo whole EBR...', 'up', True) +
                step('Rebuild whole EBR...', 'up', True) +
                step('Undo interrupted container...', 'up', True) +
                step('Undo extended container...', 'up', True))
        numbers = (typing('CONTAINER: MBR SLOT', '1') +
                   typing('CONTAINER: FIRST EBR DISK LBA', '63') +
                   typing('CONTAINER: TOTAL SECTORS', str(total-63)) +
                   typing('CONTAINER TYPE:', '0F') +
                   typing('CONTAINER: BIOS GEOMETRY HEADS', '16') +
                   typing('CONTAINER: BIOS SECTORS PER TRACK', '63'))
        keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                step('BIOS 81h', 'f10', absent='BIOS DEVICE:') + menu +
                step('Rebuild extended container...', 'enter', True) + numbers +
                step('REVIEW EXTENDED CONTAINER', 'esc') +
                step('BIOS 81h', 'f10', absent='REVIEW EXTENDED CONTAINER') + menu +
                step('Rebuild extended container...', 'enter', True) + numbers +
                step('REVIEW EXTENDED CONTAINER', 'enter') +
                typing('NEW CONTAINER JOURNAL PATH', f'C:\\C{bits}.JRN') +
                typing('TYPE CONTAINER TO RECONSTRUCT', 'CONTAINER') +
                step('BIOS REPAIR REQUIREMENTS', 'enter') +
                step('Container inserted and verified.', 'f10') + menu +
                step('Rebuild extended container...', 'down', True) +
                step('Undo extended container...', 'enter', True) +
                typing('CONTAINER JOURNAL TO RESTORE', f'C:\\C{bits}.JRN') +
                step('REVIEW CONTAINER UNDO', 'enter') +
                typing('TYPE RESTORE TO UNDO CONTAINER', 'RESTORE') +
                step('BIOS REPAIR REQUIREMENTS', 'enter') +
                step('Container restored and verified.', 'esc'))
        assert len(keys.splitlines()) <= 128
        put(system_image, text_file(root, 'CONT.KEY', keys))
        batch = ['@echo off', 'C:', 'KEYPLAY CONT.KEY', 'if errorlevel 1 goto end',
                 'MAGICDSK --tui', ':end', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, disk, target_cylinders)
        expected = bytearray(raw[:512])
        expected[446:462] = entry(15, 63, 63, total - 63)
        saved = command('mtype', '-i', system_image, f'::/C{bits}.JRN').stdout
        assert len(saved) == 3152 and saved[:8] == b'MDCTR001'
        assert saved[64:576] == raw[:512] and saved[576:1088] == expected
        assert digest(disk) == original
        print(f'PASS {args.kernel} FAT{bits}: TUI container review cancellation, typed apply/undo and exact disk preservation', flush=True)
assert digest(args.boot) == boot_hash
