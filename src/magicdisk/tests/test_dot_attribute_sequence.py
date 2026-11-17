#!/usr/bin/env python3
"""Attribute cleanup followed by extra-dot cleanup, preserving exact undo states."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import Layout, command
from dot_attribute_fixture import fixture
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-dot-attribute-sequence-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        damaged, attributes_fixed, repaired, removed = fixture(path, bits)
        if len(sys.argv) > 2:
            subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(root / 'FAULT.MDJ')], check=True)
        preview = run(exe, path, '--repair-attributes')
        assert 'FOLLOWUP_DOTS' in preview and '2 root and 4 extra' in preview, preview
        assert path.read_bytes() == damaged
        attr_journal = root / f'attr{bits}.mdj'
        dot_journal = root / f'dots{bits}.mdj'
        run(exe, path, '--repair-attributes', '--apply', '--journal', attr_journal)
        assert path.read_bytes() == attributes_fixed
        report = run(exe, path, code=1)
        assert 'DIR_DOT_ROOT' in report and 'DIR_DOT_POSITION' in report, report
        assert 'DIR_DOT_METADATA' not in report and 'DIR_ATTRIBUTE' not in report, report
        run(exe, path, '--clear-extra-dots', '--apply', '--journal', dot_journal)
        assert path.read_bytes() == repaired
        run(exe, path)
        command('fsck.fat', '-n', str(path))
        # Undo must respect the two separately approved transactions.
        run(exe, path, '--restore-journal', attr_journal, '--apply', code=2)
        assert path.read_bytes() == repaired
        run(exe, path, '--restore-journal', dot_journal, '--apply')
        assert path.read_bytes() == attributes_fixed
        run(exe, path, '--restore-journal', attr_journal, '--apply')
        assert path.read_bytes() == damaged
        for offset in removed:
            for field, fmt, value in [(11, 'B', 0xe0), (26, '<H', 1234), (28, '<I', 1)]:
                unsafe = bytearray(damaged)
                struct.pack_into(fmt, unsafe, offset + field, value)
                path.write_bytes(unsafe)
                journal = root / 'REFUSE.MDJ'
                run(exe, path, '--repair-attributes', '--apply', '--journal', journal, code=2)
                assert not journal.exists() and path.read_bytes() == unsafe
        layout = Layout(damaged, bits)
        parent = layout.cluster(layout.child())
        unsafe = bytearray(damaged)
        unsafe[layout.slots(parent)[0]] = 0xe5
        path.write_bytes(unsafe)
        run(exe, path, '--repair-attributes', '--apply', '--journal', root / 'REFUSE.MDJ', code=2)
        assert not (root / 'REFUSE.MDJ').exists() and path.read_bytes() == unsafe
        print(f'PASS FAT{bits}: attribute/dot repair sequence, explicit remaining dots, exact stages, reverse-only undo and ambiguous metadata refusal', flush=True)
