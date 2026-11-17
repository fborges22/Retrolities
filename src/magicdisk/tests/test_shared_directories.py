#!/usr/bin/env python3
"""Detach a directory's shared tail while retaining its unique files and children."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import create, Layout, command, PARENT, CHILD, UNICODE_FILE
from chain_fixture import set_fat
from source_fixture import rebuilt_info

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-shared-directories-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'fat{bits}.img'
        create(path, bits)
        command('mmd', '-i', str(path), '::/Later notes')
        command('mmd', '-i', str(path), '::/Later notes/Keep subfolder')
        payload = root / 'payload.bin'
        payload.write_bytes(b'Keep the later directory and its unique contents.\r\n')
        names = ['::/Later notes/Unique long report.txt', '::/Later notes/Keep subfolder/Keep.txt']
        for name in names:
            command('mcopy', '-i', str(path), str(payload), name)
        clean = path.read_bytes()
        layout = Layout(clean, bits)
        owner_entry = layout.child()
        owner = layout.cluster(owner_entry)
        _, later_entry = next((parts, entry) for parts, entry in layout.long_names(layout.root)
                              if layout.decoded_name(parts) == 'Later notes')
        later = layout.cluster(later_entry)
        last = (layout.slots(later)[-1] // 512 - layout.data_sector) // layout.spc + 2
        prefix = (layout.data_sector + (later - 2) * layout.spc) * 512
        for metadata in [False, True]:
            crossed = bytearray(clean)
            set_fat(crossed, bits, last, owner)
            if metadata:
                struct.pack_into('<I', crossed, later_entry + 28, 123456)
                struct.pack_into('<H', crossed, prefix + 26, 0)
            for source in ([0, 1, 2] if metadata else [None, 0, 1, 2]):
                original, expected = bytearray(crossed), bytearray(clean)
                options = ['--repair-directories'] if source is None else ['--repair-tree']
                if source:
                    other = 2 - source
                    start = (layout.reserved + other * layout.fatsz) * 512
                    original[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                    expected = rebuilt_info(expected, bits)
                    options += ['--fat-source', str(source)]
                path.write_bytes(original)
                preview = run(path, *options)
                assert 'detach shared tail; first owner retains it' in preview
                assert path.read_bytes() == original
                journal = root / f'{bits}-{metadata}-{source}.mdj'
                run(path, *options, '--apply', '--journal', str(journal))
                assert path.read_bytes() == expected
                run(path)
                for name in names:
                    assert command('mtype', '-i', str(path), name).stdout == payload.read_bytes()
                assert command('mtype', '-i', str(path), f'::/{PARENT}/{CHILD}/{UNICODE_FILE}').stdout == b'MagicDisk LFN preservation test.\r\n'
                check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
                assert check.returncode == 0, check.stdout + check.stderr
                # Ownership outside the changed FAT/entry sectors still guards undo.
                if not metadata:
                    changed = bytearray(expected)
                    struct.pack_into('<H', changed, owner_entry + 26, 1000)
                    path.write_bytes(changed)
                    run(path, '--restore-journal', str(journal), '--apply', code=2)
                    assert path.read_bytes() == changed
                path.write_bytes(expected)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == original
                raw = journal.read_bytes()
                sector = struct.unpack_from('<I', raw, 1088)[0]
                partial = bytearray(original)
                partial[sector * 512:(sector + 1) * 512] = raw[1604:2116]
                path.write_bytes(partial)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == original
        if bits == 32:
            for active in [0, 1]:
                original, expected = bytearray(clean), bytearray(clean)
                set_fat(original, bits, last, owner)
                for sector in [0, 6]:
                    struct.pack_into('<H', original, sector * 512 + 40, 0x80 | active)
                    struct.pack_into('<H', expected, sector * 512 + 40, 0x80 | active)
                inactive = (layout.reserved + (1 - active) * layout.fatsz) * 512
                expected[inactive:inactive + layout.fatsz * 512] = original[inactive:inactive + layout.fatsz * 512]
                path.write_bytes(original)
                journal = root / f'active{active}.mdj'
                run(path, '--repair-directories', '--apply', '--journal', str(journal))
                assert path.read_bytes() == expected
                run(path)
                run(path, '--restore-journal', str(journal), '--apply')
                assert path.read_bytes() == original
        # Chain-only repair refuses a fully shared reference. Tree repair creates
        # an empty independent directory and retains the old orphan allocation.
        shared = bytearray(clean)
        struct.pack_into('<H', shared, later_entry + 26, owner & 0xffff)
        if bits == 32:
            struct.pack_into('<H', shared, later_entry + 20, owner >> 16)
        path.write_bytes(shared)
        unused = root / f'no-prefix-{bits}.mdj'
        run(path, '--repair-directories', '--apply', '--journal', str(unused), code=2)
        assert path.read_bytes() == shared and not unused.exists()
        journal = root / f'empty-alias-{bits}.mdj'
        preview = run(path, '--repair-tree', '--apply', '--journal', str(journal))
        assert 'EMPTY directory' in preview
        assert 'LOST_CLUSTERS' in run(path, code=1)
        run(path, '--restore-journal', str(journal), '--apply')
        assert path.read_bytes() == shared
print('PASS: FAT12/16/32 shared directory tails, unique contents and children, either FAT authority, ownership guards and exact/partial undo')
