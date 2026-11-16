"""Separate alias/LFN collision flags, retaining the earliest entry."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create
from duplicate_fixture import checksum, long_name, alias_name, CASE_PAIRS

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-conflicts-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        layout = create(path, bits)
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        names = list(layout.long_names(child))
        first_parts, first = names[0]
        second_parts, second = names[40]
        cases = ['clean', 'alias', 'long', 'both', 'alias-long', 'long-alias', *range(len(CASE_PAIRS))]
        for case in cases:
            data = bytearray(layout.data)
            expected = 0
            if case in ['alias', 'both']:
                data[second:second + 11] = data[first:first + 11]
                for p in second_parts:
                    data[p + 13] = checksum(data[second:second + 11])
                expected |= 1
            if case in ['long', 'both', 'alias-long']:
                name = alias_name(data, first) if case == 'alias-long' else layout.decoded_name(first_parts)
                long_name(data, second_parts, second, name)
                expected |= 2
            if case == 'long-alias':
                long_name(data, first_parts, first, alias_name(data, second))
                expected = 1
            if isinstance(case, int):
                lower, upper = CASE_PAIRS[case]
                long_name(data, first_parts, first, lower)
                long_name(data, second_parts, second, upper)
                expected = 2
            path.write_bytes(data)
            for parts, offset, flags in [(first_parts, first, 0), (second_parts, second, expected)]:
                blob = root / 'LFN.BIN'
                blob.write_bytes(b''.join(data[p:p + 32] for p in parts if data[p] != 0xe5))
                subprocess.run([str(exe), str(path), str(child), str(offset), str(flags), str(blob)], check=True)
            assert path.read_bytes() == data
        print(f'PASS FAT{bits}: earliest entry retained, separate alias/LFN and mixed collisions, Unicode case keys, cancellation and source unchanged', flush=True)
