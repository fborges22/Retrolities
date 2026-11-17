"""Run record-review fault injection and memory disposal in QEMU."""
import argparse
from pathlib import Path
import tempfile
from bios_fixture import digest, prepare, put, run_vm, text_file
from directory_fixture import command

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('probe', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', required=True, choices=['msdos622', 'freedos', 'win98'])
args = parser.parse_args()
original = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-ebr-faults-') as temporary:
    root = Path(temporary)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.probe, 'RECCHK.EXE')
    target = root / 'UNUSED.IMG'
    target.write_bytes(bytes(1008 * 512))
    batch = '@echo off\nC:\nRECCHK > A:\\FAULT.TXT\nif errorlevel 1 echo FAIL >> A:\\FAULT.TXT\nEXITTEST.COM\n'
    put(str(system) + '@@32256', text_file(root, 'RUN.BAT', batch))
    run_vm(boot, system, cylinders, target, 1)
    report = command('mtype', '-i', str(boot), '::/FAULT.TXT').stdout.decode()
    assert 'FAIL' not in report and report.count('PASS:') == 6, report
    assert target.read_bytes() == bytes(1008 * 512)
    print(f'PASS {args.kernel}: DOS record-review fault suite\n{report}', flush=True)
assert digest(args.boot) == original
