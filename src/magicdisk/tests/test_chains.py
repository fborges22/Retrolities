#!/usr/bin/env python3
"""Repair file chains while preserving every unrelated byte, including VFAT names."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import create, Layout, damaged_names, UNICODE_FILE, PARENT, CHILD, command
from chain_fixture import chain_fixture, set_fat

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0, contains=None):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    if contains:
        assert contains in result.stdout, result.stdout
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-chains-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        layout = create(path, bits)
        clean = path.read_bytes()
        unicode_broken = damaged_names(layout)
        path.write_bytes(unicode_broken)
        preview = run(path, '--repair-files', contains=UNICODE_FILE)
        assert 'x' * 255 in preview
        unicode_journal = root / f'unicode{bits}.mdj'
        run(path, '--repair-files', '--apply', '--journal', str(unicode_journal))
        unicode_expected = bytearray(clean)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        for cluster, name in [(layout.root, 'x' * 255), (child, UNICODE_FILE)]:
            entry = next(short for parts, short in layout.long_names(cluster)
                         if layout.decoded_name(parts) == name)
            struct.pack_into('<I', unicode_expected, entry + 28, layout.spc * 512)
        assert path.read_bytes() == unicode_expected
        run(path, contains='0 issues')
        run(path, '--restore-journal', str(unicode_journal), '--apply')
        assert path.read_bytes() == unicode_broken
        broken, expected, entries = chain_fixture(clean, bits)
        path.write_bytes(broken)
        preview = run(path, '--repair-files', contains='Repair 9 file chains/lengths')
        assert preview.count('FILE_REPAIR') == 9
        assert 'Long report number' in preview and 'size ' in preview
        assert path.read_bytes() == broken
        journal = root / f'{bits}.mdj'
        run(path, '--repair-files', '--apply', '--journal', str(journal), contains='committed')
        assert path.read_bytes() == expected
        run(path, contains='0 issues')
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        raw = journal.read_bytes()
        version, count = struct.unpack_from('<II', raw, 8)
        assert version == 5 and count > 2
        unused = root / f'unused{bits}.mdj'
        run(path, '--repair-files', '--apply', '--journal', str(unused), contains='already up to date')
        assert not unused.exists()
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        partial = bytearray(broken)
        for index in range(count // 2):
            record = 1088 + index * 1032
            sector = struct.unpack_from('<I', raw, record)[0]
            partial[sector * 512:(sector + 1) * 512] = raw[record + 516:record + 1028]
        path.write_bytes(partial)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == broken
        # A checksummed record for a file-data sector must not become a restore write.
        bad = bytearray(raw)
        struct.pack_into('<I', bad, 1088, layout.data_sector + 998 * layout.spc)
        struct.pack_into('<I', bad, 1088 + 1028, zlib.crc32(bad[1088:1088 + 1028]))
        invalid = root / 'invalid.mdj'
        invalid.write_bytes(bad)
        run(path, '--restore-journal', str(invalid), '--apply', code=2)
        assert path.read_bytes() == broken
        # New unrelated changes in a directory sector cannot be undone over silently.
        changed = bytearray(expected)
        changed[entries[0] + 22] ^= 1
        path.write_bytes(changed)
        run(path, '--restore-journal', str(journal), '--apply', code=2, contains='unrelated changes')
        assert path.read_bytes() == changed
        # A later file with its own prefix keeps that prefix before the shared tail.
        prefix = bytearray(clean)
        first, second = (layout.cluster(pos) for pos in entries[:2])
        set_fat(prefix, bits, second, first)
        path.write_bytes(prefix)
        prefix_journal = root / f'prefix{bits}.mdj'
        run(path, '--repair-files', '--apply', '--journal', str(prefix_journal))
        assert path.read_bytes() == clean
        run(path, '--restore-journal', str(prefix_journal), '--apply')
        assert path.read_bytes() == prefix
        # An unusable start becomes an empty file; the old unreferenced chain is retained.
        outside = bytearray(clean)
        struct.pack_into('<H', outside, entries[0] + 26, 1)
        path.write_bytes(outside)
        outside_journal = root / f'outside{bits}.mdj'
        run(path, '--repair-files', '--apply', '--journal', str(outside_journal))
        emptied = bytearray(outside)
        struct.pack_into('<H', emptied, entries[0] + 26, 0)
        struct.pack_into('<I', emptied, entries[0] + 28, 0)
        assert path.read_bytes() == emptied
        findings = run(path, code=1, contains='LOST_CLUSTERS')
        assert 'CHAIN_' not in findings and 'FILE_SIZE' not in findings
        run(path, '--restore-journal', str(outside_journal), '--apply')
        assert path.read_bytes() == outside
        if bits == 32:
            active = bytearray(broken)
            active_expected = bytearray(expected)
            for sector in [0, 6]:
                struct.pack_into('<H', active, sector * 512 + 40, 0x81)
                struct.pack_into('<H', active_expected, sector * 512 + 40, 0x81)
            first, second = layout.reserved * 512, (layout.reserved + layout.fatsz) * 512
            active_expected[first:second] = active[first:second]
            path.write_bytes(active)
            saved = root / 'active.mdj'
            run(path, '--repair-files', '--apply', '--journal', str(saved))
            assert path.read_bytes() == active_expected
            run(path, contains='0 issues')
            run(path, '--restore-journal', str(saved), '--apply')
            assert path.read_bytes() == active
        # The repair refuses damaged directory ownership and mismatched FAT copies.
        invalid_dir = bytearray(broken)
        parent = layout.cluster(layout.child())
        set_fat(invalid_dir, bits, parent, parent)
        path.write_bytes(invalid_dir)
        run(path, '--repair-files', code=2, contains='undamaged directories')
        assert path.read_bytes() == invalid_dir
        mismatched = bytearray(broken)
        mismatched[layout.reserved * 512 + 100] ^= 1
        path.write_bytes(mismatched)
        run(path, '--repair-files', code=2, contains='consistent FAT copies')
        assert path.read_bytes() == mismatched

    # Directory-sector journals can exceed the old pre-root metadata limit.
    path = root / 'many.img'
    create(path, 12)
    files = root / 'many-files'
    files.mkdir()
    inputs = []
    for index in range(250):
        source = files / f'More directory file number {index:03d}.txt'
        source.write_bytes(b'payload')
        inputs.append(str(source))
    command('mcopy', '-i', str(path), *inputs, f'::/{PARENT}/{CHILD}/')
    layout = Layout(path.read_bytes(), 12)
    parent = layout.cluster(layout.child())
    child = layout.cluster(layout.child(parent))
    broken, expected = bytearray(layout.data), bytearray(layout.data)
    changes = 0
    for entry in layout.entries(child):
        if broken[entry + 11] == 32:
            struct.pack_into('<I', broken, entry + 28, layout.spc * 512 + 1)
            struct.pack_into('<I', expected, entry + 28, layout.spc * 512)
            changes += 1
    path.write_bytes(broken)
    journal = root / 'many.mdj'
    run(path, '--repair-files', '--apply', '--journal', str(journal),
        contains=f'Repair {changes} file chains/lengths')
    assert path.read_bytes() == expected
    assert struct.unpack_from('<I', journal.read_bytes(), 12)[0] > layout.root_sector
    run(path, contains='0 issues')
    run(path, '--restore-journal', str(journal), '--apply')
    assert path.read_bytes() == broken

print('PASS: FAT12/16/32 file lengths, loops, cross-links, free/bad/invalid endings, exact repairs and journal undo')
