"""QEMU direct BIOS previews of a missing EBR link on FAT12/16/32 disks."""
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
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'LINKTXN.EXE')
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
        raw[446:462] = entry(0x85, 63, 63, total - 63)
        raw[510:512] = b'\x55\xaa'
        for record in [63, target]:
            raw[(record + 63) * 512:(record + 63 + sectors) * 512] = data
            raw[record * 512:record * 512 + 446] = bytes((i * 13 + 7) % 256 for i in range(446))
            raw[record * 512 + 446:record * 512 + 462] = entry({12:1,16:6,32:11}[bits], record + 63, 63, sectors)
            raw[record * 512 + 510:record * 512 + 512] = b'\x55\xaa'
        disk.write_bytes(raw)
        original = digest(disk)
        batch = ['@echo off', 'C:', f'LINKTXN BIOS81 63 {target} 16 63 C:\\L{bits}.BIN > A:\\LINK.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\LINK.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, disk, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/LINK.TXT').stdout.decode()
        assert 'FAIL' not in report and 'recovered=1' in report, report
        proposed = command('mtype', '-i', system_image, f'::/L{bits}.BIN').stdout
        expected = bytearray(raw[63 * 512:64 * 512])
        expected[462:478] = entry(0x85, target, target - 63, total - target)
        assert proposed == expected and digest(disk) == original
        saved = command('mtype', '-i', system_image, f'::/L{target}.JRN').stdout
        assert len(saved) == 2632 and saved[:8] == b'MDLNK001'
        assert saved[576:1088] == raw[63 * 512:64 * 512] and saved[1088:1600] == expected
        print(f'PASS {args.kernel} FAT{bits}: BIOS link apply/undo on unmounted type-85 container; exact whole-disk undo', flush=True)
        if bits != 32 or args.kernel != 'msdos622':
            # A DOS-recognized container mounts the first volume on the next boot.
            raw[450] = 5
            disk.write_bytes(raw)
            mounted_original = digest(disk)
            batch = ['@echo off', 'C:', f'LINKTXN BIOS81 63 {target} 16 63 C:\\DENY.BIN DENY > A:\\DENY.TXT',
                     'if errorlevel 1 echo FAIL >> A:\\DENY.TXT', 'EXITTEST.COM']
            put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
            run_vm(boot, system, cylinders, disk, target_cylinders)
            report = command('mtype', '-i', str(boot), '::/DENY.TXT').stdout.decode()
            assert 'FAIL' not in report and 'mounted DOS volume' in report, report
            assert digest(disk) == mounted_original
            print(f'PASS {args.kernel} FAT{bits}: mounted-container link writes refused', flush=True)
assert digest(args.boot) == boot_hash
