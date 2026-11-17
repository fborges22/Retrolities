#!/usr/bin/env python3
"""Inspect invalid DOS aliases even when their VFAT names are valid."""
from pathlib import Path
import subprocess
import re
import sys
import tempfile
from directory_fixture import create
from short_name_fixture import entries, invalid_aliases, valid_aliases

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-short-names-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'FAT{bits}.IMG'
        create(path, bits)
        original = path.read_bytes()
        valid = valid_aliases(original, bits)
        path.write_bytes(valid)
        result = subprocess.run([str(exe), str(path)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert path.read_bytes() == valid
        invalid, expected = invalid_aliases(original, bits)
        path.write_bytes(invalid)
        result = subprocess.run([str(exe), str(path)], capture_output=True, text=True)
        assert result.returncode == 1, result.stdout + result.stderr
        assert result.stdout.count('DIR_NAME') == len(expected), result.stdout
        assert 'LFN_' not in result.stdout and 'DIR_DUPLICATE' not in result.stdout
        for offset, value in expected:
            assert re.search(r'DIR_NAME\s+' + str(offset // 512) + r'\s+[^\n]*' +
                             f'0x{value:02X} at sector byte {offset % 512}; name unchanged',
                             result.stdout), result.stdout
        for action in ['--repair-files', '--repair-tree', '--repair-directory-entries']:
            journal = root / 'UNUSED.MDJ'
            result = subprocess.run([str(exe), str(path), action, '--apply', '--journal', str(journal)],
                                    capture_output=True, text=True)
            assert result.returncode == 2, result.stdout + result.stderr
            assert not journal.exists()
        assert path.read_bytes() == invalid
        # The same checks must run when there is no long name to display.
        without_lfn = bytearray(invalid)
        for parts, _ in entries(original, bits):
            for part in parts:
                without_lfn[part] = 0xe5
        path.write_bytes(without_lfn)
        result = subprocess.run([str(exe), str(path)], capture_output=True, text=True)
        assert result.returncode == 1, result.stdout + result.stderr
        assert result.stdout.count('DIR_NAME') == len(expected), result.stdout
        assert 'LFN_' not in result.stdout
        assert path.read_bytes() == without_lfn
        print(f'PASS: FAT{bits}: invalid alias bytes, intact LFNs, byte locations, repair refusal and OEM preservation')
