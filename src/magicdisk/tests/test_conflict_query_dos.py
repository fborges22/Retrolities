"""Run the read-only namespace query in real DOS with Unicode/mixed collisions."""
import argparse
from pathlib import Path
import tempfile
from bios_fixture import prepare, put, text_file, run_vm, digest
from directory_fixture import create, command
from duplicate_fixture import checksum, long_name, alias_name

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('boot', type=Path)
parser.add_argument('--kernel', required=True)
args = parser.parse_args()
original_boot = digest(args.boot)
with tempfile.TemporaryDirectory(prefix='magicdisk-conflict-query-dos-') as directory:
    root = Path(directory)
    boot, system, cylinders = prepare(root, args.boot, args.kernel, args.executable, 'QUERY.EXE')
    target = str(system) + '@@32256'
    source = root / 'SOURCE.IMG'
    layout = create(source, 12)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    names = list(layout.long_names(child))
    fp, first = names[0]
    sp, second = names[40]
    batch = ['@echo off', 'C:', 'cd \\', 'echo START > A:\\RESULT.TXT']
    originals = {}
    for case in range(4):
        data = bytearray(layout.data)
        if case == 0:
            data[second:second + 11] = data[first:first + 11]
            for part in sp:
                data[part + 13] = checksum(data[second:second + 11])
            flags = 1
        elif case == 1:
            long_name(data, fp, first, 'Résumé.txt')
            long_name(data, sp, second, 'RÉSUMÉ.TXT')
            flags = 2
        elif case == 2:
            long_name(data, sp, second, alias_name(data, first))
            flags = 2
        else:
            long_name(data, fp, first, alias_name(data, second))
            flags = 1
        image = root / f'CASE{case}.IMG'
        image.write_bytes(data)
        put(target, image)
        originals[image.name] = bytes(data)
        for index, (parts, offset, expected) in enumerate([(fp, first, 0), (sp, second, flags)]):
            blob = root / f'L{case}{index}.BIN'
            blob.write_bytes(b''.join(data[p:p + 32] for p in parts if data[p] != 0xe5))
            put(target, blob)
            batch += [f'QUERY {image.name} {child} {offset} {expected} {blob.name} >> A:\\RESULT.TXT',
                      'if errorlevel 1 echo FAIL >> A:\\RESULT.TXT']
    batch += ['echo DONE >> A:\\RESULT.TXT', 'EXITTEST.COM']
    put(target, text_file(root, 'RUN.BAT', '\n'.join(batch) + '\n'))
    # A second disposable disk is required by the shared QEMU harness.
    hard = root / 'EMPTY.IMG'
    hard.write_bytes(bytes(1008 * 512))
    run_vm(boot, system, cylinders, hard, 1)
    report = command('mtype', '-i', str(boot), '::/RESULT.TXT').stdout
    assert b'DONE' in report and b'FAIL' not in report, report
    for name, data in originals.items():
        assert command('mtype', '-i', target, '::/' + name).stdout == data
assert digest(args.boot) == original_boot
print(f'PASS {args.kernel}: DOS namespace classification, earliest name, Unicode and mixed alias/LFN conflicts, cancellation, source unchanged')
