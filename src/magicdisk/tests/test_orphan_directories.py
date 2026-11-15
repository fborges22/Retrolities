#!/usr/bin/env python3
"""Discover an unreferenced allocated tree with no surviving parent alias."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from directory_fixture import create, PARENT, Layout


def fixture(path, bits):
    layout = create(path, bits)
    parts, entry = next((p, e) for p, e in layout.long_names(layout.root)
                        if layout.decoded_name(p) == PARENT)
    outer = layout.cluster(entry)
    inner = layout.cluster(layout.child(outer))
    expected = {e: layout.decoded_name(p) for p, e in layout.long_names(inner)}
    data = bytearray(layout.data)
    for offset in parts + [entry]:
        data[offset:offset + 32] = b'\xe5' + bytes(31)
    path.write_bytes(data)
    return bytes(data), expected, outer, inner


def verify(manifest, read_file, expected, lba=0):
    assert 'FINAL COMPLETE_UNVERIFIED' in manifest, manifest
    assert manifest.count('ORPHAN_DIRECTORY ') == 2
    assert manifest.count('DIRECTORY_BEGIN ') == 2
    found = {}
    for block in manifest.split('DELETED_BEGIN ')[1:]:
        match = re.match(r'entry_lba=(\d+) offset=(\d+)', block)
        offset = (int(match[1]) - lba) * 512 + int(match[2])
        names = re.findall(r'^FILE_BEGIN (\S+)', block, re.M)
        assert b''.join(read_file(name) for name in names) == b'MagicDisk LFN preservation test.\r\n'
        decoded = re.findall(r'NAME_CANDIDATE .* name_hex=(\w+)', block)
        assert len(decoded) == 1
        found[offset] = bytes.fromhex(decoded[0]).decode('utf-8')
        assert 'parent_hex=' + 'orphan-directory@'.encode().hex().upper() in block
    assert found == expected


if __name__ == '__main__':
    exe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='magicdisk-orphans-') as directory:
        root = Path(directory)
        for bits in [12, 16, 32]:
            path = root / f'{bits}.img'
            original, expected, outer, inner = fixture(path, bits)
            output = root / str(bits)
            output.mkdir()
            result = subprocess.run([exe, str(path), '--recover-directories', str(output)],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 1, result.stdout + result.stderr
            recovered = output / 'REC00001'
            verify((recovered / 'RECOVERY.TXT').read_text(),
                   lambda name: (recovered / name).read_bytes(), expected)
            assert path.read_bytes() == original
            result = subprocess.run([exe, str(path), '--recover-directories', str(output),
                                     '--apply', '--journal', str(root / 'NO.MDJ')],
                                    capture_output=True, timeout=60)
            assert result.returncode == 2 and not (root / 'NO.MDJ').exists()
            layout = Layout(original, bits)
            if len(sys.argv) > 2:
                first = layout.data_sector + (outer - 2) * layout.spc
                for mode in ['orphan-cancel', 'orphan-read', 'orphan-transient']:
                    target = root / f'{bits}-{mode}'
                    target.mkdir()
                    result = subprocess.run([str(Path(sys.argv[2]).resolve()), str(path),
                                             str(target), mode, 'orphan', str(first)],
                                            capture_output=True, text=True, timeout=60)
                    assert result.returncode == 0, result.stdout + result.stderr
                    manifest = (target / 'REC00001' / 'RECOVERY.TXT').read_text()
                    if mode == 'orphan-transient':
                        verify(manifest, lambda name: (target / 'REC00001' / name).read_bytes(), expected)
                    else:
                        assert 'FINAL INCOMPLETE' in manifest
                        if mode == 'orphan-read':
                            assert 'ORPHAN_SEARCH_GAP ' in manifest
                    assert path.read_bytes() == original
            invalid = bytearray(original)
            for cluster in [outer, inner]:
                first = (layout.data_sector + (cluster - 2) * layout.spc) * 512
                invalid[first] = ord('X')
            path.write_bytes(invalid)
            result = subprocess.run([exe, str(path), '--recover-directories', str(output)],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr
            manifest = (output / 'REC00002' / 'RECOVERY.TXT').read_text()
            assert 'ORPHAN_DIRECTORY ' not in manifest and 'files=0' in manifest
            assert path.read_bytes() == invalid
            create(path, bits)
            live = path.read_bytes()
            result = subprocess.run([exe, str(path), '--recover-directories', str(output)],
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr
            manifest = (output / 'REC00003' / 'RECOVERY.TXT').read_text()
            assert 'ORPHAN_DIRECTORY ' not in manifest and 'files=0' in manifest
            assert path.read_bytes() == live
            print(f'PASS FAT{bits}: orphan discovery, LFN contents, live exclusion, exact source')
