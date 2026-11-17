"""Exact conflict repair, preserving first entries and all file allocation/data."""
from pathlib import Path
import struct
import sys
import tempfile
from directory_fixture import create, command
from duplicate_fixture import checksum, long_name, alias_name, CASE_PAIRS
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-conflict-repair-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        layout = create(path, bits)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        names = list(layout.long_names(child))
        fp, first = names[0]
        sp, second = names[40]
        for case in ['alias', 'long', 'both', 'alias-long', 'long-alias', *range(len(CASE_PAIRS))]:
            data = bytearray(layout.data)
            change_long = case in ['long', 'both', 'alias-long'] or isinstance(case, int)
            if case in ['alias', 'both']:
                data[second:second + 11] = data[first:first + 11]
                for p in sp:
                    data[p + 13] = checksum(data[second:second + 11])
            if case in ['long', 'both', 'alias-long']:
                name = alias_name(data, first) if case == 'alias-long' else layout.decoded_name(fp)
                long_name(data, sp, second, name)
            if case == 'long-alias':
                long_name(data, fp, first, alias_name(data, second))
            if isinstance(case, int):
                lower, upper = CASE_PAIRS[case]
                long_name(data, fp, first, lower)
                long_name(data, sp, second, upper)
            path.write_bytes(data)
            expected = bytearray(data)
            expected[second:second + 11] = b'R0000001CHK'
            active = [p for p in sp if data[p] != 0xe5]
            if change_long:
                long_name(expected, active, second, 'R0000001.CHK')
            else:
                for p in active:
                    expected[p + 13] = checksum(b'R0000001CHK')
            report = run(exe, path, '--repair-name-conflicts')
            assert report.count('RENAME_CONFLICT') == 1, report
            assert report.count('RENAME_LONG_NAME') == int(change_long), report
            if change_long:
                original_name = CASE_PAIRS[case][1] if isinstance(case, int) else name
                assert original_name + ' -> R0000001.CHK' in report, report
            assert path.read_bytes() == data
            journal = root / f'{bits}-{case}.mdj'
            run(exe, path, '--repair-name-conflicts', '--apply', '--journal', journal)
            actual = path.read_bytes()
            assert actual == expected, (bits, case, [(i, a, b) for i,(a,b) in enumerate(zip(actual,expected)) if a != b][:10])
            run(exe, path)
            command('fsck.fat', '-n', str(path))
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 20
            for count in range((len(raw)-1088)//1032 + 1):
                partial = bytearray(data)
                for i in range(count):
                    p = 1088 + i * 1032
                    sector = struct.unpack_from('<I', raw, p)[0]
                    partial[sector*512:(sector+1)*512] = raw[p+516:p+1028]
                path.write_bytes(partial)
                run(exe, path, '--restore-journal', journal, '--apply')
                assert path.read_bytes() == data
            run(exe, path, '--restore-journal', journal, '--apply')
            assert path.read_bytes() == data
        print(f'PASS FAT{bits}: duplicate aliases/LFNs, mixed and Unicode collisions, first names preserved, exact changed bytes, independent fsck and partial/repeated undo', flush=True)
