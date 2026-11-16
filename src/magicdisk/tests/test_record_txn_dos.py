"""QEMU BIOS transactions of missing first, middle and terminal EBR records."""
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
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'RECTXN.EXE')
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
            volume_data = bytearray(data)
            struct.pack_into('<HHI', volume_data, 24, 63, 16, start)
            if bits == 32:
                volume_data[6*512:7*512] = volume_data[:512]
            raw[start*512:(start+sectors)*512] = volume_data
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
            def boot_run(mode, journal, expect):
                batch = ['@echo off', 'C:',
                         f'RECTXN BIOS81 {at} {at+63} {target} 0 {mode} C:\\{journal} C:\\OUT.BIN > A:\\REC.TXT',
                         'if errorlevel 1 echo FAIL >> A:\\REC.TXT', 'EXITTEST.COM']
                assert all(len(line) <= 126 for line in batch)
                put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
                run_vm(boot, system, cylinders, disk, target_cylinders)
                report = command('mtype', '-i', str(boot), '::/REC.TXT').stdout.decode()
                assert 'FAIL' not in report and expect in report, report

            boot_run('ROUND', f'R{bits}{i}.JRN', f'tail={2-i}')
            proposed = command('mtype', '-i', system_image, '::/OUT.BIN').stdout
            expected = raw[at*512:(at+1)*512]
            assert proposed == expected and digest(disk) == original
            saved = command('mtype', '-i', system_image, f'::/R{bits}{i}.JRN').stdout
            assert len(saved) == 2624 + (2-i)*1032 and saved[:8] == b'MDREC001'
            assert saved[64:576] == raw[:512]
            assert saved[576:1088] == bytes(512) and saved[1600:2112] == expected
            print(f'PASS {args.kernel} FAT{bits} EBR {i+1}: BIOS record apply/undo, exact journal and whole-disk restoration', flush=True)
            normal = bytearray(raw)
            normal[450] = 5
            for ebr in records[:-1]:
                normal[ebr*512+466] = 5
            normal_damaged = bytearray(normal)
            normal_damaged[at*512:(at+1)*512] = bytes(512)
            disk.write_bytes(normal_damaged)
            if i == 0:
                boot_run('APPLY', f'N{bits}.JRN', f'tail={2-i}')
                assert disk.read_bytes() == normal
                mode = 'UNDO' if bits == 32 and args.kernel == 'msdos622' else 'DENYUNDO'
                boot_run(mode, f'N{bits}.JRN', 'verified' if mode == 'UNDO' else 'mounted DOS volume')
                assert disk.read_bytes() == (normal_damaged if mode == 'UNDO' else normal)
                print(f'PASS {args.kernel} FAT{bits}: first-record reboot {mode}, exact disk state', flush=True)
            elif bits != 32 or args.kernel != 'msdos622':
                boot_run('DENY', f'D{bits}{i}.JRN', 'mounted DOS volume')
                assert disk.read_bytes() == normal_damaged
                print(f'PASS {args.kernel} FAT{bits} EBR {i+1}: existing mounted volume blocks record reconstruction', flush=True)
assert digest(args.boot) == boot_hash
