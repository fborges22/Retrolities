#!/usr/bin/env python3
"""Extended SBCS policies: full decoding tables, case flags and collisions."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create
from short_name_fixture import entries
from oem_fixture import extended_fixture, EXTENDED_PAGES

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='magicdisk-oem-extended-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        create(path, bits)
        original = path.read_bytes()
        for cp in EXTENDED_PAGES:
            for lfn in [False, True]:
                data, expected = extended_fixture(original, bits, cp, lfn)
                path.write_bytes(data)
                args = [exe, str(path), '--code-page', str(cp)]
                result = subprocess.run(args, capture_output=True, text=True)
                assert result.returncode == 1 and result.stdout.count('DIR_DUPLICATE') == 1, result.stdout
                result = subprocess.run(args + ['--repair-files', '--apply', '--journal', str(root / 'NO.MDJ')],
                                        capture_output=True)
                assert result.returncode == 2 and not (root / 'NO.MDJ').exists()
                output = root / f'{bits}-{cp}-{lfn}'
                output.mkdir()
                result = subprocess.run(args + ['--export-files', str(output)], capture_output=True)
                assert result.returncode == 1, result.stdout
                manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
                for name in expected:
                    assert name.encode().hex().upper() in manifest
                assert path.read_bytes() == data
            # Cover every high byte, including the escaped E5 first byte.
            data = bytearray(original)
            expected = []
            chunks = [bytes(range(start, min(start + 3, 256))) for start in range(128, 256, 3)]
            chunks.append(b'\xe5')
            for index, ((parts, entry), codes) in enumerate(zip(entries(original, bits), chunks)):
                alias = codes + f'{index:03d}'.encode()
                name = codes.decode(f'cp{cp}') + f'{index:03d}.TXT'
                expected.append(name)
                for part in parts:
                    data[part] = 0xe5
                data[entry:entry + 11] = alias.ljust(8, b' ') + b'TXT'
                if data[entry] == 0xe5:
                    data[entry] = 5
            path.write_bytes(data)
            output = root / f'all-{bits}-{cp}'
            output.mkdir()
            result = subprocess.run([exe, str(path), '--code-page', str(cp), '--export-files', str(output)],
                                    capture_output=True)
            assert result.returncode == 0, result.stdout
            manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
            for name in expected:
                assert name.encode().hex().upper() in manifest, name
            assert path.read_bytes() == data
        print(f'PASS FAT{bits}: CP852/860/866 full high-byte tables, case flags, alias/LFN conflicts and repair guards')
