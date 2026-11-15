#!/usr/bin/env python3
"""Rebuild dots while preserving short-file entries occupying their slots."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create
from dot_space_fixture import occupied_dot_fixture as fixture

exe = Path(sys.argv[1]).resolve()


def run(image, *args, code=0):
    result = subprocess.run([str(exe), str(image), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout




with tempfile.TemporaryDirectory(prefix='magicdisk-dot-space-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        image = root / f'fat{bits}.img'
        clean = create(image, bits).data
        for deleted, count in [(False, 1), (True, 1), (False, 2), (True, 2)]:
            damaged, expected, prefix, vacant = fixture(clean, bits, deleted, count)
            image.write_bytes(damaged)
            journal = root / f'{bits}-{deleted}-{count}.mdj'
            run(image, '--repair-tree')
            preview = run(image, '--repair-directory-entries')
            assert f'relocate {count} entries' in preview
            assert image.read_bytes() == damaged
            run(image, '--repair-directory-entries', '--apply', '--journal', str(journal))
            assert image.read_bytes() == expected
            run(image)
            check = subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            run(image, '--restore-journal', str(journal), '--apply')
            assert image.read_bytes() == damaged
            run(image, '--restore-journal', str(journal), '--apply')
            assert image.read_bytes() == damaged
        # Deleted final slots can be reused without advancing an end marker.
        damaged, expected, prefix, vacant = fixture(clean, bits)
        for index in range(vacant, 16):
            offset = prefix + index * 32
            if index < 14:
                damaged[offset:offset + 11] = f'E{index:07d}TXT'.encode()
                damaged[offset + 11] = 32
            else:
                damaged[offset] = 0xe5
        expected[prefix + vacant * 32:prefix + 512] = damaged[prefix + vacant * 32:prefix + 512]
        expected[prefix + 14 * 32:prefix + 512] = damaged[prefix:prefix + 64]
        image.write_bytes(damaged)
        journal = root / f'end-slots{bits}.mdj'
        run(image, '--repair-directory-entries', '--apply', '--journal', str(journal))
        assert image.read_bytes() == expected
        run(image)
        run(image, '--restore-journal', str(journal), '--apply')
        assert image.read_bytes() == damaged
        # A full prefix or hidden record after its end marker cannot be reused.
        for kind in ['hidden', 'lfn']:
            damaged, _, prefix, vacant = fixture(clean, bits)
            if kind == 'hidden':
                damaged[prefix + (vacant + 1) * 32] = ord('X')
            else:
                damaged[prefix + 11] = 15
            image.write_bytes(damaged)
            run(image, '--repair-directory-entries', code=2)
            assert image.read_bytes() == damaged
print('PASS: FAT12/16/32 occupied dot slots, exact short-entry relocation, merged child-size repair, undo and space/LFN guards')
