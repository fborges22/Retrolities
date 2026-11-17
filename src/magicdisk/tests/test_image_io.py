#!/usr/bin/env python3
"""Sparse host-image boundary checks using the production image backend."""
from pathlib import Path
import subprocess
import sys
import tempfile


def pattern(sector, changed=False):
    return bytes(((sector >> ((index % 4) * 8)) ^ (index * 29 + 17) ^ (255 if changed else 0)) & 255
                 for index in range(512))


with tempfile.TemporaryDirectory(prefix='magicdisk-image-io-host-') as directory:
    path = Path(directory) / 'IMAGE.IMG'
    for length in [4096, 0x7ffffe00, 0x80000400, 0xfffffe00, 0xffffffff * 512]:
        sectors = length // 512
        positions = [0, sectors - 1] + ([0x3fffff, 0x400000, 0x400001] if sectors > 0x400001 else [])
        with path.open('wb') as file:
            file.truncate(length)
            for sector in positions:
                file.seek(sector * 512)
                file.write(pattern(sector))
        subprocess.run([sys.argv[1], str(path), str(sectors)], check=True)
        assert path.stat().st_size == length
        with path.open('rb') as file:
            for sector in positions:
                file.seek(sector * 512)
                assert file.read(512) == pattern(sector, True)
                for neighbor in [sector - 1, sector + 1]:
                    if 0 <= neighbor < sectors and neighbor not in positions:
                        file.seek(neighbor * 512)
                        assert file.read(512) == bytes(512)
        print(f'PASS: image size {length}: exact reads/writes, preserved neighbors and no extension')
