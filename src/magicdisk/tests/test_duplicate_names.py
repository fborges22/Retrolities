#!/usr/bin/env python3
"""Detect name collisions independently of cluster ownership and preserve source bytes."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create
from duplicate_fixture import checksum, alias_name, long_name, CASE_PAIRS

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-duplicates-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        clean = path.read_bytes()
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        names = list(layout.long_names(child))
        first_parts, first = names[0]
        # Far apart entries exercise indexing across multiple directory sectors/clusters.
        second_parts, second = names[40]
        for mode in ['alias', 'long', 'case', 'unicode', 'alias-long']:
            broken = bytearray(clean)
            if mode == 'alias':
                broken[second:second + 11] = broken[first:first + 11]
                for part in second_parts:
                    broken[part + 13] = checksum(broken[second:second + 11])
            elif mode == 'unicode':
                long_name(broken, first_parts, first, 'Relatório.txt')
                long_name(broken, second_parts, second, 'Relatório.txt')
            elif mode == 'alias-long':
                long_name(broken, second_parts, second, alias_name(clean, first))
            else:
                name = layout.decoded_name(first_parts)
                long_name(broken, second_parts, second, name.upper() if mode == 'case' else name)
            path.write_bytes(broken)
            report = run(path, code=1)
            assert 'DIR_DUPLICATE' in report and 'disk LBA' in report
            assert 'LFN_' not in report and 'CHAIN_' not in report
            assert path.read_bytes() == broken
            journal = root / f'{bits}-{mode}.mdj'
            run(path, '--repair-files', '--apply', '--journal', str(journal), code=2)
            assert not journal.exists() and path.read_bytes() == broken
        for index, (lower, upper) in enumerate(CASE_PAIRS):
            broken = bytearray(clean)
            long_name(broken, first_parts, first, lower)
            long_name(broken, second_parts, second, upper)
            path.write_bytes(broken)
            report = run(path, code=1)
            assert 'DIR_DUPLICATE' in report and upper in report, report
            assert 'LFN_' not in report and 'CHAIN_' not in report
            journal = root / f'{bits}-unicode-{index}.mdj'
            run(path, '--repair-files', '--apply', '--journal', str(journal), code=2)
            assert not journal.exists() and path.read_bytes() == broken
        # Simple uppercase does not expand strings or normalize accents.
        for lower, upper in [('straße.txt', 'STRASSE.TXT'), ('é.txt', 'e\u0301.txt')]:
            distinct = bytearray(clean)
            long_name(distinct, first_parts, first, lower)
            long_name(distinct, second_parts, second, upper)
            path.write_bytes(distinct)
            run(path)
            assert path.read_bytes() == distinct
        # An entry's own LFN can equal its alias; equal names in other directories are valid.
        same = bytearray(clean)
        long_name(same, first_parts, first, alias_name(clean, first).lower())
        path.write_bytes(same)
        run(path)
        same = bytearray(clean)
        root_alias = next(short for parts, short in layout.long_names(layout.root)
                          if layout.decoded_name(parts) == 'x' * 255)
        same[first:first + 11] = same[root_alias:root_alias + 11]
        for part in first_parts:
            same[part + 13] = checksum(same[first:first + 11])
        path.write_bytes(same)
        run(path)
        # Exercise the full UTF-8 key capacity and supplementary decoding.
        root_parts, root_short = next((parts, short) for parts, short in layout.long_names(layout.root)
                                     if layout.decoded_name(parts) == 'x' * 255)
        for name in ['ａ' * 255, '𐐨' * 127 + 'a']:
            maximum = bytearray(clean)
            long_name(maximum, root_parts, root_short, name)
            path.write_bytes(maximum)
            run(path)
            assert path.read_bytes() == maximum
        path.write_bytes(same)
        if len(sys.argv) > 2:
            subprocess.run([str(Path(sys.argv[2]).resolve()), str(path)], check=True)
            assert path.read_bytes() == same

print('PASS: FAT12/16/32 duplicate aliases, LFNs, ASCII and Unicode case, Unicode exact matches and alias/LFN collisions; directory scope preserved')
