#!/usr/bin/env python3
"""Exact invalid-LFN cleanup, valid suffix preservation, journal undo and guards."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from lfn_repair_fixture import fixture
from directory_fixture import Layout, create, command
from chain_fixture import set_fat


def run(exe, path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *map(str, args)], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


if __name__ == '__main__':
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='magicdisk-lfn-cleanup-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            journal = root / f'{bits}.mdj'
            broken, expected, removed = fixture(path, bits)
            if len(sys.argv) > 2:
                result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path), str(journal), 'lfn'],
                                        capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
            preview = run(exe, path, '--clear-invalid-lfn')
            assert preview.count('CLEAR_LFN') == len(removed), preview
            assert path.read_bytes() == broken
            run(exe, path, '--clear-invalid-lfn', '--apply', '--journal', journal)
            assert path.read_bytes() == expected
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            raw = journal.read_bytes()
            version, count = struct.unpack_from('<II', raw, 8)
            assert version == 17 and count >= 3
            patches = [(struct.unpack_from('<I', raw, p)[0], raw[p + 4:p + 516],
                        raw[p + 516:p + 1028]) for p in range(1088, len(raw), 1032)]
            assert len(patches) == count
            for applied in range(count + 1):
                partial = bytearray(broken)
                for sector, before, after in patches[:applied]:
                    partial[sector * 512:(sector + 1) * 512] = after
                path.write_bytes(partial)
                run(exe, path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == broken
            run(exe, path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == broken
            # Journal with a forged after-image and a valid record CRC is refused.
            forged = bytearray(raw)
            record = 1088
            forged[record + 516 + 12] ^= 1
            struct.pack_into('<I', forged, record + 1028, zlib.crc32(forged[record:record + 1028]))
            forged_path = root / f'forged{bits}.mdj'
            forged_path.write_bytes(forged)
            changed = bytearray(expected)
            changed[patches[0][0] * 512 + 12] ^= 1
            path.write_bytes(changed)
            run(exe, path, '--restore-journal', forged_path, '--apply', code=2)
            assert path.read_bytes() == changed
            # Directory loops prevent ownership validation and must never yield a repair.
            damaged = bytearray(broken)
            layout = Layout(broken, bits)
            parent = layout.cluster(layout.child())
            set_fat(damaged, bits, parent, parent)
            path.write_bytes(damaged)
            rejected = root / f'rejected{bits}.mdj'
            run(exe, path, '--clear-invalid-lfn', '--apply', '--journal', rejected, code=2)
            assert not rejected.exists() and path.read_bytes() == damaged
            # More than twenty orphan slots, across sectors, must all be removed.
            # Grow the FAT32 root first; deleted entries leave the directory chain.
            create(path, bits)
            empty = root / 'EMPTY'
            empty.write_bytes(b'')
            command('mcopy', '-i', str(path), str(empty), '::/' + 'y' * 255)
            command('mdel', '-i', str(path), '::/' + 'y' * 255)
            layout = Layout(path.read_bytes(), bits)
            slots = layout.slots()
            start = next(i for i, offset in enumerate(slots) if layout.data[offset] in [0, 0xe5])
            assert len(slots) - start >= 24
            overflow = bytearray(layout.data)
            for offset in slots[start:start + 23]:
                overflow[offset:offset + 32] = bytes(32)
                overflow[offset] = 1
                overflow[offset + 11] = 15
            overflow[slots[start + 23]] = 0
            cleaned = bytearray(overflow)
            for offset in slots[start:start + 23]:
                cleaned[offset] = 0xe5
            path.write_bytes(overflow)
            preview = run(exe, path, '--clear-invalid-lfn')
            assert preview.count('CLEAR_LFN') == 23, preview
            extra_journal = root / f'overflow{bits}.mdj'
            run(exe, path, '--clear-invalid-lfn', '--apply', '--journal', extra_journal)
            assert path.read_bytes() == cleaned
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            run(exe, path, '--restore-journal', extra_journal, '--apply')
            assert path.read_bytes() == overflow
            print(f'PASS FAT{bits}: exact LFN slot cleanup, preserved valid suffix/aliases/data, partial/repeated undo, forged journal and ownership guards')
