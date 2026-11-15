#!/usr/bin/env python3
"""Retain directory prefixes, refuse ambiguous owners, and restore exact metadata."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, Layout, command
from chain_fixture import set_fat
from directory_chain_fixture import directory_chain_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-directory-chains-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        clean = path.read_bytes()
        broken, expected, starts, ends = directory_chain_fixture(clean, bits)
        path.write_bytes(broken)
        preview = run(path, '--repair-directories', contains=f'Repair {len(starts)} directory chains')
        assert preview.count('DIR_REPAIR') == len(starts)
        assert 'Pasta de projetos/Relatórios longos' in preview
        assert 'remove loop' in preview and 'remove invalid ending' in preview
        assert path.read_bytes() == broken
        journal = root / f'{bits}.mdj'
        run(path, '--repair-directories', '--apply', '--journal', str(journal), contains='committed')
        assert path.read_bytes() == expected
        assert expected[layout.root_sector * 512:] == clean[layout.root_sector * 512:]
        run(path, contains='0 issues')
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        raw = journal.read_bytes()
        version, count = struct.unpack_from('<II', raw, 8)
        assert version == 6 and count >= 2
        unused = root / f'unused{bits}.mdj'
        run(path, '--repair-directories', '--apply', '--journal', str(unused), contains='already up to date')
        assert not unused.exists()
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        partial = bytearray(broken)
        sector = struct.unpack_from('<I', raw, 1088)[0]
        partial[sector * 512:(sector + 1) * 512] = raw[1604:2116]
        path.write_bytes(partial)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # An interior loop must be distinguished from a previously visited other owner.
        interior = bytearray(clean)
        next_cluster = layout.fat(starts[1])
        assert next_cluster < {12: 0xff8, 16: 0xfff8, 32: 0x0ffffff8}[bits]
        set_fat(interior, bits, ends[1], next_cluster)
        path.write_bytes(interior)
        saved_loop = root / f'interior{bits}.mdj'
        run(path, '--repair-directories', '--apply', '--journal', str(saved_loop), contains='remove loop')
        assert path.read_bytes() == clean
        run(path, '--restore-journal', str(saved_loop), '--apply')
        assert path.read_bytes() == interior
        # A sibling's unique prefix leading into an earlier directory is a cross-link.
        path.write_bytes(clean)
        command('mmd', '-i', str(path), '::/Another directory')
        sibling_layout = Layout(path.read_bytes(), bits)
        sibling_entry = next(short for parts, short in sibling_layout.long_names(sibling_layout.root)
                             if sibling_layout.decoded_name(parts) == 'Another directory')
        sibling = sibling_layout.cluster(sibling_entry)
        crossed = bytearray(sibling_layout.data)
        set_fat(crossed, bits, sibling, starts[0])
        path.write_bytes(crossed)
        crossing_journal = root / f'crossing{bits}.mdj'
        run(path, '--repair-directories', '--apply', '--journal', str(crossing_journal),
            contains='detach shared tail')
        assert path.read_bytes() == sibling_layout.data
        run(path, contains='0 issues')
        run(path, '--restore-journal', str(crossing_journal), '--apply')
        assert path.read_bytes() == crossed
        # Rechecks directory entry ownership, which lives outside this FAT-only journal.
        changed = bytearray(expected)
        struct.pack_into('<H', changed, layout.child() + 26, 1000)
        path.write_bytes(changed)
        run(path, '--restore-journal', str(journal), '--apply', code=2, contains='ownership')
        assert path.read_bytes() == changed
        for failure in ['cross-link', 'missing-prefix', 'dot-entry', 'file-damage', 'mirrors']:
            invalid = bytearray(broken)
            if failure == 'cross-link':
                set_fat(invalid, bits, ends[0], starts[1])
            elif failure == 'missing-prefix':
                set_fat(invalid, bits, starts[0], 0)
            elif failure == 'dot-entry':
                pos = (layout.data_sector + (starts[0] - 2) * layout.spc) * 512
                invalid[pos] = ord('X')
            elif failure == 'file-damage':
                entry = next(p for p in layout.entries(starts[1]) if clean[p + 11] == 32)
                struct.pack_into('<I', invalid, entry + 28, layout.spc * 512 + 1)
            else:
                invalid[layout.reserved * 512 + 100] ^= 1
            path.write_bytes(invalid)
            run(path, '--repair-directories', code=2, contains='refuses')
            assert path.read_bytes() == invalid
        path.write_bytes(broken)
        for failure in ['count', 'sector', 'after', 'version']:
            bad = bytearray(raw)
            if failure == 'count':
                struct.pack_into('<I', bad, 28, len(starts) + 1)
            elif failure == 'sector':
                struct.pack_into('<I', bad, 1088, layout.root_sector)
            elif failure == 'after':
                bad[1088 + 516] ^= 1
            else:
                struct.pack_into('<I', bad, 8, 5)
            struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
            struct.pack_into('<I', bad, 60, zlib.crc32(bad[64:1088], zlib.crc32(bad[:60])))
            invalid = root / 'invalid.mdj'
            invalid.write_bytes(bad)
            run(path, '--restore-journal', str(invalid), '--apply', code=2)
            assert path.read_bytes() == broken
        if bits == 32:
            active, repaired = bytearray(broken), bytearray(expected)
            for sector in [0, 6]:
                struct.pack_into('<H', active, sector * 512 + 40, 0x81)
                struct.pack_into('<H', repaired, sector * 512 + 40, 0x81)
            first, second = layout.reserved * 512, (layout.reserved + layout.fatsz) * 512
            repaired[first:second] = active[first:second]
            path.write_bytes(active)
            saved = root / 'active.mdj'
            run(path, '--repair-directories', '--apply', '--journal', str(saved))
            assert path.read_bytes() == repaired
            run(path, contains='0 issues')
            run(path, '--restore-journal', str(saved), '--apply')
            assert path.read_bytes() == active

print('PASS: FAT12/16/32 directory loops/endings, Unicode names, FAT32 root/active FAT, exact undo and ownership guards')
