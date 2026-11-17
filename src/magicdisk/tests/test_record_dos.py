"""QEMU BIOS previews of missing first, middle and terminal EBR records."""
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


def chs(lba):
    cylinder, remainder = divmod(lba, 1008)
    head, sector = divmod(remainder, 63)
    return bytes([head, sector + 1 | ((cylinder >> 2) & 192), cylinder & 255])


def entry(kind, absolute, relative, size):
    return bytes([0]) + chs(absolute) + bytes([kind]) + chs(absolute + size - 1) + struct.pack('<II', relative, size)


with tempfile.TemporaryDirectory(prefix='magicdisk-record-dos-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'RECPRV.EXE')
    system_image = str(system) + '@@32256'
    for bits in [12, 16, 32]:
        volume, disk = root / 'VOL.IMG', root / 'TARGET.IMG'
        create(volume, bits)
        data = volume.read_bytes()
        sectors = len(data) // 512
        records = [63 + i * (sectors + 126) for i in range(3)]
        target_cylinders = (records[-1] + 63 + sectors + 1007) // 1008
        total = target_cylinders * 1008
        raw = bytearray(total * 512)
        raw[446:462] = entry(15, 63, 63, total - 63)
        raw[510:512] = b'\x55\xaa'
        for i, at in enumerate(records):
            start = at + 63
            raw[start*512:(start+sectors)*512] = data
            raw[at*512+446:at*512+462] = entry({12: 1, 16: 6, 32: 11}[bits], start, 63, sectors)
            raw[at*512+510:at*512+512] = b'\x55\xaa'
            if i < 2:
                target = records[i+1]
                raw[at*512+462:at*512+478] = entry(15, target, target-63, total-target)
        for i, at in enumerate(records):
            target = records[i+1] if i < 2 else 0
            damaged = bytearray(raw)
            damaged[at*512:(at+1)*512] = bytes(512)
            disk.write_bytes(damaged)
            original = digest(disk)
            batch = ['@echo off', 'C:',
                     f'RECPRV BIOS81 {at} {at+63} {target} 0 16 63 C:\\P{bits}{i}.BIN > A:\\REC.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\REC.TXT', 'EXITTEST.COM']
            assert all(len(line) <= 126 for line in batch)
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, disk, target_cylinders)
            report = command('mtype', '-i', str(boot), '::/REC.TXT').stdout.decode()
            assert 'FAIL' not in report and f'tail={2-i}' in report, report
            proposed = command('mtype', '-i', system_image, f'::/P{bits}{i}.BIN').stdout
            assert proposed == raw[at*512:(at+1)*512] and digest(disk) == original
            print(f'PASS {args.kernel} FAT{bits} EBR {i+1}: exact whole-record BIOS preview; source unchanged', flush=True)
assert digest(args.boot) == boot_hash
