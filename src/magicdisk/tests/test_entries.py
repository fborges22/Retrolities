#!/usr/bin/env python3
"""Directory-entry repair preserves all bytes outside determinable metadata."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, Layout
from entry_fixture import entry_fixture
from chain_fixture import set_fat

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-entries-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'{bits}.img'
        layout = create(path, bits)
        clean = path.read_bytes()
        # Reserved FAT12/16 high cluster words and all timestamps must survive.
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        preserved = bytearray(clean)
        for cluster in [parent, child]:
            prefix = (layout.data_sector + (cluster - 2) * layout.spc) * 512
            for offset in [prefix, prefix + 32]:
                preserved[offset + 13] = 199
                if bits != 32:
                    struct.pack_into('<H', preserved, offset + 20, 0x1234)
        broken, expected = entry_fixture(preserved, bits)
        path.write_bytes(broken)
        preview = run(path, '--repair-directory-entries', contains='Repair entries in 2 directories')
        assert preview.count('ENTRY_REPAIR') == 2
        assert 'Pasta de projetos/Relatórios longos' in preview
        assert path.read_bytes() == broken
        journal = root / f'{bits}.mdj'
        run(path, '--repair-directory-entries', '--apply', '--journal', str(journal), contains='committed')
        assert path.read_bytes() == expected
        run(path, contains='0 issues')
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        raw = journal.read_bytes()
        assert struct.unpack_from('<I', raw, 8)[0] == 10
        assert struct.unpack_from('<I', raw, 36)[0] == 10
        unused = root / 'unused.mdj'
        run(path, '--repair-directory-entries', '--apply', '--journal', str(unused), contains='already up to date')
        assert not unused.exists()
        for restored in [False, True]:
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == broken
        partial = bytearray(broken)
        sector = struct.unpack_from('<I', raw, 1088)[0]
        partial[sector * 512:(sector + 1) * 512] = raw[1604:2116]
        path.write_bytes(partial)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        for reason in ['missing', 'position', 'loop', 'shared', 'mirror', 'size']:
            invalid = bytearray(broken)
            prefix = (layout.data_sector + (parent - 2) * layout.spc) * 512
            if reason == 'missing':
                invalid[prefix] = ord('X')
            elif reason == 'position':
                invalid[prefix:prefix + 11] = b'KEEP    TXT'
            elif reason == 'loop':
                set_fat(invalid, bits, parent, parent)
            elif reason == 'shared':
                struct.pack_into('<H', invalid, layout.child(parent) + 26, parent)
            elif reason == 'mirror':
                invalid[layout.reserved * 512 + 100] ^= 1
            else:
                short = next(o for o in layout.entries(child) if clean[o + 11] == 32)
                struct.pack_into('<I', invalid, short + 28, 999999)
            path.write_bytes(invalid)
            run(path, '--repair-directory-entries', code=2, contains='refuses')
            assert path.read_bytes() == invalid
        path.write_bytes(broken)
        for reason in ['count', 'after', 'data', 'version', 'flag']:
            bad = bytearray(raw)
            if reason == 'count':
                struct.pack_into('<I', bad, 28, 3)
            elif reason == 'after':
                bad[1088 + 516 + 12] ^= 1
            elif reason == 'data':
                struct.pack_into('<I', bad, 1088, layout.data_sector + 1000)
            elif reason == 'version':
                struct.pack_into('<I', bad, 8, 8)
            else:
                struct.pack_into('<I', bad, 32, 1)
            struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
            struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
            forged = root / 'forged.mdj'
            forged.write_bytes(bad)
            run(path, '--restore-journal', str(forged), '--apply', code=2)
            assert path.read_bytes() == broken
        # Ownership outside journaled sectors must still validate on undo.
        changed = bytearray(expected)
        set_fat(changed, bits, parent, parent)
        path.write_bytes(changed)
        run(path, '--restore-journal', str(journal), '--apply', code=2, contains='ownership')
        assert path.read_bytes() == changed

print('PASS: FAT12/16/32 dot pointers, attributes and sizes, LFN/timestamp preservation, exact undo and forged-journal guards')
