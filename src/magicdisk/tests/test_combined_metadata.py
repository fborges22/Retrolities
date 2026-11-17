#!/usr/bin/env python3
"""Repair independent damage in stages without auxiliary-metadata deadlocks."""
from pathlib import Path
import subprocess
import sys
import tempfile
from directory_fixture import create
from chain_fixture import chain_fixture, set_fat
from directory_chain_fixture import directory_chain_fixture
from lost_fixture import lost_fixture

exe = Path(sys.argv[1]).resolve()


def run(path, *args, code=0):
    result = subprocess.run([str(exe), str(path), *args], capture_output=True, text=True)
    assert result.returncode == code, result.stdout + result.stderr
    return result.stdout


with tempfile.TemporaryDirectory(prefix='magicdisk-combined-') as directory:
    root = Path(directory)
    path = root / 'fat32.img'
    layout = create(path, 32)
    clean = path.read_bytes()
    for mode in ['fsinfo-first', 'boot-first', 'files', 'directories', 'lost', 'mirrors', 'tree']:
        if mode == 'tree':
            files_broken, files_fixed, _ = chain_fixture(clean, 32)
            broken = directory_chain_fixture(files_broken, 32)[0]
            expected_data = directory_chain_fixture(files_fixed, 32)[1]
            operations = [('--repair-tree',)]
        elif mode == 'files':
            broken, expected_data, _ = chain_fixture(clean, 32)
            operations = [('--repair-files',)]
        elif mode == 'directories':
            broken, expected_data, _, _ = directory_chain_fixture(clean, 32)
            operations = [('--repair-directories',)]
        elif mode == 'lost':
            broken, payloads = lost_fixture(clean, 32)
            expected_data = broken[:]
            for cluster in payloads:
                set_fat(expected_data, 32, cluster, 0)
            operations = [('--reclaim-lost',)]
        else:
            broken, expected_data = bytearray(clean), clean
            operations = []
            if mode == 'mirrors':
                start = (layout.reserved + layout.fatsz) * 512
                broken[start:start + layout.fatsz * 512] = b'\x5a' * (layout.fatsz * 512)
                operations = [('--repair-fat', '1')]
        for sector in [1, 6, 7]:
            broken[sector * 512:(sector + 1) * 512] = bytes(512)
        path.write_bytes(broken)
        if mode == 'lost':
            output = root / 'recovery'
            output.mkdir()
            assert '13/13 clusters' in run(path, '--recover-lost', str(output))
            assert path.read_bytes() == broken
        if mode == 'fsinfo-first':
            operations += [('--repair-fsinfo',), ('--repair-boot-backup',)]
        else:
            operations += [('--repair-boot-backup',), ('--repair-fsinfo',)]
        history = []
        for index, operation in enumerate(operations):
            before = path.read_bytes()
            run(path, *operation)
            assert path.read_bytes() == before
            journal = root / f'{mode}-{index}.mdj'
            run(path, *operation, '--apply', '--journal', str(journal))
            if journal.exists():
                history.append((journal, before))
            else:
                assert path.read_bytes() == before
        repaired = path.read_bytes()
        assert repaired[layout.reserved * 512:] == expected_data[layout.reserved * 512:]
        assert repaired[:512] == clean[:512] and repaired[6 * 512:7 * 512] == clean[:512]
        assert '0 issues' in run(path)
        check = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout + check.stderr
        for journal, before in reversed(history):
            run(path, '--restore-journal', str(journal), '--apply')
            assert path.read_bytes() == before
        assert path.read_bytes() == broken

print('PASS: simultaneous FAT32 backup/FSInfo and file/directory/orphan/mirror damage, staged repairs and exact reverse undo')
