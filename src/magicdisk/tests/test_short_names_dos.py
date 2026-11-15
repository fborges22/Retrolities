#!/usr/bin/env python3
"""Check invalid short aliases beneath valid LFNs and repair refusal in DOS/QEMU."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from bios_fixture import digest, partition, prepare, put, run_vm, text_file
from directory_fixture import command, create
from short_name_fixture import invalid_aliases

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)

with tempfile.TemporaryDirectory(prefix='magicdisk-short-names-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    for bits in [12, 16, 32]:
        volume = root / 'VOLUME.IMG'
        create(volume, bits)
        data, expected = invalid_aliases(volume.read_bytes(), bits)
        struct.pack_into('<HHI', data, 24, 63, 16, 63)
        if bits == 32:
            data[6 * 512:7 * 512] = data[:512]
        sectors = len(data) // 512
        mbr = bytearray(63 * 512)
        partition(mbr, sectors, {12: 0x11, 16: 0x16, 32: 0x1c}[bits])
        target_cylinders = (63 + sectors + 1007) // 1008 + 1
        original = mbr + data + bytes(target_cylinders * 1008 * 512 - len(mbr) - len(data))
        hard = root / 'TARGET.IMG'
        hard.write_bytes(original)
        journal = f'U{bits}.MDJ'
        batch = ['@echo off', 'C:', 'cd \\',
                 'MAGICDSK --bios 81 --partition 1 > A:\\SCAN.TXT',
                 'if not errorlevel 1 echo FAIL >> A:\\SCAN.TXT',
                 'if errorlevel 2 echo FAIL >> A:\\SCAN.TXT',
                 f'MAGICDSK --bios 81 --partition 1 --repair-files --apply --journal C:\\{journal} > A:\\REPAIR.TXT',
                 'if not errorlevel 2 echo FAIL >> A:\\REPAIR.TXT', 'EXITTEST.COM']
        put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
        run_vm(boot, system, cylinders, hard, target_cylinders)
        report = command('mtype', '-i', str(boot), '::/SCAN.TXT').stdout.decode('utf-8')
        repair = command('mtype', '-i', str(boot), '::/REPAIR.TXT').stdout.decode('utf-8')
        assert 'FAIL' not in report + repair, report + repair
        assert report.count('DIR_NAME') == len(expected), report
        for offset, value in expected:
            assert f'0x{value:02X} at sector byte {offset % 512}; name unchanged' in report, report
        assert 'LFN_' not in report and 'CHAIN_' not in report, report
        assert hard.read_bytes() == original
        exists = subprocess.run(['mtype', '-i', system_image, '::/' + journal], capture_output=True)
        assert exists.returncode != 0, journal
        print(f'PASS: {args.kernel} FAT{bits}: invalid aliases under valid LFNs, exact byte findings, repair refused; exact source bytes')

assert digest(args.boot) == source_hash
