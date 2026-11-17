#!/usr/bin/env python3
"""Repair independent attribute/name defects in stages, with reverse journal undo."""
from pathlib import Path
import sys
import subprocess
import tempfile
from directory_fixture import create, command
from short_name_fixture import replace_alias
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-attribute-alias-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        layout = create(path, bits)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        parts, entry = list(layout.long_names(child))[10]
        damaged = bytearray(layout.data)
        alias = bytearray(damaged[entry:entry + 11])
        alias[2] = ord('?')
        replace_alias(damaged, parts, entry, alias)
        damaged[entry + 11] |= 0xc0
        path.write_bytes(damaged)
        if len(sys.argv) > 2:
            subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(root / 'FAULT.MDJ')], check=True)
        report = run(exe, path, '--repair-attributes')
        assert 'FOLLOWUP_NAMES' in report and '1 invalid DOS' in report, report
        assert path.read_bytes() == damaged
        attr_journal = root / f'attr{bits}.mdj'
        name_journal = root / f'name{bits}.mdj'
        run(exe, path, '--repair-attributes', '--apply', '--journal', attr_journal)
        attributes_fixed = bytearray(damaged)
        attributes_fixed[entry + 11] &= 0x3f
        assert path.read_bytes() == attributes_fixed
        report = run(exe, path, code=1)
        assert 'DIR_NAME' in report and 'DIR_ATTRIBUTE' not in report
        run(exe, path, '--repair-short-names', '--apply', '--journal', name_journal)
        expected = bytearray(attributes_fixed)
        replace_alias(expected, parts, entry, b'R0000001CHK')
        assert path.read_bytes() == expected
        run(exe, path)
        command('fsck.fat', '-n', str(path))
        run(exe, path, '--restore-journal', name_journal, '--apply')
        assert path.read_bytes() == attributes_fixed
        run(exe, path, '--restore-journal', attr_journal, '--apply')
        assert path.read_bytes() == damaged
        print(f'PASS FAT{bits}: reserved attributes then invalid alias, explicit remaining-name report, exact stages and reverse undo', flush=True)
