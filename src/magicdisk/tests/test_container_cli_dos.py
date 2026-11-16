"""QEMU direct BIOS previews of a missing extended container on FAT12/16/32 disks."""
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

with tempfile.TemporaryDirectory(prefix='magicdisk-link-dos-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
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
        preview = f'MAGICDSK --bios 81 --rebuild-container 1 63 {total-63} 0F --chs 16 63'
        batch = ['@echo off', 'C:', preview + ' > C:\\PRE.TXT',
                 'if errorlevel 1 echo FAIL > A:\\LINK.TXT',
                 preview + f' --apply --journal C:\\C{bits}.JRN > C:\\APP.TXT',
                 'if errorlevel 1 echo FAIL > A:\\LINK.TXT',
                 'MAGICDSK --bios 81 --list > C:\\LIST.TXT',
                 'if errorlevel 1 echo FAIL > A:\\LINK.TXT',
                 f'MAGICDSK --bios 81 --restore-container C:\\C{bits}.JRN --apply > C:\\UNDO.TXT',
                 'if errorlevel 1 echo FAIL > A:\\LINK.TXT',
                 'echo DONE >> A:\\LINK.TXT', 'EXITTEST.COM']
        assert all(len(line) <= 126 for line in batch)
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, disk, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/LINK.TXT').stdout.decode()
        assert 'FAIL' not in report and 'DONE' in report, report
        preview_text = command('mtype', '-i', system_image, '::/PRE.TXT').stdout.decode()
        assert '2 recovered volumes' in preview_text and 'Preview only' in preview_text
        applied = command('mtype', '-i', system_image, '::/APP.TXT').stdout.decode()
        assert 'Container inserted and verified.' in applied, applied
        listing = command('mtype', '-i', system_image, '::/LIST.TXT').stdout.decode()
        assert listing.count('type ') == 2, listing
        undone = command('mtype', '-i', system_image, '::/UNDO.TXT').stdout.decode()
        assert 'Container restored and verified.' in undone, undone
        expected = bytearray(raw[:512])
        expected[446:462] = entry(15, 63, 63, total - 63)
        saved = command('mtype', '-i', system_image, f'::/C{bits}.JRN').stdout
        assert len(saved) == 3152 and saved[:8] == b'MDCTR001'
        assert saved[64:576] == raw[:512] and saved[576:1088] == expected
        assert digest(disk) == original
        print(f'PASS {args.kernel} FAT{bits}: CLI container preview/apply/list/undo and exact disk preservation', flush=True)
assert digest(args.boot) == boot_hash
