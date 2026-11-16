"""Reboot after reconstruction: refuse mounted disks; undo an unmounted FAT32 on MS-DOS 6.22."""
import argparse
from pathlib import Path
import struct
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import create, command

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', required=True, choices=['msdos622', 'freedos', 'win98'])
args = parser.parse_args()
boot_hash = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-mbr-reboot-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    for bits in [16, 32]:
        volume, target = root / 'VOLUME.IMG', root / 'TARGET.IMG'
        create(volume, bits)
        data = bytearray(volume.read_bytes())
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        target_cylinders = (63 + len(data) // 512 + 1007) // 1008
        raw = bytearray(target_cylinders * 1008 * 512)
        raw[63 * 512:63 * 512 + len(data)] = data
        target.write_bytes(raw)
        original = digest(target)
        apply = f'MAGICDSK --bios 81 --rebuild-mbr 2 --volume-start 63 --chs 16 63 --apply --journal C:\\M{bits}.JRN'
        batch = ['@echo off', 'C:', apply + ' > A:\\APPLY.TXT',
                 'if errorlevel 1 echo FAIL >> A:\\APPLY.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, target, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/APPLY.TXT').stdout.decode()
        assert 'FAIL' not in report and 'reconstructed and verified' in report, report
        applied = digest(target)
        saved = command('mtype', '-i', system_image, f'::/M{bits}.JRN').stdout
        unmounted = bits == 32 and args.kernel == 'msdos622'
        batch = ['@echo off', 'C:', 'MAGICDSK --dos-map > A:\\MAP.TXT',
                 f'MAGICDSK --bios 81 --restore-mbr C:\\M{bits}.JRN --apply > A:\\UNDO.TXT',
                 ('if errorlevel 1 echo FAIL >> A:\\UNDO.TXT' if unmounted else
                  'if not errorlevel 2 echo FAIL >> A:\\UNDO.TXT'), 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, target, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/UNDO.TXT').stdout.decode()
        inventory = command('mtype', '-i', str(boot), '::/MAP.TXT').stdout.decode()
        assert 'FAIL' not in report, report + inventory
        if unmounted:
            assert 'MBR restored and verified' in report, report + inventory
            assert digest(target) == original
        else:
            assert 'mounted DOS volume' in report, report + inventory
            assert digest(target) == applied
        assert command('mtype', '-i', system_image, f'::/M{bits}.JRN').stdout == saved
        print(f'PASS {args.kernel} FAT{bits}: reboot ' +
              ('undo exactly restores unmounted disk' if unmounted else 'mounted-disk undo refused without writes'), flush=True)
assert digest(args.boot) == boot_hash
