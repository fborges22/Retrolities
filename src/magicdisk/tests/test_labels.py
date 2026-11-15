#!/usr/bin/env python3
"""Detect extra root volume labels without interpreting deleted/end entries."""
from pathlib import Path
import subprocess
import sys
import tempfile
from label_fixture import create_labels

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-labels-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'LABELS.IMG'
        clean, first, second = create_labels(path, bits, duplicate=False)
        for mode in ['single', 'distinct', 'same', 'deleted', 'after-end']:
            data = bytearray(clean)
            if mode != 'single':
                data[second:second + 32] = data[first:first + 32]
                if mode != 'same':
                    data[second:second + 11] = b'SECOND     '
                if mode == 'deleted':
                    data[second] = 0xe5
                if mode == 'after-end':
                    data[first + 32] = 0
            path.write_bytes(data)
            invalid = mode in ['distinct', 'same']
            result = subprocess.run([str(exe), str(path)], capture_output=True, text=True)
            assert result.returncode == int(invalid), result.stdout + result.stderr
            assert result.stdout.count('DIR_LABEL') == int(invalid), result.stdout
            if invalid:
                assert f'first label at disk LBA {first // 512} byte 0' in result.stdout
                for action in ['--repair-files', '--repair-tree', '--repair-directory-entries']:
                    journal = root / 'UNUSED.MDJ'
                    result = subprocess.run([str(exe), str(path), action, '--apply', '--journal', str(journal)],
                                            capture_output=True, text=True)
                    assert result.returncode == 2, result.stdout + result.stderr
                    assert not journal.exists()
            assert path.read_bytes() == data
        print(f'PASS: FAT{bits}: identical/distinct duplicate labels, sector boundaries, deleted/end markers, repair refusal')
