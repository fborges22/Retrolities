#!/usr/bin/env python3
"""Combined repair follows relocated records while correcting file chains."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from directory_fixture import Layout, command
from dot_lfn_fixture import occupied_lfn_fixture, PAYLOAD
from chain_fixture import set_fat
from source_fixture import rebuilt_info

exe = Path(sys.argv[1]).resolve()


def run(image, *args, code=0):
    result = subprocess.run([str(exe), str(image), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-tree-relocation-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        image = root / f'fat{bits}.img'
        for child in [False, True]:
            name = 'Relatório ' + 'x' * 210
            broken, fixed, prefix = occupied_lfn_fixture(image, bits, 'file', name,
                                                        directory=child)
            layout = Layout(bytes(broken), bits)
            parent = (prefix // 512 - layout.data_sector) // layout.spc + 2
            last = (layout.slots(parent)[-1] // 512 - layout.data_sector) // layout.spc + 2
            set_fat(broken, bits, last, parent)
            # FIRST.TXT also moves. Its oversized length must be corrected at
            # its destination, without changing the newly rebuilt dot entry.
            struct.pack_into('<I', broken, prefix + 28, layout.spc * 512 + 1)
            repaired = Layout(fixed, bits)
            first = next(offset for offset in repaired.entries(parent)
                         if fixed[offset:offset + 11] == b'FIRST   TXT')
            struct.pack_into('<I', fixed, first + 28, layout.spc * 512)
            if not child:
                _, original_entry = next((parts, entry) for parts, entry in layout.long_names(parent)
                                         if layout.decoded_name(parts) == name)
                _, new_entry = next((parts, entry) for parts, entry in repaired.long_names(parent)
                                    if repaired.decoded_name(parts) == name)
                # A bad start pointer has no allocated prefix. The combined
                # repair must clear both size and start at the relocated slot.
                struct.pack_into('<H', broken, original_entry + 26, 1)
                if fixed[original_entry] == 0xe5:
                    struct.pack_into('<H', fixed, original_entry + 26, 1)
                struct.pack_into('<H', fixed, new_entry + 26, 0)
                struct.pack_into('<I', fixed, new_entry + 28, 0)
            for source in [0, 1, 2]:
                damaged, expected = bytearray(broken), bytearray(fixed)
                options = ['--repair-tree']
                if source:
                    other = 2 - source
                    start = (layout.reserved + other * layout.fatsz) * 512
                    damaged[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                    expected = rebuilt_info(expected, bits)
                    options += ['--fat-source', str(source)]
                image.write_bytes(damaged)
                preview = run(image, *options)
                assert 'ENTRY_REPAIR' in preview and 'FILE_REPAIR' in preview
                assert image.read_bytes() == damaged
                journal = root / f'{bits}-{child}-{source}.mdj'
                run(image, *options, '--apply', '--journal', str(journal))
                assert image.read_bytes() == expected
                # A destroyed start leaves orphan allocation; contents remain
                # intact for recovery, so that finding is expected.
                run(image, code=0 if child else 1)
                first_data = command('mtype', '-i', str(image), '::/Empty/FIRST.TXT').stdout
                assert first_data[:len(PAYLOAD)] == PAYLOAD
                assert len(first_data) == layout.spc * 512
                if child:
                    assert command('mtype', '-i', str(image),
                                   '::/Empty/' + name + '/DATA.TXT').stdout == PAYLOAD
                    check = subprocess.run(['fsck.fat', '-n', str(image)],
                                           capture_output=True, text=True)
                    assert check.returncode == 0, check.stdout + check.stderr
                run(image, '--restore-journal', str(journal), '--apply')
                assert image.read_bytes() == damaged
                # Restore after only the first journal sector reached disk.
                raw = journal.read_bytes()
                sector = struct.unpack_from('<I', raw, 1088)[0]
                partial = bytearray(damaged)
                partial[sector * 512:(sector + 1) * 512] = raw[1604:2116]
                image.write_bytes(partial)
                run(image, '--restore-journal', str(journal), '--apply')
                assert image.read_bytes() == damaged
print('PASS: FAT12/16/32 combined relocation, file start/size correction, child metadata, either FAT authority and partial/exact undo')
