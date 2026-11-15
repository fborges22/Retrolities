#!/usr/bin/env python3
"""Detect name collisions independently of cluster ownership and preserve source bytes."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create

exe = Path(sys.argv[1]).resolve()


def checksum(alias):
    value = 0
    for byte in alias:
        value = ((value >> 1) | ((value & 1) << 7)) + byte
        value &= 255
    return value


def alias_name(data, entry):
    base = data[entry:entry + 8].decode('ascii').rstrip()
    extension = data[entry + 8:entry + 11].decode('ascii').rstrip()
    return base + ('.' + extension if extension else '')


def long_name(data, parts, short, name):
    fields = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    units = list(struct.unpack('<' + 'H' * (len(name.encode('utf-16-le')) // 2), name.encode('utf-16-le')))
    count = (len(units) + 12) // 13
    assert count <= len(parts)
    if len(units) < count * 13:
        units += [0] + [0xffff] * (count * 13 - len(units) - 1)
    for part in parts[:-count]:
        data[part] = 0xe5
    for index, part in enumerate(parts[-count:]):
        ordinal = count - index
        data[part:part + 32] = bytes(32)
        data[part] = ordinal | (0x40 if index == 0 else 0)
        data[part + 11] = 15
        data[part + 13] = checksum(data[short:short + 11])
        for field, value in zip(fields, units[(ordinal - 1) * 13:ordinal * 13]):
            struct.pack_into('<H', data, part + field, value)


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
        if len(sys.argv) > 2:
            subprocess.run([str(Path(sys.argv[2]).resolve()), str(path)], check=True)
            assert path.read_bytes() == same

print('PASS: FAT12/16/32 duplicate aliases, LFNs, ASCII case, Unicode exact matches and alias/LFN collisions; directory scope preserved')
