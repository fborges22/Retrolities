#!/usr/bin/env python3
"""Validate the distributable payload and run its executable in DOS/QEMU."""
import argparse
import hashlib
from pathlib import Path
import re
import tempfile
import zipfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import command
from ui_scripts import step

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('package', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
parser.add_argument('--keyplay', type=Path, required=True)
args = parser.parse_args()
source_hash = digest(args.boot)
package_hash = digest(args.package)

with tempfile.TemporaryDirectory(prefix='magicdisk-package-') as directory:
    root = Path(directory)
    with zipfile.ZipFile(args.package) as archive:
        names = archive.namelist()
        assert set(names) == {'MAGICDSK.EXE', 'README.TXT', 'LICENSE.TXT', 'UNICODE.TXT',
                              'DOS32A.TXT', 'WATCOM.TXT', 'SHA256.TXT'}
        assert len(names) == len(set(names)) and archive.testzip() is None
        for info in archive.infolist():
            assert re.fullmatch(r'[A-Z0-9]{1,8}\.[A-Z0-9]{1,3}', info.filename)
            assert info.extract_version <= 20 and not info.flag_bits & 1
            assert info.date_time == (1980, 1, 1, 0, 0, 0)
        payload = {name: archive.read(name) for name in names}
    for line in payload['SHA256.TXT'].decode('ascii').splitlines():
        checksum, name = line.split('  ')
        assert hashlib.sha256(payload[name]).hexdigest() == checksum
    guide = payload['README.TXT'].decode('ascii')
    assert 'Felipe Andrade Allemand Borges' in guide
    assert 'This product uses DOS/32 Advanced DOS Extender technology.' in guide
    assert all(len(line) <= 79 for line in guide.splitlines())
    for name, data in payload.items():
        if name.endswith('.TXT'):
            assert b'\n' not in data.replace(b'\r\n', b'')
        (root / name).write_bytes(data)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, root / 'MAGICDSK.EXE', 'MAGICDSK.EXE')
    system_image = str(system) + '@@32256'
    for name in names:
        if name != 'MAGICDSK.EXE':
            put(system_image, root / name)
    put(system_image, args.keyplay)
    keys = (step('Ready. F10 opens the menu', 'f10') + step('Open image...', 'right', True) +
            step('Filesystem check', 'right') + step('Allocation map', 'right') +
            step('Save report...', 'right') + step('Keyboard / guide', 'down', True) +
            step('About MagicDisk', 'enter', True) +
            step('Developed by Felipe Andrade Allemand Borges', 'esc') +
            step('Ready. F10 opens the menu', 'esc', absent='ABOUT MAGICDISK'))
    put(system_image, text_file(root, 'ABOUT.KEY', keys))
    batch = ['@echo off', 'C:', 'cd \\', 'MAGICDSK --help > A:\\HELP.TXT',
             'if errorlevel 1 echo FAIL >> A:\\HELP.TXT', 'KEYPLAY ABOUT.KEY',
             'if errorlevel 1 echo FAIL >> A:\\HELP.TXT', 'MAGICDSK --tui',
             'type README.TXT > A:\\GUIDE.TXT', 'EXITTEST.COM']
    put(system_image, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
    hard = root / 'UNUSED.IMG'
    hard.write_bytes(bytes(2 * 1008 * 512))
    run_vm(boot, system, cylinders, hard, 2)
    help_text = command('mtype', '-i', str(boot), '::/HELP.TXT').stdout.decode()
    assert 'Usage: MAGICDSK' in help_text and '--retest-bad' in help_text and 'FAIL' not in help_text, help_text
    assert command('mtype', '-i', str(boot), '::/GUIDE.TXT').stdout == payload['README.TXT']
    for name in names:
        assert command('mtype', '-i', system_image, '::/' + name).stdout == payload[name], name
    assert hard.read_bytes() == bytes(2 * 1008 * 512)
    print(f'PASS: {args.kernel}: packaged EXE help/About, developer credit, DOS guide, notices and payload hashes')

assert digest(args.boot) == source_hash and digest(args.package) == package_hash
