#!/usr/bin/env python3
"""Deleted LFN inference rejects malformed evidence and crosses sector/cluster boundaries."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create

FIELDS = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
ALIAS = b'DELETED TXT'


def make_group(name):
    raw = name.encode('utf-16-le')
    units = list(struct.unpack('<' + 'H' * (len(raw) // 2), raw))
    if len(units) % 13:
        units += [0] + [0xffff] * (12 - len(units) % 13)
    checksum = 0
    for byte in ALIAS:
        checksum = (((checksum >> 1) | ((checksum & 1) << 7)) + byte) & 255
    result = []
    for first in reversed(range(0, len(units), 13)):
        slot = bytearray(32)
        slot[0], slot[11], slot[13] = 0xe5, 15, checksum
        for field, value in zip(FIELDS, units[first:first + 13]):
            struct.pack_into('<H', slot, field, value)
        result.append(slot)
    short = bytearray(32)
    short[:11], short[11] = ALIAS, 32
    short[0] = 0xe5
    result.append(short)
    return result


with tempfile.TemporaryDirectory(prefix='magicdisk-deleted-names-') as directory:
    root = Path(directory)
    exe = Path(sys.argv[1]).resolve()
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        child = layout.cluster(layout.child(layout.cluster(layout.child())))
        slots = layout.slots(child)
        base = bytearray(layout.data)
        for offset in slots[2:]:
            base[offset:offset + 32] = bytes(32)
        start = layout.spc * 16 - 1
        for i, offset in enumerate(slots[2:start]):
            base[offset:offset + 11] = f'F{i:07d}TXT'.encode()
            base[offset + 11] = 32
        tests = [(name, 'valid', name) for name in
                 ['a' * 13, 'b' * 26, 'a' * 12 + '\U0001f600漢.txt', 'é' * 255]]
        tests += [('Valid deleted name.txt', mode, None) for mode in
                  ['checksum', 'type', 'cluster', 'surrogate', 'padding', 'character',
                   'terminator', 'alias', 'live-separator', 'overflow', 'illegal-first', 'reorder']]
        tests += [('Escaped OEM alias.txt', 'escaped-first', 'Escaped OEM alias.txt')]
        tests += [(name, 'valid', None) for name in ['.', '..', 'x' * 256]]
        for case, (name, mode, expected) in enumerate(tests):
            group = make_group(name)
            if mode == 'checksum':
                group[0][13] ^= 1
            elif mode == 'type':
                group[0][12] = 1
            elif mode == 'cluster':
                group[0][26] = 1
            elif mode == 'surrogate':
                struct.pack_into('<H', group[-2], 1, 0xdc00)
            elif mode == 'padding':
                struct.pack_into('<H', group[0], 30, ord('x'))
            elif mode == 'character':
                struct.pack_into('<H', group[-2], 1, ord('/'))
            elif mode == 'terminator':
                struct.pack_into('<H', group[-2], 1, 0)
            elif mode in ['illegal-first', 'escaped-first']:
                checksum = 0 if mode == 'illegal-first' else 5
                for byte in ALIAS[1:]:
                    checksum = (((checksum >> 1) | ((checksum & 1) << 7)) + byte) & 255
                for slot in group[:-1]:
                    slot[13] = checksum
            elif mode == 'reorder':
                group[0], group[1] = group[1], group[0]
            elif mode == 'alias':
                group[-1][2] = ord('/')
            elif mode == 'live-separator':
                group.insert(-1, bytearray(b'EMPTY   TXT' + bytes([32]) + bytes(20)))
            elif mode == 'overflow':
                group = [group[0][:] for _ in range(21)] + [group[-1]]
            assert start + len(group) < len(slots)
            data = bytearray(base)
            for offset, slot in zip(slots[start:], group):
                data[offset:offset + 32] = slot
            path.write_bytes(data)
            output = root / f'{bits}-{case}'
            output.mkdir()
            result = subprocess.run([str(exe), str(path), '--recover-deleted', str(output)],
                                    capture_output=True, text=True)
            assert result.returncode == 1, result.stdout + result.stderr
            manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
            matches = re.findall(r'NAME_CANDIDATE .*short_first_hex=(\w+) name_hex=(\w+)', manifest)
            names = [(first, bytes.fromhex(raw).decode('utf-8')) for first, raw in matches]
            assert names == ([] if expected is None else [('05' if mode == 'escaped-first' else '44', expected)]), (bits, mode, names)
            assert path.read_bytes() == data
print('PASS: deleted LFN UTF-8, surrogate/255-unit limits, cluster boundaries and malformed-evidence rejection')
