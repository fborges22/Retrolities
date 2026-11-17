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
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'MAGICDSK.EXE')
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
            preview = f'MAGICDSK --bios 81 --rebuild-record {at} {target} --volume-start {at+63} --chs 16 63'
            batch = ['@echo off', 'C:', preview + ' > C:\\PRE.TXT',
                     'if errorlevel 1 echo FAIL > A:\\REC.TXT',
                     preview + f' --apply --journal C:\\R{bits}{i}.JRN > C:\\APP.TXT',
                     'if errorlevel 1 echo FAIL > A:\\REC.TXT',
                     'MAGICDSK --bios 81 --list > C:\\LIST.TXT',
                     'if errorlevel 1 echo FAIL > A:\\REC.TXT',
                     f'MAGICDSK --bios 81 --restore-record C:\\R{bits}{i}.JRN --apply > C:\\UNDO.TXT',
                     'if errorlevel 1 echo FAIL > A:\\REC.TXT',
                     'echo DONE >> A:\\REC.TXT', 'EXITTEST.COM']
            assert all(len(line) <= 126 for line in batch)
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, disk, target_cylinders)
            report = command('mtype', '-i', str(boot), '::/REC.TXT').stdout.decode()
            assert 'FAIL' not in report and 'DONE' in report, report
            preview_text = command('mtype', '-i', system_image, '::/PRE.TXT').stdout.decode()
            assert f'{2-i} tail volumes' in preview_text and 'Preview only' in preview_text
            applied = command('mtype', '-i', system_image, '::/APP.TXT').stdout.decode()
            assert 'EBR record reconstructed and verified.' in applied, applied
            listing = command('mtype', '-i', system_image, '::/LIST.TXT').stdout.decode()
            assert listing.count('type ') == 3, listing
            undone = command('mtype', '-i', system_image, '::/UNDO.TXT').stdout.decode()
            assert 'EBR record restored and verified.' in undone, undone
            saved = command('mtype', '-i', system_image, f'::/R{bits}{i}.JRN').stdout
            assert len(saved) == 2624+(2-i)*1032 and saved[:8] == b'MDREC001'
            assert saved[64:576] == raw[:512]
            assert saved[576:1088] == bytes(512) and saved[1600:2112] == raw[at*512:(at+1)*512]
            assert digest(disk) == original
            print(f'PASS {args.kernel} FAT{bits} EBR {i+1}: CLI record preview/apply/list/undo and exact disk preservation', flush=True)
assert digest(args.boot) == boot_hash
