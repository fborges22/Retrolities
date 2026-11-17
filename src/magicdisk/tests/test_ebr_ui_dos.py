"""QEMU BIOS-only EBR transactions; exact proposed bytes and unchanged target."""
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
with tempfile.TemporaryDirectory(prefix='magicdisk-mbr-plan-dos-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    put(system_image, args.probe.with_name('KEYPLAY.EXE'))
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
            data[510:512] = bytes(2)
        sectors = len(data) // 512
        target_cylinders = (126 + sectors + 1007) // 1008
        target = root / 'TARGET.IMG'
        raw = bytearray(target_cylinders * 1008 * 512)
        raw[:446] = bytes((i * 17 + 3) % 256 for i in range(446))
        raw[126 * 512:126 * 512 + len(data)] = data
        raw[450] = 15
        struct.pack_into('<II', raw, 454, 63, target_cylinders * 1008 - 63)
        raw[510:512] = b'\x55\xaa'
        target.write_bytes(raw)
        original = digest(target)
        source = 6 if bits == 32 else 0
        menu = (step('Open image...', 'down', True) +
                step('BIOS device...', 'down', True) +
                step('DOS volume...', 'down', True) +
                step('Select volume...', 'down', True) +
                step('Search lost volumes...', 'down', True) +
                step('Reconstruct MBR...', 'down', True) +
                step('Undo MBR reconstruction...', 'down', True) +
                step('Undo interrupted MBR...', 'down', True))
        numbers = (typing('EBR: RECORD DISK LBA', '63') + typing('EBR: CANDIDATE START LBA', '126') +
                   typing('EBR: BOOT SOURCE', str(source)) + typing('EBR: BIOS GEOMETRY HEADS', '16') +
                   typing('EBR: BIOS SECTORS PER TRACK', '63'))
        keys = (device_keys + step('SELECT BIOS DEVICE', 'end') +
                step('Other device...', 'enter', True) + typing('BIOS DEVICE: 00=A', '81') +
                step('BIOS 81h', 'f10', absent='BIOS DEVICE:') + menu +
                step('Reconstruct EBR...', 'enter', True) + numbers +
                step('REVIEW EBR RECONSTRUCTION', 'esc') +
                step('BIOS 81h', 'f10', absent='REVIEW EBR RECONSTRUCTION') + menu +
                step('Reconstruct EBR...', 'enter', True) + numbers +
                step('REVIEW EBR RECONSTRUCTION', 'enter') +
                typing('NEW EBR JOURNAL PATH', f'C:\\EBR{bits}.JRN') +
                typing('TYPE EBR TO RECONSTRUCT TABLE', 'EBR') +
                step('BIOS REPAIR REQUIREMENTS', 'enter') +
                step('EBR reconstructed and verified.', 'f10') + menu +
                step('Reconstruct EBR...', 'down', True) +
                step('Undo EBR reconstruction...', 'enter', True) +
                step('REVIEW EBR UNDO', 'enter') +
                typing('EBR JOURNAL TO RESTORE', f'C:\\EBR{bits}.JRN') +
                step('REVIEW EBR JOURNAL TARGET', 'enter') +
                typing('TYPE RESTORE TO UNDO EBR', 'RESTORE') +
                step('BIOS REPAIR REQUIREMENTS', 'enter') +
                step('EBR restored and verified.', 'esc'))
        put(system_image, text_file(root, 'EBR.KEY', keys))
        batch = ['@echo off', 'C:', 'cd \\', 'KEYPLAY EBR.KEY', 'MAGICDSK --tui', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, target, target_cylinders)
        saved = command('mtype', '-i', system_image, f'::/EBR{bits}.JRN').stdout
        assert len(saved) == 2112 and saved[:8] == b'MDEBR001'
        proposed = saved[576:1088]
        assert saved[64:576] == raw[63 * 512:64 * 512]
        expected = bytearray(raw[63 * 512:64 * 512])
        entry = bytearray(16)
        entry[4] = {12:1, 16:6, 32:11}[bits]
        for offset, lba in [(1, 126), (5, 126 + sectors - 1)]:
            cylinder, rest = divmod(lba, 1008)
            head, sector = divmod(rest, 63)
            entry[offset:offset + 3] = bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])
        struct.pack_into('<II', entry, 8, 63, sectors)
        expected[446:462] = entry
        expected[510:512] = b'\x55\xaa'
        assert proposed == expected
        assert digest(target) == original
        print(f'PASS {args.kernel} FAT{bits}: TUI preview cancellation, typed apply and undo, source {source}, target unchanged', flush=True)
assert digest(args.boot) == boot_hash
