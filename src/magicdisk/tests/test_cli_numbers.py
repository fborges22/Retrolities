#!/usr/bin/env python3
"""Malformed unsigned options must fail before opening an image or BIOS disk."""
from pathlib import Path
import subprocess
import sys

exe = Path(sys.argv[1]).resolve()
invalid = ['', ' ', '-2', ' -2', '-4294967294', '-4294967295', '+2', '2 ', '4294967296', '0x10', '2x']
options = ['--mark-sector', '--unmark-sector', '--mark-bad', '--unmark-bad',
           '--relocate-cluster', '--salvage-cluster', '--export-cluster', '--boot-source', '--fat-source',
           '--repair-fat', '--partition']
for option in options:
    for value in invalid:
        args = [str(exe), '/no-such-magicdisk-image', option, value]
        if option == '--export-cluster':
            args.append('/no-such-magicdisk-destination')
        result = subprocess.run(args, capture_output=True, text=True)
        assert result.returncode == 2, result.stdout + result.stderr
        assert 'Cannot open' not in result.stdout, (args, result.stdout)
        assert any(word in result.stdout for word in ['Invalid', 'must be']), (args, result.stdout)
for value in ['', ' ', '-2', ' -2', '-4294967294', '-4294967295', '+2', '80 ', '0x', '100000000', '0x100000000', '80x']:
    result = subprocess.run([str(exe), '--bios', value], capture_output=True, text=True)
    assert result.returncode == 2 and 'Invalid BIOS drive number' in result.stdout, result.stdout
print('PASS: all numeric CLI options refuse signs, spaces, malformed values and overflow before source I/O')
