#!/usr/bin/env python3
"""Three independent repairs preserve intermediate states and exact journal undo."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import command
from metadata_sequence_fixture import fixture
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-metadata-sequence-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        broken, attrs, labels, clean = fixture(path, bits)
        if len(sys.argv) > 2:
            path.write_bytes(attrs)
            subprocess.run([str(Path(sys.argv[2]).resolve()), str(path),
                            str(root / 'FAULT.MDJ')], check=True)
            path.write_bytes(broken)
        journals = [root / f'{bits}-{index}.mdj' for index in range(3)]
        preview = run(exe, path, '--repair-attributes')
        assert 'FOLLOWUP_LABELS' in preview and 'FOLLOWUP_CONFLICTS' in preview, preview
        run(exe, path, '--repair-attributes', '--apply', '--journal', journals[0])
        assert path.read_bytes() == attrs
        preview = run(exe, path, '--repair-labels')
        assert 'FOLLOWUP_CONFLICTS' in preview, preview
        assert path.read_bytes() == attrs
        run(exe, path, '--repair-labels', '--apply', '--journal', journals[1])
        assert path.read_bytes() == labels
        pending = run(exe, path, code=1)
        assert 'DIR_DUPLICATE' in pending and 'DIR_LABEL' not in pending, pending
        raw = journals[1].read_bytes()
        records = list(range(1088, len(raw), 1032))
        for applied in range(len(records) + 1):
            partial = bytearray(attrs)
            for offset in records[:applied]:
                sector = struct.unpack_from('<I', raw, offset)[0]
                partial[sector * 512:(sector + 1) * 512] = raw[offset + 516:offset + 1028]
            path.write_bytes(partial)
            run(exe, path, '--restore-journal', journals[1], '--apply')
            assert path.read_bytes() == attrs
        path.write_bytes(labels)
        run(exe, path, '--repair-name-conflicts', '--apply', '--journal', journals[2])
        assert path.read_bytes() == clean
        run(exe, path)
        command('fsck.fat', '-n', str(path))
        for journal, expected in zip(reversed(journals), [labels, attrs, broken]):
            run(exe, path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == expected
        print(f'PASS FAT{bits}: attributes/labels/names, exact stages, explicit follow-up, label partial undo and three-journal reverse undo', flush=True)
