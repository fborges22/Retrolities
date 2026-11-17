#!/usr/bin/env python3
"""Production image backend on small DOS files and near-4-GiB FAT32 files."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command
from chain_fixture import set_fat


def pattern(sector, changed=False):
    return bytes(((sector >> ((index % 4) * 8)) ^ (index * 29 + 17) ^ (255 if changed else 0)) & 255
                 for index in range(512))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-image-io-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'IMAGEIO.EXE')
    system_image = str(system) + '@@32256'
    hard = root / 'TARGET.IMG'
    large = args.kernel != 'msdos622'
    length = 0xfffffe00 if large else 4096
    sectors = length // 512
    positions = [0, sectors - 1] + ([0x3fffff, 0x400000, 0x400001] if large else [])
    if large:
        target_cylinders = (5 * 1024 * 1024 * 1024 // 512 + 63 + 1007) // 1008 + 1
        with hard.open('wb') as file:
            file.truncate(target_cylinders * 1008 * 512)
        command('mkfs.fat', '-F', '32', '-s', '64', '-g', '16/63', '-h', '63', '--offset=63', str(hard))
        with hard.open('r+b') as file:
            file.seek(63 * 512)
            boot_sector = file.read(512)
            reserved = struct.unpack_from('<H', boot_sector, 14)[0]
            fatsz = struct.unpack_from('<I', boot_sector, 36)[0]
            total = struct.unpack_from('<I', boot_sector, 32)[0]
            assert struct.unpack_from('<I', boot_sector, 44)[0] == 2
            spc = boot_sector[13]
            data_sector = reserved + 2 * fatsz
            clusters = (length + spc * 512 - 1) // (spc * 512)
            fat_chain = bytearray(clusters * 4)
            for index in range(clusters):
                struct.pack_into('<I', fat_chain, index * 4, index + 4 if index + 1 < clusters else 0x0fffffff)
            for copy in range(2):
                file.seek((63 + reserved + copy * fatsz) * 512 + 3 * 4)
                file.write(fat_chain)
            for sector in [1, 7]:
                file.seek((63 + sector) * 512 + 488)
                free = struct.unpack('<I', file.read(4))[0]
                file.seek((63 + sector) * 512 + 488)
                file.write(struct.pack('<II', free - clusters, clusters + 3))
            entry = bytearray(32)
            entry[:11] = b'BIGIMAGEIMG'
            entry[11] = 32
            struct.pack_into('<HI', entry, 26, 3, length)
            file.seek((63 + data_sector) * 512)
            file.write(entry)
            payload_offset = (63 + data_sector + spc) * 512
            for sector in positions:
                file.seek(payload_offset + sector * 512)
                file.write(pattern(sector))
            mbr = bytearray(63 * 512)
            partition(mbr, total, 0x0c)
            file.seek(0)
            file.write(mbr)
        image_path = 'D:\\BIGIMAGE.IMG'
    else:
        target_cylinders = 2
        hard.write_bytes(bytes(target_cylinders * 1008 * 512))
        small = root / 'BIGIMAGE.IMG'
        contents = bytearray(length)
        for sector in positions:
            contents[sector * 512:(sector + 1) * 512] = pattern(sector)
        small.write_bytes(contents)
        put(system_image, small)
        image_path = 'C:\\BIGIMAGE.IMG'
    batch = ['@echo off', 'C:', 'cd \\',
             'dir D:\\ > A:\\DIRECT.TXT',
             f'IMAGEIO {image_path} {sectors} > A:\\RESULT.TXT',
             'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT', 'EXITTEST.COM']
    put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
    run_vm(boot, system, cylinders, hard, target_cylinders)
    report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
    assert 'PASS: image reads' in report and 'FAIL' not in report, report + command('mtype', '-i', str(boot), '::/DIRECT.TXT').stdout.decode()
    if large:
        with hard.open('rb') as file:
            file.seek((63 + data_sector) * 512 + 28)
            assert struct.unpack('<I', file.read(4))[0] == length
            for copy in range(2):
                file.seek((63 + reserved + copy * fatsz) * 512 + 3 * 4)
                assert file.read(len(fat_chain)) == fat_chain
            for sector in positions:
                file.seek(payload_offset + sector * 512)
                assert file.read(512) == pattern(sector, True)
                for neighbor in [sector - 1, sector + 1]:
                    if 0 <= neighbor < sectors and neighbor not in positions:
                        file.seek(payload_offset + neighbor * 512)
                        assert file.read(512) == bytes(512)
    else:
        expected = bytearray(length)
        for sector in positions:
            expected[sector * 512:(sector + 1) * 512] = pattern(sector, True)
        assert command('mtype', '-i', system_image, '::/BIGIMAGE.IMG').stdout == expected
    print(f'PASS: {args.kernel}: image size {length}, exact boundary writes, reopen, rejected extension and intact FAT chains')
    if large:
        # Exercise production journal identity and FAT updates at file offsets
        # above 2 GiB, using a small nested partition rather than a huge scan.
        put(system_image, args.probe.with_name('MAGICDSK.EXE'))
        inner = root / 'INNER.IMG'
        with inner.open('wb') as file:
            file.truncate(1440 * 1024)
        command('mkfs.fat', '-F', '12', str(inner))
        before = inner.read_bytes()
        after = bytearray(before)
        set_fat(after, 12, 1000, 0xff7)
        inner_lba = 0x400000
        inner_mbr = bytearray(512)
        inner_mbr[450] = 1
        struct.pack_into('<II', inner_mbr, 454, inner_lba, len(before) // 512)
        inner_mbr[510:512] = b'\x55\xaa'
        with hard.open('r+b') as file:
            file.seek(payload_offset)
            file.write(inner_mbr)
            file.seek(payload_offset + inner_lba * 512)
            file.write(before)
        for operation in ['apply', 'undo']:
            option = ('--mark-bad 1000 --journal C:\\HIGH.MDJ' if operation == 'apply' else
                      '--restore-journal C:\\HIGH.MDJ')
            batch = ['@echo off', 'C:', 'cd \\',
                     f'MAGICDSK D:\\BIGIMAGE.IMG --partition 1 {option} --apply > A:\\REPAIR.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\REPAIR.TXT', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, hard, target_cylinders)
            report = command('mtype', '-i', str(boot), '::/REPAIR.TXT').stdout.decode()
            assert 'FAIL' not in report and 'verified' in report, report
            with hard.open('rb') as file:
                file.seek(payload_offset)
                assert file.read(512) == inner_mbr
                file.seek(payload_offset + inner_lba * 512)
                assert file.read(len(before)) == (after if operation == 'apply' else before)
            print(f'PASS: {args.kernel}: production {operation} above 2 GiB, exact partition bytes')

assert digest(args.boot) == source_hash
