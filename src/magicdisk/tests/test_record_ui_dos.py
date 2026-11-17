"""QEMU BIOS previews of missing first, middle and terminal EBR records."""
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


with tempfile.TemporaryDirectory(prefix='magicdisk-record-dos-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.probe.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume, disk = root / 'VOL.IMG', root / 'TARGET.IMG'
        create(volume, bits)
        data = volume.read_bytes()
        sectors = len(data) // 512
        records = [63 + i * (sectors + 126) for i in range(3)]
        target_cylinders = (records[-1] + 63 + sectors + 1007) // 1008
        total = target_cylinders * 1008
        raw = bytearray(total * 512)
        raw[446:462] = entry(0x85, 63, 63, total - 63)
        raw[510:512] = b'\x55\xaa'
        for i, at in enumerate(records):
            start = at + 63
            raw[start*512:(start+sectors)*512] = data
            raw[at*512+446:at*512+462] = entry({12: 1, 16: 6, 32: 11}[bits], start, 63, sectors)
            raw[at*512+510:at*512+512] = b'\x55\xaa'
            if i < 2:
                target = records[i+1]
                raw[at*512+462:at*512+478] = entry(0x85, target, target-63, total-target)
        for i, at in enumerate(records):
            target = records[i+1] if i < 2 else 0
            damaged = bytearray(raw)
            damaged[at*512:(at+1)*512] = bytes(512)
            disk.write_bytes(damaged)
            original = digest(disk)
            def open_keys(ready='BIOS 81h'):
                keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                        step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81'))
                if ready == 'Allocation view ready.':
                    return (keys + step('SELECT VOLUME', 'enter') +
                            step(ready, 'f10', absent='SELECT VOLUME'))
                return keys + step(ready, 'f10', absent='BIOS DEVICE:')

            menu = (step('Open image...', 'end', True) + step('Exit', 'up', True) +
                    step('OEM code page...', 'up', True))
            rebuild = (menu + step('Undo interrupted record...', 'up', True) +
                       step('Undo whole EBR...', 'up', True) +
                       step('Rebuild whole EBR...', 'enter', True))
            numbers = (typing('RECORD: EBR DISK LBA', str(at)) +
                       typing('RECORD: VOLUME DISK LBA', str(at+63)) +
                       typing('RECORD: NEXT EBR LBA', str(target)) +
                       typing('RECORD: BOOT SOURCE', '0') +
                       typing('RECORD: BIOS GEOMETRY HEADS', '16') +
                       typing('RECORD: BIOS SECTORS PER TRACK', '63'))

            def tour(keys):
                assert len(keys.splitlines()) <= 128, len(keys.splitlines())
                put(system_image, text_file(root, 'REC.KEY', keys))
                batch = ['@echo off', 'C:', 'KEYPLAY REC.KEY', 'if errorlevel 1 goto end',
                         'MAGICDSK --tui', ':end', 'EXITTEST.COM']
                put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
                run_vm(boot, system, cylinders, disk, target_cylinders)

            def apply_keys(name):
                return (open_keys() + rebuild + numbers + step('REVIEW WHOLE EBR RECORD', 'enter') +
                        typing('NEW WHOLE EBR JOURNAL PATH', f'C:\\{name}') +
                        typing('TYPE RECORD TO REBUILD WHOLE EBR', 'RECORD') +
                        step('BIOS REPAIR REQUIREMENTS', 'enter'))

            def undo_keys(name, torn=False):
                select = (menu + step('Undo interrupted record...', 'enter', True) +
                          step('INTERRUPTED WHOLE EBR UNDO', 'enter')) if torn else (
                          menu + step('Undo interrupted record...', 'up', True) +
                          step('Undo whole EBR...', 'enter', True))
                return (select + typing('WHOLE EBR JOURNAL TO RESTORE', f'C:\\{name}') +
                        step('REVIEW WHOLE EBR UNDO', 'enter') +
                        typing('TYPE RESTORE TO UNDO WHOLE EBR', 'RESTORE') +
                        step('BIOS REPAIR REQUIREMENTS', 'enter') +
                        step('EBR record restored and verified.', 'esc'))

            if i == 0:
                tour(open_keys() + rebuild + numbers + step('REVIEW WHOLE EBR RECORD', 'esc') +
                     step('BIOS 81h', 'esc', absent='REVIEW WHOLE EBR RECORD'))
                assert digest(disk) == original
            name = f'R{bits}{i}.JRN'
            tour(apply_keys(name) + step('EBR record reconstructed and verified.', 'f10') +
                 undo_keys(name))
            saved = command('mtype', '-i', system_image, f'::/{name}').stdout
            assert len(saved) == 2624+(2-i)*1032 and saved[:8] == b'MDREC001'
            assert saved[576:1088] == bytes(512)
            assert saved[1600:2112] == raw[at*512:(at+1)*512] and digest(disk) == original
            print(f'PASS {args.kernel} FAT{bits} EBR {i+1}: TUI review, typed apply/undo and exact disk preservation', flush=True)
            if i == 1:
                name = f'T{bits}.JRN'
                tour(apply_keys(name) + step('EBR record reconstructed and verified.', 'esc'))
                assert disk.read_bytes() == raw
                torn = bytearray(raw)
                torn[at*512+450] = 0
                disk.write_bytes(torn)
                tour(open_keys('Allocation view ready.') + undo_keys(name, torn=True))
                assert disk.read_bytes() == damaged
                print(f'PASS {args.kernel} FAT{bits}: TUI interrupted whole-EBR undo restores exact original disk', flush=True)
assert digest(args.boot) == boot_hash
