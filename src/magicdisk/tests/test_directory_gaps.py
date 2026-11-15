#!/usr/bin/env python3
"""Retain readable file entries around a directory gap without authorizing repair."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from directory_fixture import create
from duplicate_fixture import alias_name

probe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='magicdisk-directory-gaps-') as directory:
    root = Path(directory)
    for bits in [12, 16, 32]:
        path = root / 'SOURCE.IMG'
        layout = create(path, bits)
        original = path.read_bytes()
        parent = layout.cluster(layout.child())
        child = layout.cluster(layout.child(parent))
        slots = layout.slots(child)
        # Choose the second directory sector, crossing an LFN group.
        failed = slots[16] // 512
        files = [(parts, entry) for parts, entry in layout.long_names(child)]
        all_entries = {entry for _, entry in files}
        all_entries.update(entry for _, entry in layout.long_names(layout.root)
                           if not (original[entry + 11] & 16))
        for mode in ['catalog-gap', 'catalog-transient', 'catalog-cancel']:
            output = root / f'{bits}-{mode}'
            output.mkdir()
            subprocess.run([str(probe), str(path), str(output), mode, 'live', str(failed)], check=True)
            assert path.read_bytes() == original
            if mode == 'catalog-cancel':
                assert list(output.iterdir()) == []
                continue
            folder = output / 'REC00001'
            manifest = (folder / 'RECOVERY.TXT').read_text()
            exported = {int(lba) * 512 + int(offset) for lba, offset in
                        re.findall(r'LIVE_BEGIN entry_lba=(\d+) offset=(\d+)', manifest)}
            expected = {entry for entry in all_entries if entry // 512 != failed} if mode == 'catalog-gap' else all_entries
            assert exported == expected, (exported ^ expected)
            parts_written = [line.split()[1] for line in manifest.splitlines() if line.startswith('FILE_BEGIN ')]
            assert len(parts_written) == len(exported)
            for name in parts_written:
                assert (folder / name).read_bytes() == b'MagicDisk LFN preservation test.\r\n'
            if mode == 'catalog-gap':
                assert f'ISSUE code=DIRECTORY_GAP where={failed}' in manifest
                assert 'UNRESOLVED_CLUSTERS' in manifest and 'LOST_CLUSTERS' not in manifest
                assert 'Directory catalog: INCOMPLETE' in manifest and 'FINAL INCOMPLETE' in manifest
                # The first surviving alias after a missing LFN prefix must not
                # acquire a long name assembled across the unreadable sector.
                for parts, entry in files:
                    if entry // 512 != failed and any(part // 512 == failed for part in parts):
                        line = next(line for line in manifest.splitlines()
                                    if line.startswith(f'LIVE_BEGIN entry_lba={entry // 512} offset={entry % 512} '))
                        name = bytes.fromhex(line.split('path_hex=')[1]).decode()
                        assert name.rsplit('/', 1)[-1] == alias_name(original, entry), name
            else:
                assert 'DIRECTORY_GAP' not in manifest and 'FINAL COMPLETE ' in manifest
        for missing in [False, True]:
            selected = next(entry for _, entry in reversed(files)
                            if (entry // 512 == failed) == missing)
            output = root / f'{bits}-selected-{missing}'
            output.mkdir()
            subprocess.run([str(probe), str(path), str(output),
                            'catalog-missing' if missing else 'catalog-gap', 'selected',
                            str(layout.cluster(selected)), str(failed)], check=True)
            if missing:
                assert list(output.iterdir()) == []
            else:
                manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
                assert manifest.count('LIVE_BEGIN ') == 1
                assert f'entry_lba={selected // 512} offset={selected % 512}' in manifest
                assert 'FINAL INCOMPLETE' in manifest
            assert path.read_bytes() == original
        failed = layout.slots(layout.root)[0] // 512
        assert layout.child() // 512 == failed
        output = root / f'{bits}-root-gap'
        output.mkdir()
        subprocess.run([str(probe), str(path), str(output), 'catalog-gap', 'live', str(failed)], check=True)
        manifest = (output / 'REC00001' / 'RECOVERY.TXT').read_text()
        exported = {int(lba) * 512 + int(offset) for lba, offset in
                    re.findall(r'LIVE_BEGIN entry_lba=(\d+) offset=(\d+)', manifest)}
        expected = {entry for _, entry in layout.long_names(layout.root)
                    if entry // 512 != failed and not (original[entry + 11] & 16)}
        assert expected and exported == expected
        assert 'UNRESOLVED_CLUSTERS' in manifest and 'LOST_CLUSTERS' not in manifest
        assert 'FINAL INCOMPLETE' in manifest
        assert path.read_bytes() == original
        print(f'PASS: FAT{bits}: root/child directory gaps, surviving entries/payloads, incomplete catalog, retries and cancellation')
