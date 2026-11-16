#!/usr/bin/env python3
"""Reserved-bit cleanup preserves name/label defects for separately reviewed repair."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import Layout, command
from attribute_followup_fixture import fixture
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-attribute-followups-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        for kind in ['conflicts', 'labels']:
            path = root / 'SOURCE.IMG'
            broken, attributes_fixed, expected, entries = fixture(path, bits, kind)
            if len(sys.argv) > 2:
                subprocess.run([str(Path(sys.argv[2]).resolve()), str(path),
                                str(root / 'FAULT.MDJ')], check=True)
            followup = 'FOLLOWUP_' + kind.upper()
            code = 'DIR_DUPLICATE' if kind == 'conflicts' else 'DIR_LABEL'
            operation = '--repair-name-conflicts' if kind == 'conflicts' else '--repair-labels'
            preview = run(exe, path, '--repair-attributes')
            assert followup in preview, preview
            assert path.read_bytes() == broken
            first = root / f'{kind}{bits}-attr.mdj'
            second = root / f'{kind}{bits}-next.mdj'
            run(exe, path, '--repair-attributes', '--apply', '--journal', first)
            assert path.read_bytes() == attributes_fixed
            pending = run(exe, path, code=1)
            assert code in pending and 'DIR_ATTRIBUTE' not in pending, pending
            run(exe, path, operation, '--apply', '--journal', second)
            assert path.read_bytes() == expected
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            run(exe, path, '--restore-journal', first, '--apply', code=2)
            assert path.read_bytes() == expected
            run(exe, path, '--restore-journal', second, '--apply')
            assert path.read_bytes() == attributes_fixed
            run(exe, path, '--restore-journal', first, '--apply')
            assert path.read_bytes() == broken
            if kind == 'labels':
                # Even a label with no reserved bits must not hide a file.
                for offset in entries:
                    for clear in [False, True]:
                        for field, fmt, value in [(11, 'B', 0x18), (26, '<H', 2), (28, '<I', 1)] + (
                                [(20, '<H', 1)] if bits == 32 else []):
                            damaged = bytearray(broken)
                            if clear:
                                damaged[offset + 11] &= 0x3f
                            struct.pack_into(fmt, damaged, offset + field, value)
                            path.write_bytes(damaged)
                            journal = root / 'REFUSED.MDJ'
                            run(exe, path, '--repair-attributes', '--apply', '--journal', journal, code=2)
                            assert not journal.exists() and path.read_bytes() == damaged
            print(f'PASS FAT{bits} {kind}: exact attribute/follow-up stages, explicit remaining findings, reverse-only undo and ownership guards', flush=True)
