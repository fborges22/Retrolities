#!/usr/bin/env python3
"""Control bytes in labels must be reported without changing nonownership text."""
from pathlib import Path
import sys
import tempfile
from label_fixture import create_labels
from test_alias_repair import run

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-label-controls-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        clean, first, second = create_labels(path, bits, duplicate=False)
        for position in [0, 1, 10]:
            for value in range(32):
                if position == 0 and value == 0:
                    continue  # An end marker is not a live label.
                data = bytearray(clean)
                data[first + position] = value
                path.write_bytes(data)
                escaped = position == 0 and value == 5
                report = run(exe, path, code=0 if escaped else 1)
                if not escaped:
                    assert report.count('DIR_LABEL_NAME') == 1, report
                    assert f'0x{value:02X}' in report, report
                    assert f'sector byte {first % 512 + position}' in report, report
                assert path.read_bytes() == data
        for value in [32, 65, 127, 128, 229, 255]:
            data = bytearray(clean)
            data[first + 2] = value
            path.write_bytes(data)
            run(exe, path)
            assert path.read_bytes() == data
        # Label-placement cleanup must retain the first label's text and keep
        # its original journal undo usable even with this additional diagnosis.
        data, first, second = create_labels(path, bits)
        data[first + 2] = 9
        path.write_bytes(data)
        journal = root / f'{bits}.mdj'
        run(exe, path, '--repair-labels', '--apply', '--journal', journal)
        expected = bytearray(data)
        expected[second] = 0xe5
        assert path.read_bytes() == expected
        report = run(exe, path, code=1)
        assert report.count('DIR_LABEL_NAME') == 1, report
        run(exe, path, '--restore-journal', journal, '--apply')
        assert path.read_bytes() == data
        print(f'PASS FAT{bits}: control-byte matrix, exact offsets, escaped E5, printable/OEM preservation and independent label-repair undo', flush=True)
