"""Candidate collisions, directory aliases and tampered conflict journals."""
from pathlib import Path
import struct
import sys
import tempfile
import zlib
from directory_fixture import create, command, Layout
from duplicate_fixture import checksum, long_name
from short_name_fixture import replace_alias
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-conflict-edges-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        layout = create(path, bits)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        names = list(layout.long_names(child))
        fp, first = names[0]
        sp, second = names[40]
        data = bytearray(layout.data)
        # Candidate generation must consider existing aliases and LFNs, even
        # names appearing after the entry currently being repaired.
        long_name(data, *names[48], 'R0000001.CHK')
        replace_alias(data, *names[49], b'R0000002CHK')
        replace_alias(data, sp, second, data[first:first + 11])
        long_name(data, sp, second, layout.decoded_name(fp))
        expected = bytearray(data)
        expected[second:second + 11] = b'R0000003CHK'
        long_name(expected, sp, second, 'R0000003.CHK')
        path.write_bytes(data)
        journal = root / f'skip{bits}.mdj'
        run(exe, path, '--repair-name-conflicts', '--apply', '--journal', journal)
        assert path.read_bytes() == expected
        command('fsck.fat', '-n', str(path))
        raw = journal.read_bytes()
        forged = bytearray(raw)
        record = 1088
        forged[record + 516 + 12] ^= 1
        struct.pack_into('<I', forged, record + 1028, zlib.crc32(forged[record:record + 1028]))
        bad_journal = root / f'forged{bits}.mdj'
        bad_journal.write_bytes(forged)
        changed = bytearray(expected)
        sector = struct.unpack_from('<I', raw, record)[0]
        changed[sector * 512 + 12] ^= 1
        path.write_bytes(changed)
        run(exe, path, '--restore-journal', bad_journal, '--apply', code=2)
        assert path.read_bytes() == changed
        path.write_bytes(expected)
        run(exe, path, '--restore-journal', journal, '--apply')
        assert path.read_bytes() == data
        # CP850 has a case pair at 9B/9D which is distinct under unknown/437.
        data = bytearray(layout.data)
        replace_alias(data, fp, first, b'\x9bFILE   TXT')
        replace_alias(data, sp, second, b'\x9dFILE   TXT')
        path.write_bytes(data)
        no_op = run(exe, path, '--repair-name-conflicts')
        assert 'RENAME_CONFLICT' not in no_op
        oem_journal = root / f'oem{bits}.mdj'
        run(exe, path, '--code-page', 850, '--repair-name-conflicts', '--apply', '--journal', oem_journal)
        expected = bytearray(data)
        replace_alias(expected, sp, second, b'R0000001CHK')
        assert path.read_bytes() == expected
        oem_raw = oem_journal.read_bytes()
        assert struct.unpack_from('<I', oem_raw, 32)[0] == 850
        for policy in [0, 999, 0xffffffff]:
            forged = bytearray(oem_raw)
            struct.pack_into('<I', forged, 32, policy)
            crc = zlib.crc32(forged[:60])
            struct.pack_into('<I', forged, 60, zlib.crc32(forged[64:1088], crc))
            bad_journal.write_bytes(forged)
            run(exe, path, '--restore-journal', bad_journal, '--apply', code=2)
            assert path.read_bytes() == expected
        run(exe, path, '--restore-journal', oem_journal, '--apply')
        assert path.read_bytes() == data
        # A directory alias changes without touching its dot links or children.
        layout = create(path, bits)
        command('mmd', '-i', str(path), '::/Other directory')
        layout = Layout(path.read_bytes(), bits)
        fp, first = next((p, e) for p, e in layout.long_names(layout.root) if e == layout.child())
        sp, second = next((p, e) for p, e in layout.long_names(layout.root) if layout.decoded_name(p) == 'Other directory')
        data = bytearray(layout.data)
        replace_alias(data, sp, second, data[first:first + 11])
        expected = bytearray(data)
        replace_alias(expected, sp, second, b'R0000001CHK')
        path.write_bytes(data)
        journal = root / f'directory{bits}.mdj'
        run(exe, path, '--repair-name-conflicts', '--apply', '--journal', journal)
        assert path.read_bytes() == expected
        command('fsck.fat', '-n', str(path))
        run(exe, path, '--restore-journal', journal, '--apply')
        assert path.read_bytes() == data
        print(f'PASS FAT{bits}: future alias/LFN collision skips, forged after-images and OEM policy refusal, fresh-policy undo and directory alias preservation', flush=True)
