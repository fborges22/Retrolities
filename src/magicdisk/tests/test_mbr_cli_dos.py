"""QEMU BIOS-only MBR transactions; exact proposed bytes and unchanged target."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import create, command

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
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
            data[510:512] = bytes(2)
        sectors = len(data) // 512
        target_cylinders = (63 + sectors + 1007) // 1008
        target = root / 'TARGET.IMG'
        raw = bytearray(target_cylinders * 1008 * 512)
        raw[:446] = bytes((i * 17 + 3) % 256 for i in range(446))
        raw[63 * 512:63 * 512 + len(data)] = data
        target.write_bytes(raw)
        original = digest(target)
        source = 6 if bits == 32 else 0
        options = f'--bios 81 --rebuild-mbr 2 --volume-start 63 --chs 16 63'
        if source:
            options += f' --boot-source {source}'
        batch = ['@echo off', 'C:', 'cd \\',
                 f'MAGICDSK {options} > A:\\PLAN.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\PLAN.TXT',
                 f'MAGICDSK {options} --apply --journal C:\\MBR{bits}.JRN >> A:\\PLAN.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\PLAN.TXT',
                 'MAGICDSK --bios 81 --list >> A:\\PLAN.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\PLAN.TXT',
                 f'MAGICDSK --bios 81 --restore-mbr C:\\MBR{bits}.JRN --apply >> A:\\PLAN.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\PLAN.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, target, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/PLAN.TXT').stdout.decode()
        assert 'FAIL' not in report and f'FAT{bits}' in report, report
        assert 'MBR reconstructed and verified' in report and 'MBR restored and verified' in report, report
        saved = command('mtype', '-i', system_image, f'::/MBR{bits}.JRN').stdout
        assert len(saved) == 1600 and saved[:8] == b'MDMBR001'
        proposed = saved[576:1088]
        assert saved[64:576] == raw[:512]
        expected = bytearray(raw[:512])
        entry = bytearray(16)
        entry[4] = {12:1, 16:6, 32:11}[bits]
        for offset, lba in [(1, 63), (5, 63 + sectors - 1)]:
            cylinder, rest = divmod(lba, 1008)
            head, sector = divmod(rest, 63)
            entry[offset:offset + 3] = bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])
        struct.pack_into('<II', entry, 8, 63, sectors)
        expected[462:478] = entry
        expected[510:512] = b'\x55\xaa'
        assert proposed == expected
        assert digest(target) == original
        print(f'PASS {args.kernel} FAT{bits}: CLI BIOS preview, apply, list and undo, source {source}, target unchanged', flush=True)
assert digest(args.boot) == boot_hash
