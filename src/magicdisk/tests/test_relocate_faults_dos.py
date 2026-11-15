#!/usr/bin/env python3
"""Run relocation media-fault checks against synthetic media in DOS/QEMU."""
import argparse
from pathlib import Path
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import command

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', choices=['msdos622', 'freedos', 'win98'], required=True)
args = parser.parse_args()
source_hash = digest(args.boot)

with tempfile.TemporaryDirectory(prefix='magicdisk-relocate-faults-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'RELOCFLT.EXE')
    batch = ['@echo off', 'C:', 'cd \\', 'RELOCFLT C:\\TEST.MDJ > A:\\RESULT.TXT',
             'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT',
             'RELOCFLT C:\\TEST.MDJ --directory >> A:\\RESULT.TXT',
             'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT', 'EXITTEST.COM']
    put(str(system) + '@@32256', text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
    hard = root / 'UNUSED.IMG'
    hard.write_bytes(bytes(2 * 1008 * 512))
    run_vm(boot, system, cylinders, hard, 2)
    report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout.decode()
    assert report.count('PASS: relocation read failures') == 2 and 'FAIL' not in report, report
    assert hard.read_bytes() == bytes(2 * 1008 * 512)
    print(f'PASS: {args.kernel}: file/directory relocation: unreadable destinations, cancellation, rollback and exact undo')

assert digest(args.boot) == source_hash
