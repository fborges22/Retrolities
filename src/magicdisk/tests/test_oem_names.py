#!/usr/bin/env python3
"""Explicit OEM selection decodes aliases without guessing a volume's locale."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create
from oem_fixture import fixture, lowercase_alias

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-oem-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / f'FAT{bits}.IMG'
        create(path, bits)
        original = path.read_bytes()
        for mode in ['alias-alias', 'alias-lfn', 'distinct-pages', 'punctuation', 'display', 'escaped-lead']:
            data, entry = fixture(original, bits, mode)
            path.write_bytes(data)
            for cp in [0, 437, 850]:
                args = [str(exe), str(path), '--code-page', str(cp)]
                result = subprocess.run(args, capture_output=True, text=True)
                duplicate = cp != 0 and (mode in ['alias-alias', 'alias-lfn'] or
                                        (mode == 'distinct-pages' and cp == 850))
                invalid = cp != 0 and mode == 'punctuation'
                assert result.returncode == int(duplicate or invalid), result.stdout + result.stderr
                assert result.stdout.count('DIR_DUPLICATE') == int(duplicate), result.stdout
                assert result.stdout.count('DIR_NAME') == int(invalid), result.stdout
                if duplicate or invalid:
                    journal = root / 'REJECT.MDJ'
                    result = subprocess.run(args + ['--repair-files', '--apply', '--journal', str(journal)],
                                            capture_output=True, text=True)
                    assert result.returncode == 2 and not journal.exists(), result.stdout
                if mode in ['display', 'escaped-lead']:
                    output = root / f'export-{bits}-{mode}-{cp}'
                    output.mkdir()
                    result = subprocess.run(args + ['--export-files', str(output)], capture_output=True, text=True)
                    assert result.returncode == 0, result.stdout + result.stderr
                    manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
                    lead = 0x9b if mode == 'display' else 0xe5
                    name = (bytes([lead]).decode(f'cp{cp}') if cp else f'\\x{lead:02X}') + 'FILE.TXT'
                    assert name.encode().hex().upper() in manifest, manifest
                    assert f'OEM code page: {cp} (0 means unknown)' in manifest
                assert path.read_bytes() == data
        for flags in [0, 8, 16, 24]:
            data = bytearray(original)
            lowercase_alias(data, bits, flags)
            path.write_bytes(data)
            for cp in [0, 437, 850]:
                output = root / f'lower-{bits}-{flags}-{cp}'
                output.mkdir()
                result = subprocess.run([str(exe), str(path), '--code-page', str(cp),
                                         '--export-files', str(output)], capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
                manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
                base = ('é' if flags & 8 else 'É') if cp else r'\x90'
                extension = ('ö' if flags & 16 else 'Ö') if cp else r'\x99'
                name = base + ('lower' if flags & 8 else 'LOWER') + '.' + extension + ('xt' if flags & 16 else 'XT')
                assert name.encode().hex().upper() in manifest, manifest
                assert path.read_bytes() == data
        # Prefix syntax also configures TUI sessions and rejects unsupported policies.
        for cp in ['-1', '65536', '999', 'utf8', ' 850', '850x']:
            result = subprocess.run([str(exe), '--code-page', cp, str(path)], capture_output=True)
            assert result.returncode == 2
        result = subprocess.run([str(exe), '--code-page', '850', str(path)], capture_output=True)
        assert result.returncode == 0, result.stdout
        print(f'PASS: FAT{bits}: OEM decoding, alias/LFN collisions, punctuation, repair guards and unchanged sources')
