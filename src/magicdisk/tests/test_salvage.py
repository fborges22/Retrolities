#!/usr/bin/env python3
"""Validate salvage's readable case, ownership guards and version-16 journal fields."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from directory_fixture import command, UNICODE_FILE
from selected_export_fixture import create_selected

exe = Path(sys.argv[1]).resolve()

def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout

with tempfile.TemporaryDirectory(prefix='magicdisk-salvage-native-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'VOLUME.IMG'
        original, layout, chain, payload, _ = create_selected(path, bits)
        directory_entry = next(offset for offset in layout.entries() if original[offset:offset + 11] == b'DIRECTRY   ')
        report = run(path, '--salvage-cluster', str(layout.cluster(directory_entry)), code=2)
        assert 'regular file data only' in report
        run(path, '--salvage-cluster', '1000', code=2)
        assert path.read_bytes() == original
        for cluster in [chain[0], chain[1], chain[-1]]:
            journal = root / f'S{bits}-{cluster}.MDJ'
            preview = run(path, '--salvage-cluster', str(cluster))
            assert '0 unreadable sectors become ZERO (0 bytes)' in preview
            assert path.read_bytes() == original
            run(path, '--salvage-cluster', str(cluster), '--apply', '--journal', str(journal))
            changed = path.read_bytes()
            assert command('mtype', '-i', str(path), '::/' + UNICODE_FILE).stdout == payload
            raw = journal.read_bytes()
            assert struct.unpack_from('<I', raw, 8)[0] == 16 and raw[40:56] == bytes(16)
            for field, value in [(40, 1), (55, 128), (56, 1), (36, 15), (8, 15)]:
                forged = bytearray(raw)
                forged[field] = value
                struct.pack_into('<I', forged, 60, zlib.crc32(forged[:60] + forged[64:1088]))
                altered = root / 'FORGED.MDJ'
                altered.write_bytes(forged)
                run(path, '--restore-journal', str(altered), '--apply', code=2)
                assert path.read_bytes() == changed
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == original
        print(f'PASS: FAT{bits}: salvage heads/middle/tails, payload/LFN preservation, directory/free refusal, forged masks and exact undo')
