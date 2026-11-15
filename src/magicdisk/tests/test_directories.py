#!/usr/bin/env python3
"""Real LFN trees plus isolated structural corruptions and repair refusal."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from directory_fixture import CHILD, PARENT, UNICODE_FILE, Layout, create, command

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, encoding='utf-8')
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-directories-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        original = layout.data
        run(path, contains='54 files, 2 directories')
        command('fsck.fat', '-n', str(path))
        parent_entry = layout.child()
        parent = layout.cluster(parent_entry)
        child = layout.cluster(layout.child(parent))
        slots = layout.slots(child)
        groups = list(layout.long_names(child))
        assert len(groups) == 53
        assert any(a[0] // 512 != b // 512 for a, b in groups), 'No cross-sector LFN fixture'
        assert any((a[0] // 512 - layout.data_sector) // layout.spc !=
                   (b // 512 - layout.data_sector) // layout.spc for a, b in groups), 'No cross-cluster LFN fixture'
        first, short = groups[0]
        fields = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
        pad = next(parts[0] + field for parts, _ in groups for field in fields
                   if original[parts[0] + field:parts[0] + field + 2] == b'\xff\xff')
        cases = [
            ('DIR_DOT_MISSING', slots[0], b'\xe5'),
            ('DIR_DOT_METADATA', slots[0] + 26, struct.pack('<H', child + 1)),
            ('DIR_DOT_METADATA', slots[1] + 26, b'\0\0'),
            ('DIR_DOT_METADATA', slots[0] + 11, b'\x20'),
            ('DIR_DOT_ORDER', slots[0], b'..         '),
            ('DIR_SIZE', parent_entry + 28, struct.pack('<I', 10)),
            ('DIR_DOT_ROOT', parent_entry, b'.          '),
            ('DIR_ATTRIBUTE', short + 11, b'\xa0'),
            ('LFN_ORDER', first[0], bytes([original[first[0]] & 31])),
            ('LFN_ORDER', first[1], b'\x02'),
            ('LFN_ORDER', first[0], b'\x55'),
            ('LFN_CHECKSUM', first[0] + 13, bytes([original[first[0] + 13] ^ 1])),
            ('LFN_METADATA', first[0] + 12, b'\x01'),
            ('LFN_METADATA', first[0] + 26, b'\x01'),
            ('LFN_METADATA', first[0] + 11, b'\x8f'),
            ('LFN_NAME', pad, b'Z\0'),
            ('LFN_NAME', first[-1] + 1, b'?\0'),
            ('LFN_NAME', first[-1] + 1, b'\0\0'),
            ('LFN_ORPHAN', short, b'\xe5'),
            ('LFN_ORPHAN', short, b'\0'),
        ]
        for code, offset, replacement in cases:
            changed = bytearray(original)
            changed[offset:offset + len(replacement)] = replacement
            path.write_bytes(changed)
            run(path, code=1, contains=code)
            assert path.read_bytes() == changed, 'Inspection changed an LFN tree'
        # A 256-code-unit name must not fit into the allowed 255-code-unit limit.
        maximum, _ = next((a, b) for a, b in layout.long_names(layout.root) if len(layout.decoded_name(a)) == 255)
        changed = bytearray(original)
        struct.pack_into('<H', changed, maximum[0] + fields[8], ord('x'))
        struct.pack_into('<H', changed, maximum[0] + fields[9], 0)
        path.write_bytes(changed)
        run(path, code=1, contains='LFN_NAME')
        # Orphans at the end of a full directory chain must not be overlooked.
        changed = bytearray(original)
        end_index = next(i for i, slot in enumerate(slots) if original[slot] == 0)
        for slot in slots[end_index:]:
            changed[slot] = 0xe5
        changed[slots[-1]:slots[-1] + 32] = original[first[-1]:first[-1] + 32]
        changed[slots[-1]] = 0x41
        path.write_bytes(changed)
        run(path, code=1, contains='LFN_ORPHAN')
        # File-chain diagnostics must include the actual UTF-8 LFN path.
        unicode_parts, unicode_short = next((a, b) for a, b in groups
                                             if layout.decoded_name(a) == UNICODE_FILE)
        changed = bytearray(original)
        struct.pack_into('<I', changed, unicode_short + 28, layout.spc * 512 + 1)
        path.write_bytes(changed)
        run(path, code=1, contains=f'{PARENT}/{CHILD}/{UNICODE_FILE}:')
        # Supplementary Unicode characters use a UTF-16 surrogate pair on disk.
        struct.pack_into('<HH', changed, unicode_parts[-1] + 1, 0xd83d, 0xde00)
        path.write_bytes(changed)
        run(path, code=1, contains=f'{PARENT}/{CHILD}/😀{UNICODE_FILE[2:]}:')
        struct.pack_into('<H', changed, unicode_parts[-1] + 3, ord('e'))
        path.write_bytes(changed)
        run(path, code=1, contains='LFN_NAME')
        # A malformed LFN falls back to its short alias for allocation diagnostics.
        changed[unicode_parts[0] + 13] ^= 1
        path.write_bytes(changed)
        output = run(path, code=1, contains='LFN_CHECKSUM')
        assert UNICODE_FILE not in output
        # Corrupt names cannot be used to authorize metadata writes.
        free = next(c for c in range(2, 1000) if layout.fat(c) == 0)
        journal = root / f'refused{bits}.mdj'
        run(path, '--mark-bad', str(free), '--apply', '--journal', str(journal), code=2,
            contains='Filesystem check must complete')
        assert not journal.exists() and path.read_bytes() == changed
        if bits == 32:
            run(path, '--repair-fsinfo', '--apply', '--journal', str(journal), code=2,
                contains='no issues outside FAT32 auxiliary metadata')
            assert not journal.exists()
        # Valid trees must survive a marker transaction and its undo exactly.
        path.write_bytes(original)
        run(path, '--mark-bad', str(free), '--apply', '--journal', str(journal))
        run(path, contains='0 issues')
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == original
        if bits == 12:
            changed = bytearray(original)
            changed[first[0] + 13] ^= 1
            mbr = bytearray(63 * 512)
            mbr[510:512] = b'\x55\xaa'
            mbr[450] = 1
            struct.pack_into('<II', mbr, 454, 63, len(changed) // 512)
            partitioned = root / 'partition.img'
            partitioned.write_bytes(mbr + changed)
            output = run(partitioned, '--partition', '1', code=1)
            assert re.search(r'LFN_CHECKSUM\s+' + str(63 + first[0] // 512) + r'\s', output), output

print('PASS: FAT12/16/32 LFN boundaries, Unicode paths, dot entries, corruption and repair preservation')
