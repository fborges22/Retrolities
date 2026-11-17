#!/usr/bin/env python3
"""LFN groups occupying dot slots move intact, without touching payloads."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import command, Layout
from dot_lfn_fixture import occupied_lfn_fixture, NAME, PAYLOAD

exe = Path(sys.argv[1]).resolve()


def run(image, *args, code=0):
    result = subprocess.run([str(exe), str(image), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-dot-lfn-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        image = root / f'fat{bits}.img'
        cases = [(mode, name) for mode in ['first', 'self', 'file']
                 for name in [NAME, 'x' * 100, 'z' * 150, 'b' * 169, 'a' * 182, 'y' * 220, 'u' * 255]]
        for mode, name in cases:
            original, expected, prefix = occupied_lfn_fixture(image, bits, mode, name)
            image.write_bytes(original)
            journal = root / f'{bits}-{mode}-{len(name)}.mdj'
            run(image, '--repair-tree')
            preview = run(image, '--repair-directory-entries')
            assert 'LFN groups intact' in preview
            assert image.read_bytes() == original
            run(image, '--repair-directory-entries', '--apply', '--journal', str(journal))
            assert image.read_bytes() == expected
            run(image)
            assert command('mtype', '-i', str(image), '::/Empty/' + name).stdout == PAYLOAD
            if mode == 'file':
                assert command('mtype', '-i', str(image), '::/Empty/FIRST.TXT').stdout == PAYLOAD
            check = subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            run(image, '--restore-journal', str(journal), '--apply')
            assert image.read_bytes() == original
        for mode in ['first', 'self', 'file']:
            for name in [NAME, 'd' * 100, 'q' * 220, 'v' * 255]:
                original, expected, _ = occupied_lfn_fixture(image, bits, mode, name,
                                                            directory=True)
                image.write_bytes(original)
                journal = root / f'child-{bits}-{mode}-{len(name)}.mdj'
                run(image, '--repair-tree')
                run(image, '--repair-directory-entries', '--apply', '--journal', str(journal))
                assert image.read_bytes() == expected
                run(image)
                assert command('mtype', '-i', str(image),
                               '::/Empty/' + name + '/DATA.TXT').stdout == PAYLOAD
                check = subprocess.run(['fsck.fat', '-n', str(image)],
                                       capture_output=True, text=True)
                assert check.returncode == 0, check.stdout + check.stderr
                run(image, '--restore-journal', str(journal), '--apply')
                assert image.read_bytes() == original
        # Exercise an ordinary short subdirectory entry in slot zero as well.
        original, expected, prefix = occupied_lfn_fixture(image, bits, directory=True)
        canonical_dots = expected[prefix:prefix + 64]
        short = prefix
        while original[short + 11] == 15:
            short += 32
        record = original[short:short + 32]
        for offset in range(prefix, short + 32, 32):
            original[offset] = 0xe5
        original[prefix:prefix + 32] = record
        expected[prefix:prefix + 512] = original[prefix:prefix + 512]
        expected[prefix:prefix + 64] = canonical_dots
        expected[prefix + 64:prefix + 96] = record
        expected[prefix + 92:prefix + 96] = bytes(4)
        image.write_bytes(original)
        journal = root / f'short-child-{bits}.mdj'
        run(image, '--repair-directory-entries', '--apply', '--journal', str(journal))
        assert image.read_bytes() == expected
        run(image)
        run(image, '--restore-journal', str(journal), '--apply')
        assert image.read_bytes() == original
        shared = bytearray(original)
        layout = Layout(shared, bits)
        slot = next(offset for offset in layout.slots() if not shared[offset])
        shared[slot:slot + 32] = record
        shared[slot:slot + 11] = b'SHARED     '
        shared[slot + 28:slot + 32] = bytes(4)
        image.write_bytes(shared)
        unused = root / f'refused{bits}-shared-child.mdj'
        run(image, '--repair-directory-entries', '--apply', '--journal', str(unused), code=2)
        assert image.read_bytes() == shared and not unused.exists()
        original, _, prefix = occupied_lfn_fixture(image, bits)
        for corruption in ['checksum', 'hidden']:
            invalid = bytearray(original)
            if corruption == 'checksum':
                invalid[prefix + 13] ^= 1
            else:
                invalid[prefix + 6 * 32] = ord('X')
            image.write_bytes(invalid)
            unused = root / f'refused{bits}-{corruption}.mdj'
            run(image, '--repair-directory-entries', '--apply', '--journal', str(unused), code=2)
            assert image.read_bytes() == invalid and not unused.exists()
        original, _, prefix = occupied_lfn_fixture(image, bits, name='w' * 220)
        layout = Layout(original, bits)
        parent = (prefix // 512 - layout.data_sector) // layout.spc + 2
        slots = layout.slots(parent)
        for fault in ['hidden', 'checksum']:
            invalid = bytearray(original)
            if fault == 'checksum':
                invalid[slots[16] + 13] ^= 1
            elif fault == 'hidden':
                invalid[slots[18]] = invalid[slots[19]] = 0
                invalid[slots[-1]] = ord('X')
            image.write_bytes(invalid)
            unused = root / f'refused{bits}-cross-{fault}.mdj'
            run(image, '--repair-directory-entries', '--apply', '--journal', str(unused), code=2)
            assert image.read_bytes() == invalid and not unused.exists()
print('PASS: FAT12/16/32 file/subdirectory relocation, child metadata, intact LFNs, payload preservation, exact undo and unsafe-layout refusal')
